#include <dc/perfctr.h>
#include "dc_model.h"
#include "dc_engine.h"
#include "pvrtex.h"
#include "dt_colours.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================
 * Module state
 * ================================================================ */

/* pvr_dr_target() writes the new store queue address back before the vertex
 * is written to it. That cached store right in front of the store queue
 * writes costs the SH4 about two cycles a vertex (the fast loop below lost
 * 5 fps at 55k vertices), so the vertex goes out first and the address is
 * written back after, with the commit. The dr argument is kept for the
 * callers; this KOS's pvr_dr_target() does not use it either. */
static inline pvr_vertex_t* dr_vertex(void) {
    return __builtin_assume_aligned((void*)(pvr_dr_addr ^ 32), 32);
}
static inline void dr_send(pvr_vertex_t* pv) {
    pvr_dr_addr = (uint32_t)pv;
    pvr_dr_commit(pv);
}

/* A word read or written through another type's pointer */
typedef uint32_t __attribute__((may_alias)) uint32_alias;

/* The hot loops keep the store queue address in a register rather than
 * pvr_dr_addr: a store to the queue could be to that global as far as the
 * compiler knows, so it loaded and stored it every vertex. sq_next() flips to
 * the other half of the queue, sq_put() writes a whole vertex there and
 * sends it, and sq_end() gives the address back.
 * The vertex is written from its last word back to its first, each store
 * moving the address down 4 (@-r), as Ninja's njDirectDraw: GCC has no
 * such store and fmov has no offset, so it built each address with a mov
 * and an add, 14 instructions a vertex. The stores are not volatile: GCC
 * holds a volatile asm as a wall nothing is moved across, and 8 a vertex
 * left the float work nowhere to go (24.2ms against 21.4ms on the Toyota).
 * They keep their order through the address each hands the next, and the
 * pref that sends them takes the last one's. */
static inline uint32_t sq_next(uint32_t sq) { return sq ^ 32; }
static inline __attribute__((always_inline)) void sq_f(uint32_t* p, float f) {
    __asm__("fmov %1,@-%0" : "+r"(*p) : "f"(f));
}
static inline __attribute__((always_inline)) void sq_w(uint32_t* p, uint32_t w) {
    __asm__("mov.l %1,@-%0" : "+r"(*p) : "r"(w));
}
static inline __attribute__((always_inline)) void sq_send(uint32_t sq) {
    __asm__ volatile("pref @%0" : : "r"(sq));
}
static inline __attribute__((always_inline))
void sq_put(uint32_t sq, uint32_t flags, float x, float y, float z, float a, float b,
            float c, float d) {
    uint32_t p = sq + 32;
    sq_f(&p, d); sq_f(&p, c); sq_f(&p, b); sq_f(&p, a);
    sq_f(&p, z); sq_f(&p, y); sq_f(&p, x); sq_w(&p, flags);
    sq_send(p);
}
/* The same for a vertex with a packed colour (the offset colour left out) */
static inline __attribute__((always_inline))
void sq_put_argb(uint32_t sq, uint32_t flags, float x, float y, float z, float u, float v,
                 uint32_t argb) {
    uint32_t p = sq + 28;
    sq_w(&p, argb); sq_f(&p, v); sq_f(&p, u);
    sq_f(&p, z); sq_f(&p, y); sq_f(&p, x); sq_w(&p, flags);
    sq_send(p);
}
static inline void sq_end(uint32_t sq) { pvr_dr_addr = sq; }
/* The same with the offset colour too (a highlight) */
static inline __attribute__((always_inline))
void sq_put_argb2(uint32_t sq, uint32_t flags, float x, float y, float z, float u, float v,
                  uint32_t argb, uint32_t oargb) {
    uint32_t p = sq + 32;
    sq_w(&p, oargb); sq_w(&p, argb); sq_f(&p, v); sq_f(&p, u);
    sq_f(&p, z); sq_f(&p, y); sq_f(&p, x); sq_w(&p, flags);
    sq_send(p);
}

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

/* The chip keeps a strip as blocks of six triangles, each eight vertices
 * (24 bytes textured, 28 with an offset colour) after 12 bytes of its own:
 * 34 and 39 a vertex along a long strip. Estimated at 36 and 44 to get in;
 * at 32 for everything it wrote past the end of the buffer on the police car
 * and hung. A clipped mesh then counts what it really sends (render_clipped)
 * and guards its per-triangle output. */
static inline int32_t vtxbuf_need(const DMSMesh* mesh) {
    int32_t per = DMS_MAT_SHINE_POWER(mesh->material_flags) ? 44 : 36;
    return (int32_t)(64 + mesh->vertex_count * per);
}

static inline int vtxbuf_full(const DMSMesh* mesh, int clip) {
    int32_t need = vtxbuf_need(mesh);
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
    pvr_vertex_t* pv = dr_vertex();
    pv->flags = flags;
    pv->x = v->x * inv_w;
    pv->y = v->y * inv_w;
    pv->z = inv_w;
    pv->u = v->u;
    pv->v = v->v;
    pv->argb = v->argb;
    dr_send(pv);
}

/* ================================================================
 * Runtime light
 *
 * The lights, multiplied over the colours already baked into the vertices.
 * Each is moved into the space the model's vertices are stored in once per
 * draw call, so the vertex loop dots it straight against the int8 normal
 * each vertex already carries and nothing is rotated per vertex. One light
 * is the common case and costs one dot a vertex; each one after it costs
 * another. The ambient, the cel bands and the blend are the first light's.
 * ================================================================ */

typedef struct {
    float x, y, z;       /* the light, in the model's own space */
    float pos_w;         /* 1 for a light in a place, 0 for a sun */
    float r, g, b;       /* colour, times 256/127 for the byte multiply below */
    float ambient;       /* times 127, to match the length of an int8 normal */
    float inv_range;     /* 0 for a sun, which never fades */
} ModelLight;

static DCLight    g_lights[DC_MAX_LIGHTS];
static int        g_light_n;               /* how many are set; 0 for none */
#define g_light   g_lights[0]              /* the first: ambient, cel, the blend */
static ModelLight g_ml[DC_MAX_LIGHTS];     /* the lights in the space of the model being drawn */
static int        g_ml_n;
static float      g_ml_ambient;            /* the first light's, times 127 */
static float      g_ml_model[DC_MAX_LIGHTS][3];   /* the same before a bone moved them (skinned) */
static ModelLight g_ml_place[DC_MAX_LIGHTS];      /* as light_to_model left them, see lights_at() */
static bool       g_ml_to_sun;                    /* lights_at() has lights to turn into suns */
static bool       g_ml_suns;                      /* every light is a sun (after lights_at()) */
static int        g_lit;     /* this draw call shades instead of copying argb */
static int        g_tint_on; /* dc_model_set_tint(): every colour multiplied by it */
static uint32_t   g_tint_r, g_tint_g, g_tint_b;   /* 0 to 256, so 255 is "as it is" */

void dc_model_set_tint(uint32_t rgb) {
    rgb &= 0xFFFFFFu;
    g_tint_on = rgb != 0 && rgb != 0xFFFFFFu;
    g_tint_r = ((rgb >> 16) & 0xff) + 1;
    g_tint_g = ((rgb >>  8) & 0xff) + 1;
    g_tint_b = ( rgb        & 0xff) + 1;
}

static inline uint32_t tint_argb(uint32_t c) {
    uint32_t r = (((c >> 16) & 0xff) * g_tint_r) >> 8;
    uint32_t g = (((c >>  8) & 0xff) * g_tint_g) >> 8;
    uint32_t b = (( c        & 0xff) * g_tint_b) >> 8;
    return (c & 0xff000000u) | (r << 16) | (g << 8) | b;
}

/* Rim light (Fresnel), after tiny3d's fresnel example: a vertex whose normal
 * turns away from the camera takes on the mesh's rim colour, so the outline
 * of a shiny thing glows and it reads as glossy and round. The camera is
 * moved into the model's space once per draw call like the light, and each
 * vertex dots its normal against the way to it. Set per mesh (rim_color). */
typedef struct { float x, y, z; float r, g, b; } RimLight;
static RimLight   g_rim;
static int        g_rim_on;  /* this mesh has a rim colour */
static bool       g_glow_only;   /* the bloom pass (defined with the reflections below) */
/* This mesh is drawn only to block the glow behind it (the bloom pass), so it
 * goes out black. Set per mesh, and only while dc_model_set_glow_only(). */
static int        g_flat;

/* Cel shading (DCLight.bands), after tiny3d's: how much light a vertex
 * catches goes into its U, and a ramp texture of flat steps turns that into
 * bands. The PVR carries U smoothly across a triangle and the ramp is read
 * without filtering, so the edge between two bands is a sharp line wherever
 * it falls, not a blend from one vertex to the next. A triangle has only one
 * texture, so the mesh keeps its own by being drawn unlit first; the bands
 * are then laid over it, multiplied, as a second pass (draw_cel). */
#define CEL_RAMP_W 64
#define CEL_RAMP_H 8

static pvr_ptr_t      g_cel_tex;
static pvr_poly_hdr_t g_cel_hdr __attribute__((aligned(32)));
static int            g_cel_bands;               /* 0: smooth light */
static int            g_cel_ramp_bands;          /* what the ramp holds now */
static float          g_cel_ramp_ambient = -1.0f;

static bool cel_setup(int bands, float ambient) {
    if (!g_cel_tex) {
        g_cel_tex = pvr_mem_malloc(CEL_RAMP_W * CEL_RAMP_H * 2);
        if (!g_cel_tex) return false;

        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                         CEL_RAMP_W, CEL_RAMP_H, g_cel_tex, PVR_FILTER_NONE);
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.txr.env = PVR_TXRENV_MODULATE;        /* ramp times the light colour */
        cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
        /* src * dst + dst * src (as a dst factor DESTCOLOR is the source
         * colour): the frame times twice the ramp, so the lit side can come
         * out brighter than its baked colour, as with the smooth light */
        cxt.blend.src = PVR_BLEND_DESTCOLOR;
        cxt.blend.dst = PVR_BLEND_DESTCOLOR;
        /* The same triangles as the solid draw, so the same depth */
        cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
        cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
        pvr_poly_compile(&g_cel_hdr, &cxt);
    }
    if (bands != g_cel_ramp_bands || ambient != g_cel_ramp_ambient) {
        /* Texel i is a vertex catching i / (W - 1) of the light. Half of
         * what the smooth light would multiply by, for the blend above. */
        uint16_t* t = (uint16_t*)g_cel_tex;
        for (int i = 0; i < CEL_RAMP_W; i++) {
            int b = (int)((float)i / (CEL_RAMP_W - 1) * bands);
            if (b > bands - 1) b = bands - 1;
            float v = (ambient + (float)b / (bands - 1)) * 0.5f;
            uint32_t g = v >= 1.0f ? 255 : (uint32_t)(v * 255.0f);
            uint16_t texel = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
            for (int y = 0; y < CEL_RAMP_H; y++) t[y * CEL_RAMP_W + i] = texel;
        }
        g_cel_ramp_bands = bands;
        g_cel_ramp_ambient = ambient;
    }
    return true;
}

void dc_model_set_lights(const DCLight* lights, int count) {
    if (!lights || count <= 0) count = 0;
    if (count > DC_MAX_LIGHTS) count = DC_MAX_LIGHTS;
    g_light_n = count;
    for (int i = 0; i < count; i++) g_lights[i] = lights[i];
    const DCLight* light = count ? &g_light : NULL;

    g_cel_bands = 0;
    if (light && light->bands >= 2) {
        int bands = light->bands < CEL_RAMP_W ? light->bands : CEL_RAMP_W;
        if (cel_setup(bands, light->ambient > 0.0f ? light->ambient : 0.25f))
            g_cel_bands = bands;
    }
}

void dc_model_set_light(const DCLight* light) { dc_model_set_lights(light, light ? 1 : 0); }

bool dc_model_cel_on(void) { return g_cel_bands != 0; }

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
    g_lit = g_light_n > 0 && scale > 0.0f;
    g_ml_to_sun = false;
    g_ml_suns = true;
    if (!g_lit) return;
    g_ml_n = g_light_n;
    g_ml_ambient = (g_light.ambient > 0.0f ? g_light.ambient : 0.25f) * 127.0f;

    /* A stretched model has columns shorter than 1 (see dc_draw_ex): a
     * direction goes back through the inverse, which divides each axis by
     * the column's length squared. Unit columns divide by 1. */
    float lx2 = cols[0] * cols[0] + cols[1] * cols[1] + cols[2] * cols[2];
    float ly2 = cols[3] * cols[3] + cols[4] * cols[4] + cols[5] * cols[5];
    float lz2 = cols[6] * cols[6] + cols[7] * cols[7] + cols[8] * cols[8];
    const float k = 256.0f / 127.0f;

    for (int i = 0; i < g_light_n; i++) {
        const DCLight* l = &g_lights[i];
        ModelLight* ml = &g_ml[i];
        float w[3];
        if (l->sun) {
            w[0] = -l->pos.x; w[1] = -l->pos.y; w[2] = -l->pos.z;
            float n = shz_inv_sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
            w[0] *= n; w[1] *= n; w[2] *= n;
            ml->pos_w = 0.0f;
            ml->inv_range = 0.0f;
        } else {
            float inv_scale = 1.0f / scale;
            w[0] = (l->pos.x - pos.x) * inv_scale;
            w[1] = (l->pos.y - pos.y) * inv_scale;
            w[2] = (l->pos.z - pos.z) * inv_scale;
            ml->pos_w = 1.0f;
            ml->inv_range = scale / (l->range > 0.0f ? l->range : 500.0f);
        }
        ml->x = (cols[0] * w[0] + cols[1] * w[1] + cols[2] * w[2]) / lx2;
        ml->y = (cols[3] * w[0] + cols[4] * w[1] + cols[5] * w[2]) / ly2;
        ml->z = (cols[6] * w[0] + cols[7] * w[1] + cols[8] * w[2]) / lz2;
        g_ml_model[i][0] = ml->x; g_ml_model[i][1] = ml->y; g_ml_model[i][2] = ml->z;

        int white = l->r <= 0.0f && l->g <= 0.0f && l->b <= 0.0f;
        ml->r = (white ? 1.0f : l->r) * k;
        ml->g = (white ? 1.0f : l->g) * k;
        ml->b = (white ? 1.0f : l->b) * k;
        g_ml_place[i] = *ml;
        if (ml->pos_w != 0.0f && g_light_n > 1) g_ml_to_sun = true;
        if (ml->pos_w != 0.0f) g_ml_suns = false;
    }
    if (g_ml_to_sun) g_ml_suns = true;
}

/* With more than one light, each light in a place becomes a sun for the mesh
 * about to be drawn: its direction and fade from the middle of the mesh (c,
 * in the model's space), worked out once instead of at every vertex. GTA III
 * did the same for each car and person; per mesh, the parts of a long car
 * each get their own. A vertex working out its own distance, square root and
 * fade for four lights cost more than the rest of it put together, and over
 * a mesh the direction barely changes.
 * The fade rides in inv_range: with the direction of unit length, a sun's
 * fade is 1 - inv_range, and a real sun's is 0. One light keeps the exact
 * sums: there are loops for it that cost little. */
static void lights_at(float cx, float cy, float cz) {
    if (!g_ml_to_sun) return;
    for (int i = 0; i < g_ml_n; i++) {
        const ModelLight* pl = &g_ml_place[i];
        if (pl->pos_w == 0.0f) continue;
        ModelLight* ml = &g_ml[i];
        float dx = pl->x - cx, dy = pl->y - cy, dz = pl->z - cz;
        float d2 = dx * dx + dy * dy + dz * dz;
        float fade = 0.0f;
        if (d2 > 0.0f) {
            float inv = shz_inv_sqrtf(d2);
            fade = 1.0f - d2 * inv * pl->inv_range;
            if (fade < 0.0f) fade = 0.0f;
            dx *= inv; dy *= inv; dz *= inv;
        }
        ml->x = dx; ml->y = dy; ml->z = dz;
        ml->pos_w = 0.0f;
        ml->inv_range = 1.0f - fade;
        g_ml_model[i][0] = dx; g_ml_model[i][1] = dy; g_ml_model[i][2] = dz;
    }
}

/* The camera into the space the vertices are stored in, for the rim */
static void cam_to_model(shz_vec3_t pos, float scale, const float* cols, const DCCamera* cam) {
    float inv_scale = scale > 0.0f ? 1.0f / scale : 0.0f;
    float w[3] = { (cam->pos.x - pos.x) * inv_scale, (cam->pos.y - pos.y) * inv_scale, (cam->pos.z - pos.z) * inv_scale };
    float lx2 = cols[0] * cols[0] + cols[1] * cols[1] + cols[2] * cols[2];
    float ly2 = cols[3] * cols[3] + cols[4] * cols[4] + cols[5] * cols[5];
    float lz2 = cols[6] * cols[6] + cols[7] * cols[7] + cols[8] * cols[8];
    g_rim.x = (cols[0] * w[0] + cols[1] * w[1] + cols[2] * w[2]) / lx2;
    g_rim.y = (cols[3] * w[0] + cols[4] * w[1] + cols[5] * w[2]) / ly2;
    g_rim.z = (cols[6] * w[0] + cols[7] * w[1] + cols[8] * w[2]) / lz2;
}

/* This mesh's rim colour, or none. Off in the glow pass: a rim is caught light */
static inline void rim_set(const DMSMesh* mesh) {
    uint32_t c = mesh->rim_color;
    g_rim_on = c != 0 && !g_glow_only;
    if (!g_rim_on) return;
    g_rim.r = (float)((c >> 16) & 0xff);
    g_rim.g = (float)((c >> 8) & 0xff);
    g_rim.b = (float)(c & 0xff);
}

