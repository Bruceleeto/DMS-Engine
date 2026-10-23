#include "dc_corona.h"
#include "dc_draw.h"
#include "dc_engine.h"
#include "dc_model.h"
#include "main.h"
#include <kos.h>
#include <stdio.h>
#include <string.h>

#include "corona_default.h"

/* ================================================================
 * Coronas
 *
 * As GTA 3 (renderer/Coronas.cpp): a table of lights asked for this frame,
 * each fading towards lit or not, drawn at frame end as squares facing the
 * camera, added over the frame with no depth write. GTA pulled each one 1.5m
 * towards the camera so the lamp it sits in doesn't hide it; here that is
 * its own size, at most half the way. Drawn once for each view of the
 * screen. A light can also have GTA's lens flare (DC_CORONA_FLARE).
 * ================================================================ */

#define CORONAS     64
#define FADE_SPEED  3.0f    /* of full a second: GTA's 15 a tick at 50Hz, out of 255 */
#define BATCH_QUADS 16
#define VIEWS       4       /* split screen views drawn with coronas */

typedef struct {
    uint32_t   id;
    shz_vec3_t pos;
    uint32_t   rgb;
    float      size;
    float      fade;        /* 0 to 1, how lit it is shown */
    uint32_t   asked;       /* the frame it was last asked for */
    uint32_t   flags;
    bool       used;
} Corona;

/* GTA 3's headlight flare (HeadLightsFlareDef): discs on the line from the
 * light through the middle of the screen. where: along that line, 1 the
 * light, 0 the middle, below 0 past it. half: half its width in pixels at
 * 480 high. rgb: of the light's colour, out of 256. tex: hex, circle, ring. */
typedef struct {
    float   where, half;
    uint8_t rgb, alpha, tex;
} FlareDisc;

static const FlareDisc g_flare[] = {
    { -0.5f, 62.0f, 70, 200, 0 },
    { -1.0f, 40.0f, 70, 200, 1 },
    { -1.5f, 22.0f, 50, 200, 2 },
    {  0.5f, 48.0f, 50, 200, 0 },
    { 0.05f, 80.0f, 40, 200, 1 },
    {  1.3f, 32.0f, 60, 200, 2 },
    { -2.0f, 48.0f, 50, 200, 0 },
    { -2.3f, 60.0f, 40, 200, 1 },
    { -3.0f, 64.0f, 40, 200, 2 },
};
#define FLARE_DISCS (int)(sizeof g_flare / sizeof g_flare[0])
#define FLARE_TEXES 3
#define FLARE_SIZE  64

/* A flaring light's place on the screen, found while the camera matrix is
 * loaded, drawn after */
typedef struct {
    float    x, y, inv_w;
    uint32_t rgb;
    float    fade;
} FlareAt;

static Corona          g_coronas[CORONAS];
static int             g_live;
static dttex_info_t    g_tex;
static bool            g_tex_tried;
static pvr_poly_hdr_t  g_hdr __attribute__((aligned(32)));
static pvr_poly_hdr_t  g_flare_hdr[FLARE_TEXES] __attribute__((aligned(32)));
static bool            g_flare_ok;  /* all FLARE_TEXES made: no flare without */
static uint32_t        g_stepped = 0xFFFFFFFFu;

/* Grey 0 to 1 as RGB565 with R, G and B the same 32 levels */
static uint16_t grey565(float f) {
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    uint16_t q = (uint16_t)(f * 31.0f + 0.5f);
    return (uint16_t)((q << 11) | ((q * 2 + (q >> 4)) << 5) | q);
}

static float clamp01(float f) { return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f; }

/* GTA's coronahex, coronacircle and coronaringa, drawn here rather than
 * shipped: a hexagon and a disc, each brighter to the edge, and a ring */
