#ifndef DC_ENGINE_H
#define DC_ENGINE_H

#include <kos.h>
#include <stdint.h>
#include <stdbool.h>

/* ================================================================
 * Init / Shutdown
 * ================================================================ */

typedef struct {
    int   width;          /* 640 default */
    int   height;         /* 480 default */
    float fov;            /* 60.0 default (degrees) */
    float near_z, far_z;  /* 0.1, 100.0 defaults */
    int   vram_size;      /* PVR VRAM pool in bytes, default 1.5 MB */
} DCInitParams;

/* Init PVR, timing, internal state. Zero-init params for defaults. */
void dc_init(DCInitParams params);

/* Free engine resources, pvr_shutdown(). */
void dc_shutdown(void);

/* ================================================================
 * Frame management
 * ================================================================ */

/* Call at top of frame: computes delta time, pvr_scene_begin(). */
void dc_frame_begin(void);

/* Call at end of frame: closes open PVR lists, pvr_scene_finish(). */
void dc_frame_end(void);

/* ================================================================
 * Timing
 * ================================================================ */

/* Seconds since last frame, clamped to 0.1s max. */
float dc_delta_time(void);

/* Rolling average FPS over last 60 frames. */
float dc_fps(void);

/* Milliseconds since boot. */
uint64_t dc_time_ms(void);

/* ================================================================
 * Frame statistics (shown by dc_debug_stats)
 * ================================================================ */

/* Parts of a frame the engine times by itself */
enum { DC_PROF_ANIM, DC_PROF_CAM, DC_PROF_DRAW, DC_PROF_COUNT };

/* Engine use: time a part. Nested calls for the same part count once. */
void dc_prof_begin(int part);
void dc_prof_end(int part);

/* Averages over the last 60 frames */
typedef struct {
    bool     valid;                 /* false until the first 60 frames are in */
    float    frame_ms;              /* dc_frame_begin to the end of dc_frame_end */
    float    anim_us;               /* dc_model_animate */
    float    cam_us;                /* dc_player_update, dc_camera_update */
    float    draw_us;               /* drawing the queue */
    uint32_t meshes_drawn, meshes_culled;
    uint32_t verts_xformed, verts_clipped;
} DCFrameStats;

const DCFrameStats* dc_frame_stats(void);

/* Engine use (dc_debug_stats): print the FPS / polys per second line on the
 * serial log at the end of this averaging interval */
void dc_frame_stats_log(void);

/* ================================================================
 * Display
 * ================================================================ */

/* Set PVR background clear color (0xAARRGGBB). */
void dc_set_clear_color(uint32_t argb);

/* ================================================================
 * PVR list helpers (used by rendering code)
 * ================================================================ */

/* Ensure the given PVR list is open and ready for DR submission.
 * Closes any previously open list if switching. Returns DR state. */
pvr_dr_state_t* dc_list_begin(int pvr_list);

/* Finish the currently open list (if any). */
void dc_list_finish(void);

/* Engine use (the draw queue): draw a scene into a texture (RGB565, not
 * twiddled, w by h) before the screen's scene. Returns false if it cannot. */
bool dc_scene_begin_texture(pvr_ptr_t txr, int w, int h);
void dc_scene_end_texture(void);

/* Size in pixels of what is being drawn into: the screen, or a render target */
void dc_render_size(float* w, float* h);

/* Get current DR state (only valid between dc_list_begin and dc_list_finish). */
pvr_dr_state_t* dc_dr_state(void);

#endif /* DC_ENGINE_H */
