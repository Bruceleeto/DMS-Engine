#ifndef DC_DRAW2D_H
#define DC_DRAW2D_H

#include <stdint.h>

/* Common colors (ARGB) */
#define DC_COLOR_WHITE    0xFFFFFFFF
#define DC_COLOR_BLACK    0xFF000000
#define DC_COLOR_RED      0xFFE62937
#define DC_COLOR_GREEN    0xFF00E430
#define DC_COLOR_BLUE     0xFF0079F1
#define DC_COLOR_YELLOW   0xFFFDF900
#define DC_COLOR_ORANGE   0xFFFFA100
#define DC_COLOR_GRAY     0xFF828282

/* Initialize font. Call once after dc_init(). */
void dc_draw2d_init(void);

/* Draw text. Call it at any point in the frame: the text is copied (up to 63
 * characters) and drawn at dc_frame_end(), on top of everything. */
void dc_draw_text(const char* text, int x, int y, int size, uint32_t argb);

/* Engine use: draws the queued text. Called by dc_frame_end(). */
void dc_draw2d_flush(void);

/* ---- Images ---- */

typedef struct DCImage DCImage;

/* Load a .dt image (make assets turns assets/<dir>/<name>.png into one).
 * Returns NULL if it cannot be loaded. */
DCImage* dc_image_load(const char* filename);
void dc_image_free(DCImage* img);

/* Fill the screen with an image, behind everything. Call it at any point in
 * the frame; it is drawn at dc_frame_end(). */
void dc_draw_background(const DCImage* img);

/* The image shiny things reflect. Any material with Metallic at 0.5 or more
 * in the .glb reflects it. Solid with Roughness under 0.25: a mirror, the image
 * in place of its texture. Solid and rougher: the image added over it as a
 * shine. See-through: a reflection cut out by its texture's alpha. Without this call metallic
 * materials draw as normal and cost nothing. NULL turns it off. */
void dc_set_environment(const DCImage* img);

#endif /* DC_DRAW2D_H */
