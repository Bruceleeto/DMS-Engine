/* Engine use: name what the next pvr_mem_malloc() is for, so the editor's
 * VRAM map can say (the PC build only; on the Dreamcast these are nothing). */
#ifndef DC_VRAM_TAG_H
#define DC_VRAM_TAG_H

#ifdef DMS_HOST
#include "pvr_host.h"
#define DC_VRAM_TAG(tag)                 pvr_host_mem_tag(tag)
#define DC_VRAM_TAG_TEX(tag, w, h, fmt)  pvr_host_mem_tag_tex(tag, w, h, fmt)
#else
#define DC_VRAM_TAG(tag)                 ((void)0)
#define DC_VRAM_TAG_TEX(tag, w, h, fmt)  ((void)0)
#endif

#endif
