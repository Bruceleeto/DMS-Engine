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
#include "dms/dc_scene.h"

/* Where assets are read from: /pc/ over dcload; on a disc the engine finds them at /cd/ */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

/* The goal block from the Pyrite64 jam game (Cathode Quest 64), a rounded
 * cube drawn as a mirror of a small environment image, turning the way
 * tiny3d's fresnel example turns its models. The image is looked up by the
 * normal as the camera sees it, so it slides over the block as it turns.
 *
 * The Goal, its place and starting turn, the camera and the background are
 * scenes/main.json; the code turns it and moves it about. */

static DCCamera camera;
static DCScene* scene = NULL;
static DCSceneObject* goal = NULL;
static DCImage*  env[2] = { NULL, NULL };
static const char* ENV_NAMES[2] = { "env1 (the jam's)", "env_gold" };
static int which = 0;

#define MOVE_SPEED  6.0f    /* units a second */
#define SPIN        0.9f    /* radians a second, tiny3d's 0.015 a frame */

static float angle = 0.0f;     /* on top of the scene's turn */

static bool check_exit(void) {
    const DCInput* inp = dc_input_get(0);
    return inp && (inp->buttons & CONT_START);
}

int main(int argc, char* argv[]) {
    dc_init((DCInitParams){ .vram_size = 1024 * 1024 });
    dc_draw2d_init();

    scene = dc_scene_play(ASSETS, "scenes/main.json");   /* background: tiny3d's 40, 40, 60 */
    if (!scene) return 1;
    dc_scene_apply(scene);
    dc_scene_camera(scene, &camera);
    dc_camera_update(&camera);
    goal = dc_scene_find(scene, "Goal");
    if (!goal) { printf("FRESNEL: the scene has no object called Goal\n"); return 1; }
    /* tiny3d's euler (0, angle*0.8 - 1.5, angle*0.1): Y then a little Z,
     * the scene's -1.5 radians about y being where it starts */
    const shz_vec3_t start_turn = goal->rot;

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
            if (dc_input_held(inp, CONT_DPAD_LEFT))  goal->pos.x -= MOVE_SPEED * dt;
            if (dc_input_held(inp, CONT_DPAD_RIGHT)) goal->pos.x += MOVE_SPEED * dt;
            if (dc_input_held(inp, CONT_DPAD_UP))    goal->pos.y += MOVE_SPEED * dt;
            if (dc_input_held(inp, CONT_DPAD_DOWN))  goal->pos.y -= MOVE_SPEED * dt;
            if (inp->ltrig > 0) goal->pos.z -= MOVE_SPEED * dt;
            if (inp->rtrig > 0) goal->pos.z += MOVE_SPEED * dt;
            if (goal->pos.z < 4.0f)  goal->pos.z = 4.0f;
            if (goal->pos.z > 40.0f) goal->pos.z = 40.0f;
        }
        dc_camera_update(&camera);

        goal->rot.y = start_turn.y + SHZ_RAD_TO_DEG(angle * 0.8f);
        goal->rot.z = start_turn.z + SHZ_RAD_TO_DEG(angle * 0.1f);

        dc_set_camera(&camera);
        dc_scene_draw(scene);
        dc_draw_text(ENV_NAMES[which], 16, 440, 16, DC_COLOR_WHITE);
        dc_frame_end();
    }

    dc_scene_free(scene);
    dc_set_environment(NULL);
    dc_image_free(env[0]);
    dc_image_free(env[1]);
    dc_shutdown();
    return 0;
}
