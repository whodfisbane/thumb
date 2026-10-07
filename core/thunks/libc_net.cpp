// Sockets: real networking for translated code (streaming, LAN games).
// Android itself enforces the app's INTERNET permission, so THUMB's
// "Block internet" option (which removes it) still keeps apps offline.
//
// sockaddr_* and pollfd have the same layout on 32- and 64-bit. What differs:
//   - struct timeval (SO_RCVTIMEO/SO_SNDTIMEO, select): 8 bytes on the guest
//   - struct msghdr / iovec / cmsghdr (sendmsg/recvmsg): 4-byte pointers and lengths
//   - struct addrinfo / servent: pointers, built in guest memory
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "thunks/libc_internal.h"
#include "thunks/thunks.h"

namespace h32 {
namespace {

uint32_t arg(GuestThread& t, size_t i) { return t.arg_word(i); }

void ret_errno(GuestThread& t, long r) {
    set_ret32(t, uint32_t(r));
    sync_guest_errno(t);
}

// ---- socket options: timeval is 8 bytes on the guest ----

bool is_timeval_opt(int level, int name) { return level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO); }

// int setsockopt(int fd, int level, int name, const void* value, socklen_t len)
void t_setsockopt(GuestThread& t) {
    int fd = int32_t(arg(t, 0)), level = int32_t(arg(t, 1)), name = int32_t(arg(t, 2));
    gaddr value = arg(t, 3);
    socklen_t len = arg(t, 4);
    errno = 0;
    if (is_timeval_opt(level, name) && value && len >= 8) {
        timeval tv{mem().read<int32_t>(value), mem().read<int32_t>(value + 4)};
        return ret_errno(t, ::setsockopt(fd, level, name, &tv, sizeof tv));
    }
    ret_errno(t, ::setsockopt(fd, level, name, mem().ptr<void>(value), len));
}

// int getsockopt(int fd, int level, int name, void* value, socklen_t* len)
void t_getsockopt(GuestThread& t) {
    int fd = int32_t(arg(t, 0)), level = int32_t(arg(t, 1)), name = int32_t(arg(t, 2));
    gaddr value = arg(t, 3), lenp = arg(t, 4);
    errno = 0;
    if (is_timeval_opt(level, name) && value && lenp) {
        timeval tv{};
        socklen_t n = sizeof tv;
        int r = ::getsockopt(fd, level, name, &tv, &n);
        if (r == 0) {
            mem().write<int32_t>(value, int32_t(tv.tv_sec));
            mem().write<int32_t>(value + 4, int32_t(tv.tv_usec));
            mem().write<uint32_t>(lenp, 8);
        }
        return ret_errno(t, r);
    }
    ret_errno(t, ::getsockopt(fd, level, name, mem().ptr<void>(value), mem().ptr<socklen_t>(lenp)));
}

// ---- select: fd_set has the same bytes on both sides; timeval differs ----

// int select(int n, fd_set* r, fd_set* w, fd_set* e, struct timeval* timeout)
void t_select(GuestThread& t) {
    gaddr tvp = arg(t, 4);
    timeval tv{};
    if (tvp) tv = {mem().read<int32_t>(tvp), mem().read<int32_t>(tvp + 4)};
    errno = 0;
    int r = ::select(int32_t(arg(t, 0)), mem().ptr<fd_set>(arg(t, 1)), mem().ptr<fd_set>(arg(t, 2)),
                     mem().ptr<fd_set>(arg(t, 3)), tvp ? &tv : nullptr);
    if (tvp) {  // Linux reports the time left
        mem().write<int32_t>(tvp, int32_t(tv.tv_sec));
        mem().write<int32_t>(tvp + 4, int32_t(tv.tv_usec));
    }
    ret_errno(t, r);
}

// ---- sendmsg / recvmsg ----
// Guest msghdr: name(4) namelen(4) iov(4) iovlen(4) control(4) controllen(4) flags(4).
// Guest cmsghdr: len(4) level(4) type(4) data, 4-byte aligned.

constexpr size_t kGuestCmsgHdr = 12;
size_t guest_cmsg_align(size_t n) { return (n + 3) & ~size_t(3); }

struct HostMsg {
    msghdr msg{};
    std::vector<iovec> iov;
    std::vector<uint8_t> control;
};

HostMsg host_msghdr(gaddr g, bool sending) {
    HostMsg h;
    h.msg.msg_name = mem().ptr<void>(mem().read<uint32_t>(g));
    h.msg.msg_namelen = mem().read<uint32_t>(g + 4);
    gaddr iov = mem().read<uint32_t>(g + 8);
    uint32_t iovlen = mem().read<uint32_t>(g + 12);
    h.iov.resize(iovlen);
    for (uint32_t i = 0; i < iovlen; i++) {
        h.iov[i].iov_base = mem().ptr<void>(mem().read<uint32_t>(iov + i * 8));
        h.iov[i].iov_len = mem().read<uint32_t>(iov + i * 8 + 4);
    }
    h.msg.msg_iov = h.iov.data();
    h.msg.msg_iovlen = iovlen;
    gaddr control = mem().read<uint32_t>(g + 16);
    uint32_t controllen = mem().read<uint32_t>(g + 20);
    if (control && controllen) {
        // Host headers are bigger: give room for twice as many bytes.
        h.control.assign(controllen * 2 + 64, 0);
        if (sending) {
            size_t out = 0, in = 0;
            while (in + kGuestCmsgHdr <= controllen) {
                uint32_t len = mem().read<uint32_t>(control + in);
                if (len < kGuestCmsgHdr || in + len > controllen) break;
                size_t data = len - kGuestCmsgHdr;
                auto* c = reinterpret_cast<cmsghdr*>(h.control.data() + out);
                c->cmsg_len = CMSG_LEN(data);
                c->cmsg_level = mem().read<int32_t>(control + in + 4);
                c->cmsg_type = mem().read<int32_t>(control + in + 8);
                std::memcpy(CMSG_DATA(c), mem().ptr<void>(control + in + kGuestCmsgHdr), data);
                out += CMSG_SPACE(data);
                in += guest_cmsg_align(len);
            }
            h.control.resize(out);
        }
        h.msg.msg_control = h.control.empty() ? nullptr : h.control.data();
        h.msg.msg_controllen = h.control.size();
    }
    h.msg.msg_flags = mem().read<int32_t>(g + 24);
    return h;
}

// ssize_t sendmsg(int fd, const struct msghdr*, int flags)
void t_sendmsg(GuestThread& t) {
    HostMsg h = host_msghdr(arg(t, 1), true);
    errno = 0;
    ret_errno(t, ::sendmsg(int32_t(arg(t, 0)), &h.msg, int32_t(arg(t, 2))));
}

// ssize_t recvmsg(int fd, struct msghdr*, int flags)
void t_recvmsg(GuestThread& t) {
    gaddr g = arg(t, 1);
    HostMsg h = host_msghdr(g, false);
    errno = 0;
    ssize_t r = ::recvmsg(int32_t(arg(t, 0)), &h.msg, int32_t(arg(t, 2)));
    if (r >= 0) {
        mem().write<uint32_t>(g + 4, h.msg.msg_namelen);
        mem().write<int32_t>(g + 24, h.msg.msg_flags);
        // Control messages back into guest layout.
        gaddr control = mem().read<uint32_t>(g + 16);
        uint32_t room = mem().read<uint32_t>(g + 20), out = 0;
        if (control && h.msg.msg_control) {
            for (cmsghdr* c = CMSG_FIRSTHDR(&h.msg); c; c = CMSG_NXTHDR(&h.msg, c)) {
                size_t data = c->cmsg_len - CMSG_LEN(0);
                if (out + kGuestCmsgHdr + data > room) {
                    mem().write<int32_t>(g + 24, h.msg.msg_flags | MSG_CTRUNC);
                    break;
                }
                mem().write<uint32_t>(control + out, uint32_t(kGuestCmsgHdr + data));
                mem().write<int32_t>(control + out + 4, c->cmsg_level);
                mem().write<int32_t>(control + out + 8, c->cmsg_type);
                std::memcpy(mem().ptr<void>(control + out + kGuestCmsgHdr), CMSG_DATA(c), data);
                out += uint32_t(guest_cmsg_align(kGuestCmsgHdr + data));
            }
        }
        mem().write<uint32_t>(g + 20, out);
    }
    ret_errno(t, r);
}

// ---- name resolution ----
// Guest addrinfo: flags family socktype protocol addrlen(4) canonname(4) addr(4) next(4) = 32 bytes.

std::mutex g_ai_lock;
std::unordered_map<gaddr, std::vector<gaddr>> g_ai_blocks;  // list head -> guest allocations

// Bionic and the host agree on Android; the Linux harness (glibc) uses negative EAI codes.
int guest_eai(int e) {
#ifdef __ANDROID__
    return e;
#else
    switch (e) {
    case 0: return 0;
    case EAI_AGAIN: return 2;
    case EAI_BADFLAGS: return 3;
    case EAI_FAMILY: return 5;
    case EAI_MEMORY: return 6;
    case EAI_NONAME: return 8;
    case EAI_SERVICE: return 9;
    case EAI_SOCKTYPE: return 10;
    case EAI_SYSTEM: return 11;
    default: return 4;  // EAI_FAIL
    }
#endif
}

// int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res)
void t_getaddrinfo(GuestThread& t) {
    addrinfo hints{}, *hp = nullptr;
    if (gaddr g = arg(t, 2)) {
        hints.ai_flags = mem().read<int32_t>(g);
        hints.ai_family = mem().read<int32_t>(g + 4);
        hints.ai_socktype = mem().read<int32_t>(g + 8);
        hints.ai_protocol = mem().read<int32_t>(g + 12);
        hp = &hints;
    }
    addrinfo* res = nullptr;
    int r = ::getaddrinfo(mem().str(arg(t, 0)), mem().str(arg(t, 1)), hp, &res);
    gaddr head = 0, prev = 0;
    std::vector<gaddr> blocks;
    for (addrinfo* a = res; r == 0 && a; a = a->ai_next) {
        gaddr g = mem().calloc(1, 32);
        blocks.push_back(g);
        mem().write<int32_t>(g, a->ai_flags);
        mem().write<int32_t>(g + 4, a->ai_family);
        mem().write<int32_t>(g + 8, a->ai_socktype);
        mem().write<int32_t>(g + 12, a->ai_protocol);
        mem().write<uint32_t>(g + 16, a->ai_addrlen);
        if (a->ai_addr && a->ai_addrlen) {
            gaddr sa = mem().malloc(a->ai_addrlen);
            blocks.push_back(sa);
            std::memcpy(mem().ptr<void>(sa), a->ai_addr, a->ai_addrlen);
            mem().write<uint32_t>(g + 24, sa);
        }
        if (a->ai_canonname) {
            gaddr s = mem().strdup(a->ai_canonname);
            blocks.push_back(s);
            mem().write<uint32_t>(g + 20, s);
        }
        if (prev) mem().write<uint32_t>(prev + 28, g);
        else head = g;
        prev = g;
    }
    if (res) ::freeaddrinfo(res);
    if (head) {
        std::lock_guard lk(g_ai_lock);
        g_ai_blocks[head] = std::move(blocks);
    }
    if (gaddr out = arg(t, 3)) mem().write<uint32_t>(out, head);
    set_ret32(t, uint32_t(guest_eai(r)));
}

void t_freeaddrinfo(GuestThread& t) {
    std::vector<gaddr> blocks;
    {
        std::lock_guard lk(g_ai_lock);
        auto it = g_ai_blocks.find(arg(t, 0));
        if (it == g_ai_blocks.end()) return;
        blocks = std::move(it->second);
        g_ai_blocks.erase(it);
    }
    for (gaddr b : blocks) mem().free(b);
}

void t_gai_strerror(GuestThread& t) {
    static std::mutex m;
    static std::unordered_map<int, gaddr> cache;
    int e = int32_t(arg(t, 0));
    std::lock_guard lk(m);
    auto& slot = cache[e];
    if (!slot) slot = mem().static_string(::gai_strerror(e));  // same codes on Android
    set_ret32(t, slot);
}

// Lengths are socklen_t in glibc, size_t in bionic: both 4 bytes on the guest.
int h_getnameinfo(const sockaddr* sa, uint32_t salen, char* host, uint32_t hostlen, char* serv, uint32_t servlen, int flags) {
    return guest_eai(::getnameinfo(sa, salen, host, hostlen, serv, servlen, flags));
}

// struct servent* getservbyport(int port, const char* proto) / getservbyname(name, proto)
// Guest servent: name(4) aliases(4) port(4) proto(4), kept per thread.
void put_servent(GuestThread& t, servent* s) {
    if (!s) return set_ret32(t, 0);
    thread_local gaddr g = 0, name = 0, proto = 0, aliases = 0;
    if (!g) {
        g = mem().alloc_static(16);
        aliases = mem().alloc_static(4);  // empty list
    }
    if (name) mem().free(name);
    if (proto) mem().free(proto);
    name = mem().strdup(s->s_name ? s->s_name : "");
    proto = mem().strdup(s->s_proto ? s->s_proto : "");
    mem().write<uint32_t>(g, name);
    mem().write<uint32_t>(g + 4, aliases);
    mem().write<int32_t>(g + 8, s->s_port);
    mem().write<uint32_t>(g + 12, proto);
    set_ret32(t, g);
}
void t_getservbyport(GuestThread& t) { put_servent(t, ::getservbyport(int32_t(arg(t, 0)), mem().str(arg(t, 1)))); }
void t_getservbyname(GuestThread& t) { put_servent(t, ::getservbyname(mem().str(arg(t, 0)), mem().str(arg(t, 1)))); }

// char* inet_ntoa(struct in_addr) — the address arrives by value in r0.
void t_inet_ntoa(GuestThread& t) {
    uint32_t a = arg(t, 0);
    thread_local gaddr buf = 0;
    if (!buf) buf = mem().alloc_static(16);
    snprintf(mem().ptr<char>(buf), 16, "%u.%u.%u.%u", a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, a >> 24);
    set_ret32(t, buf);
}

}  // namespace

