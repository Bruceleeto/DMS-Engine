#include "dc_model.h"
#include "dc_engine.h"
#include "pvrtex.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================
 * Module state
 * ================================================================ */

static ClipVertex* g_clip_buffer = NULL;
static uint32_t    g_clip_buffer_size = 0;

static pvr_dr_state_t* g_dr;   /* set by render_clipped for submit_vert */

static DCModelStats g_stats;

/* ================================================================
 * Vertex buffer guard
 *
 * If the PVR vertex buffer overflows, the TA writes over other VRAM and
 * the GPU hangs, so a mesh that might not fit is skipped. Drawing resumes
 * by itself as soon as there is room again (the next frame, or a smaller
 * mesh).
 *
 * Reading the TA's write position is slow (~0.5us a register, it was half
 * the cost of drawing a small mesh), so it is read once per model draw call
 * and the bytes sent after that are counted here instead. The count can only
 * run ahead of the TA, never behind it.
 * ================================================================ */

#define VTXBUF_MARGIN (32 * 1024)   /* TA lag, background poly, HUD after models */

static void vtxbuf_warn(void) {
    static int warned = 0;
    if (!warned) {
        warned = 1;
        printf("DMS: PVR vertex buffer full (%luKB), skipping meshes so it doesn't hang. "
               "Raise DCInitParams.vram_size or draw less.\n",
               (unsigned long)((PVR_GET(PVR_TA_VERTBUF_END) - PVR_GET(PVR_TA_VERTBUF_START)) / 1024));
    }
}

/* Bytes left in the vertex buffer after the safety margin */
static int32_t g_vtx_left;

/* Re-read the real position (the HUD, other code and the last frame all
 * moved it); both registers, KOS swaps buffers every frame */
static inline void vtxbuf_sync(void) {
    g_vtx_left = (int32_t)(PVR_GET(PVR_TA_VERTBUF_END) - PVR_GET(PVR_TA_VERTBUF_POS)) - VTXBUF_MARGIN;
}

/* Clipped meshes are mostly plain strips, so both paths are estimated at
 * 32 bytes/vertex to get in. A clipped mesh then counts what it really
 * sends (render_clipped) and guards its per-triangle output. */
static inline int vtxbuf_full(const DMSMesh* mesh, int clip) {
    int32_t need = (int32_t)(32 + mesh->vertex_count * 32);
    if (SHZ_LIKELY(need <= g_vtx_left)) {
        g_vtx_left -= clip ? 32 : need;
        return 0;
    }
    vtxbuf_warn();
    g_stats.meshes_vtxfull++;
    return 1;
}

/* ================================================================
 * Clipping utilities
 * ================================================================ */

/* Only the near plane is clipped: the TA bins by screen bounding box and
 * drops whatever lands outside the tile area, so triangles may hang off the
 * screen edges. What it can't take is w <= 0. Clip-space z is a constant here
 * (the PVR only needs 1/w), so the near plane is w = NEAR_Z. No far clip: the
 * block/sphere cull handles it. */
static inline uint32_t compute_outcode(const ClipVertex* v) {
    return v->w < NEAR_Z ? OC_NEAR : 0;
}

static inline void clip_lerp(const ClipVertex* a, const ClipVertex* b,
                              float da, float db, ClipVertex* out) {
    float t = da / (da - db);
    float s = 1.0f - t;
    out->x = s * a->x + t * b->x;
    out->y = s * a->y + t * b->y;
    out->z = s * a->z + t * b->z;
    out->w = s * a->w + t * b->w;
    out->u = s * a->u + t * b->u;
    out->v = s * a->v + t * b->v;

    uint8_t* ca = (uint8_t*)&a->argb;
    uint8_t* cb = (uint8_t*)&b->argb;
    uint8_t* co = (uint8_t*)&out->argb;
    co[0] = (uint8_t)(s * ca[0] + t * cb[0]);
    co[1] = (uint8_t)(s * ca[1] + t * cb[1]);
    co[2] = (uint8_t)(s * ca[2] + t * cb[2]);
    co[3] = (uint8_t)(s * ca[3] + t * cb[3]);
}

/* Clip a triangle against the near plane; out gets 0, 3 or 4 verts */
static int clip_tri_near(const ClipVertex* const in[3], ClipVertex* out) {
    int out_n = 0;
    for (int i = 0; i < 3; i++) {
        int j = (i + 1 < 3) ? i + 1 : 0;
        float di = in[i]->w - NEAR_Z;
        float dj = in[j]->w - NEAR_Z;
        if (di >= 0.0f) {
            out[out_n++] = *in[i];
            if (dj < 0.0f)
                clip_lerp(in[i], in[j], di, dj, &out[out_n++]);
        } else if (dj >= 0.0f) {
            clip_lerp(in[i], in[j], di, dj, &out[out_n++]);
        }
    }
    return out_n;
}

static inline void submit_vert(ClipVertex* v, uint32_t flags) {
    g_vtx_left -= 32;
    float inv_w = shz_invf_fsrra(v->w);
    pvr_vertex_t* pv = pvr_dr_target(*g_dr);
    pv->flags = flags;
    pv->x = v->x * inv_w;
    pv->y = v->y * inv_w;
    pv->z = inv_w;
    pv->u = v->u;
    pv->v = v->v;
    pv->argb = v->argb;
    pvr_dr_commit(pv);
}

/* ================================================================
 * Render: fast path (static mesh, fully inside frustum)
 * ================================================================ */

