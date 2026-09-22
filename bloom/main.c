#include <kos.h>
#include <sh4zam/shz_sh4zam.h>
#include <stdio.h>
#include "dms/dc_engine.h"
#include "dms/dc_input.h"
#include "dms/dc_camera.h"
#include "dms/dc_model.h"
#include "dms/dc_draw.h"
#include "dms/dc_draw2d.h"
#include "dms/dc_debug.h"

/* Where assets are read from: /pc/ over dcload, /cd/ when built with make disc */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

/* A dark room with bright things in it, to show bloom.
 *
 * Nothing in this file says which things glow. The neon strips, the lamps on
 * the pillars and the core in the middle have an Emission colour on their
 * material in Blender, and that is the whole of it: the converter marks those
 * meshes, and dc_set_bloom() draws them a second time into a small picture,
 * softens it and adds it over the frame.
 *
 * The room itself is lit by the same materials, baked (`BAKE = 1` in the
 * Makefile), so what glows and what lights the walls are the one thing said
 * once.
 */

#define LOOK_SPEED  1.5f
#define ZOOM_SPEED  400.0f
#define DRIFT       0.12f   /* radians a second the camera turns by itself */

/* The middle of the level, which the camera turns around */
#define LOOK_X  -19.5f
#define LOOK_Y  -44.3f
#define LOOK_Z -105.8f

static DCCamera  camera;
static DMSModel* scene;
static float     distance;

static bool check_exit(void) {
    const DCInput* inp = dc_input_get(0);
    return inp && (inp->buttons & CONT_START);
}

int main(int argc, char* argv[]) {
    dc_init((DCInitParams){ .vram_size = 1024 * 1024 });
    dc_draw2d_init();
    dc_debug_init();
    dc_set_clear_color(0xFF05060A);

    scene = dc_model_load(ASSETS "bloom/arena.dms");

    /* The camera starts where the Empty in arena.glb is. dc_camera_orbit puts
     * the camera behind its target along the way it looks, so the turn and
     * the distance that land it there are worked back out of the Empty. */
    dc_camera_init(&camera);
    shz_vec3_t look = shz_vec3_init(LOOK_X, LOOK_Y, LOOK_Z);
    const DMSEntity* start = dc_model_entity(scene, "Empty");
    shz_vec3_t cam = start ? start->pos : shz_vec3_init(0.0f, 100.0f, 200.0f);
    float vx = cam.x - LOOK_X, vy = cam.y - LOOK_Y, vz = cam.z - LOOK_Z;
    distance = sqrtf(vx * vx + vy * vy + vz * vz);
    camera.pitch = asinf(vy / distance);
    camera.yaw   = atan2f(-vx, -vz);

    printf("BLOOM: A bloom on/off, d-pad up/down strength, left/right spread, "
           "X picture size, triggers zoom, stick turns, Start exits\n");
    printf("VRAM free after loading: %luKB\n",
           (unsigned long)(pvr_mem_available() / 1024));

    DCBloom bloom = { .strength = 0.85f, .spread = 1.4f, .size = 128 };
    bool on = true, turning = false;
    float drift = 0.0f;

    dc_set_bloom(&bloom);

    while (!check_exit()) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        if (inp) {
            if (inp->ltrig > 0) distance -= ZOOM_SPEED * dt;
            if (inp->rtrig > 0) distance += ZOOM_SPEED * dt;
            if (distance < 120.0f)  distance = 120.0f;
            if (distance > 1700.0f) distance = 1700.0f;

            if (inp->buttons & CONT_DPAD_UP)    bloom.strength += 0.6f * dt;
            if (inp->buttons & CONT_DPAD_DOWN)  bloom.strength -= 0.6f * dt;
            if (inp->buttons & CONT_DPAD_RIGHT) bloom.spread += 1.6f * dt;
            if (inp->buttons & CONT_DPAD_LEFT)  bloom.spread -= 1.6f * dt;
            if (bloom.strength < 0.05f) bloom.strength = 0.05f;
            if (bloom.strength > 1.00f) bloom.strength = 1.00f;
            if (bloom.spread   < 0.20f) bloom.spread   = 0.20f;
            if (bloom.spread   > 3.00f) bloom.spread   = 3.00f;

            if (dc_input_pressed(inp, CONT_A)) on = !on;
            if (dc_input_pressed(inp, CONT_X))
                bloom.size = bloom.size == 64 ? 128 : bloom.size == 128 ? 256 : 64;

            if (dc_input_pressed(inp, CONT_Y)) turning = !turning;
            if (turning) drift += DRIFT * dt;
        }
        camera.yaw += drift;
        drift = 0.0f;
        dc_camera_orbit(&camera, look, distance, inp, LOOK_SPEED, dt);
        dc_camera_update(&camera);

        dc_set_bloom(on ? &bloom : NULL);
        dc_set_camera(&camera);
        dc_draw(scene, shz_vec3_init(0.0f, 0.0f, 0.0f));

        char line[64];
        if (on) snprintf(line, sizeof(line), "bloom  strength %.2f  spread %.1f  %dx%d",
                         bloom.strength, bloom.spread, bloom.size, bloom.size);
        else    snprintf(line, sizeof(line), "bloom off");
        dc_draw_text(line, 16, 440, 16, DC_COLOR_WHITE);

        dc_debug_stats();
        dc_frame_end();
    }

    dc_set_bloom(NULL);
    dc_model_free(scene);
    dc_shutdown();
    return 0;
}
