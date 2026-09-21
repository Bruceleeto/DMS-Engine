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

/* The TV shows what the projector on the table sees. The projector looks at
 * the TV, so the TV shows itself, smaller each time. */
static DCTarget* tv = NULL;
static DCCamera  projector_cam;
#define TV_SIZE        256          /* the TV's picture, in pixels */
#define TV_MATERIAL    "RTT_effect" /* the screen's material name in Blender */
#define START_X        -3.0f        /* the player starts behind the table */

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
    player.cam_mode = DC_CAM_FPS;
    player.move_speed = 0.06f;  /* per frame; the room is 10 units across */

    /* Load world */
    dms_model = dc_model_load(ASSETS "world/rtt.dms");
    dms_pos = shz_vec3_init(0.0f, 0.0f, 0.0f);
    dms_scale = 1.0f;

    if (dms_model && !dms_model->skeleton)
        col_world = col_build(dms_model, dms_pos, dms_scale);

    tv = dc_target_create(TV_SIZE, TV_SIZE);
    dc_target_show_on(tv, dms_model, TV_MATERIAL);

    /* Just above the projector (it sits on the table in the middle of the
     * room), looking along +x at the TV on the wall, zoomed in on it */
    dc_camera_init(&projector_cam);
    projector_cam.pos = shz_vec3_init(0.3f, 1.05f, 0.0f);
    projector_cam.yaw = F_PI * 0.5f;
    projector_cam.fov = 60.0f;

    /* Spawn player on the floor behind the table, facing the TV */
    {
        shz_vec3_t spawn = shz_vec3_init(START_X, 2.0f, 0.0f);
        ColGroundHit gh = col_ground(col_world, spawn, 200.0f);
        if (gh.hit)
            spawn.y = gh.y + player.eye_height;
        player.pos = spawn;

        /* First person: camera sits at the player's eyes */
        camera.pos = spawn;
        camera.yaw = player.yaw = F_PI * 0.5f;
    }

    printf("VRAM free after loading: %luKB\n", (unsigned long)(pvr_mem_available() / 1024));

    while (!check_exit()) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        /* ---- Input ---- */
        /* Reset (Y button) */
        if (inp && dc_input_held(inp, CONT_Y)) {
            player.pos = shz_vec3_init(START_X, 2.0f, 0.0f);
            player.vy = 0.0f;
            camera.pos = player.pos;
            camera.yaw = player.yaw = F_PI * 0.5f;
            camera.pitch = 0.0f;
        }

        dc_player_update(&player, &camera, inp, col_world, dt);
        dc_camera_update(&camera);
        /* The projector's camera sweeps the room, a full turn every 12 seconds,
         * so the TV is seen to be live */
        projector_cam.yaw = (float)(dc_time_ms() % 12000) * (2.0f * F_PI / 12000.0f);
        dc_camera_update(&projector_cam);

        /* ---- Rendering ---- */
        /* The room as the projector sees it, into the TV's picture */
        dc_set_target(tv);
        dc_set_camera(&projector_cam);
        dc_draw(dms_model, dms_pos);

        /* The room as the player sees it, to the screen */
        dc_set_target(NULL);
        dc_set_camera(&camera);
        dc_draw(dms_model, dms_pos);
        dc_debug_stats();
        dc_frame_end();
    }

    dc_target_free(tv);
    dc_model_free(dms_model);
    col_free(col_world);
    dc_shutdown();

    return 0;
}