static void render_fast(const DMSVertex* src, int count,
                        pvr_dr_state_t* dr) {
    if (count < 1) return;

    SHZ_PREFETCH(&src[0]);
    SHZ_PREFETCH(&src[1]);
    SHZ_PREFETCH(&src[2]);
    SHZ_PREFETCH(&src[3]);

    shz_vec4_t t0 = shz_xmtrx_transform_vec4(
        shz_vec4_init(src[0].x, src[0].y, -src[0].z, 1.0f)
    );
    t0 = shz_vec4_swizzle(t0, 1, 2, 3, 0);

    float    cur_invw  = shz_invf_fsrra(t0.w);
    float    cur_sx    = t0.x * cur_invw;
    float    cur_sy    = t0.y * cur_invw;
    uint32_t cur_flags = src[0].flags;
    float    cur_u     = src[0].u;
    float    cur_v     = src[0].v;
    uint32_t cur_argb  = src[0].argb;

    for (int i = 1; i < count; i++) {
        SHZ_PREFETCH(&src[i + 4]);

        float    nx     = src[i].x;
        float    ny     = src[i].y;
        float    nz     = -src[i].z;
        uint32_t nflags = src[i].flags;
        float    nu     = src[i].u;
        float    nv     = src[i].v;
        uint32_t nargb  = src[i].argb;

        shz_vec4_t next_t = shz_xmtrx_transform_vec4(
            shz_vec4_init(nx, ny, nz, 1.0f)
        );

        pvr_vertex_t* pv = pvr_dr_target(*dr);
        pv->flags = cur_flags;
        pv->x     = cur_sx;
        pv->y     = cur_sy;
        pv->z     = cur_invw;
        pv->u     = cur_u;
        pv->v     = cur_v;
        pv->argb  = cur_argb;
        pvr_dr_commit(pv);

        next_t = shz_vec4_swizzle(next_t, 1, 2, 3, 0);
        cur_invw  = shz_invf_fsrra(next_t.w);
        cur_sx    = next_t.x * cur_invw;
        cur_sy    = next_t.y * cur_invw;
        cur_flags = nflags;
        cur_u     = nu;
        cur_v     = nv;
        cur_argb  = nargb;
    }

    pvr_vertex_t* pv = pvr_dr_target(*dr);
    pv->flags = cur_flags;
    pv->x     = cur_sx;
    pv->y     = cur_sy;
    pv->z     = cur_invw;
    pv->u     = cur_u;
    pv->v     = cur_v;
    pv->argb  = cur_argb;
    pvr_dr_commit(pv);
}

/* ================================================================
 * Render: clipped path (near-plane intersection)
 * ================================================================ */

static void render_clipped(const DMSVertex* src, int count,
                           pvr_dr_state_t* dr) {
    g_dr = dr;

    /* Transform all verts, compute outcodes */
    uint32_t combined_or = 0;
    for (int i = 0; i < count; i++) {
        SHZ_PREFETCH(&src[i + 4]);

        shz_vec4_t t = shz_xmtrx_transform_vec4(
            shz_vec4_init(src[i].x, src[i].y, -src[i].z, 1.0f)
        );
        t = shz_vec4_swizzle(t, 1, 2, 3, 0);

        g_clip_buffer[i].x = t.x;
        g_clip_buffer[i].y = t.y;
        g_clip_buffer[i].z = t.z;
        g_clip_buffer[i].w = t.w;
        g_clip_buffer[i].u = src[i].u;
        g_clip_buffer[i].v = src[i].v;
        g_clip_buffer[i].argb = src[i].argb;
        g_clip_buffer[i].flags = compute_outcode(&g_clip_buffer[i]);
        combined_or |= g_clip_buffer[i].flags;
    }

    /* All verts inside frustum: fast submit from clip buffer */
    if (combined_or == 0) {
        g_vtx_left -= count * 32;
        for (int i = 0; i < count; i++) {
            ClipVertex* cv = &g_clip_buffer[i];
            float inv_w = shz_invf_fsrra(cv->w);
            pvr_vertex_t* pv = pvr_dr_target(*dr);
            pv->flags = src[i].flags;
            pv->x     = cv->x * inv_w;
            pv->y     = cv->y * inv_w;
            pv->z     = inv_w;
            pv->u     = cv->u;
            pv->v     = cv->v;
            pv->argb  = cv->argb;
            pvr_dr_commit(pv);
        }
        return;
    }

    /* Walk triangle strips (some verts actually need clipping) */
    int idx = 0;
    while (idx < count) {
        int strip_start = idx;
        int strip_end = idx;

        while (strip_end < count && src[strip_end].flags != PVR_CMD_VERTEX_EOL)
            strip_end++;
        if (strip_end < count) strip_end++;

        int strip_len = strip_end - strip_start;
        if (strip_len < 3) { idx = strip_end; continue; }

        ClipVertex* v = &g_clip_buffer[strip_start];
        int in_strip = 0;

        for (int j = 2; j < strip_len; j++) {
            int eos = (j == strip_len - 1);
            uint32_t oc0 = v[j-2].flags;
            uint32_t oc1 = v[j-1].flags;
            uint32_t oc2 = v[j].flags;

            uint32_t or_codes  = oc0 | oc1 | oc2;
            uint32_t and_codes = oc0 & oc1 & oc2;

            if (or_codes == 0) {
                if (!in_strip) {
                    submit_vert(&v[j-2], PVR_CMD_VERTEX);
                    submit_vert(&v[j-1], PVR_CMD_VERTEX);
                    in_strip = 1;
                }
                int next_inside = !eos && (v[j+1].flags == 0);
                int end_strip = eos || !next_inside;
                submit_vert(&v[j], end_strip ? PVR_CMD_VERTEX_EOL
                                             : PVR_CMD_VERTEX);
                if (end_strip) in_strip = 0;

            } else if (and_codes != 0) {
                if (in_strip) in_strip = 0;

            } else {
                if (in_strip) in_strip = 0;

                /* Up to 4 verts per clipped triangle */
                if (g_vtx_left < 4 * 32) { vtxbuf_warn(); continue; }

                const ClipVertex* tri[3] = {
                    (j & 1) ? &v[j-1] : &v[j-2],
                    (j & 1) ? &v[j-2] : &v[j-1],
                    &v[j]
                };
                ClipVertex poly[4];
                int n = clip_tri_near(tri, poly);

                /* Triangle, or a quad sent as a 4-vert strip */
                if (n == 3) {
                    submit_vert(&poly[0], PVR_CMD_VERTEX);
                    submit_vert(&poly[1], PVR_CMD_VERTEX);
                    submit_vert(&poly[2], PVR_CMD_VERTEX_EOL);
                } else if (n == 4) {
                    submit_vert(&poly[0], PVR_CMD_VERTEX);
                    submit_vert(&poly[1], PVR_CMD_VERTEX);
                    submit_vert(&poly[3], PVR_CMD_VERTEX);
                    submit_vert(&poly[2], PVR_CMD_VERTEX_EOL);
                }
            }
        }
        idx = strip_end;
    }
}

