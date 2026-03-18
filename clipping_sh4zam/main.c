#include <kos.h>
#include <dc/perfctr.h>
#include <sh4zam/shz_sh4zam.h>
#include <stdlib.h>
#include <string.h>
#include "dms/dc_engine.h"
#include "dms/dc_input.h"
#include "dms/dc_camera.h"
#include "dms/dc_model.h"
#include "dms/dc_player.h"
#include "dms/dc_draw2d.h"
#include "dms/dc_debug.h"
#include "dms/collision.h"

/* ---- App state ---- */
static DCCamera camera;
static DCPlayer player;
static ColWorld* col_world = NULL;

static DMSModel* dms_model = NULL;
static shz_vec3_t dms_pos;
static float dms_scale = 1.0f;

static DMSModel* robot_model = NULL;
static float robot_scale = 1.0f;

/* ========== PROFILING ========== */

typedef struct {
    uint64_t anim_ns;
    uint64_t camera_ns;
    uint64_t render_ns;
    uint64_t frame_total_ns;

    uint32_t world_meshes_drawn;
    uint32_t world_meshes_culled;
    uint32_t world_verts_xformed;
    uint32_t world_verts_clipped;

    uint32_t samples;
} FrameProfile;

static FrameProfile prof = {0};

#define PROF_INTERVAL 60

/* On-screen profile text (updated every PROF_INTERVAL frames) */
static char prof_lines[6][48];

static void prof_print_and_reset(void) {
    if (prof.samples == 0) return;
    float n = (float)prof.samples;

    float anim_us   = (float)prof.anim_ns / n / 1000.0f;
    float cam_us    = (float)prof.camera_ns / n / 1000.0f;
    float render_us = (float)prof.render_ns / n / 1000.0f;
    float total_ms  = (float)prof.frame_total_ns / n / 1000000.0f;
    uint32_t drawn  = prof.world_meshes_drawn / prof.samples;
    uint32_t culled = prof.world_meshes_culled / prof.samples;
    uint32_t xform  = prof.world_verts_xformed / prof.samples;
    uint32_t clip   = prof.world_verts_clipped / prof.samples;

    snprintf(prof_lines[0], sizeof(prof_lines[0]), "FRAME: %.2f ms", total_ms);
    snprintf(prof_lines[1], sizeof(prof_lines[1]), "ANIM: %.0f us", anim_us);
    snprintf(prof_lines[2], sizeof(prof_lines[2]), "CAM:  %.0f us", cam_us);
    snprintf(prof_lines[3], sizeof(prof_lines[3]), "DRAW: %.0f us", render_us);
    snprintf(prof_lines[4], sizeof(prof_lines[4]), "drawn %lu culled %lu", (unsigned long)drawn, (unsigned long)culled);
    snprintf(prof_lines[5], sizeof(prof_lines[5]), "xform %lu clip %lu", (unsigned long)xform, (unsigned long)clip);

    memset(&prof, 0, sizeof(prof));
}

/* ========== MAIN ========== */

static bool check_exit(void) {
    const DCInput* inp = dc_input_get(0);
    if (!inp) return false;
    return (inp->buttons & CONT_START) != 0;
}

