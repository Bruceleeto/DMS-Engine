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

/* Draw text on the PT list. Auto-opens PT if not already open.
 * Call after all opaque (OP) rendering is complete. */
void dc_draw_text(const char* text, int x, int y, int size, uint32_t argb);

#endif /* DC_DRAW2D_H */
