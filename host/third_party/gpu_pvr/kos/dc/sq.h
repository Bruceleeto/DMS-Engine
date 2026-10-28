/* Host stand-in for KOS <dc/sq.h>: store queues on libpvr.  See kos_pvr.cpp. */
#ifndef __DC_SQ_H
#define __DC_SQ_H
#include <stdint.h>
#include <stddef.h>
#include <sys/cdefs.h>
__BEGIN_DECLS
/* dest is a KOS SQ destination: PVR_TA_INPUT (0x10xxxxxx) returns the
 * direct-render staging buffer, PVR_TA_TEX_MEM | offset (0x11xxxxxx) returns
 * the host pointer into VRAM at that offset. */
uint32_t *sq_lock(void *dest);
void sq_unlock(void);
void sq_wait(void);
/* 32 bytes at src: a staging-buffer line goes to the TA, VRAM lines are
 * already in place. */
void sq_flush(void *src);
void *sq_cpy(void *dest, const void *src, size_t n);
void *sq_set32(void *dest, uint32_t c, size_t n);
__END_DECLS
#endif
