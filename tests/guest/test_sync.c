/* Mutexes and condition variables: types and static initializers, the guest
 * word being the lock itself, contention, and (timed) waits. */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "check.h"
#include "timing.h"

/* Runs fn(m) on a new thread and returns its result. */
typedef int (*mutex_op)(pthread_mutex_t*);
struct other { mutex_op fn; pthread_mutex_t* m; int r; };
static void* other_main(void* p) {
    struct other* o = p;
    o->r = o->fn(o->m);
    return NULL;
}
static int on_other_thread(mutex_op fn, pthread_mutex_t* m) {
    struct other o = {fn, m, -1};
    pthread_t th;
    pthread_create(&th, NULL, other_main, &o);
    pthread_join(th, NULL);
    return o.r;
}
static int trylock_and_release(pthread_mutex_t* m) {
    int r = pthread_mutex_trylock(m);
    if (r == 0) pthread_mutex_unlock(m);
    return r;
}
static int unlock(pthread_mutex_t* m) { return pthread_mutex_unlock(m); }
static int timedlock_100ms(pthread_mutex_t* m) {
    struct timespec dl = after_ms(CLOCK_REALTIME, 100);
    int r = pthread_mutex_timedlock(m, &dl);
    if (r == 0) pthread_mutex_unlock(m);
    return r;
}

static void test_types(void) {
    pthread_mutex_t n = PTHREAD_MUTEX_INITIALIZER;
    CHECK(pthread_mutex_lock(&n) == 0, "normal: lock");
    CHECK(pthread_mutex_trylock(&n) == EBUSY, "normal: trylock by the owner is EBUSY");
    CHECK(on_other_thread(trylock_and_release, &n) == EBUSY, "normal: trylock by another thread is EBUSY");
    CHECK(pthread_mutex_unlock(&n) == 0, "normal: unlock");
    CHECK(on_other_thread(trylock_and_release, &n) == 0, "normal: free after unlock");

    pthread_mutex_t r = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
    CHECK(pthread_mutex_lock(&r) == 0 && pthread_mutex_lock(&r) == 0 && pthread_mutex_trylock(&r) == 0,
          "recursive initializer: three levels");
    CHECK(on_other_thread(trylock_and_release, &r) == EBUSY, "recursive: held by its owner");
    CHECK(pthread_mutex_unlock(&r) == 0 && pthread_mutex_unlock(&r) == 0, "recursive: unlock two levels");
    CHECK(on_other_thread(trylock_and_release, &r) == EBUSY, "recursive: still held at the last level");
    CHECK(pthread_mutex_unlock(&r) == 0, "recursive: last unlock");
    CHECK(on_other_thread(trylock_and_release, &r) == 0, "recursive: free after the last unlock");
    CHECK(pthread_mutex_unlock(&r) == EPERM, "recursive: unlocking it unheld is EPERM");

    pthread_mutexattr_t a;
    pthread_mutex_t e;
    pthread_mutexattr_init(&a);
    CHECK(pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK) == 0, "mutexattr_settype");
    CHECK(pthread_mutex_init(&e, &a) == 0, "mutex_init with attributes");
    pthread_mutexattr_destroy(&a);
    CHECK(pthread_mutex_lock(&e) == 0, "errorcheck: lock");
    CHECK(pthread_mutex_lock(&e) == EDEADLK, "errorcheck: relock is EDEADLK");
    CHECK(on_other_thread(unlock, &e) == EPERM, "errorcheck: unlock by another thread is EPERM");
    CHECK(pthread_mutex_destroy(&e) == EBUSY, "destroy while locked is EBUSY");
    CHECK(pthread_mutex_unlock(&e) == 0, "errorcheck: unlock");
    CHECK(pthread_mutex_unlock(&e) == EPERM, "errorcheck: second unlock is EPERM");
    CHECK(pthread_mutex_destroy(&e) == 0, "destroy");
}

/* The guest word is the lock: memory reused after a mutex was freed while
 * locked (no pthread_mutex_destroy) is just a fresh, unlocked mutex. */
static void test_reused_memory(void) {
    pthread_mutex_t* m = calloc(1, sizeof *m);
    CHECK(pthread_mutex_lock(m) == 0, "zeroed memory works as a mutex");
    memset(m, 0, sizeof *m);
    CHECK(on_other_thread(trylock_and_release, m) == 0, "reused memory is not still locked");
    free(m);
}

