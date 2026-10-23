#include <kos.h>
#include <sh4zam/shz_sh4zam.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
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


/* Track */
static DMSModel* world_model = NULL;
static ColWorld* col_world = NULL;

/* Car: the player's model, drawn at the player's feet */
static DMSModel* car_model = NULL;

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
    player.cam_mode = DC_CAM_THIRD;
    player.move_speed = 1.0f;   /* per frame */

    player.pos = shz_vec3_init(0.0f, 50.0f, 0.0f);

    world_model = dc_model_load(ASSETS "world/test.dms");
    if (world_model)
        col_world = col_build(world_model, shz_vec3_init(0.0f, 0.0f, 0.0f), 1.0f);

    /* Car: camera distance follows the car's size */
    car_model = dc_model_load(ASSETS "car/car.dms");
    if (car_model) {
        float r = 0.0f;
        for (uint32_t m = 0; m < car_model->mesh_count; m++) {
            const DMSMesh* mesh = &car_model->meshes[m];
            float d = sqrtf(mesh->bound_cx * mesh->bound_cx + mesh->bound_cy * mesh->bound_cy +
                            mesh->bound_cz * mesh->bound_cz) + mesh->bound_radius;
            if (d > r) r = d;
        }
        player.cam_distance = r * 2.0f;
        player.cam_height   = r * 0.8f;
        printf("Car radius %.2f\n", r);
    }

    /* Camera starts behind the player */
    {
        shz_sincos_t sc = shz_sincosf(player.yaw);
        float feet = player.pos.y - player.eye_height;
        camera.pos = shz_vec3_init(player.pos.x - sc.sin * player.cam_distance,
                                   feet + player.cam_height,
                                   player.pos.z - sc.cos * player.cam_distance);
    }

    printf("VRAM free after loading: %luKB\n", (unsigned long)(pvr_mem_available() / 1024));

    while (!check_exit()) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        /* ---- Input ---- */
        /* Reset (Y button) */
        if (inp && dc_input_held(inp, CONT_Y)) {
            player.pos = shz_vec3_init(0.0f, 50.0f, 0.0f);
            player.vy = 0.0f;
            camera.pos = player.pos;
            camera.yaw = 0.0f;
            camera.pitch = 0.0f;
        }

        dc_player_update(&player, &camera, inp, col_world, dt);
        dc_camera_update(&camera);

        shz_vec3_t car_pos = shz_vec3_init(player.pos.x, player.pos.y - player.eye_height, player.pos.z);
        float car_yaw = -(player.yaw + player.model_yaw_offset);

        /* ---- Rendering ---- */
        dc_set_camera(&camera);
        dc_draw(world_model, shz_vec3_init(0.0f, 0.0f, 0.0f));
        dc_draw_ex(car_model, &(DCDrawOpts){ .pos = car_pos, .yaw = car_yaw });
        dc_debug_stats();
        dc_frame_end();
    }

    col_free(col_world);
    dc_model_free(world_model);
    dc_model_free(car_model);
    dc_shutdown();

    return 0;
}
