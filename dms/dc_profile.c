#include "dc_profile.h"
#include "dc_engine.h"
#include "dc_input.h"
#include "dc_model.h"
#include "dc_draw2d.h"
#include <dc/perfctr.h>
#include <dc/pvr.h>
#include <arch/timer.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifndef DMS_HOST
#include <malloc.h>
#include <unistd.h>
#endif

#define HISTORY  128   /* frames in the graph */
#define INTERVAL 30    /* frames averaged for the numbers */

/* Two clocks. The zones use the CPU's cycle counter, which is cheap to read
 * but stops while the CPU sleeps; the frame and the wait for the PVR (where
 * it sleeps) use the TMU, which keeps time. What no zone saw (other sleeps)
 * is Idle. */
static const char* const zone_names[DC_ZONE_COUNT + 1] = {
    "Game", "Input", "Wait", "Anim", "Camera", "Physics", "Draw", "Cull", "Lit", "EnvLit",
    "Lights", "Mesh", "Clip", "Headers", "Skin", "Reflect", "Cel", "Shadow", "Volume",
    "Particles", "Coronas", "Text", "Submit", "Audio", "Profiler", "Idle",
};
#define IDLE DC_ZONE_COUNT

/* What it drew, from DCModelStats each frame; the last four are as they
 * were last, not a frame's */
enum {
    C_MESHES, C_CULLED, C_TRIS, C_VERTS, C_CLIPPED, C_SKINNED, C_HEADERS, C_BONES,
    C_VTXFULL, C_VTXBUF, C_TEXFREE, C_HEAP, C_HEAPFREE, C_COUNT
};
static const char* const counter_names[C_COUNT] = {
    "Meshes", "Culled", "Triangles", "Vertices", "ClippedVerts", "SkinnedVerts", "Headers",
    "SkinMatrices", "VtxBufSkips", "VtxBufKB", "TextureFreeKB", "HeapKB", "HeapFreeKB",
};

bool dc_profile_timing;
static int mode = DC_PROFILE_OFF, asked = -1;
static bool held;

/* This frame */
static int      current = DC_ZONE_GAME;
static uint64_t last_ns, frame_start_ns, wait_start_ns, zone_ns[DC_ZONE_COUNT + 1];
static uint32_t reads;

/* Summed over the interval, then the averages shown */
static struct {
    uint32_t frames, reads;
    uint64_t ns, max_ns, wait_ns, gpu_ns, zone[DC_ZONE_COUNT + 1];
    float    counts[C_COUNT];
} sum;
static struct {
    float fps, ms, max_ms, wait_ms, gpu_ms, reads;
    float zone_ms[DC_ZONE_COUNT + 1], counts[C_COUNT];
} shown;
static bool dirty = true;

static struct { float ms, wait; } history[HISTORY];
static uint32_t history_at;

int dc_profile_switch(int zone) {
    uint64_t now = perf_cntr_timer_ns();
    if (current == DC_ZONE_WAIT) zone_ns[DC_ZONE_WAIT] += timer_ns_gettime64() - wait_start_ns;
    else zone_ns[current] += now - last_ns;
    if (zone == DC_ZONE_WAIT) wait_start_ns = timer_ns_gettime64();
    last_ns = now;
    reads++;
    int was = current;
    current = zone;
    return was;
}

void dc_profile_show(int m) { asked = m; }
int  dc_profile_mode(void)  { return asked >= 0 ? asked : mode; }

