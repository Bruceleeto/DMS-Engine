/* BotBoy on DMS. Shared bits every scene uses. */
#ifndef BOTBOY_GAME_H
#define BOTBOY_GAME_H

#include <kos.h>
#include <stdbool.h>
#include "dms/dc_engine.h"
#include "dms/dc_input.h"
#include "dms/dc_draw.h"
#include "dms/dc_draw2d.h"

/* Where assets are read from: /pc/ over dcload, /cd/ when built with make disc */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

/* The game was laid out for 320x240. Everything 2D is placed in that space
 * and drawn twice the size on the 640x480 screen. */
#define VIEW_W 320
#define VIEW_H 240
#define VIEW_SCALE 2.0f

/* A sprite in 320x240 space: top left at x, y (or the middle, with center),
 * scaled by sx, sy, faded by alpha */
static inline void ui_image(const DCImage* img, float x, float y, float sx, float sy,
                            float alpha, bool center) {
    if (!img) return;
    dc_draw_image_ex(img, &(DCImageOpts){
        .x = x * VIEW_SCALE, .y = y * VIEW_SCALE,
        .width  = dc_image_width(img)  * sx * VIEW_SCALE,
        .height = dc_image_height(img) * sy * VIEW_SCALE,
        .alpha = alpha, .center = center });
}

/* Text in 320x240 space */
static inline void ui_text(const char* text, float x, float y, int size, uint32_t argb) {
    dc_draw_text(text, (int)(x * VIEW_SCALE), (int)(y * VIEW_SCALE), (int)(size * VIEW_SCALE), argb);
}

/* How wide it draws, in 320x240 space */
static inline float ui_text_width(const char* text, int size) {
    return dc_text_width(text, (int)(size * VIEW_SCALE)) / VIEW_SCALE;
}

/* A filled rectangle in 320x240 space */
static inline void ui_rect(float x, float y, float w, float h, uint32_t argb) {
    dc_draw_rect(x * VIEW_SCALE, y * VIEW_SCALE, w * VIEW_SCALE, h * VIEW_SCALE, argb);
}

/* The whole screen, black, at this alpha: fades */
static inline void ui_fade(float alpha) {
    if (alpha <= 0.0f) return;
    if (alpha > 1.0f) alpha = 1.0f;
    dc_draw_rect(0, 0, VIEW_W * VIEW_SCALE, VIEW_H * VIEW_SCALE, (uint32_t)(alpha * 255.0f) << 24);
}

/* Models were made for the N64 at 64 units to the metre; the level data is
 * in those units, so every model draws at 64 times its own scale */
#define WORLD_SCALE 64.0f

/* Two player co-op: set by the menu before the game scene starts. Nothing is
 * saved in it. */
extern bool game_coop;

/* Any of these skips a splash or confirms a menu */
#define BTN_CONFIRM (CONT_A | CONT_B | CONT_START)

#endif
