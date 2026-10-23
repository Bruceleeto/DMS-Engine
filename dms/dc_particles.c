#include "dc_particles.h"
#include "dc_draw.h"
#include "dc_draw2d.h"
#include "dc_engine.h"
#include "dc_model.h"
#include "pvrtex.h"
#include <kos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Particles
 *
 * Each one is a flat square turned to face the camera, four vertices, drawn
 * in the transparent list and added over what is behind it. They are moved on
 * the CPU (a few adds each) and never clipped against each other, so the cost
 * is the fill: a screen full of big particles is the slow case, not the count.
 *
 * The picture is a soft round glow made here, white with its edges fading
 * out, so the colour comes from the vertex colours and one texture does fire,
 * sparks and dust. A .dt picture can be used instead.
 * ================================================================ */

#define GLOW_SIZE   32          /* the made-here glow, across */
#define BATCH_QUADS 16          /* built on the stack, then submitted */

typedef struct {
    float x, y, z;
    float vx, vy, vz;
    float age, life;
    float size, spin_angle;
    float bright;               /* a little darker or lighter than its fellows */
    float pad;
} Particle;

struct DCParticles {
    DCParticleOpts o;
    Particle*      p;
    int            most, live;
    float          carry;       /* part of a particle left over from last frame */
    float          scale;       /* dc_particles_scale() */
    bool           paused;
    uint32_t       seed;
    const DCCamera* cam;        /* the camera it was last drawn with */
    uint32_t       stepped;     /* the frame it last moved in */

    dttex_info_t   tex;
    bool           own_tex;     /* the glow was made here, so free it here */

    /* where they all are, for culling */
    shz_vec3_t     mid;
    float          radius;

    pvr_poly_hdr_t hdr __attribute__((aligned(32)));
};

/* ---- Random ---- */

static inline uint32_t rnd(DCParticles* s) {
    uint32_t x = s->seed;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (s->seed = x);
}

/* -1 to 1 */
static inline float rnd_signed(DCParticles* s) {
    return (float)(int32_t)rnd(s) * (1.0f / 2147483648.0f);
}

/* 0 to 1 */
static inline float rnd_unit(DCParticles* s) {
    return (float)(rnd(s) >> 8) * (1.0f / 16777216.0f);
}

/* ---- The glow ---- */

/* White, fading to nothing at the edge. ARGB4444, so it works both added
 * (the alpha dims it) and mixed in (the alpha is see-through). */
static bool glow_make(DCParticles* s) {
    const int n = GLOW_SIZE;
    uint16_t* pix = (uint16_t*)malloc(n * n * 2);
    if (!pix) return false;

    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            float dx = ((float)x + 0.5f) * (2.0f / n) - 1.0f;
            float dy = ((float)y + 0.5f) * (2.0f / n) - 1.0f;
            float d = shz_sqrtf_fsrra(dx * dx + dy * dy);
            float a = 1.0f - d;
            if (a < 0.0f) a = 0.0f;
            a = a * a * (3.0f - 2.0f * a);      /* soft shoulders */
            uint32_t av = (uint32_t)(a * 15.0f + 0.5f);
            pix[y * n + x] = (uint16_t)((av << 12) | 0x0FFF);
        }
    }

    s->tex.ptr = pvr_mem_malloc(n * n * 2);
    if (!s->tex.ptr) {
        free(pix);
        printf("dc_particles: no VRAM for the glow\n");
        return false;
    }
    pvr_txr_load(pix, s->tex.ptr, n * n * 2);
    free(pix);

    s->tex.width = n;
    s->tex.height = n;
    s->tex.pvrformat = PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED;
    s->own_tex = true;
    return true;
}

/* ---- Making and freeing ---- */

DCParticles* dc_particles_create(int most, const DCParticleOpts* opts) {
    if (most <= 0 || !opts) return NULL;

    DCParticles* s = (DCParticles*)calloc(1, sizeof(DCParticles));
    if (!s) return NULL;
    s->p = (Particle*)memalign(32, (size_t)most * sizeof(Particle));
    if (!s->p) { free(s); return NULL; }

    s->o = *opts;
    s->most = most;
    s->seed = 0x2545F491u;

    /* Fields left out mean "as it is" */
    if (s->o.life <= 0.0f)   s->o.life = 2.0f;
    if (s->o.size <= 0.0f)   s->o.size = 1.0f;
    s->scale = 1.0f;
    if (s->o.grow <= 0.0f)   s->o.grow = 1.0f;
    if (s->o.start == 0)     s->o.start = 0xFFFFFF;
    if (s->o.middle == 0)    s->o.middle = s->o.start;
    if (s->o.bounce_keep <= 0.0f) s->o.bounce_keep = 0.4f;
    if (s->o.drag < 0.0f)    s->o.drag = 0.0f;
    if (s->o.drag > 1.0f)    s->o.drag = 1.0f;

    if (s->o.texture) {
        if (!pvrtex_load(s->o.texture, &s->tex)) {
            printf("dc_particles: cannot load %s, using the glow\n", s->o.texture);
            if (!glow_make(s)) { free(s->p); free(s); return NULL; }
        }
    } else if (!glow_make(s)) {
        free(s->p); free(s); return NULL;
    }

    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, s->tex.pvrformat,
                     s->tex.width, s->tex.height, s->tex.ptr, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
    cxt.blend.src = PVR_BLEND_SRCALPHA;
    cxt.blend.dst = s->o.smoke ? PVR_BLEND_INVSRCALPHA : PVR_BLEND_ONE;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;   /* they never hide each other */
    pvr_poly_compile(&s->hdr, &cxt);

    return s;
}

