// Less common libc functions (added for VLC and similar apps): math helpers,
// string/wide-char functions, file and process calls.
#include <fcntl.h>
#include <math.h>
#include <sched.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/statfs.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <unistd.h>
#include <wchar.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include "thunks/libc_internal.h"
#include "thunks/thunks.h"

namespace h32 {
namespace {

uint32_t arg(GuestThread& t, size_t i) { return t.arg_word(i); }

void fail_errno(GuestThread& t, int e) {
    mem().write<int32_t>(t.errno_addr(), e);
    set_ret32(t, uint32_t(-1));
}

// ---- math: bionic's classification values are bit flags ----

int bionic_fpclass(int c) {
    switch (c) {
    case FP_INFINITE: return 0x01;
    case FP_NAN: return 0x02;
    case FP_NORMAL: return 0x04;
    case FP_SUBNORMAL: return 0x08;
    default: return 0x10;  // FP_ZERO
    }
}
int h_fpclassifyd(double x) { return bionic_fpclass(std::fpclassify(x)); }
int h_fpclassifyf(float x) { return bionic_fpclass(std::fpclassify(x)); }
int h_isfinite(double x) { return std::isfinite(x); }
int h_isfinitef(float x) { return std::isfinite(x); }
int h_isinf(double x) { return std::isinf(x); }
int h_signbit(double x) { return std::signbit(x); }
float h_cbrtf(float x) { return ::cbrtf(x); }
float h_exp2f(float x) { return ::exp2f(x); }
float h_ldexpf(float x, int e) { return ::ldexpf(x, e); }
long long h_llrint(double x) { return ::llrint(x); }
long long h_llround(double x) { return ::llround(x); }
long long h_llroundf(float x) { return ::llroundf(x); }
double h_remainder(double a, double b) { return ::remainder(a, b); }
long long h_atoll(const char* s) { return ::atoll(s); }

// div_t div(int, int): an 8-byte struct, returned through a hidden pointer in r0.
void t_div(GuestThread& t) {
    gaddr out = arg(t, 0);
    int n = int32_t(arg(t, 1)), d = int32_t(arg(t, 2));
    div_t r = ::div(n, d);
    mem().write<int32_t>(out, r.quot);
    mem().write<int32_t>(out + 4, r.rem);
}

// lldiv_t lldiv(long long, long long): hidden pointer, then two aligned 64-bit args.
void t_lldiv(GuestThread& t) {
    ArgCursor c{t};
    gaddr out = c.word();
    long long n = int64_t(c.dword()), d = int64_t(c.dword());
    lldiv_t r = ::lldiv(n, d);
    mem().write<int64_t>(out, r.quot);
    mem().write<int64_t>(out + 8, r.rem);
}

// The *rand48 family: the guest's state is three 16-bit words, same as the host.
double h_erand48(unsigned short* x) { return ::erand48(x); }
long h_nrand48(unsigned short* x) { return ::nrand48(x); }
long h_jrand48(unsigned short* x) { return ::jrand48(x); }

// ---- strings ----

void* h_memmem(const void* h, size_t hl, const void* n, size_t nl) { return ::memmem(h, hl, n, nl); }
void* h_memrchr(const void* s, int c, size_t n) { return const_cast<void*>(::memrchr(s, c, n)); }
char* h_strcasestr(const char* h, const char* n) { return const_cast<char*>(::strcasestr(h, n)); }
size_t h_strnlen(const char* s, size_t n) { return ::strnlen(s, n); }

// char* strndup(const char*, size_t): the copy must live in guest memory.
void t_strndup(GuestThread& t) {
    const char* s = mem().str(arg(t, 0));
    if (!s) return set_ret32(t, 0);
    size_t n = ::strnlen(s, arg(t, 1));
    gaddr g = mem().malloc(n + 1);
    if (g) {
        std::memcpy(mem().ptr<char>(g), s, n);
        mem().ptr<char>(g)[n] = 0;
    }
    set_ret32(t, g);
}

// char* strsep(char** stringp, const char* delim): *stringp is a guest pointer.
void t_strsep(GuestThread& t) {
    gaddr pp = arg(t, 0);
    char* s = mem().ptr<char>(mem().read<uint32_t>(pp));
    char* tok = ::strsep(&s, mem().str(arg(t, 1)));
    mem().write<uint32_t>(pp, mem().addr(s));
    set_ret32(t, mem().addr(tok));
}

// char* strtok_r(char* str, const char* delim, char** saveptr)
void t_strtok_r(GuestThread& t) {
    gaddr savep = arg(t, 2);
    char* save = mem().ptr<char>(mem().read<uint32_t>(savep));
    char* tok = ::strtok_r(mem().ptr<char>(arg(t, 0)), mem().str(arg(t, 1)), &save);
    mem().write<uint32_t>(savep, mem().addr(save));
    set_ret32(t, mem().addr(tok));
}

// wchar_t is 4 bytes on both sides.
wchar_t* h_wcscpy(wchar_t* d, const wchar_t* s) { return ::wcscpy(d, s); }
int h_wcwidth(int c) { return ::wcwidth(wchar_t(c)); }

// long wcstol(const wchar_t*, wchar_t** end, int base)
void t_wcstol(GuestThread& t) {
    wchar_t* end = nullptr;
    errno = 0;
    long v = ::wcstol(mem().ptr<const wchar_t>(arg(t, 0)), &end, int32_t(arg(t, 2)));
    if (gaddr e = arg(t, 1)) mem().write<uint32_t>(e, mem().addr(end));
    // The guest's long is 32-bit: clamp like a 32-bit wcstol would.
    if (v > INT32_MAX) { v = INT32_MAX; errno = ERANGE; }
    if (v < INT32_MIN) { v = INT32_MIN; errno = ERANGE; }
    set_ret32(t, uint32_t(int32_t(v)));
    sync_guest_errno(t);
}

// ---- time: guest time_t is 32-bit; struct tm starts with the same nine ints ----

tm guest_tm(gaddr g) {
    tm r{};
    int* f[] = {&r.tm_sec, &r.tm_min, &r.tm_hour, &r.tm_mday, &r.tm_mon, &r.tm_year, &r.tm_wday, &r.tm_yday, &r.tm_isdst};
    for (int i = 0; i < 9; i++) *f[i] = mem().read<int32_t>(g + i * 4);
    return r;
}

// char* asctime_r(const struct tm*, char* buf)
void t_asctime_r(GuestThread& t) {
    tm v = guest_tm(arg(t, 0));
    char* out = ::asctime_r(&v, mem().ptr<char>(arg(t, 1)));
    set_ret32(t, out ? arg(t, 1) : 0);
}

// char* ctime(const time_t*)
void t_ctime(GuestThread& t) {
    thread_local gaddr buf = 0;
    if (!buf) buf = mem().alloc_static(32);
    time_t tt = mem().read<int32_t>(arg(t, 0));
    char* r = ::ctime_r(&tt, mem().ptr<char>(buf));
    set_ret32(t, r ? buf : 0);
}

// ---- files ----

int h_ftruncate(int fd, int32_t len) { return ::ftruncate(fd, len); }
long long h_lseek64(int fd, long long off, int whence) { return ::lseek64(fd, off, whence); }
int h_pread(int fd, void* buf, size_t n, int32_t off) { return int(::pread(fd, buf, n, off)); }
int h_pipe2(int* fds, int flags) { return ::pipe2(fds, flags); }

// ssize_t readv(int fd, const struct iovec* iov, int cnt): guest iovec is 8 bytes.
void t_readv(GuestThread& t) {
    int cnt = int32_t(arg(t, 2));
    gaddr iov = arg(t, 1);
    std::vector<iovec> v(size_t(cnt > 0 ? cnt : 0));
    for (int i = 0; i < cnt; i++) {
        v[i].iov_base = mem().ptr<void>(mem().read<uint32_t>(iov + i * 8));
        v[i].iov_len = mem().read<uint32_t>(iov + i * 8 + 4);
    }
    errno = 0;
    set_ret32(t, uint32_t(::readv(int32_t(arg(t, 0)), v.data(), cnt)));
    sync_guest_errno(t);
}

// long pathconf(const char* path, int name)
void t_pathconf(GuestThread& t) {
    std::string p = map_path(mem().str(arg(t, 0)));
    errno = 0;
    set_ret32(t, uint32_t(::pathconf(p.c_str(), int32_t(arg(t, 1)))));
    sync_guest_errno(t);
}

// int fstatfs(int fd, struct statfs*): 32-bit bionic layout (84 bytes):
// type bsize blocks(8) bfree(8) bavail(8) files(8) ffree(8) fsid(8) namelen frsize flags spare[4]
void t_fstatfs(GuestThread& t) {
    struct statfs s{};
    errno = 0;
    int r = ::fstatfs(int32_t(arg(t, 0)), &s);
    sync_guest_errno(t);
    if (r == 0) {
        gaddr g = arg(t, 1);
        std::memset(mem().ptr<void>(g), 0, 84);
        mem().write<uint32_t>(g + 0, uint32_t(s.f_type));
        mem().write<uint32_t>(g + 4, uint32_t(s.f_bsize));
        mem().write<uint64_t>(g + 8, s.f_blocks);
        mem().write<uint64_t>(g + 16, s.f_bfree);
        mem().write<uint64_t>(g + 24, s.f_bavail);
        mem().write<uint64_t>(g + 32, s.f_files);
        mem().write<uint64_t>(g + 40, s.f_ffree);
        std::memcpy(mem().ptr<void>(g + 48), &s.f_fsid, 8);
        mem().write<uint32_t>(g + 56, uint32_t(s.f_namelen));
        mem().write<uint32_t>(g + 60, uint32_t(s.f_frsize));
        mem().write<uint32_t>(g + 64, uint32_t(s.f_flags));
    }
    set_ret32(t, uint32_t(r));
}

// ---- processes and signals ----

// int getrusage(int who, struct rusage*): guest = two 8-byte timevals + 14 32-bit longs.
void t_getrusage(GuestThread& t) {
    rusage u{};
    errno = 0;
    int r = ::getrusage(int32_t(arg(t, 0)), &u);
    sync_guest_errno(t);
    if (r == 0) {
        gaddr g = arg(t, 1);
        mem().write<int32_t>(g + 0, int32_t(u.ru_utime.tv_sec));
        mem().write<int32_t>(g + 4, int32_t(u.ru_utime.tv_usec));
        mem().write<int32_t>(g + 8, int32_t(u.ru_stime.tv_sec));
        mem().write<int32_t>(g + 12, int32_t(u.ru_stime.tv_usec));
        long rest[14] = {u.ru_maxrss, u.ru_ixrss, u.ru_idrss, u.ru_isrss, u.ru_minflt, u.ru_majflt, u.ru_nswap,
                         u.ru_inblock, u.ru_oublock, u.ru_msgsnd, u.ru_msgrcv, u.ru_nsignals, u.ru_nvcsw, u.ru_nivcsw};
        for (int i = 0; i < 14; i++) mem().write<int32_t>(g + 16 + i * 4, int32_t(rest[i]));
    }
    set_ret32(t, uint32_t(r));
}

// Guest sigset_t is 4 bytes (signals 1..32).
sigset_t host_sigset(gaddr g) {
    sigset_t s;
    sigemptyset(&s);
    uint32_t bits = g ? mem().read<uint32_t>(g) : 0;
    for (int i = 0; i < 32; i++)
        if (bits & (1u << i)) sigaddset(&s, i + 1);
    return s;
}

// int sigwait(const sigset_t*, int* sig)
void t_sigwait(GuestThread& t) {
    sigset_t s = host_sigset(arg(t, 0));
    int sig = 0;
    int r = ::sigwait(&s, &sig);
    if (r == 0 && arg(t, 1)) mem().write<int32_t>(arg(t, 1), sig);
    set_ret32(t, uint32_t(r));
}

// int sigpending(sigset_t*): nothing is ever pending for translated code.
void t_sigpending(GuestThread& t) {
    if (gaddr g = arg(t, 0)) mem().write<uint32_t>(g, 0);
    set_ret32(t, 0);
}

void t_nosys(GuestThread& t) { fail_errno(t, ENOSYS); }
void t_eperm(GuestThread& t) { fail_errno(t, EPERM); }
void t_nop(GuestThread&) {}

}  // namespace

namespace thunks {

void register_libc_extra() {
    add("__fpclassifyd", H32_WRAP(h_fpclassifyd, int(double)));
    add("__fpclassify", H32_WRAP(h_fpclassifyd, int(double)));
    add("__fpclassifyf", H32_WRAP(h_fpclassifyf, int(float)));
    add("__isfinite", H32_WRAP(h_isfinite, int(double)));
    add("__isfinitef", H32_WRAP(h_isfinitef, int(float)));
    add("__isinf", H32_WRAP(h_isinf, int(double)));
    add("__signbit", H32_WRAP(h_signbit, int(double)));
    add("cbrtf", H32_WRAP(h_cbrtf, float(float)));
    add("exp2f", H32_WRAP(h_exp2f, float(float)));
    add("ldexpf", H32_WRAP(h_ldexpf, float(float, int)));
    add("llrint", H32_WRAP(h_llrint, long long(double)));
    add("llround", H32_WRAP(h_llround, long long(double)));
    add("llroundf", H32_WRAP(h_llroundf, long long(float)));
    add("remainder", H32_WRAP(h_remainder, double(double, double)));
    add("atoll", H32_WRAP(h_atoll, long long(const char*)));
    add("div", t_div);
    add("lldiv", t_lldiv);
    add("erand48", H32_WRAP(h_erand48, double(unsigned short*)));
    add("nrand48", H32_WRAP(h_nrand48, long(unsigned short*)));
    add("jrand48", H32_WRAP(h_jrand48, long(unsigned short*)));

    add("memmem", H32_WRAP(h_memmem, void*(const void*, size_t, const void*, size_t)));
    add("memrchr", H32_WRAP(h_memrchr, void*(const void*, int, size_t)));
    add("strcasestr", H32_WRAP(h_strcasestr, char*(const char*, const char*)));
    add("strnlen", H32_WRAP(h_strnlen, size_t(const char*, size_t)));
    add("strndup", t_strndup);
    add("strsep", t_strsep);
    add("strtok_r", t_strtok_r);
    add("wcscpy", H32_WRAP(h_wcscpy, wchar_t*(wchar_t*, const wchar_t*)));
    add("wcwidth", H32_WRAP(h_wcwidth, int(int)));
    add("wcstol", t_wcstol);

    add("asctime_r", t_asctime_r);
    add("ctime", t_ctime);

    H32_ADD(fchdir, int(int));
    H32_ADD(flock, int(int, int));
    H32_ADD(fsync, int(int));
    add("ftruncate", H32_WRAP(h_ftruncate, int(int, int32_t)));
    add("lseek64", H32_WRAP(h_lseek64, long long(int, long long, int)));
    add("pread", H32_WRAP(h_pread, int(int, void*, size_t, int32_t)));
    add("pipe2", H32_WRAP(h_pipe2, int(int*, int)));
    add("readv", t_readv);
    add("pathconf", t_pathconf);
    add("fstatfs", t_fstatfs);
    H32_ADD(mlock, int(const void*, size_t));
    H32_ADD(eventfd, int(unsigned int, int));

    H32_ADD(getppid, pid_t());
    H32_ADD(geteuid, uid_t());
    H32_ADD(getgid, gid_t());
    add("setuid", t_eperm);
    add("daemon", t_nosys);
    add("vfork", t_nosys);
    add("getrusage", t_getrusage);
    H32_ADD(uname, int(utsname*));  // six char[65] fields: same layout
    add("sigwait", t_sigwait);
    add("sigpending", t_sigpending);
    H32_ADD(sched_get_priority_max, int(int));
    H32_ADD(sched_get_priority_min, int(int));
    H32_ADD(sched_getparam, int(pid_t, sched_param*));
    H32_ADD(sched_setscheduler, int(pid_t, int, const sched_param*));
    H32_ADD(setlogmask, int(int));
    add("__google_potentially_blocking_region_begin", t_nop);
    add("__google_potentially_blocking_region_end", t_nop);
}

}  // namespace thunks
}  // namespace h32
