#ifndef DC_MODEL_H
#define DC_MODEL_H

#include "main.h"
#include "dc_camera.h"

/* ================================================================
 * Model loading
 * ================================================================ */

/* Load DMS model from file. Returns NULL on failure. */
DMSModel* dc_model_load(const char* filename);

/* Free model and VRAM textures. */
void dc_model_free(DMSModel* model);

/* ================================================================
 * Drawing
 * ================================================================ */

/* Draw all meshes of a model (opens each PVR list it needs).
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

/* Draw list turned any way (static models only). rot is 3 columns: where the
 * model's x, y and z axes point in the world. */
void dc_model_draw_list_oriented(DMSModel* model, shz_vec3_t pos, float scale,
                                 const float rot[9], const DCCamera* cam, int target_list);

/* Engine use (dc_draw_ex .shadow): the model's flat shadow on the floor at
 * floor_y. light is where the light is, or with sun the way it shines. */
void dc_model_draw_shadow(DMSModel* model, shz_vec3_t pos, float scale, float yaw,
                          const float* rot, const DCCamera* cam,
                          shz_vec3_t light, bool sun, float floor_y, float dark);

/* Engine use (dc_particles): squares of 4 world-space vertices, submitted
 * with the matrix now in xmtrx. A square that reaches the near plane is
 * dropped whole, not clipped. Stops if the vertex buffer is full. */
void dc_model_submit_quads(const DMSVertex* verts, int quads, pvr_dr_state_t* dr);

/* Engine use (dc_draw_ex .add): model draws that follow are additive, all in
 * the transparent list, until set back to false */
void dc_model_set_add(bool add);

/* Engine use: build a mesh's PVR header from its material flags and a texture
 * (txr NULL for none) */
void dc_model_compile_header(const DMSMesh* mesh, pvr_poly_hdr_t* out, int pvrformat,
                             int width, int height, pvr_ptr_t txr);

/* ================================================================
 * Reflections
 * ================================================================ */

/* Engine use (dc_set_environment): the image metallic meshes reflect, or NULL */
void dc_model_set_environment(const dttex_info_t* tex);

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
    uint32_t tris_drawn;
    uint32_t meshes_vtxfull;   /* skipped: PVR vertex buffer nearly full */
} DCModelStats;

/* Reset counters to zero. */
void dc_model_reset_stats(void);

/* Get accumulated stats since last reset. */
const DCModelStats* dc_model_get_stats(void);

#endif /* DC_MODEL_H */