/* ================================================================
 * Render: skinned path (animated mesh, fused skin+MVP+submit)
 * ================================================================ */

static void render_skinned(const DMSVertex* src, int count,
                           const DMSSkeleton* sk,
                           const shz_mat4x4_t* mvp,
                           pvr_dr_state_t* dr) {
    if (count < 1 || !sk) return;

    int last_bone = -1;

    SHZ_PREFETCH(&src[0]);
    SHZ_PREFETCH(&src[1]);
    SHZ_PREFETCH(&src[2]);
    SHZ_PREFETCH(&src[3]);

    /* Prime: load bone matrix for vertex 0 */
    {
        uint8_t bone_id = src[0].pad;
        if (bone_id != last_bone) {
            shz_xmtrx_load_apply_4x4(mvp, &sk->bones[bone_id].skinMatrix);
            last_bone = bone_id;
        }
    }

    /* No -z: Z negation is baked into MVP scale */
    shz_vec4_t t0 = shz_xmtrx_transform_vec4(
        shz_vec4_init(src[0].x, src[0].y, src[0].z, 1.0f)
    );
    t0 = shz_vec4_swizzle(t0, 1, 2, 3, 0);

    float    cur_invw  = shz_invf_fsrra(t0.w);
    float    cur_sx    = t0.x * cur_invw;
    float    cur_sy    = t0.y * cur_invw;
    uint32_t cur_flags = src[0].flags;
    float    cur_u     = src[0].u;
    float    cur_v     = src[0].v;
    uint32_t cur_argb  = src[0].argb;

    for (int i = 1; i < count; i++) {
        SHZ_PREFETCH(&src[i + 4]);

        uint8_t bone_id = src[i].pad;

        if (bone_id != last_bone) {
            shz_xmtrx_load_apply_4x4(mvp, &sk->bones[bone_id].skinMatrix);
            last_bone = bone_id;
        }

        float    nx     = src[i].x;
        float    ny     = src[i].y;
        float    nz     = src[i].z;  /* no negation — baked into MVP */
        uint32_t nflags = src[i].flags;
        float    nu     = src[i].u;
        float    nv     = src[i].v;
        uint32_t nargb  = src[i].argb;

        shz_vec4_t next_t = shz_xmtrx_transform_vec4(
            shz_vec4_init(nx, ny, nz, 1.0f)
        );

        /* Submit previous vertex while next_t result settles */
        pvr_vertex_t* pv = pvr_dr_target(*dr);
        pv->flags = cur_flags;
        pv->x     = cur_sx;
        pv->y     = cur_sy;
        pv->z     = cur_invw;
        pv->u     = cur_u;
        pv->v     = cur_v;
        pv->argb  = cur_argb;
        pvr_dr_commit(pv);

        next_t = shz_vec4_swizzle(next_t, 1, 2, 3, 0);
        cur_invw  = shz_invf_fsrra(next_t.w);
        cur_sx    = next_t.x * cur_invw;
        cur_sy    = next_t.y * cur_invw;
        cur_flags = nflags;
        cur_u     = nu;
        cur_v     = nv;
        cur_argb  = nargb;
    }

    /* Flush last vertex */
    pvr_vertex_t* pv = pvr_dr_target(*dr);
    pv->flags = cur_flags;
    pv->x     = cur_sx;
    pv->y     = cur_sy;
    pv->z     = cur_invw;
    pv->u     = cur_u;
    pv->v     = cur_v;
    pv->argb  = cur_argb;
    pvr_dr_commit(pv);
}

/* ================================================================
 * Mesh dispatch (per-mesh frustum cull + path selection)
 * ================================================================ */

/* Skinned mesh: cull the whole animated bound, then skin + submit.
 * Returns 1 if the mesh was drawn, 0 if culled. */
