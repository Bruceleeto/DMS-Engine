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

/* dc_audio.c is only linked by games that want sound */
extern void dc_audio_update(void) __attribute__((weak));

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
    int  clip[4];            /* the tiles the open list is clipped to (dc_list_clip); -1: none sent */

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

    /* One extra list costs ~525KB of texture RAM (128 bytes x 300 tiles x the
     * x7 overflow, doubled), and at vram_size 2300KB it overruns the 4MB half,
     * so TR_MOD is asked for. OP_MOD stays off: volumes need the translucent
     * list to blend with what is behind them, so nothing writes to it. */
    pvr_init(&(pvr_init_params_t){
        { PVR_BINSIZE_32,                                    /* OP_POLY */
          PVR_BINSIZE_0,                                     /* OP_MOD, unused */
          PVR_BINSIZE_32,                                    /* TR_POLY */
          params.volumes ? PVR_BINSIZE_32 : PVR_BINSIZE_0,   /* TR_MOD */
          PVR_BINSIZE_32 },                                  /* PT_POLY */
        params.vram_size, 0, 0, 0, 6
    });

    dc_set_fog_off();

    /* Timing */
    g_engine.last_frame_ms = timer_ms_gettime64();
    g_engine.delta_time = 1.0f / 60.0f;  /* assume 60fps initially */
    g_engine.current_list = -1;
}

bool dc_volumes_enabled(void) {
    return g_engine.params.volumes;
}

void dc_shutdown(void) {
    pvr_shutdown();
}

/* ================================================================
 * Frame management
 * ================================================================ */

static void blends_step(float dt);
static void list_close(void);

uint32_t dc_clip_cmd;

void dc_clip_scene(bool on) {
    dc_clip_cmd = on ? (uint32_t)PVR_USERCLIP_INSIDE << 16 : 0;
}

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

    blends_step(g_engine.delta_time);

    /* ---- Input ---- */
    dc_input_poll();

    /* ---- PVR scene ---- */
    pvr_scene_begin();
    dc_model_frame_begin();   /* buffer is wound back, so the guard resets here */
    g_engine.current_list = -1;
    dc_clip_cmd = 0;
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
    /* Everything queued with dc_draw*, the 2D layer inside it */
    dc_prof_begin(DC_PROF_DRAW);
    dc_draw_flush();
    dc_prof_end(DC_PROF_DRAW);

    /* Close any open list */
    if (g_engine.current_list >= 0) list_close();

    pvr_scene_finish();
    g_engine.scene_active = false;

    /* Keep music streaming when the game linked dc_audio */
    if (dc_audio_update) dc_audio_update();

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

/* What the fog and the clear colour are now, and what they are on their way
 * to. Stepped each frame from dc_frame_begin(). */
static uint32_t g_clear_now = 0xFF000000u, g_clear_from, g_clear_to;
static float    g_clear_t, g_clear_secs;
static bool     g_clear_blending;
static bool     g_fog_on, g_fog_blending;
static uint32_t g_fog_now, g_fog_from, g_fog_to;
static float    g_fog_near, g_fog_far, g_fog_near_from, g_fog_far_from, g_fog_near_to, g_fog_far_to;
static float    g_fog_t, g_fog_secs;

static uint32_t rgb_lerp(uint32_t a, uint32_t b, float t) {
    uint32_t c = 0;
    for (int sh = 0; sh <= 24; sh += 8) {
        float x = (float)((a >> sh) & 0xFF), y = (float)((b >> sh) & 0xFF);
        c |= (uint32_t)(x + (y - x) * t + 0.5f) << sh;
    }
    return c;
}

static void clear_apply(uint32_t argb) {
    float r = ((argb >> 16) & 0xFF) / 255.0f;
    float g = ((argb >>  8) & 0xFF) / 255.0f;
    float b = ( argb        & 0xFF) / 255.0f;
    pvr_set_bg_color(r, g, b);
    g_clear_now = argb;
}

