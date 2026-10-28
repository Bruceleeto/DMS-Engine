/* Host stand-in for KOS <kos/cdefs.h>: just the attribute macros the PVR
 * headers and bloom's pvr.c use. */
#ifndef __KOS_CDEFS_H
#define __KOS_CDEFS_H
#include <sys/cdefs.h>
#include <stddef.h>
#include <stdint.h>
#ifndef __noinline
#define __noinline __attribute__((__noinline__))
#endif
#ifndef __always_inline
#define __always_inline inline __attribute__((__always_inline__))
#endif
#ifndef __depr
#define __depr(m) __attribute__((deprecated(m)))
#endif
#ifndef __array_size
#define __array_size(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef __align_up
#define __align_up(v, a) (((v) + ((a) - 1)) & ~((a) - 1))
#endif
#ifndef __align_down
#define __align_down(v, a) ((v) & ~((a) - 1))
#endif
#ifndef __is_aligned
#define __is_aligned(v, a) (((uintptr_t)(v) & ((a) - 1)) == 0)
#endif
#ifndef __predict_true
#define __predict_true(x) __builtin_expect(!!(x), 1)
#define __predict_false(x) __builtin_expect(!!(x), 0)
#endif
#ifndef __used
#define __used __attribute__((used))
#endif
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#ifndef __pure
#define __pure __attribute__((pure))
#endif
#ifndef __weak
#define __weak __attribute__((weak))
#endif
#ifndef __hot
#define __hot __attribute__((hot))
#endif
#ifndef __cold
#define __cold __attribute__((cold))
#endif
#ifndef __deprecated
#define __deprecated __attribute__((deprecated))
#endif
#ifndef __printflike
#define __printflike(a, b) __attribute__((format(printf, a, b)))
#endif
#ifndef __is_defined
#define __is_defined(x) 0
#endif
#ifndef __likely
#define __likely(x) __predict_true(x)
#define __unlikely(x) __predict_false(x)
#endif
#endif
