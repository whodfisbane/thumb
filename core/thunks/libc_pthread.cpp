// <pthread.h>. Guest pthread objects are small (a bionic32 mutex is 4 bytes),
// so each one is backed by a host object found through its guest address.
#include <pthread.h>

#include <atomic>
#include <memory>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "thunks/libc_internal.h"
#include "thunks/thunks.h"

namespace h32 {
namespace {

// ---- mutexes ----
// Always recursive: harmless for correct code, and some games lock twice.
std::mutex g_mutex_table_lock;
std::unordered_map<gaddr, std::recursive_mutex*> g_mutexes;

std::recursive_mutex* host_mutex(gaddr g) {
    std::lock_guard lk(g_mutex_table_lock);
    auto& m = g_mutexes[g];
    if (!m) m = new std::recursive_mutex();
    return m;
}

void t_mutex_init(GuestThread& t) {
    host_mutex(t.regs()[0]);
    set_ret32(t, 0);
}
void t_mutex_destroy(GuestThread& t) {
    std::lock_guard lk(g_mutex_table_lock);
    auto it = g_mutexes.find(t.regs()[0]);
    if (it != g_mutexes.end()) {
        delete it->second;
        g_mutexes.erase(it);
    }
    set_ret32(t, 0);
}
void t_mutex_lock(GuestThread& t) {
    host_mutex(t.regs()[0])->lock();
    set_ret32(t, 0);
}
void t_mutex_trylock(GuestThread& t) { set_ret32(t, host_mutex(t.regs()[0])->try_lock() ? 0 : EBUSY); }
void t_mutex_unlock(GuestThread& t) {
    host_mutex(t.regs()[0])->unlock();
    set_ret32(t, 0);
}

// ---- condition variables (guest pthread_cond_t is 4 bytes) ----
std::mutex g_cond_table_lock;
std::unordered_map<gaddr, std::condition_variable_any*> g_conds;

std::condition_variable_any* host_cond(gaddr g) {
    std::lock_guard lk(g_cond_table_lock);
    auto& c = g_conds[g];
    if (!c) c = new std::condition_variable_any();
    return c;
}

void t_cond_init(GuestThread& t) {
    host_cond(t.regs()[0]);
    set_ret32(t, 0);
}
void t_cond_destroy(GuestThread& t) {
    std::lock_guard lk(g_cond_table_lock);
    auto it = g_conds.find(t.regs()[0]);
    if (it != g_conds.end()) {
        delete it->second;
        g_conds.erase(it);
    }
    set_ret32(t, 0);
}
void t_cond_wait(GuestThread& t) {
    host_cond(t.regs()[0])->wait(*host_mutex(t.regs()[1]));
    set_ret32(t, 0);
}
// int pthread_cond_timedwait(cond, mutex, const struct timespec* abstime) — 32-bit timespec, CLOCK_REALTIME
void t_cond_timedwait(GuestThread& t) {
    gaddr ts = t.regs()[2];
    auto since_epoch = std::chrono::seconds(mem().read<int32_t>(ts)) + std::chrono::nanoseconds(mem().read<int32_t>(ts + 4));
    auto deadline = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(since_epoch));
    auto r = host_cond(t.regs()[0])->wait_until(*host_mutex(t.regs()[1]), deadline);
    set_ret32(t, r == std::cv_status::timeout ? ETIMEDOUT : 0);
}
void t_cond_signal(GuestThread& t) {
    host_cond(t.regs()[0])->notify_one();
    set_ret32(t, 0);
}
void t_cond_broadcast(GuestThread& t) {
    host_cond(t.regs()[0])->notify_all();
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
std::atomic<uint32_t> g_next_tid{2};
thread_local uint32_t t_self_id = 0;

uint32_t self_id() {
    if (!t_self_id) t_self_id = g_next_tid++;
    return t_self_id;
}

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

// ---- mutex attributes: mutexes are always recursive, so just record the type ----
void t_mutexattr_init(GuestThread& t) {
    mem().write<uint32_t>(t.regs()[0], 0);
    set_ret32(t, 0);
}
void t_mutexattr_settype(GuestThread& t) {
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
    gaddr ts = t.regs()[1];
    auto since = std::chrono::seconds(mem().read<int32_t>(ts)) + std::chrono::nanoseconds(mem().read<int32_t>(ts + 4));
    auto deadline = std::chrono::system_clock::time_point(std::chrono::duration_cast<std::chrono::system_clock::duration>(since));
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
    add("pthread_once", t_once);
    add("pthread_cond_init", t_cond_init);
    add("pthread_cond_destroy", t_cond_destroy);
    add("pthread_cond_wait", t_cond_wait);
    add("pthread_cond_timedwait", t_cond_timedwait);
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
    add("pthread_condattr_init", t_attr_ok);
    add("pthread_condattr_destroy", t_attr_ok);
    add("pthread_condattr_setclock", t_attr_ok);
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