/* The colour with the rim added: nothing facing the camera, all of it edge-on,
 * squared so it hugs the outline */
static inline uint32_t rim_light(uint32_t c, const DMSVertex* s) {
    float dx = g_rim.x - s->x, dy = g_rim.y - s->y, dz = g_rim.z - s->z;
    float inv = shz_inv_sqrtf(dx * dx + dy * dy + dz * dz);
    float ndv = (s->nx * dx + s->ny * dy + s->nz * dz) * inv * (1.0f / 127.0f);
    float f = 1.0f - fabsf(ndv);
    if (f <= 0.0f) return c;
    f *= f;
    uint32_t r = ((c >> 16) & 0xff) + (uint32_t)(g_rim.r * f);
    uint32_t g = ((c >>  8) & 0xff) + (uint32_t)(g_rim.g * f);
    uint32_t b = ( c        & 0xff) + (uint32_t)(g_rim.b * f);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (c & 0xff000000u) | (r << 16) | (g << 8) | b;
}

/* The vertex's baked colour with the light over it. pos_w is what lets a sun
 * use the same arithmetic: the difference below collapses to the light
 * direction, which is already unit length, and nothing fades. */
static inline float catch_light(const DMSVertex* s, const ModelLight* ml) {
    float dx = ml->x - s->x * ml->pos_w;
    float dy = ml->y - s->y * ml->pos_w;
    float dz = ml->z - s->z * ml->pos_w;
    float d2 = dx * dx + dy * dy + dz * dz;
    float inv = shz_inv_sqrtf(d2);

    float ndl = (s->nx * dx + s->ny * dy + s->nz * dz) * inv;
    float att = 1.0f - (d2 * inv) * ml->inv_range;
    if (ndl < 0.0f) ndl = 0.0f;
    if (att < 0.0f) att = 0.0f;
    return ndl * att;    /* 0 to 127, the length of an int8 normal */
}

/* The same for a sun: its direction is of unit length and its fade is
 * 1 - inv_range (see lights_at()), so there is no distance to work out */
static inline float catch_sun(const DMSVertex* s, const ModelLight* ml) {
    float ndl = s->nx * ml->x + s->ny * ml->y + s->nz * ml->z;
    if (ndl < 0.0f) ndl = 0.0f;
    return ndl * (1.0f - ml->inv_range);
}

static inline uint32_t shade(const DMSVertex* s) {
    uint32_t c = s->argb;
    if (g_lit) {
        /* The first light carries the ambient; the others only add */
        float lr, lg, lb;
        if (g_ml_suns) {
            float lit = g_ml_ambient + catch_sun(s, &g_ml[0]);
            lr = lit * g_ml[0].r; lg = lit * g_ml[0].g; lb = lit * g_ml[0].b;
            for (int i = 1; i < g_ml_n; i++) {
                lit = catch_sun(s, &g_ml[i]);
                lr += lit * g_ml[i].r; lg += lit * g_ml[i].g; lb += lit * g_ml[i].b;
            }
        } else {
            float lit = g_ml_ambient + catch_light(s, &g_ml[0]);
            lr = lit * g_ml[0].r; lg = lit * g_ml[0].g; lb = lit * g_ml[0].b;
            for (int i = 1; i < g_ml_n; i++) {
                lit = catch_light(s, &g_ml[i]);
                lr += lit * g_ml[i].r; lg += lit * g_ml[i].g; lb += lit * g_ml[i].b;
            }
        }
        uint32_t r = (((c >> 16) & 0xff) * (uint32_t)lr) >> 8;
        uint32_t g = (((c >>  8) & 0xff) * (uint32_t)lg) >> 8;
        uint32_t b = (( c        & 0xff) * (uint32_t)lb) >> 8;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        c = (c & 0xff000000u) | (r << 16) | (g << 8) | b;
    }
    if (g_tint_on) c = tint_argb(c);
    return g_rim_on ? rim_light(c, s) : c;
}

/* ================================================================
 * Render: fast path (static mesh, fully inside frustum)
 * ================================================================ */

/* dc_model_scroll(): how far this mesh's texture has slid, added to every
 * vertex's u and v by the loops below. Set per mesh from the clock, wrapped
 * to a texture width so it never grows. */
static float g_uv_u = 0.0f, g_uv_v = 0.0f;
static bool  g_uv_on;   /* an offset to add: the plain loops stay as they are without one */

static inline void uv_scroll_set(const DMSMesh* m) {
    if (m->flip_count) {   /* dc_model_flipbook(): jump to this moment's frame */
        uint32_t frame = (uint32_t)((float)(dc_time_ms() % 3600000u) * 0.001f * m->flip_fps) % m->flip_count;
        g_uv_u = (frame % m->flip_across) * m->flip_w;
        g_uv_v = (frame / m->flip_across) * m->flip_h;
        g_uv_on = true;
        return;
    }
    if (m->scroll_u == 0.0f && m->scroll_v == 0.0f) { g_uv_u = g_uv_v = 0.0f; g_uv_on = false; return; }
    float t = (float)(dc_time_ms() % 3600000u) * 0.001f;
    float u = m->scroll_u * t, v = m->scroll_v * t;
    g_uv_u = u - floorf(u);
    g_uv_v = v - floorf(v);
    g_uv_on = true;
}

/* The lit copy of the loop below. The shading goes between the matrix multiply
 * and the divide that wants its result, which is where the unlit loop stalls,
 * so this one has no software pipeline to keep it busy. */
static inline __attribute__((always_inline))
void render_fast_lit_impl(const DMSVertex* src, int count,
                          pvr_dr_state_t* dr, const bool uv) {
    const float uv_u = uv ? g_uv_u : 0.0f, uv_v = uv ? g_uv_v : 0.0f;
    SHZ_PREFETCH(&src[0]);
    SHZ_PREFETCH(&src[1]);
    uint32_t sq = pvr_dr_addr;

    for (int i = 0; i < count; i++) {
        SHZ_PREFETCH(&src[i + 2]);

        shz_vec4_t t = shz_xmtrx_transform_vec4(
            shz_vec4_init(src[i].x, src[i].y, -src[i].z, 1.0f)
        );
        uint32_t argb = shade(&src[i]);
        t = shz_vec4_swizzle(t, 1, 2, 3, 0);

        float inv_w = shz_invf_fsrra(t.w);
        sq = sq_next(sq);
        sq_put_argb(sq, src[i].flags, t.x * inv_w, t.y * inv_w, inv_w, src[i].u + uv_u, src[i].v + uv_v, argb);
    }
    sq_end(sq);
}

static inline __attribute__((always_inline))
void render_fast_impl(const DMSVertex* src, int count,
                      pvr_dr_state_t* dr, const bool uv) {
    if (count < 1) return;
    uint32_t sq = pvr_dr_addr;

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
    const float uv_u = uv ? g_uv_u : 0.0f, uv_v = uv ? g_uv_v : 0.0f;
    float    cur_u     = src[0].u + uv_u;
    float    cur_v     = src[0].v + uv_v;
    uint32_t cur_argb  = src[0].argb;

    for (int i = 1; i < count; i++) {
        SHZ_PREFETCH(&src[i + 4]);

        float    nx     = src[i].x;
        float    ny     = src[i].y;
        float    nz     = -src[i].z;
        uint32_t nflags = src[i].flags;
        float    nu     = src[i].u + uv_u;
        float    nv     = src[i].v + uv_v;
        uint32_t nargb  = src[i].argb;

        shz_vec4_t next_t = shz_xmtrx_transform_vec4(
            shz_vec4_init(nx, ny, nz, 1.0f)
        );

        sq = sq_next(sq);
        sq_put_argb(sq, cur_flags, cur_sx, cur_sy, cur_invw, cur_u, cur_v, cur_argb);

        next_t = shz_vec4_swizzle(next_t, 1, 2, 3, 0);
        cur_invw  = shz_invf_fsrra(next_t.w);
        cur_sx    = next_t.x * cur_invw;
        cur_sy    = next_t.y * cur_invw;
        cur_flags = nflags;
        cur_u     = nu;
        cur_v     = nv;
        cur_argb  = nargb;
    }

    sq = sq_next(sq);
    sq_put_argb(sq, cur_flags, cur_sx, cur_sy, cur_invw, cur_u, cur_v, cur_argb);
    sq_end(sq);
}

/* The plain loops, and the same with a texture offset (dc_model_scroll,
 * dc_model_flipbook). The offset is folded out of the plain ones: two adds
 * a vertex showed on the highpoly test, run 55k times a frame. */
static void render_fast(const DMSVertex* src, int count, pvr_dr_state_t* dr) {
    render_fast_impl(src, count, dr, false);
}
static __attribute__((noinline)) void render_fast_uv(const DMSVertex* src, int count, pvr_dr_state_t* dr) {
    render_fast_impl(src, count, dr, true);
}
static void render_fast_lit(const DMSVertex* src, int count, pvr_dr_state_t* dr) {
    render_fast_lit_impl(src, count, dr, false);
}
static __attribute__((noinline)) void render_fast_lit_uv(const DMSVertex* src, int count, pvr_dr_state_t* dr) {
    render_fast_lit_impl(src, count, dr, true);
}

/* ================================================================
 * Render: one light over one colour (the PVR's intensity mode)
 *
 * Most models keep their colour in the material and the detail in the
 * texture, so every vertex of a mesh has the same colour. Such a mesh sends
 * that colour once, in the header, and each vertex only how bright it is; the
 * graphics chip multiplies the two. The CPU is left a dot product a vertex
 * instead of three multiplies, three clamps and a repack -- how Sega's Ninja
 * library lit its car viewer.
 *
 * The header's second colour, the offset, is added after the texture, which
 * is what a highlight is: it sits on the paint rather than being tinted by
 * it. A glossy material gets one, from its glTF roughness and clear coat.
 * ================================================================ */

static bool g_add;   /* dc_model_set_add(), below */
static const dttex_info_t* g_env;   /* dc_model_set_environment(), below */

/* A vertex of the intensity mode: brightness in place of the colours */
typedef struct __attribute__((aligned(32))) {
    uint32_t flags;
    float x, y, z, u, v;
    float base, offset;
} IntensityVertex;

/* One light, a mesh of one colour, fully in view: the other cases (more
 * lights, a rim, the additive or flat passes) keep render_fast_lit */
static inline bool intensity_ok(const DMSMesh* mesh) {
    return g_lit && g_ml_n == 1 && !g_rim_on && !g_add && !g_flat &&
           (mesh->material_flags & DMS_MAT_ONE_COLOUR);
}

bool dc_model_reflects(const DMSMesh* mesh);

/* Metal with no texture of its own, under one sun, shows the environment
 * image AS its texture, tinted by its colour and shaded by the sun, in the
 * one pass -- how Sega's Katana car viewer drew its body. The second,
 * additive pass would send every vertex again. */
static inline bool env_single(const DMSMesh* mesh) {
    uint32_t f = mesh->material_flags;
    return g_env && g_light.sun && !mesh->header.m0.txr_en &&
           (f & DMS_MAT_METALLIC) && !(f & DMS_MAT_MIRROR) && !mesh->rim_color &&
           (f & DMS_MAT_ONE_COLOUR) && dc_model_reflects(mesh);
}

/* The camera's right and up in model space, scaled so a full-length int8
 * normal gives 0.5: a normal's lookup into the sphere-map environment image */
static float g_env_right[3], g_env_up[3];
static float g_half_x, g_half_y, g_half_z;   /* sun_half_set(), below */

static void env_axes(const DCCamera* cam, const float* cols) {
    shz_xmtrx_init_identity();
    shz_xmtrx_apply_rotation_y(-cam->yaw);
    shz_xmtrx_apply_rotation_x(-cam->pitch);
    shz_vec4_t wr = shz_xmtrx_transform_vec4(shz_vec4_init(-1.0f, 0.0f, 0.0f, 0.0f));
    shz_vec4_t wu = shz_xmtrx_transform_vec4(shz_vec4_init(0.0f, 1.0f, 0.0f, 0.0f));
    wr.z = -wr.z;
    wu.z = -wu.z;
    const float k = 0.5f / 127.0f;
    for (int j = 0; j < 3; j++) {
        g_env_right[j] = (cols[j*3] * wr.x + cols[j*3+1] * wr.y + cols[j*3+2] * wr.z) * k;
        g_env_up[j]    = (cols[j*3] * wu.x + cols[j*3+1] * wu.y + cols[j*3+2] * wu.z) * k;
    }
}

/* The mesh's header with the colour format switched, and the colours: the
 * mesh's own times the light's (and the tint), the highlight's white, or for
 * metal its own colour. A highlight needs the 64-byte header, which carries
 * both; without one the 32-byte header carries the face colour alone. */
/* ---- Packed vertices ----
 * A one-colour mesh on a model with no skeleton is kept packed for the sun
 * loops by hand: one 32-byte read a vertex. x y z u v are where they always
 * are; the colour, the int8 normal and the flags make way for the normal as
 * floats. The colour is the mesh's (colour). The normal came from int8, so
 * as floats their low 16 bits are otherwise 0: the vertex's shine (pad) is
 * the low byte of nx, and the end of a strip bit 12 of nz. A 0 is stored as
 * 2^-16, which has those bits free and is still 0 as an int8. Everything
 * else reads the mesh through mesh_verts(), which unpacks it into a buffer
 * of its own. */
typedef struct { float x, y, z, u, v, nx, ny, nz; } DMSPacked;
#define PACK_END 0x1000u

static DMSVertex*      g_view;
static uint32_t        g_view_cap;
static const DMSMesh*  g_view_mesh;

static inline void unpack_vertex(DMSVertex* d, const DMSPacked* s, const DMSMesh* mesh) {
    DMSPacked p = *s;                   /* d and s may be the same vertex */
    uint32_t nx, nz;
    memcpy(&nx, &p.nx, 4);
    memcpy(&nz, &p.nz, 4);
    int end = (nz & PACK_END) != 0;
    uint8_t shine = nx & 0xFFu;
    nx &= ~0xFFu;
    nz &= ~PACK_END;
    memcpy(&p.nx, &nx, 4);
    memcpy(&p.nz, &nz, 4);
    d->x = p.x; d->y = p.y; d->z = p.z; d->u = p.u; d->v = p.v;
    d->argb = mesh->colour;
    d->nx = (int8_t)p.nx; d->ny = (int8_t)p.ny; d->nz = (int8_t)p.nz;
    d->pad = shine;
    d->flags = end ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
}

/* The mesh's vertices as DMSVertex, packed or not */
static const DMSVertex* mesh_verts(const DMSMesh* mesh) {
    if (!mesh->packed) return mesh->vertices;
    if (g_view_mesh == mesh) return g_view;
    if (mesh->vertex_count > g_view_cap) {
        free(g_view);
        g_view = memalign(32, mesh->vertex_count * sizeof(DMSVertex));
        g_view_cap = g_view ? mesh->vertex_count : 0;
        g_view_mesh = NULL;
        if (!g_view) return mesh->vertices;
    }
    const DMSPacked* src = (const DMSPacked*)mesh->vertices;
    for (uint32_t i = 0; i < mesh->vertex_count; i++)
        unpack_vertex(&g_view[i], &src[i], mesh);
    g_view_mesh = mesh;
    return g_view;
}

/* The plain loop over a packed mesh, read as it is stored: one colour, the
 * mesh's, and the end of a strip bit 12 of nz, which moved up 16 is the bit
 * PVR_CMD_VERTEX_EOL adds. Through mesh_verts() an unlit packed mesh was
 * unpacked into a buffer every frame and then read again. */
static void render_fast_packed(const DMSMesh* mesh) {
    const DMSPacked* src = (const DMSPacked*)mesh->vertices;
    const int count = (int)mesh->vertex_count;
    const uint32_t argb = mesh->colour;
    const float uv_u = g_uv_u, uv_v = g_uv_v;
    uint32_t sq = pvr_dr_addr;

    SHZ_PREFETCH(&src[0]);
    SHZ_PREFETCH(&src[1]);
    SHZ_PREFETCH(&src[2]);
    SHZ_PREFETCH(&src[3]);

    for (int i = 0; i < count; i++) {
        SHZ_PREFETCH(&src[i + 4]);

        shz_vec4_t t = shz_xmtrx_transform_vec4(
            shz_vec4_init(src[i].x, src[i].y, -src[i].z, 1.0f)
        );
        const uint32_t nz = ((const uint32_alias*)&src[i])[7];
        uint32_t flags = PVR_CMD_VERTEX | ((nz & PACK_END) << 16);
        t = shz_vec4_swizzle(t, 1, 2, 3, 0);

        float inv_w = shz_invf_fsrra(t.w);
        sq = sq_next(sq);
        sq_put_argb(sq, flags, t.x * inv_w, t.y * inv_w, inv_w,
                    src[i].u + uv_u, src[i].v + uv_v, argb);
    }
    sq_end(sq);
}

static uint32_t mesh_colour(const DMSMesh* mesh) {
    return mesh->packed ? mesh->colour : mesh->vertices[0].argb;
}

/* Back to DMSVertex for good, before anything changes the vertices */
void dc_model_mesh_unpack(DMSMesh* mesh) {
    if (!mesh || !mesh->packed) return;
    DMSPacked* src = (DMSPacked*)mesh->vertices;
    for (uint32_t i = 0; i < mesh->vertex_count; i++)
        unpack_vertex(&mesh->vertices[i], &src[i], mesh);
    mesh->packed = 0;
    g_view_mesh = NULL;
}