int main(int argc, char* argv[]) {
    dc_init((DCInitParams){0});
    dc_draw2d_init();
    dc_debug_init();

    dc_camera_init(&camera);
    dc_player_init(&player);
    player.cam_mode = DC_CAM_THIRD;

    /* Load world */
    dms_model = dc_model_load("/pc/world/test.dms");
    if (dms_model)
        dc_model_load_textures(dms_model, "/pc/world");
    dms_pos = shz_vec3_init(0.0f, 0.0f, 0.0f);
    dms_scale = 1.0f;

    if (dms_model && !dms_model->skeleton)
        col_world = col_build(dms_model, dms_pos, dms_scale, 4.0f);

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

    /* Load robot (player character model) */
    robot_model = dc_model_load("/pc/robot/robot.dms");
    if (robot_model)
        dc_model_load_textures(robot_model, "/pc/robot");
    robot_scale = 1.0f;

    char fps_str[32];
    snprintf(fps_str, sizeof(fps_str), "FPS: --");

    while (!check_exit()) {
        uint64_t frame_start = perf_cntr_timer_ns();

        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        /* ---- Animation: run when moving, freeze when idle ---- */
        uint64_t t0 = perf_cntr_timer_ns();
        {
            bool is_moving = false;
            if (inp) {
                float sx = inp->stick_x;
                float sy = inp->stick_y;
                is_moving = (sx * sx + sy * sy > 0.02f);
                if (!is_moving) {
                    is_moving = dc_input_held(inp, CONT_DPAD_UP) ||
                                dc_input_held(inp, CONT_DPAD_DOWN) ||
                                dc_input_held(inp, CONT_DPAD_LEFT) ||
                                dc_input_held(inp, CONT_DPAD_RIGHT);
                }
            }

            dc_model_set_anim(robot_model, 12);  /* sn_run_loop */
            dc_model_animate(robot_model, is_moving ? dt * 3.0f : 0.0f);
        }
        prof.anim_ns += perf_cntr_timer_ns() - t0;

        /* ---- Input ---- */
        t0 = perf_cntr_timer_ns();

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
        prof.camera_ns += perf_cntr_timer_ns() - t0;

        /* ---- FPS string ---- */
        snprintf(fps_str, sizeof(fps_str), "FPS: %.1f", dc_fps());

        /* ---- Robot position: at player feet ---- */
        shz_vec3_t robot_pos = shz_vec3_init(
            player.pos.x,
            player.pos.y - player.eye_height,
            player.pos.z
        );

        /* ---- Rendering (by PVR list: OP -> TR -> PT) ---- */
        t0 = perf_cntr_timer_ns();
        dc_model_reset_stats();

        /* Pass 1: Opaque */
        dc_list_begin(PVR_LIST_OP_POLY);
        dc_model_draw_list(dms_model, dms_pos, dms_scale, &camera, PVR_LIST_OP_POLY);
        dc_model_draw_list_rotated(robot_model, robot_pos, robot_scale,
                                   -(player.yaw + player.model_yaw_offset), &camera, PVR_LIST_OP_POLY);

        /* Pass 2: Transparent */
        dc_list_begin(PVR_LIST_TR_POLY);
        dc_model_draw_list(dms_model, dms_pos, dms_scale, &camera, PVR_LIST_TR_POLY);
        dc_model_draw_list_rotated(robot_model, robot_pos, robot_scale,
                                   -(player.yaw + player.model_yaw_offset), &camera, PVR_LIST_TR_POLY);

        /* Pass 3: Punch-through + HUD */
        dc_list_begin(PVR_LIST_PT_POLY);
        dc_model_draw_list(dms_model, dms_pos, dms_scale, &camera, PVR_LIST_PT_POLY);
        dc_model_draw_list_rotated(robot_model, robot_pos, robot_scale,
                                   -(player.yaw + player.model_yaw_offset), &camera, PVR_LIST_PT_POLY);

        dc_draw_text(fps_str, 10, 10, 16, DC_COLOR_GREEN);

        /* On-screen profile */
        if (prof_lines[0][0]) {
            int py = 480 - 16 * 6 - 4;
            for (int i = 0; i < 6; i++)
                dc_draw_text(prof_lines[i], 10, py + i * 16, 16, DC_COLOR_GREEN);
        }

        const DCModelStats* ms = dc_model_get_stats();
        prof.world_meshes_drawn  += ms->meshes_drawn;
        prof.world_meshes_culled += ms->meshes_culled;
        prof.world_verts_xformed += ms->verts_xformed;
        prof.world_verts_clipped += ms->verts_clipped;

        prof.render_ns += perf_cntr_timer_ns() - t0;

        dc_frame_end();

        prof.frame_total_ns += perf_cntr_timer_ns() - frame_start;
        prof.samples++;

        if (prof.samples >= PROF_INTERVAL)
            prof_print_and_reset();
    }

    dc_model_free(robot_model);
    dc_model_free(dms_model);
    col_free(col_world);
    dc_shutdown();

    return 0;
}