static int draw_skinned_mesh(DMSMesh* mesh, DMSModel* model,
                             shz_vec3_t pos, float scale, float yaw,
                             const DCCamera* cam, pvr_dr_state_t* dr) {
    float bcx = model->anim_bound_cx;
    float bcz = model->anim_bound_cz;

    /* Rotate bounding sphere center to match model yaw */
    if (yaw != 0.0f) {
        shz_sincos_t sc = shz_sincosf(yaw);
        float rx = bcx * sc.cos - bcz * sc.sin;
        float rz = bcx * sc.sin + bcz * sc.cos;
        bcx = rx;
        bcz = rz;
    }

    shz_vec3_t wc = shz_vec3_init(pos.x + bcx * scale,
                                  pos.y + model->anim_bound_cy * scale,
                                  pos.z + bcz * scale);
    float wr = model->anim_bound_radius * scale;

    /* Skinned meshes have no clip path: cull if off-screen or crossing near */
    if (dc_frustum_cull_sphere(cam, wc, wr) < 0 ||
        dc_frustum_near_intersect(cam, wc, wr)) {
        g_stats.meshes_culled++;
        return 0;
    }

    if (vtxbuf_full(mesh, 0))
        return 0;

    g_stats.meshes_drawn++;
    g_stats.tris_drawn += mesh->tri_count;

    shz_sq_memcpy32_1_xmtrx(pvr_dr_target(*dr), &mesh->header);

    /* Build MVP with Z negation baked into scale */
    shz_xmtrx_load_4x4((shz_mat4x4_t*)dc_camera_get_pv(cam));
    shz_xmtrx_translate(pos.x - cam->pos.x, pos.y - cam->pos.y, -(pos.z - cam->pos.z));
    if (yaw != 0.0f) shz_xmtrx_apply_rotation_y(yaw);
    shz_xmtrx_apply_scale(scale, scale, -scale);

    alignas(32) shz_mat4x4_t mvp;
    shz_xmtrx_store_4x4(&mvp);

    g_stats.verts_xformed += mesh->vertex_count;
    render_skinned(mesh->vertices, mesh->vertex_count, model->skeleton, &mvp, dr);
    return 1;
}

/* ================================================================
 * dc_model_draw
 * ================================================================ */

void dc_model_draw(DMSModel* model, shz_vec3_t pos, float scale,
                   const DCCamera* cam) {
    dc_model_draw_rotated(model, pos, scale, 0.0f, cam);
}

/* Frustum test with the side planes already in XMTRX. Same decisions as
 * dc_frustum_cull_sphere(); also hands back the near-plane distance so the
 * clip test doesn't redo it. */
static inline int sphere_visible(const WorldFrustum* fr, shz_vec3_t c, float r, float* near_dist) {
    shz_vec4_t p = shz_vec4_init(c.x, c.y, c.z, 1.0f);
    float nd = shz_vec4_dot(fr->near_plane, p);
    *near_dist = nd;
    if (nd < -r) return 0;
    if (shz_vec4_dot(fr->far_plane, p) < -r) return 0;
    shz_vec4_t d = shz_xmtrx_transform_vec4(p);
    return !(d.x < -r || d.y < -r || d.z < -r || d.w < -r);
}

enum { XM_OTHER, XM_PLANES, XM_MVP };   /* what XMTRX holds right now */

#define DRAW_BATCH 64

/* Static model with blocks: one sphere test per block run (a culled block's
 * meshes are never touched), then one per mesh inside a visible block.
 * XMTRX swaps between the frustum side planes (tests) and the MVP (drawing)
 * as few times as possible: a block's meshes are all tested first, then the
 * survivors drawn. The header copy goes through XMTRX, so the MVP is only
 * reloaded after a header was sent. */
/* Bounds centre of a block or mesh, turned the way the model is drawn.
 * rot is 3 columns (where the model's x, y and z axes point), or NULL for yaw */
static inline shz_vec3_t turn_centre(const float* rot, float yaw, shz_sincos_t sc,
                                     float cx, float cy, float cz) {
    if (rot)
        return shz_vec3_init(rot[0] * cx + rot[3] * cy + rot[6] * cz,
                             rot[1] * cx + rot[4] * cy + rot[7] * cz,
                             rot[2] * cx + rot[5] * cy + rot[8] * cz);
    if (yaw != 0.0f)
        return shz_vec3_init(cx * sc.cos - cz * sc.sin, cy, cx * sc.sin + cz * sc.cos);
    return shz_vec3_init(cx, cy, cz);
}

