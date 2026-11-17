// <pthread.h>. Mutexes and condition variables live in the guest's own 4-byte
// objects (bionic32 layouts) and sleep on host futexes; the other pthread
// objects are backed by host objects found through their guest address.
#include <linux/futex.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "jni/jni_bridge.h"
#include "thunks/libc_internal.h"
#include "thunks/thunks.h"
#include "thunks/timescale.h"

namespace h32 {
namespace {

// ---- thread ids ----
// Guest pthread_t values are small ids, handed out on first use.
std::atomic<uint32_t> g_next_tid{2};
thread_local uint32_t t_self_id = 0;

uint32_t self_id() {
    if (!t_self_id) t_self_id = g_next_tid++;
    return t_self_id;
}

// ---- futexes and deadlines ----
// Arena addresses are real host memory, so a host futex works on guest words.
std::atomic_ref<uint32_t> guest_word(gaddr a) { return std::atomic_ref<uint32_t>(*mem().ptr<uint32_t>(a)); }

// Sleeps while the word at `a` still holds `expected`, for at most `real_ns`
// (< 0: no limit). Returns 0, or EAGAIN / EINTR / ETIMEDOUT.
int futex_wait(gaddr a, uint32_t expected, int64_t real_ns) {
    timespec ts{}, *tp = nullptr;
    if (real_ns >= 0) {
        ts = {time_t(real_ns / 1000000000), long(real_ns % 1000000000)};
        tp = &ts;
    }
    long r = ::syscall(SYS_futex, mem().ptr<uint32_t>(a), FUTEX_WAIT_PRIVATE, expected, tp, nullptr, 0);
    return r == 0 ? 0 : errno;
}
void futex_wake(gaddr a, int count) {
    ::syscall(SYS_futex, mem().ptr<uint32_t>(a), FUTEX_WAKE_PRIVATE, count, nullptr, nullptr, 0);
}

// When a timed wait gives up, as a guest-clock time (the clock the guest
// computed it from), so the speed setting can't skew it.
struct Deadline {
    bool forever = true;
    clockid_t clock = CLOCK_REALTIME;
    int64_t guest_abs_ns = 0;

