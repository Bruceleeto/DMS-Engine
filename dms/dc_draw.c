#include "dc_draw.h"
#include "dc_draw2d.h"
#include "dc_engine.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

#define DRAW_QUEUE_MAX 256

typedef enum { ENTRY_MODEL, ENTRY_CALL } EntryKind;

/* One queued draw. A struct, so lights and effects can be added to it later
 * without the calls that fill it changing. */
typedef struct {
    EntryKind       kind;
    DCTarget*       target;     /* NULL for the screen */
    DMSModel*       model;
    const DCCamera* cam;
    shz_vec3_t      pos;
    float           scale;
    float           yaw;
    bool            has_rot;
    float           rot[9];
    bool            add;
    bool            glow;       /* the bloom pass: only what gives off light */
    bool            has_shadow;
    DCShadow        shadow;
    int             call_list;
    void          (*call_fn)(void* user);
    void*           call_user;
} DrawEntry;

static DrawEntry       queue[DRAW_QUEUE_MAX];
static int             queue_count;
static const DCCamera* current_cam;
static DCTarget*       current_target;

static DrawEntry* queue_push(void) {
    if (queue_count >= DRAW_QUEUE_MAX) {
        static bool warned;
        if (!warned) {
            printf("dc_draw: more than %d draws in a frame, the rest are dropped\n",
                   DRAW_QUEUE_MAX);
            warned = true;
        }
        return NULL;
    }
    DrawEntry* e = &queue[queue_count++];
    e->target = current_target;
    return e;
}

static DrawEntry* queue_model(DMSModel* model, shz_vec3_t pos, float scale) {
    if (!model) return NULL;
    if (!current_cam) {
        static bool warned;
        if (!warned) {
            printf("dc_draw: call dc_set_camera() first\n");
            warned = true;
        }
        return NULL;
    }
    DrawEntry* e = queue_push();
    if (!e) return NULL;
    e->kind = ENTRY_MODEL;
    e->model = model;
    e->cam = current_cam;
    e->pos = pos;
    e->scale = scale;
    e->yaw = 0.0f;
    e->has_rot = false;
    e->add = false;
    e->glow = false;
    e->has_shadow = false;
    return e;
}

const DCCamera* dc_get_camera(void) {
    return current_cam;
}

void dc_set_camera(const DCCamera* cam) {
    current_cam = cam;
}

void dc_set_light(const DCLight* light) {
    dc_model_set_light(light);
}

void dc_draw(DMSModel* model, shz_vec3_t pos) {
    queue_model(model, pos, 1.0f);
}

void dc_draw_ex(DMSModel* model, const DCDrawOpts* opts) {
    if (!opts) return;
    DrawEntry* e = queue_model(model, opts->pos, opts->scale != 0.0f ? opts->scale : 1.0f);
    if (!e) return;
    e->yaw = opts->yaw;
    e->add = opts->add;
    if (opts->shadow) {
        e->has_shadow = true;
        e->shadow = *opts->shadow;
    }
    if (opts->rot) {
        e->has_rot = true;
        memcpy(e->rot, opts->rot, sizeof(e->rot));
    }
}

void dc_draw_call(int pvr_list, void (*fn)(void* user), void* user) {
    if (!fn) return;
    DrawEntry* e = queue_push();
    if (!e) return;
    e->kind = ENTRY_CALL;
    e->call_list = pvr_list;
    e->call_fn = fn;
    e->call_user = user;
}

/* ================================================================
 * Render targets
 * ================================================================ */

#define TARGET_WEARERS 16
#define TARGETS_A_FRAME 8
#define TARGET_QUADS 16     /* bloom takes nine of them */
#define TARGET_BACK_DEPTH 0.0001f   /* 1/w: behind everything (FAR_Z is 0.001) */
#define TARGET_QUAD_DEPTH 50.0f     /* 1/w: in front of anything past 0.02 units */

typedef struct {
    DMSMesh*       mesh;
    pvr_poly_hdr_t hdr[2] __attribute__((aligned(32)));   /* one per picture */
} TargetWearer;