/* Packed if it can be: one colour, no skeleton (pad is the bone there), and
 * plain strip flags */
static void mesh_pack(DMSMesh* mesh, bool skinned) {
    if (skinned || mesh->packed || !mesh->vertex_count) return;
    uint32_t f = mesh->material_flags;
    if (!(f & DMS_MAT_ONE_COLOUR)) return;
    /* The reflection pass reads a packed mesh as it is (render_env_packed),
     * but not a mirror (drawn there alone), textured glass's three passes or
     * a rim: those would unpack every frame, so are left as they are */
    if (dc_model_reflects(mesh) &&
        ((f & DMS_MAT_MIRROR) || mesh->rim_color ||
         ((f & 0x3) == 2 && mesh->texture_id >= 0))) return;
    const DMSVertex* v = mesh->vertices;
    for (uint32_t i = 0; i < mesh->vertex_count; i++)
        if (v[i].flags != PVR_CMD_VERTEX && v[i].flags != PVR_CMD_VERTEX_EOL) return;
    mesh->colour = v[0].argb;
    DMSPacked* dst = (DMSPacked*)mesh->vertices;
    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
        DMSVertex s = v[i];
        DMSPacked p = { s.x, s.y, s.z, s.u, s.v, s.nx, s.ny, s.nz };
        uint32_t nx, nz;
        if (p.nx == 0.0f) p.nx = 1.0f / 65536.0f;   /* 0 has no bits to spare */
        if (p.nz == 0.0f) p.nz = 1.0f / 65536.0f;
        memcpy(&nx, &p.nx, 4);
        memcpy(&nz, &p.nz, 4);
        nx |= s.pad;
        if (s.flags == PVR_CMD_VERTEX_EOL) nz |= PACK_END;
        memcpy(&p.nx, &nx, 4);
        memcpy(&p.nz, &nz, 4);
        dst[i] = p;
    }
    mesh->packed = 1;
    g_view_mesh = NULL;
}

/* ================================================================
 * Render: several lights over a packed mesh
 *
 * A light in a place becomes a sun for each mesh: its direction and its fade
 * are worked out once, from the middle of the mesh, as GTA III did for each
 * car and person (per mesh here, so a long car's ends each get their own).
 * Every vertex working out its own distance, square root and fade cost about
 * 130 cycles a vertex for two lights; over a mesh the direction barely
 * changes and the result is the same to look at.
 *
 * So all four lights are suns by the time the loop sees them, one to a row of
 * the matrix unit: one ftrv of the normal gives the normal against all four.
 * With the matrix busy, the position goes to the screen by three fipr. A
 * light out of range, or not there, is a row of nothing.
 * ================================================================ */

/* With the sheen the one-light loops give: the sun's highlight as the offset colour, and metal
 * with no texture showing the environment image as its texture.
 *
 * k, in the order the loop reads it:
 *   the w, x and y rows of mvp (z negated)
 *   u = (normal, u).(4) + (1), v = (normal, v).(4) + (1): the stored u and v
 *     plus the scroll, or the environment image's lookup
 *   if power: the half vector / 2, the sun * 4/127 (the highlight fades in
 *     over the first quarter past the shadow line), the highlight's colour / 4
 *   the red, green and blue each light adds (over 16, with the mesh colour
 *   in), then (ambient - 255) / 2 per colour: the clamp
 *   to 255 is min(v - 255, 0) + 255, the min being (h - |h|) of h = half of it */

/* LOOP_CONSTS: the numbers a loop needs every vertex go in its k too. The
 * SH4 has no way to put most of them in an instruction, so the compiler keeps
 * them in a pool beside the code and reads them from there, through the data
 * cache. The stack (k, spills) never moves but the code does with every
 * build, and when a pool shares a cache line's slot with the stack the two
 * throw each other out every vertex: 1431 -> 1790ns a vertex on the police car
 * from moving the code alone. In k they sit by the rest and can't.
 *   2.0f, PACK_END in the command word's place, the vertex command, 255. */
static inline void loop_consts(float* k) {
    const uint32_t c[4] = { 0x40000000u /* 2.0f */, PACK_END << 16, PVR_CMD_VERTEX, 255 };
    memcpy(k, c, sizeof c);
}

/* Four of k, in order */
typedef struct { float a, b, c, d; } K4;
static inline __attribute__((always_inline)) K4 k4(const float** k) {
    const float* p = *k;
    K4 r = { p[0], p[1], p[2], p[3] };
    *k = p + 4;
    return r;
}

/* The loop itself, shaped for the SH4 rather than written the obvious way.
 * The SH4 can't load a float from an offset, so a table read at fixed
 * places costs an address each, which the compiler works out once, runs out
 * of registers for and keeps on the stack. So k is walked instead, from the
 * start each vertex, read through a volatile so the compiler can't work the
 * addresses out ahead. One fipr per colour, the clamps as sums of absolute
 * values, and nothing to branch on but the highlight's squaring. shine is a
 * constant in each copy the compiler makes; power is only how many times to
 * square. */
static inline __attribute__((always_inline))
uint32_t lights_loop(const DMSPacked* src, int count, const float* k0, uint32_t sq,
                     uint32_t alpha, int power, const bool shine) {
    /* k in three parts, each from its own start: past 127 bytes from where
     * it starts, a read's offset won't fit in the instruction and comes from
     * a pool in the code (see LOOP_CONSTS) */
    const float* volatile kstart = k0;
    const float* volatile klight = k0 + 12 + 10;
    const float* volatile kcolour = k0 + 12 + 10 + (shine ? 11 : 0);
    for (int i = 0; i < count; i++) {
        SHZ_PREFETCH(&src[i + 2]);
        const float* k = kstart;
        const float* kz = kcolour + 12 + 3;   /* LOOP_CONSTS */
        const float two = kz[0];
        const uint32_t end_bit = ((const uint32_alias*)kz)[1];
        const uint32_t cmd = ((const uint32_alias*)kz)[2];
        const int c255 = ((const int32_t __attribute__((may_alias))*)kz)[3];
        const float x = src[i].x, y = src[i].y, z = src[i].z;
        const float nx = src[i].nx, ny = src[i].ny, nz = src[i].nz;
        /* the shine and the end of the strip, in the normal's spare bits */
        const uint32_t nxb = ((const uint32_alias*)&src[i])[5];
        const uint32_t nzb = ((const uint32_alias*)&src[i])[7];

        /* To the screen */
        K4 m = k4(&k);
        float tw = shz_dot8f(m.a, m.b, m.c, m.d, x, y, z, 1.0f);
        m = k4(&k);
        float tx = shz_dot8f(m.a, m.b, m.c, m.d, x, y, z, 1.0f);
        m = k4(&k);
        float ty = shz_dot8f(m.a, m.b, m.c, m.d, x, y, z, 1.0f);
        float inv_w = shz_inv_sqrtf_fsrra(tw * tw);

        /* u and v: the stored ones plus the scroll, or the environment image */
        m = k4(&k);
        float u = shz_dot8f(nx, ny, nz, src[i].u, m.a, m.b, m.c, m.d);
        m = k4(&k);
        float v = shz_dot8f(nx, ny, nz, src[i].v, m.a, m.b, m.c, m.d);
        u += k[0];
        v += k[1];
        k = klight;

        /* The sun's highlight, as the offset colour */
        uint32_t oargb = 0;
        if (shine) {
            m = k4(&k);
            float ndh = shz_dot8f(nx, ny, nz, 0.0f, m.a, m.b, m.c, 0.0f);
            m = k4(&k);
            float q = shz_dot8f(nx, ny, nz, 0.0f, m.a, m.b, m.c, 0.0f);
            float h = ndh + shz_fabsf(ndh);                       /* max(ndh, 0) */
#pragma GCC unroll 1
            for (int p = 0; p < power; p++) h *= h;
            float o = h * (shz_fabsf(q) - shz_fabsf(q - 1.0f) + 1.0f);   /* 2 x the fade */
            o = (o + two - shz_fabsf(o - two)) * (float)(nxb & 0xffu);  /* 4 min(, 1) x shine */
            oargb = alpha | ((uint32_t)(int)(o * k[0]) << 16) |
                    ((uint32_t)(int)(o * k[1]) << 8) | (uint32_t)(int)(o * k[2]);
            k += 3;
        }

        /* The lights: -N.L for all four in one go, then 2 max(N.L, 0) each */
        shz_vec4_t mv = shz_xmtrx_transform_vec4(shz_vec4_init(nx, ny, nz, 0.0f));
        const float l0 = shz_fabsf(mv.x) - mv.x, l1 = shz_fabsf(mv.y) - mv.y,
                    l2 = shz_fabsf(mv.z) - mv.z, l3 = shz_fabsf(mv.w) - mv.w;
        k = kcolour;
        m = k4(&k);
        float r = shz_dot8f(l0, l1, l2, l3, m.a, m.b, m.c, m.d);
        m = k4(&k);
        float g = shz_dot8f(l0, l1, l2, l3, m.a, m.b, m.c, m.d);
        m = k4(&k);
        float b = shz_dot8f(l0, l1, l2, l3, m.a, m.b, m.c, m.d);
        r += k[0]; g += k[1]; b += k[2];
        uint32_t argb = alpha |
            ((uint32_t)(c255 + (int)(r - shz_fabsf(r))) << 16) |
            ((uint32_t)(c255 + (int)(g - shz_fabsf(g))) << 8) |
             (uint32_t)(c255 + (int)(b - shz_fabsf(b)));
        uint32_t flags = cmd | ((nzb << 16) & end_bit);

        sq = sq_next(sq);
        sq_put_argb2(sq, flags, tx * inv_w, ty * inv_w, inv_w, u, v, argb, oargb);
    }
    return sq;
}

static uint32_t lights_c(const DMSPacked* src, int count, const float* k, uint32_t sq,
                         uint32_t alpha, int power) {
    return power ? lights_loop(src, count, k, sq, alpha, power, true)
                 : lights_loop(src, count, k, sq, alpha, 0, false);
}

/* Returns true when it sent a header of its own (a highlight or the image) */
static bool render_packed_lights(pvr_dr_state_t* dr, const DMSMesh* mesh,
                                 const shz_mat4x4_t* mvp) {
    uint32_t c = mesh->colour;
    const bool sun = g_light.sun;
    const bool env = sun && !g_add && env_single(mesh);
    /* The PVR adds an offset colour to textured polygons only. Showing the
     * environment, the image is its shine: no highlight, as Ninja */
    int power = sun && !g_add && mesh->header.m0.txr_en ?
                (int)DMS_MAT_SHINE_POWER(mesh->material_flags) : 0;
    if (power > 8) power = 8;
    const bool metal = (mesh->material_flags & DMS_MAT_METALLIC) != 0;
    /* Metal that shows the image is mostly what it reflects (as the
     * intensity header has it) */
    const float dim = metal && g_env && dc_model_reflects(mesh) && !env ? 0.5f : 1.0f;
    const float cr = (float)((c >> 16) & 0xff) * (1.0f / 256.0f) * dim;
    const float cg = (float)((c >>  8) & 0xff) * (1.0f / 256.0f) * dim;
    const float cb = (float)( c        & 0xff) * (1.0f / 256.0f) * dim;

    if (env) ((DMSMesh*)mesh)->env_frame = dc_frame_count() + 1;
    if (power || env) {
        alignas(32) uint32_t h[8];
        if (env)
            dc_model_compile_header(mesh, (pvr_poly_hdr_t*)h, g_env->pvrformat,
                                    g_env->width, g_env->height, g_env->ptr);
        else
            memcpy(h, &mesh->header, 32);
        h[0] = (h[0] | dc_clip_cmd) & ~(PVR_TA_CMD_CLRFMT_MASK | PVR_TA_CMD_SPECULAR_MASK);
        h[0] |= PVR_CLRFMT_ARGBPACKED << PVR_TA_CMD_CLRFMT_SHIFT;
        if (power) h[0] |= PVR_TA_CMD_SPECULAR_MASK;
        shz_sq_memcpy32_1_xmtrx(pvr_dr_target(*dr), h);
    }

    alignas(8) shz_vec4_t row[4];
    alignas(4) float k[12 + 10 + 11 + 12 + 3 + 4];
    const float* m = mvp->elem;
    float* p = k;
    const float rows[12] = { m[0], m[4], -m[8],  m[12],
                             m[1], m[5], -m[9],  m[13],
                             m[2], m[6], -m[10], m[14] };
    memcpy(p, rows, sizeof rows); p += 12;
    if (env) {
        const float uv[10] = { g_env_right[0], g_env_right[1], g_env_right[2], 0.0f,
                               -g_env_up[0], -g_env_up[1], -g_env_up[2], 0.0f, 0.5f, 0.5f };
        memcpy(p, uv, sizeof uv);
    } else {
        const float uv[10] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, g_uv_u, g_uv_v };
        memcpy(p, uv, sizeof uv);
    }
    p += 10;
    if (power) {
        const float to_light = 127.0f / 256.0f / 255.0f;   /* as send_intensity_header */
        float s = (float)DMS_MAT_SHINE_STRENGTH(mesh->material_flags) * (1.0f / 255.0f);
        float sr = metal ? (float)((c >> 16) & 0xff) : 255.0f;
        float sg = metal ? (float)((c >>  8) & 0xff) : 255.0f;
        float sb = metal ? (float)( c        & 0xff) : 255.0f;
        float hr = sr * g_ml[0].r * to_light * s, hg = sg * g_ml[0].g * to_light * s,
              hb = sb * g_ml[0].b * to_light * s;
        float l = shz_inv_sqrtf(g_ml[0].x * g_ml[0].x + g_ml[0].y * g_ml[0].y +
                                g_ml[0].z * g_ml[0].z) * (4.0f / 127.0f);
        const float hl[11] = {
            g_half_x * 0.5f, g_half_y * 0.5f, g_half_z * 0.5f, 0.0f,
            g_ml[0].x * l, g_ml[0].y * l, g_ml[0].z * l, 0.0f,
            (hr > 1.0f ? 1.0f : hr) * 0.25f, (hg > 1.0f ? 1.0f : hg) * 0.25f,
            (hb > 1.0f ? 1.0f : hb) * 0.25f,
        };
        memcpy(p, hl, sizeof hl);
        p += 11;
    }
    /* Each light as a sun (lights_at() has made them so, bar a single light
     * in a place, done the same way here): a row of -4 fade L (unit), so with
     * the 127-long normal the loop's 2 max(N.L, 0) comes to 1016 fade cos,
     * the scale the colours below were set for. */
    float* kc = p;
    for (int i = 0; i < 4; i++) {
        float lx = 0.0f, ly = 0.0f, lz = 0.0f, r = 0.0f, g = 0.0f, b = 0.0f;
        if (i < g_ml_n) {
            const ModelLight* ml = &g_ml[i];
            float fade = 1.0f - ml->inv_range;   /* a sun's, see lights_at() */
            lx = ml->x; ly = ml->y; lz = ml->z;
            if (ml->pos_w != 0.0f) {
                lx -= mesh->bound_cx; ly -= mesh->bound_cy; lz -= mesh->bound_cz;
            }
            float d2 = lx * lx + ly * ly + lz * lz;
            float inv = d2 > 0.0f ? shz_inv_sqrtf(d2) : 0.0f;
            if (ml->pos_w != 0.0f) {
                fade = 1.0f - d2 * inv * ml->inv_range;
                if (fade < 0.0f) fade = 0.0f;
            }
            float s = -4.0f * fade * inv;
            lx *= s; ly *= s; lz *= s;
            r = ml->r * cr * (1.0f / 16.0f);
            g = ml->g * cg * (1.0f / 16.0f);
            b = ml->b * cb * (1.0f / 16.0f);
        }
        row[i] = shz_vec4_init(lx, ly, lz, 0.0f);
        kc[i] = r; kc[4 + i] = g; kc[8 + i] = b;
    }
    kc[12] = (g_ml_ambient * g_ml[0].r * cr - 255.0f) * 0.5f;
    kc[13] = (g_ml_ambient * g_ml[0].g * cg - 255.0f) * 0.5f;
    kc[14] = (g_ml_ambient * g_ml[0].b * cb - 255.0f) * 0.5f;
    loop_consts(&kc[15]);

    shz_xmtrx_load_rows_4x4(&row[0], &row[1], &row[2], &row[3]);
    pvr_dr_addr = lights_c((const DMSPacked*)mesh->vertices, (int)mesh->vertex_count, k,
                           pvr_dr_addr, c & 0xff000000u, power);
    return power || env;
}

