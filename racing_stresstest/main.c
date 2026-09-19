#include <kos.h>
#include <dc/perfctr.h>
#include <sh4zam/shz_sh4zam.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
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


/* Ship: the player's model, drawn at the player's feet */
static DMSModel* ship_model = NULL;

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
    uint32_t tris_drawn;

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

    /* Wall-clock interval since the last report, so vsync waits are counted */
    static uint64_t last_report_ms = 0;
    uint64_t now_ms = timer_ms_gettime64();
    if (last_report_ms) {
        float secs = (float)(now_ms - last_report_ms) / 1000.0f;
        pvr_stats_t ps;
        pvr_get_stats(&ps);
        printf("FPS: %.1f  PPS: %.0f polys/sec (%lu tris/frame)  "
               "vtxbuf %luKB (max %luKB)  render %.2fms\n",
               n / secs, (float)prof.tris_drawn / secs,
               (unsigned long)(prof.tris_drawn / prof.samples),
               (unsigned long)(ps.vtx_buffer_used / 1024),
               (unsigned long)(ps.vtx_buffer_used_max / 1024),
               (float)ps.rnd_last_time / 1e6f);
    }
    last_report_ms = now_ms;

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
    /* Vertex buffer is double-buffered by KOS, so this costs 2x in VRAM */
    dc_init((DCInitParams){ .vram_size = 2300 * 1024 });
    dc_draw2d_init();
    dc_debug_init();

    dc_camera_init(&camera);
    dc_player_init(&player);
    player.cam_mode = DC_CAM_THIRD;
    player.move_speed = 1.0f;   /* per frame */

    /* Space: no world, no gravity. Triggers move up/down. */
    player.gravity = 0.0f;
    player.jump_force = 0.0f;
    player.pos = shz_vec3_init(0.0f, 0.0f, 0.0f);

    /* Ship: camera distance follows the ship's size */
    ship_model = dc_model_load("/pc/ship/ship.dms");
    if (ship_model) {
        float r = 0.0f;
        for (uint32_t m = 0; m < ship_model->mesh_count; m++) {
            const DMSMesh* mesh = &ship_model->meshes[m];
            float d = sqrtf(mesh->bound_cx * mesh->bound_cx + mesh->bound_cy * mesh->bound_cy +
                            mesh->bound_cz * mesh->bound_cz) + mesh->bound_radius;
            if (d > r) r = d;
        }
        player.cam_distance = r * 2.0f;
        player.cam_height   = r * 0.8f;
        printf("Ship radius %.2f\n", r);
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

    char fps_str[32];
    snprintf(fps_str, sizeof(fps_str), "FPS: --");

    while (!check_exit()) {
        uint64_t frame_start = perf_cntr_timer_ns();

        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        uint64_t t0;

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

        dc_player_update(&player, &camera, inp, NULL, dt);
        if (inp)
            player.pos.y += (inp->rtrig - inp->ltrig) * player.move_speed;
        dc_camera_update(&camera);
        prof.camera_ns += perf_cntr_timer_ns() - t0;

        /* ---- FPS string ---- */
        snprintf(fps_str, sizeof(fps_str), "FPS: %.1f", dc_fps());

        shz_vec3_t ship_pos = shz_vec3_init(player.pos.x, player.pos.y - player.eye_height, player.pos.z);
        float ship_yaw = -(player.yaw + player.model_yaw_offset);

        /* ---- Rendering (by PVR list: OP -> TR -> PT) ---- */
        t0 = perf_cntr_timer_ns();
        dc_model_reset_stats();

        /* Pass 1: Opaque */
        dc_list_begin(PVR_LIST_OP_POLY);
        dc_model_draw_list_rotated(ship_model, ship_pos, 1.0f, ship_yaw, &camera, PVR_LIST_OP_POLY);

        /* Pass 2: Transparent */
        dc_list_begin(PVR_LIST_TR_POLY);
        dc_model_draw_list_rotated(ship_model, ship_pos, 1.0f, ship_yaw, &camera, PVR_LIST_TR_POLY);

        /* Pass 3: Punch-through + HUD */
        dc_list_begin(PVR_LIST_PT_POLY);
        dc_model_draw_list_rotated(ship_model, ship_pos, 1.0f, ship_yaw, &camera, PVR_LIST_PT_POLY);

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
        prof.tris_drawn          += ms->tris_drawn;

        prof.render_ns += perf_cntr_timer_ns() - t0;

        dc_frame_end();

        prof.frame_total_ns += perf_cntr_timer_ns() - frame_start;
        prof.samples++;

        if (prof.samples >= PROF_INTERVAL)
            prof_print_and_reset();
    }

    dc_model_free(ship_model);
    dc_shutdown();

    return 0;
}