static void draw_blocks_list(DMSModel* model, shz_vec3_t pos, float scale,
                             float yaw, const float* rot,
                             const DCCamera* cam, int target_list) {
    shz_sincos_t sc = shz_sincosf(yaw);
    const shz_mat4x4_t* pv = dc_camera_get_pv(cam);
    const WorldFrustum* fr = dc_camera_get_frustum(cam);
    alignas(32) shz_mat4x4_t mvp;
    shz_xmtrx_load_4x4((shz_mat4x4_t*)pv);
    shz_xmtrx_translate(pos.x - cam->pos.x, pos.y - cam->pos.y, -(pos.z - cam->pos.z));
    if (rot) {
        /* Vertices go in with z negated, so the z row and z column flip sign */
        alignas(32) shz_mat4x4_t rm;
        rm.elem2D[0][0] =  rot[0]; rm.elem2D[0][1] =  rot[1]; rm.elem2D[0][2] = -rot[2]; rm.elem2D[0][3] = 0.0f;
        rm.elem2D[1][0] =  rot[3]; rm.elem2D[1][1] =  rot[4]; rm.elem2D[1][2] = -rot[5]; rm.elem2D[1][3] = 0.0f;
        rm.elem2D[2][0] = -rot[6]; rm.elem2D[2][1] = -rot[7]; rm.elem2D[2][2] =  rot[8]; rm.elem2D[2][3] = 0.0f;
        rm.elem2D[3][0] = 0.0f;    rm.elem2D[3][1] = 0.0f;    rm.elem2D[3][2] = 0.0f;    rm.elem2D[3][3] = 1.0f;
        shz_xmtrx_apply_4x4(&rm);
    } else if (yaw != 0.0f) {
        shz_xmtrx_apply_rotation_y(yaw);
    }
    shz_xmtrx_apply_scale(scale, scale, scale);
    shz_xmtrx_store_4x4(&mvp);
    int xm = XM_MVP;

    const pvr_poly_hdr_t* last_hdr = NULL;
    pvr_dr_state_t* dr = NULL;
    int list = target_list == PVR_LIST_OP_POLY ? 0 : target_list == PVR_LIST_PT_POLY ? 1 : 2;

    uint32_t batch[DRAW_BATCH];          /* mesh index, top bit = clip path */

    for (uint32_t r = model->list_runs[list]; r < model->list_runs[list + 1]; r++) {
        const DMSBlockRun* run = &model->runs[r];
        const DMSBlock* b = &model->blocks[run->block];
        shz_vec3_t bc = turn_centre(rot, yaw, sc, b->cx, b->cy, b->cz);
        shz_vec3_t wc = shz_vec3_init(pos.x + bc.x * scale, pos.y + bc.y * scale,
                                      pos.z + bc.z * scale);
        float wr = b->radius * scale;
        if (xm != XM_PLANES) {
            shz_xmtrx_load_4x4((shz_mat4x4_t*)&fr->side_planes);
            xm = XM_PLANES;
        }
        float nd;
        if (!sphere_visible(fr, wc, wr, &nd)) {
            g_stats.meshes_culled += run->count;
            continue;
        }
        int block_near = nd < wr && nd > -wr;

        uint32_t m = run->first, run_end = run->first + run->count;
        while (m < run_end) {
            /* Test: cull each mesh on its own sphere; if the block crosses
             * the near plane, only meshes whose own sphere crosses it take
             * the clip path */
            int n = 0;
            if (xm != XM_PLANES) {
                shz_xmtrx_load_4x4((shz_mat4x4_t*)&fr->side_planes);
                xm = XM_PLANES;
            }
            for (; m < run_end && n < DRAW_BATCH; m++) {
                const DMSMesh* mesh = &model->meshes[m];
                if (mesh->material_flags & DMS_MAT_COLLISION_ONLY) continue;
                shz_vec3_t tc = turn_centre(rot, yaw, sc, mesh->bound_cx, mesh->bound_cy,
                                            mesh->bound_cz);
                shz_vec3_t mc = shz_vec3_init(pos.x + tc.x * scale, pos.y + tc.y * scale,
                                              pos.z + tc.z * scale);
                float mr = mesh->bound_radius * scale;
                if (!sphere_visible(fr, mc, mr, &nd)) {
                    g_stats.meshes_culled++;
                    continue;
                }
                int clip = block_near && nd < mr && nd > -mr;
                batch[n++] = m | (clip ? 0x80000000u : 0);
            }

            /* Draw the survivors */
            for (int i = 0; i < n; i++) {
                DMSMesh* mesh = &model->meshes[batch[i] & 0x7fffffffu];
                if (vtxbuf_full(mesh, batch[i] >> 31)) continue;

                if (!dr) {
                    dc_list_begin(target_list);
                    dr = dc_dr_state();
                    xm = XM_OTHER;
                }

                g_stats.meshes_drawn++;
                g_stats.tris_drawn += mesh->tri_count;

                if (!last_hdr || memcmp(last_hdr, &mesh->header, sizeof(pvr_poly_hdr_t))) {
                    shz_sq_memcpy32_1_xmtrx(pvr_dr_target(*dr), &mesh->header);
                    last_hdr = &mesh->header;
                    xm = XM_OTHER;
                }

                if (xm != XM_MVP) {
                    shz_xmtrx_load_4x4(&mvp);
                    xm = XM_MVP;
                }
                if (batch[i] & 0x80000000u) {
                    g_stats.verts_clipped += mesh->vertex_count;
                    render_clipped(mesh->vertices, mesh->vertex_count, dr);
                } else {
                    g_stats.verts_xformed += mesh->vertex_count;
                    render_fast(mesh->vertices, mesh->vertex_count, dr);
                }
            }
        }
    }
}

/* Skinned models: one mesh at a time */
static void draw_skinned_list(DMSModel* model, shz_vec3_t pos, float scale,
                              float yaw, const DCCamera* cam, int target_list) {
    pvr_dr_state_t* dr = NULL;

    for (uint32_t m = 0; m < model->mesh_count; m++) {
        DMSMesh* mesh = &model->meshes[m];
        int alpha_mode = mesh->material_flags & 0x3;
        int pvr_list = alpha_mode == 0 ? PVR_LIST_OP_POLY
                     : alpha_mode == 1 ? PVR_LIST_PT_POLY : PVR_LIST_TR_POLY;
        if (pvr_list != target_list) continue;
        if (mesh->material_flags & DMS_MAT_COLLISION_ONLY) continue;

        if (!dr) {
            dc_list_begin(target_list);
            dr = dc_dr_state();
        }
        draw_skinned_mesh(mesh, model, pos, scale, yaw, cam, dr);
    }
}

void dc_model_draw_list_rotated(DMSModel* model, shz_vec3_t pos, float scale,
                                float yaw, const DCCamera* cam, int target_list) {
    if (!model || model->mesh_count == 0) return;

    vtxbuf_sync();
    if (model->skeleton)
        draw_skinned_list(model, pos, scale, yaw, cam, target_list);
    else
        draw_blocks_list(model, pos, scale, yaw, NULL, cam, target_list);
}

void dc_model_draw_list_oriented(DMSModel* model, shz_vec3_t pos, float scale,
                                 const float rot[9], const DCCamera* cam, int target_list) {
    if (!model || model->mesh_count == 0 || model->skeleton) return;

    vtxbuf_sync();
    draw_blocks_list(model, pos, scale, 0.0f, rot, cam, target_list);
}

