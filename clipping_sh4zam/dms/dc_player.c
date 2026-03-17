#include "dc_player.h"
#include <math.h>

void dc_player_init(DCPlayer* p) {
    p->pos = shz_vec3_init(0.0f, 0.0f, 0.0f);
    p->vy = 0.0f;
    p->grounded = false;

    p->eye_height   = 1.6f;
    p->radius       = 0.3f;
    p->gravity      = 0.15f;
    p->ground_snap  = 0.5f;
    p->max_step     = 0.35f;
    p->move_speed   = 0.1f;
    p->sprint_speed = 0.3f;
    p->look_speed_x = 0.02f;
    p->look_speed_y = 0.015f;
    p->pitch_limit  = SHZ_F_PI * 0.45f;
    p->jump_force   = 0.25f;
}

void dc_player_update(DCPlayer* p, DCCamera* cam,
                      const DCInput* inp, ColWorld* col) {
    if (!inp || !inp->connected) return;

    /* ---- Look ---- */
    cam->yaw   += inp->stick_x * p->look_speed_x;
    cam->pitch += inp->stick_y * p->look_speed_y;
    if (cam->pitch >  p->pitch_limit) cam->pitch =  p->pitch_limit;
    if (cam->pitch < -p->pitch_limit) cam->pitch = -p->pitch_limit;

    /* ---- Movement speed ---- */
    float speed = (inp->rtrig > 0.5f) ? p->sprint_speed : p->move_speed;
    if (inp->ltrig > 0.5f) speed *= 2.0f;

    /* ---- Compute forward/right from yaw ---- */
    shz_sincos_t sc = shz_sincosf(cam->yaw);
    float fwd_x   =  sc.sin;
    float fwd_z   =  sc.cos;
    float right_x =  sc.cos;
    float right_z = -sc.sin;

    /* ---- Build desired XZ movement ---- */
    float dx = 0.0f, dz = 0.0f;
    if (dc_input_held(inp, CONT_DPAD_UP))    { dx += fwd_x * speed; dz += fwd_z * speed; }
    if (dc_input_held(inp, CONT_DPAD_DOWN))  { dx -= fwd_x * speed; dz -= fwd_z * speed; }
    if (dc_input_held(inp, CONT_DPAD_LEFT))  { dx -= right_x * speed; dz -= right_z * speed; }
    if (dc_input_held(inp, CONT_DPAD_RIGHT)) { dx += right_x * speed; dz += right_z * speed; }

    /* ---- Desired position ---- */
    shz_vec3_t desired = shz_vec3_init(
        p->pos.x + dx,
        p->pos.y,
        p->pos.z + dz
    );

    /* ---- Collide & slide, substep if moving fast ---- */
    float move_dist_sq = dx * dx + dz * dz;
    shz_vec3_t resolved;

    float half_radius = p->radius * 0.5f;
    float half_radius_sq = half_radius * half_radius;

    if (move_dist_sq > half_radius_sq) {
        float move_dist = shz_sqrtf_fsrra(move_dist_sq);
        int steps = (int)(move_dist / half_radius) + 1;
        if (steps > 8) steps = 8;

        float inv_steps = 1.0f / (float)steps;
        float sub_dx = dx * inv_steps;
        float sub_dz = dz * inv_steps;

        resolved = p->pos;
        for (int s = 0; s < steps; s++) {
            shz_vec3_t sub_target = shz_vec3_init(
                resolved.x + sub_dx,
                resolved.y,
                resolved.z + sub_dz
            );
            resolved = col_move(col, resolved, sub_target, p->radius);
        }
    } else {
        resolved = col_move(col, p->pos, desired, p->radius);
    }

    /* ---- Step height check ---- */
    float max_ground = col ? (resolved.y - col->min_y + 1.0f) : 50.0f;
    ColGroundHit gh = col_ground(col, resolved, max_ground);
    float feet_y = resolved.y - p->eye_height;

    if (gh.hit) {
        float step_up = gh.y - feet_y;

        if (step_up > p->max_step && step_up < p->eye_height) {
            resolved.x = p->pos.x;
            resolved.z = p->pos.z;

            max_ground = col ? (p->pos.y - col->min_y + 1.0f) : 50.0f;
            gh = col_ground(col,
                shz_vec3_init(p->pos.x, p->pos.y, p->pos.z), max_ground);
            feet_y = p->pos.y - p->eye_height;
        }
    }

    /* ---- Gravity / ground snap ---- */
    if (!gh.hit || feet_y > gh.y + p->ground_snap) {
        /* No ground or airborne — freefall */
        p->vy -= p->gravity;
        resolved.y += p->vy;

        if (gh.hit) {
            /* Re-check at new position in case we fell onto ground */
            float max_ground2 = col ? (resolved.y - col->min_y + 1.0f) : 50.0f;
            ColGroundHit gh2 = col_ground(col, resolved, max_ground2);
            if (gh2.hit && resolved.y - p->eye_height < gh2.y) {
                resolved.y = gh2.y + p->eye_height;
                p->vy = 0.0f;
            }
        }
        p->grounded = false;
    } else {
        resolved.y = gh.y + p->eye_height;
        p->vy = 0.0f;
        p->grounded = true;

        if (dc_input_held(inp, CONT_A)) {
            p->vy = p->jump_force;
            p->grounded = false;
        }
    }

    /* ---- Commit ---- */
    p->pos = resolved;
    cam->pos = resolved;
}
