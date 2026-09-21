#include "dc_engine.h"
#include "dc_input.h"
#include "dc_model.h"
#include "dc_draw.h"
#include "dc_draw2d.h"
#include <dc/perfctr.h>
#include <dc/pvr.h>
#include <arch/timer.h>
#include <string.h>
#include <math.h>

/* ================================================================
 * Internal state
 * ================================================================ */

static struct {
    DCInitParams params;

    /* Timing */
    uint64_t last_frame_ms;
    float    delta_time;
    float    frame_secs[60];     /* last 60 frame times, for dc_fps */
    int      fps_idx;
    uint32_t frame_count;

    /* PVR list management */
    pvr_dr_state_t dr_state;
    int  current_list;       /* currently open PVR list, or -1 */
    bool scene_active;
    bool list_opened;        /* a list was opened in the scene being built */
    float render_w, render_h;   /* size of what is being drawn into */

    /* Frame statistics, summed over PROF_INTERVAL frames then averaged */
    uint64_t frame_start_ns;
    uint64_t part_start_ns[DC_PROF_COUNT];
    int      part_depth[DC_PROF_COUNT];
    uint64_t part_ns[DC_PROF_COUNT];
    uint64_t frame_ns;
    uint32_t sum_drawn, sum_culled, sum_xformed, sum_clipped, sum_tris;
    bool     stats_log;         /* dc_debug_stats() was called: print the serial line */
    uint64_t last_report_ms;
    uint32_t samples;
    DCFrameStats stats;
} g_engine;

#define PROF_INTERVAL 60

/* ================================================================
 * Init / Shutdown
 * ================================================================ */

void dc_init(DCInitParams params) {
    memset(&g_engine, 0, sizeof(g_engine));

    /* Apply defaults for zero-init fields */
    if (params.width == 0)     params.width = 640;
    if (params.height == 0)    params.height = 480;
    if (params.fov == 0.0f)    params.fov = 60.0f;
    if (params.near_z == 0.0f) params.near_z = 0.1f;
    if (params.far_z == 0.0f)  params.far_z = 100.0f;
    if (params.vram_size == 0) params.vram_size = (int)(1024 * 1024 * 1.5f);
    g_engine.params = params;

    /* PVR init — OP, PT, TR lists enabled */
    pvr_init(&(pvr_init_params_t){
        { PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32,
          PVR_BINSIZE_0, PVR_BINSIZE_32 },
        params.vram_size, 0, 0, 0, 6
    });

    /* Timing */
    g_engine.last_frame_ms = timer_ms_gettime64();
    g_engine.delta_time = 1.0f / 60.0f;  /* assume 60fps initially */
    g_engine.current_list = -1;
}

void dc_shutdown(void) {
    pvr_shutdown();
}

/* ================================================================
 * Frame management
 * ================================================================ */

void dc_frame_begin(void) {
    g_engine.frame_start_ns = perf_cntr_timer_ns();
    dc_model_reset_stats();

    /* ---- Timing ---- */
    uint64_t now = timer_ms_gettime64();
    g_engine.delta_time = (float)(now - g_engine.last_frame_ms) / 1000.0f;
    if (g_engine.delta_time > 0.1f) g_engine.delta_time = 0.1f; /* clamp */
    if (g_engine.delta_time < 0.0001f) g_engine.delta_time = 0.0001f;
    g_engine.last_frame_ms = now;

    /* Frame times for the FPS rolling average */
    g_engine.frame_secs[g_engine.fps_idx % 60] = g_engine.delta_time;
    g_engine.fps_idx++;

    g_engine.frame_count++;

    /* ---- Input ---- */
    dc_input_poll();

    /* ---- PVR scene ---- */
    pvr_scene_begin();
    g_engine.current_list = -1;
    g_engine.scene_active = true;
    g_engine.list_opened = false;
    g_engine.render_w = SCR_W;
    g_engine.render_h = SCR_H;
}

/* Add this frame to the running sums; every PROF_INTERVAL frames turn them
 * into the averages dc_frame_stats() hands out */
static void stats_frame_done(void) {
    const DCModelStats* ms = dc_model_get_stats();
    g_engine.sum_drawn   += ms->meshes_drawn;
    g_engine.sum_culled  += ms->meshes_culled;
    g_engine.sum_xformed += ms->verts_xformed;
    g_engine.sum_clipped += ms->verts_clipped;
    g_engine.sum_tris    += ms->tris_drawn;
    g_engine.frame_ns += perf_cntr_timer_ns() - g_engine.frame_start_ns;

    if (++g_engine.samples < PROF_INTERVAL) return;

    float n = (float)g_engine.samples;
    DCFrameStats* st = &g_engine.stats;
    st->valid    = true;
    st->frame_ms = (float)g_engine.frame_ns / n / 1000000.0f;
    st->anim_us  = (float)g_engine.part_ns[DC_PROF_ANIM] / n / 1000.0f;
    st->cam_us   = (float)g_engine.part_ns[DC_PROF_CAM]  / n / 1000.0f;
    st->draw_us  = (float)g_engine.part_ns[DC_PROF_DRAW] / n / 1000.0f;
    st->meshes_drawn  = g_engine.sum_drawn   / g_engine.samples;
    st->meshes_culled = g_engine.sum_culled  / g_engine.samples;
    st->verts_xformed = g_engine.sum_xformed / g_engine.samples;
    st->verts_clipped = g_engine.sum_clipped / g_engine.samples;

    /* Wall-clock interval since the last report, so vsync waits are counted */
    uint64_t now_ms = timer_ms_gettime64();
    if (g_engine.stats_log && g_engine.last_report_ms) {
        float secs = (float)(now_ms - g_engine.last_report_ms) / 1000.0f;
        pvr_stats_t ps;
        pvr_get_stats(&ps);
        printf("FPS: %.1f  PPS: %.0f polys/sec (%lu tris/frame)  "
               "vtxbuf %luKB (max %luKB)  render %.2fms\n",
               n / secs, (float)g_engine.sum_tris / secs,
               (unsigned long)(g_engine.sum_tris / g_engine.samples),
               (unsigned long)(ps.vtx_buffer_used / 1024),
               (unsigned long)(ps.vtx_buffer_used_max / 1024),
               (float)ps.rnd_last_time / 1e6f);
    }
    g_engine.last_report_ms = now_ms;
    g_engine.stats_log = false;
    g_engine.sum_tris = 0;

    memset(g_engine.part_ns, 0, sizeof(g_engine.part_ns));
    g_engine.frame_ns = 0;
    g_engine.sum_drawn = g_engine.sum_culled = 0;
    g_engine.sum_xformed = g_engine.sum_clipped = 0;
    g_engine.samples = 0;
}