static void summarise(void) {
    const float n = (float)sum.frames;
    shown.fps     = sum.ns ? n * 1e9f / (float)sum.ns : 0.0f;
    shown.ms      = (float)sum.ns / n * 1e-6f;
    shown.max_ms  = (float)sum.max_ns * 1e-6f;
    shown.wait_ms = (float)sum.wait_ns / n * 1e-6f;
    shown.gpu_ms  = (float)sum.gpu_ns / n * 1e-6f;
    shown.reads   = (float)sum.reads / n;
    for (int z = 0; z <= IDLE; z++) shown.zone_ms[z] = (float)sum.zone[z] / n * 1e-6f;
    for (int c = 0; c < C_VTXBUF; c++) shown.counts[c] = sum.counts[c] / n;

    /* Memory, as it is now */
    pvr_stats_t st;
    pvr_get_stats(&st);
    shown.counts[C_VTXBUF]  = (float)(st.vtx_buffer_used / 1024);
    shown.counts[C_TEXFREE] = (float)(pvr_mem_available() / 1024);
#ifndef DMS_HOST
    struct mallinfo mi = mallinfo();
    uint32_t top = (uint32_t)(uintptr_t)sbrk(0) & 0x1fffffffu;
    shown.counts[C_HEAP]     = (float)(mi.uordblks / 1024);
    shown.counts[C_HEAPFREE] = (float)((mi.fordblks + (0x0d000000u - top)) / 1024);
#endif
    memset(&sum, 0, sizeof sum);
    dirty = true;
}

static void frame_done(uint64_t ns) {
    const DCModelStats* ms = dc_model_get_stats();
    pvr_stats_t st;
    pvr_get_stats(&st);

    uint64_t seen = 0;
    for (int z = 0; z < DC_ZONE_COUNT; z++) seen += zone_ns[z];
    zone_ns[IDLE] = ns > seen ? ns - seen : 0;
    /* Not the CPU's: waiting for the PVR, and asleep anywhere else */
    const uint64_t wait = zone_ns[DC_ZONE_WAIT] + zone_ns[IDLE];

    history[history_at % HISTORY].ms = (float)ns * 1e-6f;
    history[history_at % HISTORY].wait = (float)wait * 1e-6f;
    history_at++;

    sum.frames++;
    sum.reads += reads;
    sum.ns += ns;
    if (ns > sum.max_ns) sum.max_ns = ns;
    sum.wait_ns += wait;
    sum.gpu_ns += st.rnd_last_time;
    for (int z = 0; z <= IDLE; z++) sum.zone[z] += zone_ns[z];
    sum.counts[C_MESHES]  += ms->meshes_drawn;
    sum.counts[C_CULLED]  += ms->meshes_culled;
    sum.counts[C_TRIS]    += ms->tris_drawn;
    sum.counts[C_VERTS]   += ms->verts_xformed;
    sum.counts[C_CLIPPED] += ms->verts_clipped;
    sum.counts[C_SKINNED] += ms->verts_skin;
    sum.counts[C_HEADERS] += ms->hdrs_sent;
    sum.counts[C_BONES]   += ms->skin_bone_loads;
    sum.counts[C_VTXFULL] += ms->meshes_vtxfull;
    if (sum.frames >= INTERVAL) summarise();
}

void dc_profile_frame(void) {
    if (dc_profile_timing) {
        dc_profile_switch(DC_ZONE_GAME);
        if (frame_start_ns) frame_done(timer_ns_gettime64() - frame_start_ns);
    }

    /* Both triggers held, and down (or up) on the d-pad, steps it */
    const DCInput* in = dc_input_get(0);
    bool combo = in && in->connected && in->ltrig > 0.8f && in->rtrig > 0.8f &&
                 (in->buttons & (CONT_DPAD_DOWN | CONT_DPAD_UP));
    if (combo && !held)
        asked = (mode + ((in->buttons & CONT_DPAD_DOWN) ? 1 : 2)) % 3;
    held = combo;
    if (asked >= 0) {
        if (asked != mode) {
            if (mode == DC_PROFILE_OFF) {
                memset(&sum, 0, sizeof sum);
                memset(history, 0, sizeof history);
                memset(&shown, 0, sizeof shown);
            }
            mode = asked;
            dirty = true;
        }
        asked = -1;
    }

    dc_profile_timing = mode != DC_PROFILE_OFF;
    if (!dc_profile_timing) { frame_start_ns = 0; return; }
    memset(zone_ns, 0, sizeof zone_ns);
    reads = 0;
    current = DC_ZONE_GAME;
    last_ns = perf_cntr_timer_ns();
    frame_start_ns = timer_ns_gettime64();
}