void dc_model_draw_list(DMSModel* model, shz_vec3_t pos, float scale,
                        const DCCamera* cam, int target_list) {
    dc_model_draw_list_rotated(model, pos, scale, 0.0f, cam, target_list);
}

void dc_model_draw_rotated(DMSModel* model, shz_vec3_t pos, float scale,
                           float yaw, const DCCamera* cam) {
    dc_model_draw_list_rotated(model, pos, scale, yaw, cam, PVR_LIST_OP_POLY);
    dc_model_draw_list_rotated(model, pos, scale, yaw, cam, PVR_LIST_PT_POLY);
    dc_model_draw_list_rotated(model, pos, scale, yaw, cam, PVR_LIST_TR_POLY);
}

/* ================================================================
 * Animation
 * ================================================================ */

static void update_skeleton(DMSSkeleton* sk, float delta_time) {
    if (!sk || sk->animCount == 0) return;

    DMSAnimation* anim = &sk->animations[sk->currentAnim];
    if (anim->frameCount < 2 || anim->duration <= 0.0f) return;

    /* Advance time, loop */
    sk->currentTime += delta_time;
    while (sk->currentTime >= anim->duration)
        sk->currentTime -= anim->duration;
    while (sk->currentTime < 0.0f)
        sk->currentTime += anim->duration;

    /* Compute interpolation frame + alpha */
    float progress = sk->currentTime * shz_invf_fsrra(anim->duration);
    float frame_f  = progress * (float)(anim->frameCount - 1);
    int   frame    = (int)frame_f;
    int   next     = frame + 1;
    if (next >= anim->frameCount) next = 0;
    float alpha    = frame_f - (float)frame;

    /* Interpolate each bone's local pose and build world matrices */
    for (int i = 0; i < sk->boneCount; i++) {
        DMSBone* bone = &sk->bones[i];

        DMSTransform* curr = &anim->framePoses[frame * sk->boneCount + i];
        DMSTransform* nxt  = &anim->framePoses[next  * sk->boneCount + i];

        bone->localPose.translation =
            shz_vec3_lerp(curr->translation, nxt->translation, alpha);
        bone->localPose.scale =
            shz_vec3_lerp(curr->scale, nxt->scale, alpha);
        bone->localPose.rotation =
            shz_quat_nlerp(curr->rotation, nxt->rotation, alpha);

        shz_xmtrx_init_scale(bone->localPose.scale.x,
                             bone->localPose.scale.y,
                             bone->localPose.scale.z);
        shz_xmtrx_apply_rotation_quat(bone->localPose.rotation);
        shz_xmtrx_set_translation(bone->localPose.translation.x,
                                  bone->localPose.translation.y,
                                  bone->localPose.translation.z);

        if (bone->parent >= 0) {
            shz_xmtrx_apply_reverse_4x4(&sk->bones[bone->parent].worldPose);
        }

        shz_xmtrx_store_4x4(&bone->worldPose);
        shz_xmtrx_apply_store_4x4(&bone->skinMatrix, &bone->inverseBindMatrix);
    }
}

static void update_bounds_from_skeleton(DMSModel* model) {
    DMSSkeleton* sk = model->skeleton;
    if (!sk || sk->boneCount == 0) return;

    float minx, maxx, miny, maxy, minz, maxz;
    {
        shz_mat4x4_t* wp = &sk->bones[0].worldPose;
        minx = maxx = wp->elem2D[3][0];
        miny = maxy = wp->elem2D[3][1];
        minz = maxz = wp->elem2D[3][2];
    }

    for (int i = 1; i < sk->boneCount; i++) {
        shz_mat4x4_t* wp = &sk->bones[i].worldPose;
        float bx = wp->elem2D[3][0];
        float by = wp->elem2D[3][1];
        float bz = wp->elem2D[3][2];
        if (bx < minx) minx = bx;
        if (bx > maxx) maxx = bx;
        if (by < miny) miny = by;
        if (by > maxy) maxy = by;
        if (bz < minz) minz = bz;
        if (bz > maxz) maxz = bz;
    }

    float ex = (maxx - minx) * 0.5f;
    float ey = (maxy - miny) * 0.5f;
    float ez = (maxz - minz) * 0.5f;
    float r = shz_sqrtf_fsrra(ex * ex + ey * ey + ez * ez);
    r += model->max_bind_radius * 0.3f;

    model->anim_bound_cx = (minx + maxx) * 0.5f;
    model->anim_bound_cy = (miny + maxy) * 0.5f;
    model->anim_bound_cz = (minz + maxz) * 0.5f;
    model->anim_bound_radius = r;
}

void dc_model_animate(DMSModel* model, float dt) {
    if (!model || !model->skeleton) return;

    update_skeleton(model->skeleton, dt);
    update_bounds_from_skeleton(model);
}

void dc_model_set_anim(DMSModel* model, int anim_index) {
    if (!model || !model->skeleton) return;
    if (anim_index < 0 || anim_index >= model->skeleton->animCount) return;
    if (model->skeleton->currentAnim == anim_index) return;
    model->skeleton->currentAnim = anim_index;
    model->skeleton->currentTime = 0.0f;
}

int dc_model_get_anim(DMSModel* model) {
    if (!model || !model->skeleton) return -1;
    return model->skeleton->currentAnim;
}

/* ================================================================
 * Loading
 * ================================================================ */