struct DCTarget {
    int       width, height;
    pvr_ptr_t txr[2];
    int       front;            /* the picture being shown; the other is drawn into */
    bool      feeds_itself;     /* its own picture is drawn into it (a trail): dithering is off */
    int       wearer_count;
    TargetWearer wearers[TARGET_WEARERS] __attribute__((aligned(32)));
};

static bool power_of_two(int n) { return n >= 8 && n <= 1024 && !(n & (n - 1)); }

DCTarget* dc_target_create(int width, int height) {
    if (!power_of_two(width) || !power_of_two(height)) {
        printf("dc_target_create: %dx%d, sizes must be powers of two from 8 to 1024\n",
               width, height);
        return NULL;
    }
    DCTarget* t = memalign(32, sizeof(DCTarget));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->width = width;
    t->height = height;
    for (int i = 0; i < 2; i++) {
        t->txr[i] = pvr_mem_malloc(width * height * 2);
        if (!t->txr[i]) {
            /* Still a target, with no picture: what is drawn into it is
             * dropped, not sent to the screen */
            printf("dc_target_create: no VRAM for %dx%d (%luKB free)\n", width, height,
                   (unsigned long)pvr_mem_available() / 1024);
            if (t->txr[0]) pvr_mem_free(t->txr[0]);
            t->txr[0] = t->txr[1] = NULL;
            return t;
        }
        /* Black until first drawn */
        memset(t->txr[i], 0, width * height * 2);
    }
    return t;
}

void dc_target_free(DCTarget* t) {
    if (!t) return;
    /* A scene still rendering may be reading or writing it */
    pvr_wait_ready();
    if (current_target == t) current_target = NULL;
    if (t->feeds_itself) vid_set_dithering(true);
    for (int i = 0; i < 2; i++)
        if (t->txr[i]) pvr_mem_free(t->txr[i]);
    free(t);
}

void dc_set_target(DCTarget* target) {
    current_target = target;
}

/* UVs for a flat mesh that has none: the picture across its bounds, as seen
 * by someone facing it */
static void flat_uvs(DMSMesh* mesh) {
    if (!mesh->vertex_count) return;
    /* No UVs in Blender comes out as one UV for every vertex (0,0, or 0,1 with
     * v flipped) */
    for (uint32_t i = 1; i < mesh->vertex_count; i++)
        if (mesh->vertices[i].u != mesh->vertices[0].u ||
            mesh->vertices[i].v != mesh->vertices[0].v) return;

    shz_vec3_t n = shz_vec3_init(0.0f, 0.0f, 0.0f);
    for (uint32_t i = 0; i < mesh->vertex_count; i++)
        n = shz_vec3_add(n, shz_vec3_init(mesh->vertices[i].nx, mesh->vertices[i].ny,
                                          mesh->vertices[i].nz));
    if (shz_vec3_dot(n, n) < 1.0f) return;
    n = shz_vec3_normalize(n);

    /* A screen lying flat has its top towards -z */
    shz_vec3_t world_up = (n.y > 0.9f || n.y < -0.9f) ? shz_vec3_init(0.0f, 0.0f, -1.0f)
                                                      : shz_vec3_init(0.0f, 1.0f, 0.0f);
    /* The engine draws x to the right when looking along +z (left handed), so
     * someone facing the mesh has n x up on their right */
    shz_vec3_t right = shz_vec3_normalize(shz_vec3_cross(n, world_up));
    shz_vec3_t up = shz_vec3_cross(right, n);

    float r0 = 1e30f, r1 = -1e30f, u0 = 1e30f, u1 = -1e30f;
    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
        shz_vec3_t p = shz_vec3_init(mesh->vertices[i].x, mesh->vertices[i].y, mesh->vertices[i].z);
        float r = shz_vec3_dot(p, right), u = shz_vec3_dot(p, up);
        if (r < r0) r0 = r;
        if (r > r1) r1 = r;
        if (u < u0) u0 = u;
        if (u > u1) u1 = u;
    }
    if (r1 - r0 < 1e-6f || u1 - u0 < 1e-6f) return;

    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
        shz_vec3_t p = shz_vec3_init(mesh->vertices[i].x, mesh->vertices[i].y, mesh->vertices[i].z);
        mesh->vertices[i].u = (shz_vec3_dot(p, right) - r0) / (r1 - r0);
        mesh->vertices[i].v = 1.0f - (shz_vec3_dot(p, up) - u0) / (u1 - u0);
    }
}

