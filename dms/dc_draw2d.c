#include "dc_draw2d.h"
#include "dc_engine.h"
#include "font.h"
#include "dc_draw.h"
#include "dc_model.h"
#include "pvrtex.h"
#include "dc_camera.h"
#include <stdio.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

void dc_draw2d_init(void) {
    InitFont();
}

struct DCImage {
    dttex_info_t tex;
};

/* Text and images are kept until the end of the frame and drawn in the order
 * they were asked for, in the translucent list, each one a little nearer than
 * the one before so the PVR's depth sort keeps that order. */
#define QUEUE_MAX      512
#define TEXT_MAX_CHARS 96
#define LAYER_DEPTH    100.0f   /* 1/w: nearer than anything 3D (FAR_Z is 0.001, targets sit at 50) */
#define LAYER_STEP     0.01f

typedef struct {
    enum { ENTRY_TEXT, ENTRY_IMAGE, ENTRY_RECT } kind;
    float    x, y, w, h;          /* image, rect: on screen. text: x, y and size in w */
    float    u0, v0, u1, v1;      /* image */
    uint32_t argb;
    bool     add;
    const DCImage* img;
    char     text[TEXT_MAX_CHARS];
} Entry;

static Entry queue[QUEUE_MAX];
static int   queue_count;

static Entry* queue_push(void) {
    if (queue_count >= QUEUE_MAX) {
        static bool warned;
        if (!warned) {
            printf("DMS: more than %d text and image draws in a frame, the rest are dropped\n", QUEUE_MAX);
            warned = true;
        }
        return NULL;
    }
    return &queue[queue_count++];
}

void dc_draw_text(const char* text, int x, int y, int size, uint32_t argb) {
    if (!text) return;
    Entry* e = queue_push();
    if (!e) return;
    e->kind = ENTRY_TEXT;
    strncpy(e->text, text, TEXT_MAX_CHARS - 1);
    e->text[TEXT_MAX_CHARS - 1] = '\0';
    e->x = (float)x; e->y = (float)y; e->w = (float)size;
    e->argb = argb;
}

int dc_text_width(const char* text, int size) {
    if (!text) return 0;
    const float scale = (float)size / DEFAULT_FONT_SIZE;
    float w = 0.0f, best = 0.0f;
    for (; *text; text++) {
        if (*text == '\n') { if (w > best) best = w; w = 0.0f; continue; }
        int index = *text - 32;
        if (index < 0 || index >= 224) continue;
        w += (charInfo[index].width + 2) * scale;
    }
    return (int)(w > best ? w : best);
}

void dc_draw_rect(float x, float y, float w, float h, uint32_t argb) {
    if ((argb >> 24) == 0 || w <= 0.0f || h <= 0.0f) return;
    Entry* e = queue_push();
    if (!e) return;
    e->kind = ENTRY_RECT;
    e->x = x; e->y = y; e->w = w; e->h = h;
    e->argb = argb;
    e->add = false;
    e->img = NULL;
}

/* ---- Images ---- */

DCImage* dc_image_load(const char* filename) {
    DCImage* img = (DCImage*)calloc(1, sizeof(DCImage));
    if (!img) return NULL;
    if (!pvrtex_load(filename, &img->tex)) {
        printf("dc_image_load: cannot load %s\n", filename);
        free(img);
        return NULL;
    }
    return img;
}

void dc_image_free(DCImage* img) {
    if (!img) return;
    pvrtex_unload(&img->tex);
    free(img);
}

const void* dc_image_tex(const DCImage* img) { return img ? &img->tex : NULL; }
int dc_image_width(const DCImage* img)  { return img ? img->tex.pixel_w : 0; }
int dc_image_height(const DCImage* img) { return img ? img->tex.pixel_h : 0; }

void dc_draw_image(const DCImage* img, float x, float y) {
    dc_draw_image_ex(img, &(DCImageOpts){ .x = x, .y = y });
}

