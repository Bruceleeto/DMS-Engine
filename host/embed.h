/* A game run inside the editor's Game tab. The editor makes a file of this
 * size, maps it, and starts the game with DMS_EMBED=<file>. The game then
 * opens no window. Each frame it copies its picture in and takes the pad
 * from here. The editor writes pad, paused, step and quit; the game writes
 * the rest. */
#ifndef DMS_EMBED_H
#define DMS_EMBED_H

#include <stdint.h>

#define DMS_EMBED_MAGIC 0x444d5345u     /* "DMSE" */

typedef struct DMSEmbed {
    uint32_t magic;
    volatile uint32_t frame;        /* frames shown, counting up */
    volatile uint32_t buttons;      /* CONT_* */
    volatile int32_t  joyx, joyy;   /* -128 to 127 */
    volatile uint32_t ltrig, rtrig; /* 0 to 255 */
    volatile uint32_t paused;       /* the game waits while this is set */
    volatile uint32_t step;         /* paused: frames it may still run */
    volatile uint32_t quit;
    volatile uint32_t heap_used, heap_free;     /* the Dreamcast heap, bytes */
    volatile float    fps;
    uint8_t rgb[640 * 480 * 3];     /* the last frame, RGB24 rows */
} DMSEmbed;

#endif
