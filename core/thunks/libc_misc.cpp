// Networking and other system calls. Online services for old games are
// usually dead, so networking reports "unreachable" instead of being bridged.
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <poll.h>

#include <vector>

#include "thunks/libc_internal.h"
#include "thunks/thunks.h"

namespace h32 {
namespace {

void fail_errno(GuestThread& t, int e) {
    mem().write<int32_t>(t.errno_addr(), e);
    set_ret32(t, uint32_t(-1));
}

void t_socket(GuestThread& t) {
    H32_DEBUG("socket(%d, %d, %d) -> refused (networking disabled)", int32_t(t.regs()[0]), int32_t(t.regs()[1]), int32_t(t.regs()[2]));
    fail_errno(t, EACCES);
}
void t_netfail(GuestThread& t) { fail_errno(t, ENETUNREACH); }

void t_getaddrinfo(GuestThread& t) {
    H32_DEBUG("getaddrinfo(\"%s\") -> EAI_FAIL (networking disabled)", mem().str(t.regs()[0]) ?: "");
    if (gaddr res = t.regs()[3]) mem().write<uint32_t>(res, 0);
    set_ret32(t, uint32_t(bionic::kEaiFail));
}
void t_freeaddrinfo(GuestThread&) {}
void t_gai_strerror(GuestThread& t) {
    gaddr buf = thread_scratch(kScratchGai, 32);
    std::strcpy(mem().ptr<char>(buf), "network disabled");
    set_ret32(t, buf);
}

// char* inet_ntoa(struct in_addr) — the address arrives by value in r0.
void t_inet_ntoa(GuestThread& t) {
    uint32_t a = t.regs()[0];
    gaddr buf = thread_scratch(kScratchInetNtoa, 16);
    snprintf(mem().ptr<char>(buf), 16, "%u.%u.%u.%u", a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, a >> 24);
    set_ret32(t, buf);
}

// int poll(struct pollfd*, nfds_t, int) — pollfd is 8 bytes on both sides.
int h_poll(pollfd* fds, unsigned n, int timeout) { return ::poll(fds, n, timeout); }

// ssize_t writev(int fd, const struct iovec* iov, int cnt) — iovec is 8 bytes on the guest.
void t_writev(GuestThread& t) {
    int fd = int32_t(t.regs()[0]);
    gaddr iov = t.regs()[1];
    int cnt = int32_t(t.regs()[2]);
    std::vector<iovec> v(size_t(cnt > 0 ? cnt : 0));
    for (int i = 0; i < cnt; i++) {
        v[i].iov_base = mem().ptr<void>(mem().read<uint32_t>(iov + i * 8));
        v[i].iov_len = mem().read<uint32_t>(iov + i * 8 + 4);
    }
    errno = 0;
    set_ret32(t, uint32_t(::writev(fd, v.data(), cnt)));
    sync_guest_errno(t);
}

// int ioctl(int fd, int request, ...) — only FIONREAD/FIONBIO (int*) are bridged.
void t_ioctl(GuestThread& t) {
    unsigned long req = t.regs()[1];
    if (req != FIONREAD && req != FIONBIO) {
        H32_WARN("ioctl request 0x%lx not supported", req);
        return fail_errno(t, ENOTTY);
    }
    errno = 0;
    set_ret32(t, uint32_t(::ioctl(int32_t(t.regs()[0]), req, mem().ptr<int>(t.regs()[2]))));
    sync_guest_errno(t);
}

// select() is only used with sockets here, which never exist.
void t_select(GuestThread& t) { set_ret32(t, 0); }

}  // namespace

namespace thunks {

void register_libc_misc() {
    add("socket", t_socket);
    add("connect", t_netfail);
    add("send", t_netfail);
    add("sendto", t_netfail);
    add("recv", t_netfail);
    add("recvfrom", t_netfail);
    add("getaddrinfo", t_getaddrinfo);
    add("freeaddrinfo", t_freeaddrinfo);
    add("gai_strerror", t_gai_strerror);
    add("inet_ntoa", t_inet_ntoa);
    add("select", t_select);
    add("poll", H32_WRAP(h_poll, int(pollfd*, unsigned, int)));
    add("writev", t_writev);
    add("ioctl", t_ioctl);
}

}  // namespace thunks
}  // namespace h32
