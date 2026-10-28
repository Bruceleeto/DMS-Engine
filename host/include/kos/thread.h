#pragma once
/* KOS threads on pthreads. */
#include <stdint.h>
#include <unistd.h>
typedef struct kthread kthread_t;
#define PRIO_DEFAULT 10
kthread_t* thd_create(int detach, void* (*routine)(void*), void* param);
int  thd_join(kthread_t* thd, void** value_out);
static inline int thd_set_prio(kthread_t* thd, int prio) { (void)thd; (void)prio; return 0; }
static inline void thd_sleep(unsigned ms) { usleep(ms * 1000u); }
static inline void thd_pass(void) { usleep(0); }
/* No KOS scheduler here: no idle thread to measure. */
static inline kthread_t* thd_get_idle(void) { return 0; }
static inline uint64_t thd_get_cpu_time(kthread_t* thd) { (void)thd; return 0; }
