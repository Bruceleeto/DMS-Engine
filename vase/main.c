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

/* ---- App state ---- */
static DCCamera camera;

static DMSModel* vase = NULL;
static DCImage* background = NULL;
static DCImage* environment = NULL;
#define VASE_SCALE  1.6f
#define MOVE_SPEED  3.0f    /* units a second */
#define TURN_SPEED  3.0f    /* radians a second */

/* Where the vase is, in front of a camera that stays at the origin looking down +z */
static float vase_x = 0.0f, vase_y = 0.0f, vase_dist = 5.0f;
static float rot_x = 0.0f, rot_y = 0.0f;

/* ========== MAIN ========== */

static bool check_exit(void) {
    const DCInput* inp = dc_input_get(0);
    if (!inp) return false;
    return (inp->buttons & CONT_START) != 0;
}

/* The vase turned by rot_x then rot_y: where its x, y and z axes point */
static void vase_rotation(float rot[9]) {
    shz_sincos_t x = shz_sincosf(rot_x), y = shz_sincosf(rot_y);
    rot[0] = y.cos;         rot[1] = 0.0f;   rot[2] = -y.sin;
    rot[3] = x.sin * y.sin; rot[4] = x.cos;  rot[5] = x.sin * y.cos;
    rot[6] = x.cos * y.sin; rot[7] = -x.sin; rot[8] = x.cos * y.cos;
}

int main(int argc, char* argv[]) {
    dc_init((DCInitParams){ .vram_size = 1024 * 1024 });
    dc_draw2d_init();
    dc_debug_init();

    dc_camera_init(&camera);
    dc_camera_update(&camera);

    vase = dc_model_load(ASSETS "vase/vase.dms");
    background = dc_image_load(ASSETS "background/background.dt");
    environment = dc_image_load(ASSETS "environment/environment.dt");
    dc_set_environment(environment);

    printf("VRAM free after loading: %luKB\n", (unsigned long)(pvr_mem_available() / 1024));

    while (!check_exit()) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        /* D-pad moves, triggers push and pull, A/B and X/Y turn */
        if (inp) {
            if (dc_input_held(inp, CONT_DPAD_LEFT))  vase_x -= MOVE_SPEED * dt;
            if (dc_input_held(inp, CONT_DPAD_RIGHT)) vase_x += MOVE_SPEED * dt;
            if (dc_input_held(inp, CONT_DPAD_UP))    vase_y += MOVE_SPEED * dt;
            if (dc_input_held(inp, CONT_DPAD_DOWN))  vase_y -= MOVE_SPEED * dt;
            if (dc_input_held(inp, CONT_A)) rot_y -= TURN_SPEED * dt;
            if (dc_input_held(inp, CONT_B)) rot_y += TURN_SPEED * dt;
            if (dc_input_held(inp, CONT_X)) rot_x -= TURN_SPEED * dt;
            if (dc_input_held(inp, CONT_Y)) rot_x += TURN_SPEED * dt;
            if (inp->ltrig > 0) vase_dist -= MOVE_SPEED * dt;
            if (inp->rtrig > 0) vase_dist += MOVE_SPEED * dt;
            if (vase_dist < 1.0f)  vase_dist = 1.0f;
            if (vase_dist > 20.0f) vase_dist = 20.0f;
        }
        dc_camera_update(&camera);

        float rot[9];
        vase_rotation(rot);

        dc_set_camera(&camera);
        dc_draw_background(background);
        dc_draw_ex(vase, &(DCDrawOpts){ .pos = shz_vec3_init(vase_x, vase_y, vase_dist),
                                        .scale = VASE_SCALE, .rot = rot });
        dc_debug_stats();
        dc_frame_end();
    }

    dc_model_free(vase);
    dc_image_free(background);
    dc_set_environment(NULL);
    dc_image_free(environment);
    dc_shutdown();

    return 0;
}
