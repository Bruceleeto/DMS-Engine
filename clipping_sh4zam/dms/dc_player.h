#ifndef DC_PLAYER_H
#define DC_PLAYER_H

#include "dc_camera.h"
#include "dc_input.h"
#include "collision.h"

/* ================================================================
 * First-person player controller with collision
 * ================================================================ */

typedef struct {
    shz_vec3_t pos;
    float      vy;          /* vertical velocity */
    bool       grounded;

    /* Tuning (set after dc_player_init for custom values) */
    float eye_height;       /* 1.6 */
    float radius;           /* 0.3 */
    float gravity;          /* 0.15 */
    float ground_snap;      /* 0.5 */
    float max_step;         /* 0.35 */
    float move_speed;       /* 0.1 */
    float sprint_speed;     /* 0.3 */
    float look_speed_x;    /* 0.02 */
    float look_speed_y;    /* 0.015 */
    float pitch_limit;      /* PI * 0.45 */
    float jump_force;       /* 0.25 */
} DCPlayer;

/* Initialize with defaults. */
void dc_player_init(DCPlayer* p);

/* Update look, movement, collision, gravity, jump.
 * Writes resulting position to both p->pos and cam->pos. */
void dc_player_update(DCPlayer* p, DCCamera* cam,
                      const DCInput* inp, ColWorld* col);

#endif /* DC_PLAYER_H */
