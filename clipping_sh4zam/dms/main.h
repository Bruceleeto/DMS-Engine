#ifndef MAIN_H
#define MAIN_H

#include <kos.h>
#include <sh4zam/shz_sh4zam.h>
#include "pvrtex.h"

/* ---- Screen / projection constants ---- */
#define SCR_W   640.0f
#define SCR_H   480.0f
#define NEAR_Z  0.1f
#define FAR_Z   100.0f

/* ---- Player constants ---- */
#define EYE_HEIGHT      1.6f
#define PLAYER_RADIUS   0.3f
#define GRAVITY         0.15f
#define GROUND_SNAP     0.5f
#define MAX_STEP_HEIGHT 0.35f

/* ---- Outcode flags ---- */
#define OC_NEAR   0x01
#define OC_FAR    0x02
#define OC_LEFT   0x04
#define OC_RIGHT  0x08
#define OC_BOTTOM 0x10
#define OC_TOP    0x20

/* ================================================================
 * Animation / Skeleton structures
 * ================================================================ */

/* Transform: 40 bytes on disk — matches converter's WriteDMSTransform
 *   translation (vec3, 12) + rotation (quat WXYZ, 16) + scale (vec3, 12)
 */
typedef struct {
    shz_vec3_t translation;
    shz_quat_t rotation;    /* WXYZ order — SH4ZAM native */
    shz_vec3_t scale;
} DMSTransform;

typedef struct {
    char name[64];
    int  parent;
    DMSTransform bindPose;
    DMSTransform localPose;
    shz_mat4x4_t worldPose          __attribute__((aligned(32)));
    shz_mat4x4_t inverseBindMatrix  __attribute__((aligned(32)));
    shz_mat4x4_t skinMatrix         __attribute__((aligned(32)));
} DMSBone;

typedef struct {
    char  name[32];
    int   boneCount;
    int   frameCount;
    float duration;
    DMSTransform *framePoses;   /* frameCount * boneCount entries */
} DMSAnimation;

typedef struct {
    DMSBone      *bones;
    int           boneCount;
    DMSAnimation *animations;
    int           animCount;
    int           currentAnim;
    float         currentTime;
} DMSSkeleton;

/* ================================================================
 * DMS model structures
 * ================================================================ */
typedef struct __attribute__((aligned(32))) {
    float x, y, z;
    float u, v;
    uint32_t argb;
    int8_t nx, ny, nz;
    uint8_t pad;        /* boneId for animated models */
    uint32_t flags;
} DMSVertex;

typedef struct {
    uint32_t vertex_count;
    int32_t  texture_id;
    uint32_t material_color;
    float    bound_cx, bound_cy, bound_cz;
    float    bound_radius;
    uint32_t material_flags;        /* v5: packed material bits */
    float    alpha_cutoff;          /* v5: for CUTOUT alpha mode */
    DMSVertex *vertices;            /* bind-pose / static verts */
    DMSVertex *animated_vertices;   /* skinned output (NULL if static) */
    pvr_poly_hdr_t header __attribute__((aligned(32)));
} DMSMesh;

typedef struct {
    uint32_t    mesh_count;
    uint32_t    opaque_count;       /* v5: meshes [0..opaque-1] → OP list */
    uint32_t    cutout_count;       /* v5: meshes [opaque..+cutout-1] → PT list */
    uint32_t    transparent_count;  /* v5: remaining → TR list */
    DMSMesh    *meshes;
    dttex_info_t *textures;
    int          texture_count;
    DMSSkeleton *skeleton;
    float        anim_bound_cx, anim_bound_cy, anim_bound_cz;
    float        anim_bound_radius;
    float        max_bind_radius;
} DMSModel;

/* ================================================================
 * Rendering vertex types
 * ================================================================ */
typedef struct __attribute__((aligned(32))) {
    float x, y, z;
    float u, v;
    uint32_t argb;
    uint32_t flags;
} MeshVertex;

typedef struct __attribute__((aligned(32))) {
    float x, y, z, w;
    float u, v;
    uint32_t argb;
    uint32_t flags;
} ClipVertex;

/* ================================================================
 * Camera
 * ================================================================ */
typedef struct {
    shz_vec3_t pos;
    float yaw;
    float pitch;
} Camera;

/* ================================================================
 * Frustum (world-space planes packed for FIPR)
 * ================================================================ */
typedef struct __attribute__((aligned(32))) {
    shz_mat4x4_t side_planes;
    shz_vec4_t near_plane;
    shz_vec4_t far_plane;
} WorldFrustum;

#endif /* MAIN_H */