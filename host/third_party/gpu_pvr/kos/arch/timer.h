#ifndef __ARCH_TIMER_H
#define __ARCH_TIMER_H
#include <stdint.h>
#include <time.h>

/* KOS's millisecond monotonic clock.  On the DC this reads TMU2; here it is
 * CLOCK_MONOTONIC, which is the same thing for platform.c's purposes (frame
 * pacing and the stats line). */
static inline uint64_t timer_ms_gettime64(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif
