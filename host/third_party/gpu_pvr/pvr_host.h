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

__END_DECLS

#endif
