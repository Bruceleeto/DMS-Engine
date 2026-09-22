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
 * An overflow makes the TA write over other VRAM and hang the GPU, so a mesh
 * that might not fit is skipped. The budget is the buffer size, set once a
 * frame and counted down in software. Reading the TA's write position instead
 * returned the previous frame's total, which halved the budget.
 * ================================================================ */

#define VTXBUF_MARGIN (32 * 1024)   /* TA lag, background poly, HUD after models */

/* Bytes left in the vertex buffer after the safety margin */
static int32_t g_vtx_left;

static uint32_t g_vtxbuf_size;   /* constant; a PVR register read is ~0.5us */

/* Once a scene, not once a frame: the PVR hands the whole buffer back at the
 * start of each one. A frame that draws render targets is several scenes, and
 * charging the screen for what a target used would have it skip meshes with
 * most of the buffer free. */
void dc_model_frame_begin(void) {
    if (SHZ_UNLIKELY(!g_vtxbuf_size))
        g_vtxbuf_size = PVR_GET(PVR_TA_VERTBUF_END) - PVR_GET(PVR_TA_VERTBUF_START);
    g_vtx_left = (int32_t)g_vtxbuf_size - VTXBUF_MARGIN;
}

static void vtxbuf_warn_need(int32_t need) {
    static int warned = 0;
    if (warned) return;
    warned = 1;
    printf("DMS: PVR vertex buffer full (%luKB buffer, %ldKB left, %ldKB wanted), "
           "skipping meshes so it doesn't hang. Raise DCInitParams.vram_size or "
           "draw less.\n",
           (unsigned long)(g_vtxbuf_size / 1024),
           (long)(g_vtx_left / 1024), (long)(need / 1024));
}

static void vtxbuf_warn(void) { vtxbuf_warn_need(0); }

/* Clipped meshes are mostly plain strips, so both paths are estimated at
 * 32 bytes/vertex to get in. A clipped mesh then counts what it really
 * sends (render_clipped) and guards its per-triangle output. */
