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
#include "dms/collision.h"
#include "dms/dc_scene.h"

/* Where assets are read from: /pc/ over dcload; on a disc the engine finds them at /cd/ */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

/* The track (World) and the car (Car) are scenes/main.json; the car's place
 * there is where the player starts, and the code then drives it. */

/* ---- App state ---- */
static DCCamera camera;
static DCPlayer player;
static DCScene* scene = NULL;
static ColWorld* col_world = NULL;
static DCSceneObject* car = NULL;    /* drawn at the player's feet */
static shz_vec3_t start;             /* the player's eyes over the car's start */

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

    dc_player_init(&player);
    player.cam_mode = DC_CAM_THIRD;
    player.move_speed = 1.0f;   /* per frame */

    scene = dc_scene_play(ASSETS, "scenes/main.json");
    if (!scene) return 1;
    dc_scene_apply(scene);
    dc_scene_camera(scene, &camera);    /* its fov; the code puts it behind the car */
    car = dc_scene_find(scene, "Car");
    if (!car) { printf("RACE_TRACK: the scene has no object called Car\n"); return 1; }
    start = shz_vec3_init(car->pos.x, car->pos.y + player.eye_height, car->pos.z);
    player.pos = start;
    player.yaw = SHZ_DEG_TO_RAD(car->rot.y);

    DCSceneObject* world = dc_scene_find(scene, "World");
    if (world && world->m)
        col_world = col_build(world->m, world->pos, world->scale.x);

    /* Car: camera distance follows the car's size */
    const DMSModel* car_model = car->m;
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

    /* Camera starts behind the player (the scene's camera is about there) */
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
            player.pos = start;
            player.vy = 0.0f;
            camera.pos = player.pos;
            camera.yaw = 0.0f;
            camera.pitch = 0.0f;
        }

        dc_player_update(&player, &camera, inp, col_world, dt);
        dc_camera_update(&camera);

        car->pos = shz_vec3_init(player.pos.x, player.pos.y - player.eye_height, player.pos.z);
        car->rot.y = SHZ_RAD_TO_DEG(player.yaw + player.model_yaw_offset);

        /* ---- Rendering ---- */
        dc_set_camera(&camera);
        dc_scene_draw(scene);
        dc_frame_end();
    }

    col_free(col_world);
    dc_scene_free(scene);
    dc_shutdown();

    return 0;
}
