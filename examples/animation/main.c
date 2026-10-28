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
#include "dms/dc_scene.h"

/* Where assets are read from: /pc/ over dcload; on a disc the engine finds them at /cd/ */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

/* The world and the Player (the skinned character: where it starts, which
 * way it faces, and its scale, ~1.8 units tall) are scenes/main.json. */

/* ---- App state ---- */
static DCCamera camera;
static DCPlayer player;
static ColWorld* col_world = NULL;
static DCScene* scene = NULL;

/* Player character (skinned, animated); dc_scene_draw draws it */
static DCSceneObject* hero = NULL;
static DMSModel* char_model = NULL;

/* Animation indices, in the order they are in model.glb */
#define ANIM_ATTACK 0
#define ANIM_STAND  12
#define ANIM_WALK   14

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

    dc_player_init(&player);
    player.cam_mode = DC_CAM_THIRD;
    player.move_speed = 0.08f;   /* walking pace for the character (per frame) */
    player.cam_distance = 3.0f;  /* closer so the character fills more of the screen */
    player.cam_height = 1.7f;

    /* Load world and character */
    scene = dc_scene_play(ASSETS, "scenes/main.json");
    if (!scene) {
        dc_shutdown();
        return 1;
    }
    dc_scene_apply(scene);
    dc_scene_camera(scene, &camera);

    DCSceneObject* world = dc_scene_find(scene, "World");
    if (world && world->m && !world->m->skeleton)
        col_world = col_build(world->m, world->pos, world->scale.x);

    hero = dc_scene_find(scene, "Player");
    char_model = hero ? hero->m : NULL;
    if (hero) player.yaw = SHZ_DEG_TO_RAD(hero->rot.y) - player.model_yaw_offset;

    /* Spawn player on the ground below the Player's place */
    {
        shz_vec3_t spawn = shz_vec3_init(hero ? hero->pos.x : 0.0f, col_world ? col_world->max_y + 50.0f : 40.0f,
                                         hero ? hero->pos.z : 0.0f);
        ColGroundHit gh = col_ground(col_world, spawn, 200.0f);
        if (gh.hit)
            spawn.y = gh.y + player.eye_height;
        player.pos = spawn;

        /* Third person: camera starts behind the player */
        shz_sincos_t sc = shz_sincosf(player.yaw);
        float feet = spawn.y - player.eye_height;
        camera.pos = shz_vec3_init(spawn.x - sc.sin * player.cam_distance,
                                   feet + player.cam_height,
                                   spawn.z - sc.cos * player.cam_distance);
    }

    printf("VRAM free after loading: %luKB\n", (unsigned long)(pvr_mem_available() / 1024));

    while (!check_exit()) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        /* ---- Animation: walk when moving, attack on X, else stand ---- */
        if (char_model) {
            bool moving = false;
            if (inp) {
                moving = inp->stick_x * inp->stick_x + inp->stick_y * inp->stick_y > 0.02f ||
                         dc_input_held(inp, CONT_DPAD_UP) || dc_input_held(inp, CONT_DPAD_DOWN) ||
                         dc_input_held(inp, CONT_DPAD_LEFT) || dc_input_held(inp, CONT_DPAD_RIGHT);
            }
            int anim = ANIM_STAND;
            if (inp && dc_input_held(inp, CONT_X)) anim = ANIM_ATTACK;
            else if (moving) anim = ANIM_WALK;
            if (dc_model_get_anim(char_model) != anim)
                dc_model_set_anim(char_model, anim);
            dc_model_animate(char_model, dt);
        }

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

        /* ---- Character sits at the player's feet ---- */
        if (hero) {
            hero->pos = shz_vec3_init(player.pos.x, player.pos.y - player.eye_height, player.pos.z);
            hero->rot.y = SHZ_RAD_TO_DEG(player.yaw + player.model_yaw_offset);
        }

        /* ---- Rendering ---- */
        dc_set_camera(&camera);
        dc_scene_draw(scene);
        dc_debug_stats();
        dc_frame_end();
    }

    col_free(col_world);
    dc_scene_free(scene);
    dc_shutdown();

    return 0;
}