void dc_draw_image_ex(const DCImage* img, const DCImageOpts* opts) {
    if (!img || !opts) return;
    float alpha = opts->alpha > 0.0f ? opts->alpha : 1.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    if (alpha < 0.004f) return;   /* nothing to see */

    Entry* e = queue_push();
    if (!e) return;
    e->kind = ENTRY_IMAGE;
    e->img = img;

    float pw = (float)img->tex.pixel_w, ph = (float)img->tex.pixel_h;
    float sx = opts->src_x, sy = opts->src_y;
    float sw = opts->src_width  > 0.0f ? opts->src_width  : pw - sx;
    float sh = opts->src_height > 0.0f ? opts->src_height : ph - sy;
    e->w = opts->width  > 0.0f ? opts->width  : sw;
    e->h = opts->height > 0.0f ? opts->height : sh;
    e->x = opts->center ? opts->x - e->w * 0.5f : opts->x;
    e->y = opts->center ? opts->y - e->h * 0.5f : opts->y;

    /* UVs over the PVR's (power of two) texture */
    float tw = (float)img->tex.width, th = (float)img->tex.height;
    e->u0 = sx / tw;        e->v0 = sy / th;
    e->u1 = (sx + sw) / tw; e->v1 = (sy + sh) / th;

    uint32_t tint = opts->tint ? (opts->tint & 0x00FFFFFF) : 0x00FFFFFF;
    e->argb = ((uint32_t)(alpha * 255.0f) << 24) | tint;
    e->add = opts->add;
}

/* One header for the font, one per image and blend; sent again only when
 * the next entry needs a different one */
static void image_header(pvr_dr_state_t* dr, const DCImage* img, bool add) {
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, img->tex.pvrformat,
                     img->tex.width, img->tex.height, img->tex.ptr, PVR_FILTER_NONE);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
    cxt.blend.src = PVR_BLEND_SRCALPHA;
    cxt.blend.dst = add ? PVR_BLEND_ONE : PVR_BLEND_INVSRCALPHA;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_hdr_t* hdr = (pvr_poly_hdr_t*)pvr_dr_target(*dr);
    dc_cxt_clip(&cxt); pvr_poly_compile(hdr, &cxt);
    pvr_dr_commit(hdr);
}

static void rect_header(pvr_dr_state_t* dr) {
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_hdr_t* hdr = (pvr_poly_hdr_t*)pvr_dr_target(*dr);
    dc_cxt_clip(&cxt); pvr_poly_compile(hdr, &cxt);
    pvr_dr_commit(hdr);
}

static void font_header(pvr_dr_state_t* dr) {
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, fontTexture.pvrformat,
                     fontTexture.width, fontTexture.height, fontTexture.ptr, PVR_FILTER_NONE);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_hdr_t* hdr = (pvr_poly_hdr_t*)pvr_dr_target(*dr);
    dc_cxt_clip(&cxt); pvr_poly_compile(hdr, &cxt);
    pvr_dr_commit(hdr);
}