DMSModel* dc_model_load(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) {
        printf("DMS: Failed to open %s\n", filename);
        return NULL;
    }

    uint32_t magic, mesh_count, bone_count;
    fread(&magic, 4, 1, f);
    fread(&mesh_count, 4, 1, f);
    fread(&bone_count, 4, 1, f);

    if (magic != DMS_MAGIC) { printf("DMS: %s is not a .dms file\n", filename); fclose(f); return NULL; }

    int is_animated = (bone_count > 0);

    DMSModel* model = malloc(sizeof(DMSModel));
    memset(model, 0, sizeof(DMSModel));
    model->mesh_count = mesh_count;

    fread(&model->opaque_count, 4, 1, f);
    fread(&model->cutout_count, 4, 1, f);
    fread(&model->transparent_count, 4, 1, f);
    printf("DMS: %lu opaque, %lu cutout, %lu transparent\n",
           (unsigned long)model->opaque_count,
           (unsigned long)model->cutout_count,
           (unsigned long)model->transparent_count);

    model->meshes = memalign(32, mesh_count * sizeof(DMSMesh));
    memset(model->meshes, 0, mesh_count * sizeof(DMSMesh));
    model->textures = NULL;
    model->texture_count = 0;
    model->skeleton = NULL;

    /* ---- Load skeleton ---- */
    if (is_animated) {
        printf("DMS: Loading skeleton with %lu bones\n", (unsigned long)bone_count);

        DMSSkeleton* sk = calloc(1, sizeof(DMSSkeleton));
        sk->boneCount = bone_count;
        sk->bones = memalign(32, bone_count * sizeof(DMSBone));
        memset(sk->bones, 0, bone_count * sizeof(DMSBone));

        for (uint32_t i = 0; i < bone_count; i++) {
            DMSBone* bone = &sk->bones[i];
            fread(bone->name, sizeof(char), 64, f);
            fread(&bone->parent, sizeof(int), 1, f);
            fread(&bone->bindPose, sizeof(DMSTransform), 1, f);
            fread(&bone->inverseBindMatrix, sizeof(shz_mat4x4_t), 1, f);
            bone->localPose = bone->bindPose;

            printf("  Bone %lu: %s (parent=%d)\n",
                   (unsigned long)i, bone->name, bone->parent);
        }

        /* Build initial world poses from bind pose hierarchy */
        for (uint32_t i = 0; i < bone_count; i++) {
            DMSBone* bone = &sk->bones[i];

            shz_xmtrx_init_scale(bone->bindPose.scale.x,
                                 bone->bindPose.scale.y,
                                 bone->bindPose.scale.z);
            shz_xmtrx_apply_rotation_quat(bone->bindPose.rotation);
            shz_xmtrx_apply_translation(bone->bindPose.translation.x,
                                        bone->bindPose.translation.y,
                                        bone->bindPose.translation.z);

            if (bone->parent >= 0) {
                shz_xmtrx_apply_reverse_4x4(&sk->bones[bone->parent].worldPose);
            }

            shz_xmtrx_store_4x4(&bone->worldPose);
        }

        /* Load animations */
        uint32_t anim_count;
        fread(&anim_count, 4, 1, f);
        printf("DMS: %lu animations\n", (unsigned long)anim_count);

        if (anim_count > 0) {
            sk->animCount = anim_count;
            sk->animations = calloc(anim_count, sizeof(DMSAnimation));

            for (uint32_t i = 0; i < anim_count; i++) {
                DMSAnimation* anim = &sk->animations[i];
                fread(anim->name, sizeof(char), 32, f);
                fread(&anim->boneCount, sizeof(int), 1, f);
                fread(&anim->frameCount, sizeof(int), 1, f);
                fread(&anim->duration, sizeof(float), 1, f);

                size_t total_poses = anim->frameCount * anim->boneCount;
                anim->framePoses = calloc(total_poses, sizeof(DMSTransform));
                fread(anim->framePoses, sizeof(DMSTransform), total_poses, f);

                printf("  Anim %lu: '%s' %d bones, %d frames, %.2fs\n",
                       (unsigned long)i, anim->name,
                       anim->boneCount, anim->frameCount, anim->duration);
            }
        }

        sk->currentAnim = 0;
        sk->currentTime = 0.0f;
        model->skeleton = sk;

    } else {
        uint32_t anim_count;
        fread(&anim_count, 4, 1, f);
    }

    /* ---- Block table (static models only) ---- */
    fread(&model->block_count, 4, 1, f);
    model->blocks = malloc(model->block_count * sizeof(DMSBlock));
    fread(model->blocks, sizeof(DMSBlock), model->block_count, f);
    if (!is_animated) printf("DMS: %lu blocks\n", (unsigned long)model->block_count);
    if (!is_animated && model->block_count == 0) {
        printf("DMS: %s has no blocks\n", filename);
        free(model->blocks); free(model->meshes); free(model); fclose(f);
        return NULL;
    }

    /* ---- Load meshes ---- */
    uint32_t max_verts = 0;

    for (uint32_t m = 0; m < mesh_count; m++) {
        DMSMesh* mesh = &model->meshes[m];

        fread(&mesh->vertex_count, 4, 1, f);
        fread(&mesh->texture_id, 4, 1, f);
        fread(&mesh->material_color, 4, 1, f);
        fread(&mesh->bound_cx, 4, 1, f);
        fread(&mesh->bound_cy, 4, 1, f);
        fread(&mesh->bound_cz, 4, 1, f);
        fread(&mesh->bound_radius, 4, 1, f);

        fread(&mesh->material_flags, 4, 1, f);
        fread(&mesh->alpha_cutoff, sizeof(float), 1, f);
        fread(&mesh->block, 4, 1, f);

        mesh->vertices = memalign(32, mesh->vertex_count * sizeof(DMSVertex));
        fread(mesh->vertices, sizeof(DMSVertex), mesh->vertex_count, f);
        mesh->animated_vertices = NULL;

        /* Count triangles: each strip of n verts contributes n-2 */
        mesh->tri_count = 0;
        for (uint32_t v = 0, strip_len = 0; v < mesh->vertex_count; v++) {
            strip_len++;
            if (mesh->vertices[v].flags == PVR_CMD_VERTEX_EOL) {
                if (strip_len >= 3) mesh->tri_count += strip_len - 2;
                strip_len = 0;
            }
        }

        if (mesh->vertex_count > max_verts)
            max_verts = mesh->vertex_count;
    }

    /* ---- Block runs: meshes are sorted by list, then block ---- */
    if (!is_animated) {
        model->runs = malloc(mesh_count * sizeof(DMSBlockRun));
        uint32_t n = 0, list = 0;
        model->list_runs[0] = 0;
        for (uint32_t m = 0; m < mesh_count; m++) {
            const DMSMesh* mesh = &model->meshes[m];
            uint32_t mode = mesh->material_flags & 0x3;
            while (list < mode) model->list_runs[++list] = n;
            if (n == model->list_runs[list] || model->runs[n - 1].block != mesh->block) {
                model->runs[n].block = mesh->block;
                model->runs[n].first = m;
                model->runs[n].count = 0;
                n++;
            }
            model->runs[n - 1].count++;
        }
        while (list < 3) model->list_runs[++list] = n;
    }

    /* ---- Cache max bind radius for animated bounds ---- */
    model->max_bind_radius = 0.0f;
    for (uint32_t m = 0; m < mesh_count; m++) {
        if (model->meshes[m].bound_radius > model->max_bind_radius)
            model->max_bind_radius = model->meshes[m].bound_radius;
    }

    /* Clip buffer (shared, grows to largest static model) */
    if (!is_animated && max_verts > g_clip_buffer_size) {
        if (g_clip_buffer) free(g_clip_buffer);
        g_clip_buffer = memalign(32, max_verts * sizeof(ClipVertex));
        g_clip_buffer_size = max_verts;
    }

    /* ---- Embedded textures & PVR headers ---- */
    uint32_t tex_count;
    fread(&tex_count, 4, 1, f);
    printf("DMS: %lu embedded textures\n", (unsigned long)tex_count);

    model->texture_count = tex_count;
    if (tex_count > 0) {
        struct { uint32_t offset; uint32_t size; } *tex_table;
        tex_table = malloc(tex_count * 8);
        fread(tex_table, 8, tex_count, f);

        model->textures = calloc(tex_count, sizeof(dttex_info_t));

        for (uint32_t i = 0; i < tex_count; i++) {
            if (tex_table[i].size == 0) continue;

            void *buf = malloc(tex_table[i].size);
            fseek(f, tex_table[i].offset, SEEK_SET);
            fread(buf, 1, tex_table[i].size, f);

            pvrtex_load_from_buffer(buf, tex_table[i].size, &model->textures[i]);
            free(buf);

            printf("  Tex %lu: %ux%u, %lu bytes\n",
                   (unsigned long)i,
                   model->textures[i].width,
                   model->textures[i].height,
                   (unsigned long)tex_table[i].size);
        }
        free(tex_table);
    }

    /* Compile PVR headers using material_flags */
    for (uint32_t m = 0; m < mesh_count; m++) {
        DMSMesh* mesh = &model->meshes[m];
        pvr_poly_cxt_t cxt;

        int alpha_mode   = mesh->material_flags & 0x3;
        int tex_filter   = (mesh->material_flags >> 9) & 0x1;

        int pvr_list;
        if (alpha_mode == 0)      pvr_list = PVR_LIST_OP_POLY;
        else if (alpha_mode == 1) pvr_list = PVR_LIST_PT_POLY;
        else                      pvr_list = PVR_LIST_TR_POLY;

        int tid = mesh->texture_id;

        if (tid >= 0 && tid < (int)tex_count && model->textures[tid].ptr) {
            dttex_info_t* tex = &model->textures[tid];
            pvr_poly_cxt_txr(&cxt, pvr_list,
                             tex->pvrformat,
                             tex->width, tex->height,
                             tex->ptr,
                             tex_filter ? PVR_FILTER_NONE : PVR_FILTER_BILINEAR);
        } else {
            pvr_poly_cxt_col(&cxt, pvr_list);
        }

        cxt.gen.culling = PVR_CULLING_NONE;

        if (alpha_mode == 2) {
            cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
            cxt.blend.src = PVR_BLEND_SRCALPHA;
            cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
        }

        pvr_poly_compile(&mesh->header, &cxt);
    }

    fclose(f);
    return model;
}

/* ================================================================
 * Free
 * ================================================================ */

void dc_model_free(DMSModel* model) {
    if (!model) return;

    for (uint32_t m = 0; m < model->mesh_count; m++) {
        free(model->meshes[m].vertices);
        if (model->meshes[m].animated_vertices)
            free(model->meshes[m].animated_vertices);
    }
    free(model->meshes);
    free(model->blocks);
    free(model->runs);

    if (model->textures) {
        for (int i = 0; i < model->texture_count; i++)
            pvrtex_unload(&model->textures[i]);
        free(model->textures);
    }

    if (model->skeleton) {
        DMSSkeleton* sk = model->skeleton;
        if (sk->bones) free(sk->bones);
        if (sk->animations) {
            for (int i = 0; i < sk->animCount; i++) {
                if (sk->animations[i].framePoses)
                    free(sk->animations[i].framePoses);
            }
            free(sk->animations);
        }
        free(sk);
    }

    free(model);
}

/* ================================================================
 * Stats
 * ================================================================ */

void dc_model_reset_stats(void) {
    memset(&g_stats, 0, sizeof(g_stats));
}

const DCModelStats* dc_model_get_stats(void) {
    return &g_stats;
}
