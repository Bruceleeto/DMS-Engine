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

/* Player character (skinned, animated) */
static DMSModel* char_model = NULL;
static float char_scale = 1.0f;

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

    dc_camera_init(&camera);
    dc_player_init(&player);
    player.cam_mode = DC_CAM_THIRD;
    player.move_speed = 0.08f;   /* walking pace for the character (per frame) */
    player.cam_distance = 3.0f;  /* closer so the character fills more of the screen */
    player.cam_height = 1.7f;

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

        /* Third person: camera starts behind the player */
        shz_sincos_t sc = shz_sincosf(player.yaw);
        float feet = spawn.y - player.eye_height;
        camera.pos = shz_vec3_init(spawn.x - sc.sin * player.cam_distance,
                                   feet + player.cam_height,
                                   spawn.z - sc.cos * player.cam_distance);
    }

    /* Load character, scaled so it matches the player's collision height */
    char_model = dc_model_load(ASSETS "char/model.dms");
    if (char_model) {
        float lo = 1e30f, hi = -1e30f;
        for (uint32_t m = 0; m < char_model->mesh_count; m++) {
            const DMSMesh* mesh = &char_model->meshes[m];
            for (uint32_t v = 0; v < mesh->vertex_count; v++) {
                float y = mesh->vertices[v].y;
                if (y < lo) lo = y;
                if (y > hi) hi = y;
            }
        }
        if (hi > lo) char_scale = player.height / (hi - lo);
        printf("Character height %.2f, scale %.3f\n", hi - lo, char_scale);
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
        shz_vec3_t char_pos = shz_vec3_init(player.pos.x, player.pos.y - player.eye_height, player.pos.z);
        float char_yaw = -(player.yaw + player.model_yaw_offset);

        /* ---- Rendering ---- */
        dc_set_camera(&camera);
        dc_draw(dms_model, dms_pos);
        dc_draw_ex(char_model, &(DCDrawOpts){ .pos = char_pos, .scale = char_scale, .yaw = char_yaw });
        dc_debug_stats();
        dc_frame_end();
    }

    dc_model_free(char_model);
    dc_model_free(dms_model);
    col_free(col_world);
    dc_shutdown();

    return 0;
}
