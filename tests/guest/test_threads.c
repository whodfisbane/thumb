#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdlib.h>
#include "check.h"

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static long counter;
static void* adder(void* arg) {
    for (int i = 0; i < 20000; i++) { pthread_mutex_lock(&mu); counter++; pthread_mutex_unlock(&mu); }
    return (void*)((intptr_t)arg * 2);
}

static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int ready;
static void* signaller(void* arg) {
    (void)arg;
    pthread_mutex_lock(&mu); ready = 1; pthread_cond_signal(&cv); pthread_mutex_unlock(&mu);
    return NULL;
}

static sem_t sem;
static void* poster(void* arg) { (void)arg; for (int i = 0; i < 5; i++) sem_post(&sem); return NULL; }

static int once_count;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void once_fn(void) { once_count++; }

static int cleanups[3], cleanup_order;
static void cleanup_fn(void* arg) { cleanups[(intptr_t)arg] = ++cleanup_order; }
static void* cleaner(void* arg) {
    (void)arg;
    pthread_cleanup_push(cleanup_fn, (void*)0);
    pthread_cleanup_push(cleanup_fn, (void*)1);
    pthread_cleanup_push(cleanup_fn, (void*)2);
    pthread_cleanup_pop(1);  /* runs handler 2 now */
    pthread_exit((void*)5);  /* must run 1, then 0 */
    pthread_cleanup_pop(0);
    pthread_cleanup_pop(0);
    return NULL;
}

/* Two pthread_once inits that wait on each other across threads: init A
   starts a thread that runs init B, and waits for it. One global lock deadlocks here. */
static pthread_once_t once_a = PTHREAD_ONCE_INIT, once_b = PTHREAD_ONCE_INIT;
static int b_done;
static void init_b(void) { b_done = 1; }
static void* run_b(void* arg) { (void)arg; pthread_once(&once_b, init_b); return NULL; }
static void init_a(void) { pthread_t th; pthread_create(&th, NULL, run_b, NULL); pthread_join(th, NULL); }

static pthread_key_t key;
static int dtor_calls;
static void key_dtor(void* v) { (void)v; dtor_calls++; }
static void* keyed(void* arg) { pthread_setspecific(key, arg); return pthread_getspecific(key); }

static void* exiter(void* arg) { (void)arg; pthread_exit((void*)77); return NULL; }

TEST_MAIN({
    pthread_t t[4];
    for (intptr_t i = 0; i < 4; i++) pthread_create(&t[i], NULL, adder, (void*)i);
    intptr_t rets = 0;
    for (int i = 0; i < 4; i++) { void* r; pthread_join(t[i], &r); rets += (intptr_t)r; }
    CHECK(counter == 80000, "mutex-protected counter across 4 threads");
    CHECK(rets == (0 + 1 + 2 + 3) * 2, "pthread_join return values");

    pthread_t s; pthread_create(&s, NULL, signaller, NULL);
    pthread_mutex_lock(&mu); while (!ready) pthread_cond_wait(&cv, &mu); pthread_mutex_unlock(&mu);
    pthread_join(s, NULL);
    CHECK(ready == 1, "condition variable");

    sem_init(&sem, 0, 0);
    pthread_t p; pthread_create(&p, NULL, poster, NULL);
    for (int i = 0; i < 5; i++) sem_wait(&sem);
    pthread_join(p, NULL);
    int val = -1; sem_getvalue(&sem, &val);
    CHECK(val == 0, "semaphore wait/post");

    pthread_once(&once, once_fn); pthread_once(&once, once_fn);
    CHECK(once_count == 1, "pthread_once");
    pthread_once(&once_a, init_a);
    CHECK(b_done == 1, "nested pthread_once on another thread doesn't deadlock");

    pthread_key_create(&key, key_dtor);
    pthread_t k; void* kr; pthread_create(&k, NULL, keyed, (void*)0x1234); pthread_join(k, &kr);
    CHECK(kr == (void*)0x1234 && dtor_calls == 1, "thread-specific data + destructor");

    pthread_t e; void* er; pthread_create(&e, NULL, exiter, NULL); pthread_join(e, &er);
    CHECK(er == (void*)77, "pthread_exit value");

    pthread_t c; void* cr; pthread_create(&c, NULL, cleaner, NULL); pthread_join(c, &cr);
    CHECK(cr == (void*)5 && cleanups[2] == 1 && cleanups[1] == 2 && cleanups[0] == 3,
          "pthread_exit runs cleanup handlers, newest first");
})
