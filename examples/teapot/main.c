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

/* After tiny3d's hdr_bloom example: a chrome teapot over a blurred photo. The
 * teapot is a mirror of a small sphere image (Metallic 1, Roughness 0) and
 * has an Emission colour too, so the bloom pass takes what it reflects and
 * the bright parts of the reflection flare. */

static DCCamera  camera;
static DMSModel* teapot = NULL;
static DCImage*  background = NULL;
static DCImage*  env[2] = { NULL, NULL };
static const char* ENV_NAMES[2] = { "env0", "env_gold" };
static int which = 0;

#define MOVE_SPEED  8.0f    /* units a second */
#define SPIN        0.6f    /* radians a second */

static float pot_x = 0.0f, pot_y = 0.0f, pot_dist = 16.0f;
static float angle = 0.0f;

static bool check_exit(void) {
    const DCInput* inp = dc_input_get(0);
    return inp && (inp->buttons & CONT_START);
}

/* Turned about Y, tipped a little towards the camera */
static void pot_rotation(float rot[9]) {
    shz_sincos_t y = shz_sincosf(angle), x = shz_sincosf(0.35f);
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

    teapot     = dc_model_load(ASSETS "teapot/teapot.dms");
    background = dc_image_load(ASSETS "background/blur.dt");
    env[0]     = dc_image_load(ASSETS "environment/env0.dt");
    env[1]     = dc_image_load(ASSETS "environment/env_gold.dt");
    dc_set_environment(env[which]);

    DCBloom bloom = { .strength = 0.9f, .spread = 1.4f, .size = 128 };
    bool on = true;
    dc_set_bloom(&bloom);

    printf("TEAPOT: A bloom on/off, X image, B reset turn, d-pad up/down strength, "
           "left/right spread, triggers push and pull, Start exits\n");

    while (!check_exit()) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        angle += SPIN * dt;
        if (inp) {
            if (dc_input_pressed(inp, CONT_A)) on = !on;
            if (dc_input_pressed(inp, CONT_B)) angle = 0.0f;
            if (dc_input_pressed(inp, CONT_X)) { which ^= 1; dc_set_environment(env[which]); }
            if (inp->buttons & CONT_DPAD_UP)    bloom.strength += 0.6f * dt;
            if (inp->buttons & CONT_DPAD_DOWN)  bloom.strength -= 0.6f * dt;
            if (inp->buttons & CONT_DPAD_RIGHT) bloom.spread += 1.6f * dt;
            if (inp->buttons & CONT_DPAD_LEFT)  bloom.spread -= 1.6f * dt;
            if (bloom.strength < 0.05f) bloom.strength = 0.05f;
            if (bloom.strength > 1.00f) bloom.strength = 1.00f;
            if (bloom.spread   < 0.20f) bloom.spread   = 0.20f;
            if (bloom.spread   > 3.00f) bloom.spread   = 3.00f;
            if (inp->ltrig > 0) pot_dist -= MOVE_SPEED * dt;
            if (inp->rtrig > 0) pot_dist += MOVE_SPEED * dt;
            if (pot_dist < 8.0f)  pot_dist = 8.0f;
            if (pot_dist > 60.0f) pot_dist = 60.0f;
        }
        dc_camera_update(&camera);

        float rot[9];
        pot_rotation(rot);

        dc_set_bloom(on ? &bloom : NULL);
        dc_set_camera(&camera);
        dc_draw_background(background);
        dc_draw_ex(teapot, &(DCDrawOpts){ .pos = shz_vec3_init(pot_x, pot_y, pot_dist), .scale = 1.0f, .rot = rot });

        char line[64];
        if (on) snprintf(line, sizeof(line), "%s  bloom strength %.2f spread %.1f", ENV_NAMES[which], bloom.strength, bloom.spread);
        else    snprintf(line, sizeof(line), "%s  bloom off", ENV_NAMES[which]);
        dc_draw_text(line, 16, 440, 16, DC_COLOR_WHITE);
        dc_debug_stats();
        dc_frame_end();
    }

    dc_set_bloom(NULL);
    dc_model_free(teapot);
    dc_image_free(background);
    dc_set_environment(NULL);
    dc_image_free(env[0]);
    dc_image_free(env[1]);
    dc_shutdown();
    return 0;
}