static inline int vtxbuf_full(const DMSMesh* mesh, int clip) {
    int32_t need = (int32_t)(32 + mesh->vertex_count * 32);
    if (SHZ_LIKELY(need <= g_vtx_left)) {
        g_vtx_left -= clip ? 32 : need;
        return 0;
    }
    vtxbuf_warn_need(need);
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
 * Runtime light
 *
 * One light, multiplied over the colours already baked into the vertices. It
 * is moved into the space the model's vertices are stored in once per draw
 * call, so the vertex loop dots it straight against the int8 normal each
 * vertex already carries and nothing is rotated per vertex.
 * ================================================================ */

typedef struct {
    float x, y, z;       /* the light, in the model's own space */
    float pos_w;         /* 1 for a light in a place, 0 for a sun */
    float r, g, b;       /* colour, times 256/127 for the byte multiply below */
    float ambient;       /* times 127, to match the length of an int8 normal */
    float inv_range;     /* 0 for a sun, which never fades */
} ModelLight;

static DCLight    g_light;
static bool       g_light_set;
static ModelLight g_ml;      /* g_light in the space of the model being drawn */
static int        g_lit;     /* this draw call shades instead of copying argb */
/* This mesh is drawn only to block the glow behind it (the bloom pass), so it
 * goes out black. Set per mesh, and only while dc_model_set_glow_only(). */
static int        g_flat;

void dc_model_set_light(const DCLight* light) {
    g_light_set = light != NULL;
    if (light) g_light = *light;
}

int dc_model_points(DMSModel* model, const char* material,
                    shz_vec3_t* out, int max) {
    if (!model || !material || !model->material_names) return 0;
    int found = 0;
    for (uint32_t m = 0; m < model->mesh_count; m++) {
        if (strcmp(model->material_names[m], material)) continue;
        DMSMesh* mesh = &model->meshes[m];
        mesh->material_flags |= DMS_MAT_MARKER;
        for (uint32_t i = 0; i < mesh->vertex_count; i++) {
            const DMSVertex* v = &mesh->vertices[i];
            /* A strip repeats its corners, so only the new ones are kept */
            int seen = 0;
            for (int j = 0; j < found && j < max; j++)
                if (out[j].x == v->x && out[j].y == v->y && out[j].z == v->z) {
                    seen = 1;
                    break;
                }
            if (seen) continue;
            if (found < max) out[found] = shz_vec3_init(v->x, v->y, v->z);
            found++;
        }
    }
    return found;
}

/* Move the light into the space the vertices are stored in. cols is where the
 * model's x, y and z axes point in the world (the rot[9] of the public call),
 * so its transpose brings a world direction back the other way. */
static void light_to_model(shz_vec3_t pos, float scale, const float* cols) {
    g_lit = g_light_set && scale > 0.0f;
    if (!g_lit) return;

    float w[3];
    if (g_light.sun) {
        w[0] = -g_light.pos.x; w[1] = -g_light.pos.y; w[2] = -g_light.pos.z;
        float n = shz_inv_sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        w[0] *= n; w[1] *= n; w[2] *= n;
        g_ml.pos_w = 0.0f;
        g_ml.inv_range = 0.0f;
    } else {
        float inv_scale = 1.0f / scale;
        w[0] = (g_light.pos.x - pos.x) * inv_scale;
        w[1] = (g_light.pos.y - pos.y) * inv_scale;
        w[2] = (g_light.pos.z - pos.z) * inv_scale;
        g_ml.pos_w = 1.0f;
        g_ml.inv_range = scale / (g_light.range > 0.0f ? g_light.range : 500.0f);
    }
    g_ml.x = cols[0] * w[0] + cols[1] * w[1] + cols[2] * w[2];
    g_ml.y = cols[3] * w[0] + cols[4] * w[1] + cols[5] * w[2];
    g_ml.z = cols[6] * w[0] + cols[7] * w[1] + cols[8] * w[2];

    const float k = 256.0f / 127.0f;
    int white = g_light.r <= 0.0f && g_light.g <= 0.0f && g_light.b <= 0.0f;
    g_ml.r = (white ? 1.0f : g_light.r) * k;
    g_ml.g = (white ? 1.0f : g_light.g) * k;
    g_ml.b = (white ? 1.0f : g_light.b) * k;
    g_ml.ambient = (g_light.ambient > 0.0f ? g_light.ambient : 0.25f) * 127.0f;
}

/* The vertex's baked colour with the light over it. pos_w is what lets a sun
 * use the same arithmetic: the difference below collapses to the light
 * direction, which is already unit length, and nothing fades. */
static inline uint32_t shade(const DMSVertex* s) {
    float dx = g_ml.x - s->x * g_ml.pos_w;
    float dy = g_ml.y - s->y * g_ml.pos_w;
    float dz = g_ml.z - s->z * g_ml.pos_w;
    float d2 = dx * dx + dy * dy + dz * dz;
    float inv = shz_inv_sqrtf(d2);

    float ndl = (s->nx * dx + s->ny * dy + s->nz * dz) * inv;
    float att = 1.0f - (d2 * inv) * g_ml.inv_range;
    if (ndl < 0.0f) ndl = 0.0f;
    if (att < 0.0f) att = 0.0f;
    float lit = g_ml.ambient + ndl * att;

    uint32_t c = s->argb;
    uint32_t r = (((c >> 16) & 0xff) * (uint32_t)(lit * g_ml.r)) >> 8;
    uint32_t g = (((c >>  8) & 0xff) * (uint32_t)(lit * g_ml.g)) >> 8;
    uint32_t b = (( c        & 0xff) * (uint32_t)(lit * g_ml.b)) >> 8;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (c & 0xff000000u) | (r << 16) | (g << 8) | b;
}

/* ================================================================
 * Render: fast path (static mesh, fully inside frustum)
 * ================================================================ */

/* The lit copy of the loop below. The shading goes between the matrix multiply
 * and the divide that wants its result, which is where the unlit loop stalls,
 * so this one has no software pipeline to keep it busy. */
static void render_fast_lit(const DMSVertex* src, int count,
                            pvr_dr_state_t* dr) {
    SHZ_PREFETCH(&src[0]);
    SHZ_PREFETCH(&src[1]);

    for (int i = 0; i < count; i++) {
        SHZ_PREFETCH(&src[i + 2]);

        shz_vec4_t t = shz_xmtrx_transform_vec4(
            shz_vec4_init(src[i].x, src[i].y, -src[i].z, 1.0f)
        );
        uint32_t argb = shade(&src[i]);
        t = shz_vec4_swizzle(t, 1, 2, 3, 0);

        float inv_w = shz_invf_fsrra(t.w);
        pvr_vertex_t* pv = pvr_dr_target(*dr);
        pv->flags = src[i].flags;
        pv->x     = t.x * inv_w;
        pv->y     = t.y * inv_w;
        pv->z     = inv_w;
        pv->u     = src[i].u;
        pv->v     = src[i].v;
        pv->argb  = argb;
        pvr_dr_commit(pv);
    }
}

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

/* The same, in black. It is how the bloom pass stops a lamp shining through
 * the wall in front of it: everything that is not a lamp is drawn into the
 * small picture too, in black, so it takes the depth test and leaves nothing
 * behind. The mesh keeps its own header, so a cutout still cuts out and a
 * texture still multiplies -- by black, which is black. */
static void render_fast_flat(const DMSVertex* src, int count,
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

    for (int i = 1; i < count; i++) {
        SHZ_PREFETCH(&src[i + 4]);

        float    nx     = src[i].x;
        float    ny     = src[i].y;
        float    nz     = -src[i].z;
        uint32_t nflags = src[i].flags;
        float    nu     = src[i].u;
        float    nv     = src[i].v;

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
        pv->argb  = 0xFF000000u;
        pvr_dr_commit(pv);

        next_t = shz_vec4_swizzle(next_t, 1, 2, 3, 0);
        cur_invw  = shz_invf_fsrra(next_t.w);
        cur_sx    = next_t.x * cur_invw;
        cur_sy    = next_t.y * cur_invw;
        cur_flags = nflags;
        cur_u     = nu;
        cur_v     = nv;
    }

    pvr_vertex_t* pv = pvr_dr_target(*dr);
    pv->flags = cur_flags;
    pv->x     = cur_sx;
    pv->y     = cur_sy;
    pv->z     = cur_invw;
    pv->u     = cur_u;
    pv->v     = cur_v;
    pv->argb  = 0xFF000000u;
    pvr_dr_commit(pv);
}

/* ================================================================
 * Render: clipped path (near-plane intersection)
 * ================================================================ */

static void render_clipped(const DMSVertex* src, int count,
                           pvr_dr_state_t* dr, int lit) {
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
        g_clip_buffer[i].argb = g_flat ? (src[i].argb & 0xff000000u)
                              : lit    ? shade(&src[i])
                                       : src[i].argb;
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

/* Additive drawing (dc_draw_ex .add): every mesh goes in the transparent list
 * and is added to what is behind it, so black adds nothing. The mesh header
 * is sent with its list, blend and depth write changed. */
static bool g_add;

void dc_model_set_add(bool add) {
    g_add = add;
}

static inline void send_header(pvr_dr_state_t* dr, const DMSMesh* mesh) {
    (void)dr;
    if (!g_add) {
        shz_sq_memcpy32_1_xmtrx(pvr_dr_target(*dr), &mesh->header);
        return;
    }
    alignas(32) pvr_poly_hdr_t h = mesh->header;
    h.cmd = (h.cmd & ~PVR_TA_CMD_TYPE_MASK) | (PVR_LIST_TR_POLY << PVR_TA_CMD_TYPE_SHIFT);
    h.mode1 &= ~PVR_TA_PM1_DEPTHWRITE_MASK;
    h.mode1 |= PVR_DEPTHWRITE_DISABLE << PVR_TA_PM1_DEPTHWRITE_SHIFT;
    h.mode2 &= ~(PVR_TA_PM2_SRCBLEND_MASK | PVR_TA_PM2_DSTBLEND_MASK);
    h.mode2 |= (PVR_BLEND_SRCALPHA << PVR_TA_PM2_SRCBLEND_SHIFT) |
               (PVR_BLEND_ONE << PVR_TA_PM2_DSTBLEND_SHIFT);
    shz_sq_memcpy32_1_xmtrx(pvr_dr_target(*dr), &h);
}

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

    send_header(dr, mesh);

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

/* The environment image (dc_set_environment), NULL for none. Mirrors are
 * left out of the normal draw while it is set: draw_reflections draws them. */
static const dttex_info_t* g_env;

/* The bloom pass (dc_set_bloom): only meshes that give off light are drawn,
 * so what lands in the small picture is the glow and nothing else */
static bool g_glow_only;

void dc_model_set_glow_only(bool on) { g_glow_only = on; g_flat = 0; }

static void draw_reflections(DMSModel* model, shz_vec3_t pos, float scale, float yaw,
                             const float* rot, const DCCamera* cam, int target_list);

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
    /* The block index below falls through to 2, so without this the modifier
     * lists would each get a copy of the transparent block */
    if (target_list == PVR_LIST_OP_MOD || target_list == PVR_LIST_TR_MOD) return;
    shz_sincos_t sc = shz_sincosf(yaw);
    float yaw_cols[9] = { sc.cos, 0.0f, sc.sin,  0.0f, 1.0f, 0.0f,  -sc.sin, 0.0f, sc.cos };
    light_to_model(pos, scale, rot ? rot : yaw_cols);
    /* The glow pass draws what a mesh gives off, which a light cannot change */
    if (g_glow_only) g_lit = 0;
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
    uint32_t skip = DMS_MAT_COLLISION_ONLY | DMS_MAT_MARKER |
                    (g_env ? DMS_MAT_MIRROR : 0);

    uint32_t run_first = model->list_runs[list], run_last = model->list_runs[list + 1];
    if (g_add) {
        /* All of it, in the transparent list */
        if (target_list != PVR_LIST_TR_POLY) return;
        run_first = model->list_runs[0];
        run_last = model->list_runs[3];
    }

    for (uint32_t r = run_first; r < run_last; r++) {
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
                if (mesh->material_flags & skip) continue;
                if (model->vol_on == m + 1) continue;   /* drawn by the volume path */
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
                    send_header(dr, mesh);
                    last_hdr = &mesh->header;
                    xm = XM_OTHER;
                }

                if (xm != XM_MVP) {
                    shz_xmtrx_load_4x4(&mvp);
                    xm = XM_MVP;
                }
                /* In the glow pass everything that is not a lamp is still
                 * drawn, in black, so it blocks what is behind it */
                g_flat = g_glow_only && !(mesh->material_flags & DMS_MAT_GLOW);
                if (batch[i] & 0x80000000u) {
                    g_stats.verts_clipped += mesh->vertex_count;
                    render_clipped(mesh->vertices, mesh->vertex_count, dr, g_lit);
                } else {
                    g_stats.verts_xformed += mesh->vertex_count;
                    if (g_flat)     render_fast_flat(mesh->vertices, mesh->vertex_count, dr);
                    else if (g_lit) render_fast_lit(mesh->vertices, mesh->vertex_count, dr);
                    else            render_fast(mesh->vertices, mesh->vertex_count, dr);
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
        if (g_add ? target_list != PVR_LIST_TR_POLY : pvr_list != target_list) continue;
        if (mesh->material_flags & (DMS_MAT_COLLISION_ONLY | DMS_MAT_MARKER)) continue;
        /* The skinned path writes its own vertices and has no black mode, so
         * in the glow pass a skinned model puts its lamps in but does not
         * block anything: it is left out rather than drawn in its own colours */
        if (g_glow_only && !(mesh->material_flags & DMS_MAT_GLOW)) continue;
        if (model->vol_on == m + 1) continue;   /* drawn by the volume path */

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

    if (model->skeleton)
        draw_skinned_list(model, pos, scale, yaw, cam, target_list);
    else {
        draw_blocks_list(model, pos, scale, yaw, NULL, cam, target_list);
        if (!g_add) draw_reflections(model, pos, scale, yaw, NULL, cam, target_list);
    }
}

void dc_model_draw_list_oriented(DMSModel* model, shz_vec3_t pos, float scale,
                                 const float rot[9], const DCCamera* cam, int target_list) {
    if (!model || model->mesh_count == 0 || model->skeleton) return;

    draw_blocks_list(model, pos, scale, 0.0f, rot, cam, target_list);
    if (!g_add) draw_reflections(model, pos, scale, 0.0f, rot, cam, target_list);
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
 * Reflections
 *
 * Metallic meshes reflect the environment image. The picture is looked up
 * with the normal as the camera sees it, so it slides over the surface as
 * the model or the camera turns.
 *
 * Mirror (solid, roughness near 0): the image in place of its own texture,
 * tinted by its vertex colours. One OP pass, the same cost as drawing it plain.
 * Other solid mesh: the image is added over it, in the TR list.
 * See-through mesh: the three accumulation buffer passes of Katana's Vase
 * sample. The image goes into the PVR's second buffer, the mesh's own
 * texture cuts it out by its alpha, and the result is laid over the frame.
 * ================================================================ */

#define SHINE_ALPHA 0x80000000u   /* strength of the image over a solid mesh */

static pvr_poly_hdr_t g_env_mirror_hdr __attribute__((aligned(32)));
static pvr_poly_hdr_t g_env_shine_hdr __attribute__((aligned(32)));
static pvr_poly_hdr_t g_env_accum_hdr __attribute__((aligned(32)));
static pvr_poly_hdr_t g_env_flush_hdr __attribute__((aligned(32)));

static DMSVertex* g_env_verts;
static uint32_t   g_env_verts_size;

void dc_model_set_environment(const dttex_info_t* tex) {
    g_env = (tex && tex->ptr) ? tex : NULL;
    if (!g_env) return;

    pvr_poly_cxt_t cxt;

    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, tex->pvrformat, tex->width, tex->height,
                     tex->ptr, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&g_env_mirror_hdr, &cxt);

    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, tex->pvrformat, tex->width, tex->height,
                     tex->ptr, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
    cxt.blend.src = PVR_BLEND_SRCALPHA;
    cxt.blend.dst = PVR_BLEND_ONE;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(&g_env_shine_hdr, &cxt);

    /* Front faces only from here on */
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, tex->pvrformat, tex->width, tex->height,
                     tex->ptr, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_CCW;
    cxt.txr.env = PVR_TXRENV_REPLACE;
    cxt.blend.src = PVR_BLEND_ONE;
    cxt.blend.dst = PVR_BLEND_ZERO;
    cxt.blend.dst_enable = PVR_BLEND_ENABLE;
    pvr_poly_compile(&g_env_accum_hdr, &cxt);

    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.culling = PVR_CULLING_CCW;
    cxt.blend.src = PVR_BLEND_SRCALPHA;
    cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
    cxt.blend.src_enable = PVR_BLEND_ENABLE;
    pvr_poly_compile(&g_env_flush_hdr, &cxt);
}

/* The mesh's vertices with their UVs swapped for a lookup into the
 * environment image. right and up are the camera's axes in model space,
 * already scaled so a full-length normal gives 0.5. */
static const DMSVertex* env_vertices(const DMSMesh* mesh, const float* right,
                                     const float* up, uint32_t argb_and, uint32_t argb_or) {
    if (mesh->vertex_count > g_env_verts_size) {
        free(g_env_verts);
        g_env_verts = memalign(32, mesh->vertex_count * sizeof(DMSVertex));
        g_env_verts_size = g_env_verts ? mesh->vertex_count : 0;
        if (!g_env_verts) return NULL;
    }
    const DMSVertex* src = mesh->vertices;
    DMSVertex* dst = g_env_verts;
    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
        SHZ_PREFETCH(&src[i + 4]);
        float nx = src[i].nx, ny = src[i].ny, nz = src[i].nz;
        dst[i].x = src[i].x;
        dst[i].y = src[i].y;
        dst[i].z = src[i].z;
        dst[i].u = 0.5f + (nx * right[0] + ny * right[1] + nz * right[2]);
        dst[i].v = 0.5f - (nx * up[0] + ny * up[1] + nz * up[2]);
        dst[i].argb = (src[i].argb & argb_and) | argb_or;
        dst[i].flags = src[i].flags;
    }
    return dst;
}

static void draw_reflections(DMSModel* model, shz_vec3_t pos, float scale, float yaw,
                             const float* rot, const DCCamera* cam, int target_list) {
    if (!g_env || !model->metallic_count) return;
    if (g_glow_only) return;   /* a reflection is caught light, not given off */
    g_lit = 0;   /* the reflection passes carry their own colour */
    if (target_list == PVR_LIST_PT_POLY) return;
    /* Same fallthrough as draw_blocks_list */
    if (target_list == PVR_LIST_OP_MOD || target_list == PVR_LIST_TR_MOD) return;
    int want_mirror = target_list == PVR_LIST_OP_POLY;
    if (want_mirror && !model->mirror_count) return;

    shz_sincos_t sc = shz_sincosf(yaw);
    float yaw_rot[9] = { sc.cos, 0.0f, sc.sin,  0.0f, 1.0f, 0.0f,  -sc.sin, 0.0f, sc.cos };
    const float* cols = rot ? rot : yaw_rot;

    /* Camera right and up in the world, the same way the frustum is turned,
     * then into model space */
    shz_xmtrx_init_identity();
    shz_xmtrx_apply_rotation_y(-cam->yaw);
    shz_xmtrx_apply_rotation_x(-cam->pitch);
    shz_vec4_t wr = shz_xmtrx_transform_vec4(shz_vec4_init(1.0f, 0.0f, 0.0f, 0.0f));
    shz_vec4_t wu = shz_xmtrx_transform_vec4(shz_vec4_init(0.0f, 1.0f, 0.0f, 0.0f));
    wr.z = -wr.z;
    wu.z = -wu.z;
    const float k = 0.5f / 127.0f;
    float right[3], up[3];
    for (int j = 0; j < 3; j++) {
        right[j] = (cols[j*3] * wr.x + cols[j*3+1] * wr.y + cols[j*3+2] * wr.z) * k;
        up[j]    = (cols[j*3] * wu.x + cols[j*3+1] * wu.y + cols[j*3+2] * wu.z) * k;
    }

    const shz_mat4x4_t* pv = dc_camera_get_pv(cam);
    const WorldFrustum* fr = dc_camera_get_frustum(cam);
    alignas(32) shz_mat4x4_t mvp;
    shz_xmtrx_load_4x4((shz_mat4x4_t*)pv);
    shz_xmtrx_translate(pos.x - cam->pos.x, pos.y - cam->pos.y, -(pos.z - cam->pos.z));
    if (rot) {
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

    pvr_dr_state_t* dr = NULL;

    for (uint32_t m = 0; m < model->mesh_count; m++) {
        DMSMesh* mesh = &model->meshes[m];
        if (!(mesh->material_flags & DMS_MAT_METALLIC)) continue;
        if (mesh->material_flags & DMS_MAT_COLLISION_ONLY) continue;
        int mirror = (mesh->material_flags & DMS_MAT_MIRROR) != 0;
        if (mirror != want_mirror) continue;

        shz_vec3_t tc = turn_centre(rot, yaw, sc, mesh->bound_cx, mesh->bound_cy, mesh->bound_cz);
        shz_vec3_t mc = shz_vec3_init(pos.x + tc.x * scale, pos.y + tc.y * scale,
                                      pos.z + tc.z * scale);
        float mr = mesh->bound_radius * scale;
        float nd;
        shz_xmtrx_load_4x4((shz_mat4x4_t*)&fr->side_planes);
        if (!sphere_visible(fr, mc, mr, &nd)) continue;
        int clip = nd < mr && nd > -mr;

        /* See-through with a texture of its own: Katana's passes. Anything
         * else gets the image added over it. */
        int tid = mesh->texture_id;
        int glass = (mesh->material_flags & 0x3) == 2 && tid >= 0 &&
                    tid < model->texture_count && model->textures[tid].ptr;

        const DMSVertex* env = mirror ? env_vertices(mesh, right, up, 0xFFFFFFFFu, 0)
                             : glass  ? env_vertices(mesh, right, up, 0, 0xFFFFFFFFu)
                                      : env_vertices(mesh, right, up, 0x00FFFFFFu, SHINE_ALPHA);
        if (!env) return;

        pvr_poly_hdr_t cut_hdr __attribute__((aligned(32)));
        const pvr_poly_hdr_t* hdrs[3] = { mirror ? &g_env_mirror_hdr : &g_env_shine_hdr, NULL, NULL };
        const DMSVertex*      vtx[3]  = { env, mesh->vertices, mesh->vertices };
        int passes = 1;
        if (mirror) {
            g_stats.meshes_drawn++;
        } else if (glass) {
            const dttex_info_t* tex = &model->textures[tid];
            pvr_poly_cxt_t cxt;
            pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, tex->pvrformat, tex->width, tex->height,
                             tex->ptr, ((mesh->material_flags >> 9) & 1) ? PVR_FILTER_NONE
                                                                        : PVR_FILTER_BILINEAR);
            cxt.gen.culling = PVR_CULLING_CCW;
            cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
            cxt.blend.src = PVR_BLEND_DESTALPHA;
            cxt.blend.dst = PVR_BLEND_SRCALPHA;
            cxt.blend.dst_enable = PVR_BLEND_ENABLE;
            pvr_poly_compile(&cut_hdr, &cxt);
            hdrs[0] = &g_env_accum_hdr;
            hdrs[1] = &cut_hdr;
            hdrs[2] = &g_env_flush_hdr;
            passes = 3;
        }

        for (int p = 0; p < passes; p++) {
            if (vtxbuf_full(mesh, clip)) return;
            if (!dr) {
                dc_list_begin(target_list);
                dr = dc_dr_state();
            }
            shz_sq_memcpy32_1_xmtrx(pvr_dr_target(*dr), hdrs[p]);
            shz_xmtrx_load_4x4(&mvp);
            g_stats.tris_drawn += mesh->tri_count;
            if (clip) {
                g_stats.verts_clipped += mesh->vertex_count;
                render_clipped(vtx[p], mesh->vertex_count, dr, 0);
            } else {
                g_stats.verts_xformed += mesh->vertex_count;
                render_fast(vtx[p], mesh->vertex_count, dr);
            }
        }
    }
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

    dc_prof_begin(DC_PROF_ANIM);
    update_skeleton(model->skeleton, dt);
    update_bounds_from_skeleton(model);
    dc_prof_end(DC_PROF_ANIM);
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

    /* Clip buffer (shared, grows to the largest mesh ever loaded). Animated
     * models have no clip path, but the volume paths transform through this
     * too, so they need one: without it they wrote every vertex to address 0. */
    if (max_verts > g_clip_buffer_size) {
        ClipVertex* grown = memalign(32, max_verts * sizeof(ClipVertex));
        if (grown) {
            free(g_clip_buffer);
            g_clip_buffer = grown;
            g_clip_buffer_size = max_verts;
        }
    }

    /* ---- Embedded textures & PVR headers ---- */
    uint32_t tex_count;
    fread(&tex_count, 4, 1, f);
    printf("DMS: %lu embedded textures\n", (unsigned long)tex_count);

    model->texture_count = tex_count;
    long tex_end = ftell(f) + (long)tex_count * 8;   /* end of the texture data */
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
            if ((long)(tex_table[i].offset + tex_table[i].size) > tex_end)
                tex_end = (long)(tex_table[i].offset + tex_table[i].size);
            free(buf);

            printf("  Tex %lu: %ux%u, %lu bytes\n",
                   (unsigned long)i,
                   model->textures[i].width,
                   model->textures[i].height,
                   (unsigned long)tex_table[i].size);
        }
        free(tex_table);
    }

    /* Material names follow the last texture */
    char tag[4];
    if (fseek(f, tex_end, SEEK_SET) == 0 &&
        fread(tag, 1, 4, f) == 4 && !memcmp(tag, "MATN", 4)) {
        model->material_names = malloc(mesh_count * 32);
        if (model->material_names &&
            fread(model->material_names, 32, mesh_count, f) != mesh_count) {
            free(model->material_names);
            model->material_names = NULL;
        }
    }

    /* Compile PVR headers using material_flags */
    for (uint32_t m = 0; m < mesh_count; m++) {
        DMSMesh* mesh = &model->meshes[m];
        int tid = mesh->texture_id;

        if (tid >= 0 && tid < (int)tex_count && model->textures[tid].ptr) {
            dttex_info_t* tex = &model->textures[tid];
            dc_model_compile_header(mesh, &mesh->header, tex->pvrformat,
                                    tex->width, tex->height, tex->ptr);
        } else {
            dc_model_compile_header(mesh, &mesh->header, 0, 0, 0, NULL);
        }

        if ((mesh->material_flags & DMS_MAT_METALLIC) && !model->skeleton &&
            !(mesh->material_flags & DMS_MAT_COLLISION_ONLY)) {
            model->metallic_count++;
            if (mesh->material_flags & DMS_MAT_MIRROR) model->mirror_count++;
        }
        if ((mesh->material_flags & DMS_MAT_GLOW) &&
            !(mesh->material_flags & DMS_MAT_COLLISION_ONLY))
            model->glow_count++;
    }

    fclose(f);
    return model;
}

