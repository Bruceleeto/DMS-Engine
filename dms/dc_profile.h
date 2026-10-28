#ifndef DC_PROFILE_H
#define DC_PROFILE_H

/* The engine's profiler, on screen. Hold both triggers on controller 1 and
 * press down on the d-pad to step it (up steps back):
 *   full  the frame rate, a graph of the last frames (CPU, then the wait for
 *         the PVR, stacked), where the engine's time went and what it drew
 *   rate  the frame rate and the graph alone
 *   off
 * It times DMS itself. The time a frame spends outside the engine is Game:
 * a game profiling its own code does that itself. The zones are exclusive
 * (a zone's time leaves out the zones opened inside it), so they add up to
 * the frame. Numbers are averages over the last half second. The clock is
 * read only while it is on. */

#include <stdbool.h>

enum { DC_PROFILE_OFF, DC_PROFILE_FULL, DC_PROFILE_RATE };

/* Show it from code, as if stepped to (from the next frame) */
void dc_profile_show(int mode);
int  dc_profile_mode(void);

/* ================================================================
 * Engine use
 * ================================================================ */

typedef enum {
    DC_ZONE_GAME,        /* outside the engine */
    DC_ZONE_INPUT,       /* dc_input_poll */
    DC_ZONE_WAIT,        /* pvr_scene_begin: the PVR still on the last frame */
    DC_ZONE_ANIM,        /* dc_model_animate */
    DC_ZONE_CAMERA,      /* dc_camera_update, dc_player_update */
    DC_ZONE_PHYSICS,     /* pp_physics_step */
    DC_ZONE_DRAW,        /* dc_draw_flush, less what follows */
    DC_ZONE_CULL,        /* mesh sphere tests */
    DC_ZONE_LIT,         /* lit static meshes, the sun loop */
    DC_ZONE_ENV,         /* lit with the environment image */
    DC_ZONE_LIGHTS,      /* lit by point lights */
    DC_ZONE_MESH,        /* unlit, flat, tinted and rim meshes */
    DC_ZONE_CLIP,        /* meshes crossing the near plane */
    DC_ZONE_HEADERS,     /* polygon headers, matrix reloads */
    DC_ZONE_SKIN,        /* skinned meshes */
    DC_ZONE_REFLECT,     /* reflections */
    DC_ZONE_CEL,         /* cel outlines */
    DC_ZONE_SHADOW,      /* shadows */
    DC_ZONE_VOLUME,      /* modifier volumes */
    DC_ZONE_PARTICLES,   /* dc_particles_draw */
    DC_ZONE_CORONAS,     /* dc_corona_flush */
    DC_ZONE_TEXT,        /* the 2D layer */
    DC_ZONE_SUBMIT,      /* closing lists, pvr_scene_finish */
    DC_ZONE_AUDIO,       /* dc_audio_update */
    DC_ZONE_PROFILER,    /* this */
    DC_ZONE_COUNT
} DCZone;

extern bool dc_profile_timing;
int dc_profile_switch(int zone);

/* Time what follows as zone, until the matching leave:
 *   int z = dc_zone_enter(DC_ZONE_CULL); ... dc_zone_leave(z); */
static inline int dc_zone_enter(int zone) {
    return dc_profile_timing ? dc_profile_switch(zone) : zone;
}
static inline void dc_zone_leave(int previous) {
    if (dc_profile_timing) dc_profile_switch(previous);
}

/* A frame ends (dc_frame_begin, before anything else) */
void dc_profile_frame(void);
/* Into the open translucent list, after the 2D layer (dc_draw_flush) */
void dc_profile_draw(void);

#endif /* DC_PROFILE_H */
