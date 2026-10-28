#include <kos.h>
#include <sh4zam/shz_sh4zam.h>
#include <stdlib.h>
#include <string.h>
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

#define LOOK_SPEED  1.5f
#define ZOOM_SPEED  10.0f
#define LIGHT_SPEED 1.2f        /* radians a second */
#define TURN_SPEED  0.6f

#define FLOOR_Y      0.0f
#define KNOT_HEIGHT  5.0f
#define LIGHT_REACH  9.0f       /* how far the light is from the knot */

/* ---- App state ---- */
static DCCamera camera;
static DMSModel* floor_model = NULL;
static DMSModel* knot = NULL;

static float distance = 22.0f;
static float light_around = 0.6f;   /* the light goes around the knot... */
static float light_up = 1.0f;       /* ...and up and down (radians above level) */

/* ========== MAIN ========== */

static bool check_exit(void) {
    const DCInput* inp = dc_input_get(0);
    if (!inp) return false;
    return (inp->buttons & CONT_START) != 0;
}

int main(int argc, char* argv[]) {
    dc_init((DCInitParams){ .vram_size = 512 * 1024 });
    dc_draw2d_init();
    dc_debug_init();

    dc_camera_init(&camera);
    camera.yaw = 0.5f;
    camera.pitch = 0.45f;

    floor_model = dc_model_load(ASSETS "world/floor.dms");
    knot = dc_model_load(ASSETS "knot/torus_knot.dms");

    printf("VRAM free after loading: %luKB\n", (unsigned long)(pvr_mem_available() / 1024));

    shz_vec3_t origin = shz_vec3_init(0.0f, 0.0f, 0.0f);
    shz_vec3_t knot_pos = shz_vec3_init(0.0f, KNOT_HEIGHT, 0.0f);
    float knot_yaw = 0.0f;

    while (!check_exit()) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        /* Stick turns the camera around the knot, triggers zoom, D-pad moves the light */
        if (inp) {
            if (inp->ltrig > 0) distance -= ZOOM_SPEED * dt;
            if (inp->rtrig > 0) distance += ZOOM_SPEED * dt;
            if (distance < 8.0f)  distance = 8.0f;
            if (distance > 35.0f) distance = 35.0f;
            if (dc_input_held(inp, CONT_DPAD_LEFT))  light_around -= LIGHT_SPEED * dt;
            if (dc_input_held(inp, CONT_DPAD_RIGHT)) light_around += LIGHT_SPEED * dt;
            if (dc_input_held(inp, CONT_DPAD_UP))    light_up += LIGHT_SPEED * dt;
            if (dc_input_held(inp, CONT_DPAD_DOWN))  light_up -= LIGHT_SPEED * dt;
            if (light_up < 0.7f) light_up = 0.7f;
            if (light_up > 1.5f) light_up = 1.5f;
        }
        dc_camera_orbit(&camera, knot_pos, distance, inp, LOOK_SPEED, dt);
        if (camera.pitch < 0.05f) camera.pitch = 0.05f;     /* stay above the floor */
        dc_camera_update(&camera);

        knot_yaw += TURN_SPEED * dt;

        shz_sincos_t around = shz_sincosf(light_around), up = shz_sincosf(light_up);
        DCShadow shadow = {
            .light = shz_vec3_init(knot_pos.x + LIGHT_REACH * up.cos * around.sin,
                                   knot_pos.y + LIGHT_REACH * up.sin,
                                   knot_pos.z + LIGHT_REACH * up.cos * around.cos),
            .floor_y = FLOOR_Y,
        };

        /* ---- Rendering ---- */
        dc_set_camera(&camera);
        dc_draw(floor_model, origin);
        dc_draw_ex(knot, &(DCDrawOpts){ .pos = knot_pos, .yaw = knot_yaw, .shadow = &shadow });
        dc_debug_stats();
        dc_frame_end();
    }

    dc_model_free(knot);
    dc_model_free(floor_model);
    dc_shutdown();

    return 0;
}