/* One line per mesh, with the material name the .glb called it -- the name
 * everything else in the engine is asked for by. */
void dc_model_materials(const DMSModel* model) {
    if (!model) return;

    printf("DMS: %lu meshes, %d textures, %s\n",
           (unsigned long)model->mesh_count, model->texture_count,
           model->skeleton ? "animated" : "static");
    printf("DMS: %lu opaque, %lu cutout, %lu transparent\n",
           (unsigned long)model->opaque_count,
           (unsigned long)model->cutout_count,
           (unsigned long)model->transparent_count);

    for (uint32_t m = 0; m < model->mesh_count; m++) {
        const DMSMesh* mesh = &model->meshes[m];
        uint32_t mode = mesh->material_flags & 0x3;
        printf("  [%2lu] %-24s %6lu verts %5lu tris  tex %2ld  %s%s%s%s%s\n",
               (unsigned long)m,
               model->material_names ? model->material_names[m] : "(no name)",
               (unsigned long)mesh->vertex_count,
               (unsigned long)mesh->tri_count,
               (long)mesh->texture_id,
               mode == 0 ? "opaque" : mode == 1 ? "cutout" : "transparent",
               (mesh->material_flags & DMS_MAT_COLLISION_ONLY) ? " collision-only" : "",
               (mesh->material_flags & DMS_MAT_MIRROR)   ? " mirror"   : "",
               (mesh->material_flags & DMS_MAT_METALLIC) ? " metallic" : "",
               (mesh->material_flags & DMS_MAT_GLOW)     ? " glow"     : "");
    }
}

