// Core libc: heap, errno, process exit, stack protector, setjmp/longjmp,
// ctype tables, C++ runtime hooks.
#include <unistd.h>
#include <sys/syscall.h>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <mutex>
#include <random>
#include <vector>

#include "loader/elf_loader.h"
#include "thunks/libc_internal.h"
#include "thunks/thunks.h"

namespace h32 {

void sync_guest_errno(GuestThread& t) { mem().write<int32_t>(t.errno_addr(), errno); }

gaddr thread_scratch(int slot, size_t size) {
    thread_local gaddr bufs[kScratchCount] = {};
    thread_local size_t sizes[kScratchCount] = {};
    if (sizes[slot] < size) {
        mem().free(bufs[slot]);
        bufs[slot] = mem().malloc(size);
        sizes[slot] = size;
    }
    return bufs[slot];
}

namespace {

ArgCursor args(GuestThread& t) { return ArgCursor{t}; }

// ---- heap ----
void t_malloc(GuestThread& t) { set_ret32(t, mem().malloc(t.regs()[0])); }
void t_calloc(GuestThread& t) { set_ret32(t, mem().calloc(t.regs()[0], t.regs()[1])); }
void t_realloc(GuestThread& t) { set_ret32(t, mem().realloc(t.regs()[0], t.regs()[1])); }
void t_free(GuestThread& t) { mem().free(t.regs()[0]); }
void t_memalign(GuestThread& t) { set_ret32(t, mem().memalign(t.regs()[0], t.regs()[1])); }
void t_posix_memalign(GuestThread& t) {
    gaddr out = t.regs()[0];
    gaddr p = mem().memalign(t.regs()[1], t.regs()[2]);
    if (!p) return set_ret32(t, ENOMEM);
    mem().write<uint32_t>(out, p);
    set_ret32(t, 0);
}
void t_malloc_usable_size(GuestThread& t) { set_ret32(t, uint32_t(mem().usable_size(t.regs()[0]))); }

// ---- errno / stack protector / abort ----
void t_errno(GuestThread& t) { set_ret32(t, t.errno_addr()); }

void t_stack_chk_fail(GuestThread& t) {
    fatal("guest stack corruption detected (__stack_chk_fail) called from %s", describe_address(t.regs()[14]).c_str());
}

void t_abort(GuestThread& t) {
    fatal("guest called abort() from %s", describe_address(t.regs()[14]).c_str());
}

void t_raise(GuestThread& t) {
    int sig = int32_t(t.regs()[0]);
    if (sig == SIGABRT || sig == SIGSEGV || sig == SIGKILL)
        fatal("guest raised signal %d from %s", sig, describe_address(t.regs()[14]).c_str());
    H32_WARN("guest raise(%d) ignored", sig);
    set_ret32(t, 0);
}

// ---- exit / atexit ----
struct AtExit {
    gaddr fn, arg, dso;
};
std::mutex g_atexit_mutex;
std::vector<AtExit> g_atexit;

void t_cxa_atexit(GuestThread& t) {
    std::lock_guard lk(g_atexit_mutex);
    g_atexit.push_back({t.regs()[0], t.regs()[1], t.regs()[2]});
    set_ret32(t, 0);
}
void t_aeabi_atexit(GuestThread& t) {  // (arg, fn, dso)
    std::lock_guard lk(g_atexit_mutex);
    g_atexit.push_back({t.regs()[1], t.regs()[0], t.regs()[2]});
    set_ret32(t, 0);
}
void t_cxa_finalize(GuestThread&) {}  // the process ends without unloading
void t_exit(GuestThread& t) {
    H32_INFO("guest called exit(%d)", int32_t(t.regs()[0]));
    std::exit(int32_t(t.regs()[0]));
}

// ---- setjmp / longjmp ----
// Our jmp_buf layout (guest has room for 64 words):
//   [0..10]  r4-r14   [11..26] d8-d15 (as 16 words)   [27] fpscr marker
void t_setjmp(GuestThread& t) {
    auto& r = t.regs();
    gaddr env = r[0];
    for (int i = 4; i <= 14; i++) mem().write<uint32_t>(env + (i - 4) * 4, r[i]);
    auto& x = t.ext_regs();
    for (int i = 0; i < 16; i++) mem().write<uint32_t>(env + 44 + i * 4, x[16 + i]);
    set_ret32(t, 0);
}

void t_longjmp(GuestThread& t) {
    auto r = t.regs();
    gaddr env = r[0];
    uint32_t val = r[1] ? r[1] : 1;
    for (int i = 4; i <= 14; i++) r[i] = mem().read<uint32_t>(env + (i - 4) * 4);
    auto& x = t.ext_regs();
    for (int i = 0; i < 16; i++) x[16 + i] = mem().read<uint32_t>(env + 44 + i * 4);
    r[0] = val;
    r[15] = r[14] & ~1u;
    uint32_t cpsr = (t.cpsr() & ~(1u << 5)) | ((r[14] & 1) << 5);
    t.request_jump(r, cpsr);
}

// ---- C++ exception support ----
// int* __gnu_Unwind_Find_exidx(_Unwind_Ptr pc, int* count)
void t_find_exidx(GuestThread& t) {
    Module* m = module_containing(t.regs()[0]);
    gaddr count = t.regs()[1];
    if (!m || !m->exidx) {
        if (count) mem().write<int32_t>(count, 0);
        return set_ret32(t, 0);
    }
    if (count) mem().write<int32_t>(count, int32_t(m->exidx_count));
    set_ret32(t, m->exidx);
}

// ---- environment / process ----
void t_system(GuestThread& t) {
    H32_WARN("guest system(\"%s\") refused", mem().str(t.regs()[0]));
    set_ret32(t, uint32_t(-1));
}

// long syscall(long number, ...) — numbers are ARM EABI.
void t_syscall(GuestThread& t) {
    uint32_t nr = t.regs()[0];
    switch (nr) {
    case 224:  // gettid
        set_ret32(t, uint32_t(::syscall(SYS_gettid)));
        return;
    case 20:  // getpid
        set_ret32(t, uint32_t(getpid()));
        return;
    default: {
        // Everything else goes through the raw syscall layer (futex, ...).
        uint32_t args[6];
        for (int i = 0; i < 6; i++) args[i] = t.arg_word(1 + i);
        int32_t r = guest_raw_syscall(t, nr, args);
        if (r < 0 && r > -4096) {
            mem().write<int32_t>(t.errno_addr(), -r);
            r = -1;
        }
        set_ret32(t, uint32_t(r));
    }
    }
}

// ---- qsort with a guest comparator ----
void t_qsort(GuestThread& t) {
    gaddr base = t.regs()[0];
    uint32_t n = t.regs()[1], size = t.regs()[2];
    gaddr cmp = t.regs()[3];
    if (n < 2 || size == 0) return;
    // Sort an index array with the guest comparator, then permute once.
    std::vector<uint32_t> idx(n);
    for (uint32_t i = 0; i < n; i++) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) {
        GuestArgs ga;
        ga.u32(base + a * size).u32(base + b * size);
        return int32_t(t.call(cmp, ga).r0) < 0;
    });
    std::vector<uint8_t> tmp(size_t(n) * size);
    for (uint32_t i = 0; i < n; i++) std::memcpy(&tmp[size_t(i) * size], mem().ptr<uint8_t>(base + idx[i] * size), size);
    std::memcpy(mem().ptr<uint8_t>(base), tmp.data(), tmp.size());
}