void dc_set_clear_color(uint32_t argb) {
    g_clear_blending = false;
    clear_apply(argb);
}

void dc_set_clear_color_over(uint32_t argb, float seconds) {
    if (seconds <= 0.0f) { dc_set_clear_color(argb); return; }
    g_clear_from = g_clear_now;
    g_clear_to = argb;
    g_clear_t = 0.0f;
    g_clear_secs = seconds;
    g_clear_blending = true;
}

static void fog_apply(uint32_t rgb, float near, float far) {
    float r = ((rgb >> 16) & 0xFF) / 255.0f;
    float g = ((rgb >>  8) & 0xFF) / 255.0f;
    float b = ( rgb        & 0xFF) / 255.0f;
    if (far <= near) far = near + 1.0f;
    pvr_fog_table_color(1.0f, r, g, b);
    pvr_fog_table_linear(near, far);
    g_fog_on = true;
    g_fog_now = rgb & 0xFFFFFFu; g_fog_near = near; g_fog_far = far;
}

void dc_set_fog_over(uint32_t rgb, float near, float far, float seconds) {
    if (seconds <= 0.0f || !g_fog_on) { dc_set_fog(rgb, near, far); return; }
    g_fog_from = g_fog_now; g_fog_near_from = g_fog_near; g_fog_far_from = g_fog_far;
    g_fog_to = rgb & 0xFFFFFFu; g_fog_near_to = near; g_fog_far_to = far;
    g_fog_t = 0.0f;
    g_fog_secs = seconds;
    g_fog_blending = true;
}

static void blends_step(float dt) {
    if (g_clear_blending) {
        g_clear_t += dt / g_clear_secs;
        float t = g_clear_t >= 1.0f ? 1.0f : g_clear_t;
        clear_apply(rgb_lerp(g_clear_from, g_clear_to, t));
        if (t >= 1.0f) g_clear_blending = false;
    }
    if (g_fog_blending) {
        g_fog_t += dt / g_fog_secs;
        float t = g_fog_t >= 1.0f ? 1.0f : g_fog_t;
        fog_apply(rgb_lerp(g_fog_from, g_fog_to, t),
                  g_fog_near_from + (g_fog_near_to - g_fog_near_from) * t,
                  g_fog_far_from + (g_fog_far_to - g_fog_far_from) * t);
        if (t >= 1.0f) g_fog_blending = false;
    }
    dc_draw_step_blends(dt);
}

/* Model headers always ask for table fog; a zero table is "off". Vertex fog
 * and the 2D layer stay untouched. */
void dc_set_fog(uint32_t rgb, float near, float far) {
    g_fog_blending = false;
    fog_apply(rgb, near, far);
}

void dc_set_fog_off(void) {
    g_fog_blending = false;
    g_fog_on = false;
    float table[129];
    for (int i = 0; i < 129; i++) table[i] = 0.0f;
    pvr_fog_far_depth(1.0f);
    pvr_fog_table_custom(table);
}

/* ================================================================
 * PVR list helpers
 * ================================================================ */

/* Modifier volume lists take no polygon headers, so no clip and no dummy */
static bool list_is_volumes(int pvr_list) {
    return pvr_list == PVR_LIST_OP_MOD || pvr_list == PVR_LIST_TR_MOD;
}

/* Holly bug 18 (Dreamcast/Dev.Box System Architecture, CORE & TA bugs): a
 * change of user tile clip, the area from the clip object or the mode from a
 * polygon header, lands one polygon early, on the polygon before it. The
 * chip revision that is in every console has it, and on it the TA locked up
 * when the first polygon of a frame was clipped. The workaround is the one
 * Katana and Bloom use: a dummy polygon before every switch, so the early
 * switch hits that. It carries the mode of the polygons before it, is never
 * drawn (depth test never passes) and costs 128 bytes. Closing a list resets
 * the clip too, so one goes before every close. */