    static Deadline never() { return {}; }
    static Deadline at(clockid_t c, int64_t ns) { return {false, c, ns}; }
    static Deadline in(int64_t guest_ns) { return at(CLOCK_MONOTONIC, timescale::now_ns(CLOCK_MONOTONIC) + guest_ns); }
    // Real ns left: -1 = no limit, 0 = passed.
    int64_t real_left() const { return forever ? -1 : timescale::real_ns_until(clock, guest_abs_ns); }
};

// Reads a guest (32-bit) struct timespec as nanoseconds; false if invalid.
bool read_timespec(gaddr ts, int64_t& ns) {
    if (!ts) return false;
    int32_t sec = mem().read<int32_t>(ts), nsec = mem().read<int32_t>(ts + 4);
    if (nsec < 0 || nsec >= 1000000000) return false;
    ns = int64_t(sec) * 1000000000 + nsec;
    return true;
}

// ---- mutexes ----
// The guest's pthread_mutex_t word is the lock itself (bionic32 layout):
//   bits 0-1   state: 0 unlocked, 1 locked, 2 locked and someone may be waiting
//   bits 2-12  how many more times the owner has locked it (recursion)
//   bits 14-15 type: 0 normal, 1 recursive, 2 errorcheck
//   bits 16-31 owner: THUMB thread id (1..65535)
// So the static initializers games compile in (PTHREAD_MUTEX_INITIALIZER = 0,
// PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP = 0x4000, ..._ERRORCHECK_... = 0x8000)
// just work, nothing is allocated per mutex, and memory reused after a mutex
// was freed without pthread_mutex_destroy is simply a new mutex.
constexpr uint32_t kStateMask = 3, kCountOne = 1u << 2, kCountMask = 0x7FFu << 2;
constexpr uint32_t kTypeMask = 3u << 14, kTypeNormal = 0, kTypeRecursive = 1u << 14, kTypeErrorcheck = 2u << 14;
constexpr uint32_t kOwnerMask = 0xFFFF0000u;

uint32_t owner_bits() { return (self_id() % 0xFFFF + 1) << 16; }

// Bionic deadlocks when a thread locks a normal mutex it already holds. THUMB
// has always let that through (some games do it), so keep doing so, but say so.
void warn_normal_relock(gaddr m) {
    static std::atomic<int> warned{0};
    if (warned.fetch_add(1) < 5) H32_WARN("thread relocked normal mutex 0x%08x it already holds; treating it as recursive", m);
}

int mutex_lock(gaddr m, const Deadline& deadline) {
    auto w = guest_word(m);
    const uint32_t me = owner_bits();
    uint32_t v = w.load(std::memory_order_relaxed);
    const uint32_t type = v & kTypeMask;
    if (type == kTypeMask) return EINVAL;
    if ((v & kStateMask) && (v & kOwnerMask) == me) {
        if (type == kTypeErrorcheck) return EDEADLK;
        if (type == kTypeNormal) warn_normal_relock(m);
        for (;;) {  // only the owner changes the count; others may flip state 1 -> 2
            if ((v & kCountMask) == kCountMask) return EAGAIN;
            if (w.compare_exchange_weak(v, v + kCountOne, std::memory_order_relaxed)) return 0;
        }
    }
    uint32_t unlocked = type;
    if (w.compare_exchange_strong(unlocked, type | me | 1, std::memory_order_acquire)) return 0;
    // Contended: mark the mutex as having waiters, then sleep until an unlock
    // wakes us. Taking it in state 2 keeps later unlocks waking the others.
    for (;;) {
        v = w.load(std::memory_order_relaxed);
        if ((v & kStateMask) == 0) {
            if (w.compare_exchange_weak(v, type | me | 2, std::memory_order_acquire)) return 0;
            continue;
        }
        if ((v & kStateMask) == 1 && !w.compare_exchange_weak(v, v + 1, std::memory_order_relaxed)) continue;
        v = (v & ~kStateMask) | 2;
        int64_t left = deadline.real_left();
        if (left == 0) return ETIMEDOUT;
        futex_wait(m, v, left);  // woken, changed, timed out or interrupted: look again
    }
}

int mutex_trylock(gaddr m) {
    auto w = guest_word(m);
    const uint32_t me = owner_bits();
    uint32_t v = w.load(std::memory_order_relaxed);
    const uint32_t type = v & kTypeMask;
    if (type == kTypeMask) return EINVAL;
    if (v & kStateMask) {
        if ((v & kOwnerMask) != me || type != kTypeRecursive) return EBUSY;  // owner too, like bionic
        for (;;) {
            if ((v & kCountMask) == kCountMask) return EAGAIN;
            if (w.compare_exchange_weak(v, v + kCountOne, std::memory_order_relaxed)) return 0;
        }
    }
    return w.compare_exchange_strong(v, type | me | 1, std::memory_order_acquire) ? 0 : EBUSY;
}

int mutex_unlock(gaddr m) {
    auto w = guest_word(m);
    uint32_t v = w.load(std::memory_order_relaxed);
    const uint32_t type = v & kTypeMask;
    if (type == kTypeMask) return EINVAL;
    if ((v & kStateMask) == 0) return type == kTypeNormal ? 0 : EPERM;
    if ((v & kOwnerMask) != owner_bits() && type != kTypeNormal) return EPERM;
    while (v & kCountMask)  // a nested level: count down, stay locked
        if (w.compare_exchange_weak(v, v - kCountOne, std::memory_order_relaxed)) return 0;
    if ((w.exchange(type, std::memory_order_release) & kStateMask) == 2) futex_wake(m, 1);
    return 0;
}

void t_mutex_init(GuestThread& t) {  // (mutex, const pthread_mutexattr_t* attr)
    uint32_t kind = t.regs()[1] ? mem().read<uint32_t>(t.regs()[1]) & 0xF : 0;
    if (kind > 2) return set_ret32(t, EINVAL);
    mem().write<uint32_t>(t.regs()[0], kind << 14);
    set_ret32(t, 0);
}
void t_mutex_destroy(GuestThread& t) {
    set_ret32(t, (mem().read<uint32_t>(t.regs()[0]) & kStateMask) ? EBUSY : 0);
}
void t_mutex_lock(GuestThread& t) { set_ret32(t, mutex_lock(t.regs()[0], Deadline::never())); }
void t_mutex_trylock(GuestThread& t) { set_ret32(t, mutex_trylock(t.regs()[0])); }
void t_mutex_unlock(GuestThread& t) { set_ret32(t, mutex_unlock(t.regs()[0])); }
template <clockid_t Clock>
void t_mutex_timedlock(GuestThread& t) {  // (mutex, const struct timespec* abstime)
    int64_t abs;
    if (!read_timespec(t.regs()[1], abs)) return set_ret32(t, EINVAL);
    set_ret32(t, mutex_lock(t.regs()[0], Deadline::at(Clock, abs)));
}
void t_mutex_lock_timeout_np(GuestThread& t) {  // (mutex, unsigned ms): old bionic32, EBUSY on timeout
    int r = mutex_lock(t.regs()[0], Deadline::in(int64_t(t.regs()[1]) * 1000000));
    set_ret32(t, r == ETIMEDOUT ? EBUSY : r);
}

// ---- condition variables ----
// The guest's pthread_cond_t word (bionic32 layout): bit 0 shared, bit 1 set
// = timed waits use CLOCK_MONOTONIC, bits 2-31 a counter that signal and
// broadcast bump. A waiter sleeps on the value it saw before releasing the
// mutex, so a signal in between wakes it at once. PTHREAD_COND_INITIALIZER
// (0) just works.
constexpr uint32_t kCondShared = 1, kCondMonotonic = 2, kCondStep = 4;

int cond_wait(gaddr c, gaddr m, const Deadline& deadline) {
    const uint32_t seq = guest_word(c).load(std::memory_order_relaxed);
    // Release the mutex completely, even if this thread holds it several
    // levels deep (otherwise nobody else can ever signal), and restore the
    // levels once it has the mutex back.
    auto mw = guest_word(m);
    uint32_t v = mw.load(std::memory_order_relaxed), levels = 0;
    if ((v & kStateMask) && (v & kOwnerMask) == owner_bits())
        while (v & kCountMask) {
            uint32_t n = v & kCountMask;
            if (mw.compare_exchange_weak(v, v & ~kCountMask, std::memory_order_relaxed)) {
                levels = n;
                break;
            }
        }
    mutex_unlock(m);
    int64_t left = deadline.real_left();
    int r = left == 0 ? ETIMEDOUT : futex_wait(c, seq, left);
    mutex_lock(m, Deadline::never());
    if (levels) mw.fetch_add(levels, std::memory_order_relaxed);
    return r == ETIMEDOUT ? ETIMEDOUT : 0;  // anything else is a (possibly spurious) wakeup
}

void t_cond_init(GuestThread& t) {  // (cond, const pthread_condattr_t* attr)
    uint32_t flags = t.regs()[1] ? mem().read<uint32_t>(t.regs()[1]) & (kCondShared | kCondMonotonic) : 0;
    mem().write<uint32_t>(t.regs()[0], flags);
    set_ret32(t, 0);
}
void t_cond_destroy(GuestThread& t) { set_ret32(t, 0); }
void t_cond_wait(GuestThread& t) { set_ret32(t, cond_wait(t.regs()[0], t.regs()[1], Deadline::never())); }
// int pthread_cond_timedwait(cond, mutex, const struct timespec* abstime): on
// the clock chosen by pthread_condattr_setclock (CLOCK_REALTIME by default).
void t_cond_timedwait(GuestThread& t) {
    int64_t abs;
    if (!read_timespec(t.regs()[2], abs)) return set_ret32(t, EINVAL);
    clockid_t clock = (mem().read<uint32_t>(t.regs()[0]) & kCondMonotonic) ? CLOCK_MONOTONIC : CLOCK_REALTIME;
    set_ret32(t, cond_wait(t.regs()[0], t.regs()[1], Deadline::at(clock, abs)));
}
// pthread_cond_timedwait_monotonic(_np): old bionic32 calls, CLOCK_MONOTONIC.
void t_cond_timedwait_monotonic(GuestThread& t) {
    int64_t abs;
    if (!read_timespec(t.regs()[2], abs)) return set_ret32(t, EINVAL);
    set_ret32(t, cond_wait(t.regs()[0], t.regs()[1], Deadline::at(CLOCK_MONOTONIC, abs)));
}
void t_cond_timedwait_relative_np(GuestThread& t) {  // (cond, mutex, const struct timespec* reltime)
    int64_t rel;
    if (!read_timespec(t.regs()[2], rel)) return set_ret32(t, EINVAL);
    set_ret32(t, cond_wait(t.regs()[0], t.regs()[1], Deadline::in(rel)));
}
void t_cond_timeout_np(GuestThread& t) {  // (cond, mutex, unsigned ms)
    set_ret32(t, cond_wait(t.regs()[0], t.regs()[1], Deadline::in(int64_t(t.regs()[2]) * 1000000)));
}
void t_cond_clockwait(GuestThread& t) {  // (cond, mutex, clockid_t, const struct timespec* abstime)
    clockid_t clock = clockid_t(t.regs()[2]);
    int64_t abs;
    if ((clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC) || !read_timespec(t.regs()[3], abs))
        return set_ret32(t, EINVAL);
    set_ret32(t, cond_wait(t.regs()[0], t.regs()[1], Deadline::at(clock, abs)));
}
void t_cond_signal(GuestThread& t) {
    guest_word(t.regs()[0]).fetch_add(kCondStep, std::memory_order_release);
    futex_wake(t.regs()[0], 1);
    set_ret32(t, 0);
}
void t_cond_broadcast(GuestThread& t) {
    guest_word(t.regs()[0]).fetch_add(kCondStep, std::memory_order_release);
    futex_wake(t.regs()[0], INT_MAX);
    set_ret32(t, 0);
}

// pthread_condattr_t (bionic32: a long holding the same flag bits as the cond).
void t_condattr_init(GuestThread& t) {
    mem().write<uint32_t>(t.regs()[0], 0);
    set_ret32(t, 0);
}
void t_condattr_setclock(GuestThread& t) {  // (attr, clockid_t)
    gaddr a = t.regs()[0];
    clockid_t clock = clockid_t(t.regs()[1]);
    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC) return set_ret32(t, EINVAL);
    uint32_t f = mem().read<uint32_t>(a) & ~kCondMonotonic;
    mem().write<uint32_t>(a, clock == CLOCK_MONOTONIC ? (f | kCondMonotonic) : f);
    set_ret32(t, 0);
}
void t_condattr_getclock(GuestThread& t) {  // (attr, clockid_t* out)
    bool mono = mem().read<uint32_t>(t.regs()[0]) & kCondMonotonic;
    mem().write<int32_t>(t.regs()[1], mono ? CLOCK_MONOTONIC : CLOCK_REALTIME);
    set_ret32(t, 0);
}
void t_condattr_setpshared(GuestThread& t) {  // (attr, int): process-shared is just a flag here
    gaddr a = t.regs()[0];
    uint32_t f = mem().read<uint32_t>(a) & ~kCondShared;
    mem().write<uint32_t>(a, t.regs()[1] ? (f | kCondShared) : f);
    set_ret32(t, 0);
}

// ---- once ----
// Each pthread_once_t holds its own state: 0 = not started (PTHREAD_ONCE_INIT),
// 1 = running, 2 = done. The lock only guards state changes, never the init
// function, so inits on different controls can run (and wait on each other)
// concurrently; callers of a control that's running wait for it to finish.
std::mutex g_once_lock;
std::condition_variable g_once_done;
void t_once(GuestThread& t) {
    gaddr ctl = t.regs()[0];
    gaddr fn = t.regs()[1];
    {
        std::unique_lock lk(g_once_lock);
        for (;;) {
            int32_t state = mem().read<int32_t>(ctl);
            if (state == 2) return set_ret32(t, 0);
            if (state == 0) break;
            g_once_done.wait(lk);
        }
        mem().write<int32_t>(ctl, 1);
    }
    t.call(fn);
    {
        std::lock_guard lk(g_once_lock);
        mem().write<int32_t>(ctl, 2);
    }
    g_once_done.notify_all();
    set_ret32(t, 0);
}

// ---- thread-specific data ----
std::mutex g_keys_lock;
bool g_key_used[GuestThread::kMaxKeys];
gaddr g_key_dtor[GuestThread::kMaxKeys];

void t_key_create(GuestThread& t) {
    std::lock_guard lk(g_keys_lock);
    for (int k = 0; k < GuestThread::kMaxKeys; k++)
        if (!g_key_used[k]) {
            g_key_used[k] = true;
            g_key_dtor[k] = t.regs()[1];
            mem().write<uint32_t>(t.regs()[0], uint32_t(k));
            return set_ret32(t, 0);
        }
    set_ret32(t, EAGAIN);
}
void t_key_delete(GuestThread& t) {
    uint32_t k = t.regs()[0];
    std::lock_guard lk(g_keys_lock);
    if (k < GuestThread::kMaxKeys) g_key_used[k] = false;
    set_ret32(t, 0);
}
void t_getspecific(GuestThread& t) {
    uint32_t k = t.regs()[0];
    set_ret32(t, k < GuestThread::kMaxKeys ? t.tls_values[k] : 0);
}
void t_setspecific(GuestThread& t) {
    uint32_t k = t.regs()[0];
    if (k >= GuestThread::kMaxKeys) return set_ret32(t, EINVAL);
    t.tls_values[k] = t.regs()[1];
    set_ret32(t, 0);
}

// ---- threads ----
// Guest pthread_t values are small ids; joinable threads keep their state
// here until joined.
struct ThreadState {
    std::mutex m;
    std::condition_variable cv;
    bool done = false, detached = false;
    uint32_t retval = 0;
};
std::mutex g_threads_lock;
std::unordered_map<uint32_t, std::shared_ptr<ThreadState>> g_threads;

// bionic32 pthread_attr_t (24 bytes): flags, stack_base, stack_size, guard_size, sched_policy, sched_priority
constexpr uint32_t kAttrDetached = 1;
void t_attr_init(GuestThread& t) {
    gaddr a = t.regs()[0];
    const uint32_t v[6] = {0, 0, 1024 * 1024, 4096, 0, 0};
    for (int i = 0; i < 6; i++) mem().write<uint32_t>(a + i * 4, v[i]);
    set_ret32(t, 0);
}
void t_attr_ok(GuestThread& t) { set_ret32(t, 0); }
void t_attr_setdetachstate(GuestThread& t) {
    gaddr a = t.regs()[0];
    uint32_t f = mem().read<uint32_t>(a);
    mem().write<uint32_t>(a, t.regs()[1] == 1 ? (f | kAttrDetached) : (f & ~kAttrDetached));
    set_ret32(t, 0);
}
void t_attr_getdetachstate(GuestThread& t) {
    mem().write<int32_t>(t.regs()[1], (mem().read<uint32_t>(t.regs()[0]) & kAttrDetached) ? 1 : 0);
    set_ret32(t, 0);
}
void t_attr_setstacksize(GuestThread& t) {
    mem().write<uint32_t>(t.regs()[0] + 8, t.regs()[1]);
    set_ret32(t, 0);
}
void t_attr_getstacksize(GuestThread& t) {
    mem().write<uint32_t>(t.regs()[1], mem().read<uint32_t>(t.regs()[0] + 8));
    set_ret32(t, 0);
}

void run_tls_destructors(GuestThread& gt) {
    for (int pass = 0; pass < 4; pass++) {
        bool any = false;
        for (int k = 0; k < GuestThread::kMaxKeys; k++) {
            gaddr v = gt.tls_values[k];
            if (!v || !g_key_used[k] || !g_key_dtor[k]) continue;
            gt.tls_values[k] = 0;
            GuestArgs da;
            da.u32(v);
            gt.call(g_key_dtor[k], da);
            any = true;
        }
        if (!any) break;
    }
}

// int pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*)
void t_create(GuestThread& t) {
    gaddr out = t.regs()[0];
    gaddr attr = t.regs()[1];
    gaddr start = t.regs()[2];
    gaddr arg = t.regs()[3];
    uint32_t id = g_next_tid++;
    auto st = std::make_shared<ThreadState>();
    st->detached = attr && (mem().read<uint32_t>(attr) & kAttrDetached);
    {
        std::lock_guard lk(g_threads_lock);
        g_threads[id] = st;
    }
    if (out) mem().write<uint32_t>(out, id);
    H32_DEBUG("pthread_create #%u -> %s", id, describe_address(start).c_str());
    std::thread([start, arg, id, st] {
        t_self_id = id;
        auto& gt = GuestThread::current();
        GuestArgs a;
        a.u32(arg);
        uint32_t ret = gt.call(start, a).r0;
        run_tls_destructors(gt);
        jni::detach_current_thread();  // after the destructors: they may use JNI
        bool detached;
        {
            std::lock_guard lk(st->m);
            st->done = true;
            st->retval = ret;
            detached = st->detached;
        }
        st->cv.notify_all();
        if (detached) {
            std::lock_guard lk(g_threads_lock);
            g_threads.erase(id);
        }
        H32_DEBUG("guest thread #%u finished", id);
    }).detach();
    set_ret32(t, 0);
}

std::shared_ptr<ThreadState> find_thread(uint32_t id) {
    std::lock_guard lk(g_threads_lock);
    auto it = g_threads.find(id);
    return it == g_threads.end() ? nullptr : it->second;
}

// int pthread_join(pthread_t, void** retval)
void t_join(GuestThread& t) {
    uint32_t id = t.regs()[0];
    gaddr out = t.regs()[1];
    auto st = find_thread(id);
    if (!st) return set_ret32(t, ESRCH);
    {
        std::unique_lock lk(st->m);
        st->cv.wait(lk, [&] { return st->done; });
        if (out) mem().write<uint32_t>(out, st->retval);
    }
    std::lock_guard lk(g_threads_lock);
    g_threads.erase(id);
    set_ret32(t, 0);
}

void t_detach(GuestThread& t) {
    uint32_t id = t.regs()[0];
    auto st = find_thread(id);
    if (!st) return set_ret32(t, ESRCH);
    bool done;
    {
        std::lock_guard lk(st->m);
        st->detached = true;
        done = st->done;
    }
    if (done) {
        std::lock_guard lk(g_threads_lock);
        g_threads.erase(id);
    }
    set_ret32(t, 0);
}

// Innermost __pthread_cleanup_t of this thread (see __pthread_cleanup_push below).
thread_local gaddr t_cleanup_stack = 0;

// pthread_exit runs the thread's cleanup handlers first, newest first, like
// bionic. Apps that cancel threads by exiting (VLC) unlock mutexes and signal
// "finished" in them; skipping them leaves other threads waiting forever.
void t_exit(GuestThread& t) {
    uint32_t value = t.regs()[0];
    while (gaddr c = t_cleanup_stack) {
        t_cleanup_stack = mem().read<uint32_t>(c);
        gaddr routine = mem().read<uint32_t>(c + 4);
        if (routine) t.call(routine, GuestArgs().u32(mem().read<uint32_t>(c + 8)));
    }
    if (t.depth() > 1) H32_WARN("pthread_exit from a nested call: unwinding only the innermost guest frame");
    t.request_exit(value);
}

void t_self(GuestThread& t) { set_ret32(t, self_id()); }
void t_equal(GuestThread& t) { set_ret32(t, t.regs()[0] == t.regs()[1] ? 1 : 0); }

// Signals are owned by the host process; guests get harmless answers.
void t_kill(GuestThread& t) {
    if (t.regs()[1] != 0) H32_WARN("pthread_kill(#%u, %d) ignored", t.regs()[0], int32_t(t.regs()[1]));
    set_ret32(t, 0);
}
void t_sigmask(GuestThread& t) {  // (how, const sigset_t* set, sigset_t* old) — 4-byte sigset on bionic32
    if (gaddr old = t.regs()[2]) mem().write<uint32_t>(old, 0);
    set_ret32(t, 0);
}

// ---- mutex attributes (bionic32: a long, type in the low bits) ----
void t_mutexattr_init(GuestThread& t) {
    mem().write<uint32_t>(t.regs()[0], 0);
    set_ret32(t, 0);
}
void t_mutexattr_settype(GuestThread& t) {  // NORMAL 0, RECURSIVE 1, ERRORCHECK 2
    if (t.regs()[1] > 2) return set_ret32(t, EINVAL);
    mem().write<uint32_t>(t.regs()[0], t.regs()[1]);
    set_ret32(t, 0);
}
void t_mutexattr_gettype(GuestThread& t) {
    mem().write<uint32_t>(t.regs()[1], mem().read<uint32_t>(t.regs()[0]));
    set_ret32(t, 0);
}

// ---- semaphores (guest sem_t is 4 bytes) ----
struct Sem {
    std::mutex m;
    std::condition_variable cv;
    uint32_t count = 0;
};
std::mutex g_sems_lock;
std::unordered_map<gaddr, Sem*> g_sems;

Sem* host_sem(gaddr g) {
    std::lock_guard lk(g_sems_lock);
    auto& s = g_sems[g];
    if (!s) s = new Sem();
    return s;
}

void t_sem_init(GuestThread& t) {  // (sem, pshared, value)
    Sem* s = host_sem(t.regs()[0]);
    std::lock_guard lk(s->m);
    s->count = t.regs()[2];
    set_ret32(t, 0);
}
void t_sem_destroy(GuestThread& t) {
    std::lock_guard lk(g_sems_lock);
    auto it = g_sems.find(t.regs()[0]);
    if (it != g_sems.end()) {
        delete it->second;
        g_sems.erase(it);
    }
    set_ret32(t, 0);
}
void t_sem_post(GuestThread& t) {
    Sem* s = host_sem(t.regs()[0]);
    {
        std::lock_guard lk(s->m);
        s->count++;
    }
    s->cv.notify_one();
    set_ret32(t, 0);
}
void t_sem_wait(GuestThread& t) {
    Sem* s = host_sem(t.regs()[0]);
    std::unique_lock lk(s->m);
    s->cv.wait(lk, [&] { return s->count > 0; });
    s->count--;
    set_ret32(t, 0);
}
void t_sem_trywait(GuestThread& t) {
    Sem* s = host_sem(t.regs()[0]);
    std::lock_guard lk(s->m);
    if (s->count == 0) {
        mem().write<int32_t>(t.errno_addr(), EAGAIN);
        return set_ret32(t, uint32_t(-1));
    }
    s->count--;
    set_ret32(t, 0);
}
void t_sem_timedwait(GuestThread& t) {  // (sem, const struct timespec* abstime) — CLOCK_REALTIME
    Sem* s = host_sem(t.regs()[0]);
    int64_t abs;
    if (!read_timespec(t.regs()[1], abs)) {
        mem().write<int32_t>(t.errno_addr(), EINVAL);
        return set_ret32(t, uint32_t(-1));
    }
    // The guest computed abstime from its (virtual) CLOCK_REALTIME.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timescale::real_ns_until(CLOCK_REALTIME, abs));
    std::unique_lock lk(s->m);
    if (!s->cv.wait_until(lk, deadline, [&] { return s->count > 0; })) {
        mem().write<int32_t>(t.errno_addr(), ETIMEDOUT);
        return set_ret32(t, uint32_t(-1));
    }
    s->count--;
    set_ret32(t, 0);
}
void t_sem_getvalue(GuestThread& t) {
    Sem* s = host_sem(t.regs()[0]);
    std::lock_guard lk(s->m);
    mem().write<int32_t>(t.regs()[1], int32_t(s->count));
    set_ret32(t, 0);
}

// ---- read-write locks: one host pthread_rwlock per guest lock (tracks reader/writer itself) ----

std::mutex g_rwlock_table_lock;
std::unordered_map<gaddr, pthread_rwlock_t*> g_rwlocks;

pthread_rwlock_t* host_rwlock(gaddr g) {
    std::lock_guard lk(g_rwlock_table_lock);
    auto& l = g_rwlocks[g];
    if (!l) {
        l = new pthread_rwlock_t;
        pthread_rwlock_init(l, nullptr);
    }
    return l;
}

void t_rwlock_init(GuestThread& t) {
    host_rwlock(t.regs()[0]);
    set_ret32(t, 0);
}
void t_rwlock_destroy(GuestThread& t) {
    std::lock_guard lk(g_rwlock_table_lock);
    auto it = g_rwlocks.find(t.regs()[0]);
    if (it != g_rwlocks.end()) {
        pthread_rwlock_destroy(it->second);
        delete it->second;
        g_rwlocks.erase(it);
    }
    set_ret32(t, 0);
}
template <int (*Fn)(pthread_rwlock_t*)>
void t_rwlock_op(GuestThread& t) { set_ret32(t, uint32_t(Fn(host_rwlock(t.regs()[0])))); }

// ---- cleanup handlers (pthread_cleanup_push/pop macros) ----
// Guest __pthread_cleanup_t: prev(4) routine(4) arg(4), on the guest stack.
// t_cleanup_stack is declared with pthread_exit, which runs the handlers.

// void __pthread_cleanup_push(__pthread_cleanup_t* c, void (*routine)(void*), void* arg)
void t_cleanup_push(GuestThread& t) {
    gaddr c = t.regs()[0];
    mem().write<uint32_t>(c, t_cleanup_stack);
    mem().write<uint32_t>(c + 4, t.regs()[1]);
    mem().write<uint32_t>(c + 8, t.regs()[2]);
    t_cleanup_stack = c;
}

// void __pthread_cleanup_pop(__pthread_cleanup_t* c, int execute)
void t_cleanup_pop(GuestThread& t) {
    gaddr c = t.regs()[0];
    bool execute = t.regs()[1] != 0;
    t_cleanup_stack = mem().read<uint32_t>(c);
    if (execute) {
        gaddr routine = mem().read<uint32_t>(c + 4);
        if (routine) t.call(routine, GuestArgs().u32(mem().read<uint32_t>(c + 8)));
    }
}

// int pthread_getschedparam(pthread_t, int* policy, struct sched_param*): normal scheduling.
void t_getschedparam(GuestThread& t) {
    if (gaddr p = t.regs()[1]) mem().write<int32_t>(p, SCHED_OTHER);
    if (gaddr s = t.regs()[2]) mem().write<int32_t>(s, 0);
    set_ret32(t, 0);
}

}  // namespace

