#ifndef DC_DRAW_H
#define DC_DRAW_H

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

/* As it is: no turn, full size */
void dc_draw(DMSModel* model, shz_vec3_t pos);

/* Everything else. Fields left out are zero, which means "as it is". */
typedef struct {
    shz_vec3_t   pos;
    float        scale;     /* 0 means 1 */
    float        yaw;       /* radians, about y */
    const float* rot;       /* turned any way, used instead of yaw (static models
                             * only): 9 floats, 3 columns, where the model's x, y
                             * and z axes point in the world. It is copied. */
} DCDrawOpts;

void dc_draw_ex(DMSModel* model, const DCDrawOpts* opts);

/* Your own PVR drawing: fn runs at frame end with the given list open
 * (PVR_LIST_OP_POLY, PVR_LIST_TR_POLY or PVR_LIST_PT_POLY). */
void dc_draw_call(int pvr_list, void (*fn)(void* user), void* user);

/* Engine use: draws everything queued. Called by dc_frame_end(). */
void dc_draw_flush(void);

#endif /* DC_DRAW_H */
