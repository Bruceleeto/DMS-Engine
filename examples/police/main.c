#include <kos.h>
#include <sh4zam/shz_sh4zam.h>
#include <stdio.h>
#include <math.h>
#include "dms/dc_engine.h"
#include "dms/dc_input.h"
#include "dms/dc_camera.h"
#include "dms/dc_model.h"
#include "dms/dc_draw.h"
#include "dms/dc_draw2d.h"
#include "dms/dc_debug.h"
#include "dms/dc_corona.h"

/* Where assets are read from: /pc/ over dcload, /cd/ when built with make disc */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

/* A police car pulled straight off Sketchfab, shown on its own: the camera
 * circles it under a sun. */

/* Which car: 0 the police car, 1 the Toyota (plain paint with no texture, so
 * it shows the environment as its texture in the one pass). The Toyota has
 * no siren. */
#define TOYOTA 0

#if TOYOTA
#define CAR ASSETS "Toyota/Toyota.dms"
#else
#define CAR ASSETS "police/police2.dms"
#endif

#define LOOK_SPEED 1.5f
#define ZOOM_SPEED 4.0f     /* car lengths a second */

/* The siren, as GTA 3 (CAutomobile::ProcessControl): 4 lights from one end of
 * the lightbar to the other, each 64ms behind the last, going red, off, blue,
 * off every 256ms. The second one flares (GTA: only when red). */
#define SIREN_LIGHTS 4

static void siren(const DMSEntity* a, const DMSEntity* b, float size) {
    uint32_t t = (uint32_t)dc_time_ms();
    for (int i = 0; i < SIREN_LIGHTS; i++) {
        float f = (float)i / (SIREN_LIGHTS - 1);
        shz_vec3_t pos = shz_vec3_init(a->pos.x + (b->pos.x - a->pos.x) * f,
                                       a->pos.y + (b->pos.y - a->pos.y) * f,
                                       a->pos.z + (b->pos.z - a->pos.z) * f);
        switch (((t + (i << 6)) >> 8) & 3) {
        case 0: dc_corona(i, pos, 0xFF0000, size, i == 1 ? DC_CORONA_FLARE : 0); break;
        case 2: dc_corona(i, pos, 0x0000FF, size, i == 1 ? DC_CORONA_FLARE : 0); break;
        default: break;     /* off: it fades */
        }
    }
}

/* The light the siren throws, as GTA: red for half a second, blue for half,
 * each coming up over 100ms. 0 to 1 in each of r, g, b. */
static void siren_colour(float* r, float* g, float* b) {
    uint32_t t = (uint32_t)dc_time_ms();
    bool red = (t & 1023) < 512;
    float up = (t & 511) < 100 ? (t & 511) / 100.0f : 1.0f;
    *r = red ? up : 0.0f;
    *g = 0.0f;
    *b = red ? 0.0f : up;
}

/* GTA's headlights (CAutomobile::PreRender): seen from in front they grow
 * and brighten the more head-on you are, with a bigger glow over them when
 * right in front. behind: 1 behind the car, -1 in front. */
static void headlights(const DMSEntity* a, const DMSEntity* b, float behind, float length) {
    if (behind >= 0.0f) return;                 /* from behind: they fade */
    float bright = -0.5f * behind + 0.3f;
    uint32_t c = (uint32_t)(210 * bright);
    uint32_t rgb = (c << 16) | (c << 8) | (uint32_t)(195 * bright);
    float size = (1.0f - behind) * length * 0.1f;
    dc_corona(10, a->pos, rgb, size, 0);
    dc_corona(11, b->pos, rgb, size, 0);
    if (behind < -0.97f) {
        dc_corona(12, a->pos, 0xA0A08C, length * 0.25f, 0);
        dc_corona(13, b->pos, 0xA0A08C, length * 0.25f, 0);
    }
}

static bool check_exit(void) {
    const DCInput* inp = dc_input_get(0);
    return inp && (inp->buttons & CONT_START);
}