void dc_particles_free(DCParticles* s) {
    if (!s) return;
    if (s->own_tex) {
        if (s->tex.ptr) pvr_mem_free(s->tex.ptr);
    } else {
        pvrtex_unload(&s->tex);
    }
    free(s->p);
    free(s);
}

void dc_particles_move(DCParticles* s, shz_vec3_t pos) {
    if (s) s->o.pos = pos;
}

void dc_particles_pause(DCParticles* s, bool paused) {
    if (s) s->paused = paused;
}

void dc_particles_scale(DCParticles* p, float scale) {
    if (p) p->scale = scale > 0.0f ? scale : 0.0f;
}

int dc_particles_count(const DCParticles* s) {
    return s ? s->live : 0;
}

/* ---- Being born ---- */

static void spawn(DCParticles* s, int how_many) {
    for (int i = 0; i < how_many && s->live < s->most; i++) {
        Particle* p = &s->p[s->live++];
        /* Everything measured in world units is scaled together, so the
         * whole puff grows and shrinks keeping its shape */
        float k = s->scale;
        p->x = s->o.pos.x + rnd_signed(s) * s->o.spread.x * k;
        p->y = s->o.pos.y + rnd_signed(s) * s->o.spread.y * k;
        p->z = s->o.pos.z + rnd_signed(s) * s->o.spread.z * k;
        p->vx = (s->o.speed.x + rnd_signed(s) * s->o.speed_spread.x) * k;
        p->vy = (s->o.speed.y + rnd_signed(s) * s->o.speed_spread.y) * k;
        p->vz = (s->o.speed.z + rnd_signed(s) * s->o.speed_spread.z) * k;
        p->age = 0.0f;
        p->life = s->o.life + rnd_signed(s) * s->o.life_spread;
        if (p->life < 0.05f) p->life = 0.05f;
        p->size = (s->o.size + rnd_signed(s) * s->o.size_spread) * k;
        if (p->size < 0.0f) p->size = 0.0f;
        p->spin_angle = s->o.spin != 0.0f ? rnd_unit(s) * F_PI * 2.0f : 0.0f;
        p->bright = 0.7f + rnd_unit(s) * 0.3f;
    }
}

void dc_particles_burst(DCParticles* s, int how_many) {
    if (s) spawn(s, how_many);
}

/* ---- Moving ---- */

