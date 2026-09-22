#include <kos.h>
#include <sh4zam/shz_sh4zam.h>
#include <stdlib.h>
#include <math.h>
#include "dms/dc_engine.h"
#include "dms/dc_input.h"
#include "dms/dc_camera.h"
#include "dms/dc_model.h"
#include "dms/dc_draw.h"
#include "dms/dc_draw2d.h"
#include "dms/dc_particles.h"
#include "dms/dc_debug.h"

/* Where assets are read from: /pc/ over dcload, /cd/ when built with make disc */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

#define LOOK_SPEED  1.5f
#define ZOOM_SPEED  80.0f
#define FLAMES      4

/* The flame level, LH in the original: 5 is a pilot light, 60 is full. It
 * drifts between the two on its own and the d-pad pushes it either way. */
#define LH_LOW      5.0f
#define LH_HIGH     60.0f
#define LH_DRIFT    3.0f    /* a second, when left alone */
#define LH_PUSH     30.0f   /* a second, on the d-pad */

/* After Fire from Sega's Katana SDK (`Kamui2/k2Gasfl`): four gas flames on a
 * burner, lighting a head and torso.
 *
 * The flames are not in the model and never were. The original holds eight
 * points in a mesh it never draws, four at the bottom of the flames and four
 * at the top, and builds the flames between them every frame. Those eight
 * points came through the converter, so they are read out with
 * dc_model_points() and a puff of particles is put on each one.
 *
 * Up and down on the d-pad turn the gas up and down, as in the original. The
 * one number sets how big the flames burn and how brightly they light the
 * model, so the two cannot drift apart.
 *
 * X swaps how the light works. A light in a place, sitting in the burner, is
 * the obvious way. The original could not afford one and used a sun instead:
 * a single direction for the whole model, swung about on sin() to fake the
 * flame moving.
 */

static DCCamera  camera;
static DMSModel* scene = NULL;
static float     distance = 700.0f;

static bool check_exit(void) {
    const DCInput* inp = dc_input_get(0);
    if (!inp) return false;
    return (inp->buttons & CONT_START) != 0;
}

int main(int argc, char* argv[]) {
    dc_init((DCInitParams){ .vram_size = 1024 * 1024 });
    dc_draw2d_init();
    dc_debug_init();

    dc_camera_init(&camera);
    camera.yaw = 0.5f;
    camera.pitch = 0.1f;

    scene = dc_model_load(ASSETS "fire/fire.dms");
    dc_model_materials(scene);

    /* The burner's eight markers: a bottom and a top for each flame */
    shz_vec3_t mark[FLAMES * 2];
    int marks = dc_model_points(scene, "MFIRE", mark, FLAMES * 2);
    printf("FIRE: %d flame markers\n", marks);

    DCParticles* flame[FLAMES] = { NULL };
    shz_vec3_t   middle = shz_vec3_init(0.0f, 0.0f, 0.0f);
    int          flames = 0;

    for (int i = 0; i + 1 < marks && flames < FLAMES; i += 2) {
        shz_vec3_t a = mark[i], b = mark[i + 1];
        if (b.y < a.y) { shz_vec3_t t = a; a = b; b = t; }
        float gap = b.y - a.y;

        /* Set up for full gas, which the scale each frame then turns down.
         * The original's flame is LH/5 marker gaps tall, so 12 at LH 60. */
        float tall = gap * 12.0f;
        flame[flames] = dc_particles_create(96, &(DCParticleOpts){
            .pos = a,
            .spread       = shz_vec3_init(gap * 0.3f, 0.0f, gap * 0.3f),
            .speed        = shz_vec3_init(0.0f, tall * 2.0f, 0.0f),
            .speed_spread = shz_vec3_init(tall * 0.1f, tall * 0.4f, tall * 0.1f),
            .life = 0.5f, .life_spread = 0.12f,
            .size = gap * 1.3f, .size_spread = gap * 0.4f,
            .grow = 0.4f,
            .start = 0xFFE8A0, .middle = 0xFF7018, .end = 0x180000,
            .rate = 130.0f,
        });
        middle.x += a.x; middle.y += a.y + tall * 0.25f; middle.z += a.z;
        flames++;
    }
    if (flames) { middle.x /= flames; middle.y /= flames; middle.z /= flames; }

    printf("FIRE: d-pad up/down turns the gas, triggers zoom, stick turns, "
           "X swaps the light, B light off, Y flames off\n");
    printf("VRAM free after loading: %luKB\n", (unsigned long)(pvr_mem_available() / 1024));

    shz_vec3_t origin = shz_vec3_init(0.0f, 0.0f, 0.0f);
    float t = 0.0f, lh = 20.0f, drift = LH_DRIFT;
    bool  sun = false, no_light = false, no_flame = false, held = false;

    while (!check_exit()) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        if (inp) {
            if (inp->ltrig > 0) distance -= ZOOM_SPEED * dt;
            if (inp->rtrig > 0) distance += ZOOM_SPEED * dt;
            if (distance < 200.0f)  distance = 200.0f;
            if (distance > 1400.0f) distance = 1400.0f;

            /* The gas tap. Left alone it drifts up and down by itself. */
            if (inp->buttons & CONT_DPAD_UP)        lh += LH_PUSH * dt;
            else if (inp->buttons & CONT_DPAD_DOWN) lh -= LH_PUSH * dt;
            else                                    lh += drift * dt;

            bool press = (inp->buttons & (CONT_X | CONT_B | CONT_Y)) != 0;
            if (press && !held) {
                if (inp->buttons & CONT_X)      sun = !sun;
                else if (inp->buttons & CONT_B) no_light = !no_light;
                else                            no_flame = !no_flame;
            }
            held = press;
        }
        dc_camera_orbit(&camera, origin, distance, inp, LOOK_SPEED, dt);
        dc_camera_update(&camera);

        if (lh > LH_HIGH) { lh = LH_HIGH; drift = -LH_DRIFT; }
        if (lh < LH_LOW)  { lh = LH_LOW;  drift =  LH_DRIFT; }

        /* How much gas is on, 0 to 1. It sets how big the flames burn and how
         * brightly they light the model, which is the point of the demo: one
         * number, and the two stay together. The wobble on top is the flame
         * not burning perfectly steadily. */
        t += dt;
        float gas = lh / LH_HIGH;
        float burn = gas * (0.88f + 0.12f * sinf(t * 9.0f) * sinf(t * 5.3f));

        DCLight light = { .ambient = 0.15f, .range = 1200.0f, .pos = middle,
                          .r = burn, .g = burn * 0.78f, .b = burn * 0.45f };
        if (sun) {
            /* KMGASFL.C: no place, just a direction that is swung about */
            light.sun = true;
            light.pos = shz_vec3_init(-0.35f, -0.55f, 0.75f + 0.35f * sinf(t * 1.5f));
        }
        dc_set_light(no_light ? NULL : &light);

        /* KMGASFL.C:1231 -- how tall they stand rides on the gas */
        for (int i = 0; i < flames; i++) {
            dc_particles_scale(flame[i], burn);
            dc_particles_pause(flame[i], no_flame);
        }

        dc_set_camera(&camera);
        dc_draw(scene, origin);
        for (int i = 0; i < flames; i++) dc_particles_draw(flame[i]);

        dc_debug_stats();
        dc_frame_end();
    }

    for (int i = 0; i < flames; i++) dc_particles_free(flame[i]);
    dc_model_free(scene);
    dc_shutdown();
    return 0;
}
