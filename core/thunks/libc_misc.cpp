// Networking and other system calls. Online services for old games are
// usually dead, so networking reports "unreachable" instead of being bridged.
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <time.h>
#include <sys/uio.h>
#include <poll.h>
#include <unistd.h>

#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "loader/elf_loader.h"
#include "thunks/timescale.h"
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

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

// long sysconf(int name) — bionic numbering.
void t_sysconf(GuestThread& t) {
    int32_t name = int32_t(t.regs()[0]);
    int32_t v;
    switch (name) {
    case 0x0006: v = 100; break;                                                   // _SC_CLK_TCK
    case 0x0027:                                                                   // _SC_PAGESIZE
    case 0x0028: v = 4096; break;                                                  // _SC_PAGE_SIZE (the guest's view)
    case 0x0060:                                                                   // _SC_NPROCESSORS_CONF
    case 0x0061: v = int32_t(std::thread::hardware_concurrency()); break;          // _SC_NPROCESSORS_ONLN
    default:
        H32_WARN("sysconf(0x%x) not supported", name);
        mem().write<int32_t>(t.errno_addr(), EINVAL);
        v = -1;
    }
    set_ret32(t, uint32_t(v));
}

// Guest memory is always read/write and code is translated, so protection
// changes are no-ops. But a guest that changes protections is often about to
// patch or decrypt its code, so drop any translations of that range.
void t_mprotect(GuestThread& t) {
    gaddr start = t.regs()[0];
    uint32_t len = t.regs()[1];
    H32_DEBUG("mprotect(0x%08x, 0x%x, %u)", start, len, t.regs()[2]);
    GuestThread::invalidate_code(start, len);
    set_ret32(t, 0);
}

// int dladdr(const void* addr, Dl_info* info) — Dl_info is 4 pointers on the guest.
void t_dladdr(GuestThread& t) {
    gaddr a = t.regs()[0];
    gaddr info = t.regs()[1];
    Module* m = module_containing(a);
    if (!m || !info) return set_ret32(t, 0);
    static std::mutex lock;
    static std::unordered_map<Module*, gaddr> names;
    gaddr name;
    {
        std::lock_guard lk(lock);
        auto& n = names[m];
        if (!n) n = mem().static_string(m->name);
        name = n;
    }
    mem().write<uint32_t>(info + 0, name);     // dli_fname
    mem().write<uint32_t>(info + 4, m->base);  // dli_fbase
    mem().write<uint32_t>(info + 8, 0);        // dli_sname
    mem().write<uint32_t>(info + 12, 0);       // dli_saddr
    set_ret32(t, 1);
}

void t_sleep(GuestThread& t) {
    ::usleep(useconds_t(timescale::real_sleep_ns(int64_t(t.regs()[0]) * 1000000000) / 1000));
    set_ret32(t, 0);
}
void t_usleep(GuestThread& t) { set_ret32(t, uint32_t(::usleep(useconds_t(timescale::real_sleep_ns(int64_t(t.regs()[0]) * 1000) / 1000)))); }

// int vasprintf(char** out, const char* fmt, va_list ap)
void t_vasprintf(GuestThread& t) {
    VarArgs va(t, t.regs()[2]);
    std::string s = guest_format(mem().str(t.regs()[1]), va);
    gaddr p = mem().strdup(s);
    mem().write<uint32_t>(t.regs()[0], p);
    set_ret32(t, p ? uint32_t(s.size()) : uint32_t(-1));
}
void t_asprintf(GuestThread& t) {
    ArgCursor c{t, 2};
    VarArgs va(c);
    std::string s = guest_format(mem().str(t.regs()[1]), va);
    gaddr p = mem().strdup(s);
    mem().write<uint32_t>(t.regs()[0], p);
    set_ret32(t, p ? uint32_t(s.size()) : uint32_t(-1));
}

// void __assert2(const char* file, int line, const char* func, const char* msg)
void t_assert2(GuestThread& t) {
    fatal("guest assertion failed: %s:%d %s: %s", mem().str(t.regs()[0]), int32_t(t.regs()[1]), mem().str(t.regs()[2]),
          mem().str(t.regs()[3]));
}

