#include "dc_bolt.h"
#include "dc_draw.h"
#include "dc_engine.h"
#include "dc_camera.h"
#include "main.h"
#include "dc_vram_tag.h"
#include "dc_model.h"
#include <kos.h>
#include <math.h>
#include <stdalign.h>

#define BOLTS   12              /* at once */
#define LEVELS  4               /* halvings: 2^4 = 16 pieces along */
#define POINTS  ((1 << LEVELS) + 1)
#define FORKS   2               /* off each */
#define FORK_POINTS 5
#define LINE    32              /* the picture across: this wide */
#define GLOW_WIDE 5.0f          /* the glow, this many times the core */
#define MOST_QUADS (BOLTS * ((POINTS - 1) + FORKS * (FORK_POINTS - 1)) * 2)

typedef struct {
    shz_vec3_t from, to;
    uint32_t rgb;
    float width, wild;
} Bolt;

static struct {
    Bolt b[BOLTS];
    int n;
    uint32_t frame;             /* the frame these are for */
    DCCamera cam;
    uint32_t seed;
} g;

static pvr_ptr_t g_line;
static alignas(32) pvr_poly_hdr_t g_line_hdr;

static float clampf(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
static shz_vec3_t add(shz_vec3_t a, shz_vec3_t b) { return shz_vec3_init(a.x + b.x, a.y + b.y, a.z + b.z); }
static shz_vec3_t sub(shz_vec3_t a, shz_vec3_t b) { return shz_vec3_init(a.x - b.x, a.y - b.y, a.z - b.z); }
static shz_vec3_t scale(shz_vec3_t a, float s) { return shz_vec3_init(a.x * s, a.y * s, a.z * s); }
static shz_vec3_t cross(shz_vec3_t a, shz_vec3_t b) {
    return shz_vec3_init(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static shz_vec3_t unit(shz_vec3_t a) {
    float l = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
    return l > 1e-6f ? scale(a, 1.0f / l) : shz_vec3_init(0, 1, 0);
}

/* -1..1 */
static float rnd(void) {
    g.seed ^= g.seed << 13; g.seed ^= g.seed >> 17; g.seed ^= g.seed << 5;
    return (float)(g.seed & 0xFFFF) * (2.0f / 65535.0f) - 1.0f;
}

/* Across a line: bright in the middle, soft to nothing at the sides */
static bool line_make(void) {
    if (g_line) return true;
    static bool failed;
    if (failed) return false;
    DC_VRAM_TAG_TEX("bolt", LINE, LINE, "ARGB4444");
    g_line = pvr_mem_malloc(LINE * LINE * 2);
    if (!g_line) { failed = true; return false; }
    alignas(32) static uint16_t px[LINE * LINE];
    for (int y = 0; y < LINE; y++) {
        float f = (y + 0.5f) / (LINE / 2) - 1.0f;
        float a = expf(-f * f * 6.0f) * (1.0f - f * f);
        uint16_t v = (uint16_t)(((int)(clampf(a) * 15.0f + 0.5f) << 12) | 0x0FFF);
        for (int x = 0; x < LINE; x++) px[y * LINE + x] = v;
    }
    pvr_txr_load(px, g_line, sizeof px);
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED,
                     LINE, LINE, g_line, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
    cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
    cxt.blend.src = PVR_BLEND_SRCALPHA;
    cxt.blend.dst = PVR_BLEND_ONE;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(&g_line_hdr, &cxt);
    return true;
}

/* A strip through n points, half wide across, facing the camera: a quad a
 * piece, each end turned to the camera at its own point so they meet */
static int strip(DMSVertex* q, const shz_vec3_t* p, int n, float half, uint32_t argb) {
    shz_vec3_t side[POINTS];
    for (int i = 0; i < n; i++) {
        shz_vec3_t along = sub(p[i + 1 < n ? i + 1 : i], p[i > 0 ? i - 1 : 0]);
        side[i] = scale(unit(cross(along, sub(g.cam.pos, p[i]))), half);
    }
    for (int i = 0; i + 1 < n; i++) {
        DMSVertex* v = &q[i * 4];
        shz_vec3_t c[4] = { sub(p[i], side[i]), add(p[i], side[i]), sub(p[i + 1], side[i + 1]), add(p[i + 1], side[i + 1]) };
        for (int k = 0; k < 4; k++) {
            v[k].x = c[k].x; v[k].y = c[k].y; v[k].z = c[k].z;
            v[k].u = (k & 2) ? 1.0f : 0.0f;
            v[k].v = (k & 1) ? 1.0f : 0.0f;
            v[k].argb = argb;
            v[k].flags = k == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        }
    }
    return n - 1;
}

/* Glow, then core, through the same points */
static int both(DMSVertex* q, const shz_vec3_t* p, int n, float width, uint32_t rgb, float bright) {
    uint32_t r = (rgb >> 16) & 0xFF, gg = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    /* The core nearly white, a little of its colour */
    uint32_t core = ((r + 3 * 255) / 4) << 16 | ((gg + 3 * 255) / 4) << 8 | ((b + 3 * 255) / 4);
    int k = strip(q, p, n, width * GLOW_WIDE * 0.5f, (uint32_t)(clampf(bright * 0.45f) * 255.0f) << 24 | rgb);
    k += strip(q + k * 4, p, n, width * 0.5f, (uint32_t)(clampf(bright) * 255.0f) << 24 | core);
    return k;
}

static void bolt_draw(void* user) {
    (void)user;
    pvr_dr_state_t* dr = dc_dr_state();
    if (!g.n || !line_make()) { g.n = 0; return; }
    static alignas(32) DMSVertex q[MOST_QUADS * 4];
    int nq = 0;
    for (int i = 0; i < g.n; i++) {
        const Bolt* b = &g.b[i];
        shz_vec3_t d = sub(b->to, b->from);
        float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
        if (len < 1e-4f) continue;
        shz_vec3_t dir = scale(d, 1.0f / len);
        shz_vec3_t ax = unit(cross(dir, fabsf(dir.y) < 0.9f ? shz_vec3_init(0, 1, 0) : shz_vec3_init(1, 0, 0)));
        shz_vec3_t ay = cross(dir, ax);

        /* Halved again and again, each middle pushed off sideways, less each time */
        shz_vec3_t p[POINTS];
        p[0] = b->from;
        p[POINTS - 1] = b->to;
        float amp = len * b->wild;
        for (int step = POINTS - 1; step > 1; step /= 2, amp *= 0.5f)
            for (int s = 0; s + step < POINTS; s += step) {
                shz_vec3_t m = scale(add(p[s], p[s + step]), 0.5f);
                p[s + step / 2] = add(m, add(scale(ax, rnd() * amp), scale(ay, rnd() * amp)));
            }
        nq += both(&q[nq * 4], p, POINTS, b->width, b->rgb, 1.0f);

        /* Forks: off a point along it, out at a slant, short, thinner */
        for (int f = 0; f < FORKS; f++) {
            int at = 3 + (int)((rnd() * 0.5f + 0.5f) * (POINTS - 7));
            float flen = len * (0.12f + 0.12f * (rnd() * 0.5f + 0.5f));
            shz_vec3_t fd = unit(add(dir, add(scale(ax, rnd() * 1.2f), scale(ay, rnd() * 1.2f))));
            shz_vec3_t fp[FORK_POINTS];
            fp[0] = p[at];
            for (int k = 1; k < FORK_POINTS; k++) {
                shz_vec3_t step = scale(fd, flen / (FORK_POINTS - 1));
                fp[k] = add(add(fp[k - 1], step), add(scale(ax, rnd() * flen * 0.15f), scale(ay, rnd() * flen * 0.15f)));
            }
            nq += both(&q[nq * 4], fp, FORK_POINTS, b->width * 0.6f, b->rgb, 0.6f);
        }
    }
    g.n = 0;
    if (!nq) return;
    dc_send_hdr(dr, &g_line_hdr);
    shz_xmtrx_load_4x4((shz_mat4x4_t*)dc_camera_get_pv(&g.cam));
    shz_xmtrx_translate(-g.cam.pos.x, -g.cam.pos.y, g.cam.pos.z);
    dc_model_submit_quads(q, nq, dr);
}

void dc_bolt(shz_vec3_t from, shz_vec3_t to, uint32_t rgb, float width, float wild) {
    /* What was said in a frame that was never drawn is gone */
    if (g.frame != dc_frame_count()) { g.frame = dc_frame_count(); g.n = 0; }
    if (g.n >= BOLTS) return;
    if (!g.n) {
        const DCCamera* cam = dc_get_camera();
        if (!cam) return;
        g.cam = *cam;
        g.seed = g.seed * 1664525u + 1013904223u + g.frame;
        if (!g.seed) g.seed = 1;
        dc_draw_call(PVR_LIST_TR_POLY, bolt_draw, NULL);
    }
    g.b[g.n++] = (Bolt){ from, to, rgb & 0xFFFFFF, width, wild };
}