int main(int argc, char* argv[]) {
    dc_init((DCInitParams){ .vram_size = 2240 * 1024 });
    dc_draw2d_init();
    dc_debug_init();
    dc_set_clear_color(0xFF303848);

    DMSModel* car = dc_model_load(CAR);
    if (car) dc_model_bench_sun(car);
    if (!car) { dc_shutdown(); return 1; }
    DMSModel* floor = dc_model_load(ASSETS "floor/floor.dms");
    DCImage* glow = dc_image_load(ASSETS "siren/siren.dt");    /* two beams, black round */
    float floor_y = floor ? dc_model_bounds(floor, NULL).max.y : 0.0f;

    /* The car sits wherever it was in its Sketchfab scene: look at its middle */
    DCBounds b = dc_model_bounds(car, NULL);
    shz_vec3_t look = shz_vec3_init((b.min.x + b.max.x) * 0.5f, (b.min.y + b.max.y) * 0.5f,
                                    (b.min.z + b.max.z) * 0.5f);
    shz_vec3_t size = shz_vec3_sub(b.max, b.min);
    float length = size.x > size.z ? size.x : size.z;

    DCCamera camera;
    dc_camera_init(&camera);
    camera.yaw = 0.8f;
    camera.pitch = 0.3f;

    float distance = length * 1.3f;

    /* Not baked, so a sun gives it shape and the paint its highlight. The
     * siren does not light the car: a point light is worked out on every
     * vertex, which cost more than the rest of the frame; its colour goes on
     * the ground only */
    DCLight sun = { .pos = shz_vec3_init(-0.4f, -1.0f, -0.3f), .sun = true, .ambient = 0.35f };

    /* Two Empties at the ends of the lightbar, added in Blender */
    const DMSEntity* siren_l = TOYOTA ? NULL : dc_model_entity(car, "red_siren");
    const DMSEntity* siren_r = TOYOTA ? NULL : dc_model_entity(car, "blue_siren");
    if (!TOYOTA && (!siren_l || !siren_r))
        printf("POLICE: no red_siren / blue_siren Empties in the model, no siren\n");
    else if (siren_l && siren_r)
        printf("POLICE: siren from (%.2f %.2f %.2f) to (%.2f %.2f %.2f)\n", siren_l->pos.x,
               siren_l->pos.y, siren_l->pos.z, siren_r->pos.x, siren_r->pos.y, siren_r->pos.z);
    float siren_size = length * 0.16f;     /* GTA's 0.8m on a 5m car */

    const DMSEntity* head_a = dc_model_entity(car, "headlight1");
    const DMSEntity* head_b = dc_model_entity(car, "headlight2");
    if (!head_a || !head_b)
        printf("POLICE: no headlight1 / headlight2 Empties in the model, no headlights\n");
    DCImage* beam = dc_image_load(ASSETS "headlight/headlight.dt");    /* two beams going up */

    /* Which way the car faces: from its middle to between its headlights */
    shz_vec3_t fwd = shz_vec3_init(1.0f, 0.0f, 0.0f), head_mid = look;
    if (head_a && head_b) {
        head_mid = shz_vec3_init((head_a->pos.x + head_b->pos.x) * 0.5f,
                                 (head_a->pos.y + head_b->pos.y) * 0.5f,
                                 (head_a->pos.z + head_b->pos.z) * 0.5f);
        shz_vec3_t f = shz_vec3_init(head_mid.x - look.x, 0.0f, head_mid.z - look.z);
        if (shz_vec3_dot(f, f) > 1e-6f) fwd = shz_vec3_normalize(f);
    }

    printf("POLICE: stick turns, triggers zoom, Start exits\n");

    while (!check_exit()) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        if (inp) {
            if (inp->ltrig > 0) distance -= ZOOM_SPEED * length * dt;
            if (inp->rtrig > 0) distance += ZOOM_SPEED * length * dt;
            if (distance < length * 0.6f) distance = length * 0.6f;
            if (distance > length * 4.0f) distance = length * 4.0f;
        }
        dc_camera_orbit(&camera, look, distance, inp, LOOK_SPEED, dt);
        dc_camera_update(&camera);

        dc_set_camera(&camera);
        dc_set_lights(&sun, 1);

        if (siren_l && siren_r) {
            /* GTA: a beacon's two beams going round on the ground (a 5m car) */
            shz_vec3_t mid = shz_vec3_init((siren_l->pos.x + siren_r->pos.x) * 0.5f,
                                           (siren_l->pos.y + siren_r->pos.y) * 0.5f,
                                           (siren_l->pos.z + siren_r->pos.z) * 0.5f);
            float r, g, b;
            siren_colour(&r, &g, &b);
            if (glow && r + b > 0.01f)     /* a tint of 0 would be white */
                dc_draw_decal(glow, &(DCDecalOpts){
                    .pos = shz_vec3_init(mid.x, floor_y + length * 0.004f, mid.z),
                    .size = length * 3.2f,
                    .yaw = (float)((uint32_t)dc_time_ms() & 1023) * (6.2831853f / 1024.0f),
                    .tint = ((uint32_t)(r * 255.0f) << 16) | (uint32_t)(b * 255.0f),
                    .add = true });
        }

        if (head_a && head_b) {
            shz_vec3_t to_car = shz_vec3_sub(look, camera.pos);
            float behind = shz_vec3_dot(shz_vec3_normalize(to_car), fwd);
            headlights(head_a, head_b, behind, length);

            /* GTA's beams on the road, from the lights forward. The picture's
             * bottom edge is on the lights, and its beams start 0.15 of its
             * half width either side of the middle, so it is sized to put
             * them on the two lights */
            if (beam) {
                float dx = head_b->pos.x - head_a->pos.x, dz = head_b->pos.z - head_a->pos.z;
                float half = shz_sqrtf(dx * dx + dz * dz) * 0.5f / 0.15f;
                dc_draw_decal(beam, &(DCDecalOpts){
                    .pos = shz_vec3_init(head_mid.x + fwd.x * half, floor_y + length * 0.004f,
                                         head_mid.z + fwd.z * half),
                    .size = half * 2.0f,
                    .yaw = atan2f(fwd.z, -fwd.x),
                    .tint = 0x606058,
                    .add = true });
            }
        }

        if (floor) dc_draw(floor, shz_vec3_init(0.0f, 0.0f, 0.0f));
        dc_draw(car, shz_vec3_init(0.0f, 0.0f, 0.0f));
        if (siren_l && siren_r) siren(siren_l, siren_r, siren_size);

        dc_debug_stats();
        dc_frame_end();
    }

    dc_set_light(NULL);
    dc_model_free(car);
    if (floor) dc_model_free(floor);
    if (glow) dc_image_free(glow);
    if (beam) dc_image_free(beam);
    dc_shutdown();
    return 0;
}