static void send_intensity_header(pvr_dr_state_t* dr, const DMSMesh* mesh, bool shine,
                                  bool env) {
    alignas(32) uint32_t h[16];
    if (env)
        dc_model_compile_header(mesh, (pvr_poly_hdr_t*)h, g_env->pvrformat,
                                g_env->width, g_env->height, g_env->ptr);
    else
        memcpy(h, &mesh->header, 32);
    h[0] = (h[0] | dc_clip_cmd) & ~(PVR_TA_CMD_CLRFMT_MASK | PVR_TA_CMD_SPECULAR_MASK);
    h[0] |= PVR_CLRFMT_INTENSITY << PVR_TA_CMD_CLRFMT_SHIFT;

    uint32_t c = mesh_colour(mesh);
    if (g_tint_on) c = tint_argb(c);
    const float to_light = 127.0f / 256.0f / 255.0f;   /* undo ModelLight's k, bytes to 0-1 */
    float lr = g_ml[0].r * to_light, lg = g_ml[0].g * to_light, lb = g_ml[0].b * to_light;
    float cr = (float)((c >> 16) & 0xff), cg = (float)((c >> 8) & 0xff), cb = (float)(c & 0xff);
    bool metal = (mesh->material_flags & DMS_MAT_METALLIC) != 0;
    /* Metal that shows the image is mostly what it reflects */
    float dim = metal && g_env && dc_model_reflects(mesh) && !env ? 0.5f : 1.0f;

    float* col = (float*)&h[shine ? 8 : 4];
    col[0] = (float)(c >> 24) * (1.0f / 255.0f);
    col[1] = cr * lr * dim; col[2] = cg * lg * dim; col[3] = cb * lb * dim;
    if (shine) {
        h[0] |= PVR_TA_CMD_SPECULAR_MASK;
        float s = (float)DMS_MAT_SHINE_STRENGTH(mesh->material_flags) * (1.0f / 255.0f);
        float sr = metal ? cr : 255.0f, sg = metal ? cg : 255.0f, sb = metal ? cb : 255.0f;
        col[4] = 1.0f;
        col[5] = sr * lr * s; col[6] = sg * lg * s; col[7] = sb * lb * s;
        for (int i = 1; i < 8; i++) if (col[i] > 1.0f) col[i] = 1.0f;
        shz_sq_memcpy32_1_xmtrx(pvr_dr_target(*dr), &h[0]);
        shz_sq_memcpy32_1_xmtrx(pvr_dr_target(*dr), &h[8]);
    } else {
        for (int i = 1; i < 4; i++) if (col[i] > 1.0f) col[i] = 1.0f;
        shz_sq_memcpy32_1_xmtrx(pvr_dr_target(*dr), &h[0]);
    }
}

/* A light in a place: the direction to it, and to the eye, change across the
 * mesh. power is how many times the highlight is squared, 1 to 8. */
static inline __attribute__((always_inline))
void render_fast_point_impl(const DMSVertex* src, int count, const bool shine, int power) {
    const ModelLight* ml = &g_ml[0];
    const float lx = ml->x, ly = ml->y, lz = ml->z, ir = ml->inv_range;
    const float amb = g_ml_ambient * (1.0f / 127.0f);
    const float ex = g_rim.x, ey = g_rim.y, ez = g_rim.z;   /* the camera */
    const float uv_u = g_uv_u, uv_v = g_uv_v;
    SHZ_PREFETCH(&src[0]);
    SHZ_PREFETCH(&src[1]);
    uint32_t sq = pvr_dr_addr;

    for (int i = 0; i < count; i++) {
        SHZ_PREFETCH(&src[i + 2]);
        const DMSVertex* s = &src[i];

        shz_vec4_t t = shz_xmtrx_transform_vec4(shz_vec4_init(s->x, s->y, -s->z, 1.0f));

        float nx = s->nx, ny = s->ny, nz = s->nz;
        float dx = lx - s->x, dy = ly - s->y, dz = lz - s->z;
        float d2 = dx * dx + dy * dy + dz * dz;
        float inv = shz_inv_sqrtf_fsrra(d2);
        float ndl = (nx * dx + ny * dy + nz * dz) * inv * (1.0f / 127.0f);
        float att = 1.0f - d2 * inv * ir;
        if (att < 0.0f) att = 0.0f;

        float base = amb, offset = 0.0f;
        if (ndl > 0.0f) {
            base += ndl * att;
            if (shine) {
                /* Blinn: the normal against halfway between light and eye */
                float vx = ex - s->x, vy = ey - s->y, vz = ez - s->z;
                float vinv = shz_inv_sqrtf_fsrra(vx * vx + vy * vy + vz * vz);
                float hx = dx * inv + vx * vinv, hy = dy * inv + vy * vinv, hz = dz * inv + vz * vinv;
                float hinv = shz_inv_sqrtf_fsrra(hx * hx + hy * hy + hz * hz);
                float ndh = (nx * hx + ny * hy + nz * hz) * hinv * (1.0f / 127.0f);
                if (ndh > 0.0f) {
                    for (int k = 0; k < power; k++) ndh *= ndh;
                    float fade = ndl * 4.0f;
                    offset = (fade < 1.0f ? ndh * fade : ndh) * att *
                             ((float)s->pad * (1.0f / 255.0f));
                }
            }
        }
        if (base > 1.0f) base = 1.0f;

        t = shz_vec4_swizzle(t, 1, 2, 3, 0);
        float inv_w = shz_invf_fsrra(t.w);
        sq = sq_next(sq);
        sq_put(sq, s->flags, t.x * inv_w, t.y * inv_w, inv_w, s->u + uv_u, s->v + uv_v, base, offset);
    }
    sq_end(sq);
}

/* The halfway vector of a sun, for one draw: the eye is taken from the
 * model's centre rather than from each vertex (fixed-function OpenGL's
 * default, the "infinite viewer"), so it is the same all across the model and
 * a vertex's highlight is one dot product. The whole model, not each mesh: a
 * body cut into blocks is several meshes, and a direction each put a step in
 * the highlight where they meet. Over 1/127, the int8 normal's length. */

static void sun_half_set(shz_vec3_t centre) {
    float vx = g_rim.x - centre.x, vy = g_rim.y - centre.y, vz = g_rim.z - centre.z;
    float vinv = shz_inv_sqrtf(vx * vx + vy * vy + vz * vz);
    float hx = g_ml[0].x + vx * vinv, hy = g_ml[0].y + vy * vinv, hz = g_ml[0].z + vz * vinv;
    float hinv = shz_inv_sqrtf(hx * hx + hy * hy + hz * hz) * (1.0f / 127.0f);
    g_half_x = hx * hinv; g_half_y = hy * hinv; g_half_z = hz * hinv;
}

/* A sun on a mesh that isn't packed (a packed one takes sun_loop, below): the
 * light is the same direction everywhere, so a vertex is two dot products and
 * nothing else. power (0 for no highlight) is how many times to square; env
 * is a constant in each of the two copies. */
typedef struct {
    float lx, ly, lz, hx, hy, hz, amb, uv_u, uv_v, rx, ry, rz, ux, uy, uz;
    float quarter, four, pad_k;
} SunConsts;

typedef struct {
    float u, v, base, offset;
} SunShade;

static inline __attribute__((always_inline))
SunShade sun_shade(const DMSVertex* s, const SunConsts* c, const int power, const bool env) {
    SunShade o;
    float nx = s->nx, ny = s->ny, nz = s->nz;
    float ndl = nx * c->lx + ny * c->ly + nz * c->lz;
    o.u = s->u + c->uv_u;
    o.v = s->v + c->uv_v;
    if (env) {
        o.u = 0.5f + (nx * c->rx + ny * c->ry + nz * c->rz);
        o.v = 0.5f - (nx * c->ux + ny * c->uy + nz * c->uz);
    }
    o.base = c->amb;
    o.offset = 0.0f;
    if (ndl > 0.0f) {
        o.base += ndl;
        if (power) {
            float ndh = nx * c->hx + ny * c->hy + nz * c->hz;
            if (ndh > 0.0f) {
                for (int p = 0; p < power; p++) ndh *= ndh;
                /* Faded in over the first stretch past the shadow line,
                 * so it does not stop dead at the vertex that crosses it.
                 * pad is how much of the mesh's shine this vertex has. */
                if (ndl < c->quarter) ndh *= ndl * c->four;
                o.offset = ndh * ((float)s->pad * c->pad_k);
            }
        }
    }
    if (o.base > 1.0f) o.base = 1.0f;
    return o;
}

static inline __attribute__((always_inline))
void render_fast_sun_impl(const DMSVertex* src, int count, int power, const bool env) {
    if (count < 1) return;
    const float k = 1.0f / 127.0f;
    const SunConsts c = {
        g_ml[0].x * k, g_ml[0].y * k, g_ml[0].z * k,
        g_half_x, g_half_y, g_half_z,
        g_ml_ambient * k, g_uv_u, g_uv_v,
        g_env_right[0], g_env_right[1], g_env_right[2],
        g_env_up[0], g_env_up[1], g_env_up[2],
        0.25f, 4.0f, 1.0f / 255.0f,
    };
    SHZ_PREFETCH(&src[0]);
    SHZ_PREFETCH(&src[1]);
    SHZ_PREFETCH(&src[2]);
    SHZ_PREFETCH(&src[3]);

    uint32_t sq = pvr_dr_addr;
    for (int i = 0; i < count; i++) {
        SHZ_PREFETCH(&src[i + 2]);
        const DMSVertex* s = &src[i];
        shz_vec4_t t = shz_xmtrx_transform_vec4(shz_vec4_init(s->x, s->y, -s->z, 1.0f));
        SunShade o = sun_shade(s, &c, power, env);
        /* The projection leaves w in the first lane (see render_fast_impl's swizzle) */
        float inv_w = shz_invf_fsrra(t.x);
        sq = sq_next(sq);
        sq_put(sq, s->flags, t.y * inv_w, t.z * inv_w, inv_w, o.u, o.v, o.base, o.offset);
    }
    sq_end(sq);
}

static __attribute__((noinline)) void render_fast_sun(const DMSVertex* src, int count,
                                                      int power, bool env) {
    if (env) render_fast_sun_impl(src, count, power, true);
    else     render_fast_sun_impl(src, count, power, false);
}

static __attribute__((noinline)) void render_fast_point(const DMSVertex* src, int count, int power) {
    if (power) render_fast_point_impl(src, count, true, power);
    else       render_fast_point_impl(src, count, false, 0);
}

/* The sun over a packed mesh: one light the same direction everywhere, so a
 * vertex is a transform and two or three dot products. The constants are
 * read into locals once, and the dot products written out rather than
 * fipr's: its operands are fixed registers the transform wants too, which
 * cost moves and spills. shine and env are constants in each copy the
 * compiler makes, and power too, so its squares are written out.
 *   k: light/2 (xyz 0), half vector (xyz 0, see below), (1-amb)/2,
 *      (1-amb)/2 + amb, 1/8, then with env the camera's right and 0.5 (u)
 *      and its up negated and 0.5 (v), without it the scroll (u, v), then
 *      at 19 LOOP_CONSTS.
 * The base is amb + clamp(N.L, 0, 1-amb): with d = N.L/2 and h = (1-amb)/2
 * that is h + amb + |d| - |d - h|. The fade, 2 clamp(d, 0, 1/8), is
 * |d| + 1/8 - |d - 1/8|. */
static inline __attribute__((always_inline))
void sun_shade_packed(float d, float hd, uint32_t nxb, float h, float h_amb, float eighth,
               const int power, const bool shine, float* base, float* offset) {
    *base = h_amb + shz_fabsf(d) - shz_fabsf(d - h);
    *offset = 0.0f;
    if (shine) {
        float o = hd + shz_fabsf(hd);                          /* 2 max(hd, 0) */
#pragma GCC unroll 8
        for (int p = 0; p < power; p++) o *= o;
        float fade = shz_fabsf(d) + eighth - shz_fabsf(d - eighth);
        *offset = o * fade * (float)(nxb & 0xffu);
    }
}

static inline __attribute__((always_inline))
uint32_t sun_loop(const DMSPacked* src, int count, const float* k, uint32_t sq,
                  const int power, const bool shine) {
    const float lx = k[0], ly = k[1], lz = k[2];
    const float hx = k[4], hy = k[5], hz = k[6];
    const float h = k[8], h_amb = k[9], eighth = k[10];
    const float su = k[11], sv = k[12];
    const uint32_t end_bit = ((const uint32_alias*)k)[19 + 1];   /* LOOP_CONSTS */
    const uint32_t cmd = ((const uint32_alias*)k)[19 + 2];
    for (const DMSPacked* s = src; s < src + count; s++) {
        SHZ_PREFETCH(s + 3);
        const float nx = s->nx, ny = s->ny, nz = s->nz;
        /* the shine and the end of the strip, in the normal's spare bits */
        const uint32_alias* w = (const uint32_alias*)s;
        __asm__("" : "+r"(w));   /* read as words apart from the floats, not moved over */
        const uint32_t nxb = w[5];
        const uint32_t nzb = w[7];

        /* The projection leaves w in the first lane */
        shz_vec4_t t = shz_xmtrx_transform_vec4(shz_vec4_init(s->x, s->y, s->z, 1.0f));
        float inv_w = shz_inv_sqrtf_fsrra(t.x * t.x);

        float base, offset;
        sun_shade_packed(nx * lx + ny * ly + nz * lz, nx * hx + ny * hy + nz * hz, nxb,
                  h, h_amb, eighth, power, shine, &base, &offset);
        uint32_t flags = cmd | ((nzb << 16) & end_bit);

        sq = sq_next(sq);
        sq_put(sq, flags, t.y * inv_w, t.z * inv_w, inv_w, s->u + su, s->v + sv, base, offset);
    }
    return sq;
}

/* The same with the environment image as the texture (env_single). Its
 * normal has four dot products (the sun, the half vector, the image's u and
 * v), more constants than the float registers hold beside the transform. As
 * Ninja does, the matrix unit does them: a run of vertices has its normals
 * put through a matrix of the four, and what they give kept, then its
 * positions through the projection. */
#define ENV_RUN 64
static inline __attribute__((always_inline))
uint32_t sun_env_loop(const DMSPacked* src, int count, const float* k, uint32_t sq,
                      const int power, const bool shine) {
    alignas(32) float lit[ENV_RUN * 4];
    shz_mat4x4_t proj;
    shz_xmtrx_store_4x4(&proj);
    const shz_vec4_t rl = shz_vec4_init(k[0], k[1], k[2], 0.0f);
    const shz_vec4_t rh = shz_vec4_init(k[4], k[5], k[6], 0.0f);
    const shz_vec4_t ru = shz_vec4_init(k[11], k[12], k[13], k[14]);
    const shz_vec4_t rv = shz_vec4_init(k[15], k[16], k[17], k[18]);
    const float h = k[8], h_amb = k[9], eighth = k[10];
    const uint32_t end_bit = ((const uint32_alias*)k)[19 + 1];   /* LOOP_CONSTS */
    const uint32_t cmd = ((const uint32_alias*)k)[19 + 2];
    for (int done = 0; done < count; done += ENV_RUN) {
        const DMSPacked* run = src + done;
        const int n = count - done < ENV_RUN ? count - done : ENV_RUN;

        shz_xmtrx_load_rows_4x4(&rl, &rh, &ru, &rv);
        float* o = lit;
        for (const DMSPacked* s = run; s < run + n; s++, o += 4) {
            SHZ_PREFETCH(s + 3);
            const uint32_alias* w = (const uint32_alias*)s;
            __asm__("" : "+r"(w));
            shz_vec4_t r = shz_xmtrx_transform_vec4(shz_vec4_init(s->nx, s->ny, s->nz, 1.0f));
            sun_shade_packed(r.x, r.y, w[5], h, h_amb, eighth, power, shine, &o[2], &o[3]);
            o[0] = r.z;
            o[1] = r.w;
        }

        shz_xmtrx_load_4x4(&proj);
        o = lit;
        for (const DMSPacked* s = run; s < run + n; s++, o += 4) {
            const uint32_alias* w = (const uint32_alias*)s;
            __asm__("" : "+r"(w));
            shz_vec4_t t = shz_xmtrx_transform_vec4(shz_vec4_init(s->x, s->y, s->z, 1.0f));
            float inv_w = shz_inv_sqrtf_fsrra(t.x * t.x);
            uint32_t flags = cmd | ((w[7] << 16) & end_bit);
            sq = sq_next(sq);
            sq_put(sq, flags, t.y * inv_w, t.z * inv_w, inv_w, o[0], o[1], o[2], o[3]);
        }
    }
    return sq;
}

static __attribute__((noinline)) uint32_t sun_c(const DMSPacked* src, int count, const float* k, uint32_t sq,
                      int power, bool env) {
    /* A copy for each power, its squares written out */
#define SUN_POWERS(loop)                                                  \
    switch (power) {                                                      \
    case 0: return loop(src, count, k, sq, 0, false);                     \
    case 1: return loop(src, count, k, sq, 1, true);                      \
    case 2: return loop(src, count, k, sq, 2, true);                      \
    case 3: return loop(src, count, k, sq, 3, true);                      \
    case 4: return loop(src, count, k, sq, 4, true);                      \
    case 5: return loop(src, count, k, sq, 5, true);                      \
    case 6: return loop(src, count, k, sq, 6, true);                      \
    case 7: return loop(src, count, k, sq, 7, true);                      \
    default: return loop(src, count, k, sq, 8, true);                     \
    }
    if (env) SUN_POWERS(sun_env_loop)
    SUN_POWERS(sun_loop)
#undef SUN_POWERS
}

static void render_fast_sun_packed(const DMSMesh* mesh, int power, bool env) {
    float amb = g_ml_ambient * (1.0f / 127.0f);
    if (amb > 1.0f) amb = 1.0f;
    float h = (1.0f - amb) * 0.5f;
    const float l = 0.5f / 127.0f;
    /* The highlight is multiplied by the vertex's shine as it is stored,
     * 0-255; the 4/255 that makes it the fade's 4 times 0-1 goes on the half
     * vector instead, as its 2^power-th root, since the highlight is the half
     * vector's dot to that power. */
    static float root[9];
    if (!root[1])
        for (int p = 1; p <= 8; p++) root[p] = powf(4.0f / 255.0f, 1.0f / (float)(1 << p));
    const float hs = power ? 0.5f * root[power] : 0.5f;
    alignas(8) float k[19 + 4] = {
        g_ml[0].x * l, g_ml[0].y * l, g_ml[0].z * l, 0.0f,
        g_half_x * hs, g_half_y * hs, g_half_z * hs, 0.0f,
        h, h + amb, 0.125f,
        g_env_right[0], g_env_right[1], g_env_right[2], 0.5f,
        -g_env_up[0], -g_env_up[1], -g_env_up[2], 0.5f,
    };
    if (!env) { k[11] = g_uv_u; k[12] = g_uv_v; }
    loop_consts(&k[19]);
    /* The loop takes z as it is; the unpacked ones negate it going in */
    shz_xmtrx_apply_scale(1.0f, 1.0f, -1.0f);
    uint64_t t0 = perf_cntr_timer_ns();
    pvr_dr_addr = sun_c((const DMSPacked*)mesh->vertices, (int)mesh->vertex_count, k,
                        pvr_dr_addr, power, env);
    int b = env ? 2 : power ? 1 : 0;
    g_stats.ns_sun[b] += perf_cntr_timer_ns() - t0;
    g_stats.verts_sun[b] += mesh->vertex_count;
    g_stats.sun_power_v += power * mesh->vertex_count;
}

