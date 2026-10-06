// <pthread.h>. Guest pthread objects are small (a bionic32 mutex is 4 bytes),
// so each one is backed by a host object found through its guest address.
#include <pthread.h>

#include <atomic>
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

// ---- once ----
std::recursive_mutex g_once_lock;
void t_once(GuestThread& t) {
    gaddr ctl = t.regs()[0];
    gaddr fn = t.regs()[1];
    std::lock_guard lk(g_once_lock);
    if (mem().read<int32_t>(ctl) == 0) {
        mem().write<int32_t>(ctl, 1);
        t.call(fn);
    }
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
std::atomic<uint32_t> g_next_tid{2};

// int pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*)
void t_create(GuestThread& t) {
    gaddr out = t.regs()[0];
    gaddr start = t.regs()[2];
    gaddr arg = t.regs()[3];
    uint32_t id = g_next_tid++;
    if (out) mem().write<uint32_t>(out, id);
    H32_DEBUG("pthread_create #%u -> %s", id, describe_address(start).c_str());
    std::thread([start, arg, id] {
        auto& gt = GuestThread::current();
        GuestArgs a;
        a.u32(arg);
        gt.call(start, a);
        // Run TLS destructors like pthread_exit would.
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
        H32_DEBUG("guest thread #%u finished", id);
    }).detach();
    set_ret32(t, 0);
}

void t_self(GuestThread& t) {
    thread_local uint32_t id = 0;
    if (!id) id = g_next_tid++;
    set_ret32(t, id);
}

}  // namespace

namespace thunks {

void register_libc_pthread() {
    add("pthread_mutex_init", t_mutex_init);
    add("pthread_mutex_destroy", t_mutex_destroy);
    add("pthread_mutex_lock", t_mutex_lock);
    add("pthread_mutex_trylock", t_mutex_trylock);
    add("pthread_mutex_unlock", t_mutex_unlock);
    add("pthread_once", t_once);
    add("pthread_key_create", t_key_create);
    add("pthread_key_delete", t_key_delete);
    add("pthread_getspecific", t_getspecific);
    add("pthread_setspecific", t_setspecific);
    add("pthread_create", t_create);
    add("pthread_self", t_self);
}

}  // namespace thunks
}  // namespace h32
