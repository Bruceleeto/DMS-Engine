#ifndef __KOS_THREAD_H
#define __KOS_THREAD_H
#include <stdint.h>

/* Stub.  platform.c uses these only for the idle/CPU percentages in its
 * stats line; there is no KOS scheduler here, so idle time is reported as
 * zero and the percentages come out as "all busy". */
typedef void kthread_t;

static inline kthread_t *thd_get_idle(void)
{
	return (kthread_t *)0;
}

static inline uint64_t thd_get_cpu_time(kthread_t *thd)
{
	(void)thd;
	return 0;
}
#endif