// libc-style result: -errno -> errno + -1
void ret_or_errno(GuestThread& t, int32_t r) {
    if (r < 0 && r > -4096) {
        mem().write<int32_t>(t.errno_addr(), -r);
        return set_ret32(t, uint32_t(-1));
    }
    set_ret32(t, uint32_t(r));
}

// void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset) — 32-bit off_t
void t_mmap(GuestThread& t) {
    ArgCursor c{t};
    gaddr addr = c.word();
    uint32_t len = c.word(), prot = c.word();
    int32_t flags = int32_t(c.word()), fd = int32_t(c.word());
    uint32_t off = c.word();
    ret_or_errno(t, guest_mmap(addr, len, prot, flags, fd, off));
}
void t_munmap(GuestThread& t) { ret_or_errno(t, guest_munmap(t.regs()[0])); }

// int open(const char* path, int flags, ...) / openat(int dirfd, ...)
void t_open(GuestThread& t) { ret_or_errno(t, guest_open(AT_FDCWD, mem().str(t.regs()[0]), int32_t(t.regs()[1]), int32_t(t.regs()[2]))); }
void t_openat(GuestThread& t) {
    int dirfd = int32_t(t.regs()[0]) == -100 ? AT_FDCWD : int32_t(t.regs()[0]);
    ret_or_errno(t, guest_open(dirfd, mem().str(t.regs()[1]), int32_t(t.regs()[2]), int32_t(t.regs()[3])));
}

void t_access(GuestThread& t) {
    std::string p = map_path(mem().str(t.regs()[0]));
    errno = 0;
    set_ret32(t, uint32_t(::access(p.c_str(), int32_t(t.regs()[1]))));
    sync_guest_errno(t);
}
void t_chdir(GuestThread& t) {
    std::string p = map_path(mem().str(t.regs()[0]));
    errno = 0;
    set_ret32(t, uint32_t(::chdir(p.c_str())));
    sync_guest_errno(t);
}
// char* getcwd(char* buf, size_t size); buf == NULL allocates
void t_getcwd(GuestThread& t) {
    char host[4096];
    if (!::getcwd(host, sizeof host)) {
        sync_guest_errno(t);
        return set_ret32(t, 0);
    }
    size_t n = std::strlen(host) + 1;
    gaddr buf = t.regs()[0];
    size_t size = t.regs()[1];
    if (!buf) {
        buf = mem().malloc(size > n ? size : n);
    } else if (size < n) {
        mem().write<int32_t>(t.errno_addr(), ERANGE);
        return set_ret32(t, 0);
    }
    std::memcpy(mem().ptr<char>(buf), host, n);
    set_ret32(t, buf);
}

// Processes: an app can't fork a translated copy of itself.
void t_nosys(GuestThread& t) {
    mem().write<int32_t>(t.errno_addr(), ENOSYS);
    set_ret32(t, uint32_t(-1));
}

// Signals belong to the host runtime (ART, dynarmic); guest handlers are not installed.
void t_signal(GuestThread& t) { set_ret32(t, 0); }  // returns SIG_DFL
void t_sigaction(GuestThread& t) {                  // (sig, const sigaction* act, sigaction* old)
    if (t.regs()[1]) H32_DEBUG("sigaction(%d): guest signal handler not installed", int32_t(t.regs()[0]));
    if (gaddr old = t.regs()[2]) std::memset(mem().ptr<void>(old), 0, 16);  // bionic32 struct sigaction
    set_ret32(t, 0);
}

// int nanosleep(const struct timespec* req, struct timespec* rem) — 32-bit timespec
void t_nanosleep(GuestThread& t) {
    gaddr req = t.regs()[0];
    int64_t want = timescale::real_sleep_ns(int64_t(mem().read<int32_t>(req)) * 1000000000 + mem().read<int32_t>(req + 4));
    timespec ts{time_t(want / 1000000000), long(want % 1000000000)};
    timespec rem{};
    int r = ::nanosleep(&ts, &rem);
    if (gaddr out = t.regs()[1]) {
        mem().write<int32_t>(out, int32_t(rem.tv_sec));
        mem().write<int32_t>(out + 4, int32_t(rem.tv_nsec));
    }
    if (r != 0) sync_guest_errno(t);
    set_ret32(t, uint32_t(r));
}