static bool flare_make(void) {
    static uint16_t px[FLARE_SIZE * FLARE_SIZE] __attribute__((aligned(32)));
    pvr_ptr_t made[FLARE_TEXES];
    for (int t = 0; t < FLARE_TEXES; t++) {
        for (int y = 0; y < FLARE_SIZE; y++)
            for (int x = 0; x < FLARE_SIZE; x++) {
                float fx = (x + 0.5f) / (FLARE_SIZE / 2) - 1.0f;
                float fy = (y + 0.5f) / (FLARE_SIZE / 2) - 1.0f;
                float ax = fx < 0 ? -fx : fx, ay = fy < 0 ? -fy : fy;
                float d = shz_sqrtf(fx * fx + fy * fy), f;
                if (t == 0) {
                    float h = ax * 0.866f + ay * 0.5f;
                    if (ay > h) h = ay;
                    f = clamp01((0.95f - h) * 12.0f) * (0.35f + 0.65f * h * h);
                } else if (t == 1) {
                    f = clamp01((0.95f - d) * 12.0f) * (0.35f + 0.65f * d * d);
                } else {
                    float r = d - 0.8f;
                    f = clamp01(1.0f - (r < 0 ? -r : r) * 8.0f);
                }
                px[y * FLARE_SIZE + x] = grey565(f);
            }

        pvr_ptr_t ptr = pvr_mem_malloc(sizeof px);
        if (!ptr) {
            printf("dc_corona: no VRAM for the flare, drawn without\n");
            for (int k = 0; k < t; k++) pvr_mem_free(made[k]);
            return false;
        }
        made[t] = ptr;
        pvr_txr_load(px, ptr, sizeof px);

        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                         FLARE_SIZE, FLARE_SIZE, ptr, PVR_FILTER_BILINEAR);
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_ONE;
        cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
        pvr_poly_compile(&g_flare_hdr[t], &cxt);
    }
    return true;
}

static bool tex_make(void) {
    if (g_tex_tried) return g_tex.ptr != NULL;
    g_tex_tried = true;

    const int n = CORONA_DEFAULT_SIZE;
    g_tex.ptr = pvr_mem_malloc(n * n * 2);
    if (!g_tex.ptr) {
        printf("dc_corona: no VRAM for the picture\n");
        return false;
    }
    pvr_txr_load(corona_default, g_tex.ptr, n * n * 2);
    g_tex.width = n;
    g_tex.height = n;
    g_tex.pvrformat = PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED;

    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, g_tex.pvrformat, n, n, g_tex.ptr,
                     PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
    cxt.blend.src = PVR_BLEND_SRCALPHA;
    cxt.blend.dst = PVR_BLEND_ONE;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(&g_hdr, &cxt);
    g_flare_ok = flare_make();
    return true;
}

/* Towards lit if it was asked for this frame, towards out if not; gone once
 * out */
static void step(float dt) {
    if (dt > 0.1f) dt = 0.1f;
    uint32_t frame = dc_frame_count();
    g_live = 0;
    for (int i = 0; i < CORONAS; i++) {
        Corona* c = &g_coronas[i];
        if (!c->used) continue;
        if (c->asked == frame) {
            c->fade += FADE_SPEED * dt;
            if (c->fade > 1.0f) c->fade = 1.0f;
        } else {
            c->fade -= FADE_SPEED * dt;
            if (c->fade <= 0.0f) { c->used = false; continue; }
        }
        g_live++;
    }
}

/* On the screen: from the middle of the view out through each light, only
 * while the light itself is in the view (as GTA). Each disc is depth tested
 * at the light's depth where it lands, so something nearer than the light
 * hides the disc it covers; a light hidden behind a wall is not known here
 * and still flares. */
