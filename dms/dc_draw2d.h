#ifndef DC_DRAW2D_H
#define DC_DRAW2D_H

#include <stdint.h>
#include <stdbool.h>
#include <sh4zam/shz_sh4zam.h>

typedef struct DMSModel DMSModel;   /* main.h; for dc_model_texture() */

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

/* Draw text. Call it at any point in the frame. Text and images are drawn at
 * dc_frame_end() over everything 3D, in the order they were asked for, so
 * text asked for after an image lands on top of it. */
void dc_draw_text(const char* text, int x, int y, int size, uint32_t argb);

/* How wide that text would draw, in pixels, for centring it */
int dc_text_width(const char* text, int size);

/* A filled rectangle on the same layer, in the same order: a box behind
 * text, a bar under a menu choice, a fade to black over the whole screen
 * (alpha in the colour). */
void dc_draw_rect(float x, float y, float w, float h, uint32_t argb);

/* Engine use: draws the queued text and images. Called by dc_draw_flush(). */
void dc_draw2d_flush(void);

/* ---- Images ---- */

typedef struct DCImage DCImage;

/* Load a .dt image (make assets turns assets/<dir>/<name>.png into one).
 * Returns NULL if it cannot be loaded. */
DCImage* dc_image_load(const char* filename);
void dc_image_free(DCImage* img);

/* The picture's size in pixels */
int dc_image_width(const DCImage* img);
int dc_image_height(const DCImage* img);

/* The image on the screen at its own size, top left corner at x, y. Same
 * ordering as text: it is drawn where it was asked for. Alpha in the image
 * cuts it out (or blends it, if the png had soft alpha). */
void dc_draw_image(const DCImage* img, float x, float y);

/* The same with more say. Fields left out are zero, which means "as it is". */
typedef struct {
    float    x, y;                /* top left corner, or the middle with .center */
    float    width, height;       /* on screen; 0 is the image's own size */
    float    src_x, src_y;        /* part of the image to show, in pixels */
    float    src_width, src_height; /* 0 is all of it (a frame of an animation, a piece of a sheet) */
    float    alpha;               /* 0 means 1 (solid); under 1 fades it */
    uint32_t tint;                /* 0xRRGGBB multiplied in; 0 means white (as it is) */
    bool     add;                 /* added to what is behind it, black adds nothing */
    bool     center;              /* x, y is the middle of it */
} DCImageOpts;

void dc_draw_image_ex(const DCImage* img, const DCImageOpts* opts);

/* Fill the screen with an image, behind everything. Call it at any point in
 * the frame; it is drawn at dc_frame_end(). */
void dc_draw_background(const DCImage* img);

/* The image shiny things reflect. Any material with Metallic at 0.5 or more
 * in the .glb reflects it. Solid with Roughness under 0.25: a mirror, the image
 * in place of its texture. Solid and rougher: the image added over it as a
 * shine. See-through: a reflection cut out by its texture's alpha. Without this call metallic
 * materials draw as normal and cost nothing. NULL turns it off.
 * Each draw keeps the image current when it was queued (as with the camera),
 * so gold things and silver things can share a scene: set it, draw them, set
 * the next. Changing it between draws costs a few headers rebuilt at the
 * flush, nothing when it stays. */
void dc_set_environment(const DCImage* img);
const DCImage* dc_get_environment(void);
/* Engine use: the texture under an image, a dttex_info_t */
const void* dc_image_tex(const DCImage* img);

/* Every mesh of the model wearing this Blender material draws with the image
 * in place of its own texture from now on; NULL gives it its own back.
 * material NULL means the whole model. A damage flash, a power-up glow, a
 * television changing picture. The image must be kept loaded while it is
 * worn. It rebuilds the PVR header of each mesh it changes, as loading does:
 * cheap, but not for nothing, so swap when it changes, not every frame.
 * Returns how many meshes it changed. */
int dc_model_texture(DMSModel* model, const char* material, const DCImage* img);

/* ---- Decals: a picture lying on the ground ---- */

/* A flat square laid on the floor in the world: a blob shadow under a jumping
 * character, a scorch mark, an oil stain, a target ring, a footprint. It is
 * tilted to the floor's normal (what col_ground() returns), so it lies on a
 * slope too. Give it a little lift off the floor (a unit) so the floor does
 * not show through it. Fields left out are zero, which means "as it is". */
typedef struct {
    shz_vec3_t pos;         /* the middle, on the floor */
    shz_vec3_t normal;      /* the floor's; all zero means straight up */
    float      size;        /* across, in world units. 0 means 1 */
    float      yaw;         /* radians, turned about the normal */
    float      alpha;       /* 0 means 1 (solid); under 1 fades it */
    uint32_t   tint;        /* 0xRRGGBB multiplied in; 0 means white (as it is) */
    bool       add;         /* added to what is behind it, black adds nothing */
} DCDecalOpts;

/* img NULL is a soft round dark spot the engine makes itself (a blob shadow
 * with no asset; .tint does nothing to it). Mixed into the scene at frame
 * end, depth tested against it, so a wall hides it and it never hides
 * anything. Four vertices and one PVR header each; up to 64 a frame, the
 * rest are dropped. Draw it every frame it should be seen. */
void dc_draw_decal(const DCImage* img, const DCDecalOpts* opts);

#endif /* DC_DRAW2D_H */
