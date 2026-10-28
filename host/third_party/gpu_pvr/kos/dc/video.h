/* Host stand-in for KOS <dc/video.h>: only what bloom's pvr.c uses. */
#ifndef __DC_VIDEO_H
#define __DC_VIDEO_H
#include <stdbool.h>
#include <sys/cdefs.h>
__BEGIN_DECLS
void vid_set_dithering(bool enable);
__END_DECLS
#endif