void t_clock_gettime(GuestThread& t) {
    timespec ts;
    clockid_t clock = clockid_t(t.regs()[0]);
    int r = ::clock_gettime(clock, &ts);
    if (r == 0) {
        int64_t ns = timescale::virtual_ns(clock, int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec);
        mem().write<int32_t>(t.regs()[1], int32_t(ns / 1000000000));
        mem().write<int32_t>(t.regs()[1] + 4, int32_t(ns % 1000000000));
    } else {
        sync_guest_errno(t);
    }
    set_ret32(t, uint32_t(r));
}

// ---- environment: a guest-side view layered over the host environment ----
std::mutex g_env_lock;
std::unordered_map<std::string, gaddr> g_env;  // name -> guest string (or 0 = unset)

void t_getenv(GuestThread& t) {
    const char* name = mem().str(t.regs()[0]);
    if (!name) return set_ret32(t, 0);
    std::lock_guard lk(g_env_lock);
    auto it = g_env.find(name);
    if (it != g_env.end()) return set_ret32(t, it->second);
    const char* host = ::getenv(name);
    gaddr g = host ? mem().strdup(host) : 0;
    g_env[name] = g;  // cache so the pointer stays valid
    set_ret32(t, g);
}
void t_setenv(GuestThread& t) {
    const char* name = mem().str(t.regs()[0]);
    const char* value = mem().str(t.regs()[1]);
    if (!name || !value) return set_ret32(t, uint32_t(-1));
    std::lock_guard lk(g_env_lock);
    auto& slot = g_env[name];
    if (slot && !t.regs()[2]) return set_ret32(t, 0);  // overwrite == 0
    slot = mem().strdup(value);                         // old string leaks, as in libc
    set_ret32(t, 0);
}
void t_unsetenv(GuestThread& t) {
    std::lock_guard lk(g_env_lock);
    if (const char* name = mem().str(t.regs()[0])) g_env[name] = 0;
    set_ret32(t, 0);
}

// int __system_property_get(const char* name, char* value) — value buffer is 92 bytes
void t_property_get(GuestThread& t) {
    const char* name = mem().str(t.regs()[0]);
    char* out = mem().ptr<char>(t.regs()[1]);
    if (!out) return set_ret32(t, 0);
    out[0] = 0;
#ifdef __ANDROID__
    char value[92] = {};
    int n = __system_property_get(name, value);
    std::memcpy(out, value, sizeof value);
    set_ret32(t, uint32_t(n));
#else
    (void)name;
    set_ret32(t, 0);
#endif
}

}  // namespace

namespace thunks {

void register_libc_misc() {
    add("mmap", t_mmap);
    add("munmap", t_munmap);
    add("open", t_open);
    add("openat", t_openat);
    add("access", t_access);
    add("chdir", t_chdir);
    add("getcwd", t_getcwd);
    H32_ADD(dup, int(int));
    H32_ADD(dup2, int(int, int));
    H32_ADD(pipe, int(int*));
    H32_ADD(getpid, pid_t());
    H32_ADD(getuid, uid_t());
    H32_ADD(setpriority, int(int, id_t, int));
    H32_ADD(isatty, int(int));
    add("fork", t_nosys);
    add("execlp", t_nosys);
    add("execvp", t_nosys);
    add("waitpid", t_nosys);
    add("signal", t_signal);
    add("bsd_signal", t_signal);
    add("sigaction", t_sigaction);
    add("nanosleep", t_nanosleep);
    add("clock_gettime", t_clock_gettime);
    add("getenv", t_getenv);
    add("setenv", t_setenv);
    add("unsetenv", t_unsetenv);
    add("__system_property_get", t_property_get);
    add("sysconf", t_sysconf);
    add("mprotect", t_mprotect);
    add("dladdr", t_dladdr);
    add("sleep", t_sleep);
    add("usleep", t_usleep);
    add("vasprintf", t_vasprintf);
    add("asprintf", t_asprintf);
    add("__assert2", t_assert2);
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
