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

/* The goal block from the Pyrite64 jam game (Cathode Quest 64), a rounded
 * cube drawn as a mirror of a small environment image, turning the way
 * tiny3d's fresnel example turns its models. The image is looked up by the
 * normal as the camera sees it, so it slides over the block as it turns. */

static DCCamera camera;
static DMSModel* goal = NULL;
static DCImage*  env[2] = { NULL, NULL };
static const char* ENV_NAMES[2] = { "env1 (the jam's)", "env_gold" };
static int which = 0;

#define MOVE_SPEED  6.0f    /* units a second */
#define SPIN        0.9f    /* radians a second, tiny3d's 0.015 a frame */

static float goal_x = 0.0f, goal_y = 0.0f, goal_dist = 10.0f;
static float angle = 0.0f;

static bool check_exit(void) {
    const DCInput* inp = dc_input_get(0);
    return inp && (inp->buttons & CONT_START);
}

/* tiny3d's euler (0, angle*0.8 - 1.5, angle*0.1): Y then a little Z */
static void goal_rotation(float rot[9]) {
    shz_sincos_t y = shz_sincosf(angle * 0.8f - 1.5f), z = shz_sincosf(angle * 0.1f);
    rot[0] = y.cos * z.cos;  rot[1] = z.sin;         rot[2] = -y.sin * z.cos;
    rot[3] = -y.cos * z.sin; rot[4] = z.cos;         rot[5] = y.sin * z.sin;
    rot[6] = y.sin;          rot[7] = 0.0f;          rot[8] = y.cos;
}

int main(int argc, char* argv[]) {
    dc_init((DCInitParams){ .vram_size = 1024 * 1024 });
    dc_draw2d_init();
    dc_debug_init();
    dc_set_clear_color(0xFF28283C);   /* tiny3d's 40, 40, 60 */

    dc_camera_init(&camera);
    dc_camera_update(&camera);

    goal   = dc_model_load(ASSETS "goal/goal.dms");
    env[0] = dc_image_load(ASSETS "environment/env1.dt");
    env[1] = dc_image_load(ASSETS "environment/env_gold.dt");
    dc_set_environment(env[which]);

    while (!check_exit()) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        /* It turns on its own; A turns it faster, B puts it back. D-pad
         * moves it, the triggers push and pull, X swaps the image */
        angle += SPIN * dt;
        if (inp) {
            if (dc_input_held(inp, CONT_A)) angle += SPIN * 1.7f * dt;
            if (dc_input_pressed(inp, CONT_B)) angle = 0.0f;
            if (dc_input_pressed(inp, CONT_X)) { which ^= 1; dc_set_environment(env[which]); }
            if (dc_input_held(inp, CONT_DPAD_LEFT))  goal_x -= MOVE_SPEED * dt;
            if (dc_input_held(inp, CONT_DPAD_RIGHT)) goal_x += MOVE_SPEED * dt;
            if (dc_input_held(inp, CONT_DPAD_UP))    goal_y += MOVE_SPEED * dt;
            if (dc_input_held(inp, CONT_DPAD_DOWN))  goal_y -= MOVE_SPEED * dt;
            if (inp->ltrig > 0) goal_dist -= MOVE_SPEED * dt;
            if (inp->rtrig > 0) goal_dist += MOVE_SPEED * dt;
            if (goal_dist < 4.0f)  goal_dist = 4.0f;
            if (goal_dist > 40.0f) goal_dist = 40.0f;
        }
        dc_camera_update(&camera);

        float rot[9];
        goal_rotation(rot);

        dc_set_camera(&camera);
        dc_draw_ex(goal, &(DCDrawOpts){ .pos = shz_vec3_init(goal_x, goal_y, goal_dist), .scale = 1.0f, .rot = rot });
        dc_draw_text(ENV_NAMES[which], 16, 440, 16, DC_COLOR_WHITE);
        dc_debug_stats();
        dc_frame_end();
    }

    dc_model_free(goal);
    dc_set_environment(NULL);
    dc_image_free(env[0]);
    dc_image_free(env[1]);
    dc_shutdown();
    return 0;
}
