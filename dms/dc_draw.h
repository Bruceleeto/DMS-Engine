#ifndef DC_DRAW_H
#define DC_DRAW_H

#include <stdbool.h>
#include "dc_camera.h"
#include "dc_model.h"

/* ================================================================
 * Draw queue
 *
 * Say what to draw; the engine puts it in the right PVR lists. A call only
 * stores the model and where it goes. dc_frame_end() then walks the queue once
 * per list (opaque, transparent, punch-through) and draws. Nothing is copied
 * but the transform, so it costs the same as drawing by hand.
 *
 *     dc_set_camera(&camera);
 *     dc_draw(world, origin);
 *     dc_draw_ex(crate, &(DCDrawOpts){ .pos = pos, .rot = rot });
 *     dc_debug_stats();
 *     dc_frame_end();
 *
 * Things draw in the order they were queued. The dc_model_draw_list* calls
 * still work for drawing by hand, but do not mix both in one frame.
 * ================================================================ */

/* Camera used by the draw calls that follow. It must stay alive and unchanged
 * until dc_frame_end(). Use one DCCamera per view for split screen. */
void dc_set_camera(const DCCamera* cam);

/* Engine use: the camera the draws are using (what dc_set_camera() was
 * last given) */
const DCCamera* dc_get_camera(void);

/* As it is: no turn, full size */
void dc_draw(DMSModel* model, shz_vec3_t pos);

/* A flat shadow on a level floor, thrown away from a light. Any model, moving
 * or not. It only looks right on flat ground: it does not bend over steps. */
typedef struct {
    shz_vec3_t light;       /* where the light is */
    bool       sun;         /* light is the way it shines instead (far away light) */
    float      floor_y;     /* height of the floor under the model */
    float      dark;        /* 0 to 1, how dark. 0 means 0.5 */
} DCShadow;

/* Everything else. Fields left out are zero, which means "as it is". */
typedef struct {
    shz_vec3_t   pos;
    float        scale;     /* 0 means 1 */
    float        yaw;       /* radians, about y */
    const float* rot;       /* turned any way, used instead of yaw (static models
                             * only): 9 floats, 3 columns, where the model's x, y
                             * and z axes point in the world. It is copied. */
    const DCShadow* shadow; /* also draw its shadow. It is copied. */
    bool         add;       /* added to what is behind it, so black adds nothing:
                             * flashes, glows, fire. Draw it after the solid things. */
} DCDrawOpts;

void dc_draw_ex(DMSModel* model, const DCDrawOpts* opts);

/* ================================================================
 * Light
 * ================================================================ */

/* One light that can move, over everything drawn from now on. NULL turns it
 * off, which is how a model starts: the colours baked into it in Blender go
 * out as they are. With a light set they are multiplied by how much of it each
 * vertex catches, so the baking stays and the light is what moves.
 *
 *     dc_set_light(&(DCLight){ .pos = torch, .range = 300.0f });
 *
 * Static models only; one with a skeleton ignores it.
 *
 * Cel shaded, the light comes in flat steps with sharp edges between them:
 *
 *     dc_set_light(&(DCLight){ .pos = torch, .range = 300.0f, .bands = 3 });
 *
 * Textures stay. Solid meshes get it; cutout and see-through meshes keep the
 * smooth light. It costs a second pass over each solid mesh, in the TR list. */
void dc_set_light(const DCLight* light);

/* ================================================================
 * Bloom
 * ================================================================ */

/* Bright things bleeding into the picture around them: a lamp, a neon sign, a
 * fire, a screen in a dark room. Asked for in Blender -- any material with an
 * Emission colour gives off light -- and switched on with one call:
 *
 *     dc_set_bloom(&(DCBloom){ .strength = 1.0f });
 *
 * NULL turns it off, which is how it starts, and off it costs nothing. On, the
 * engine draws the glowing meshes a second time into a small picture, softens
 * it, and adds it over the frame. It works off the draws already queued, so
 * the models need no handling and nothing is said twice.
 *
 * The small picture is redrawn every other frame and added over both, which is
 * half the cost for a lag you would have to whip the camera round to see.
 *
 * Only the glowing meshes go into that picture, so a lamp behind a wall still
 * bleeds a little through the wall. In a dark scene it does not show.
 *
 * Fields left out are zero, which means "as it is". */