static void image_draw(pvr_dr_state_t* dr, const Entry* e, float z) {
    (void)dr;   /* this KOS's pvr_dr_target() does not use it */
    const float xs[4] = { e->x, e->x + e->w, e->x, e->x + e->w };
    const float ys[4] = { e->y, e->y, e->y + e->h, e->y + e->h };
    const float us[4] = { e->u0, e->u1, e->u0, e->u1 };
    const float vs[4] = { e->v0, e->v0, e->v1, e->v1 };
    for (int i = 0; i < 4; i++) {
        pvr_vertex_t* v = (pvr_vertex_t*)pvr_dr_target(*dr);
        v->flags = (i == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v->x = xs[i]; v->y = ys[i]; v->z = z;
        v->u = us[i]; v->v = vs[i];
        v->argb = e->argb;
        v->oargb = 0;
        pvr_dr_commit(v);
    }
}

void dc_draw2d_flush(void) {
    if (queue_count == 0) return;
    pvr_dr_state_t* dr = dc_list_begin(PVR_LIST_TR_POLY);
    SetDrawingState(dr);

    /* What the last header was for: the font, a rect, or an image and blend */
    enum { HDR_NONE, HDR_FONT, HDR_RECT, HDR_IMAGE } hdr = HDR_NONE;
    const DCImage* hdr_img = NULL;
    bool hdr_add = false;
    float z = LAYER_DEPTH;

    for (int i = 0; i < queue_count; i++, z += LAYER_STEP) {
        const Entry* e = &queue[i];
        if (e->kind == ENTRY_TEXT) {
            if (hdr != HDR_FONT) { font_header(dr); hdr = HDR_FONT; }
            Color c = { (e->argb >> 16) & 0xFF, (e->argb >> 8) & 0xFF,
                        e->argb & 0xFF, (e->argb >> 24) & 0xFF };
            DrawText(e->text, (int)e->x, (int)e->y, (int)e->w, c, z);
        } else if (e->kind == ENTRY_RECT) {
            if (hdr != HDR_RECT) { rect_header(dr); hdr = HDR_RECT; }
            image_draw(dr, e, z);
        } else {
            if (hdr != HDR_IMAGE || e->img != hdr_img || e->add != hdr_add) {
                image_header(dr, e->img, e->add);
                hdr = HDR_IMAGE; hdr_img = e->img; hdr_add = e->add;
            }
            image_draw(dr, e, z);
        }
    }
    queue_count = 0;
}

/* Depth is 1/w. Just in front of the PVR's own background plane (0.0001 in
 * KOS), and behind anything nearer than 5000 units. */
#define BACKGROUND_DEPTH 0.0002f

static void background_draw(void* user) {
    const DCImage* img = (const DCImage*)user;
    pvr_dr_state_t* dr = dc_dr_state();
    (void)dr;   /* this KOS's pvr_dr_target() does not use it */

    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, img->tex.pvrformat,
                     img->tex.width, img->tex.height, img->tex.ptr,
                     PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_hdr_t* hdr = (pvr_poly_hdr_t*)pvr_dr_target(*dr);
    dc_cxt_clip(&cxt); pvr_poly_compile(hdr, &cxt);
    pvr_dr_commit(hdr);

    /* The screen, or the render target being drawn into */
    float rw, rh;
    dc_render_size(&rw, &rh);

    static const float corner[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (int i = 0; i < 4; i++) {
        pvr_vertex_t* v = (pvr_vertex_t*)pvr_dr_target(*dr);
        v->flags = (i == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v->x = corner[i][0] * rw;
        v->y = corner[i][1] * rh;
        v->z = BACKGROUND_DEPTH;
        v->u = corner[i][0];
        v->v = corner[i][1];
        v->argb = 0xFFFFFFFF;
        v->oargb = 0;
        pvr_dr_commit(v);
    }
}

int dc_model_texture(DMSModel* model, const char* material, const DCImage* img) {
    return dc_model_retexture(model, material, img ? &img->tex : NULL);
}

void dc_draw_background(const DCImage* img) {
    if (img) dc_draw_call(PVR_LIST_OP_POLY, background_draw, (void*)img);
}

/* ================================================================
 * Decals
 * ================================================================ */

#define DECALS_A_FRAME 64
#define BLOB_SIZE      32

typedef struct {
    const DCImage*  img;
    const DCCamera* cam;
    DCDecalOpts     o;
} Decal;

static Decal    decals[DECALS_A_FRAME];
static int      decal_count;
static uint32_t decal_frame;
static DCImage  blob;           /* the dark spot, made the first time it is asked for */
static bool     blob_tried;

/* Black, solid in the middle and fading to nothing at the edge. ARGB4444 */
static bool blob_make(void) {
    if (blob.tex.ptr) return true;
    if (blob_tried) return false;
    blob_tried = true;
    const int n = BLOB_SIZE;
    uint16_t* pix = (uint16_t*)malloc(n * n * 2);
    if (!pix) return false;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            float dx = ((float)x + 0.5f) * (2.0f / n) - 1.0f;
            float dy = ((float)y + 0.5f) * (2.0f / n) - 1.0f;
            float d = shz_sqrtf_fsrra(dx * dx + dy * dy);
            float a = (1.0f - d) * 2.5f;        /* solid inside 0.6, soft beyond */
            if (a < 0.0f) a = 0.0f;
            if (a > 1.0f) a = 1.0f;
            a = a * a * (3.0f - 2.0f * a);
            pix[y * n + x] = (uint16_t)((uint32_t)(a * 15.0f + 0.5f) << 12);
        }
    blob.tex.ptr = pvr_mem_malloc(n * n * 2);
    if (!blob.tex.ptr) { free(pix); printf("dc_draw_decal: no VRAM for the blob\n"); return false; }
    pvr_txr_load(pix, blob.tex.ptr, n * n * 2);
    free(pix);
    blob.tex.width = blob.tex.height = n;
    blob.tex.pvrformat = PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED;
    return true;
}

static void decal_draw_cb(void* user) {
    const Decal* d = (const Decal*)user;
    const DCCamera* cam = d->cam;
    const dttex_info_t* tex = &d->img->tex;
    pvr_dr_state_t* dr = dc_dr_state();

    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, tex->pvrformat, tex->width, tex->height, tex->ptr, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
    cxt.blend.src = PVR_BLEND_SRCALPHA;
    cxt.blend.dst = d->o.add ? PVR_BLEND_ONE : PVR_BLEND_INVSRCALPHA;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_hdr_t* hdr = (pvr_poly_hdr_t*)pvr_dr_target(*dr);
    dc_cxt_clip(&cxt); pvr_poly_compile(hdr, &cxt);
    pvr_dr_commit(hdr);

    /* World coordinates, camera at the origin: as the particles */
    shz_xmtrx_load_4x4((shz_mat4x4_t*)dc_camera_get_pv(cam));
    shz_xmtrx_translate(-cam->pos.x, -cam->pos.y, cam->pos.z);

    /* Two directions across the floor, at right angles to its normal and to
     * each other, turned by yaw */
    shz_vec3_t n = d->o.normal;
    shz_vec3_t any = shz_vec3_init(1.0f, 0.0f, 0.0f);
    if (n.x > 0.9f || n.x < -0.9f) any = shz_vec3_init(0.0f, 0.0f, 1.0f);
    shz_vec3_t a = shz_vec3_normalize(shz_vec3_cross(n, any));
    shz_vec3_t b = shz_vec3_cross(n, a);
    if (d->o.yaw != 0.0f) {
        shz_sincos_t sc = shz_sincosf(d->o.yaw);
        shz_vec3_t a2 = shz_vec3_add(shz_vec3_scale(a, sc.cos), shz_vec3_scale(b, sc.sin));
        b = shz_vec3_sub(shz_vec3_scale(b, sc.cos), shz_vec3_scale(a, sc.sin));
        a = a2;
    }
    float half = d->o.size * 0.5f;
    a = shz_vec3_scale(a, half);
    b = shz_vec3_scale(b, half);

    uint32_t argb = ((uint32_t)(d->o.alpha * 255.0f) << 24) | (d->o.tint & 0xFFFFFF);
    static const float corner[4][2] = { {-1, -1}, {1, -1}, {-1, 1}, {1, 1} };
    static const float uv[4][2]     = { { 0,  1}, {1,  1}, { 0, 0}, {1, 0} };
    alignas(32) DMSVertex v[4];
    for (int c = 0; c < 4; c++) {
        float cu = corner[c][0], cv = corner[c][1];
        v[c].x = d->o.pos.x + a.x * cu + b.x * cv;
        v[c].y = d->o.pos.y + a.y * cu + b.y * cv;
        v[c].z = d->o.pos.z + a.z * cu + b.z * cv;
        v[c].u = uv[c][0];
        v[c].v = uv[c][1];
        v[c].argb = argb;
        v[c].flags = (c == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    }
    dc_model_submit_quads(v, 1, dr);
}

void dc_draw_decal(const DCImage* img, const DCDecalOpts* opts) {
    if (!opts) return;
    const DCCamera* cam = dc_get_camera();
    if (!cam) return;
    if (!img) { if (!blob_make()) return; img = &blob; }

    uint32_t frame = dc_frame_count();
    if (decal_frame != frame) { decal_frame = frame; decal_count = 0; }
    if (decal_count >= DECALS_A_FRAME) return;

    Decal* d = &decals[decal_count];
    d->img = img;
    d->cam = cam;
    d->o = *opts;
    if (d->o.size <= 0.0f)  d->o.size = 1.0f;
    if (d->o.alpha <= 0.0f) d->o.alpha = 1.0f;
    if (d->o.alpha > 1.0f)  d->o.alpha = 1.0f;
    if (d->o.tint == 0)     d->o.tint = 0xFFFFFF;
    float len2 = shz_vec3_dot(d->o.normal, d->o.normal);
    if (len2 < 1e-6f) d->o.normal = shz_vec3_init(0.0f, 1.0f, 0.0f);
    else if (len2 < 0.99f || len2 > 1.01f) d->o.normal = shz_vec3_normalize(d->o.normal);
    if (dc_frustum_cull_sphere(cam, d->o.pos, d->o.size * 0.71f) < 0) return;

    decal_count++;
    dc_draw_call(PVR_LIST_TR_POLY, decal_draw_cb, d);
}