/* Debug: the sun loop's parts, each over all the model's packed meshes into
 * a RAM buffer (the TA never sees it). The data is bigger than the cache, so
 * every pass reads it cold as a frame does. */
void dc_model_bench_sun(const DMSModel* model) {
    static uint32_t fake[32] __attribute__((aligned(64)));
    static volatile float sink;
    uint64_t ns[6] = {0};
    uint32_t verts = 0, pv = 0, hist[9] = {0};
    alignas(8) float k[19 + 4] = {
        0.002f, -0.003f, 0.001f, 0.0f,  0.3f, 0.4f, 0.5f, 0.0f,
        0.3f, 0.65f, 0.125f,  0.001f, 0.002f, 0.003f, 0.5f,  0.003f, 0.002f, 0.001f, 0.5f,
    };
    loop_consts(&k[19]);
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t m = 0; m < model->mesh_count; m++) {
            const DMSMesh* mesh = &model->meshes[m];
            if (!mesh->packed || !mesh->vertex_count) continue;
            const DMSPacked* src = (const DMSPacked*)mesh->vertices;
            int count = (int)mesh->vertex_count;
            int power = mesh->header.m0.txr_en ? (int)DMS_MAT_SHINE_POWER(mesh->material_flags) : 0;
            if (power > 8) power = 8;
            if (pass) { verts += count; pv += power * count; hist[power] += count; }
            shz_xmtrx_init_identity();
            shz_xmtrx_apply_scale(0.5f, 0.5f, -0.5f);
            uint64_t t;

            /* read: the vertex data alone */
            t = perf_cntr_timer_ns();
            float acc = 0.0f;
            SHZ_PREFETCH(&src[0]); SHZ_PREFETCH(&src[1]); SHZ_PREFETCH(&src[2]);
            for (int i = 0; i < count; i++) {
                SHZ_PREFETCH(&src[i + 3]);
                acc += src[i].x + src[i].nx;
            }
            sink = acc;
            (void)sink;
            if (pass) ns[0] += perf_cntr_timer_ns() - t;

            /* read + store: the vertex copied out as it is, no maths */
            t = perf_cntr_timer_ns();
            uint32_t sq = (uint32_t)fake;
            for (int i = 0; i < count; i++) {
                SHZ_PREFETCH(&src[i + 4]);
                const uint32_t nz = ((const uint32_alias*)&src[i])[7];
                sq = sq_next(sq);
                sq_put_argb(sq, PVR_CMD_VERTEX | ((nz & PACK_END) << 16), src[i].x,
                            src[i].y, src[i].z, src[i].u, src[i].v, 0xffffffffu);
            }
            if (pass) ns[1] += perf_cntr_timer_ns() - t;

            /* transform + store: the plain packed loop */
            t = perf_cntr_timer_ns();
            sq = (uint32_t)fake;
            for (int i = 0; i < count; i++) {
                SHZ_PREFETCH(&src[i + 4]);
                shz_vec4_t v = shz_xmtrx_transform_vec4(
                    shz_vec4_init(src[i].x, src[i].y, -src[i].z, 1.0f));
                const uint32_t nz = ((const uint32_alias*)&src[i])[7];
                v = shz_vec4_swizzle(v, 1, 2, 3, 0);
                float inv_w = shz_invf_fsrra(v.w);
                sq = sq_next(sq);
                sq_put_argb(sq, PVR_CMD_VERTEX | ((nz & PACK_END) << 16), v.x * inv_w,
                            v.y * inv_w, inv_w, src[i].u, src[i].v, 0xffffffffu);
            }
            if (pass) ns[2] += perf_cntr_timer_ns() - t;

            /* the sun, no highlight */
            t = perf_cntr_timer_ns();
            sun_c(src, count, k, (uint32_t)fake, 0, false);
            if (pass) ns[3] += perf_cntr_timer_ns() - t;

            /* the sun as the mesh has it */
            t = perf_cntr_timer_ns();
            sun_c(src, count, k, (uint32_t)fake, power, false);
            if (pass) ns[4] += perf_cntr_timer_ns() - t;

            /* the sun with the environment image, as the mesh has it */
            t = perf_cntr_timer_ns();
            sun_c(src, count, k, (uint32_t)fake, power, true);
            if (pass) ns[5] += perf_cntr_timer_ns() - t;
        }
    }
    if (!verts) { printf("BENCH: no packed meshes\n"); return; }
    float v = (float)verts;
    printf("BENCH: %lu packed v, avg power %.2f  ns a vertex (into RAM): read %.0f  "
           "read+store %.0f  transform+store %.0f  sun %.0f  sun+highlight %.0f  env %.0f\n",
           (unsigned long)verts, (float)pv / v, ns[0] / v, ns[1] / v, ns[2] / v,
           ns[3] / v, ns[4] / v, ns[5] / v);
    printf("BENCH: power:");
    for (int p = 0; p <= 8; p++) if (hist[p]) printf("  %d: %lu v", p, (unsigned long)hist[p]);
    printf("\n");
}

/* One mesh in the intensity mode, the header sent with it. The header's copy
 * uses the matrix registers, so mvp is loaded after it. */
static void render_fast_intensity(pvr_dr_state_t* dr, const DMSMesh* mesh,
                                  const shz_mat4x4_t* mvp) {
    bool env = env_single(mesh);
    if (env && g_ml[0].pos_w == 0.0f) ((DMSMesh*)mesh)->env_frame = dc_frame_count() + 1;
    /* The PVR adds an offset colour to textured polygons only. Showing the
     * environment, the image is its shine: no highlight, as Ninja */
    int power = mesh->header.m0.txr_en ? (int)DMS_MAT_SHINE_POWER(mesh->material_flags) : 0;
    if (power > 8) power = 8;
    send_intensity_header(dr, mesh, power != 0, env);
    shz_xmtrx_load_4x4(mvp);
    if (g_ml[0].pos_w == 0.0f && mesh->packed) {
        if (mesh->vertex_count > 0) render_fast_sun_packed(mesh, power, env);
    } else if (g_ml[0].pos_w == 0.0f) {
        render_fast_sun(mesh_verts(mesh), mesh->vertex_count, power, env);
    } else {
        render_fast_point(mesh_verts(mesh), mesh->vertex_count, power);
    }
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

        pvr_vertex_t* pv = dr_vertex();
        pv->flags = cur_flags;
        pv->x     = cur_sx;
        pv->y     = cur_sy;
        pv->z     = cur_invw;
        pv->u     = cur_u;
        pv->v     = cur_v;
        pv->argb  = 0xFF000000u;
        dr_send(pv);

        next_t = shz_vec4_swizzle(next_t, 1, 2, 3, 0);
        cur_invw  = shz_invf_fsrra(next_t.w);
        cur_sx    = next_t.x * cur_invw;
        cur_sy    = next_t.y * cur_invw;
        cur_flags = nflags;
        cur_u     = nu;
        cur_v     = nv;
    }

    pvr_vertex_t* pv = dr_vertex();
    pv->flags = cur_flags;
    pv->x     = cur_sx;
    pv->y     = cur_sy;
    pv->z     = cur_invw;
    pv->u     = cur_u;
    pv->v     = cur_v;
    pv->argb  = 0xFF000000u;
    dr_send(pv);
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
        g_clip_buffer[i].u = src[i].u + g_uv_u;
        g_clip_buffer[i].v = src[i].v + g_uv_v;
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
            pvr_vertex_t* pv = dr_vertex();
            pv->flags = src[i].flags;
            pv->x     = cv->x * inv_w;
            pv->y     = cv->y * inv_w;
            pv->z     = inv_w;
            pv->u     = cv->u;
            pv->v     = cv->v;
            pv->argb  = cv->argb;
            dr_send(pv);
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
                int next_inside = !eos && (v[j+1].flags == 0);
                int end_strip = eos || !next_inside;
                if (!in_strip) {
                    /* A strip restarted on an odd triangle would come out
                     * with the opposite winding and be culled. On its own it
                     * goes out as a triangle in the right order; with more
                     * to follow, a repeated first vertex (one degenerate
                     * triangle, 32 bytes) keeps the strip's parity */
                    if ((j & 1) && end_strip) {
                        submit_vert(&v[j-1], PVR_CMD_VERTEX);
                        submit_vert(&v[j-2], PVR_CMD_VERTEX);
                        submit_vert(&v[j],   PVR_CMD_VERTEX_EOL);
                        continue;
                    }
                    if (j & 1) submit_vert(&v[j-1], PVR_CMD_VERTEX);
                    submit_vert(&v[j-2], PVR_CMD_VERTEX);
                    submit_vert(&v[j-1], PVR_CMD_VERTEX);
                    in_strip = 1;
                }
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

/* A skinned vertex and its normal are stored in the bind pose and the bone's
 * skin matrix moves them to where the bone is now. The light is brought the
 * other way, once per run of vertices on a bone, so the loop dots it against
 * the stored normal as the static path does and nothing is rotated per
 * vertex. The 3x3 goes back through its transpose over each column's length
 * squared (a bone with scale in it); a light in a place also loses the
 * bone's translation, a sun only turns. */
static inline void light_to_bone(const shz_mat4x4_t* skin) {
    float inv2[3];
    for (int c = 0; c < 3; c++) {
        float cx = skin->elem2D[c][0], cy = skin->elem2D[c][1], cz = skin->elem2D[c][2];
        inv2[c] = 1.0f / (cx * cx + cy * cy + cz * cz);
    }
    for (int i = 0; i < g_ml_n; i++) {
        ModelLight* ml = &g_ml[i];
        float w[3] = { g_ml_model[i][0], g_ml_model[i][1], g_ml_model[i][2] };
        if (ml->pos_w != 0.0f) {
            w[0] -= skin->elem2D[3][0]; w[1] -= skin->elem2D[3][1]; w[2] -= skin->elem2D[3][2];
        }
        float out[3];
        for (int c = 0; c < 3; c++)
            out[c] = (skin->elem2D[c][0] * w[0] + skin->elem2D[c][1] * w[1] + skin->elem2D[c][2] * w[2]) * inv2[c];
        ml->x = out[0]; ml->y = out[1]; ml->z = out[2];
    }
}

enum { SKIN_PLAIN, SKIN_TINT, SKIN_LIT };

/* A vertex's bone differs from the last one's: its skin matrix goes in */
static inline __attribute__((always_inline))
void skin_bone(const DMSSkeleton* sk, int bone_id, const shz_mat4x4_t* mvp, const int mode) {
    if (mode == SKIN_LIT) light_to_bone(&sk->bones[bone_id].skinMatrix);
    shz_xmtrx_load_apply_4x4(mvp, &sk->bones[bone_id].skinMatrix);
    g_stats.skin_bone_loads++;
}

/* One loop, built three times by render_skinned with the mode fixed: with
 * shade() in the same loop GCC kept every value on the stack even unlit. Each
 * vertex is sent in the loop pass that transforms it: a bone change loads a
 * matrix, which takes every FP register, so a vertex carried over to the next
 * pass lived on the stack. */
static inline __attribute__((always_inline))
void skin_loop(const DMSVertex* src, int count, const DMSSkeleton* sk,
               const shz_mat4x4_t* mvp, const int mode) {
    uint32_t sq = pvr_dr_addr;

    SHZ_PREFETCH(&src[0]);
    SHZ_PREFETCH(&src[1]);
    SHZ_PREFETCH(&src[2]);
    SHZ_PREFETCH(&src[3]);

    const float uv_u = g_uv_u, uv_v = g_uv_v;
    int last_bone = -1;
    /* Tinted, a colour is worked out once and reused while it repeats, which
     * on a skinned model is most of the time. Lit, every vertex is its own. */
    uint32_t raw_prev = 0, tint_prev = 0;
    if (mode == SKIN_TINT) { raw_prev = src[0].argb; tint_prev = tint_argb(raw_prev); }

    for (int i = 0; i < count; i++) {
        SHZ_PREFETCH(&src[i + 4]);

        int bone_id = src[i].pad;
        if (bone_id != last_bone) {
            skin_bone(sk, bone_id, mvp, mode);
            last_bone = bone_id;
        }

        /* No -z: Z negation is baked into MVP scale */
        shz_vec4_t t = shz_xmtrx_transform_vec4(
            shz_vec4_init(src[i].x, src[i].y, src[i].z, 1.0f)
        );
        uint32_t argb = src[i].argb;
        if (mode == SKIN_LIT) argb = shade(&src[i]);
        else if (mode == SKIN_TINT) {
            if (argb != raw_prev) { raw_prev = argb; tint_prev = tint_argb(argb); }
            argb = tint_prev;
        }
        t = shz_vec4_swizzle(t, 1, 2, 3, 0);

        float inv_w = shz_invf_fsrra(t.w);
        sq = sq_next(sq);
        sq_put_argb(sq, src[i].flags, t.x * inv_w, t.y * inv_w, inv_w,
                    src[i].u + uv_u, src[i].v + uv_v, argb);
    }
    sq_end(sq);
}

static void render_skinned(const DMSVertex* src, int count,
                           const DMSSkeleton* sk,
                           const shz_mat4x4_t* mvp,
                           pvr_dr_state_t* dr) {
    (void)dr;
    if (count < 1 || !sk) return;
    if (g_lit)          skin_loop(src, count, sk, mvp, SKIN_LIT);
    else if (g_tint_on) skin_loop(src, count, sk, mvp, SKIN_TINT);
    else                skin_loop(src, count, sk, mvp, SKIN_PLAIN);
}

/* ================================================================
 * Mesh dispatch (per-mesh frustum cull + path selection)
 * ================================================================ */

/* Additive drawing (dc_draw_ex .add): every mesh goes in the transparent list
 * and is added to what is behind it, so black adds nothing. The mesh header
 * is sent with its list, blend and depth write changed. */
void dc_model_set_add(bool add) {
    g_add = add;
}