typedef struct {
    float strength;   /* how strong the glow is, 0 to 1. 0 means 1 */
    float spread;     /* how far it bleeds, in texels of the small picture.
                       * 0 means 1; over about 3 the taps start to show */
    int   size;       /* the small picture, a power of two from 8 to 256.
                       * 0 means 128, which costs 96KB of VRAM; 256 costs
                       * 384KB. Without the VRAM for it, it says so and
                       * nothing glows. Bigger is sharper, not smoother */
} DCBloom;

void dc_set_bloom(const DCBloom* bloom);

/* ================================================================
 * Render targets
 *
 * A texture the engine draws into, for a screen inside the level, a mirror, a
 * rear view. Pick it, draw into it like the screen, then go back:
 *
 *     dc_set_target(tv);                  // what follows goes into the texture
 *     dc_set_camera(&security_cam);
 *     dc_draw(world, origin);
 *     dc_set_target(NULL);                // back to the screen
 *     dc_set_camera(&camera);
 *     dc_draw(world, origin);             // the TV model in it shows the texture
 *
 * Targets are drawn first at dc_frame_end(), so the screen shows this frame's
 * picture. Inside a target, a model showing that same target has last frame's
 * picture (a screen showing itself works). The picture is the camera's whole
 * view squeezed to the texture, so a 4:3 screen in the level looks right.
 * No alpha in the picture (the PVR renders RGB565).
 * ================================================================ */

typedef struct DCTarget DCTarget;

/* width and height: powers of two, 8 to 1024. Costs width x height x 4 bytes
 * of VRAM (two pictures: the one being drawn, the one being shown). With no
 * VRAM left it says so and what is drawn into the target is dropped. Load the
 * models first or not: either way works. */
DCTarget* dc_target_create(int width, int height);

/* Free the target before the models showing it */
void dc_target_free(DCTarget* target);

/* Draws that follow go into the target; NULL for the screen. Every frame
 * starts on the screen. */
void dc_set_target(DCTarget* target);

/* Every mesh of the model with that material name (the name it has in
 * Blender) shows the target from now on. Returns how many meshes that was.
 * A mesh without UVs gets the picture stretched flat across it, upright and
 * facing the way its normals point, so a plain rectangle is enough. */
int dc_target_show_on(DCTarget* target, DMSModel* model, const char* material);

/* The target as a flat picture on the screen (a rear view mirror, or to see
 * what a target holds) */
void dc_draw_target(DCTarget* target, float x, float y, float width, float height);

/* The same with more say. Fields left out are zero, which means "as it is".
 * Drawn into the target itself, it is last frame's picture put behind what
 * is drawn this frame. A little see-through, it fades frame after frame and
 * leaves a trail behind what moves (motion blur). Then add it over the screen:
 *
 *     dc_set_target(trail);
 *     dc_draw(ship, pos);
 *     dc_draw_target_ex(trail, &(DCTargetOpts){ .alpha = 0.9f });
 *     dc_set_target(NULL);
 *     dc_draw(ship, pos);
 *     dc_draw_target_ex(trail, &(DCTargetOpts){ .add = true });
 */
typedef struct {
    float x, y, width, height;  /* no size: over everything being drawn into */
    float alpha;                /* 0 means 1 (solid) */
    bool  add;                  /* added to what is behind it, black adds nothing */
} DCTargetOpts;

void dc_draw_target_ex(DCTarget* target, const DCTargetOpts* opts);

/* Your own PVR drawing: fn runs at frame end with the given list open
 * (PVR_LIST_OP_POLY, PVR_LIST_TR_POLY or PVR_LIST_PT_POLY). */
void dc_draw_call(int pvr_list, void (*fn)(void* user), void* user);

/* Engine use: draws everything queued. Called by dc_frame_end(). */
void dc_draw_flush(void);

#endif /* DC_DRAW_H */
