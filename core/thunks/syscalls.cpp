// Raw Linux syscalls: guest code (packers, anti-tamper, hand-written asm) may
// skip libc and execute "svc #0" with the ARM EABI syscall number in r7.
// SVC 0 is reserved for this; thunk stubs use SVC 1 and up.
#include <fcntl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cstring>
#include <mutex>
#include <unordered_map>

#include "loader/elf_loader.h"
#include "thunks/libc_internal.h"
#include "thunks/thunks.h"
#include "thunks/timescale.h"

namespace h32 {

// Synthesizes /proc/self/maps as the guest would see it: its own modules at
// guest addresses with 32-bit Android paths. Returns a readable host FILE*.
FILE* guest_proc_maps();

namespace {
std::mutex g_maps_lock;
std::unordered_map<gaddr, uint32_t> g_mappings;  // guest mmap allocations -> size
int32_t neg_errno(int e) { return -e; }
}  // namespace

namespace {

// ARM EABI syscall numbers.
enum : uint32_t {
    kExit = 1, kRead = 3, kWrite = 4, kOpen = 5, kClose = 6, kGetpid = 20, kMunmap = 91, kMprotect = 125,
    kMmap2 = 192, kMadvise = 220, kGettid = 224, kFutex = 240, kExitGroup = 248, kClockGettime = 263,
    kOpenat = 322, kCacheflush = 0x0F0002,
};


}  // namespace

// mmap for the guest; returns the guest address or -errno.
int32_t guest_mmap(gaddr hint, uint32_t len, uint32_t prot, int32_t flags, int fd, uint64_t offset) {
    if (len == 0) return neg_errno(EINVAL);
    uint32_t size = (len + 0xFFF) & ~0xFFFu;
    if (flags & 0x10 /*MAP_FIXED*/) {
        // Guest memory is always mapped, so a fixed mapping just replaces the
        // contents of that range (packers use this to unpack over themselves).
        gaddr a = hint;
        if ((a & 0xFFF) || a < Arena::kNullGuardEnd || uint64_t(a) + size > 0xFFFF0000ull) return neg_errno(EINVAL);
        std::memset(mem().ptr<void>(a), 0, size);
        if (!(flags & 0x20 /*MAP_ANONYMOUS*/) && fd >= 0 && ::pread(fd, mem().ptr<void>(a), len, off_t(offset)) < 0)
            return neg_errno(errno);
        GuestThread::invalidate_code(a, size);
        H32_DEBUG("mmap(MAP_FIXED 0x%08x, len=0x%x, prot=%u, fd=%d)", a, len, prot, fd);
        return int32_t(a);
    }
    gaddr a = mem().memalign(4096, size);
    if (!a) return neg_errno(ENOMEM);
    std::memset(mem().ptr<void>(a), 0, size);
    if (!(flags & 0x20 /*MAP_ANONYMOUS*/) && fd >= 0) {
        ssize_t n = ::pread(fd, mem().ptr<void>(a), len, off_t(offset));
        if (n < 0) {
            int e = errno;
            mem().free(a);
            return neg_errno(e);
        }
    }
    std::lock_guard lk(g_maps_lock);
    g_mappings[a] = size;
    H32_DEBUG("mmap(len=0x%x, prot=%u, flags=0x%x, fd=%d) -> 0x%08x", len, prot, flags, fd, a);
    return int32_t(a);
}

int32_t guest_munmap(gaddr a) {
    std::lock_guard lk(g_maps_lock);
    auto it = g_mappings.find(a);
    if (it == g_mappings.end()) return 0;  // partial or foreign unmap: ignore
    mem().free(a);
    g_mappings.erase(it);
    return 0;
}

// ARM and arm64 share open() flag values; x86 differs for these four.
int guest_open_flags(int f) {
#if defined(__x86_64__)
    constexpr int kArmDirectory = 040000, kArmNofollow = 0100000, kArmDirect = 0200000, kArmLargefile = 0400000;
    int out = f & ~(kArmDirectory | kArmNofollow | kArmDirect | kArmLargefile);
    if (f & kArmDirectory) out |= O_DIRECTORY;
    if (f & kArmNofollow) out |= O_NOFOLLOW;
    if (f & kArmDirect) out |= O_DIRECT;
    return out;
#else
    return f;
#endif
}

FILE* guest_proc_maps();

// open/openat for the guest (path mapping, synthesized /proc/self/maps); fd or -errno.
int32_t guest_open(int dirfd, const char* guest_path, int flags, int mode) {
    std::string path = map_path(guest_path);
    if (path == "/proc/self/maps") {
        FILE* f = guest_proc_maps();
        return f ? ::dup(fileno(f)) : neg_errno(ENOENT);
    }
    int fd = ::openat(dirfd, path.c_str(), guest_open_flags(flags), mode);
    H32_DEBUG("open(\"%s\", 0x%x) -> %d", path.c_str(), flags, fd);
    return fd < 0 ? neg_errno(errno) : fd;
}

namespace {

int32_t sys_mmap2(GuestThread& t) {
    auto& r = t.regs();
    return guest_mmap(r[0], r[1], r[2], int32_t(r[3]), int32_t(r[4]), uint64_t(r[5]) * 4096);
}

int32_t sys_munmap(GuestThread& t) { return guest_munmap(t.regs()[0]); }

int32_t sys_open(GuestThread& t, int dirfd, gaddr path_addr, int flags, int mode) {
    return guest_open(dirfd, mem().str(path_addr), flags, mode);
}

// futex(uaddr, op, val, timeout | val2, uaddr2, val3). Guest addresses are
// arena addresses, so the host futex works on the very same words. The 4th
// argument is a (32-bit) timespec for the wait ops and a plain number otherwise.
// Timeouts follow the speed setting: WAIT's is relative (scaled), WAIT_BITSET's
// is an absolute guest time on CLOCK_MONOTONIC (or CLOCK_REALTIME with
// FUTEX_CLOCK_REALTIME), turned into the matching real time on that clock.
int32_t sys_futex(gaddr uaddr, int op, uint32_t val, uint32_t arg4, gaddr uaddr2, uint32_t val3) {
    int cmd = op & 127 & ~256;  // without FUTEX_PRIVATE_FLAG / FUTEX_CLOCK_REALTIME
    const void* a4 = reinterpret_cast<const void*>(uintptr_t(arg4));
    timespec ts{};
    if ((cmd == 0 /* WAIT */ || cmd == 9 /* WAIT_BITSET */) && arg4) {
        int32_t sec = mem().read<int32_t>(arg4), nsec = mem().read<int32_t>(arg4 + 4);
        if (nsec < 0 || nsec >= 1000000000) return neg_errno(EINVAL);
        int64_t guest_ns = int64_t(sec) * 1000000000 + nsec, real_ns;
        if (cmd == 0) {
            real_ns = timescale::real_sleep_ns(guest_ns);
        } else {
            clockid_t clock = (op & 256) ? CLOCK_REALTIME : CLOCK_MONOTONIC;
            timespec now;
            ::clock_gettime(clock, &now);
            real_ns = int64_t(now.tv_sec) * 1000000000 + now.tv_nsec + timescale::real_ns_until(clock, guest_ns);
        }
        ts = {time_t(real_ns / 1000000000), long(real_ns % 1000000000)};
        a4 = &ts;
    }
    long r = ::syscall(SYS_futex, mem().ptr<uint32_t>(uaddr), op, val, a4, mem().ptr<uint32_t>(uaddr2), val3);
    return r < 0 ? neg_errno(errno) : int32_t(r);
}

int32_t sys_clock_gettime(GuestThread& t) {
    timespec ts;
    clockid_t clock = clockid_t(t.regs()[0]);
    if (::clock_gettime(clock, &ts) != 0) return neg_errno(errno);
    int64_t ns = timescale::virtual_ns(clock, int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec);
    gaddr out = t.regs()[1];
    mem().write<int32_t>(out, int32_t(ns / 1000000000));
    mem().write<int32_t>(out + 4, int32_t(ns % 1000000000));
    return 0;
}

// The kernel returns -errno in r0; libc wrappers then set errno.
void t_linux_syscall(GuestThread& t) {
    auto& r = t.regs();
    uint32_t nr = r[7];
    int32_t ret;
    switch (nr) {
    case kRead: ret = int32_t(::read(int32_t(r[0]), mem().ptr<void>(r[1]), r[2])); if (ret < 0) ret = -errno; break;
    case kWrite: ret = int32_t(::write(int32_t(r[0]), mem().ptr<void>(r[1]), r[2])); if (ret < 0) ret = -errno; break;
    case kOpen: ret = sys_open(t, AT_FDCWD, r[0], int32_t(r[1]), int32_t(r[2])); break;
    case kOpenat: ret = sys_open(t, int32_t(r[0]) == -100 ? AT_FDCWD : int32_t(r[0]), r[1], int32_t(r[2]), int32_t(r[3])); break;
    case kClose: ret = ::close(int32_t(r[0])) == 0 ? 0 : -errno; break;
    case kGetpid: ret = getpid(); break;
    case kGettid: ret = int32_t(::syscall(SYS_gettid)); break;
    case kMmap2: ret = sys_mmap2(t); break;
    case kMunmap: ret = sys_munmap(t); break;
    case kMprotect:
        GuestThread::invalidate_code(r[0], r[1]);
        ret = 0;
        break;
    case kCacheflush:
        GuestThread::invalidate_code(r[0], r[1] - r[0]);
        ret = 0;
        break;
    case kMadvise: ret = 0; break;
    case kClockGettime: ret = sys_clock_gettime(t); break;
    case kFutex: ret = sys_futex(r[0], int32_t(r[1]), r[2], r[3], r[4], r[5]); break;
    case kExit:
        // ARM EABI exit (1) ends only the calling thread. Stop its guest code
        // like pthread_exit does; the thread's owner cleans up after it.
        H32_INFO("guest exit syscall(%d): ending this thread", int32_t(r[0]));
        if (t.depth() > 1) H32_WARN("exit syscall from a nested call: unwinding only the innermost guest frame");
        t.request_exit(0);
        ret = 0;
        break;
    case kExitGroup:
        // exit_group (248) ends the whole process at once: no atexit handlers.
        H32_INFO("guest exit_group syscall(%d)", int32_t(r[0]));
        std::fflush(nullptr);
        ::_exit(int32_t(r[0]));
    default:
        H32_WARN("raw syscall %u (r0=%08x r1=%08x r2=%08x) from %s not supported", nr, r[0], r[1], r[2],
                 describe_address(r[15]).c_str());
        ret = -ENOSYS;
    }
    H32_TRACE("syscall %u -> %d", nr, ret);
    r[0] = uint32_t(ret);
}

}  // namespace

// libc syscall(nr, ...): run it like a raw "svc #0" with the arguments in
// r0-r5 and the number in r7. Returns the kernel-style result (-errno on error).
int32_t guest_raw_syscall(GuestThread& t, uint32_t nr, const uint32_t (&args)[6]) {
    auto& r = t.regs();
    uint32_t saved[8];
    for (int i = 0; i < 8; i++) saved[i] = r[i];
    for (int i = 0; i < 6; i++) r[i] = args[i];
    r[7] = nr;
    t_linux_syscall(t);
    int32_t ret = int32_t(r[0]);
    for (int i = 0; i < 8; i++) r[i] = saved[i];
    return ret;
}

FILE* guest_proc_maps() {
    FILE* f = std::tmpfile();
    if (!f) return nullptr;
    for (Module* m : all_modules()) {
        // One line per module, covering its whole image (r-xp is what scanners look for).
        fprintf(f, "%08x-%08x r-xp 00000000 fd:00 0          /data/app/thumb/lib/arm/%s\n", m->base,
                (m->base + m->size + 0xFFF) & ~0xFFFu, m->name.c_str());
    }
    fprintf(f, "%08x-%08x rw-p 00000000 00:00 0          [anon:libc_malloc]\n", Arena::kHeapBase, Arena::kHeapEnd);
    fprintf(f, "ffff0000-ffff1000 r-xp 00000000 00:00 0          [vectors]\n");
    std::rewind(f);
    return f;
}

namespace thunks {

void register_syscalls() {
    // Must be the very first thunk registered so it gets SVC number 0.
    add("__linux_syscall", t_linux_syscall);
    const char* first = name_of(0);
    if (!first || std::strcmp(first, "__linux_syscall") != 0) fatal("syscall thunk must be registered first (SVC 0)");
}

}  // namespace thunks
}  // namespace h32
