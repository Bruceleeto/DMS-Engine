/* Host stand-in for KOS <arch/arch.h>: PVR_RAM_SIZE asks for the system type. */
#ifndef __ARCH_ARCH_H
#define __ARCH_ARCH_H
#include <stdint.h>
#define HW_TYPE_RETAIL 0x0
#define HW_TYPE_SET5   0x1

/* gpu_pvr's video memory in MB: 8 (the Dreamcast's) or 16 (the dev kit's). The ONE place it is set: kos_pvr.cpp's
   VRAM_SIZE is made from it, and the system type below follows it (as RETAIL, PVR_RAM_SIZE is 8 MB and
   to_pvr_txr_ptr masks every texture address to it, so with 16 MB of memory a texture in the upper 8 MB was read
   from 8 MB lower). 16 for now, while render to texture does not fit in 8. */
#ifndef GPU_PVR_VRAM_MB
#define GPU_PVR_VRAM_MB 16
#endif
#if GPU_PVR_VRAM_MB != 8 && GPU_PVR_VRAM_MB != 16
#error "GPU_PVR_VRAM_MB is 8 or 16"
#endif

static inline int hardware_sys_mode(int *region) {
    if (region) *region = 0;
    return GPU_PVR_VRAM_MB == 16 ? HW_TYPE_SET5 : HW_TYPE_RETAIL;
}
#endif