int dc_target_show_on(DCTarget* t, DMSModel* model, const char* material) {
    if (!t || !model || !material) return 0;
    if (!t->txr[0]) return 0;
    if (!model->material_names) {
        printf("dc_target_show_on: the model has no material names, reconvert it\n");
        return 0;
    }
    int n = 0;
    for (uint32_t m = 0; m < model->mesh_count; m++) {
        DMSMesh* mesh = &model->meshes[m];
        if (strncmp(model->material_names[m], material, 32) != 0) continue;
        flat_uvs(mesh);
        /* A screen gives off its own light: the picture as it is, not dimmed
         * by the room's baked lighting */
        for (uint32_t i = 0; i < mesh->vertex_count; i++)
            mesh->vertices[i].argb |= 0x00ffffffu;
        if (t->wearer_count >= TARGET_WEARERS) {
            printf("dc_target_show_on: more than %d meshes on one target\n", TARGET_WEARERS);
            break;
        }
        TargetWearer* w = &t->wearers[t->wearer_count++];
        w->mesh = mesh;
        for (int i = 0; i < 2; i++)
            dc_model_compile_header(mesh, &w->hdr[i],
                                    PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                                    t->width, t->height, t->txr[i]);
        mesh->header = w->hdr[t->front];
        n++;
    }
    if (!n) printf("dc_target_show_on: no mesh has the material \"%s\"\n", material);
    return n;
}

/* The picture just drawn becomes the one shown */
static void target_flip(DCTarget* t) {
    t->front ^= 1;
    for (int i = 0; i < t->wearer_count; i++)
        t->wearers[i].mesh->header = t->wearers[i].hdr[t->front];
}

typedef struct {
    DCTarget* target;
    float x, y, w, h;
    uint32_t argb;
    bool see_through, add;
    bool behind;        /* drawn into its own target: last frame's picture as the backdrop */
} TargetQuad;

static TargetQuad target_quads[TARGET_QUADS];
static int        target_quad_count;

