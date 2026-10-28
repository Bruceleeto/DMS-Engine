#pragma once
/* KOS's monotonic clocks (TMU2 on the Dreamcast). */
#include <stdint.h>
#include <time.h>
static inline uint64_t timer_ns_gettime64(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
}
static inline uint64_t timer_us_gettime64(void) { return timer_ns_gettime64() / 1000u; }
static inline uint64_t timer_ms_gettime64(void) { return timer_ns_gettime64() / 1000000u; }
