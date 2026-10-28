#include <kos.h>
#include <sh4zam/shz_sh4zam.h>
#include <stdlib.h>
#include <string.h>
#include "dms/dc_engine.h"
#include "dms/dc_input.h"
#include "dms/dc_camera.h"
#include "dms/dc_model.h"
#include "dms/dc_draw.h"
#include "dms/dc_player.h"
#include "dms/dc_draw2d.h"
#include "dms/collision.h"
#include "dms/dc_scene.h"

/* Where assets are read from: /pc/ over dcload; on a disc the engine finds them at /cd/ */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

/* Garry's Mod's gm_construct, brought in from the editor's Garry's Mod tab:
 * scenes/main.json holds the map as World (its baked light in the vertex
 * colours), the props where the map places them, and the camera where the
 * map starts a player. A free camera flies through it. */

/* ---- App state ---- */
static DCCamera camera;
static DCPlayer player;
static ColWorld* col_world = NULL;

static DCScene* scene = NULL;

/* ========== MAIN ========== */

static bool check_exit(void) {
    const DCInput* inp = dc_input_get(0);
    if (!inp) return false;
    return (inp->buttons & CONT_START) != 0;
}

int main(int argc, char* argv[]) {
    /* Vertex buffer is double-buffered by KOS, so this costs 2x in VRAM */
    dc_init((DCInitParams){ .vram_size = 2300 * 1024 });
    dc_draw2d_init();

    scene = dc_scene_play(ASSETS, "scenes/main.json");
    if (!scene) return 1;
    dc_scene_apply(scene);
    dc_scene_camera(scene, &camera);
    dc_player_init(&player);
    player.cam_mode = DC_CAM_NOCLIP;
    player.pos = camera.pos;
    player.yaw = camera.yaw;
    shz_vec3_t start = camera.pos;
    float start_yaw = camera.yaw;

    /* Collision for the world, where the scene puts it */
    DCSceneObject* world = dc_scene_find(scene, "World");
    if (world && world->m && !world->m->skeleton)
        col_world = col_build(world->m, world->pos, world->scale.x);

    printf("VRAM free after loading: %luKB\n", (unsigned long)(pvr_mem_available() / 1024));

    while (!check_exit()) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        /* ---- Input ---- */
        /* Back to the start (Y button) */
        if (inp && dc_input_held(inp, CONT_Y)) {
            player.pos = start;
            player.yaw = start_yaw;
            player.vy = 0.0f;
            camera.pos = start;
            camera.yaw = start_yaw;
            camera.pitch = 0.0f;
        }

        dc_player_update(&player, &camera, inp, col_world, dt);
        dc_camera_update(&camera);

        /* ---- Rendering ---- */
        dc_set_camera(&camera);
        dc_scene_draw(scene);
        dc_frame_end();
    }

    dc_scene_free(scene);
    col_free(col_world);
    dc_shutdown();

    return 0;
}