static inline void send_header(pvr_dr_state_t* dr, const DMSMesh* mesh) {
    (void)dr;
    if (!g_add) {
        dc_send_hdr(dr, &mesh->header);
        return;
    }
    alignas(32) pvr_poly_hdr_t h = mesh->header;
    h.cmd = (h.cmd & ~PVR_TA_CMD_TYPE_MASK) | (PVR_LIST_TR_POLY << PVR_TA_CMD_TYPE_SHIFT) | dc_clip_cmd;
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
                             shz_vec3_t pos, float scale, float yaw, shz_vec3_t stretch,
                             const DCCamera* cam, pvr_dr_state_t* dr) {
    float bcx = model->anim_bound_cx * stretch.x;
    float bcz = model->anim_bound_cz * stretch.z;
    float widest = stretch.x > stretch.y ? stretch.x : stretch.y;
    if (stretch.z > widest) widest = stretch.z;

    /* Rotate bounding sphere center to match model yaw */
    if (yaw != 0.0f) {
        shz_sincos_t sc = shz_sincosf(yaw);
        float rx = bcx * sc.cos - bcz * sc.sin;
        float rz = bcx * sc.sin + bcz * sc.cos;
        bcx = rx;
        bcz = rz;
    }

    shz_vec3_t wc = shz_vec3_init(pos.x + bcx * scale,
                                  pos.y + model->anim_bound_cy * stretch.y * scale,
                                  pos.z + bcz * scale);
    float wr = model->anim_bound_radius * scale * widest;

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

    /* The light into the model's space: the yaw, and the stretch as column
     * lengths, which light_to_model divides back out. The rim light is not
     * on the skinned path (its camera is worked out per static model only). */
    shz_sincos_t sc = shz_sincosf(yaw);
    float cols[9] = { sc.cos * stretch.x, 0.0f, sc.sin * stretch.x,
                      0.0f, stretch.y, 0.0f,
                      -sc.sin * stretch.z, 0.0f, sc.cos * stretch.z };
    light_to_model(pos, scale, cols);
    lights_at(mesh->bound_cx, mesh->bound_cy, mesh->bound_cz);
    if (g_glow_only) g_lit = 0;
    g_rim_on = 0;

    /* Build MVP with Z negation baked into scale */
    shz_xmtrx_load_4x4((shz_mat4x4_t*)dc_camera_get_pv(cam));
    shz_xmtrx_translate(pos.x - cam->pos.x, pos.y - cam->pos.y, -(pos.z - cam->pos.z));
    if (yaw != 0.0f) shz_xmtrx_apply_rotation_y(yaw);
    shz_xmtrx_apply_scale(scale * stretch.x, scale * stretch.y, -scale * stretch.z);

    alignas(32) shz_mat4x4_t mvp;
    shz_xmtrx_store_4x4(&mvp);

    g_stats.verts_xformed += mesh->vertex_count;
    uv_scroll_set(mesh);
    uint64_t t0 = perf_cntr_timer_ns();
    render_skinned(mesh->vertices, mesh->vertex_count, model->skeleton, &mvp, dr);
    g_stats.ns_skin += perf_cntr_timer_ns() - t0;
    g_stats.verts_skin += mesh->vertex_count;
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

/* The bloom pass (dc_set_bloom): only meshes that give off light are drawn,
 * so what lands in the small picture is the glow and nothing else.
 * g_glow_only is declared with the light above. */

void dc_model_set_glow_only(bool on) { g_glow_only = on; g_flat = 0; }

static void draw_reflections(DMSModel* model, shz_vec3_t pos, float scale, float yaw,
                             const float* rot, const DCCamera* cam, int target_list);
static void draw_cel(DMSModel* model, shz_vec3_t pos, float scale, float yaw,
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
    cam_to_model(pos, scale, rot ? rot : yaw_cols, cam);
    if (g_lit) sun_half_set(shz_vec3_scale(shz_vec3_add(model->bound_min, model->bound_max), 0.5f));
    if (g_lit && g_env && model->metallic_count) env_axes(cam, rot ? rot : yaw_cols);
    /* The glow pass draws what a mesh gives off, which a light cannot change */
    if (g_glow_only) g_lit = 0;
    /* Cel shaded solid meshes go out with their baked colours; draw_cel lays
     * the light over them */
    if (g_cel_bands && target_list == PVR_LIST_OP_POLY && !g_add) g_lit = 0;
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
            uint64_t tc0 = perf_cntr_timer_ns();
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

            g_stats.ns_cull += perf_cntr_timer_ns() - tc0;

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

                /* In the glow pass everything that is not a lamp is still
                 * drawn, in black, so it blocks what is behind it */
                g_flat = g_glow_only && !(mesh->material_flags & DMS_MAT_GLOW);
                rim_set(mesh);
                lights_at(mesh->bound_cx, mesh->bound_cy, mesh->bound_cz);
                uv_scroll_set(mesh);
                bool clipped = (batch[i] & 0x80000000u) != 0;
                uint64_t t0 = perf_cntr_timer_ns();
                if (!clipped && intensity_ok(mesh)) {
                    uint64_t c0 = perf_cntr_count(PRFC1);
                    render_fast_intensity(dr, mesh, &mvp);   /* sends its own header, */
                    if (env_single(mesh)) {
                        g_stats.ns_env += perf_cntr_timer_ns() - t0;
                        g_stats.stall_env += perf_cntr_count(PRFC1) - c0;
                        g_stats.verts_env += mesh->vertex_count;
                    } else {
                        g_stats.ns_lit += perf_cntr_timer_ns() - t0;
                        g_stats.stall_lit += perf_cntr_count(PRFC1) - c0;
                        g_stats.verts_lit += mesh->vertex_count;
                    }
                    last_hdr = NULL;                         /* so the next mesh does too */
                    xm = XM_MVP;
                    g_stats.verts_xformed += mesh->vertex_count;
                    continue;
                }

                uint64_t th0 = perf_cntr_timer_ns();
                if (!last_hdr || memcmp(last_hdr, &mesh->header, sizeof(pvr_poly_hdr_t))) {
                    send_header(dr, mesh);
                    last_hdr = &mesh->header;
                    xm = XM_OTHER;
                    g_stats.hdrs_sent++;
                }

                if (xm != XM_MVP) {
                    shz_xmtrx_load_4x4(&mvp);
                    xm = XM_MVP;
                }
                g_stats.ns_hdr += perf_cntr_timer_ns() - th0;
                int shaded = g_lit || g_rim_on || g_tint_on;
                if (clipped) {
                    g_stats.verts_clipped += mesh->vertex_count;
                    render_clipped(mesh_verts(mesh), mesh->vertex_count, dr, shaded);
                    g_stats.ns_clip += perf_cntr_timer_ns() - t0;
                } else {
                    g_stats.verts_xformed += mesh->vertex_count;
                    if (g_flat)      render_fast_flat(mesh_verts(mesh), mesh->vertex_count, dr);
                    else if (g_lit && mesh->packed && !g_rim_on && !g_tint_on) {
                        uint64_t t1 = perf_cntr_timer_ns();
                        uint64_t c1 = perf_cntr_count(PRFC1);
                        if (render_packed_lights(dr, mesh, &mvp))
                            last_hdr = NULL;   /* sent its own: the next mesh sends again */
                        uint64_t dt = perf_cntr_timer_ns() - t1;
                        g_stats.ns_lights += dt;
                        g_stats.stall_lights += perf_cntr_count(PRFC1) - c1;
                        g_stats.verts_lights += mesh->vertex_count;
                        g_stats.ns_plain -= dt;          /* plain keeps the rest: header, setup */
                        g_stats.verts_plain -= mesh->vertex_count;
                        xm = XM_OTHER;   /* the lights are in the matrix now */
                    }
                    else if (shaded) (g_uv_on ? render_fast_lit_uv : render_fast_lit)(mesh_verts(mesh), mesh->vertex_count, dr);
                    else if (mesh->packed) render_fast_packed(mesh);
                    else             (g_uv_on ? render_fast_uv : render_fast)(mesh_verts(mesh), mesh->vertex_count, dr);
                    g_stats.ns_plain += perf_cntr_timer_ns() - t0;
                    g_stats.verts_plain += mesh->vertex_count;
                }
            }
        }
    }
}

/* Skinned models: one mesh at a time */
static void draw_skinned_list(DMSModel* model, shz_vec3_t pos, float scale,
                              float yaw, shz_vec3_t stretch, const DCCamera* cam, int target_list) {
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
        draw_skinned_mesh(mesh, model, pos, scale, yaw, stretch, cam, dr);
    }
}

void dc_model_draw_list_rotated(DMSModel* model, shz_vec3_t pos, float scale,
                                float yaw, const DCCamera* cam, int target_list) {
    if (!model || model->mesh_count == 0) return;

    if (model->skeleton)
        draw_skinned_list(model, pos, scale, yaw, shz_vec3_init(1.0f, 1.0f, 1.0f), cam, target_list);
    else {
        draw_blocks_list(model, pos, scale, yaw, NULL, cam, target_list);
        if (!g_add) {
            uint64_t t0 = perf_cntr_timer_ns(), c0 = perf_cntr_count(PRFC1);
            draw_reflections(model, pos, scale, yaw, NULL, cam, target_list);
            g_stats.ns_reflect += perf_cntr_timer_ns() - t0;
            g_stats.stall_reflect += perf_cntr_count(PRFC1) - c0;
        }
        if (!g_add) draw_cel(model, pos, scale, yaw, NULL, cam, target_list);
    }
}

/* A skinned model pulled by a different factor on each of its own axes,
 * on top of scale: squash and stretch. The bones move it, then it is
 * scaled, so a squashed one still animates. Its light is close, not exact. */
void dc_model_draw_list_stretched(DMSModel* model, shz_vec3_t pos, float scale, float yaw,
                                  shz_vec3_t stretch, const DCCamera* cam, int target_list) {
    if (!model || model->mesh_count == 0 || !model->skeleton) return;
    if (stretch.x == 0.0f) stretch.x = 1.0f;
    if (stretch.y == 0.0f) stretch.y = 1.0f;
    if (stretch.z == 0.0f) stretch.z = 1.0f;
    draw_skinned_list(model, pos, scale, yaw, stretch, cam, target_list);
}

void dc_model_draw_list_oriented(DMSModel* model, shz_vec3_t pos, float scale,
                                 const float rot[9], const DCCamera* cam, int target_list) {
    if (!model || model->mesh_count == 0 || model->skeleton) return;

    draw_blocks_list(model, pos, scale, 0.0f, rot, cam, target_list);
    if (!g_add) {
        uint64_t t0 = perf_cntr_timer_ns(), c0 = perf_cntr_count(PRFC1);
        draw_reflections(model, pos, scale, 0.0f, rot, cam, target_list);
        g_stats.ns_reflect += perf_cntr_timer_ns() - t0;
        g_stats.stall_reflect += perf_cntr_count(PRFC1) - c0;
    }
    if (!g_add) draw_cel(model, pos, scale, 0.0f, rot, cam, target_list);
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

/* Anything glossy enough reflects, metal or not: a car body is mostly the sky
 * it shows. Rough metal (brushed, cast) does not: a sharp image on it reads
 * as chrome. Metal's reflection is tinted by its colour, paint and plastic's
 * is the image as it is; both at half the strength of their highlight. */

/* Metal shows the environment, and so does anything smooth enough (glass).
 * Paint and plastic keep their highlight, which costs nothing, where a
 * reflection sends them all again */
bool dc_model_reflects(const DMSMesh* mesh) {
    return (mesh->material_flags & (DMS_MAT_METALLIC | DMS_MAT_GLOSSY)) != 0;
}

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
    /* The same triangles as the solid draw, so the same depth: with the
     * default (greater) the shine passed or failed pixel by pixel */
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    pvr_poly_compile(&g_env_shine_hdr, &cxt);

    /* Front faces only from here on */
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, tex->pvrformat, tex->width, tex->height,
                     tex->ptr, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_CW;
    cxt.txr.env = PVR_TXRENV_REPLACE;
    cxt.blend.src = PVR_BLEND_ONE;
    cxt.blend.dst = PVR_BLEND_ZERO;
    cxt.blend.dst_enable = PVR_BLEND_ENABLE;
    pvr_poly_compile(&g_env_accum_hdr, &cxt);

    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.culling = PVR_CULLING_CW;
    cxt.blend.src = PVR_BLEND_SRCALPHA;
    cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
    cxt.blend.src_enable = PVR_BLEND_ENABLE;
    pvr_poly_compile(&g_env_flush_hdr, &cxt);
}

/* The mesh's vertices with their UVs swapped for a lookup into the
 * environment image. right and up are the camera's axes in model space,
 * already scaled so a full-length normal gives 0.5. */
/* A vertex's colour in a reflection pass. shine_a, when not 0, is the
 * highlight's alpha, scaled by how much of the mesh's shine the vertex has */
static inline __attribute__((always_inline))
uint32_t env_colour(const DMSVertex* s, uint32_t argb_and, uint32_t argb_or, uint32_t shine_a) {
    uint32_t argb = (s->argb & argb_and) | argb_or;
    if (shine_a) argb = (argb & 0x00FFFFFFu) | ((shine_a * s->pad + 255) >> 8) << 24;
    return g_rim_on ? rim_light(argb, s) : argb;
}

/* The environment pass of a mesh fully in view: the lookup made as each
 * vertex is sent, laid out as render_fast_impl, so the mesh is not copied
 * first. The clipped way still copies (env_vertices). */
static void render_env_fast(const DMSVertex* src, int count, pvr_dr_state_t* dr,
                            const float* right, const float* up,
                            uint32_t argb_and, uint32_t argb_or, uint32_t shine_a) {
    if (count < 1) return;
    uint32_t sq = pvr_dr_addr;
    const float rx = right[0], ry = right[1], rz = right[2];
    const float ux = up[0], uy = up[1], uz = up[2];
    SHZ_PREFETCH(&src[0]);
    SHZ_PREFETCH(&src[1]);
    SHZ_PREFETCH(&src[2]);
    SHZ_PREFETCH(&src[3]);

    shz_vec4_t t0 = shz_xmtrx_transform_vec4(shz_vec4_init(src[0].x, src[0].y, -src[0].z, 1.0f));
    t0 = shz_vec4_swizzle(t0, 1, 2, 3, 0);
    float    cur_invw  = shz_invf_fsrra(t0.w);
    float    cur_sx    = t0.x * cur_invw;
    float    cur_sy    = t0.y * cur_invw;
    uint32_t cur_flags = src[0].flags;
    float    n0x = src[0].nx, n0y = src[0].ny, n0z = src[0].nz;
    float    cur_u     = 0.5f + (n0x * rx + n0y * ry + n0z * rz);
    float    cur_v     = 0.5f - (n0x * ux + n0y * uy + n0z * uz);
    uint32_t cur_argb  = env_colour(&src[0], argb_and, argb_or, shine_a);

    for (int i = 1; i < count; i++) {
        SHZ_PREFETCH(&src[i + 4]);
        const DMSVertex* s = &src[i];
        shz_vec4_t next_t = shz_xmtrx_transform_vec4(shz_vec4_init(s->x, s->y, -s->z, 1.0f));
        float nx = s->nx, ny = s->ny, nz = s->nz;
        uint32_t nflags = s->flags;
        float nu = 0.5f + (nx * rx + ny * ry + nz * rz);
        float nv = 0.5f - (nx * ux + ny * uy + nz * uz);
        uint32_t nargb = env_colour(s, argb_and, argb_or, shine_a);

        sq = sq_next(sq);
        sq_put_argb(sq, cur_flags, cur_sx, cur_sy, cur_invw, cur_u, cur_v, cur_argb);

        next_t = shz_vec4_swizzle(next_t, 1, 2, 3, 0);
        cur_invw  = shz_invf_fsrra(next_t.w);
        cur_sx    = next_t.x * cur_invw;
        cur_sy    = next_t.y * cur_invw;
        cur_flags = nflags;
        cur_u     = nu;
        cur_v     = nv;
        cur_argb  = nargb;
    }

    sq = sq_next(sq);
    sq_put_argb(sq, cur_flags, cur_sx, cur_sy, cur_invw, cur_u, cur_v, cur_argb);
    sq_end(sq);
}

/* render_env_fast reading a packed mesh: the normal is floats already, the
 * colour the mesh's (argb), the shine the low byte of nx and the end of a
 * strip bit 12 of nz. Written like sun_loop. k: the camera's up negated and
 * 0.5 (v), its right and 0.5 (u), then as words the colour, the alpha's
 * (a * shine + b) >> 8 -- a the shine's, or 0 and b the colour's own alpha --
 * and LOOP_CONSTS (whose first, 2.0f, it doesn't use). */
static __attribute__((noinline))
uint32_t refl_loop(const DMSPacked* src, int count, const float* k0, uint32_t sq) {
    const uint32_alias* kw = (const uint32_alias*)(k0 + 8);
    const uint32_t rgb = kw[0], sa = kw[1], sb = kw[2];
    const uint32_t end_bit = kw[3 + 1], cmd = kw[3 + 2];
    const float* volatile kstart = k0;
    for (int i = 0; i < count; i++) {
        SHZ_PREFETCH(&src[i + 3]);
        const float* k = kstart;
        const float x = src[i].x, y = src[i].y, z = src[i].z;
        const float nx = src[i].nx, ny = src[i].ny, nz = src[i].nz;
        const uint32_t nxb = ((const uint32_alias*)&src[i])[5];
        const uint32_t nzb = ((const uint32_alias*)&src[i])[7];

        /* The projection leaves w in the first lane */
        shz_vec4_t t = shz_xmtrx_transform_vec4(shz_vec4_init(x, y, z, 1.0f));
        float inv_w = shz_inv_sqrtf_fsrra(t.x * t.x);

        K4 m = k4(&k);
        float v = shz_dot8f(nx, ny, nz, 1.0f, m.a, m.b, m.c, m.d);
        m = k4(&k);
        float u = shz_dot8f(nx, ny, nz, 1.0f, m.a, m.b, m.c, m.d);

        uint32_t argb = ((((nxb & 0xffu) * sa + sb) >> 8) << 24) | rgb;
        uint32_t flags = cmd | ((nzb << 16) & end_bit);

        sq = sq_next(sq);
        sq_put_argb2(sq, flags, t.y * inv_w, t.z * inv_w, inv_w, u, v, argb, 0);
    }
    return sq;
}

static void render_env_packed(const DMSPacked* src, int count, pvr_dr_state_t* dr,
                              const float* right, const float* up,
                              uint32_t argb, uint32_t shine_a) {
    (void)dr;
    if (count < 1) return;
    alignas(4) float k[8 + 3 + 4] = {
        -up[0], -up[1], -up[2], 0.5f,
        right[0], right[1], right[2], 0.5f,
    };
    uint32_t w[3] = { argb & 0x00FFFFFFu, shine_a,
                      shine_a ? 255u : (argb >> 24) << 8 };
    memcpy(&k[8], w, sizeof w);
    loop_consts(&k[11]);
    /* The loop takes z as it is; the C ones negate it going in */
    shz_xmtrx_apply_scale(1.0f, 1.0f, -1.0f);
    pvr_dr_addr = refl_loop(src, count, k, pvr_dr_addr);
}

static const DMSVertex* env_vertices(const DMSMesh* mesh, const float* right,
                                     const float* up, uint32_t argb_and, uint32_t argb_or,
                                     uint32_t shine_a) {
    if (mesh->vertex_count > g_env_verts_size) {
        free(g_env_verts);
        g_env_verts = memalign(32, mesh->vertex_count * sizeof(DMSVertex));
        g_env_verts_size = g_env_verts ? mesh->vertex_count : 0;
        if (!g_env_verts) return NULL;
    }
    const DMSVertex* src = mesh_verts(mesh);
    DMSVertex* dst = g_env_verts;
    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
        SHZ_PREFETCH(&src[i + 4]);
        float nx = src[i].nx, ny = src[i].ny, nz = src[i].nz;
        dst[i].x = src[i].x;
        dst[i].y = src[i].y;
        dst[i].z = src[i].z;
        dst[i].u = 0.5f + (nx * right[0] + ny * right[1] + nz * right[2]);
        dst[i].v = 0.5f - (nx * up[0] + ny * up[1] + nz * up[2]);
        dst[i].argb = env_colour(&src[i], argb_and, argb_or, shine_a);
        dst[i].flags = src[i].flags;
    }
    return dst;
}