/* ================================================================
 * On screen
 * ================================================================ */

#define TEXT     10      /* the engine font, as it is */
#define LINE_H   12
#define PANEL_Z  1000.0f /* 1/w: over the 2D layer (100 and up), the text over the bars */
#define BAR_Z    2000.0f
#define MARK_Z   2500.0f
#define TEXT_Z   3000.0f

#define WHITE  0xffffffffu
#define GREY   0xff9aa0b0u
#define GREEN  0xff40e060u
#define YELLOW 0xffffd040u
#define RED    0xffff5050u
#define WAITC  0xff505868u
#define PANEL  0xb0101010u

/* The graph: two pixels a frame, 50 ms high */
#define GRAPH_X 16.0f
#define GRAPH_Y 44.0f
#define GRAPH_W (2.0f * HISTORY)
#define GRAPH_H 64.0f
#define PX_MS   (GRAPH_H / 50.0f)

/* The text is made again when the numbers are, not every frame */
typedef struct { float x, y; int size; uint32_t argb; char s[40]; } Text;
static Text texts[96];
static int  text_count;
typedef struct { float x0, y0, x1, y1; } Panel;
static Panel panels[3];
static int   panel_count;
static float block_x, block_y, block_right, cursor_y;

static uint32_t rate_colour(float ms) {
    return ms <= 17.5f ? GREEN : ms <= 34.5f ? YELLOW : RED;
}

/* At x, or ending at x when right is set */
__attribute__((format(printf, 5, 6)))
static void put(float x, float y, uint32_t argb, int right_size, const char* fmt, ...) {
    if (text_count >= (int)(sizeof texts / sizeof texts[0])) return;
    Text* t = &texts[text_count++];
    va_list a;
    va_start(a, fmt);
    vsnprintf(t->s, sizeof t->s, fmt, a);
    va_end(a);
    int size = right_size < 0 ? -right_size : right_size ? right_size : TEXT;
    float w = (float)dc_text_width(t->s, size);
    if (right_size < 0) x -= w;
    t->x = (float)(int)x; t->y = (float)(int)y;
    t->size = size;
    t->argb = argb;
    if (x + w > block_right) block_right = x + w;
}
#define RIGHT (-TEXT)

static void begin_block(float x, float y) { block_x = block_right = x; block_y = cursor_y = y; }
static void end_block(void) {
    if (panel_count < 3)
        panels[panel_count++] = (Panel){ block_x - 5, block_y - 4, block_right + 5, cursor_y + 2 };
}

/* As few characters as it takes */
static const char* number(float v, char* s, int n) {
    if (v == (float)(int)v) snprintf(s, n, "%d", (int)v);
    else snprintf(s, n, v < 10 ? "%.2f" : v < 1000 ? "%.1f" : "%.0f", (double)v);
    return s;
}

