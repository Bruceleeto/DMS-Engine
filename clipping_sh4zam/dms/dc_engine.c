#include "dc_engine.h"
#include "dc_input.h"
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
    float    fps_samples[60];
    int      fps_idx;
    uint32_t frame_count;

    /* PVR list management */
    pvr_dr_state_t dr_state;
    int  current_list;       /* currently open PVR list, or -1 */
    bool scene_active;
} g_engine;

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
    /* ---- Timing ---- */
    uint64_t now = timer_ms_gettime64();
    g_engine.delta_time = (float)(now - g_engine.last_frame_ms) / 1000.0f;
    if (g_engine.delta_time > 0.1f) g_engine.delta_time = 0.1f; /* clamp */
    if (g_engine.delta_time < 0.0001f) g_engine.delta_time = 0.0001f;
    g_engine.last_frame_ms = now;

    /* FPS rolling average */
    float instant_fps = 1.0f / g_engine.delta_time;
    g_engine.fps_samples[g_engine.fps_idx % 60] = instant_fps;
    g_engine.fps_idx++;

    g_engine.frame_count++;

    /* ---- Input ---- */
    dc_input_poll();

    /* ---- PVR scene ---- */
    pvr_scene_begin();
    g_engine.current_list = -1;
    g_engine.scene_active = true;
}

void dc_frame_end(void) {
    /* Close any open list */
    if (g_engine.current_list >= 0) {
        pvr_list_finish();
        g_engine.current_list = -1;
    }

    pvr_scene_finish();
    g_engine.scene_active = false;
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

    float sum = 0.0f;
    for (int i = 0; i < count; i++)
        sum += g_engine.fps_samples[i];
    return sum / (float)count;
}

uint64_t dc_time_ms(void) {
    return timer_ms_gettime64();
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
    pvr_dr_init(&g_engine.dr_state);
    g_engine.current_list = pvr_list;

    return &g_engine.dr_state;
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
