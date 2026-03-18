#include "dc_player.h"
#include <math.h>

#define DC_PITCH_LIMIT_DEFAULT (SHZ_F_PI * 0.45f)

void dc_player_init(DCPlayer* p) {
    p->pos = shz_vec3_init(0.0f, 0.0f, 0.0f);
    p->yaw = 0.0f;
    p->vy = 0.0f;
    p->grounded = false;
    p->cam_mode = DC_CAM_FPS;

    p->height       = 1.8f;
    p->eye_height   = 1.6f;
    p->radius       = 0.3f;
    p->gravity      = 0.15f;
    p->ground_snap  = 0.5f;
    p->max_step     = 0.35f;
    p->move_speed   = 1.0f;
    p->sprint_speed = 0.3f;
    p->look_speed   = 2.0f;
    p->pitch_limit  = DC_PITCH_LIMIT_DEFAULT;
    p->jump_force   = 0.25f;

    p->cam_distance  = 5.0f;
    p->cam_height    = 2.0f;
    p->cam_pitch     = 0.15f;
    p->cam_orbit_yaw    = 0.0f;
    p->turn_speed       = 3.0f;
    p->model_yaw_offset = 0.0f;
}

/* ================================================================
 * Physics: movement, collision, gravity (shared by FPS and 3rd)
 * ================================================================ */