static void step(DCParticles* s, float dt) {
    if (dt <= 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;               /* a long frame does not fling them */

    float keep = s->o.drag > 0.0f ? 1.0f - s->o.drag * dt : 1.0f;
    if (keep < 0.0f) keep = 0.0f;
    float fall = s->o.gravity * dt;

    float lo[3] = {  1e30f,  1e30f,  1e30f };
    float hi[3] = { -1e30f, -1e30f, -1e30f };
    float biggest = 0.0f;

    for (int i = 0; i < s->live; ) {
        Particle* p = &s->p[i];
        p->age += dt;
        if (p->age >= p->life) {            /* dead: the last one takes its place */
            *p = s->p[--s->live];
            continue;
        }

        p->vy -= fall;
        if (keep != 1.0f) { p->vx *= keep; p->vy *= keep; p->vz *= keep; }
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->z += p->vz * dt;
        if (s->o.spin != 0.0f) p->spin_angle += s->o.spin * dt;

        if (s->o.bounce && p->y < s->o.floor_y && p->vy < 0.0f) {
            p->y = s->o.floor_y + (s->o.floor_y - p->y) * s->o.bounce_keep;
            p->vy = -p->vy * s->o.bounce_keep;
            p->vx *= s->o.bounce_keep;
            p->vz *= s->o.bounce_keep;
        }

        if (p->x < lo[0]) lo[0] = p->x;
        if (p->y < lo[1]) lo[1] = p->y;
        if (p->z < lo[2]) lo[2] = p->z;
        if (p->x > hi[0]) hi[0] = p->x;
        if (p->y > hi[1]) hi[1] = p->y;
        if (p->z > hi[2]) hi[2] = p->z;
        if (p->size > biggest) biggest = p->size;
        i++;
    }

    if (s->live) {
        s->mid = shz_vec3_init((lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f,
                               (lo[2] + hi[2]) * 0.5f);
        float dx = hi[0] - lo[0], dy = hi[1] - lo[1], dz = hi[2] - lo[2];
        s->radius = 0.5f * shz_sqrtf_fsrra(dx * dx + dy * dy + dz * dz) + biggest;
    } else {
        s->mid = s->o.pos;
        s->radius = 0.0f;
    }

    if (!s->paused && s->o.rate > 0.0f) {
        s->carry += s->o.rate * dt;
        int n = (int)s->carry;
        if (n > 0) { s->carry -= (float)n; spawn(s, n); }
    }
}

/* ---- Drawing ---- */

/* start, middle, end over its life, and it fades out at the end */
static inline uint32_t particle_colour(const DCParticles* s, const Particle* p) {
    float t = p->age / p->life;
    uint32_t a_col, b_col;
    float mu;
    if (t < 0.5f) { a_col = s->o.start;  b_col = s->o.middle; mu = t * 2.0f; }
    else          { a_col = s->o.middle; b_col = s->o.end;    mu = t * 2.0f - 1.0f; }

    float inv = 1.0f - mu;
    float r = ((a_col >> 16) & 0xFF) * inv + ((b_col >> 16) & 0xFF) * mu;
    float g = ((a_col >>  8) & 0xFF) * inv + ((b_col >>  8) & 0xFF) * mu;
    float b = ((a_col      ) & 0xFF) * inv + ((b_col      ) & 0xFF) * mu;
    r *= p->bright; g *= p->bright; b *= p->bright;

    float fade = t > 0.8f ? (1.0f - t) * 5.0f : 1.0f;
    uint32_t av = (uint32_t)(fade * 255.0f);
    return (av << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void particles_draw_cb(void* user) {
    DCParticles* s = (DCParticles*)user;
    const DCCamera* cam = s->cam;
    if (!cam || !s->live) return;

    pvr_dr_state_t* dr = dc_dr_state();
    dc_send_hdr(dr, &s->hdr);

    /* World coordinates, camera at the origin: the same matrix the flat
     * shadows use */
    shz_xmtrx_load_4x4((shz_mat4x4_t*)dc_camera_get_pv(cam));
    shz_xmtrx_translate(-cam->pos.x, -cam->pos.y, cam->pos.z);

    /* The square faces the camera: across the screen, and up it */
    shz_sincos_t y = shz_sincosf(cam->yaw), pi = shz_sincosf(cam->pitch);
    const float rx = -y.cos,            ry = 0.0f,   rz = y.sin;
    const float ux = y.sin * pi.sin,    uy = pi.cos, uz = y.cos * pi.sin;

    static const float corner[4][2] = { {-1, -1}, {1, -1}, {-1, 1}, {1, 1} };
    static const float uv[4][2]     = { { 0,  1}, {1,  1}, { 0, 0}, {1, 0} };

    alignas(32) DMSVertex batch[BATCH_QUADS * 4];
    int in_batch = 0;

    for (int i = 0; i < s->live; i++) {
        const Particle* p = &s->p[i];
        uint32_t argb = particle_colour(s, p);
        float grow = 1.0f + (s->o.grow - 1.0f) * (p->age / p->life);
        float half = p->size * 0.5f * grow;

        /* Turned on the screen, if it spins */
        float ax = rx, ay = ry, az = rz, bx = ux, by = uy, bz = uz;
        if (s->o.spin != 0.0f) {
            shz_sincos_t sp = shz_sincosf(p->spin_angle);
            ax = rx * sp.cos + ux * sp.sin;
            ay = ry * sp.cos + uy * sp.sin;
            az = rz * sp.cos + uz * sp.sin;
            bx = ux * sp.cos - rx * sp.sin;
            by = uy * sp.cos - ry * sp.sin;
            bz = uz * sp.cos - rz * sp.sin;
        }
        ax *= half; ay *= half; az *= half;
        bx *= half; by *= half; bz *= half;

        DMSVertex* v = &batch[in_batch * 4];
        for (int c = 0; c < 4; c++) {
            float cu = corner[c][0], cv = corner[c][1];
            v[c].x = p->x + ax * cu + bx * cv;
            v[c].y = p->y + ay * cu + by * cv;
            v[c].z = p->z + az * cu + bz * cv;
            v[c].u = uv[c][0];
            v[c].v = uv[c][1];
            v[c].argb = argb;
            v[c].flags = (c == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        }

        if (++in_batch == BATCH_QUADS) {
            dc_model_submit_quads(batch, in_batch, dr);
            in_batch = 0;
        }
    }
    if (in_batch) dc_model_submit_quads(batch, in_batch, dr);
}

void dc_particles_draw(DCParticles* s) {
    if (!s) return;

    /* Drawn into a target as well as the screen, they still only move once */
    uint32_t frame = dc_frame_count();
    if (s->stepped != frame) {
        s->stepped = frame;
        step(s, dc_delta_time());
    }
    if (!s->live) return;

    const DCCamera* cam = dc_get_camera();
    if (!cam) return;
    if (dc_frustum_cull_sphere(cam, s->mid, s->radius) < 0) return;
    s->cam = cam;

    dc_draw_call(PVR_LIST_TR_POLY, particles_draw_cb, s);
}