/* Alpha mode belongs in Blender; this is for a material that has to change
 * afterwards -- a volume's shape, say, which you want to see as well as cut
 * with. Animated only: a static model's meshes are sorted into blocks by alpha
 * mode at load, so moving one between lists breaks the run table. */
int dc_model_see_through(DMSModel* model, const char* material, uint8_t alpha) {
    if (!model || !model->material_names || !material) return 0;
    if (!model->skeleton) {
        printf("DMS: see-through: '%s' is in a static model, which sorts its"
               " meshes by alpha mode at load -- set it in Blender\n", material);
        return 0;
    }

    int changed = 0;
    for (uint32_t m = 0; m < model->mesh_count; m++) {
        if (strcasecmp(model->material_names[m], material)) continue;
        DMSMesh* mesh = &model->meshes[m];

        uint32_t was = mesh->material_flags & 0x3;
        if (was != 2) {
            if (was == 0 && model->opaque_count) model->opaque_count--;
            if (was == 1 && model->cutout_count) model->cutout_count--;
            model->transparent_count++;
            mesh->material_flags = (mesh->material_flags & ~0x3u) | 2u;
        }

        /* Translucent blending takes its alpha from the vertex colour */
        for (uint32_t v = 0; v < mesh->vertex_count; v++)
            mesh->vertices[v].argb = (mesh->vertices[v].argb & 0x00FFFFFFu) |
                                     ((uint32_t)alpha << 24);

        int tid = mesh->texture_id;
        if (tid >= 0 && tid < model->texture_count && model->textures[tid].ptr) {
            const dttex_info_t* t = &model->textures[tid];
            dc_model_compile_header(mesh, &mesh->header, t->pvrformat,
                                    t->width, t->height, t->ptr);
        } else {
            dc_model_compile_header(mesh, &mesh->header, 0, 0, 0, NULL);
        }
        changed++;
    }

    if (!changed)
        printf("DMS: see-through: no mesh wears the material '%s'\n", material);
    return changed;
}

void dc_model_submit_quads(const DMSVertex* verts, int quads, pvr_dr_state_t* dr) {
    if (quads <= 0) return;

    for (int q = 0; q < quads; q++) {
        const DMSVertex* src = &verts[q * 4];
        if (g_vtx_left < 4 * 32) { vtxbuf_warn(); return; }

        /* No clipping: a square with a corner behind the near plane is
         * dropped whole. One of four vertices is not worth a clip path, and
         * a particle vanishing as it reaches the camera is not seen. */
        shz_vec4_t t[4];
        float behind = 0.0f;
        for (int i = 0; i < 4; i++) {
            t[i] = shz_vec4_swizzle(shz_xmtrx_transform_vec4(
                       shz_vec4_init(src[i].x, src[i].y, -src[i].z, 1.0f)), 1, 2, 3, 0);
            if (t[i].w < NEAR_Z) behind = 1.0f;
        }
        if (behind != 0.0f) continue;

        g_vtx_left -= 4 * 32;
        for (int i = 0; i < 4; i++) {
            float inv_w = shz_invf_fsrra(t[i].w);
            pvr_vertex_t* pv = pvr_dr_target(*dr);
            pv->flags = src[i].flags;
            pv->x     = t[i].x * inv_w;
            pv->y     = t[i].y * inv_w;
            pv->z     = inv_w;
            pv->u     = src[i].u;
            pv->v     = src[i].v;
            pv->argb  = src[i].argb;
            pvr_dr_commit(pv);
        }
        g_stats.tris_drawn += 2;
        g_stats.verts_xformed += 4;
    }
}

