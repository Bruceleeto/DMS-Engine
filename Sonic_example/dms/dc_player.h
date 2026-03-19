#ifndef DC_PLAYER_H
#define DC_PLAYER_H

#include "dc_camera.h"
#include "dc_input.h"
#include "collision.h"

/* ================================================================
 * Camera mode
 * ================================================================ */

typedef enum {
    DC_CAM_FPS,
    DC_CAM_THIRD,
    DC_CAM_NOCLIP
} DCCamMode;

/* ================================================================
 * Player controller (works in any camera mode)
 * ================================================================ */

typedef struct {
    shz_vec3_t pos;
    float      yaw;         /* facing direction (radians) */
    float      vy;          /* vertical velocity */
    bool       grounded;
    DCCamMode  cam_mode;

    /* Tuning (set after dc_player_init for custom values) */
    float height;           /* full character height (1.8) */
    float eye_height;       /* camera height in FPS (1.6) */
    float radius;           /* collision radius (0.3) */
    float gravity;          /* 0.15 */
    float ground_snap;      /* 0.5 */
    float max_step;         /* 0.35 */
    float move_speed;       /* 0.1 */
    float sprint_speed;     /* 0.3 */
    float look_speed;       /* 2.0 */
    float pitch_limit;      /* PI * 0.45 */
    float jump_force;       /* 0.25 */

    /* 3rd-person tuning */
    float cam_distance;     /* distance behind player (4.0) */
    float cam_height;       /* height offset above player (1.5) */
    float cam_pitch;        /* vertical orbit angle */
    float cam_orbit_yaw;    /* horizontal orbit angle (auto-synced on mode switch) */
    float turn_speed;       /* how fast player rotates (rad/s at full stick) (3.0) */
    float model_yaw_offset; /* rest pose compensation (0 = model faces +Z) */
} DCPlayer;

/* Initialize with defaults. */
void dc_player_init(DCPlayer* p);

/* Update player movement, collision, gravity, jump, and camera.
 * Handles FPS, third-person, and noclip modes internally. */
void dc_player_update(DCPlayer* p, DCCamera* cam,
                      const DCInput* inp, ColWorld* col, float dt);

#endif /* DC_PLAYER_H */
