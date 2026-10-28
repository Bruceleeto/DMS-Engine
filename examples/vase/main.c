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

/* scenes/main.json has the vase (place, turn, size) and the camera, which
 * stays put; the code moves and turns the vase. Background and environment
 * map stay here. */

/* ---- App state ---- */
static DCCamera camera;

static DCScene* scene = NULL;
static DCSceneObject* vase = NULL;
static DCImage* background = NULL;
static DCImage* environment = NULL;
#define MOVE_SPEED  3.0f    /* units a second */
#define TURN_SPEED  SHZ_RAD_TO_DEG(3.0f)    /* degrees a second */

/* ========== MAIN ========== */

static bool check_exit(void) {
    const DCInput* inp = dc_input_get(0);
    if (!inp) return false;
    return (inp->buttons & CONT_START) != 0;
}

int main(int argc, char* argv[]) {
    dc_init((DCInitParams){ .vram_size = 1024 * 1024 });
    dc_draw2d_init();

    scene = dc_scene_play(ASSETS, "scenes/main.json");
    if (!scene) return 1;
    dc_scene_apply(scene);
    dc_scene_camera(scene, &camera);
    dc_camera_update(&camera);
    vase = dc_scene_find(scene, "Vase");
    if (!vase) { printf("VASE: the scene has no object called Vase\n"); return 1; }
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
            if (dc_input_held(inp, CONT_DPAD_LEFT))  vase->pos.x -= MOVE_SPEED * dt;
            if (dc_input_held(inp, CONT_DPAD_RIGHT)) vase->pos.x += MOVE_SPEED * dt;
            if (dc_input_held(inp, CONT_DPAD_UP))    vase->pos.y += MOVE_SPEED * dt;
            if (dc_input_held(inp, CONT_DPAD_DOWN))  vase->pos.y -= MOVE_SPEED * dt;
            if (dc_input_held(inp, CONT_A)) vase->rot.y -= TURN_SPEED * dt;
            if (dc_input_held(inp, CONT_B)) vase->rot.y += TURN_SPEED * dt;
            if (dc_input_held(inp, CONT_X)) vase->rot.x -= TURN_SPEED * dt;
            if (dc_input_held(inp, CONT_Y)) vase->rot.x += TURN_SPEED * dt;
            if (inp->ltrig > 0) vase->pos.z -= MOVE_SPEED * dt;
            if (inp->rtrig > 0) vase->pos.z += MOVE_SPEED * dt;
            if (vase->pos.z < 1.0f)  vase->pos.z = 1.0f;
            if (vase->pos.z > 20.0f) vase->pos.z = 20.0f;
        }
        dc_camera_update(&camera);

        dc_set_camera(&camera);
        dc_draw_background(background);
        dc_scene_draw(scene);
        dc_frame_end();
    }

    dc_scene_free(scene);
    dc_image_free(background);
    dc_set_environment(NULL);
    dc_image_free(environment);
    dc_shutdown();

    return 0;
}