/* ================================================================
 * Flat shadows
 *
 * The model squashed onto a floor, away from a light. The squash is a matrix
 * put between the camera and the model, so the usual vertex loops draw it and
 * nothing is copied.
 *
 * A squashed model lies over itself many times, and plain blending would
 * darken those places again and again. So it goes through the PVR's second
 * (accumulation) buffer, which is what works with a sorted transparent list:
 *   1. a white square on the floor, under the whole shadow, into the buffer
 *   2. the squashed model, one flat grey, over it (replacing, not blending)
 *   3. the square again, multiplying the screen by what the buffer holds
 * The grey comes from a tiny texture that replaces the vertex colours.
 *
 * The transparent list is sorted by depth by the PVR, far to near, so the
 * three steps are kept in order by depth alone: all three lie at the same
 * height, and each is given a depth a little nearer than the one before
 * (SHADOW_SORT_GAP). For that the shadow's depth must be exact,
 * which a squash away from a point cannot give (it needs a divide the vertex
 * loops do not do). So a light is treated as a far away one shining from
 * where it is towards the model, and the spread it would give is put back as
 * a plain scale about the middle of the shadow.
 *
 * All three must pass the depth test against the floor: a sorted list tests
 * "nearer or equal" whatever the header asks for. Where the first square
 * loses to the floor and the last does not, the floor is multiplied by
 * whatever the buffer held (the square shimmers). So even the first is nearer
 * than the floor by a whole gap.
 * ================================================================ */

#define SHADOW_TEX_SIZE 8
#define SHADOW_LIFT     0.01f   /* of the model's radius, above the floor (z fighting) */
#define SHADOW_SORT_GAP 0.01f   /* of the depth: floor, square, shadow, square */
#define SHADOW_MAX_SIZE 8.0f    /* the square, in model radii: a low light throws long shadows */

static pvr_ptr_t      g_shadow_tex;
static float          g_shadow_dark = -1.0f;
static pvr_poly_hdr_t g_shadow_clean_hdr __attribute__((aligned(32)));
static pvr_poly_hdr_t g_shadow_grey_hdr  __attribute__((aligned(32)));
static pvr_poly_hdr_t g_shadow_flush_hdr __attribute__((aligned(32)));

static bool shadow_setup(float dark) {
    if (!g_shadow_tex) {
        g_shadow_tex = pvr_mem_malloc(SHADOW_TEX_SIZE * SHADOW_TEX_SIZE * 2);
        if (!g_shadow_tex) return false;

        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.blend.src = PVR_BLEND_ONE;
        cxt.blend.dst = PVR_BLEND_ZERO;
        cxt.blend.dst_enable = PVR_BLEND_ENABLE;
        cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
        pvr_poly_compile(&g_shadow_clean_hdr, &cxt);

        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                         SHADOW_TEX_SIZE, SHADOW_TEX_SIZE, g_shadow_tex, PVR_FILTER_NONE);
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.txr.env = PVR_TXRENV_REPLACE;
        cxt.blend.src = PVR_BLEND_ONE;
        cxt.blend.dst = PVR_BLEND_ZERO;
        cxt.blend.dst_enable = PVR_BLEND_ENABLE;
        cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
        pvr_poly_compile(&g_shadow_grey_hdr, &cxt);

        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.blend.src = PVR_BLEND_DESTCOLOR;
        cxt.blend.dst = PVR_BLEND_ZERO;
        cxt.blend.src_enable = PVR_BLEND_ENABLE;
        pvr_poly_compile(&g_shadow_flush_hdr, &cxt);
    }
    if (dark != g_shadow_dark) {
        /* The shadow multiplies the floor by this grey */
        uint32_t g = (uint32_t)((1.0f - dark) * 255.0f);
        uint16_t texel = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
        uint16_t* t = (uint16_t*)g_shadow_tex;
        for (int i = 0; i < SHADOW_TEX_SIZE * SHADOW_TEX_SIZE; i++) t[i] = texel;
        g_shadow_dark = dark;
    }
    return true;
}

