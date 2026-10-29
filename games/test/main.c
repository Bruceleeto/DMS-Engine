#include <kos.h>
#include <sh4zam/shz_sh4zam.h>
#include "dms/dc_engine.h"
#include "dms/dc_input.h"
#include "dms/dc_camera.h"
#include "dms/dc_draw.h"
#include "dms/dc_player.h"
#include "dms/dc_scene.h"

/* Where assets are read from: /pc/ over dcload; on a disc the engine finds them at /cd/ */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

/* The starting point for a new project (the editor's File > New project
 * copies this folder): whatever scene is open in the editor, flown through
 * with a free camera from where the scene's camera is. Y goes back there,
 * Start quits. Replace it with a game. */

int main(int argc, char* argv[]) {
    dc_init((DCInitParams){ 0 });

    DCScene* scene = dc_scene_play(ASSETS, "scenes/main.json");
    if (!scene) return 1;
    dc_scene_apply(scene);

    DCCamera camera;
    DCPlayer player;
    dc_scene_camera(scene, &camera);
    dc_player_init(&player);
    player.cam_mode = DC_CAM_NOCLIP;
    player.pos = camera.pos;
    player.yaw = camera.yaw;
    shz_vec3_t start = camera.pos;
    float start_yaw = camera.yaw;

    for (;;) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);
        if (inp && (inp->buttons & CONT_START)) break;

        /* Back to the start */
        if (inp && dc_input_held(inp, CONT_Y)) {
            player.pos = camera.pos = start;
            player.yaw = camera.yaw = start_yaw;
            player.vy = 0.0f;
            camera.pitch = 0.0f;
        }

        dc_player_update(&player, &camera, inp, NULL, dt);
        dc_camera_update(&camera);

        dc_set_camera(&camera);
        dc_scene_draw(scene);
        dc_frame_end();
    }

    dc_scene_free(scene);
    dc_shutdown();
    return 0;
}
