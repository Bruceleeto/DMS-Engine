#include "dc_beam.h"
#include "dc_draw.h"
#include "dc_engine.h"
#include "dc_camera.h"
#include "main.h"
#include "dc_vram_tag.h"
#include "dc_model.h"
#include <kos.h>
#include <math.h>
#include <stdio.h>
#include <stdalign.h>
#include <string.h>

#define BEAMS    4
#define SIDES    32             /* round a cone */
#define SHAFT_SIDES 24          /* round a shaft */
#define PAST     0.15f          /* a cone starts this far past the sheet */
#define GLOW     64             /* the light across a beam: a picture this wide */
#define GLOW_AT  0.1f           /* drawn this far past the sheet */
#define GLOW_OVER 5.0f          /* its square this many times the light's: the whole view */
#define RINGS    7              /* along a shaft */

typedef struct {
    shz_vec3_t from, dir;
    float half, length;
} Beam;

typedef struct {
    shz_vec3_t from, dir;
    float r0, half, start, length, strength;
    uint32_t rgb;
} Shaft;

/* This frame's: what dc_beam_dark() and dc_beam() said, drawn at the flush */
static struct {
    DCCamera cam;
    float dark, near;
    int n;
    Beam b[BEAMS];
    DCCamera shaft_cam;
    int shafts;
    Shaft s[BEAMS];
} g;

