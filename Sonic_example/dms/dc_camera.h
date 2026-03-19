#ifndef DC_CAMERA_H
#define DC_CAMERA_H

#include <kos.h>
#include <sh4zam/shz_sh4zam.h>
#include "dc_input.h"
#include "main.h"

/* ================================================================
 * Camera struct
 * ================================================================ */

typedef struct {
    /* Public — user can read/write directly */
    shz_vec3_t pos;
    float      yaw;        /* radians */
    float      pitch;      /* radians, clamped ±89° */
    float      fov;        /* degrees (default 60) */

    /* Internal — rebuilt by dc_camera_update() */
    shz_mat4x4_t  _pv_matrix   __attribute__((aligned(32)));
    WorldFrustum   _frustum     __attribute__((aligned(32)));
} DCCamera;

/* ================================================================
 * Lifecycle
 * ================================================================ */

/* Initialize camera with defaults (origin, 0 yaw/pitch, 60 FOV). */
void dc_camera_init(DCCamera* cam);

/* Rebuild projection_view matrix and frustum from current pos/yaw/pitch.
 * Must be called once per frame after any camera changes.
 * Leaves the result in the XMTRX register for immediate vertex transforms. */
void dc_camera_update(DCCamera* cam);

/* ================================================================
 * Built-in camera modes
 * ================================================================ */

/* FPS camera: look with stick, move with DPAD/stick2.
 * look_speed: radians per second per unit stick deflection
 * move_speed: units per second */
void dc_camera_fps(DCCamera* cam, const DCInput* inp,
                   float move_speed, float look_speed, float dt);

/* Orbit camera: orbit around a target point.
 * distance: distance from target
 * look_speed: radians per second per unit stick deflection */
void dc_camera_orbit(DCCamera* cam, shz_vec3_t target, float distance,
                     const DCInput* inp, float look_speed, float dt);

/* ================================================================
 * Setters
 * ================================================================ */

void dc_camera_set_position(DCCamera* cam, shz_vec3_t pos);
void dc_camera_look_at(DCCamera* cam, shz_vec3_t target);

/* ================================================================
 * Frustum queries (use after dc_camera_update)
 * ================================================================ */

/* Returns -1 if sphere is fully outside frustum, 0 if inside/intersecting. */
int dc_frustum_cull_sphere(const DCCamera* cam, shz_vec3_t center, float radius);

/* Returns 1 if sphere intersects the near plane. */
int dc_frustum_near_intersect(const DCCamera* cam, shz_vec3_t center, float radius);

/* Get the projection_view matrix (for manual transforms). */
const shz_mat4x4_t* dc_camera_get_pv(const DCCamera* cam);

/* Get the world frustum (for direct access). */
const WorldFrustum* dc_camera_get_frustum(const DCCamera* cam);

#endif /* DC_CAMERA_H */