static void draw_reflections(DMSModel* model, shz_vec3_t pos, float scale, float yaw,
                             const float* rot, const DCCamera* cam, int target_list) {
    if (!g_env || !model->metallic_count) return;
    /* A reflection is caught light, not given off: in the glow pass only a
     * metallic mesh with an Emission colour goes in, with what it reflects */
    if (g_glow_only && !model->glow_count) return;
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
    env_axes(cam, cols);
    const float* right = g_env_right;
    const float* up = g_env_up;

    cam_to_model(pos, scale, cols, cam);
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
        if (!dc_model_reflects(mesh)) continue;
        if (mesh->material_flags & DMS_MAT_COLLISION_ONLY) continue;
        int mirror = (mesh->material_flags & DMS_MAT_MIRROR) != 0;
        int metal = (mesh->material_flags & DMS_MAT_METALLIC) != 0;
        if (mirror != want_mirror) continue;
        if (g_glow_only && !(mesh->material_flags & DMS_MAT_GLOW)) continue;
        rim_set(mesh);

        shz_vec3_t tc = turn_centre(rot, yaw, sc, mesh->bound_cx, mesh->bound_cy, mesh->bound_cz);
        shz_vec3_t mc = shz_vec3_init(pos.x + tc.x * scale, pos.y + tc.y * scale,
                                      pos.z + tc.z * scale);
        float mr = mesh->bound_radius * scale;
        float nd;
        shz_xmtrx_load_4x4((shz_mat4x4_t*)&fr->side_planes);
        if (!sphere_visible(fr, mc, mr, &nd)) continue;
        int clip = nd < mr && nd > -mr;
        /* Already shown in its own pass this frame (env_single). Not when
         * it went another way there: clipped, cel, tinted, the glow pass */
        if (mesh->env_frame == dc_frame_count() + 1) continue;

        /* See-through with a texture of its own: Katana's passes. Anything
         * else gets the image added over it. */
        int tid = mesh->texture_id;
        int glass = (mesh->material_flags & 0x3) == 2 && tid >= 0 &&
                    tid < model->texture_count && model->textures[tid].ptr;

        /* The pass's colour: a mirror its own, glass white, and a shine the
         * metal's colour or white, as bright as the vertex shines */
        uint32_t argb_and = mirror ? 0xFFFFFFFFu : glass || !metal ? 0 : 0x00FFFFFFu;
        uint32_t argb_or  = mirror ? 0 : glass ? 0xFFFFFFFFu : metal ? 0 : 0x00FFFFFFu;
        uint32_t shine_a  = mirror || glass ? 0 : DMS_MAT_SHINE_STRENGTH(mesh->material_flags) >> 1;
        /* Only the clipped way and glass's later passes need it copied */
        const DMSVertex* env = NULL;
        if (clip || glass) {
            env = env_vertices(mesh, right, up, argb_and, argb_or, shine_a);
            if (!env) return;
        }

        pvr_poly_hdr_t cut_hdr __attribute__((aligned(32)));
        const pvr_poly_hdr_t* hdrs[3] = { mirror ? &g_env_mirror_hdr : &g_env_shine_hdr, NULL, NULL };
        const DMSVertex*      vtx[3]  = { env, NULL, NULL };
        int passes = 1;
        if (mirror) {
            g_stats.meshes_drawn++;
        } else if (glass) {
            const dttex_info_t* tex = &model->textures[tid];
            pvr_poly_cxt_t cxt;
            pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, tex->pvrformat, tex->width, tex->height,
                             tex->ptr, ((mesh->material_flags >> 9) & 1) ? PVR_FILTER_NONE
                                                                        : PVR_FILTER_BILINEAR);
            cxt.gen.culling = PVR_CULLING_CW;
            cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
            cxt.blend.src = PVR_BLEND_DESTALPHA;
            cxt.blend.dst = PVR_BLEND_SRCALPHA;
            cxt.blend.dst_enable = PVR_BLEND_ENABLE;
            pvr_poly_compile(&cut_hdr, &cxt);
            hdrs[0] = &g_env_accum_hdr;
            hdrs[1] = &cut_hdr;
            hdrs[2] = &g_env_flush_hdr;
            vtx[1] = vtx[2] = mesh_verts(mesh);
            passes = 3;
        }

        /* Glass's passes go in together or not at all: the first alone
         * leaves the accumulation half done, and the glass shows it */
        if (passes > 1 && vtxbuf_need(mesh) * passes > g_vtx_left) {
            vtxbuf_warn_need(vtxbuf_need(mesh) * passes);
            g_stats.meshes_vtxfull++;
            continue;
        }
        for (int p = 0; p < passes; p++) {
            if (vtxbuf_full(mesh, clip)) return;
            if (!dr) {
                dc_list_begin(target_list);
                dr = dc_dr_state();
            }
            dc_send_hdr(dr, hdrs[p]);
            shz_xmtrx_load_4x4(&mvp);
            g_stats.tris_drawn += mesh->tri_count;
            g_stats.verts_reflect += mesh->vertex_count;
            if (clip) {
                g_stats.verts_clipped += mesh->vertex_count;
                render_clipped(vtx[p], mesh->vertex_count, dr, 0);
            } else {
                g_stats.verts_xformed += mesh->vertex_count;
                if (vtx[p])
                    render_fast(vtx[p], mesh->vertex_count, dr);
                else if (mesh->packed && !g_rim_on)
                    render_env_packed((const DMSPacked*)mesh->vertices, mesh->vertex_count,
                                      dr, right, up, (mesh->colour & argb_and) | argb_or,
                                      shine_a);
                else
                    render_env_fast(mesh_verts(mesh), mesh->vertex_count, dr, right, up,
                                    argb_and, argb_or, shine_a);
            }
        }
    }
}

/* ================================================================
 * Cel shading pass
 *
 * The solid meshes again, in the TR list, with U swapped for how much light
 * each vertex catches. The ramp (cel_setup) multiplies the frame by it.
 * ================================================================ */

static DMSVertex* g_cel_verts;
static uint32_t   g_cel_verts_size;

static const DMSVertex* cel_vertices(const DMSMesh* mesh, uint32_t argb) {
    if (mesh->vertex_count > g_cel_verts_size) {
        free(g_cel_verts);
        g_cel_verts = memalign(32, mesh->vertex_count * sizeof(DMSVertex));
        g_cel_verts_size = g_cel_verts ? mesh->vertex_count : 0;
        if (!g_cel_verts) return NULL;
    }
    /* Onto the middle of the first and last texels, so 0 and full light land
     * inside the ramp */
    const float k = (CEL_RAMP_W - 1.0f) / (CEL_RAMP_W * 127.0f);
    const float u0 = 0.5f / CEL_RAMP_W;
    const DMSVertex* src = mesh_verts(mesh);
    DMSVertex* dst = g_cel_verts;
    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
        SHZ_PREFETCH(&src[i + 4]);
        dst[i].x = src[i].x;
        dst[i].y = src[i].y;
        dst[i].z = src[i].z;
        dst[i].u = u0 + catch_light(&src[i], &g_ml[0]) * k;
        dst[i].v = 0.5f;
        dst[i].argb = argb;
        dst[i].flags = src[i].flags;
    }
    return dst;
}

static uint32_t colour_byte(float c) {
    return c >= 1.0f ? 255 : c <= 0.0f ? 0 : (uint32_t)(c * 255.0f);
}