namespace thunks {

void register_libc_pthread() {
    add("pthread_rwlock_init", t_rwlock_init);
    add("pthread_rwlock_destroy", t_rwlock_destroy);
    add("pthread_rwlock_rdlock", t_rwlock_op<pthread_rwlock_rdlock>);
    add("pthread_rwlock_wrlock", t_rwlock_op<pthread_rwlock_wrlock>);
    add("pthread_rwlock_tryrdlock", t_rwlock_op<pthread_rwlock_tryrdlock>);
    add("pthread_rwlock_trywrlock", t_rwlock_op<pthread_rwlock_trywrlock>);
    add("pthread_rwlock_unlock", t_rwlock_op<pthread_rwlock_unlock>);
    add("__pthread_cleanup_push", t_cleanup_push);
    add("__pthread_cleanup_pop", t_cleanup_pop);
    add("pthread_getschedparam", t_getschedparam);
    add("pthread_mutex_init", t_mutex_init);
    add("pthread_mutex_destroy", t_mutex_destroy);
    add("pthread_mutex_lock", t_mutex_lock);
    add("pthread_mutex_trylock", t_mutex_trylock);
    add("pthread_mutex_unlock", t_mutex_unlock);
    add("pthread_mutex_timedlock", t_mutex_timedlock<CLOCK_REALTIME>);
    add("pthread_mutex_timedlock_monotonic_np", t_mutex_timedlock<CLOCK_MONOTONIC>);
    add("pthread_mutex_lock_timeout_np", t_mutex_lock_timeout_np);
    add("pthread_once", t_once);
    add("pthread_cond_init", t_cond_init);
    add("pthread_cond_destroy", t_cond_destroy);
    add("pthread_cond_wait", t_cond_wait);
    add("pthread_cond_timedwait", t_cond_timedwait);
    add("pthread_cond_timedwait_monotonic", t_cond_timedwait_monotonic);
    add("pthread_cond_timedwait_monotonic_np", t_cond_timedwait_monotonic);
    add("pthread_cond_timedwait_relative_np", t_cond_timedwait_relative_np);
    add("pthread_cond_timeout_np", t_cond_timeout_np);
    add("pthread_cond_clockwait", t_cond_clockwait);
    add("pthread_cond_signal", t_cond_signal);
    add("pthread_cond_broadcast", t_cond_broadcast);
    add("pthread_key_create", t_key_create);
    add("pthread_key_delete", t_key_delete);
    add("pthread_getspecific", t_getspecific);
    add("pthread_setspecific", t_setspecific);
    add("pthread_create", t_create);
    add("pthread_self", t_self);
    add("pthread_join", t_join);
    add("pthread_detach", t_detach);
    add("pthread_exit", t_exit);
    add("pthread_equal", t_equal);
    add("pthread_kill", t_kill);
    add("pthread_sigmask", t_sigmask);
    add("sigprocmask", t_sigmask);
    add("pthread_attr_init", t_attr_init);
    add("pthread_attr_destroy", t_attr_ok);
    add("pthread_attr_setdetachstate", t_attr_setdetachstate);
    add("pthread_attr_getdetachstate", t_attr_getdetachstate);
    add("pthread_attr_setstacksize", t_attr_setstacksize);
    add("pthread_attr_getstacksize", t_attr_getstacksize);
    add("pthread_attr_setschedpolicy", t_attr_ok);
    add("pthread_attr_setschedparam", t_attr_ok);
    add("pthread_attr_setguardsize", t_attr_ok);
    add("pthread_setname_np", t_attr_ok);
    add("pthread_setschedparam", t_attr_ok);
    add("pthread_mutexattr_init", t_mutexattr_init);
    add("pthread_mutexattr_destroy", t_attr_ok);
    add("pthread_mutexattr_settype", t_mutexattr_settype);
    add("pthread_mutexattr_gettype", t_mutexattr_gettype);
    add("pthread_mutexattr_setpshared", t_attr_ok);
    add("pthread_condattr_init", t_condattr_init);
    add("pthread_condattr_destroy", t_attr_ok);
    add("pthread_condattr_setclock", t_condattr_setclock);
    add("pthread_condattr_getclock", t_condattr_getclock);
    add("pthread_condattr_setpshared", t_condattr_setpshared);
    add("sem_init", t_sem_init);
    add("sem_destroy", t_sem_destroy);
    add("sem_post", t_sem_post);
    add("sem_wait", t_sem_wait);
    add("sem_trywait", t_sem_trywait);
    add("sem_timedwait", t_sem_timedwait);
    add("sem_getvalue", t_sem_getvalue);
    add("sched_yield", +[](GuestThread& t) {
        std::this_thread::yield();
        set_ret32(t, 0);
    });
}

}  // namespace thunks
}  // namespace h32
