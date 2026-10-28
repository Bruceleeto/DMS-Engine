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
#include "dms/dc_scene.h"

/* Where assets are read from: /pc/ over dcload; on a disc the engine finds them at /cd/ */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

/* A police car pulled straight off Sketchfab, shown on its own: the camera
 * circles it under a sun.
 *
 * The car (Car), the floor (Floor), the sun, the background and the camera's
 * start angle are scenes/main.json. Give Car the model Toyota/Toyota.dms for
 * the Toyota (plain paint with no texture, so it shows the environment as its
 * texture in the one pass); it has no siren. */

#define LOOK_SPEED 1.5f
#define ZOOM_SPEED 4.0f     /* car lengths a second */

/* The siren, as GTA 3 (CAutomobile::ProcessControl): 4 lights from one end of
 * the lightbar to the other, each 64ms behind the last, going red, off, blue,
 * off every 256ms. The second one flares (GTA: only when red). */
#define SIREN_LIGHTS 4

static void siren(shz_vec3_t a, shz_vec3_t b, float size) {
    uint32_t t = (uint32_t)dc_time_ms();
    for (int i = 0; i < SIREN_LIGHTS; i++) {
        float f = (float)i / (SIREN_LIGHTS - 1);
        shz_vec3_t pos = shz_vec3_init(a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f,
                                       a.z + (b.z - a.z) * f);
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
static void headlights(shz_vec3_t a, shz_vec3_t b, float behind, float length) {
    if (behind >= 0.0f) return;                 /* from behind: they fade */
    float bright = -0.5f * behind + 0.3f;
    uint32_t c = (uint32_t)(210 * bright);
    uint32_t rgb = (c << 16) | (c << 8) | (uint32_t)(195 * bright);
    float size = (1.0f - behind) * length * 0.1f;
    dc_corona(10, a, rgb, size, 0);
    dc_corona(11, b, rgb, size, 0);
    if (behind < -0.97f) {
        dc_corona(12, a, 0xA0A08C, length * 0.25f, 0);
        dc_corona(13, b, 0xA0A08C, length * 0.25f, 0);
    }
}

/* A point in an object's model where the scene puts it in the world */
static shz_vec3_t place(const DCSceneObject* o, shz_vec3_t p) {
    float r[9];
    dc_scene_object_rot(o, r);
    p = shz_vec3_init(p.x * o->scale.x, p.y * o->scale.y, p.z * o->scale.z);
    return shz_vec3_init(o->pos.x + r[0] * p.x + r[3] * p.y + r[6] * p.z,
                         o->pos.y + r[1] * p.x + r[4] * p.y + r[7] * p.z,
                         o->pos.z + r[2] * p.x + r[5] * p.y + r[8] * p.z);
}

/* An Empty of the object's model, in the world; false if it has none */
static bool empty_at(const DCSceneObject* o, const char* name, shz_vec3_t* out) {
    const DMSEntity* e = dc_model_entity(o->m, name);
    if (e) *out = place(o, e->pos);
    return e != NULL;
}

static bool check_exit(void) {
    const DCInput* inp = dc_input_get(0);
    return inp && (inp->buttons & CONT_START);
}

int main(int argc, char* argv[]) {
    dc_init((DCInitParams){ .vram_size = 2240 * 1024 });
    dc_draw2d_init();
    dc_debug_init();

    /* Not baked, so the scene's sun gives it shape and the paint its
     * highlight. The siren does not light the car: a point light is worked
     * out on every vertex, which cost more than the rest of the frame; its
     * colour goes on the ground only */
    DCScene* scene = dc_scene_play(ASSETS, "scenes/main.json");
    if (!scene) { dc_shutdown(); return 1; }
    dc_scene_apply(scene);
    const DCSceneObject* car = dc_scene_find(scene, "Car");
    if (!car || !car->m) { printf("POLICE: the scene has no Car with a model\n"); dc_shutdown(); return 1; }
    dc_model_bench_sun(car->m);
    const DCSceneObject* floor = dc_scene_find(scene, "Floor");
    DCImage* glow = dc_image_load(ASSETS "siren/siren.dt");    /* two beams, black round */
    float floor_y = floor && floor->m ? place(floor, dc_model_bounds(floor->m, NULL).max).y : 0.0f;

    /* The car sits wherever it was in its Sketchfab scene: look at its middle */
    DCBounds b = dc_model_bounds(car->m, NULL);
    shz_vec3_t look = place(car, shz_vec3_init((b.min.x + b.max.x) * 0.5f, (b.min.y + b.max.y) * 0.5f,
                                               (b.min.z + b.max.z) * 0.5f));
    shz_vec3_t size = shz_vec3_sub(b.max, b.min);
    float length = (size.x > size.z ? size.x : size.z) * car->scale.x;

    DCCamera camera;
    dc_scene_camera(scene, &camera);    /* its angle; it circles the car */

    float distance = length * 1.3f;

    /* Two Empties at the ends of the lightbar, added in Blender */
    shz_vec3_t siren_l, siren_r;
    bool has_siren = empty_at(car, "red_siren", &siren_l) && empty_at(car, "blue_siren", &siren_r);
    if (!has_siren)
        printf("POLICE: no red_siren / blue_siren Empties in the model, no siren\n");
    else
        printf("POLICE: siren from (%.2f %.2f %.2f) to (%.2f %.2f %.2f)\n", siren_l.x,
               siren_l.y, siren_l.z, siren_r.x, siren_r.y, siren_r.z);
    float siren_size = length * 0.16f;     /* GTA's 0.8m on a 5m car */

    shz_vec3_t head_a, head_b;
    bool has_heads = empty_at(car, "headlight1", &head_a) && empty_at(car, "headlight2", &head_b);
    if (!has_heads)
        printf("POLICE: no headlight1 / headlight2 Empties in the model, no headlights\n");
    DCImage* beam = dc_image_load(ASSETS "headlight/headlight.dt");    /* two beams going up */

    /* Which way the car faces: from its middle to between its headlights */
    shz_vec3_t fwd = shz_vec3_init(1.0f, 0.0f, 0.0f), head_mid = look;
    if (has_heads) {
        head_mid = shz_vec3_init((head_a.x + head_b.x) * 0.5f, (head_a.y + head_b.y) * 0.5f,
                                 (head_a.z + head_b.z) * 0.5f);
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

        if (has_siren) {
            /* GTA: a beacon's two beams going round on the ground (a 5m car) */
            shz_vec3_t mid = shz_vec3_init((siren_l.x + siren_r.x) * 0.5f, (siren_l.y + siren_r.y) * 0.5f,
                                           (siren_l.z + siren_r.z) * 0.5f);
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

        if (has_heads) {
            shz_vec3_t to_car = shz_vec3_sub(look, camera.pos);
            float behind = shz_vec3_dot(shz_vec3_normalize(to_car), fwd);
            headlights(head_a, head_b, behind, length);

            /* GTA's beams on the road, from the lights forward. The picture's
             * bottom edge is on the lights, and its beams start 0.15 of its
             * half width either side of the middle, so it is sized to put
             * them on the two lights */
            if (beam) {
                float dx = head_b.x - head_a.x, dz = head_b.z - head_a.z;
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

        dc_scene_draw(scene);
        if (has_siren) siren(siren_l, siren_r, siren_size);

        dc_debug_stats();
        dc_frame_end();
    }

    dc_set_light(NULL);
    dc_scene_free(scene);
    if (glow) dc_image_free(glow);
    if (beam) dc_image_free(beam);
    dc_shutdown();
    return 0;
}
