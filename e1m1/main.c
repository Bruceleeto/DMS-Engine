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
#include "dms/dc_debug.h"
#include "dms/collision.h"

/* Where assets are read from: /pc/ over dcload, /cd/ when built with make disc */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

/* ---- App state ---- */
static DCCamera camera;
static DCPlayer player;
static ColWorld* col_world = NULL;

static DMSModel* dms_model = NULL;
static shz_vec3_t dms_pos;
static float dms_scale = 1.0f;

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
    dc_debug_init();

    dc_camera_init(&camera);
    dc_player_init(&player);
    player.cam_mode = DC_CAM_NOCLIP;

    /* Load world */
    dms_model = dc_model_load(ASSETS "world/test.dms");
    dms_pos = shz_vec3_init(0.0f, 0.0f, 0.0f);
    dms_scale = 1.0f;

    if (dms_model && !dms_model->skeleton)
        col_world = col_build(dms_model, dms_pos, dms_scale);

    /* Spawn player on ground at origin */
    {
        shz_vec3_t spawn = shz_vec3_init(0.0f, col_world ? col_world->max_y + 50.0f : 40.0f, 0.0f);
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
        dc_draw(dms_model, dms_pos);
        dc_debug_stats();
        dc_frame_end();
    }

    dc_model_free(dms_model);
    col_free(col_world);
    dc_shutdown();

    return 0;
}