static void draw_cel(DMSModel* model, shz_vec3_t pos, float scale, float yaw,
                     const float* rot, const DCCamera* cam, int target_list) {
    if (!g_cel_bands || target_list != PVR_LIST_TR_POLY) return;
    if (g_glow_only || !model->opaque_count) return;

    shz_sincos_t sc = shz_sincosf(yaw);
    float yaw_cols[9] = { sc.cos, 0.0f, sc.sin,  0.0f, 1.0f, 0.0f,  -sc.sin, 0.0f, sc.cos };
    light_to_model(pos, scale, rot ? rot : yaw_cols);
    if (!g_lit) return;
    g_lit = 0;   /* the colour is in the ramp, not the vertices */

    /* The light's colour, which the ramp is modulated by */
    uint32_t argb = 0xFFFFFFFFu;
    if (g_light.r > 0.0f || g_light.g > 0.0f || g_light.b > 0.0f)
        argb = 0xFF000000u | (colour_byte(g_light.r) << 16) |
               (colour_byte(g_light.g) << 8) | colour_byte(g_light.b);

    const WorldFrustum* fr = dc_camera_get_frustum(cam);
    alignas(32) shz_mat4x4_t mvp;
    shz_xmtrx_load_4x4((shz_mat4x4_t*)dc_camera_get_pv(cam));
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

    uint32_t skip = DMS_MAT_COLLISION_ONLY | DMS_MAT_MARKER |
                    (g_env ? DMS_MAT_MIRROR : 0);
    pvr_dr_state_t* dr = NULL;

    for (uint32_t m = 0; m < model->opaque_count; m++) {
        DMSMesh* mesh = &model->meshes[m];
        if (mesh->material_flags & skip) continue;
        if (model->vol_on == m + 1) continue;

        shz_vec3_t tc = turn_centre(rot, yaw, sc, mesh->bound_cx, mesh->bound_cy, mesh->bound_cz);
        shz_vec3_t mc = shz_vec3_init(pos.x + tc.x * scale, pos.y + tc.y * scale,
                                      pos.z + tc.z * scale);
        float mr = mesh->bound_radius * scale;
        float nd;
        shz_xmtrx_load_4x4((shz_mat4x4_t*)&fr->side_planes);
        if (!sphere_visible(fr, mc, mr, &nd)) continue;
        int clip = nd < mr && nd > -mr;

        if (vtxbuf_full(mesh, clip)) return;
        const DMSVertex* v = cel_vertices(mesh, argb);
        if (!v) return;
        if (!dr) {
            dc_list_begin(target_list);
            dr = dc_dr_state();
        }
        dc_send_hdr(dr, &g_cel_hdr);
        shz_xmtrx_load_4x4(&mvp);
        g_stats.tris_drawn += mesh->tri_count;
        if (clip) {
            g_stats.verts_clipped += mesh->vertex_count;
            render_clipped(v, mesh->vertex_count, dr, 0);
        } else {
            g_stats.verts_xformed += mesh->vertex_count;
            render_fast(v, mesh->vertex_count, dr);
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
    while (sk->currentTime >= anim->duration) {
        sk->currentTime -= anim->duration;
        sk->loops++;
    }
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

    float minx =  1e18f, maxx = -1e18f, miny =  1e18f, maxy = -1e18f, minz =  1e18f, maxz = -1e18f;
    for (int i = 0; i < sk->boneCount; i++) {
        if (!sk->bones[i].skinned) continue;
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
    if (minx > maxx) return;            /* no bone owns a vertex */

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
    model->skeleton->loops = 0;
}

float dc_model_anim_length(const DMSModel* model, int anim_index) {
    if (!model || !model->skeleton || anim_index < 0 || anim_index >= model->skeleton->animCount) return 0.0f;
    return model->skeleton->animations[anim_index].duration;
}

float dc_model_anim_time(const DMSModel* model) {
    return model && model->skeleton ? model->skeleton->currentTime : 0.0f;
}

bool dc_model_anim_done(const DMSModel* model) {
    return model && model->skeleton && model->skeleton->loops > 0;
}

void dc_model_anim_restart(DMSModel* model) {
    if (!model || !model->skeleton) return;
    model->skeleton->currentTime = 0.0f;
    model->skeleton->loops = 0;
}

int dc_model_get_anim(DMSModel* model) {
    if (!model || !model->skeleton) return -1;
    return model->skeleton->currentAnim;
}

int dc_model_anim_index(const DMSModel* model, const char* name) {
    if (!model || !model->skeleton || !name) return -1;
    for (int i = 0; i < model->skeleton->animCount; i++)
        if (strcmp(model->skeleton->animations[i].name, name) == 0) return i;
    return -1;
}

/* ================================================================
 * Loading
 * ================================================================ */

/* A file read whole, then parsed like a stream */
typedef struct { uint8_t* data; size_t size, pos; } MemFile;

/* The box round a mesh's vertices, and the model's round all of them */
static void mesh_bounds(DMSMesh* mesh) {
    float lo[3] = {  1e30f,  1e30f,  1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t v = 0; v < mesh->vertex_count; v++) {
        const DMSVertex* p = &mesh->vertices[v];
        float c[3] = { p->x, p->y, p->z };
        for (int k = 0; k < 3; k++) {
            if (c[k] < lo[k]) lo[k] = c[k];
            if (c[k] > hi[k]) hi[k] = c[k];
        }
    }
    for (int k = 0; k < 3; k++) { mesh->bound_min[k] = lo[k]; mesh->bound_max[k] = hi[k]; }
}

/* Whether every vertex has the same colour (render_fast_intensity) */
static void one_colour_mark(DMSMesh* mesh) {
    mesh->material_flags &= ~DMS_MAT_ONE_COLOUR;
    if (!mesh->vertex_count) return;
    uint32_t c = mesh->vertices[0].argb;
    for (uint32_t v = 1; v < mesh->vertex_count; v++)
        if (mesh->vertices[v].argb != c) return;
    mesh->material_flags |= DMS_MAT_ONE_COLOUR;
}

static void model_bounds(DMSModel* model) {
    float lo[3] = {  1e30f,  1e30f,  1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t m = 0; m < model->mesh_count; m++) {
        const DMSMesh* mesh = &model->meshes[m];
        if (mesh->vertex_count == 0) continue;
        for (int k = 0; k < 3; k++) {
            if (mesh->bound_min[k] < lo[k]) lo[k] = mesh->bound_min[k];
            if (mesh->bound_max[k] > hi[k]) hi[k] = mesh->bound_max[k];
        }
    }
    model->bound_min = shz_vec3_init(lo[0], lo[1], lo[2]);
    model->bound_max = shz_vec3_init(hi[0], hi[1], hi[2]);
}

DCBounds dc_model_bounds(const DMSModel* model, const char* material) {
    DCBounds b = { shz_vec3_init(1e30f, 1e30f, 1e30f), shz_vec3_init(-1e30f, -1e30f, -1e30f) };
    if (!model) return b;
    if (!material) { b.min = model->bound_min; b.max = model->bound_max; return b; }
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t m = 0; m < model->mesh_count; m++) {
        const DMSMesh* mesh = &model->meshes[m];
        if (!model->material_names || strcasecmp(model->material_names[m], material) || mesh->vertex_count == 0) continue;
        for (int k = 0; k < 3; k++) {
            if (mesh->bound_min[k] < lo[k]) lo[k] = mesh->bound_min[k];
            if (mesh->bound_max[k] > hi[k]) hi[k] = mesh->bound_max[k];
        }
    }
    b.min = shz_vec3_init(lo[0], lo[1], lo[2]);
    b.max = shz_vec3_init(hi[0], hi[1], hi[2]);
    return b;
}

/* dc_model_recolour: every vertex of the meshes wearing the material, its
 * colour from the function. The alpha stays as it was. */
int dc_model_recolour(DMSModel* model, const char* material,
                      uint32_t (*colour)(shz_vec3_t pos, void* user), void* user) {
    if (!model || !colour || !model->material_names) return 0;
    int n = 0;
    for (uint32_t m = 0; m < model->mesh_count; m++) {
        if (strcasecmp(model->material_names[m], material)) continue;
        dc_model_mesh_unpack(&model->meshes[m]);
        DMSVertex* v = model->meshes[m].vertices;
        for (uint32_t i = 0; i < model->meshes[m].vertex_count; i++, n++) {
            uint32_t c = colour(shz_vec3_init(v[i].x, v[i].y, v[i].z), user);
            v[i].argb = (v[i].argb & 0xff000000u) | (c & 0x00ffffffu);
        }
        one_colour_mark(&model->meshes[m]);
        mesh_pack(&model->meshes[m], model->skeleton != NULL);
    }
    return n;
}

/* dc_model_texture_colours: read back out of video memory, where the texture
 * went at load, a 32 bits at a time (the video memory is not read a byte at a
 * time), then read by dt_read_colours */
int dc_model_texture_colours(const DMSModel* model, const char* material,
                             uint32_t* out, int n) {
    if (!model || !out || n <= 0 || !model->material_names) return 0;
    for (uint32_t m = 0; m < model->mesh_count; m++) {
        if (strcasecmp(model->material_names[m], material)) continue;
        int tid = model->meshes[m].texture_id;
        if (tid < 0 || tid >= model->texture_count || !model->textures[tid].ptr) return 0;
        const dttex_info_t* t = &model->textures[tid];
        size_t head = fDtGetHeaderSize(&t->hdr);
        size_t size = t->hdr.chunk_size;
        uint32_t* buf = malloc((size + 3) & ~(size_t)3);
        if (!buf) return 0;
        memcpy(buf, &t->hdr, sizeof(t->hdr) < head ? sizeof(t->hdr) : head);
        const volatile uint32_t* vram = (const volatile uint32_t*)t->ptr;
        for (size_t i = 0; i < (size - head) / 4; i++) buf[head / 4 + i] = vram[i];
        int ok = dt_read_colours(buf, size, out, (uint32_t)n);
        free(buf);
        return ok;
    }
    return 0;
}

/* The clip buffer is shared and grows to the largest mesh ever loaded */
static void clip_buffer_reserve(uint32_t max_verts) {
    if (max_verts <= g_clip_buffer_size) return;
    ClipVertex* grown = memalign(32, max_verts * sizeof(ClipVertex));
    if (!grown) return;
    free(g_clip_buffer);
    g_clip_buffer = grown;
    g_clip_buffer_size = max_verts;
}

DMSModel* dc_model_cube(void) {
    DMSModel* model = calloc(1, sizeof(DMSModel));
    if (!model) return NULL;
    model->mesh_count = model->opaque_count = 1;
    model->meshes = memalign(32, sizeof(DMSMesh));
    model->blocks = malloc(sizeof(DMSBlock));
    model->runs = malloc(sizeof(DMSBlockRun));
    model->material_names = malloc(sizeof(*model->material_names));
    DMSVertex* v = memalign(32, 24 * sizeof(DMSVertex));
    if (!model->meshes || !model->blocks || !model->runs || !model->material_names || !v) {
        free(model->meshes); free(model->blocks); free(model->runs); free(model->material_names); free(v); free(model);
        return NULL;
    }
    memset(model->meshes, 0, sizeof(DMSMesh));
    memset(v, 0, 24 * sizeof(DMSVertex));

    /* Six faces, each a strip of four. n is the face normal, a and b the two
     * directions across it */
    static const float faces[6][3][3] = {
        { { 0,  0,  1 }, { 1, 0, 0 }, { 0, 1, 0 } },
        { { 0,  0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } },
        { { 1,  0,  0 }, { 0, 0, -1 }, { 0, 1, 0 } },
        { { -1, 0,  0 }, { 0, 0, 1 }, { 0, 1, 0 } },
        { { 0,  1,  0 }, { 1, 0, 0 }, { 0, 0, -1 } },
        { { 0, -1,  0 }, { 1, 0, 0 }, { 0, 0, 1 } },
    };
    static const float corner[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
    for (int f = 0; f < 6; f++) {
        const float *n = faces[f][0], *a = faces[f][1], *b = faces[f][2];
        for (int c = 0; c < 4; c++) {
            DMSVertex* p = &v[f * 4 + c];
            float cu = corner[c][0], cv = corner[c][1];
            p->x = 0.5f * (n[0] + a[0] * cu + b[0] * cv);
            p->y = 0.5f * (n[1] + a[1] * cu + b[1] * cv);
            p->z = 0.5f * (n[2] + a[2] * cu + b[2] * cv);
            p->u = cu > 0.0f ? 1.0f : 0.0f;
            p->v = cv > 0.0f ? 1.0f : 0.0f;
            p->argb = 0xFFFFFFFFu;
            p->nx = (int8_t)(n[0] * 127.0f); p->ny = (int8_t)(n[1] * 127.0f); p->nz = (int8_t)(n[2] * 127.0f);
            p->flags = c == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        }
    }

    DMSMesh* mesh = &model->meshes[0];
    mesh->vertex_count = 24;
    mesh->texture_id = -1;
    mesh->bound_radius = 0.8661f;
    mesh->material_flags = DMS_MAT_DOUBLE_SIDED;   /* one winding for all six is not worth getting right */
    mesh->tri_count = 12;
    mesh->vertices = v;
    mesh_bounds(mesh);
    model_bounds(model);
    strcpy(model->material_names[0], "cube");

    model->block_count = 1;
    model->blocks[0] = (DMSBlock){ 0.0f, 0.0f, 0.0f, 0.8661f };
    model->runs[0] = (DMSBlockRun){ 0, 0, 1 };
    model->list_runs[0] = 0; model->list_runs[1] = model->list_runs[2] = model->list_runs[3] = 1;
    model->max_bind_radius = mesh->bound_radius;
    clip_buffer_reserve(24);
    dc_model_compile_header(mesh, &mesh->header, 0, 0, 0, NULL);
    return model;
}

static size_t mf_read(void* dst, size_t sz, size_t n, MemFile* f) {
    size_t want = sz * n, left = f->size - f->pos;
    if (want > left) { n = sz ? left / sz : 0; want = sz * n; }
    memcpy(dst, f->data + f->pos, want);
    f->pos += want;
    return n;
}
static int mf_seek(MemFile* f, long off, int whence) {
    (void)whence;   /* only SEEK_SET is used */
    if (off < 0 || (size_t)off > f->size) return -1;
    f->pos = (size_t)off;
    return 0;
}
static long mf_tell(MemFile* f) { return (long)f->pos; }
static void mf_close(MemFile* f) { free(f->data); f->data = NULL; }

DMSModel* dc_model_load(const char* filename) {
    MemFile mf;
    mf.data = dc_file_read(filename, &mf.size);
    if (!mf.data) return NULL;
    mf.pos = 0;
    MemFile* f = &mf;

    uint32_t magic, mesh_count, bone_count;
    mf_read(&magic, 4, 1, f);
    mf_read(&mesh_count, 4, 1, f);
    mf_read(&bone_count, 4, 1, f);

    if (magic != DMS_MAGIC) { printf("DMS: %s is not a .dms file\n", filename); mf_close(f); return NULL; }

    int is_animated = (bone_count > 0);

    DMSModel* model = malloc(sizeof(DMSModel));
    memset(model, 0, sizeof(DMSModel));
    model->mesh_count = mesh_count;

    mf_read(&model->opaque_count, 4, 1, f);
    mf_read(&model->cutout_count, 4, 1, f);
    mf_read(&model->transparent_count, 4, 1, f);
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
        DMSSkeleton* sk = calloc(1, sizeof(DMSSkeleton));
        sk->boneCount = bone_count;
        sk->bones = memalign(32, bone_count * sizeof(DMSBone));
        memset(sk->bones, 0, bone_count * sizeof(DMSBone));

        for (uint32_t i = 0; i < bone_count; i++) {
            DMSBone* bone = &sk->bones[i];
            mf_read(bone->name, sizeof(char), 64, f);
            mf_read(&bone->parent, sizeof(int), 1, f);
            mf_read(&bone->bindPose, sizeof(DMSTransform), 1, f);
            mf_read(&bone->inverseBindMatrix, sizeof(shz_mat4x4_t), 1, f);
            bone->localPose = bone->bindPose;
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
        mf_read(&anim_count, 4, 1, f);
        printf("DMS: %lu bones, %lu animations\n", (unsigned long)bone_count,
               (unsigned long)anim_count);

        if (anim_count > 0) {
            sk->animCount = anim_count;
            sk->animations = calloc(anim_count, sizeof(DMSAnimation));

            for (uint32_t i = 0; i < anim_count; i++) {
                DMSAnimation* anim = &sk->animations[i];
                mf_read(anim->name, sizeof(char), 32, f);
                mf_read(&anim->boneCount, sizeof(int), 1, f);
                mf_read(&anim->frameCount, sizeof(int), 1, f);
                mf_read(&anim->duration, sizeof(float), 1, f);

                size_t total_poses = anim->frameCount * anim->boneCount;
                anim->framePoses = calloc(total_poses, sizeof(DMSTransform));
                mf_read(anim->framePoses, sizeof(DMSTransform), total_poses, f);
            }
        }

        sk->currentAnim = 0;
        sk->currentTime = 0.0f;
        model->skeleton = sk;

    } else {
        uint32_t anim_count;
        mf_read(&anim_count, 4, 1, f);
    }

    /* ---- Block table (static models only) ---- */
    mf_read(&model->block_count, 4, 1, f);
    model->blocks = malloc(model->block_count * sizeof(DMSBlock));
    mf_read(model->blocks, sizeof(DMSBlock), model->block_count, f);
    if (!is_animated) printf("DMS: %lu blocks\n", (unsigned long)model->block_count);
    if (!is_animated && model->block_count == 0) {
        printf("DMS: %s has no blocks\n", filename);
        free(model->blocks); free(model->meshes); free(model); mf_close(f);
        return NULL;
    }

    /* ---- Load meshes ---- */
    uint32_t max_verts = 0;

    for (uint32_t m = 0; m < mesh_count; m++) {
        DMSMesh* mesh = &model->meshes[m];

        mf_read(&mesh->vertex_count, 4, 1, f);
        mf_read(&mesh->texture_id, 4, 1, f);
        mf_read(&mesh->rim_color, 4, 1, f);
        mf_read(&mesh->bound_cx, 4, 1, f);
        mf_read(&mesh->bound_cy, 4, 1, f);
        mf_read(&mesh->bound_cz, 4, 1, f);
        mf_read(&mesh->bound_radius, 4, 1, f);

        mf_read(&mesh->material_flags, 4, 1, f);
        mf_read(&mesh->alpha_cutoff, sizeof(float), 1, f);
        mf_read(&mesh->block, 4, 1, f);

        mesh->vertices = memalign(32, mesh->vertex_count * sizeof(DMSVertex));
        mf_read(mesh->vertices, sizeof(DMSVertex), mesh->vertex_count, f);
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

        mesh_bounds(mesh);
        one_colour_mark(mesh);
        mesh_pack(mesh, is_animated);
    }
    model_bounds(model);
    {
        uint32_t pm = 0, pv = 0, om = 0, ov = 0;
        for (uint32_t m = 0; m < mesh_count; m++) {
            const DMSMesh* mesh = &model->meshes[m];
            if (!(mesh->material_flags & DMS_MAT_ONE_COLOUR)) continue;
            if (mesh->packed) { pm++; pv += mesh->vertex_count; }
            else              { om++; ov += mesh->vertex_count; }
        }
        printf("DMS: one colour: %lu meshes packed (%lu v), %lu not (%lu v)\n",
               (unsigned long)pm, (unsigned long)pv, (unsigned long)om, (unsigned long)ov);
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

    /* ---- Which bones own vertices (the bound sphere is built on those) ---- */
    if (is_animated) {
        for (uint32_t m = 0; m < mesh_count; m++) {
            const DMSMesh* mesh = &model->meshes[m];
            for (uint32_t v = 0; v < mesh->vertex_count; v++) {
                uint8_t id = mesh->vertices[v].pad;
                if (id < bone_count) model->skeleton->bones[id].skinned = true;
            }
        }
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
    clip_buffer_reserve(max_verts);

    /* ---- Embedded textures & PVR headers ---- */
    uint32_t tex_count;
    mf_read(&tex_count, 4, 1, f);
    printf("DMS: %lu embedded textures\n", (unsigned long)tex_count);

    model->texture_count = tex_count;
    long tex_end = mf_tell(f) + (long)tex_count * 8;   /* end of the texture data */
    if (tex_count > 0) {
        struct { uint32_t offset; uint32_t size; } *tex_table;
        tex_table = malloc(tex_count * 8);
        mf_read(tex_table, 8, tex_count, f);

        model->textures = calloc(tex_count, sizeof(dttex_info_t));

        for (uint32_t i = 0; i < tex_count; i++) {
            if (tex_table[i].size == 0) continue;

            void *buf = malloc(tex_table[i].size);
            mf_seek(f, tex_table[i].offset, SEEK_SET);
            mf_read(buf, 1, tex_table[i].size, f);

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
    if (mf_seek(f, tex_end, SEEK_SET) == 0 &&
        mf_read(tag, 1, 4, f) == 4 && !memcmp(tag, "MATN", 4)) {
        model->material_names = malloc(mesh_count * 32);
        if (model->material_names &&
            mf_read(model->material_names, 32, mesh_count, f) != mesh_count) {
            free(model->material_names);
            model->material_names = NULL;
        }

        /* Then the Empties */
        uint32_t count;
        if (mf_read(tag, 1, 4, f) == 4 && !memcmp(tag, "ENTS", 4) &&
            mf_read(&count, 4, 1, f) == 1 && count) {
            model->entities = malloc(count * sizeof(DMSEntity));
            if (model->entities &&
                mf_read(model->entities, sizeof(DMSEntity), count, f) == count) {
                model->entity_count = count;
            } else {
                free(model->entities);
                model->entities = NULL;
            }
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

        if (dc_model_reflects(mesh) && !model->skeleton &&
            !(mesh->material_flags & DMS_MAT_COLLISION_ONLY)) {
            model->metallic_count++;
            if (mesh->material_flags & DMS_MAT_MIRROR) model->mirror_count++;
        }
        if ((mesh->material_flags & DMS_MAT_GLOW) &&
            !(mesh->material_flags & DMS_MAT_COLLISION_ONLY))
            model->glow_count++;
    }

    mf_close(f);
    /* The built-in reflection goes into video memory now, not in the middle of
     * a frame: loading it then shares the store queues with the vertices on
     * their way to the graphics chip */
    if (model->metallic_count) dc_env_default_prepare();
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

/* The converter writes each Empty as this many bytes (EntityRecord) */
_Static_assert(sizeof(DMSEntity) == 92, "DMSEntity must match the converter");

/* The name, or a copy of it: Blender names copies "name.001" */
static bool entity_is(const DMSEntity* e, const char* name) {
    size_t n = strlen(name);
    if (strncmp(e->name, name, n)) return false;
    const char* rest = e->name + n;
    if (!*rest) return true;
    if (*rest != '.' || !rest[1]) return false;
    for (rest++; *rest; rest++)
        if (*rest < '0' || *rest > '9') return false;
    return true;
}

const DMSEntity* dc_model_entity(const DMSModel* model, const char* name) {
    if (!model || !name) return NULL;
    for (uint32_t i = 0; i < model->entity_count; i++)
        if (entity_is(&model->entities[i], name)) return &model->entities[i];
    return NULL;
}

int dc_model_entities(const DMSModel* model, const char* name,
                      const DMSEntity** out, int max) {
    if (!model || !name) return 0;
    int found = 0;
    for (uint32_t i = 0; i < model->entity_count; i++) {
        if (!entity_is(&model->entities[i], name)) continue;
        if (found < max) out[found] = &model->entities[i];
        found++;
    }
    return found;
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
        dc_model_mesh_unpack(mesh);
        for (uint32_t v = 0; v < mesh->vertex_count; v++)
            mesh->vertices[v].argb = (mesh->vertices[v].argb & 0x00FFFFFFu) |
                                     ((uint32_t)alpha << 24);
        one_colour_mark(mesh);
        mesh_pack(mesh, model->skeleton != NULL);

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

int dc_model_retexture(DMSModel* model, const char* material, const dttex_info_t* tex) {
    if (!model) return 0;
    if (material && !model->material_names) {
        printf("DMS: texture: the model has no material names\n");
        return 0;
    }
    int changed = 0;
    for (uint32_t i = 0; i < model->mesh_count; i++) {
        if (material && strcasecmp(model->material_names[i], material) != 0) continue;
        DMSMesh* mesh = &model->meshes[i];
        const dttex_info_t* t = tex;
        if (!t && mesh->texture_id >= 0 && mesh->texture_id < model->texture_count &&
            model->textures[mesh->texture_id].ptr)
            t = &model->textures[mesh->texture_id];
        if (t) dc_model_compile_header(mesh, &mesh->header, t->pvrformat, t->width, t->height, t->ptr);
        else   dc_model_compile_header(mesh, &mesh->header, 0, 0, 0, NULL);
        changed++;
    }
    if (!changed && material)
        printf("DMS: texture: no mesh wears the material '%s'\n", material);
    return changed;
}

int dc_model_scroll(DMSModel* model, const char* material, float u, float v) {
    if (!model || !material) return 0;
    if (!model->material_names) {
        printf("DMS: scroll: the model has no material names\n");
        return 0;
    }
    int changed = 0;
    for (uint32_t i = 0; i < model->mesh_count; i++) {
        if (strcasecmp(model->material_names[i], material) != 0) continue;
        model->meshes[i].scroll_u = u;
        model->meshes[i].scroll_v = v;
        changed++;
    }
    if (!changed)
        printf("DMS: scroll: no mesh wears the material '%s'\n", material);
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
            pvr_vertex_t* pv = dr_vertex();
            pv->flags = src[i].flags;
            pv->x     = t[i].x * inv_w;
            pv->y     = t[i].y * inv_w;
            pv->z     = inv_w;
            pv->u     = src[i].u;
            pv->v     = src[i].v;
            pv->argb  = src[i].argb;
            dr_send(pv);
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
    dc_send_hdr(dr, &g_shadow_clean_hdr);
    shz_xmtrx_load_4x4(&clean_mvp);
    render_clipped(quad, 4, dr, 0);

    /* 2. the model, squashed, in grey */
    dc_send_hdr(dr, &g_shadow_grey_hdr);
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
            render_clipped(mesh_verts(mesh), mesh->vertex_count, dr, 0);
        }
    }

    /* 3. the buffer onto the screen */
    dc_send_hdr(dr, &g_shadow_flush_hdr);
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

    const DMSVertex* src = mesh_verts(mesh);
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
    (!dms_strip_end(mesh, (i) - 2) &&                                \
     !dms_strip_end(mesh, (i) - 1) &&                                \
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

    /* glTF single-sided: the PVR drops the back faces. Same direction as the
     * "front faces only" reflection headers */
    cxt.gen.culling = (mesh->material_flags & DMS_MAT_DOUBLE_SIDED)
                      ? PVR_CULLING_NONE : PVR_CULLING_CW;

    /* Table fog on every model; dc_set_fog decides whether there is any */
    cxt.gen.fog_type = PVR_FOG_TABLE;

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

    g_view_mesh = NULL;
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
    free(model->entities);

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

int dc_model_flipbook(DMSModel* model, const char* material, int across, int down,
                      int count, float fps) {
    if (!model || !material || across < 1 || down < 1) return 0;
    if (!model->material_names) {
        printf("DMS: flipbook: the model has no material names\n");
        return 0;
    }
    if (count < 1 || count > across * down) count = across * down;
    int changed = 0;
    for (uint32_t i = 0; i < model->mesh_count; i++) {
        if (strcasecmp(model->material_names[i], material) != 0) continue;
        DMSMesh* m = &model->meshes[i];
        m->flip_w = 1.0f / across;
        m->flip_h = 1.0f / down;
        m->flip_across = across;
        m->flip_count = count;
        m->flip_fps = fps;
        changed++;
    }
    if (!changed)
        printf("DMS: flipbook: no mesh wears the material '%s'\n", material);
    return changed;
}
