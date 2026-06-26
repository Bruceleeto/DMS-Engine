#ifndef DC_MODEL_H
#define DC_MODEL_H

#include "main.h"
#include "dc_camera.h"

/* ================================================================
 * Model loading
 * ================================================================ */

/* Load DMS model from file. Returns NULL on failure. */
DMSModel* dc_model_load(const char* filename);

/* Load external textures for v4 models. No-op for v5 (embedded). */
void dc_model_load_textures(DMSModel* model, const char* base_path);

/* Free model and VRAM textures. */
void dc_model_free(DMSModel* model);

/* ================================================================
 * Drawing
 * ================================================================ */

/* Draw all meshes of a model. Requires an active PVR list (dc_list_begin).
 * Handles frustum culling, near-plane clipping, and skinned rendering. */
void dc_model_draw(DMSModel* model, shz_vec3_t pos, float scale,
                   const DCCamera* cam);

/* Draw with Y-axis rotation (radians). Use for character facing direction. */
void dc_model_draw_rotated(DMSModel* model, shz_vec3_t pos, float scale,
                           float yaw, const DCCamera* cam);

/* Draw only meshes belonging to a specific PVR list type (OP, TR, or PT).
 * Use this when rendering multiple models to avoid reopening closed lists. */
void dc_model_draw_list(DMSModel* model, shz_vec3_t pos, float scale,
                        const DCCamera* cam, int target_list);

/* Draw list with Y-axis rotation (radians). */
void dc_model_draw_list_rotated(DMSModel* model, shz_vec3_t pos, float scale,
                                float yaw, const DCCamera* cam, int target_list);

/* ================================================================
 * Animation
 * ================================================================ */

/* Update skeleton pose + bounding volume. Call once per frame.
 * Pass dt=0 to hold the current frame (paused). */
void dc_model_animate(DMSModel* model, float dt);

/* Set current animation by index. Resets time to 0. */
void dc_model_set_anim(DMSModel* model, int anim_index);

/* Get current animation index, or -1 if no skeleton. */
int dc_model_get_anim(DMSModel* model);

/* ================================================================
 * Render statistics
 * ================================================================ */

typedef struct {
    uint32_t meshes_drawn;
    uint32_t meshes_culled;
    uint32_t verts_xformed;
    uint32_t verts_clipped;
} DCModelStats;

/* Reset counters to zero. */
void dc_model_reset_stats(void);

/* Get accumulated stats since last reset. */
const DCModelStats* dc_model_get_stats(void);

#endif /* DC_MODEL_H */
