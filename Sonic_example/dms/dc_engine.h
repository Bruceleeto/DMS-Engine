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

/* Get current DR state (only valid between dc_list_begin and dc_list_finish). */
pvr_dr_state_t* dc_dr_state(void);

#endif /* DC_ENGINE_H */