static void flares_draw(pvr_dr_state_t* dr, const DCCamera* cam, const FlareAt* flares,
                        int n) {
    float x0 = 0.0f, y0 = 0.0f, sw, sh;
    dc_render_size(&sw, &sh);
    if (cam->view.w > 0.0f && cam->view.h > 0.0f) {
        x0 = cam->view.x; y0 = cam->view.y; sw = cam->view.w; sh = cam->view.h;
    }
    const float x1 = x0 + sw, y1 = y0 + sh;
    const float cx = x0 + sw * 0.5f, cy = y0 + sh * 0.5f, k = sh * (1.0f / 480.0f);

    for (int t = 0; t < FLARE_TEXES; t++) {
        bool sent = false;
        for (int i = 0; i < n; i++) {
            const FlareAt* f = &flares[i];
            if (f->x < x0 || f->y < y0 || f->x > x1 || f->y > y1) continue;
            float r = (float)((f->rgb >> 16) & 0xFF), g = (float)((f->rgb >> 8) & 0xFF),
                  b = (float)(f->rgb & 0xFF);

            for (int d = 0; d < FLARE_DISCS; d++) {
                const FlareDisc* fd = &g_flare[d];
                if (fd->tex != t) continue;
                float x = cx + (f->x - cx) * fd->where, y = cy + (f->y - cy) * fd->where;
                float h = fd->half * k;
                if (x + h < x0 || y + h < y0 || x - h > x1 || y - h > y1) continue;
                if (!sent) { dc_send_hdr(dr, &g_flare_hdr[t]); sent = true; }

                float m = fd->rgb * (1.0f / 256.0f);
                uint32_t argb = ((uint32_t)(f->fade * fd->alpha) << 24) |
                                ((uint32_t)(r * m) << 16) | ((uint32_t)(g * m) << 8) |
                                (uint32_t)(b * m);
                const float xs[4] = { x - h, x + h, x - h, x + h };
                const float ys[4] = { y - h, y - h, y + h, y + h };
                for (int v = 0; v < 4; v++) {
                    pvr_vertex_t* pv = (pvr_vertex_t*)pvr_dr_target(*dr);
                    pv->flags = (v == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                    pv->x = xs[v];
                    pv->y = ys[v];
                    pv->z = f->inv_w;
                    pv->u = (float)(v & 1);
                    pv->v = (float)(v >> 1);
                    pv->argb = argb;
                    pv->oargb = 0;
                    pvr_dr_commit(pv);
                }
            }
        }
    }
}

static void coronas_draw_cb(void* user) {
    const DCCamera* cam = user;
    if (!cam || !g_live || !g_tex.ptr) return;

    /* The ones on screen, first: the cull loads its planes into the matrix
     * the squares are sent through. The square is pulled up to its size
     * towards the camera and reaches 0.71 of its size corner to corner. */
    uint8_t seen[CORONAS];
    int n_seen = 0;
    for (int i = 0; i < CORONAS; i++) {
        const Corona* c = &g_coronas[i];
        if (c->used && c->size > 0.0f &&
            dc_frustum_cull_sphere(cam, c->pos, c->size * 1.71f) >= 0)
            seen[n_seen++] = (uint8_t)i;
    }
    if (!n_seen) return;

    pvr_dr_state_t* dr = dc_dr_state();
    dc_send_hdr(dr, &g_hdr);

    /* World coordinates, camera at the origin: as the particles */
    shz_xmtrx_load_4x4((shz_mat4x4_t*)dc_camera_get_pv(cam));
    shz_xmtrx_translate(-cam->pos.x, -cam->pos.y, cam->pos.z);

    shz_sincos_t y = shz_sincosf(cam->yaw), pi = shz_sincosf(cam->pitch);
    const float rx = -y.cos,         ry = 0.0f,   rz = y.sin;
    const float ux = y.sin * pi.sin, uy = pi.cos, uz = y.cos * pi.sin;

    static const float corner[4][2] = { {-1, -1}, {1, -1}, {-1, 1}, {1, 1} };
    static const float uv[4][2]     = { { 0,  1}, {1,  1}, { 0, 0}, {1, 0} };

    alignas(32) DMSVertex batch[BATCH_QUADS * 4];
    int in_batch = 0;
    FlareAt flares[CORONAS];
    int n_flares = 0;

    for (int s = 0; s < n_seen; s++) {
        const Corona* c = &g_coronas[seen[s]];
        float half = c->size * 0.5f;

        /* Pulled towards the camera by its size, so the lamp it sits in
         * doesn't cut it: at most half the way, so it never reaches the
         * camera */
        float dx = cam->pos.x - c->pos.x, dy = cam->pos.y - c->pos.y,
              dz = cam->pos.z - c->pos.z;
        float d2 = dx * dx + dy * dy + dz * dz;
        float inv_d = shz_inv_sqrtf(d2 > 1e-6f ? d2 : 1e-6f);
        float d = d2 * inv_d;
        float pull = c->size < d * 0.5f ? c->size * inv_d : 0.5f;
        float px = c->pos.x + dx * pull, py = c->pos.y + dy * pull,
              pz = c->pos.z + dz * pull;

        /* Where its middle lands, the way dc_model_submit_quads works it
         * out. The square faces the camera, so its corners are as deep. */
        shz_vec4_t t = shz_vec4_swizzle(shz_xmtrx_transform_vec4(
                           shz_vec4_init(px, py, -pz, 1.0f)), 1, 2, 3, 0);

        /* Fades out as the camera comes inside it (within its size, gone by
         * half) and before the near plane would cut it off */
        float in = (d - c->size * 0.5f) / (c->size * 0.5f);
        float near = (t.w - NEAR_Z) * (1.0f / NEAR_Z);
        float fade = c->fade * clamp01(in < near ? in : near);
        uint32_t alpha = (uint32_t)(fade * 255.0f);
        if (!alpha) continue;
        uint32_t argb = (alpha << 24) | (c->rgb & 0xFFFFFFu);

        /* Turned by 20 over its distance, as GTA: the star twists as the
         * camera comes and goes */
        shz_sincos_t sp = shz_sincosf(20.0f * inv_d);
        float ax = (rx * sp.cos + ux * sp.sin) * half;
        float ay = (ry * sp.cos + uy * sp.sin) * half;
        float az = (rz * sp.cos + uz * sp.sin) * half;
        float bx = (ux * sp.cos - rx * sp.sin) * half;
        float by = (uy * sp.cos - ry * sp.sin) * half;
        float bz = (uz * sp.cos - rz * sp.sin) * half;

        if ((c->flags & DC_CORONA_FLARE) && g_flare_ok && n_flares < CORONAS) {
            float inv_w = shz_invf_fsrra(t.w);   /* t.w > NEAR_Z: alpha above */
            FlareAt* f = &flares[n_flares++];
            f->x = t.x * inv_w;
            f->y = t.y * inv_w;
            f->inv_w = inv_w;
            f->rgb = c->rgb;
            f->fade = fade;
        }

        DMSVertex* v = &batch[in_batch * 4];
        for (int k = 0; k < 4; k++) {
            float cu = corner[k][0], cv = corner[k][1];
            v[k].x = px + ax * cu + bx * cv;
            v[k].y = py + ay * cu + by * cv;
            v[k].z = pz + az * cu + bz * cv;
            v[k].u = uv[k][0];
            v[k].v = uv[k][1];
            v[k].argb = argb;
            v[k].flags = (k == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        }
        if (++in_batch == BATCH_QUADS) {
            dc_model_submit_quads(batch, in_batch, dr);
            in_batch = 0;
        }
    }
    if (in_batch) dc_model_submit_quads(batch, in_batch, dr);

    if (n_flares) flares_draw(dr, cam, flares, n_flares);
}

/* At frame end (dc_draw_flush), whether or not any were asked for this
 * frame: the ones that weren't still have to fade out */
void dc_corona_flush(void) {
    uint32_t frame = dc_frame_count();
    if (g_stepped == frame) return;
    g_stepped = frame;
    step(dc_delta_time());
    if (!g_live) return;

    /* Every view of the screen the frame was drawn with, or the camera set
     * now when nothing was drawn. Onto the screen, whatever target the game
     * left set (the flush clears it after anyway). */
    const DCCamera* cams[VIEWS];
    int n = dc_draw_screen_cameras(cams, VIEWS);
    const DCCamera* now = dc_get_camera();
    if (!n && now) cams[n++] = now;
    dc_set_target(NULL);
    for (int i = 0; i < n; i++) {
        dc_set_camera(cams[i]);
        dc_draw_call(PVR_LIST_TR_POLY, coronas_draw_cb, (void*)cams[i]);
    }
    dc_set_camera(now);
}

void dc_corona(uint32_t id, shz_vec3_t pos, uint32_t rgb, float size, uint32_t flags) {
    /* Here, not when drawing: loading it mid-list would clash with the
     * store queues the list is being sent through */
    if (!tex_make()) return;

    Corona* free_slot = NULL;
    for (int i = 0; i < CORONAS; i++) {
        Corona* c = &g_coronas[i];
        if (c->used && c->id == id) { free_slot = c; break; }
        if (!c->used && !free_slot) free_slot = c;
    }
    if (!free_slot) return;
    Corona* c = free_slot;
    if (!c->used) {
        c->used = true;
        c->id = id;
        c->fade = 0.0f;
    }
    c->pos = pos;
    c->rgb = rgb;
    c->size = size;
    c->flags = flags;
    c->asked = dc_frame_count();
}