void dc_prof_begin(int part) {
    if (g_engine.part_depth[part]++ == 0)
        g_engine.part_start_ns[part] = perf_cntr_timer_ns();
}

void dc_prof_end(int part) {
    if (--g_engine.part_depth[part] == 0)
        g_engine.part_ns[part] += perf_cntr_timer_ns() - g_engine.part_start_ns[part];
}

void dc_frame_stats_log(void) {
    g_engine.stats_log = true;
}

const DCFrameStats* dc_frame_stats(void) {
    return &g_engine.stats;
}

void dc_frame_end(void) {
    /* Everything queued with dc_draw*, then the text on top */
    dc_prof_begin(DC_PROF_DRAW);
    dc_draw_flush();
    dc_draw2d_flush();
    dc_prof_end(DC_PROF_DRAW);

    /* Close any open list */
    if (g_engine.current_list >= 0) {
        pvr_list_finish();
        g_engine.current_list = -1;
    }

    pvr_scene_finish();
    g_engine.scene_active = false;

    stats_frame_done();
}

/* ================================================================
 * Timing
 * ================================================================ */

float dc_delta_time(void) {
    return g_engine.delta_time;
}

float dc_fps(void) {
    int count = g_engine.fps_idx < 60 ? g_engine.fps_idx : 60;
    if (count == 0) return 0.0f;

    /* Frames over total time. Averaging each frame's 1/time reads high when
     * frame times are uneven (10ms + 23ms is 60fps, not 71). */
    float secs = 0.0f;
    for (int i = 0; i < count; i++)
        secs += g_engine.frame_secs[i];
    return (float)count / secs;
}

uint64_t dc_time_ms(void) {
    return timer_ms_gettime64();
}

uint32_t dc_frame_count(void) {
    return g_engine.frame_count;
}

/* ================================================================
 * Display
 * ================================================================ */

void dc_set_clear_color(uint32_t argb) {
    float r = ((argb >> 16) & 0xFF) / 255.0f;
    float g = ((argb >>  8) & 0xFF) / 255.0f;
    float b = ( argb        & 0xFF) / 255.0f;
    pvr_set_bg_color(r, g, b);
}

/* ================================================================
 * PVR list helpers
 * ================================================================ */

pvr_dr_state_t* dc_list_begin(int pvr_list) {
    /* If same list is already open, just return DR state */
    if (g_engine.current_list == pvr_list)
        return &g_engine.dr_state;

    /* Close previous list if one is open */
    if (g_engine.current_list >= 0)
        pvr_list_finish();

    /* Open new list */
    pvr_list_begin(pvr_list);
    g_engine.list_opened = true;
    g_engine.current_list = pvr_list;

    return &g_engine.dr_state;
}

bool dc_scene_begin_texture(pvr_ptr_t txr, int w, int h) {
    /* The screen scene was begun by dc_frame_begin(). It can only be put off
     * while nothing has gone into it */
    if (g_engine.list_opened) {
        static bool warned;
        if (!warned) {
            printf("DMS: a render target cannot be drawn after drawing to the screen "
                   "by hand in the same frame\n");
            warned = true;
        }
        return false;
    }
    if (pvr_scene_begin_rtt(txr, w, h, w) < 0) return false;
    g_engine.current_list = -1;
    g_engine.render_w = (float)w;
    g_engine.render_h = (float)h;
    return true;
}

void dc_scene_end_texture(void) {
    dc_list_finish();
    pvr_scene_finish();

    /* Back to the screen. No wait: KOS renders the texture before the scene
     * that uses it */
    pvr_scene_begin();
    g_engine.current_list = -1;
    g_engine.list_opened = false;
    g_engine.render_w = SCR_W;
    g_engine.render_h = SCR_H;
}

void dc_render_size(float* w, float* h) {
    *w = g_engine.render_w;
    *h = g_engine.render_h;
}

void dc_list_finish(void) {
    if (g_engine.current_list >= 0) {
        pvr_list_finish();
        g_engine.current_list = -1;
    }
}

pvr_dr_state_t* dc_dr_state(void) {
    return &g_engine.dr_state;
}