static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static long g_counter;
static void* adder(void* p) {
    (void)p;
    for (int i = 0; i < 20000; i++) {
        pthread_mutex_lock(&g_m);
        g_counter++;
        pthread_mutex_unlock(&g_m);
    }
    return NULL;
}
static void test_contention(void) {
    pthread_t th[4];
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, adder, NULL);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    CHECK(g_counter == 80000, "4 threads x 20000 locked increments");
}

static pthread_cond_t g_c = PTHREAD_COND_INITIALIZER;
static int g_ready, g_woken;
static void* waiter(void* p) {
    (void)p;
    pthread_mutex_lock(&g_m);
    while (!g_ready) pthread_cond_wait(&g_c, &g_m);
    g_woken++;
    pthread_mutex_unlock(&g_m);
    return NULL;
}
static void test_broadcast(void) {
    pthread_t th[4];
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, waiter, NULL);
    usleep(20000);
    pthread_mutex_lock(&g_m);
    g_ready = 1;
    pthread_cond_broadcast(&g_c);
    pthread_mutex_unlock(&g_m);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    CHECK(g_woken == 4, "broadcast wakes every waiter");
}

/* Waiting with a recursive mutex held twice must release it completely, or
 * the thread that would signal can never take it. */
static pthread_mutex_t g_r = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static pthread_cond_t g_rc = PTHREAD_COND_INITIALIZER;
static int g_flag;
static void* setter(void* p) {
    (void)p;
    pthread_mutex_lock(&g_r);
    g_flag = 1;
    pthread_cond_signal(&g_rc);
    pthread_mutex_unlock(&g_r);
    return NULL;
}
static void test_cond_recursive(void) {
    pthread_mutex_lock(&g_r);
    pthread_mutex_lock(&g_r);
    pthread_t th;
    pthread_create(&th, NULL, setter, NULL);
    struct timespec dl = after_ms(CLOCK_REALTIME, 2000);
    while (!g_flag && pthread_cond_timedwait(&g_rc, &g_r, &dl) == 0) {}
    CHECK(g_flag, "cond_wait releases a recursive mutex held twice");
    CHECK(pthread_mutex_unlock(&g_r) == 0 && pthread_mutex_unlock(&g_r) == 0, "both levels are back after the wait");
    CHECK(pthread_mutex_unlock(&g_r) == EPERM, "and no more");
    pthread_join(th, NULL);
}

static void test_timed(void) {
    pthread_cond_t c = PTHREAD_COND_INITIALIZER;
    pthread_mutex_lock(&g_m);
    struct timespec t0, dl = after_ms(CLOCK_REALTIME, 100);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK(pthread_cond_timedwait(&c, &g_m, &dl) == ETIMEDOUT, "cond_timedwait times out");
    long ms = ms_since(&t0, CLOCK_MONOTONIC);
    CHECK(ms >= 90 && ms < 1000, "cond_timedwait waits ~100 ms");

    pthread_condattr_t ca;
    pthread_cond_t mc;
    clockid_t clock = -1;
    pthread_condattr_init(&ca);
    CHECK(pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) == 0, "condattr_setclock");
    CHECK(pthread_condattr_getclock(&ca, &clock) == 0 && clock == CLOCK_MONOTONIC, "condattr_getclock");
    pthread_cond_init(&mc, &ca);
    pthread_condattr_destroy(&ca);
    dl = after_ms(CLOCK_MONOTONIC, 100);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK(pthread_cond_timedwait(&mc, &g_m, &dl) == ETIMEDOUT, "monotonic cond times out");
    ms = ms_since(&t0, CLOCK_MONOTONIC);
    CHECK(ms >= 90 && ms < 1000, "monotonic cond waits ~100 ms (deadline read on CLOCK_MONOTONIC)");
    pthread_mutex_unlock(&g_m);

    pthread_mutex_t h = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&h);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK(on_other_thread(timedlock_100ms, &h) == ETIMEDOUT, "mutex_timedlock times out");
    ms = ms_since(&t0, CLOCK_MONOTONIC);
    CHECK(ms >= 90 && ms < 1000, "mutex_timedlock waits ~100 ms");
    pthread_mutex_unlock(&h);
    CHECK(on_other_thread(timedlock_100ms, &h) == 0, "mutex_timedlock takes a free mutex");
}

TEST_MAIN({
    test_types();
    test_reused_memory();
    test_contention();
    test_broadcast();
    test_cond_recursive();
    test_timed();
})