static void target_quad_draw(void* user) {
    const TargetQuad* q = (const TargetQuad*)user;
    const DCTarget* t = q->target;
    pvr_dr_state_t* dr = dc_dr_state();
    (void)dr;   /* this KOS's pvr_dr_target() does not use it */

    /* No size given: over everything being drawn into */
    float x = q->x, y = q->y, w = q->w, h = q->h;
    if (w <= 0.0f || h <= 0.0f) {
        x = y = 0.0f;
        dc_render_size(&w, &h);
    }

    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, q->see_through ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY,
                     PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                     t->width, t->height, t->txr[t->front], PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE;
    if (q->see_through) {
        /* The picture has no alpha of its own: it all comes from the vertex */
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = q->add ? PVR_BLEND_ONE : PVR_BLEND_INVSRCALPHA;
        cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    }
    pvr_poly_hdr_t* hdr = (pvr_poly_hdr_t*)pvr_dr_target(*dr);
    pvr_poly_compile(hdr, &cxt);
    pvr_dr_commit(hdr);

    static const float corner[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (int i = 0; i < 4; i++) {
        pvr_vertex_t* v = (pvr_vertex_t*)pvr_dr_target(*dr);
        v->flags = (i == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v->x = x + corner[i][0] * w;
        v->y = y + corner[i][1] * h;
        v->z = q->behind ? TARGET_BACK_DEPTH : TARGET_QUAD_DEPTH;
        v->u = corner[i][0];
        v->v = corner[i][1];
        v->argb = q->argb;
        v->oargb = 0;
        pvr_dr_commit(v);
    }
}

void dc_draw_target(DCTarget* target, float x, float y, float width, float height) {
    if (!target || !target->txr[0] || target_quad_count >= TARGET_QUADS) return;
    TargetQuad* q = &target_quads[target_quad_count++];
    q->target = target;
    q->x = x; q->y = y; q->w = width; q->h = height;
    q->argb = 0xFFFFFFFF;
    q->see_through = q->add = false;
    q->behind = false;
    dc_draw_call(PVR_LIST_OP_POLY, target_quad_draw, q);
}

void dc_draw_target_ex(DCTarget* target, const DCTargetOpts* opts) {
    if (!target || !opts || !target->txr[0] || target_quad_count >= TARGET_QUADS) return;
    float alpha = opts->alpha > 0.0f ? opts->alpha : 1.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    TargetQuad* q = &target_quads[target_quad_count++];
    q->target = target;
    q->x = opts->x; q->y = opts->y; q->w = opts->width; q->h = opts->height;
    q->argb = ((uint32_t)(alpha * 255.0f) << 24) | 0x00FFFFFF;
    q->add = opts->add;
    q->see_through = opts->add || alpha < 1.0f;
    /* Into itself: what is drawn this frame goes over last frame's picture at
     * full strength, and fades behind it frame after frame (a trail) */
    q->behind = current_target == target;
    if (q->behind && !target->feeds_itself) {
        /* The PVR dithers what it renders: a small fixed pattern added before
         * the colour is cut to 16 bits. Fed back every frame, the pattern wins
         * over the fade and dark leftovers never clear. KOS has one switch for
         * the screen and textures alike, so it goes off while this target lives. */
        target->feeds_itself = true;
        vid_set_dithering(false);
    }
    if (q->behind && !opts->add) {
        /* Behind everything there is only the black the target starts from,
         * so see-through over it is the same as the picture made darker: a
         * solid quad, which the PVR draws far cheaper than a blended one */
        uint32_t grey = (uint32_t)(alpha * 255.0f);
        q->argb = 0xFF000000 | (grey << 16) | (grey << 8) | grey;
        q->see_through = false;
    }
    dc_draw_call(q->see_through ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY, target_quad_draw, q);
}

/* ================================================================
 * Bloom
 *
 * Three steps, all on what is already queued:
 *
 *   glow    the whole scene into a small square picture, with everything that
 *           does not give off light drawn in black. The camera is squeezed to
 *           it the same way any render target is, so a mesh lands where it is
 *           on screen, and the black takes the depth test, so a lamp behind a
 *           wall does not come through it.
 *   soften  across, then down, into two pictures half that size again. Four
 *           taps a pass, a texel apart, a quarter strength each, added. Half
 *           the size doubles how far a tap reaches for a quarter of the work,
 *           and going down to it is a 2x2 box the filtering does for free.
 *   add     the last picture over the frame, stretched back to full size.
 *           Magnifying it is the rest of the softening and costs nothing.
 *
 * Blurring across and then down rather than in one go is what keeps the edge
 * of a glow smooth: a single pass of taps set out on the diagonal leaves the
 * grid of the small picture showing once it is magnified ten times.
 *
 * The glow picture is 128x128 by default and the two soften pictures 64x64, so
 * a soften pass is 4k pixels. The cost of the effect is the second pass over
 * the geometry and the three renders, so the first three steps are done every
 * other frame and the picture they leave is added over both. The frame it goes
 * over is always this frame's, and what is held is a blob a few texels across.
 * ================================================================ */

#define BLOOM_TAPS 4

static DCBloom   g_bloom;
static bool      g_bloom_on;
static DCTarget* g_bloom_glow;      /* the scene, black but for what glows */
static DCTarget* g_bloom_x;         /* softened across */
static DCTarget* g_bloom_y;         /* and then down: what goes over the frame */
static int       g_bloom_size;      /* what the glow picture was made at */
static int       g_bloom_soft;      /* and the two softened ones: half of it */
static bool      g_bloom_held;      /* g_bloom_y holds last frame's glow, and
                                     * this frame shows it again instead of
                                     * drawing a new one */

/* Black over the whole of what is being drawn into, at the far depth. The
 * background the PVR fills in is the colour the screen clears to, which for
 * these two pictures would come out as a glow over the whole frame. Covering
 * it costs 16k opaque pixels and does not depend on when the PVR reads that
 * colour. */
static void bloom_black_draw(void* user) {
    pvr_dr_state_t* dr = dc_dr_state();
    (void)user;
    (void)dr;   /* this KOS's pvr_dr_target() does not use it */

    float w, h;
    dc_render_size(&w, &h);

    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_hdr_t* hdr = (pvr_poly_hdr_t*)pvr_dr_target(*dr);
    pvr_poly_compile(hdr, &cxt);
    pvr_dr_commit(hdr);

    static const float corner[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (int i = 0; i < 4; i++) {
        pvr_vertex_t* v = (pvr_vertex_t*)pvr_dr_target(*dr);
        v->flags = (i == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v->x = corner[i][0] * w;
        v->y = corner[i][1] * h;
        v->z = TARGET_BACK_DEPTH;
        v->u = v->v = 0.0f;
        v->argb = 0xFF000000;
        v->oargb = 0;
        pvr_dr_commit(v);
    }
}

void dc_set_bloom(const DCBloom* bloom) {
    g_bloom_on = bloom != NULL;
    /* Turned off, what is in the pictures is nothing to do with the next
     * frame that turns it back on */
    if (!bloom) { g_bloom_held = false; return; }
    g_bloom = *bloom;
    if (g_bloom.strength <= 0.0f) g_bloom.strength = 1.0f;
    if (g_bloom.strength > 1.0f)  g_bloom.strength = 1.0f;
    if (g_bloom.spread <= 0.0f)   g_bloom.spread = 1.0f;
    if (g_bloom.size <= 0)        g_bloom.size = 128;
    if (g_bloom.size > 256)       g_bloom.size = 256;

    if (g_bloom_size != g_bloom.size) {
        if (g_bloom_glow) dc_target_free(g_bloom_glow);
        if (g_bloom_x)    dc_target_free(g_bloom_x);
        if (g_bloom_y)    dc_target_free(g_bloom_y);
        g_bloom_size = g_bloom.size;
        g_bloom_soft = g_bloom_size > 8 ? g_bloom_size / 2 : 8;
        g_bloom_glow = dc_target_create(g_bloom_size, g_bloom_size);
        g_bloom_x    = dc_target_create(g_bloom_soft, g_bloom_soft);
        g_bloom_y    = dc_target_create(g_bloom_soft, g_bloom_soft);
        g_bloom_held = false;   /* nothing in the new ones yet */
    }
}

/* One softening pass: `from` laid over what is being drawn into, four times,
 * spread out along one axis. A quarter each, so the four together are the
 * average of what they cover. Offsets are in texels of the picture being drawn
 * into, which on the way across is half the size of the one being read. */
static void bloom_soften(DCTarget* from, float dx, float dy) {
    static const float at[BLOOM_TAPS] = { -1.5f, -0.5f, 0.5f, 1.5f };
    float n = (float)g_bloom_soft;
    for (int i = 0; i < BLOOM_TAPS; i++)
        dc_draw_target_ex(from, &(DCTargetOpts){
            .x = at[i] * dx, .y = at[i] * dy,
            .width = n, .height = n,
            .alpha = 1.0f / BLOOM_TAPS, .add = true });
}

/* Queue the two extra pictures and the add, off the draws already queued.
 * Called from dc_draw_flush() before anything is rendered, so the targets it
 * adds are picked up by the same loop that renders the caller's own. */
static void bloom_queue(void) {
    if (!g_bloom_on || !g_bloom_glow || !g_bloom_glow->txr[0] ||
        !g_bloom_x || !g_bloom_x->txr[0] || !g_bloom_y || !g_bloom_y->txr[0]) return;

    /* Nothing in the frame gives off light: the whole pass is one walk of the
     * queue and then nothing, so bloom left on over a scene without a lamp in
     * it costs next to nothing */
    int said = queue_count;
    bool any = false;
    for (int i = 0; i < said; i++)
        any |= queue[i].kind == ENTRY_MODEL && !queue[i].target &&
               queue[i].model->glow_count != 0;
    if (!any) { g_bloom_held = false; return; }

    /* A new glow every other frame. What it costs is the second walk over the
     * geometry and three renders, and between them those are the whole of the
     * effect; what it buys is sharpness the picture does not have. The glow is
     * a blob a few texels across by the time it is softened, so holding it for
     * one frame only shows on a whip pan, and the frame it goes over is still
     * this frame's. */
    if (g_bloom_held) {
        g_bloom_held = false;
        dc_set_target(NULL);
        dc_draw_target_ex(g_bloom_y, &(DCTargetOpts){
            .alpha = g_bloom.strength, .add = true });
        return;
    }
    g_bloom_held = true;

    /* Everything headed for the screen, not only what glows: a model with no
     * lamp in it is still drawn, in black, so a wall keeps the lamp behind it
     * out of the picture. That is the second walk over the geometry, and the
     * cost of the effect. */
    for (int i = 0; i < said; i++) {
        const DrawEntry* src = &queue[i];
        if (src->kind != ENTRY_MODEL || src->target) continue;
        DrawEntry* e = queue_push();
        if (!e) break;
        *e = *src;
        e->target = g_bloom_glow;
        e->glow = true;
        e->has_shadow = false;   /* a shadow gives off nothing */
    }

    /* Every picture starts black: the first so that only what glows is in it,
     * the other two because the taps are added to what is already there */
    dc_set_target(g_bloom_glow);
    dc_draw_call(PVR_LIST_OP_POLY, bloom_black_draw, NULL);

    /* Across, then down. The sideways step is cut by the shape of the screen,
     * since a square picture holds a 4:3 view: a texel is wider than it is
     * tall once it is stretched back, and an even spread has to allow for it. */
    float s = g_bloom.spread;
    dc_set_target(g_bloom_x);
    dc_draw_call(PVR_LIST_OP_POLY, bloom_black_draw, NULL);
    bloom_soften(g_bloom_glow, s * (SCR_H / SCR_W), 0.0f);

    dc_set_target(g_bloom_y);
    dc_draw_call(PVR_LIST_OP_POLY, bloom_black_draw, NULL);
    bloom_soften(g_bloom_x, 0.0f, s);

    /* And over the frame */
    dc_set_target(NULL);
    dc_draw_target_ex(g_bloom_y, &(DCTargetOpts){
        .alpha = g_bloom.strength, .add = true });
}

/* Does the model have anything for this list? Saves a walk over its blocks. */
static bool model_uses_list(const DMSModel* model, int pvr_list) {
    if (pvr_list == PVR_LIST_OP_POLY) return model->opaque_count != 0;
    if (pvr_list == PVR_LIST_PT_POLY) return model->cutout_count != 0;
    /* The line below is a fallthrough that only ever saw TR_POLY when there
     * were three lists; with five, anything transparent or metallic would claim
     * the modifier lists too. Volumes are asked for separately by the caller. */
    if (pvr_list == PVR_LIST_OP_MOD || pvr_list == PVR_LIST_TR_MOD) return false;
    /* Reflections of solid metallic meshes and cel shading are drawn in the
     * TR list */
    return model->transparent_count != 0 || model->metallic_count != 0 ||
           (dc_model_cel_on() && model->opaque_count != 0 && !model->skeleton);
}

/* Everything queued for one target (NULL: the screen), a list at a time */
static void flush_scene(const DCTarget* target) {
    /* Modifier lists come right after the polygons they change */
    static const int lists[5] = { PVR_LIST_OP_POLY, PVR_LIST_OP_MOD,
                                  PVR_LIST_TR_POLY, PVR_LIST_TR_MOD,
                                  PVR_LIST_PT_POLY };

    /* A camera is built for the screen. Into a target its picture is squeezed
     * to the target's size: screen x and y are rows 1 and 2 of the matrix. */
    alignas(32) DCCamera squeezed;
    const DCCamera* squeezed_from = NULL;
    float sx = target ? (float)target->width / SCR_W : 1.0f;
    float sy = target ? (float)target->height / SCR_H : 1.0f;

    for (int l = 0; l < 5; l++) {
        for (int i = 0; i < queue_count; i++) {
            const DrawEntry* e = &queue[i];
            if (e->target != target) continue;
            if (e->kind == ENTRY_CALL) {
                if (e->call_list != lists[l]) continue;
                dc_list_begin(lists[l]);
                e->call_fn(e->call_user);
            } else if (e->add ? lists[l] == PVR_LIST_TR_POLY
                              : model_uses_list(e->model, lists[l]) ||
                                (e->has_shadow && lists[l] == PVR_LIST_TR_POLY) ||
                                (e->model->vol_on && lists[l] == VOL_POLY_LIST) ||
                                (e->model->vol_shape && lists[l] == VOL_MOD_LIST)) {
                const DCCamera* cam = e->cam;
                if (target) {
                    if (squeezed_from != cam) {
                        squeezed = *cam;
                        for (int c = 0; c < 4; c++) {
                            squeezed._pv_matrix.elem2D[c][1] *= sx;
                            squeezed._pv_matrix.elem2D[c][2] *= sy;
                        }
                        squeezed_from = cam;
                    }
                    cam = &squeezed;
                }
                /* Shape into the modifier list, then the mesh it works on;
                 * the normal draw below leaves that mesh out. The glow pass
                 * wants none of it: a volume and a shadow both take light
                 * away rather than give it off. */
                if (e->model->vol_shape && lists[l] == VOL_MOD_LIST) {
                    if (!e->glow)
                        dc_model_draw_volume(e->model, e->pos, e->scale, e->yaw, cam);
                    continue;
                }
                if (e->model->vol_on && lists[l] == VOL_POLY_LIST && !e->glow)
                    dc_model_draw_modified(e->model, e->pos, e->scale, e->yaw, cam);
                if (e->has_shadow && lists[l] == PVR_LIST_TR_POLY)
                    dc_model_draw_shadow(e->model, e->pos, e->scale, e->yaw,
                                         e->has_rot ? e->rot : NULL, cam, e->shadow.light,
                                         e->shadow.sun, e->shadow.floor_y, e->shadow.dark);
                if (e->add) dc_model_set_add(true);
                if (e->glow) dc_model_set_glow_only(true);
                if (e->has_rot)
                    dc_model_draw_list_oriented(e->model, e->pos, e->scale, e->rot,
                                                cam, lists[l]);
                else
                    dc_model_draw_list_rotated(e->model, e->pos, e->scale, e->yaw,
                                               cam, lists[l]);
                if (e->glow) dc_model_set_glow_only(false);
                if (e->add) dc_model_set_add(false);
            }
        }
    }
}

void dc_draw_flush(void) {
    /* Adds its own targets to the queue, so it goes before the loop below */
    bloom_queue();

    /* Targets first, in the order they were first used, then the screen */
    DCTarget* done[TARGETS_A_FRAME];
    int done_count = 0;
    for (int i = 0; i < queue_count; i++) {
        DCTarget* t = queue[i].target;
        if (!t) continue;
        int seen = 0;
        for (int d = 0; d < done_count; d++) seen |= done[d] == t;
        if (seen || done_count >= TARGETS_A_FRAME) continue;
        done[done_count++] = t;

        if (!t->txr[0]) continue;
        if (!dc_scene_begin_texture(t->txr[t->front ^ 1], t->width, t->height)) continue;
        flush_scene(t);
        dc_scene_end_texture();
        target_flip(t);
    }

    flush_scene(NULL);
    queue_count = 0;
    target_quad_count = 0;
    current_target = NULL;
}