namespace thunks {

void register_libc_net() {
    H32_ADD(socket, int(int, int, int));
    H32_ADD(socketpair, int(int, int, int, int*));
    H32_ADD(connect, int(int, const sockaddr*, socklen_t));
    H32_ADD(bind, int(int, const sockaddr*, socklen_t));
    H32_ADD(listen, int(int, int));
    H32_ADD(accept, int(int, sockaddr*, socklen_t*));
    H32_ADD(accept4, int(int, sockaddr*, socklen_t*, int));
    H32_ADD(shutdown, int(int, int));
    H32_ADD(send, ssize_t(int, const void*, size_t, int));
    H32_ADD(recv, ssize_t(int, void*, size_t, int));
    H32_ADD(sendto, ssize_t(int, const void*, size_t, int, const sockaddr*, socklen_t));
    H32_ADD(recvfrom, ssize_t(int, void*, size_t, int, sockaddr*, socklen_t*));
    H32_ADD(getsockname, int(int, sockaddr*, socklen_t*));
    H32_ADD(getpeername, int(int, sockaddr*, socklen_t*));
    add("setsockopt", t_setsockopt);
    add("getsockopt", t_getsockopt);
    add("sendmsg", t_sendmsg);
    add("recvmsg", t_recvmsg);
    add("select", t_select);
    add("getaddrinfo", t_getaddrinfo);
    add("freeaddrinfo", t_freeaddrinfo);
    add("gai_strerror", t_gai_strerror);
    add("getnameinfo", H32_WRAP(h_getnameinfo, int(const sockaddr*, uint32_t, char*, uint32_t, char*, uint32_t, int)));
    add("getservbyport", t_getservbyport);
    add("getservbyname", t_getservbyname);
    H32_ADD(inet_addr, in_addr_t(const char*));
    H32_ADD(inet_aton, int(const char*, in_addr*));
    H32_ADD(inet_pton, int(int, const char*, void*));
    H32_ADD(inet_ntop, const char*(int, const void*, char*, socklen_t));
    add("inet_ntoa", t_inet_ntoa);
    H32_ADD(if_nametoindex, unsigned(const char*));
    H32_ADD(gethostname, int(char*, size_t));
}

}  // namespace thunks
}  // namespace h32