static void clip_dummy(int mode) {
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_col(&cxt, g_engine.current_list);
    cxt.gen.culling = PVR_CULLING_SMALL;
    cxt.gen.clip_mode = mode;
    cxt.depth.comparison = PVR_DEPTHCMP_NEVER;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_hdr_t* hdr = (pvr_poly_hdr_t*)pvr_dr_target(g_engine.dr_state);
    pvr_poly_compile(hdr, &cxt);
    pvr_dr_commit(hdr);
    for (int i = 0; i < 3; i++) {
        pvr_vertex_t* v = (pvr_vertex_t*)pvr_dr_target(g_engine.dr_state);
        v->flags = (i == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v->x = 0.0f; v->y = 0.0f; v->z = 1.0f;
        v->u = 0.0f; v->v = 0.0f;
        v->argb = 0; v->oargb = 0;
        pvr_dr_commit(v);
    }
}

/* Close the open list: the dummy first, as the close resets the clip */
static void list_close(void) {
    if (dc_clip_cmd && !list_is_volumes(g_engine.current_list)) clip_dummy(PVR_USERCLIP_INSIDE);
    pvr_list_finish();
    g_engine.current_list = -1;
}

pvr_dr_state_t* dc_list_begin(int pvr_list) {
    /* If same list is already open, just return DR state */
    if (g_engine.current_list == pvr_list)
        return &g_engine.dr_state;

    /* Close previous list if one is open */
    if (g_engine.current_list >= 0)
        list_close();

    /* Open new list */
    pvr_list_begin(pvr_list);
    g_engine.list_opened = true;
    g_engine.current_list = pvr_list;
    g_engine.clip[0] = -1;
    if (!dc_clip_cmd || list_is_volumes(pvr_list)) return &g_engine.dr_state;

    /* A fresh list clips nothing; the first clipped header is a switch */
    clip_dummy(PVR_USERCLIP_DISABLE);
    dc_list_clip(0.0f, 0.0f, g_engine.render_w, g_engine.render_h);

    return &g_engine.dr_state;
}

/* The TA's user tile clip: one 32 byte object, the tile range inclusive.
 * It holds for the polygons after it in the list. */
void dc_list_clip(float x, float y, float w, float h) {
    if (!dc_clip_cmd || g_engine.current_list < 0 || list_is_volumes(g_engine.current_list)) return;
    int tw = (int)(g_engine.render_w / 32.0f), th = (int)(g_engine.render_h / 32.0f);
    int c[4] = { (int)(x / 32.0f), (int)(y / 32.0f),
                 (int)((x + w - 1.0f) / 32.0f), (int)((y + h - 1.0f) / 32.0f) };
    if (c[0] < 0) c[0] = 0;
    if (c[1] < 0) c[1] = 0;
    if (c[2] > tw - 1) c[2] = tw - 1;
    if (c[3] > th - 1) c[3] = th - 1;
    if (c[0] == g_engine.clip[0] && c[1] == g_engine.clip[1] && c[2] == g_engine.clip[2] && c[3] == g_engine.clip[3]) return;
    const bool first = g_engine.clip[0] < 0;   /* the list-open dummy just went */
    memcpy(g_engine.clip, c, sizeof(c));
    if (!first) clip_dummy(PVR_USERCLIP_INSIDE);
    uint32_t* o = (uint32_t*)pvr_dr_target(g_engine.dr_state);
    o[0] = PVR_CMD_USERCLIP;
    o[1] = o[2] = o[3] = 0;
    o[4] = (uint32_t)c[0]; o[5] = (uint32_t)c[1]; o[6] = (uint32_t)c[2]; o[7] = (uint32_t)c[3];
    pvr_dr_commit(o);
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
    dc_model_frame_begin();   /* a new scene: buffer wound back, budget back */
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
    dc_model_frame_begin();   /* likewise */
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
    if (g_engine.current_list >= 0) list_close();
}

pvr_dr_state_t* dc_dr_state(void) {
    return &g_engine.dr_state;
}