static void player_physics(DCPlayer* p, float dx, float dz, ColWorld* col) {
    /* Desired position */
    shz_vec3_t desired = shz_vec3_init(
        p->pos.x + dx,
        p->pos.y,
        p->pos.z + dz
    );

    /* Collide & slide, substep if moving fast */
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

    /* Step height check */
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

    /* Gravity / ground snap */
    if (!gh.hit || feet_y > gh.y + p->ground_snap) {
        p->vy -= p->gravity;
        resolved.y += p->vy;

        if (gh.hit) {
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
    }

    p->pos = resolved;
}

/* ================================================================
 * Angle helpers
 * ================================================================ */

static float wrap_angle(float a) {
    while (a >  SHZ_F_PI) a -= 2.0f * SHZ_F_PI;
    while (a < -SHZ_F_PI) a += 2.0f * SHZ_F_PI;
    return a;
}

/* Used by third-person camera follow */
static float lerp_angle(float from, float to, float t) __attribute__((unused));
static float lerp_angle(float from, float to, float t) {
    float diff = wrap_angle(to - from);
    return from + diff * t;
}

/* ================================================================
 * FPS mode
 * ================================================================ */

static void update_fps(DCPlayer* p, DCCamera* cam,
                       const DCInput* inp, ColWorld* col, float dt) {
    /* Look: left stick */
    cam->yaw   += inp->stick_x * p->look_speed * dt;
    cam->pitch += inp->stick_y * p->look_speed * 0.75f * dt;
    if (cam->pitch >  p->pitch_limit) cam->pitch =  p->pitch_limit;
    if (cam->pitch < -p->pitch_limit) cam->pitch = -p->pitch_limit;

    /* Player faces where camera looks */
    p->yaw = cam->yaw;

    /* Movement speed */
    float speed = (inp->rtrig > 0.5f) ? p->sprint_speed : p->move_speed;
    if (inp->ltrig > 0.5f) speed *= 2.0f;

    /* Forward/right from camera yaw */
    shz_sincos_t sc = shz_sincosf(cam->yaw);
    float fwd_x   =  sc.sin;
    float fwd_z   =  sc.cos;
    float right_x =  sc.cos;
    float right_z = -sc.sin;

    /* Build desired XZ movement from DPAD */
    float dx = 0.0f, dz = 0.0f;
    if (dc_input_held(inp, CONT_DPAD_UP))    { dx += fwd_x * speed; dz += fwd_z * speed; }
    if (dc_input_held(inp, CONT_DPAD_DOWN))  { dx -= fwd_x * speed; dz -= fwd_z * speed; }
    if (dc_input_held(inp, CONT_DPAD_LEFT))  { dx -= right_x * speed; dz -= right_z * speed; }
    if (dc_input_held(inp, CONT_DPAD_RIGHT)) { dx += right_x * speed; dz += right_z * speed; }

    /* Jump */
    if (p->grounded && dc_input_held(inp, CONT_A)) {
        p->vy = p->jump_force;
        p->grounded = false;
    }

    player_physics(p, dx, dz, col);

    /* Camera at eye position */
    cam->pos = p->pos;
}

/* ================================================================
 * Third-person mode
 * ================================================================ */

static void update_third(DCPlayer* p, DCCamera* cam,
                         const DCInput* inp, ColWorld* col, float dt) {
    /*
     * Tank controls + follow camera (Three.js approach).
     * Input is PLAYER-relative, never camera-relative.
     * Camera is a terminal output — nothing reads cam->yaw.
     *
     * stick_x → rotate player
     * stick_y → move along player's own forward
     * camera  → lerps behind player (no feedback)
     */

    /* ---- 1. Turn: stick X rotates the player ---- */
    p->yaw += inp->stick_x * p->turn_speed * dt;
    p->yaw = wrap_angle(p->yaw);

    /* ---- 2. Move: stick Y along player's forward ---- */
    float speed = p->move_speed;
    float forward_input = -inp->stick_y;  /* KOS: forward = negative stick_y */

    shz_sincos_t sc = shz_sincosf(p->yaw);
    float fwd_x = sc.sin;
    float fwd_z = sc.cos;

    float dx = fwd_x * forward_input * speed;
    float dz = fwd_z * forward_input * speed;

    /* DPAD: player-relative */
    if (dc_input_held(inp, CONT_DPAD_UP))    { dx += fwd_x * speed; dz += fwd_z * speed; }
    if (dc_input_held(inp, CONT_DPAD_DOWN))  { dx -= fwd_x * speed; dz -= fwd_z * speed; }
    if (dc_input_held(inp, CONT_DPAD_LEFT))  { p->yaw -= p->turn_speed * dt; }
    if (dc_input_held(inp, CONT_DPAD_RIGHT)) { p->yaw += p->turn_speed * dt; }

    /* ---- 3. Jump ---- */
    if (p->grounded && dc_input_pressed(inp, CONT_A)) {
        p->vy = p->jump_force;
        p->grounded = false;
    }

    /* ---- 4. Physics ---- */
    player_physics(p, dx, dz, col);

    /* ---- 5. Camera: ideal position behind player ---- */
    float feet_y = p->pos.y - p->eye_height;
    float ideal_x = p->pos.x - fwd_x * p->cam_distance;
    float ideal_y = feet_y + p->cam_height;
    float ideal_z = p->pos.z - fwd_z * p->cam_distance;

    /* ---- 6. Smooth lerp toward ideal ---- */
    float t = 5.0f * dt;
    if (t > 1.0f) t = 1.0f;

    cam->pos.x += (ideal_x - cam->pos.x) * t;
    cam->pos.y += (ideal_y - cam->pos.y) * t;
    cam->pos.z += (ideal_z - cam->pos.z) * t;

    /* ---- 7. Camera looks at player (terminal output) ---- */
    shz_vec3_t look_at = shz_vec3_init(
        p->pos.x,
        feet_y + p->height * 0.5f,
        p->pos.z
    );
    dc_camera_look_at(cam, look_at);
}

/* ================================================================
 * Noclip mode
 * ================================================================ */

static void update_noclip(DCPlayer* p, DCCamera* cam,
                          const DCInput* inp, float dt) {
    cam->yaw   += inp->stick_x * p->look_speed * dt;
    cam->pitch += inp->stick_y * p->look_speed * 0.75f * dt;
    if (cam->pitch >  p->pitch_limit) cam->pitch =  p->pitch_limit;
    if (cam->pitch < -p->pitch_limit) cam->pitch = -p->pitch_limit;

    shz_sincos_t sc = shz_sincosf(cam->yaw);
    float fwd_x   =  sc.sin;
    float fwd_z   =  sc.cos;
    float right_x =  sc.cos;
    float right_z = -sc.sin;

    float speed = 20.0f;
    float mx = 0.0f, mz = 0.0f;

    if (inp->buttons & CONT_DPAD_UP)    { mx += fwd_x;   mz += fwd_z; }
    if (inp->buttons & CONT_DPAD_DOWN)  { mx -= fwd_x;   mz -= fwd_z; }
    if (inp->buttons & CONT_DPAD_LEFT)  { mx -= right_x; mz -= right_z; }
    if (inp->buttons & CONT_DPAD_RIGHT) { mx += right_x; mz += right_z; }

    float my = inp->rtrig - inp->ltrig;

    cam->pos.x += mx * speed * dt;
    cam->pos.y += my * speed * dt;
    cam->pos.z += mz * speed * dt;

    p->yaw = cam->yaw;
}

/* ================================================================
 * Public update
 * ================================================================ */

void dc_player_update(DCPlayer* p, DCCamera* cam,
                      const DCInput* inp, ColWorld* col, float dt) {
    if (!inp || !inp->connected) return;

    switch (p->cam_mode) {
        case DC_CAM_FPS:
            update_fps(p, cam, inp, col, dt);
            break;
        case DC_CAM_THIRD:
            update_third(p, cam, inp, col, dt);
            break;
        case DC_CAM_NOCLIP:
            update_noclip(p, cam, inp, dt);
            break;
    }
}
