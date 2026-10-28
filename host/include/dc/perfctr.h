#pragma once
#include <stdint.h>
#include <time.h>
#define PRFC1 1
#define PMCR_PIPELINE_FREEZE_BY_DCACHE_MISS_MODE 0
#define PMCR_COUNT_CPU_CYCLES 0
static inline uint64_t perf_cntr_timer_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
/* SH4 cache-stall counters have no equivalent in this preview. */
static inline void perf_cntr_start(int counter, int mode, int count) {
    (void)counter; (void)mode; (void)count;
}
static inline uint64_t perf_cntr_count(int counter) { (void)counter; return 0; }
