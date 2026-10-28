/*
 * PC host side of the KOS PVR shim: the window the PVR "TV out" goes to.
 * Not used on the Dreamcast.
 */
#ifndef PVR_HOST_H
#define PVR_HOST_H

#include <stdint.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

struct SDL_Window;

/* Open the window frames are shown in (640x480 times scale). Call before or
 * after pvr_init(); frames rendered while no window is open are dropped. */
int  pvr_host_open( const char *title, int scale );
/* Show frames in a window the program already has (it keeps owning it). */
int  pvr_host_attach( struct SDL_Window *window );
void pvr_host_close( void );
struct SDL_Window *pvr_host_window( void );

/* Copy the last finished frame out as 640x480 RGB24 rows of `stride` bytes.
 * Returns 0 if nothing has been rendered yet. */
int  pvr_host_read_front( uint8_t *rgb, int stride );

/* The next pvr_scene_begin_rtt() writes ARGB4444 instead of RGB565. (On the
 * Dreamcast: PVR_SET(PVR_FB_CFG_2, 2 | dither) after pvr_scene_begin_rtt().) */
void kos_pvr_rtt_argb4444( void );

/* --- VRAM map (host only) ---------------------------------------------
 * Name what the next pvr_mem_malloc() is for: a file path or a word. It
 * applies to that one allocation, then clears. _tex adds the texture's size
 * and format (tag NULL keeps a name pvr_host_mem_tag() already gave). */
void pvr_host_mem_tag( const char *tag );
void pvr_host_mem_tag_tex( const char *tag, int w, int h, const char *fmt );

enum {
	PVR_VRAM_FREE,       /* texture heap, unallocated */
	PVR_VRAM_OPB,        /* object pointer blocks (object lists) + overflow */
	PVR_VRAM_REGION,     /* the tile region array */
	PVR_VRAM_VERTEX,     /* TA vertex/parameter buffer */
	PVR_VRAM_FRAME,      /* the two frame buffers (32-bit path, bank interleaved) */
	PVR_VRAM_UNUSED,     /* padding between fixed regions */
	PVR_VRAM_TEXTURE,    /* a pvr_mem_malloc() block */
	PVR_VRAM_KINDS
};

typedef struct pvr_host_vram_block {
	uint32_t offset, size;      /* 64-bit-path byte address and length */
	int kind;                   /* PVR_VRAM_* */
	int w, h;                   /* textures tagged with a size, else 0 */
	char tag[64];               /* what it is ("" if nobody said) */
	char fmt[24];               /* e.g. "RGB565 VQ tw mip" */
} pvr_host_vram_block;

/* The whole of VRAM in address order: the fixed regions, then the texture
 * heap's allocations and free gaps. Fills up to max blocks of out (out may
 * be NULL) and returns how many there are; *total gets the VRAM size, *used
 * the bytes not free (either may be NULL). The layout is the host's,
 * modelled on the Dreamcast's. */
int  pvr_host_vram_map( pvr_host_vram_block *out, int max, uint32_t *total, uint32_t *used );

__END_DECLS

#endif
