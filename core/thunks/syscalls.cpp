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

namespace h32 {

// Synthesizes /proc/self/maps as the guest would see it: its own modules at
// guest addresses with 32-bit Android paths. Returns a readable host FILE*.
FILE* guest_proc_maps();

namespace {

// ARM EABI syscall numbers.
enum : uint32_t {
    kExit = 1, kRead = 3, kWrite = 4, kOpen = 5, kClose = 6, kGetpid = 20, kMunmap = 91, kMprotect = 125,
    kMmap2 = 192, kMadvise = 220, kGettid = 224, kFutex = 240, kExitGroup = 248, kClockGettime = 263,
    kOpenat = 322, kCacheflush = 0x0F0002,
};

std::mutex g_maps_lock;
std::unordered_map<gaddr, uint32_t> g_mappings;  // guest mmap allocations -> size

int32_t neg_errno(int e) { return -e; }

int32_t sys_mmap2(GuestThread& t) {
    auto& r = t.regs();
    uint32_t len = r[1];
    int32_t flags = int32_t(r[3]);
    int fd = int32_t(r[4]);
    uint64_t offset = uint64_t(r[5]) * 4096;
    if (len == 0) return neg_errno(EINVAL);
    uint32_t size = (len + 0xFFF) & ~0xFFFu;
    if (flags & 0x10 /*MAP_FIXED*/) {
        // Guest memory is always mapped, so a fixed mapping just replaces the
        // contents of that range (packers use this to unpack over themselves).
        gaddr a = r[0];
        if ((a & 0xFFF) || a < Arena::kNullGuardEnd || uint64_t(a) + size > 0xFFFF0000ull) return neg_errno(EINVAL);
        std::memset(mem().ptr<void>(a), 0, size);
        if (!(flags & 0x20 /*MAP_ANONYMOUS*/) && fd >= 0 && ::pread(fd, mem().ptr<void>(a), len, off_t(offset)) < 0)
            return neg_errno(errno);
        GuestThread::invalidate_code(a, size);
        H32_DEBUG("mmap2(MAP_FIXED 0x%08x, len=0x%x, prot=%u, fd=%d)", a, len, r[2], fd);
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
    H32_DEBUG("mmap2(len=0x%x, prot=%u, flags=0x%x, fd=%d) -> 0x%08x", len, r[2], flags, fd, a);
    return int32_t(a);
}

int32_t sys_munmap(GuestThread& t) {
    gaddr a = t.regs()[0];
    std::lock_guard lk(g_maps_lock);
    auto it = g_mappings.find(a);
    if (it == g_mappings.end()) return 0;  // partial or foreign unmap: ignore
    mem().free(a);
    g_mappings.erase(it);
    return 0;
}

int32_t sys_open(GuestThread& t, int dirfd, gaddr path_addr, int flags, int mode) {
    std::string path = map_path(mem().str(path_addr));
    if (path == "/proc/self/maps") {
        FILE* f = guest_proc_maps();
        return f ? ::dup(fileno(f)) : neg_errno(ENOENT);
    }
    int fd = ::openat(dirfd, path.c_str(), flags, mode);
    return fd < 0 ? neg_errno(errno) : fd;
}

int32_t sys_clock_gettime(GuestThread& t) {
    timespec ts;
    if (::clock_gettime(clockid_t(t.regs()[0]), &ts) != 0) return neg_errno(errno);
    gaddr out = t.regs()[1];
    mem().write<int32_t>(out, int32_t(ts.tv_sec));
    mem().write<int32_t>(out + 4, int32_t(ts.tv_nsec));
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
    case kExit:
    case kExitGroup:
        H32_INFO("guest exit syscall(%d)", int32_t(r[0]));
        std::exit(int32_t(r[0]));
    default:
        H32_WARN("raw syscall %u (r0=%08x r1=%08x r2=%08x) from %s not supported", nr, r[0], r[1], r[2],
                 describe_address(r[15]).c_str());
        ret = -ENOSYS;
    }
    H32_TRACE("syscall %u -> %d", nr, ret);
    r[0] = uint32_t(ret);
}

}  // namespace

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
