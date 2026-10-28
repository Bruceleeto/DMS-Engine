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

/* scenes/main.json has the street (World) and the Spawn marker the player
 * drops from; the collision is built from the World object as placed. */

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

    /* Collision from the world as the scene places it */
    DCSceneObject* world = dc_scene_find(scene, "World");
    if (world && world->m && !world->m->skeleton)
        col_world = col_build(world->m, world->pos, world->scale.x);

    /* Spawn player on the ground under the Spawn marker (the origin if none) */
    {
        DCSceneObject* mark = dc_scene_find(scene, "Spawn");
        float sx = mark ? mark->pos.x : 0.0f, sz = mark ? mark->pos.z : 0.0f;
        shz_vec3_t spawn = shz_vec3_init(sx, col_world ? col_world->max_y + 50.0f : 40.0f, sz);
        ColGroundHit gh = col_ground(col_world, spawn, 200.0f);
        if (gh.hit)
            spawn.y = gh.y + player.eye_height;
        player.pos = spawn;

        /* Initialize camera behind player */
        shz_sincos_t _sc = shz_sincosf(player.yaw);
        float _feet = spawn.y - player.eye_height;
        camera.pos = shz_vec3_init(
            spawn.x - _sc.sin * player.cam_distance,
            _feet + player.cam_height,
            spawn.z - _sc.cos * player.cam_distance
        );
    }

    printf("VRAM free after loading: %luKB\n", (unsigned long)(pvr_mem_available() / 1024));

    while (!check_exit()) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        /* ---- Input ---- */
        /* Reset (Y button) */
        if (inp && dc_input_held(inp, CONT_Y)) {
            player.pos = shz_vec3_init(0.0f, 10.0f, 20.0f);
            player.vy = 0.0f;
            camera.pos = player.pos;
            camera.yaw = 0.0f;
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