// void* bsearch(const void* key, const void* base, size_t n, size_t size, int (*cmp)(const void*, const void*))
void t_bsearch(GuestThread& t) {
    ArgCursor c{t};
    gaddr key = c.word(), base = c.word();
    uint32_t n = c.word(), size = c.word();
    gaddr cmp = c.word();
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        gaddr elem = base + mid * size;
        GuestArgs ga;
        ga.u32(key).u32(elem);
        int32_t r = int32_t(t.call(cmp, ga).r0);
        if (r == 0) return set_ret32(t, elem);
        if (r < 0) hi = mid;
        else lo = mid + 1;
    }
    set_ret32(t, 0);
}

// ---- __aeabi memory helpers (note the argument orders) ----
void t_aeabi_memcpy(GuestThread& t) {
    std::memcpy(mem().ptr<void>(t.regs()[0]), mem().ptr<void>(t.regs()[1]), t.regs()[2]);
}
void t_aeabi_memmove(GuestThread& t) {
    std::memmove(mem().ptr<void>(t.regs()[0]), mem().ptr<void>(t.regs()[1]), t.regs()[2]);
}
void t_aeabi_memset(GuestThread& t) {  // (dest, n, c)
    std::memset(mem().ptr<void>(t.regs()[0]), int(t.regs()[2]), t.regs()[1]);
}
void t_aeabi_memclr(GuestThread& t) {  // (dest, n)
    std::memset(mem().ptr<void>(t.regs()[0]), 0, t.regs()[1]);
}

