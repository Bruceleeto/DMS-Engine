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

/* Print every mesh: material name, vertex and triangle counts, texture, alpha
 * mode, flags. Those names are what the calls below are asked for by. */
void dc_model_materials(const DMSModel* model);

/* Draw everything wearing this Blender material see-through, at this alpha
 * (0 invisible, 255 solid). Animated models only; a static model's lists are
 * fixed at load, so set the alpha mode in Blender instead. Returns how many
 * meshes it changed. */
int dc_model_see_through(DMSModel* model, const char* material, uint8_t alpha);

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

/* Translucent, because inside the shape the mesh has to stop covering what is
 * behind it. The opaque list writes every pixel it keeps and ignores the
 * blend, so there a volume can only swap one picture for another. */
#define VOL_POLY_LIST    PVR_LIST_TR_POLY
#define VOL_MOD_LIST     PVR_LIST_TR_MOD

/* Wherever the mesh `shape` covers the mesh `on`, `on` goes see-through so
 * that `shows` can be seen behind it -- an x-ray window that moves with the
 * model. All three named by Blender material, all in the one model:
 *
 *     dc_model_volume(dino, "Scanner", "Dinosaur", "Bones");
 *
 * Said once at load; drawing is dc_draw() as usual. False if a material was
 * not found. */
bool dc_model_volume(DMSModel* model, const char* shape, const char* on,
                     const char* shows);

/* How much of `on` is left inside the shape, 0 (gone, pure x-ray) to 255
 * (solid, no effect), and what colour it is tinted there -- 0xFFFFFF for
 * none. The default is a light blue wash at a quarter strength. */
void dc_model_volume_inside(DMSModel* model, uint8_t alpha, uint32_t rgb);

/* Engine use: give the vertex buffer guard its budget back. The PVR winds the
 * buffer back at the start of every scene, not every frame, so a frame drawing
 * render targets calls this once per target as well as once for the screen. */
void dc_model_frame_begin(void);

/* Engine use (dc_draw flush): the mesh the volume works on, two-parameter,
 * always in the TR list. */
void dc_model_draw_modified(DMSModel* model, shz_vec3_t pos, float scale, float yaw,
                            const DCCamera* cam);

/* Engine use (dc_draw flush): the shape, as triangles, into PVR_LIST_TR_MOD */
void dc_model_draw_volume(DMSModel* model, shz_vec3_t pos, float scale, float yaw,
                          const DCCamera* cam);

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

/* Engine use (dc_set_bloom): while on, model draws leave out every mesh but
 * the ones that give off light (DMS_MAT_GLOW) */
void dc_model_set_glow_only(bool on);

/* ================================================================
 * Light
 * ================================================================ */

/* A light that can move. See dc_set_light() in dc_draw.h. */
typedef struct {
    shz_vec3_t pos;      /* where the light is */
    bool       sun;      /* pos is the way it shines instead (a far away light) */
    float      r, g, b;  /* colour, 0 to 1. All zero means white */
    float      range;    /* how far it reaches. 0 means 500. A sun has none */
    float      ambient;  /* 0 to 1, how lit the side facing away is. 0 means 0.25 */
} DCLight;

/* Engine use (dc_set_light): the light, or NULL for none */
void dc_model_set_light(const DCLight* light);

/* ================================================================
 * Markers
 * ================================================================ */

/* Where a model marks places rather than showing something: the corners of
 * every mesh whose material has that name, in the model's own space, each
 * corner once and in the order the file holds them. Those meshes stop being
 * drawn, since asking for them says they are markers.
 *
 * It is how an effect made in code is put where the model says it goes: a
 * flame over a burner, a muzzle, a place a door swings from. Returns how many
 * there were, which may be more than max. */
int dc_model_points(DMSModel* model, const char* material,
                    shz_vec3_t* out, int max);

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
