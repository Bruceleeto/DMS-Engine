/* Host stand-in for KOS <arch/cache.h>.  There is no SH-4 dcache here:
 * prefetch/invalidate are no-ops, "allocate a line with a value" is what it
 * does architecturally (the 32-byte line holds eight copies of the word). */
#ifndef __ARCH_CACHE_H
#define __ARCH_CACHE_H
#include <stdint.h>
#include <stddef.h>
static inline void dcache_pref_line(const void *src) { __builtin_prefetch(src); }
static inline void dcache_alloc_line(void *src) { (void)src; }
static inline void dcache_alloc_line_with_value(void *src, uintptr_t value)
{
    uint32_t *p = (uint32_t *)src;
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (uint32_t)value;
}
static inline void dcache_zero_alloc_line(void *src) { dcache_alloc_line_with_value(src, 0); }
static inline void dcache_inval_line(void *src) { (void)src; }
static inline void dcache_purge_line(void *src) { (void)src; }
static inline void dcache_wback_line(void *src) { (void)src; }
static inline void dcache_inval_range(uintptr_t s, size_t n) { (void)s; (void)n; }
static inline void dcache_wback_range(uintptr_t s, size_t n) { (void)s; (void)n; }
static inline void dcache_purge_range(uintptr_t s, size_t n) { (void)s; (void)n; }
static inline void dcache_flush_range(uintptr_t s, size_t n) { (void)s; (void)n; }
static inline void icache_flush_range(uintptr_t s, size_t n) { (void)s; (void)n; }
#endif