// ---- ctype ----
// bionic exports `const char* _ctype_` pointing at a 257-entry BSD table
// (index 0 is EOF). Inline isalpha() etc. in old NDK headers read it.
gaddr make_ctype_table() {
    enum { U = 0x01, L = 0x02, N = 0x04, S = 0x08, P = 0x10, C = 0x20, X = 0x40, B = 0x80 };
    gaddr table = mem().alloc_static(257, 4);
    for (int c = 0; c < 256; c++) {
        uint8_t f = 0;
        if (c < 128) {
            if (isupper(c)) f |= U;
            if (islower(c)) f |= L;
            if (isdigit(c)) f |= N;
            if (isspace(c)) f |= S;
            if (ispunct(c)) f |= P;
            if (iscntrl(c)) f |= C;
            if (isxdigit(c) && !isdigit(c)) f |= X;
            if (c == ' ') f |= B;
        }
        mem().write<uint8_t>(table + 1 + c, f);
    }
    gaddr var = mem().alloc_static(4, 4);
    mem().write<uint32_t>(var, table);
    return var;
}

}  // namespace

namespace thunks {

void register_libc_core() {
    add("malloc", t_malloc);
    add("calloc", t_calloc);
    add("realloc", t_realloc);
    add("free", t_free);
    add("memalign", t_memalign);
    add("posix_memalign", t_posix_memalign);
    add("malloc_usable_size", t_malloc_usable_size);

    add("__errno", t_errno);
    add("__stack_chk_fail", t_stack_chk_fail);
    add("abort", t_abort);
    add("raise", t_raise);
    add("__cxa_atexit", t_cxa_atexit);
    add("__aeabi_atexit", t_aeabi_atexit);
    add("atexit", +[](GuestThread& t) {
        std::lock_guard lk(g_atexit_mutex);
        g_atexit.push_back({t.regs()[0], 0, 0});
        set_ret32(t, 0);
    });
    add("__cxa_finalize", t_cxa_finalize);
    add("exit", t_exit);
    add("_exit", t_exit);

    add("setjmp", t_setjmp);
    add("_setjmp", t_setjmp);
    add("longjmp", t_longjmp);
    add("_longjmp", t_longjmp);

    add("__gnu_Unwind_Find_exidx", t_find_exidx);
    add("dl_unwind_find_exidx", t_find_exidx);  // newer NDK name, same signature
    add("system", t_system);
    add("syscall", t_syscall);
    add("qsort", t_qsort);
    add("bsearch", t_bsearch);

    for (const char* n : {"__aeabi_memcpy", "__aeabi_memcpy4", "__aeabi_memcpy8"}) add(n, t_aeabi_memcpy);
    for (const char* n : {"__aeabi_memmove", "__aeabi_memmove4", "__aeabi_memmove8"}) add(n, t_aeabi_memmove);
    for (const char* n : {"__aeabi_memset", "__aeabi_memset4", "__aeabi_memset8"}) add(n, t_aeabi_memset);
    for (const char* n : {"__aeabi_memclr", "__aeabi_memclr4", "__aeabi_memclr8"}) add(n, t_aeabi_memclr);

    // Data symbols.
    gaddr guard = mem().alloc_static(4, 4);
    mem().write<uint32_t>(guard, std::random_device{}() | 0x01000000);
    add_data("__stack_chk_guard", guard);
    add_data("_ctype_", make_ctype_table());

    (void)args;
}

}  // namespace thunks
}  // namespace h32