static void build(void) {
    text_count = panel_count = 0;
    if (mode == DC_PROFILE_OFF) return;

    /* The rate, the frame's split, the graph's marks */
    begin_block(GRAPH_X, 14);
    put(GRAPH_X, 12, rate_colour(shown.ms), 20, "%.1f", (double)shown.fps);
    float detail = GRAPH_X + 64;
    put(detail, 14, WHITE, 0, "fps   %.1f ms   max %.1f", (double)shown.ms, (double)shown.max_ms);
    put(detail, 27, GREY, 0, "cpu %.1f   wait %.1f   gpu %.1f",
        (double)(shown.ms - shown.wait_ms), (double)shown.wait_ms, (double)shown.gpu_ms);
    const float base = GRAPH_Y + GRAPH_H, right = GRAPH_X + GRAPH_W + 4;
    put(right, base - 16.7f * PX_MS - 6, GREEN, 0, "16.7");
    put(right, base - 33.3f * PX_MS - 6, YELLOW, 0, "33.3");
    cursor_y = base + 2;
    end_block();
    if (mode != DC_PROFILE_FULL) return;

    /* The zones, most first */
    const float zx = GRAPH_X, ms_x = zx + 130, pc_x = ms_x + 36;
    begin_block(zx, base + 12);
    put(zx, cursor_y, GREY, 0, "zone");
    put(ms_x, cursor_y, GREY, RIGHT, "ms");
    put(pc_x, cursor_y, GREY, RIGHT, "%%");
    cursor_y += LINE_H;
    int order[DC_ZONE_COUNT + 1], n = 0;
    for (int z = 0; z <= IDLE; z++) {
        if (shown.zone_ms[z] < 0.005f) continue;
        int at = n++;
        while (at && shown.zone_ms[order[at - 1]] < shown.zone_ms[z]) { order[at] = order[at - 1]; at--; }
        order[at] = z;
    }
    const float frame = shown.ms > 0.0f ? shown.ms : 1.0f;
    for (int i = 0; i < n; i++) {
        float ms = shown.zone_ms[order[i]];
        uint32_t c = ms >= frame * 0.15f ? YELLOW : WHITE;
        put(zx, cursor_y, c, 0, "%s", zone_names[order[i]]);
        put(ms_x, cursor_y, c, RIGHT, "%.2f", (double)ms);
        put(pc_x, cursor_y, c, RIGHT, "%.0f", (double)(ms * 100.0f / frame));
        cursor_y += LINE_H;
    }
    put(zx, cursor_y, GREY, 0, "profiler: %.0f clock reads a frame", (double)shown.reads);
    cursor_y += LINE_H;
    end_block();

    /* What it drew, none at 0 */
    const float cx = 400, val_x = cx + 170;
    begin_block(cx, 14);
    put(cx, cursor_y, GREY, 0, "counter");
    put(val_x, cursor_y, GREY, RIGHT, "a frame");
    cursor_y += LINE_H;
    char v[24];
    for (int c = 0; c < C_COUNT; c++) {
        if (shown.counts[c] == 0.0f) continue;
        put(cx, cursor_y, WHITE, 0, "%s", counter_names[c]);
        put(val_x, cursor_y, WHITE, RIGHT, "%s", number(shown.counts[c], v, sizeof v));
        cursor_y += LINE_H;
    }
    end_block();
}

void dc_profile_draw(void) {
    if (mode == DC_PROFILE_OFF) return;
    int z = dc_zone_enter(DC_ZONE_PROFILER);
    if (dirty) { build(); dirty = false; }

    dc_draw2d_now_rects();
    for (int i = 0; i < panel_count; i++)
        dc_draw2d_now_rect(panels[i].x0, panels[i].y0, panels[i].x1, panels[i].y1, PANEL_Z, PANEL);

    /* The frames, oldest first: the CPU's part in the frame's colour, the
     * wait for the PVR over it in grey; then the 60 and 30 fps lines */
    const float base = GRAPH_Y + GRAPH_H;
    for (uint32_t i = 0; i < HISTORY; i++) {
        uint32_t k = (history_at + i) % HISTORY;
        float x = GRAPH_X + 2.0f * (float)i;
        float top = history[k].ms * PX_MS, cpu = (history[k].ms - history[k].wait) * PX_MS;
        if (top > GRAPH_H) top = GRAPH_H;
        if (cpu > top) cpu = top;
        if (cpu > 0.0f) dc_draw2d_now_rect(x, base - cpu, x + 2, base, BAR_Z, rate_colour(history[k].ms));
        if (top > cpu) dc_draw2d_now_rect(x, base - top, x + 2, base - (cpu > 0.0f ? cpu : 0.0f),
                                          BAR_Z, WAITC);
    }
    for (int l = 0; l < 2; l++) {
        float y = base - (l ? 33.3f : 16.7f) * PX_MS;
        dc_draw2d_now_rect(GRAPH_X, y, GRAPH_X + GRAPH_W, y + 1, MARK_Z, 0x90ffffffu);
    }

    dc_draw2d_now_font();
    for (int i = 0; i < text_count; i++)
        dc_draw2d_now_text(texts[i].s, texts[i].x, texts[i].y, texts[i].size, texts[i].argb, TEXT_Z);
    dc_zone_leave(z);
}
