#ifndef MAIN_H
#define MAIN_H

#include <kos.h>
#include <sh4zam/shz_sh4zam.h>
#include "pvrtex.h"

/* ---- Screen / projection constants ---- */
#define SCR_W   640.0f
#define SCR_H   480.0f
#define NEAR_Z  0.1f
#define FAR_Z   1000.0f   /* cull distance only (no far clip); lower it to trade view distance for speed */

/* ---- Player constants ---- */
#define EYE_HEIGHT      1.6f
#define PLAYER_RADIUS   0.3f
#define GRAVITY         0.15f
#define GROUND_SNAP     0.5f
#define MAX_STEP_HEIGHT 0.35f

/* ---- Outcode flag: vertex is behind the near plane ---- */
#define OC_NEAR   0x01

/* ---- .dms file magic ---- */
#define DMS_MAGIC   0x54534D44u   /* "DMST" */

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

/* material_flags bit 12: mesh is collided with but never drawn */
#define DMS_MAT_COLLISION_ONLY (1u << 12)
/* material_flags bit 13: glTF metallic of 0.5 or more. Reflects the environment
 * image once dc_set_environment() has been called */
#define DMS_MAT_METALLIC       (1u << 13)
/* material_flags bit 14: a mirror (solid, metallic, roughness near 0). Shows
 * the environment image in place of its own texture */
#define DMS_MAT_MIRROR         (1u << 14)

typedef struct {
    uint32_t vertex_count;
    int32_t  texture_id;
    uint32_t material_color;
    float    bound_cx, bound_cy, bound_cz;
    float    bound_radius;
    uint32_t material_flags;        /* packed material bits */
    float    alpha_cutoff;          /* for CUTOUT alpha mode */
    uint32_t tri_count;             /* triangles across all strips (computed at load) */
    uint32_t block;                 /* block this mesh belongs to */
    DMSVertex *vertices;            /* bind-pose / static verts */
    DMSVertex *animated_vertices;   /* skinned output (NULL if static) */
    pvr_poly_hdr_t header __attribute__((aligned(32)));
} DMSMesh;

/* static levels are cut into blocks by location; one sphere culls
 * every mesh in the block */
typedef struct {
    float cx, cy, cz, radius;
} DMSBlock;

/* Meshes of one block inside one list sit together in the file; a run is
 * that range, so a culled block is skipped without touching its meshes */
typedef struct {
    uint32_t block, first, count;
} DMSBlockRun;

typedef struct {
    uint32_t    mesh_count;
    uint32_t    opaque_count;       /* meshes [0..opaque-1] → OP list */
    uint32_t    cutout_count;       /* meshes [opaque..+cutout-1] → PT list */
    uint32_t    transparent_count;  /* remaining → TR list */
    DMSMesh    *meshes;
    uint32_t    block_count;        /* 0 for animated models */
    DMSBlock   *blocks;
    DMSBlockRun *runs;              /* built at load, static models only */
    uint32_t    list_runs[4];       /* runs of alpha mode l: [list_runs[l], list_runs[l+1]) */
    dttex_info_t *textures;
    int          texture_count;
    DMSSkeleton *skeleton;
    float        anim_bound_cx, anim_bound_cy, anim_bound_cz;
    float        anim_bound_radius;
    float        max_bind_radius;
    uint32_t     metallic_count;    /* meshes that reflect the environment */
    uint32_t     mirror_count;      /* of those, mirrors */
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