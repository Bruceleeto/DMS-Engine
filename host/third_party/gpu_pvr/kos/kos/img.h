/* Host stand-in for KOS <kos/img.h>: pvr_txr.h only needs the type name. */
#ifndef __KOS_IMG_H
#define __KOS_IMG_H
#include <stdint.h>
typedef struct kos_img {
    void *data;
    uint32_t w, h, fmt, byte_count;
} kos_img_t;
#endif