void dc_model_draw_shadow(DMSModel* model, shz_vec3_t pos, float scale, float yaw,
                          const float* rot, const DCCamera* cam,
                          shz_vec3_t light, bool sun, float floor_y, float dark) {
    if (!model || !model->mesh_count || !cam) return;
    if (model->skeleton && rot) return;
    if (dark <= 0.0f) dark = 0.5f;
    if (dark > 1.0f) dark = 1.0f;

    /* The model's bounds in the world */
    shz_sincos_t sc = shz_sincosf(yaw);
    shz_vec3_t c;
    float r;
    if (model->skeleton) {
        c = turn_centre(NULL, yaw, sc, model->anim_bound_cx, model->anim_bound_cy,
                        model->anim_bound_cz);
        r = model->anim_bound_radius;
    } else {
        /* Around the meshes' own spheres */
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        for (uint32_t m = 0; m < model->mesh_count; m++) {
            const DMSMesh* mesh = &model->meshes[m];
            if (mesh->material_flags & DMS_MAT_COLLISION_ONLY) continue;
            float mc[3] = { mesh->bound_cx, mesh->bound_cy, mesh->bound_cz };
            for (int k = 0; k < 3; k++) {
                if (mc[k] - mesh->bound_radius < lo[k]) lo[k] = mc[k] - mesh->bound_radius;
                if (mc[k] + mesh->bound_radius > hi[k]) hi[k] = mc[k] + mesh->bound_radius;
            }
        }
        if (lo[0] > hi[0]) return;
        float dx = hi[0] - lo[0], dy = hi[1] - lo[1], dz = hi[2] - lo[2];
        c = turn_centre(rot, yaw, sc, (lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f,
                        (lo[2] + hi[2]) * 0.5f);
        r = 0.5f * shz_sqrtf_fsrra(dx * dx + dy * dy + dz * dz);
    }
    c = shz_vec3_init(pos.x + c.x * scale, pos.y + c.y * scale, pos.z + c.z * scale);
    r *= scale;

    float h = floor_y + r * SHADOW_LIFT;
    if (c.y + r <= h) return;                   /* all of it is under the floor */

    /* Where the middle of the shadow lands, and how far it spreads */
    shz_vec3_t dir = sun ? light : shz_vec3_init(c.x - light.x, c.y - light.y, c.z - light.z);
    float len = shz_sqrtf_fsrra(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len < 1e-6f || dir.y >= 0.0f) return;   /* the light is not above it */
    float t = (h - c.y) / dir.y;                /* along dir from the centre to the floor */
    shz_vec3_t mid = shz_vec3_init(c.x + dir.x * t, h, c.z + dir.z * t);
    float slant = len / -dir.y;                 /* 1 straight down, more for a low light */
    float spread = sun ? 1.0f : (len + t * len) / len;
    if (spread > 3.0f) spread = 3.0f;
    float half = r * slant * spread;
    if (half > r * SHADOW_MAX_SIZE) half = r * SHADOW_MAX_SIZE;

    /* Skinned models have no clip path: their shadow must be clear of the
     * near plane */
    if (model->skeleton) {
        if (dc_frustum_near_intersect(cam, mid, half * 1.5f)) return;
    }
    if (dc_frustum_cull_sphere(cam, mid, half * 1.5f) < 0) return;

    if (!shadow_setup(dark)) return;

    /* The squash, in the space right after the camera matrix: relative to
     * the camera, z negated. Along d onto the plane y = hc:
     *   v' = v - d (v.y - hc) / d.y
     * then x and z spread about the middle of the shadow by g. */
    float d[3] = { dir.x / len, dir.y / len, -dir.z / len };
    float hc = h - cam->pos.y;
    float mx = mid.x - cam->pos.x, mz = -(mid.z - cam->pos.z);
    float g = spread > 3.0f ? 3.0f : spread;
    alignas(32) shz_mat4x4_t squash;
    memset(&squash, 0, sizeof(squash));
    squash.elem2D[0][0] = g;
    squash.elem2D[2][2] = g;
    squash.elem2D[3][3] = 1.0f;
    squash.elem2D[1][0] = -g * d[0] / d[1];
    squash.elem2D[1][2] = -g * d[2] / d[1];
    squash.elem2D[3][0] = g * d[0] * hc / d[1] + mx * (1.0f - g);
    squash.elem2D[3][1] = hc;
    squash.elem2D[3][2] = g * d[2] * hc / d[1] + mz * (1.0f - g);

    dc_list_begin(PVR_LIST_TR_POLY);
    pvr_dr_state_t* dr = dc_dr_state();

    /* The square on the floor, in world coordinates. The whole matrix times k
     * leaves it where it is on the screen and makes its depth k times as far. */
    alignas(32) DMSVertex quad[4];
    static const float corner[4][2] = { {-1, -1}, {1, -1}, {-1, 1}, {1, 1} };
    for (int i = 0; i < 4; i++) {
        memset(&quad[i], 0, sizeof(quad[i]));
        quad[i].x = mid.x + corner[i][0] * half;
        quad[i].y = h;
        quad[i].z = mid.z + corner[i][1] * half;
        quad[i].argb = 0xFFFFFFFFu;
        quad[i].flags = (i == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    }
    alignas(32) shz_mat4x4_t world_mvp;
    shz_xmtrx_load_4x4((shz_mat4x4_t*)dc_camera_get_pv(cam));
    shz_xmtrx_translate(-cam->pos.x, -cam->pos.y, cam->pos.z);
    shz_xmtrx_store_4x4(&world_mvp);
    alignas(32) shz_mat4x4_t clean_mvp, flush_mvp;
    for (int i = 0; i < 16; i++) {
        clean_mvp.elem[i] = world_mvp.elem[i] * (1.0f - SHADOW_SORT_GAP);
        flush_mvp.elem[i] = world_mvp.elem[i] * (1.0f - 3.0f * SHADOW_SORT_GAP);
    }

    /* 1. clean the buffer under the shadow */
    shz_sq_memcpy32_1_xmtrx(pvr_dr_target(*dr), &g_shadow_clean_hdr);
    shz_xmtrx_load_4x4(&clean_mvp);
    render_clipped(quad, 4, dr, 0);

    /* 2. the model, squashed, in grey */
    shz_sq_memcpy32_1_xmtrx(pvr_dr_target(*dr), &g_shadow_grey_hdr);
    shz_xmtrx_load_4x4((shz_mat4x4_t*)dc_camera_get_pv(cam));
    shz_xmtrx_apply_4x4(&squash);
    shz_xmtrx_translate(pos.x - cam->pos.x, pos.y - cam->pos.y, -(pos.z - cam->pos.z));
    if (rot) {
        alignas(32) shz_mat4x4_t rm;
        rm.elem2D[0][0] =  rot[0]; rm.elem2D[0][1] =  rot[1]; rm.elem2D[0][2] = -rot[2]; rm.elem2D[0][3] = 0.0f;
        rm.elem2D[1][0] =  rot[3]; rm.elem2D[1][1] =  rot[4]; rm.elem2D[1][2] = -rot[5]; rm.elem2D[1][3] = 0.0f;
        rm.elem2D[2][0] = -rot[6]; rm.elem2D[2][1] = -rot[7]; rm.elem2D[2][2] =  rot[8]; rm.elem2D[2][3] = 0.0f;
        rm.elem2D[3][0] = 0.0f;    rm.elem2D[3][1] = 0.0f;    rm.elem2D[3][2] = 0.0f;    rm.elem2D[3][3] = 1.0f;
        shz_xmtrx_apply_4x4(&rm);
    } else if (yaw != 0.0f) {
        shz_xmtrx_apply_rotation_y(yaw);
    }
    /* Skinned vertices go in as they are, so z is negated here */
    shz_xmtrx_apply_scale(scale, scale, model->skeleton ? -scale : scale);
    alignas(32) shz_mat4x4_t mvp;
    shz_xmtrx_store_4x4(&mvp);
    for (int i = 0; i < 16; i++) mvp.elem[i] *= 1.0f - 2.0f * SHADOW_SORT_GAP;

    for (uint32_t m = 0; m < model->mesh_count; m++) {
        DMSMesh* mesh = &model->meshes[m];
        if (mesh->material_flags & DMS_MAT_COLLISION_ONLY) continue;
        if (vtxbuf_full(mesh, !model->skeleton)) break;
        g_stats.tris_drawn += mesh->tri_count;
        if (model->skeleton) {
            g_stats.verts_xformed += mesh->vertex_count;
            render_skinned(mesh->vertices, mesh->vertex_count, model->skeleton, &mvp, dr);
        } else {
            g_stats.verts_clipped += mesh->vertex_count;
            shz_xmtrx_load_4x4(&mvp);
            render_clipped(mesh->vertices, mesh->vertex_count, dr, 0);
        }
    }

    /* 3. the buffer onto the screen */
    shz_sq_memcpy32_1_xmtrx(pvr_dr_target(*dr), &g_shadow_flush_hdr);
    shz_xmtrx_load_4x4(&flush_mvp);
    render_clipped(quad, 4, dr, 0);
}

/* ================================================================
 * Modifier volumes
 *
 * One mesh of a model is submitted to the PVR's modifier list as a shape.
 * Every pixel of another mesh then knows whether it is inside it, and takes
 * one of two sets of texture, colour and blending -- the x-ray window. All
 * three meshes are named by their Blender material, out of the one glb.
 *
 *     dc_model_volume(dino, "Scanner", "Dinosaur", "Bones");
 *     dc_draw(dino, pos);     // as usual, nothing else to say
 *
 * Two things go out each frame:
 *   1. the mesh drawn with a two-parameter header and 64-byte vertices
 *      (pvr_vertex_tpcm_t), which carry both sets of texture coordinates and
 *      colours. The same coordinates are used for both, so the second picture
 *      lines up with the first with nothing changed in the model.
 *   2. the shape, as single triangles (not strips) in the modifier list, the
 *      last one told to close the volume.
 *
 * The shape is still drawn normally too; only the mesh it works on is taken
 * off the normal path. A strip crossing the near plane is dropped whole rather
 * than clipped -- the clip path would have to carry both parameter sets.
 * ================================================================ */

/* What is left of the mesh inside the shape. dc_model_volume_inside() changes it */
static uint32_t g_vol_inside_argb = 0x40C8E6FFu;

void dc_model_volume_inside(DMSModel* model, uint8_t alpha, uint32_t rgb) {
    (void)model;   /* one wash for every volume; there is only ever one */
    g_vol_inside_argb = ((uint32_t)alpha << 24) | (rgb & 0x00FFFFFFu);
}

/* -DDMS_VOLUME_DEBUG=1: a line a second saying what reached the hardware and
 * where it landed. "Nothing sent" and "sent but invisible" look the same. */
#ifndef DMS_VOLUME_DEBUG
#define DMS_VOLUME_DEBUG 0
#endif

#if DMS_VOLUME_DEBUG
typedef struct {
    uint32_t considered, sent;          /* triangles, or strips */
    float    x0, y0, x1, y1;            /* screen box of what was sent */
    float    znear, zfar;               /* 1/w, so big is near */
} VolCount;

static VolCount g_vol_shape_c, g_vol_on_c;

static inline void volc_start(VolCount* c) {
    c->considered = c->sent = 0;
    c->x0 = c->y0 =  1.0e30f;
    c->x1 = c->y1 = -1.0e30f;
    c->znear = -1.0e30f;
    c->zfar  =  1.0e30f;
}

static inline void volc_point(VolCount* c, float x, float y, float z) {
    if (x < c->x0) c->x0 = x;
    if (x > c->x1) c->x1 = x;
    if (y < c->y0) c->y0 = y;
    if (y > c->y1) c->y1 = y;
    if (z > c->znear) c->znear = z;
    if (z < c->zfar)  c->zfar  = z;
}

static inline void volc_say(void) {
    static uint64_t last;
    uint64_t now = timer_ms_gettime64();
    if (now - last < 1000) return;
    last = now;
    printf("VOL shape %lu/%lu tris  x %.0f..%.0f y %.0f..%.0f z %.4f..%.4f\n",
           (unsigned long)g_vol_shape_c.sent, (unsigned long)g_vol_shape_c.considered,
           g_vol_shape_c.x0, g_vol_shape_c.x1, g_vol_shape_c.y0, g_vol_shape_c.y1,
           g_vol_shape_c.zfar, g_vol_shape_c.znear);
    printf("VOL   on  %lu/%lu strips  x %.0f..%.0f y %.0f..%.0f z %.4f..%.4f\n",
           (unsigned long)g_vol_on_c.sent, (unsigned long)g_vol_on_c.considered,
           g_vol_on_c.x0, g_vol_on_c.x1, g_vol_on_c.y0, g_vol_on_c.y1,
           g_vol_on_c.zfar, g_vol_on_c.znear);
}
#endif

/* The mesh whose glTF material has this name. A shared material names none of
 * them, and says so: silently taking the first looks like a broken volume. */
static int volume_find(const DMSModel* model, const char* name, const char* role) {
    if (!model->material_names || !name) return -1;

    int found = -1, count = 0;
    for (uint32_t m = 0; m < model->mesh_count; m++) {
        if (strcasecmp(model->material_names[m], name)) continue;
        if (found < 0) found = (int)m;
        count++;
    }
    if (count > 1) {
        printf("DMS: volume: %d meshes share the material '%s', so it does not"
               " say which one is the %s:\n", count, name, role);
        for (uint32_t m = 0; m < model->mesh_count; m++)
            if (!strcasecmp(model->material_names[m], name))
                printf("      mesh %lu, %lu verts, %lu tris\n",
                       (unsigned long)m,
                       (unsigned long)model->meshes[m].vertex_count,
                       (unsigned long)model->meshes[m].tri_count);
        printf("      give the one you mean its own material in Blender\n");
    }
    return found;
}

bool dc_model_volume(DMSModel* model, const char* shape, const char* on,
                     const char* shows) {
    if (!model) return false;
    /* pvr_init() sized the tile bins long ago, so this cannot be turned on
     * from here; without it the volume goes into a list with nowhere to put it */
    if (!dc_volumes_enabled()) {
        printf("DMS: volume: this needs the translucent modifier list, which is\n"
               "      off by default because it costs about 525KB of texture RAM.\n"
               "      Ask for it at startup:  dc_init((DCInitParams){ ..., .volumes = true });\n");
        return false;
    }
    int s = volume_find(model, shape, "shape");
    int o = volume_find(model, on, "mesh it shows through");
    int p = volume_find(model, shows, "picture it shows");
    if (s < 0 || o < 0 || p < 0) {
        printf("DMS: volume needs materials '%s', '%s' and '%s' (%d %d %d)\n",
               shape ? shape : "?", on ? on : "?", shows ? shows : "?", s, o, p);
        return false;
    }
    int tex = model->meshes[p].texture_id;
    if (tex < 0 || tex >= model->texture_count) {
        printf("DMS: volume: material '%s' has no picture\n", shows);
        return false;
    }
    model->vol_shape  = (uint32_t)s + 1;
    model->vol_on     = (uint32_t)o + 1;
    model->vol_inside = (uint32_t)tex + 1;
    free(model->mod_headers);
    model->mod_headers = NULL;

    printf("DMS: volume: shape mesh %d '%s' (%lu tris), through mesh %d '%s',"
           " showing texture %d\n",
           s, shape, (unsigned long)model->meshes[s].tri_count, o, on, tex);
    return true;
}

/* A two-parameter vertex is 64 bytes: two goes at the store queue */
#define MOD_VTX_BYTES 64

/* Two-parameter header: the mesh's own picture both times, opaque outside the
 * shape and blended inside. Kept until dc_model_volume() changes the volume. */
static bool mod_header_build(DMSModel* model) {
    if (model->mod_headers) return true;
    model->mod_headers = memalign(32, sizeof(pvr_poly_hdr_t));
    if (!model->mod_headers) return false;

    const DMSMesh* mesh = &model->meshes[model->vol_on - 1];
    const dttex_info_t* own =
        (mesh->texture_id >= 0 && mesh->texture_id < model->texture_count)
            ? &model->textures[mesh->texture_id] : NULL;
    if (!own && model->vol_inside &&
        (int)model->vol_inside <= model->texture_count)
        own = &model->textures[model->vol_inside - 1];
    if (!own) return false;

    /* Both sets read the same picture on the same UVs; what changes inside the
     * shape is how much of it reaches the screen */
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr_mod(&cxt, VOL_POLY_LIST,
                         own->pvrformat, own->width, own->height, own->ptr,
                         PVR_FILTER_BILINEAR,
                         own->pvrformat, own->width, own->height, own->ptr,
                         PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE;

    /* Outside: covers what is behind it, exactly as the opaque mesh did */
    cxt.blend.src  = PVR_BLEND_ONE;
    cxt.blend.dst  = PVR_BLEND_ZERO;
    cxt.txr.env    = PVR_TXRENV_MODULATE;
    cxt.gen.alpha  = PVR_ALPHA_DISABLE;

    /* Inside: blended by the vertex alpha, so what is behind shows through */
    cxt.blend.src2 = PVR_BLEND_SRCALPHA;
    cxt.blend.dst2 = PVR_BLEND_INVSRCALPHA;
    cxt.txr2.env   = PVR_TXRENV_MODULATEALPHA;
    cxt.gen.alpha2 = PVR_ALPHA_ENABLE;

    pvr_poly_mod_compile((pvr_poly_hdr_t*)model->mod_headers, &cxt);
    return true;
}

/* The mesh the volume works on, both parameter sets. Always TR, whatever it
 * was made as, so it can be seen through inside the shape. */
void dc_model_draw_modified(DMSModel* model, shz_vec3_t pos, float scale, float yaw,
                            const DCCamera* cam) {
    if (!model || !model->vol_on || !cam) return;
    if (!mod_header_build(model)) return;

    DMSMesh* mesh = &model->meshes[model->vol_on - 1];
    if (mesh->material_flags & DMS_MAT_COLLISION_ONLY) return;

    dc_list_begin(VOL_POLY_LIST);
    pvr_dr_state_t* dr = dc_dr_state();
    (void)dr;   /* this KOS's pvr_dr_target() does not use it */

    const DMSSkeleton* sk = model->skeleton;
    float zs = sk ? -scale : scale;      /* animated: Z negation baked in */

    alignas(32) shz_mat4x4_t mvp;
    shz_xmtrx_load_4x4((shz_mat4x4_t*)dc_camera_get_pv(cam));
    shz_xmtrx_translate(pos.x - cam->pos.x, pos.y - cam->pos.y, -(pos.z - cam->pos.z));
    if (yaw != 0.0f) shz_xmtrx_apply_rotation_y(yaw);
    shz_xmtrx_apply_scale(scale, scale, zs);
    shz_xmtrx_store_4x4(&mvp);

    /* Two words a vertex, so twice the room of a normal mesh */
    if (g_vtx_left < (int32_t)(mesh->vertex_count * MOD_VTX_BYTES)) {
        vtxbuf_warn();
        return;
    }

    g_stats.meshes_drawn++;
    shz_sq_memcpy32_1_xmtrx(pvr_dr_target(*dr), (pvr_poly_hdr_t*)model->mod_headers);

    const DMSVertex* src = mesh->vertices;
    int last_bone = -1;

#if DMS_VOLUME_DEBUG
    volc_start(&g_vol_on_c);
#endif

    /* Walk the strips: a strip that reaches the near plane is dropped */
    uint32_t i = 0;
    while (i < mesh->vertex_count) {
        uint32_t end = i;
        while (end < mesh->vertex_count && src[end].flags != PVR_CMD_VERTEX_EOL)
            end++;
        if (end < mesh->vertex_count) end++;

        bool ok = true;
        for (uint32_t k = i; k < end && ok; k++) {
            if (sk) {
                if ((int)src[k].pad != last_bone) {
                    last_bone = src[k].pad;
                    shz_xmtrx_load_apply_4x4(&mvp, &sk->bones[last_bone].skinMatrix);
                }
            } else if (k == i) {
                shz_xmtrx_load_4x4(&mvp);
            }
            shz_vec4_t t = shz_vec4_swizzle(shz_xmtrx_transform_vec4(
                shz_vec4_init(src[k].x, src[k].y, sk ? src[k].z : -src[k].z, 1.0f)),
                1, 2, 3, 0);
            if (t.w < NEAR_Z) ok = false;
            g_clip_buffer[k - i].x = t.x;
            g_clip_buffer[k - i].y = t.y;
            g_clip_buffer[k - i].w = t.w;
        }
#if DMS_VOLUME_DEBUG
        g_vol_on_c.considered++;
        if (ok) g_vol_on_c.sent++;
#endif
        if (ok) {
            g_vtx_left -= (int32_t)((end - i) * MOD_VTX_BYTES);
            g_stats.verts_xformed += end - i;
            for (uint32_t k = i; k < end; k++) {
                const ClipVertex* cv = &g_clip_buffer[k - i];
                float inv_w = shz_invf_fsrra(cv->w);
#if DMS_VOLUME_DEBUG
                volc_point(&g_vol_on_c, cv->x * inv_w, cv->y * inv_w, inv_w);
#endif
                pvr_vertex_tpcm_t* pv = (pvr_vertex_tpcm_t*)pvr_dr_target(*dr);
                pv->flags = src[k].flags;
                pv->x = cv->x * inv_w;
                pv->y = cv->y * inv_w;
                pv->z = inv_w;
                pv->u0 = src[k].u;   pv->v0 = src[k].v;
                pv->argb0 = src[k].argb;
                pv->oargb0 = 0;
                pvr_dr_commit(pv);
                /* Second half: same place, same picture, washed and mostly
                 * see-through */
                uint32_t* w = (uint32_t*)pvr_dr_target(*dr);
                ((float*)w)[0] = src[k].u;
                ((float*)w)[1] = src[k].v;
                w[2] = g_vol_inside_argb;
                w[3] = 0;
                w[4] = 0; w[5] = 0; w[6] = 0; w[7] = 0;
                pvr_dr_commit(w);
            }
            g_stats.tris_drawn += (end - i) >= 3 ? (end - i) - 2 : 0;
        }
        i = end;
    }
}

/* One modifier triangle: 64 bytes, so two goes at the store queue */
static inline void volume_tri(pvr_dr_state_t* dr, const float* p) {
    uint32_t* w = (uint32_t*)pvr_dr_target(*dr);
    w[0] = PVR_CMD_VERTEX_EOL;
    ((float*)w)[1] = p[0]; ((float*)w)[2] = p[1]; ((float*)w)[3] = p[2];
    ((float*)w)[4] = p[3]; ((float*)w)[5] = p[4]; ((float*)w)[6] = p[5];
    ((float*)w)[7] = p[6];
    pvr_dr_commit(w);

    w = (uint32_t*)pvr_dr_target(*dr);
    ((float*)w)[0] = p[7]; ((float*)w)[1] = p[8];
    w[2] = 0; w[3] = 0; w[4] = 0; w[5] = 0; w[6] = 0; w[7] = 0;
    pvr_dr_commit(w);
}

/* The shape as single triangles. The last one closes the volume -- which is
 * what makes "inside" mean anything -- so they are sent one behind. */
void dc_model_draw_volume(DMSModel* model, shz_vec3_t pos, float scale, float yaw,
                          const DCCamera* cam) {
    if (!model || !model->vol_shape || !cam) return;
    const DMSMesh* mesh = &model->meshes[model->vol_shape - 1];
    if (mesh->vertex_count < 3) return;

    dc_list_begin(VOL_MOD_LIST);
    pvr_dr_state_t* dr = dc_dr_state();

    const DMSSkeleton* sk = model->skeleton;

    alignas(32) shz_mat4x4_t mvp;
    shz_xmtrx_load_4x4((shz_mat4x4_t*)dc_camera_get_pv(cam));
    shz_xmtrx_translate(pos.x - cam->pos.x, pos.y - cam->pos.y, -(pos.z - cam->pos.z));
    if (yaw != 0.0f) shz_xmtrx_apply_rotation_y(yaw);
    shz_xmtrx_apply_scale(scale, scale, sk ? -scale : scale);
    shz_xmtrx_store_4x4(&mvp);

    if (g_vtx_left < (int32_t)(mesh->tri_count * 64)) { vtxbuf_warn(); return; }

    /* Every vertex once, then each triangle. Skinned like the normal path:
     * the shape is usually animated. */
    int last_bone = -1;
    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
        if (sk) {
            if ((int)mesh->vertices[i].pad != last_bone) {
                last_bone = mesh->vertices[i].pad;
                shz_xmtrx_load_apply_4x4(&mvp, &sk->bones[last_bone].skinMatrix);
            }
        } else if (i == 0) {
            shz_xmtrx_load_4x4(&mvp);
        }
        shz_vec4_t t = shz_vec4_swizzle(shz_xmtrx_transform_vec4(
            shz_vec4_init(mesh->vertices[i].x, mesh->vertices[i].y,
                          sk ? mesh->vertices[i].z : -mesh->vertices[i].z, 1.0f)),
            1, 2, 3, 0);
        float inv_w = t.w < NEAR_Z ? 0.0f : shz_invf_fsrra(t.w);
        g_clip_buffer[i].x = t.x * inv_w;
        g_clip_buffer[i].y = t.y * inv_w;
        g_clip_buffer[i].z = inv_w;
        g_clip_buffer[i].w = t.w;
    }
    g_stats.verts_xformed += mesh->vertex_count;

/* A triangle of the strip that is whole and in front of the near plane */
#define VOL_TRI_OK(i)                                                \
    (mesh->vertices[(i) - 2].flags != PVR_CMD_VERTEX_EOL &&          \
     mesh->vertices[(i) - 1].flags != PVR_CMD_VERTEX_EOL &&          \
     g_clip_buffer[(i) - 2].w >= NEAR_Z &&                           \
     g_clip_buffer[(i) - 1].w >= NEAR_Z &&                           \
     g_clip_buffer[(i)].w >= NEAR_Z)

    /* Counted first: the volume is closed by its last triangle, so one whose
     * triangles were all dropped must not be started at all */
    uint32_t tris = 0;
    for (uint32_t i = 2; i < mesh->vertex_count; i++)
        if (VOL_TRI_OK(i)) tris++;
#if DMS_VOLUME_DEBUG
    volc_start(&g_vol_shape_c);
    g_vol_shape_c.considered = mesh->vertex_count >= 2 ? mesh->vertex_count - 2 : 0;
    g_vol_shape_c.sent = tris;
    for (uint32_t i = 2; i < mesh->vertex_count; i++)
        if (VOL_TRI_OK(i))
            volc_point(&g_vol_shape_c, g_clip_buffer[i].x, g_clip_buffer[i].y,
                       g_clip_buffer[i].z);
    volc_say();
#endif
    if (!tris) return;

    alignas(32) pvr_poly_hdr_t hdr;
    if (tris > 1) {
        pvr_mod_compile(&hdr, VOL_MOD_LIST, PVR_MODIFIER_OTHER_POLY,
                        PVR_CULLING_NONE);
        shz_sq_memcpy32_1_xmtrx(pvr_dr_target(*dr), &hdr);
    }

    uint32_t sent = 0;
    for (uint32_t i = 2; i < mesh->vertex_count && sent < tris; i++) {
        if (!VOL_TRI_OK(i)) continue;

        /* The last one closes the volume, so it goes under its own header */
        if (sent == tris - 1) {
            pvr_mod_compile(&hdr, VOL_MOD_LIST, PVR_MODIFIER_INCLUDE_LAST_POLY,
                            PVR_CULLING_NONE);
            shz_sq_memcpy32_1_xmtrx(pvr_dr_target(*dr), &hdr);
        }

        const ClipVertex* a = &g_clip_buffer[i - 2];
        const ClipVertex* b = &g_clip_buffer[i - 1];
        const ClipVertex* c = &g_clip_buffer[i];
        float tri[9] = { a->x, a->y, a->z, b->x, b->y, b->z, c->x, c->y, c->z };
        volume_tri(dr, tri);
        g_vtx_left -= 64;
        g_stats.tris_drawn++;
        sent++;
    }
}
#undef VOL_TRI_OK

/* ================================================================
 * Mesh header
 * ================================================================ */

void dc_model_compile_header(const DMSMesh* mesh, pvr_poly_hdr_t* out, int pvrformat,
                             int width, int height, pvr_ptr_t txr) {
    pvr_poly_cxt_t cxt;

    int alpha_mode   = mesh->material_flags & 0x3;
    int tex_filter   = (mesh->material_flags >> 9) & 0x1;

    int pvr_list;
    if (alpha_mode == 0)      pvr_list = PVR_LIST_OP_POLY;
    else if (alpha_mode == 1) pvr_list = PVR_LIST_PT_POLY;
    else                      pvr_list = PVR_LIST_TR_POLY;

    if (txr)
        pvr_poly_cxt_txr(&cxt, pvr_list, pvrformat, width, height, txr,
                         tex_filter ? PVR_FILTER_NONE : PVR_FILTER_BILINEAR);
    else
        pvr_poly_cxt_col(&cxt, pvr_list);

    cxt.gen.culling = PVR_CULLING_NONE;

    if (alpha_mode == 2) {
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
    }

    pvr_poly_compile(out, &cxt);
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
    free(model->mod_headers);
    free(model->blocks);
    free(model->runs);
    free(model->material_names);

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
