/* Time helpers for the guest tests (all times are what the guest sees). */
#pragma once
#include <time.h>

/* Milliseconds on `c` since `start`. */
static long ms_since(const struct timespec* start, clockid_t c) {
    struct timespec now;
    clock_gettime(c, &now);
    return (now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000;
}

/* The time on `c`, `ms` milliseconds from now. */
static struct timespec after_ms(clockid_t c, long ms) {
    struct timespec t;
    clock_gettime(c, &t);
    t.tv_nsec += (ms % 1000) * 1000000;
    t.tv_sec += ms / 1000 + t.tv_nsec / 1000000000;
    t.tv_nsec %= 1000000000;
    return t;
}

static struct timespec ms_ts(long ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000};
    return t;
}
