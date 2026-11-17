// harness: --speed 2
/* Timed waits at 2x speed: each waits 200 ms of guest time, so ~200 ms on the
 * guest's own clock (100 ms real). Absolute deadlines are guest times, so a
 * wait that compared them with the real clock would be off. */
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "check.h"
#include "timing.h"

/* Old bionic32 calls (declared under other names to sidestep header availability). */
int cond_timedwait_monotonic_np(pthread_cond_t*, pthread_mutex_t*, const struct timespec*)
    __asm__("pthread_cond_timedwait_monotonic_np");
int cond_timeout_np(pthread_cond_t*, pthread_mutex_t*, unsigned) __asm__("pthread_cond_timeout_np");

static struct timespec g_t0;
static void start(void) {
    /* Let the guest clock run ahead of the real one first, as it does in a game. */
    usleep(300000);
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
}
#define CHECK_200MS(name)                                 \
    do {                                                  \
        long ms_ = ms_since(&g_t0, CLOCK_MONOTONIC);      \
        if (!(ms_ >= 170 && ms_ < 320)) printf("  %s: %ld ms\n", name, ms_); \
        CHECK(ms_ >= 170 && ms_ < 320, name);             \
    } while (0)

static pthread_mutex_t g_held = PTHREAD_MUTEX_INITIALIZER;
static void* timedlock_thread(void* p) {
    struct timespec dl = after_ms(CLOCK_REALTIME, 200);
    *(int*)p = pthread_mutex_timedlock(&g_held, &dl);
    return NULL;
}

TEST_MAIN({
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t c = PTHREAD_COND_INITIALIZER;
    pthread_mutex_lock(&m);

    start();
    struct timespec dl = after_ms(CLOCK_REALTIME, 200);
    CHECK(pthread_cond_timedwait(&c, &m, &dl) == ETIMEDOUT, "cond_timedwait (realtime)");
    CHECK_200MS("cond_timedwait (realtime) waits 200 guest ms");

    pthread_condattr_t ca;
    pthread_cond_t mc;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&mc, &ca);
    start();
    dl = after_ms(CLOCK_MONOTONIC, 200);
    CHECK(pthread_cond_timedwait(&mc, &m, &dl) == ETIMEDOUT, "cond_timedwait (monotonic attr)");
    CHECK_200MS("cond_timedwait (monotonic attr) waits 200 guest ms");

    start();
    dl = after_ms(CLOCK_MONOTONIC, 200);
    CHECK(cond_timedwait_monotonic_np(&c, &m, &dl) == ETIMEDOUT, "cond_timedwait_monotonic_np");
    CHECK_200MS("cond_timedwait_monotonic_np waits 200 guest ms");

    start();
    CHECK(cond_timeout_np(&c, &m, 200) == ETIMEDOUT, "cond_timeout_np");
    CHECK_200MS("cond_timeout_np waits 200 guest ms");
    pthread_mutex_unlock(&m);

    pthread_mutex_lock(&g_held);
    int r = -1;
    pthread_t th;
    start();
    pthread_create(&th, NULL, timedlock_thread, &r);
    pthread_join(th, NULL);
    CHECK(r == ETIMEDOUT, "mutex_timedlock");
    CHECK_200MS("mutex_timedlock waits 200 guest ms");
    pthread_mutex_unlock(&g_held);

    sem_t s;
    sem_init(&s, 0, 0);
    start();
    dl = after_ms(CLOCK_REALTIME, 200);
    CHECK(sem_timedwait(&s, &dl) == -1 && errno == ETIMEDOUT, "sem_timedwait");
    CHECK_200MS("sem_timedwait waits 200 guest ms");

    int word = 0;
    struct timespec rel = ms_ts(200);
    start();
    CHECK(syscall(__NR_futex, &word, FUTEX_WAIT_PRIVATE, 0, &rel, NULL, 0) == -1 && errno == ETIMEDOUT, "futex WAIT");
    CHECK_200MS("futex WAIT (relative) waits 200 guest ms");

    start();
    dl = after_ms(CLOCK_MONOTONIC, 200);
    CHECK(syscall(__NR_futex, &word, FUTEX_WAIT_BITSET_PRIVATE, 0, &dl, NULL, FUTEX_BITSET_MATCH_ANY) == -1 &&
              errno == ETIMEDOUT,
          "futex WAIT_BITSET");
    CHECK_200MS("futex WAIT_BITSET (absolute) waits 200 guest ms");

    start();
    dl = after_ms(CLOCK_MONOTONIC, 200);
    CHECK(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &dl, NULL) == 0, "clock_nanosleep (absolute)");
    CHECK_200MS("clock_nanosleep (absolute) sleeps 200 guest ms");

    start();
    CHECK(clock_nanosleep(CLOCK_MONOTONIC, 0, &rel, NULL) == 0, "clock_nanosleep (relative)");
    CHECK_200MS("clock_nanosleep (relative) sleeps 200 guest ms");

    start();
    nanosleep(&rel, NULL);
    CHECK_200MS("nanosleep sleeps 200 guest ms");

    /* Sanity only: the BOOTTIME jump needed a suspend, which a test can't do. */
    struct timespec boot0, mono0;
    clock_gettime(CLOCK_BOOTTIME, &boot0);
    clock_gettime(CLOCK_MONOTONIC, &mono0);
    usleep(100000);
    long db = ms_since(&boot0, CLOCK_BOOTTIME), dm = ms_since(&mono0, CLOCK_MONOTONIC);
    CHECK(db >= 90 && db < 200 && dm >= 90 && dm < 200, "BOOTTIME and MONOTONIC both run at guest speed");
})