static float clampf(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
static shz_vec3_t add(shz_vec3_t a, shz_vec3_t b) { return shz_vec3_init(a.x + b.x, a.y + b.y, a.z + b.z); }
static shz_vec3_t scale(shz_vec3_t a, float k) { return shz_vec3_init(a.x * k, a.y * k, a.z * k); }
static shz_vec3_t cross(shz_vec3_t a, shz_vec3_t b) {
    return shz_vec3_init(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static shz_vec3_t unit(shz_vec3_t a) {
    float l = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
    return l > 1e-6f ? scale(a, 1.0f / l) : a;
}

/* A world point on the screen (x, y, 1/w), with the camera's matrix loaded
 * as dc_model_draw_volume has it. False behind the near plane. */
static bool project(const DCCamera* cam, shz_vec3_t p, float* out) {
    shz_vec4_t t = shz_vec4_swizzle(shz_xmtrx_transform_vec4(
        shz_vec4_init(p.x - cam->pos.x, p.y - cam->pos.y, -(p.z - cam->pos.z), 1.0f)), 1, 2, 3, 0);
    if (t.w < NEAR_Z) return false;
    float inv_w = shz_invf_fsrra(t.w);
    out[0] = t.x * inv_w;
    out[1] = t.y * inv_w;
    out[2] = inv_w;
    return true;
}

/* One modifier triangle: 64 bytes, two goes at the store queue */
static void volume_tri(pvr_dr_state_t* dr, const float* p) {
    uint32_t* w = (uint32_t*)pvr_dr_target(*dr);
    w[0] = PVR_CMD_VERTEX_EOL;
    for (int i = 0; i < 7; i++) ((float*)w)[1 + i] = p[i];
    pvr_dr_commit(w);
    w = (uint32_t*)pvr_dr_target(*dr);
    ((float*)w)[0] = p[7]; ((float*)w)[1] = p[8];
    for (int i = 2; i < 8; i++) w[i] = 0;
    pvr_dr_commit(w);
}

#define MOST_TRIS (1 + BEAMS * (2 * SIDES + 2 * (SIDES - 2)))

static float g_tris[MOST_TRIS][9];
static int g_ntris;

static void tri(const DCCamera* cam, shz_vec3_t a, shz_vec3_t b, shz_vec3_t c) {
    float* t = g_tris[g_ntris];
    /* One across the near plane is left out: rare, a beam goes the way the
     * camera looks */
    if (!project(cam, a, t) || !project(cam, b, t + 3) || !project(cam, c, t + 6)) return;
    g_ntris++;
}

static pvr_ptr_t g_glow;      /* the light's picture (glow_make) */

static void beam_draw(void* user) {
    (void)user;
    /* With the light's picture over the whole view, that is the dark too:
     * the volume's hard edge would only show against its soft one */
    if (g.n && g_glow) return;
    const DCCamera* cam = &g.cam;
    shz_xmtrx_load_4x4((shz_mat4x4_t*)dc_camera_get_pv(cam));
    g_ntris = 0;

    /* The sheet: wider than the view, just in front of the camera */
    shz_sincos_t y = shz_sincosf(cam->yaw), p = shz_sincosf(cam->pitch);
    shz_vec3_t f = shz_vec3_init(y.sin * p.cos, -p.sin, y.cos * p.cos);
    shz_vec3_t r = shz_vec3_init(-y.cos, 0.0f, y.sin);
    shz_vec3_t u = unit(cross(f, r));
    shz_vec3_t c = add(cam->pos, scale(f, g.near));
    /* One triangle, not two: where two meet, a line of pixels can be
     * counted twice and come out the wrong side */
    float k = g.near * 4.0f;
    tri(cam, add(c, add(scale(r, -2.0f * k), scale(u, -k))), add(c, add(scale(r, 2.0f * k), scale(u, -k))),
        add(c, scale(u, 2.0f * k)));

    /* Each cone, closed at both ends */
    for (int i = 0; i < g.n; i++) {
        const Beam* b = &g.b[i];
        shz_vec3_t d = b->dir;
        shz_vec3_t bu = unit(cross(d, fabsf(d.y) < 0.9f ? shz_vec3_init(0, 1, 0) : shz_vec3_init(1, 0, 0)));
        shz_vec3_t bv = cross(d, bu);
        /* Its flat sides outside the light's round rim, not across it */
        float t = tanf(b->half) / cosf(3.14159265f / SIDES);
        float start = g.near + PAST;
        shz_vec3_t near = add(b->from, scale(d, start)), far = add(b->from, scale(d, b->length));
        float r0 = start * t, r1 = b->length * t;
        shz_vec3_t ring0[SIDES], ring1[SIDES];
        for (int s = 0; s < SIDES; s++) {
            shz_sincos_t a = shz_sincosf(6.2831853f * s / SIDES);
            shz_vec3_t o = add(scale(bu, a.cos), scale(bv, a.sin));
            ring0[s] = add(near, scale(o, r0));
            ring1[s] = add(far, scale(o, r1));
        }
        for (int s = 0; s < SIDES; s++) {
            int s1 = (s + 1) % SIDES;
            tri(cam, ring0[s], ring1[s], ring1[s1]);
            tri(cam, ring0[s], ring1[s1], ring0[s1]);
        }
        for (int s = 1; s + 1 < SIDES; s++) {
            tri(cam, ring0[0], ring0[s], ring0[s + 1]);
            tri(cam, ring1[0], ring1[s + 1], ring1[s]);
        }
    }
    if (!g_ntris) return;

    /* Not while the chip draws the frame before (the manual: FPU_SHAD_SCALE
     * "must not be rewritten while drawing"), so only on a change, after it */
    static float scale_set = -1.0f;
    if (g.dark != scale_set) {
        pvr_wait_render_done();
        pvr_set_shadow_scale(true, g.dark);
        scale_set = g.dark;
    }
    pvr_dr_state_t* dr = dc_dr_state();
    alignas(32) pvr_poly_hdr_t hdr;
    /* All one volume: the last triangle closes it, under its own header */
    if (g_ntris > 1) {
        pvr_mod_compile((pvr_mod_hdr_t*)&hdr, PVR_LIST_OP_MOD, PVR_MODIFIER_OTHER_POLY, PVR_CULLING_NONE);
        dc_sq_copy32(pvr_dr_target(*dr), &hdr);
    }
    for (int i = 0; i < g_ntris; i++) {
        if (i == g_ntris - 1) {
            pvr_mod_compile((pvr_mod_hdr_t*)&hdr, PVR_LIST_OP_MOD, PVR_MODIFIER_INCLUDE_LAST_POLY,
                            PVR_CULLING_NONE);
            dc_sq_copy32(pvr_dr_target(*dr), &hdr);
        }
        volume_tri(dr, g_tris[i]);
    }
}

/* ================================================================
 * The light across a beam: a picture multiplied over what the beam falls on
 * (twice it, so it can brighten: 0.5 leaves it as it is), on a square
 * across the beam just past the sheet. Its middle is a hot spot up to twice
 * as bright, falling off softly to the darkness outside at its rim, with
 * the faint bright ring a torch's reflector throws. Seen from the beam's
 * start, a cone's edge lands on the same circle at every distance, so from
 * the camera the picture's rim is where the volume's edge is: no seam.
 * ================================================================ */

static alignas(32) pvr_poly_hdr_t g_glow_hdr;
static float g_glow_dark = -1.0f;   /* the darkness it was made for */

static uint16_t rgb565(float r, float g, float b) {
    int ir = (int)(clampf(r) * 31.0f + 0.5f), ig = (int)(clampf(g) * 63.0f + 0.5f),
        ib = (int)(clampf(b) * 31.0f + 0.5f);
    return (uint16_t)((ir << 11) | (ig << 5) | ib);
}

static bool glow_make(float dark) {
    if (g_glow && dark == g_glow_dark) return true;
    if (!g_glow) {
        DC_VRAM_TAG_TEX("beam light", GLOW, GLOW, "RGB565");
        g_glow = pvr_mem_malloc(GLOW * GLOW * 2);
        if (!g_glow) {
            printf("dc_beam: no VRAM for the light, a flat beam\n");
            g_glow_dark = dark;
            return false;
        }
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                         GLOW, GLOW, g_glow, PVR_FILTER_BILINEAR);
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.txr.env = PVR_TXRENV_MODULATE;
        cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
        /* src * dst + dst * src: the frame times twice the picture */
        cxt.blend.src = PVR_BLEND_DESTCOLOR;
        cxt.blend.dst = PVR_BLEND_DESTCOLOR;
        /* Only on what is behind it: not the gun, nearer than the sheet */
        cxt.depth.comparison = PVR_DEPTHCMP_GREATER;
        cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
        pvr_poly_compile(&g_glow_hdr, &cxt);
    }
    g_glow_dark = dark;

    static uint16_t px[GLOW * GLOW] __attribute__((aligned(32)));
    for (int y = 0; y < GLOW; y++)
        for (int x = 0; x < GLOW; x++) {
            float fx = (x + 0.5f) / (GLOW / 2) - 1.0f, fy = (y + 0.5f) / (GLOW / 2) - 1.0f;
            float d = sqrtf(fx * fx + fy * fy);
            /* Outside the rim the volume darkens: left as it is */
            /* Outside: the dark, as far as the picture is stretched */
            float b = dark;
            if (d < 1.0f) {
                /* One smooth fall from the middle to the dark by the rim,
                 * flat at both ends: no knee in it, which the eye takes for
                 * a line round the light */
                /* Bright across most of it, easing out by the rim */
                float s = clampf(d / 0.97f), u = s * s;
                float f = 1.0f - u * u * (3.0f - 2.0f * u);
                /* Streaks out from the middle, as a reflector throws */
                float a = atan2f(fy, fx);
                f *= 1.0f + 0.12f * d * sinf(a * 7.0f) * sinf(a * 11.0f + 1.3f);
                b = dark + (2.0f - dark) * f;
            }
            /* A warm light: a little less blue, in what it adds over the
             * dark only, so the rim is as dark as outside in each colour */
            float ex = b - dark;
            float base = dark;
            px[y * GLOW + x] = rgb565((base + ex) * 0.5f, (base + ex * 0.96f) * 0.5f,
                                      (base + ex * 0.84f) * 0.5f);
        }
    pvr_txr_load(px, g_glow, sizeof px);
    return true;
}

/* ================================================================
 * The light laid on the world (dc_beam_onto): the beam's triangles, cut
 * small enough that the picture, worked out at the corners, is near right
 * across them (the PVR's mapping is the camera's, not the lamp's). Drawn
 * twice the picture over the frame as above; each corner's colour is how
 * lit it is (facing the lamp, near it) and its offset colour the darkness
 * for the rest, so an unlit corner comes out as dark as outside the beam:
 * picture * lit + dark * (1 - lit).
 * ================================================================ */

#define SOURCE_MAX 1024         /* the world's triangles in a beam */
#define CELL_MAX   256          /* triangles in one grid cell */
#define SEEN_SIZE  4096         /* cells and triangles looked at, a beam */
#define LIGHT_MAX  1400         /* what they are cut into, drawn */

static ColWorld* g_col;
static alignas(32) pvr_poly_hdr_t g_light_hdr;
static bool g_light_hdr_made;

void dc_beam_onto(ColWorld* col) { g_col = col; }

typedef struct {
    const Beam* b;
    const DCCamera* cam;
    shz_vec3_t bu, bv, n;
    float t, dark;
    pvr_dr_state_t* dr;
    int left;
} Lay;

static float dot3(shz_vec3_t a, shz_vec3_t b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static shz_vec3_t sub(shz_vec3_t a, shz_vec3_t b) { return shz_vec3_init(a.x - b.x, a.y - b.y, a.z - b.z); }
static shz_vec3_t mid(shz_vec3_t a, shz_vec3_t b) {
    return shz_vec3_init((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f);
}
static uint32_t grey(float v) {
    uint32_t c = (uint32_t)(clampf(v) * 255.0f);
    return 0xFF000000u | (c << 16) | (c << 8) | c;
}

/* One corner: on the screen, where in the picture, how lit. False behind
 * the near plane. */
static bool lay_corner(const Lay* L, shz_vec3_t p, pvr_vertex_t* v) {
    float s[3];
    if (!project(L->cam, p, s)) return false;
    shz_vec3_t o = sub(p, L->b->from);
    float lz = dot3(o, L->b->dir);
    float z = lz > 0.05f ? lz : 0.05f;
    float k = 0.5f / (z * L->t);
    v->x = s[0]; v->y = s[1];
    v->z = s[2] * 1.0005f;                  /* on what it lights, just in front */
    v->u = 0.5f + dot3(o, L->bu) * k;
    v->v = 0.5f + dot3(o, L->bv) * k;
    float d = sqrtf(dot3(o, o));
    float face = d > 1e-4f ? -dot3(L->n, o) / d : 0.0f;
    /* A floor the light skims still takes it, as a road a headlight: the
     * pool reaches far along it */
    float lit = (face > 0.0f ? 0.5f + 0.5f * face : 0.0f) * clampf(1.4f - d / L->b->length);
    if (lz <= 0.0f) lit = 0.0f;
    v->argb = grey(lit);
    v->oargb = grey(L->dark * 0.5f * (1.0f - lit));
    return true;
}

static void lay_tri(Lay* L, shz_vec3_t a, shz_vec3_t b, shz_vec3_t c, int depth) {
    if (L->left <= 0) return;
    /* Not in the beam at all: every corner past the same side. Before any
     * cutting, or the floor round your feet uses up the lot. */
    shz_vec3_t p[3] = { a, b, c };
    int out[5] = { 0 };
    for (int i = 0; i < 3; i++) {
        shz_vec3_t o = sub(p[i], L->b->from);
        float lz = dot3(o, L->b->dir), r = lz * L->t * 1.05f;
        float lx = dot3(o, L->bu), ly = dot3(o, L->bv);
        out[0] += lx > r; out[1] += -lx > r; out[2] += ly > r; out[3] += -ly > r;
        out[4] += lz < 0.0f || lz > L->b->length;
    }
    for (int i = 0; i < 5; i++) if (out[i] == 3) return;

    /* Cut in two across its longest side until small for how near it is:
     * the picture changes fastest close to the lamp */
    float ab = dot3(sub(b, a), sub(b, a)), bc = dot3(sub(c, b), sub(c, b)), ca = dot3(sub(a, c), sub(a, c));
    float longest = ab > bc ? (ab > ca ? ab : ca) : (bc > ca ? bc : ca);
    float near = dot3(sub(a, L->b->from), L->b->dir);
    float nb = dot3(sub(b, L->b->from), L->b->dir), nc = dot3(sub(c, L->b->from), L->b->dir);
    if (nb < near) near = nb;
    if (nc < near) near = nc;
    float most = near < 3.0f ? 0.5f : near < 8.0f ? 1.0f : 2.0f;
    if (longest > most * most && depth < 7) {
        if (longest == ab) { shz_vec3_t m = mid(a, b); lay_tri(L, a, m, c, depth + 1); lay_tri(L, m, b, c, depth + 1); }
        else if (longest == bc) { shz_vec3_t m = mid(b, c); lay_tri(L, a, b, m, depth + 1); lay_tri(L, a, m, c, depth + 1); }
        else { shz_vec3_t m = mid(c, a); lay_tri(L, a, b, m, depth + 1); lay_tri(L, m, b, c, depth + 1); }
        return;
    }
    alignas(32) pvr_vertex_t v[3];
    for (int i = 0; i < 3; i++) if (!lay_corner(L, p[i], &v[i])) return;
    for (int i = 0; i < 3; i++) {
        pvr_vertex_t* w = (pvr_vertex_t*)pvr_dr_target(*L->dr);
        *w = v[i];
        w->flags = i == 2 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        pvr_dr_commit(w);
    }
    L->left--;
}

/* The same triangle found twice (it is in every cell it touches) would be
 * lit twice: a small table of the ones seen. Also of the cells. */
static bool seen_before(uint32_t* table, int size, const ColTri* t) {
    uint32_t h = 2166136261u, w[9];
    memcpy(w, t, sizeof w);         /* not through a pointer: the floats are floats */
    for (int i = 0; i < 9; i++) h = (h ^ w[i]) * 16777619u;
    if (!h) h = 1;
    for (int i = 0; i < size; i++) {
        uint32_t* slot = &table[(h + i) & (size - 1)];
        if (*slot == h) return true;
        if (!*slot) { *slot = h; return false; }
    }
    return true;
}

static void light_draw(void* user) {
    (void)user;
    const DCCamera* cam = &g.cam;
    if (!g.n || !g_glow || !g_col) return;
    if (!g_light_hdr_made) {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                         GLOW, GLOW, g_glow, PVR_FILTER_BILINEAR);
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.gen.shading = PVR_SHADE_GOURAUD;
        cxt.gen.specular = PVR_SPECULAR_ENABLE;     /* the offset colour: the dark */
        cxt.txr.env = PVR_TXRENV_MODULATE;
        cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
        cxt.blend.src = PVR_BLEND_DESTCOLOR;
        cxt.blend.dst = PVR_BLEND_DESTCOLOR;
        cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
        cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
        pvr_poly_compile(&g_light_hdr, &cxt);
        g_light_hdr_made = true;
    }
    shz_xmtrx_load_4x4((shz_mat4x4_t*)dc_camera_get_pv(cam));
    pvr_dr_state_t* dr = dc_dr_state();
    dc_send_hdr(dr, &g_light_hdr);

    static ColTri tris[SOURCE_MAX], got[CELL_MAX];
    static uint32_t table[SEEN_SIZE], cells[SEEN_SIZE];
    Lay L = { .cam = cam, .dark = g.dark, .dr = dr, .left = LIGHT_MAX };
    for (int i = 0; i < g.n; i++) {
        const Beam* b = &g.b[i];
        L.b = b;
        L.t = tanf(b->half);
        L.bu = unit(cross(b->dir, fabsf(b->dir.y) < 0.9f ? shz_vec3_init(0, 1, 0) : shz_vec3_init(1, 0, 0)));
        L.bv = cross(b->dir, L.bu);

        /* The world's triangles in it: along it a cell at a time, the
         * grid's cells across it there, nearest first so that if there are
         * too many it is the far ones left out */
        memset(table, 0, sizeof table);
        memset(cells, 0, sizeof cells);
        int n = 0;
        ColWorld* w = g_col;
        for (float s = 0.0f; s < b->length && n < SOURCE_MAX; s += w->cell_size) {
            shz_vec3_t at = add(b->from, scale(b->dir, s));
            float r = s * L.t + w->cell_size;
            int x0, x1, z0, z1;
            col_cell_at(w, at.x - r, at.z - r, &x0, &z0);
            col_cell_at(w, at.x + r, at.z + r, &x1, &z1);
            for (int gz = z0; gz <= z1 && n < SOURCE_MAX; gz++)
                for (int gx = x0; gx <= x1 && n < SOURCE_MAX; gx++) {
                    ColTri key = { .v0 = shz_vec3_init((float)gx, (float)gz, 0.0f) };
                    if (seen_before(cells, SEEN_SIZE, &key)) continue;
                    int k = col_cell_tris(w, gx, gz, got, CELL_MAX);
                    for (int i = 0; i < k && n < SOURCE_MAX; i++) {
                        if (!seen_before(table, SEEN_SIZE, &got[i])) tris[n++] = got[i];
                    }
                }
        }

        for (int j = 0; j < n; j++) {
            const ColTri* t = &tris[j];
            shz_vec3_t a = t->v0, bb = add(t->v0, t->edge1), c = add(t->v0, t->edge2);
            /* Its face towards the camera (the lamp is with it): a wall's
             * back, seen through nothing, is not lit from in front */
            L.n = unit(cross(t->edge1, t->edge2));
            if (dot3(L.n, sub(cam->pos, a)) < 0.0f) L.n = scale(L.n, -1.0f);
            lay_tri(&L, a, bb, c, 0);
        }
    }
}

static void glow_draw(void* user) {
    (void)user;
    const DCCamera* cam = &g.cam;
    if (!g.n || !g_glow) return;
    pvr_dr_state_t* dr = dc_dr_state();
    dc_send_hdr(dr, &g_glow_hdr);
    shz_xmtrx_load_4x4((shz_mat4x4_t*)dc_camera_get_pv(cam));
    shz_xmtrx_translate(-cam->pos.x, -cam->pos.y, cam->pos.z);
    /* One triangle round the square, the last corner twice: a square's
     * two would meet along a seam across the view */
    static const float corner[4][2] = { { -1, -1 }, { 3, -1 }, { -1, 3 }, { -1, 3 } };
    alignas(32) DMSVertex q[BEAMS * 4];
    for (int i = 0; i < g.n; i++) {
        const Beam* b = &g.b[i];
        shz_vec3_t d = b->dir;
        shz_vec3_t bu = unit(cross(d, fabsf(d.y) < 0.9f ? shz_vec3_init(0, 1, 0) : shz_vec3_init(1, 0, 0)));
        shz_vec3_t bv = cross(d, bu);
        /* Its rim on the volume's edge: the light fades to the dark outside
         * just as the edge is reached, so the edge is not seen */
        float at = g.near + GLOW_AT, half = at * tanf(b->half) * GLOW_OVER;
        shz_vec3_t c = add(b->from, scale(d, at));
        DMSVertex* v = &q[i * 4];
        for (int k = 0; k < 4; k++) {
            shz_vec3_t p = add(c, add(scale(bu, corner[k][0] * half), scale(bv, corner[k][1] * half)));
            v[k].x = p.x; v[k].y = p.y; v[k].z = p.z;
            v[k].u = (corner[k][0] * GLOW_OVER + 1.0f) * 0.5f;
            v[k].v = (corner[k][1] * GLOW_OVER + 1.0f) * 0.5f;
            v[k].argb = 0xFFFFFFFF;
            v[k].flags = k == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        }
    }
    dc_model_submit_quads(q, g.n, dr);
}

/* ================================================================
 * Shafts: rings along it, each side's corners as bright as that side faces
 * the camera and as near the lamp as the ring is. Sent as squares the way
 * particles are.
 * ================================================================ */

static alignas(32) pvr_poly_hdr_t g_shaft_hdr;

static void shaft_draw(void* user) {
    (void)user;
    const DCCamera* cam = &g.shaft_cam;
    static bool made;
    if (!made) {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.gen.shading = PVR_SHADE_GOURAUD;
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_ONE;
        cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
        pvr_poly_compile(&g_shaft_hdr, &cxt);
        made = true;
    }
    pvr_dr_state_t* dr = dc_dr_state();
    dc_send_hdr(dr, &g_shaft_hdr);
    shz_xmtrx_load_4x4((shz_mat4x4_t*)dc_camera_get_pv(cam));
    shz_xmtrx_translate(-cam->pos.x, -cam->pos.y, cam->pos.z);

    alignas(32) DMSVertex q[SHAFT_SIDES * 4];
    for (int i = 0; i < g.shafts; i++) {
        const Shaft* s = &g.s[i];
        shz_vec3_t d = s->dir;
        shz_vec3_t bu = unit(cross(d, fabsf(d.y) < 0.9f ? shz_vec3_init(0, 1, 0) : shz_vec3_init(1, 0, 0)));
        shz_vec3_t bv = cross(d, bu);
        float t = tanf(s->half);
        float ch = cosf(s->half), sh = sinf(s->half);

        /* Each ring: its corners and how bright */
        shz_vec3_t at[RINGS][SHAFT_SIDES];
        uint32_t argb[RINGS][SHAFT_SIDES];
        for (int r = 0; r < RINGS; r++) {
            /* Closer together near its start, where it changes most */
            float f = (float)r / (RINGS - 1);
            float along = s->start + (s->length - s->start) * f * f;
            float rad = s->r0 + along * t;
            shz_vec3_t c = add(s->from, scale(d, along));
            /* In from nothing at its start, gone by the end */
            float in = clampf(f * 3.0f);
            in = in * in * (3.0f - 2.0f * in);
            float fade = s->strength * in * (1.0f - f) * sqrtf(1.0f - f);
            for (int k = 0; k < SHAFT_SIDES; k++) {
                shz_sincos_t a = shz_sincosf(6.2831853f * k / SHAFT_SIDES);
                shz_vec3_t o = add(scale(bu, a.cos), scale(bv, a.sin));
                shz_vec3_t p = add(c, scale(o, rad));
                at[r][k] = p;
                /* Edge on, a side shows only its thin rim of haze */
                shz_vec3_t to = unit(shz_vec3_init(cam->pos.x - p.x, cam->pos.y - p.y, cam->pos.z - p.z));
                float face = fabsf(o.x * to.x + o.y * to.y + o.z * to.z);
                /* Gone at its true outline (the side leans out by the
                 * cone's angle), so its edge is soft, not a line */
                shz_vec3_t n = add(scale(o, ch), scale(d, -sh));
                face *= clampf(fabsf(n.x * to.x + n.y * to.y + n.z * to.z) * 4.0f);
                uint32_t al = (uint32_t)(clampf(fade * face) * 255.0f);
                argb[r][k] = (al << 24) | (s->rgb & 0xFFFFFF);
            }
        }

        /* A square for each side between each two rings */
        for (int r = 0; r + 1 < RINGS; r++) {
            for (int k = 0; k < SHAFT_SIDES; k++) {
                int ks[2] = { k, (k + 1) % SHAFT_SIDES };
                DMSVertex* v = &q[k * 4];
                for (int m = 0; m < 4; m++) {
                    int rr = r + (m >> 1), kk = ks[m & 1];
                    v[m].x = at[rr][kk].x; v[m].y = at[rr][kk].y; v[m].z = at[rr][kk].z;
                    v[m].u = v[m].v = 0.0f;
                    v[m].argb = argb[rr][kk];
                    v[m].flags = m == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                }
            }
            dc_model_submit_quads(q, SHAFT_SIDES, dr);
        }
    }
}

void dc_beam_shaft(shz_vec3_t from, shz_vec3_t dir, float r0, float half_angle, float start,
                   float length, uint32_t rgb, float strength) {
    if (g.shafts >= BEAMS || strength <= 0.0f) return;
    if (!g.shafts) {
        const DCCamera* cam = dc_get_camera();
        if (!cam) return;
        g.shaft_cam = *cam;
        dc_draw_call(PVR_LIST_TR_POLY, shaft_draw, NULL);
    }
    if (start < 0.0f) start = 0.0f;
    if (start >= length) return;
    g.s[g.shafts++] = (Shaft){ from, unit(dir), r0, half_angle, start, length, strength, rgb };
}

void dc_beam_dark(float dark, float near) {
    g.n = 0;
    g.shafts = 0;
    if (!dc_beams_enabled()) {
        static bool said;
        if (!said) printf("dc_beam: needs dc_init(.beams = true)\n");
        said = true;
        return;
    }
    const DCCamera* cam = dc_get_camera();
    g.dark = 1.0f;
    if (!cam || dark >= 1.0f) return;
    g.cam = *cam;
    /* Snapped to what the picture's rim can hold exactly (5 bits, at half,
     * as the PVR widens it to 8) and the shadow scale too (8 bits): else the
     * rim comes out darker or lighter than outside, a ring round the light */
    int q = (int)((dark < 0.0f ? 0.0f : dark) * 0.5f * 31.0f + 0.5f);
    int e = (q << 3) | (q >> 2);
    g.dark = (2.0f * e + 0.5f) / 255.0f;
    g.near = near > NEAR_Z * 1.5f ? near : NEAR_Z * 1.5f;
    dc_draw_call(PVR_LIST_OP_MOD, beam_draw, NULL);
}

void dc_beam(shz_vec3_t from, shz_vec3_t dir, float half_angle, float length) {
    if (g.n >= BEAMS || !dc_beams_enabled() || g.dark >= 1.0f) return;
    /* The light across it, once a frame with the first */
    if (!g.n && glow_make(g.dark))
        dc_draw_call(PVR_LIST_TR_POLY, g_col ? light_draw : glow_draw, NULL);
    g.b[g.n++] = (Beam){ from, unit(dir), half_angle, length };
}
