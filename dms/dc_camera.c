#include "dc_camera.h"
#include <math.h>

#define DC_PITCH_LIMIT (SHZ_F_PI * 0.49f)  /* ~88 degrees */

/* ================================================================
 * Lifecycle
 * ================================================================ */

void dc_camera_init(DCCamera* cam) {
    cam->pos = shz_vec3_init(0.0f, 0.0f, 0.0f);
    cam->yaw = 0.0f;
    cam->pitch = 0.0f;
    cam->fov = 60.0f;
}

/* ================================================================
 * Projection + frustum rebuild
 * ================================================================ */

static void build_frustum(DCCamera* cam, float fov_rad, float aspect) {
    float half_fov_y = fov_rad * 0.5f;
    float half_fov_x = shz_atanf(shz_tanf(half_fov_y) * aspect);

    shz_sincos_t esc_x = shz_sincosf(half_fov_x);
    shz_sincos_t esc_y = shz_sincosf(half_fov_y);

    shz_vec4_t view_left   = shz_vec4_init( esc_x.cos, 0.0f,       -esc_x.sin, 0.0f);
    shz_vec4_t view_right  = shz_vec4_init(-esc_x.cos, 0.0f,       -esc_x.sin, 0.0f);
    shz_vec4_t view_bottom = shz_vec4_init( 0.0f,      esc_y.cos,  -esc_y.sin, 0.0f);
    shz_vec4_t view_top    = shz_vec4_init( 0.0f,     -esc_y.cos,  -esc_y.sin, 0.0f);
    shz_vec4_t view_near   = shz_vec4_init( 0.0f,      0.0f,       -1.0f,      0.0f);
    shz_vec4_t view_far    = shz_vec4_init( 0.0f,      0.0f,        1.0f,      0.0f);

    /* Rotate planes from view-space to world-space */
    shz_xmtrx_init_identity();
    shz_xmtrx_apply_rotation_y(-cam->yaw);
    shz_xmtrx_apply_rotation_x(-cam->pitch);

    shz_vec4_t world_left   = shz_xmtrx_transform_vec4(view_left);
    shz_vec4_t world_right  = shz_xmtrx_transform_vec4(view_right);
    shz_vec4_t world_bottom = shz_xmtrx_transform_vec4(view_bottom);
    shz_vec4_t world_top    = shz_xmtrx_transform_vec4(view_top);
    shz_vec4_t world_near   = shz_xmtrx_transform_vec4(view_near);
    shz_vec4_t world_far    = shz_xmtrx_transform_vec4(view_far);

    /* Negate Z for right-hand → PVR convention */
    world_left.z   = -world_left.z;
    world_right.z  = -world_right.z;
    world_bottom.z = -world_bottom.z;
    world_top.z    = -world_top.z;
    world_near.z   = -world_near.z;
    world_far.z    = -world_far.z;

    shz_vec3_t p = cam->pos;

    float d_left   = -(world_left.x   * p.x + world_left.y   * p.y + world_left.z   * p.z);
    float d_right  = -(world_right.x  * p.x + world_right.y  * p.y + world_right.z  * p.z);
    float d_bottom = -(world_bottom.x * p.x + world_bottom.y * p.y + world_bottom.z * p.z);
    float d_top    = -(world_top.x    * p.x + world_top.y    * p.y + world_top.z    * p.z);
    float d_near   = -(world_near.x   * p.x + world_near.y   * p.y + world_near.z   * p.z) - NEAR_Z;
    float d_far    = -(world_far.x    * p.x + world_far.y    * p.y + world_far.z    * p.z) + FAR_Z;

    cam->_frustum.side_planes.elem2D[0][0] = world_left.x;
    cam->_frustum.side_planes.elem2D[1][0] = world_left.y;
    cam->_frustum.side_planes.elem2D[2][0] = world_left.z;
    cam->_frustum.side_planes.elem2D[3][0] = d_left;

    cam->_frustum.side_planes.elem2D[0][1] = world_right.x;
    cam->_frustum.side_planes.elem2D[1][1] = world_right.y;
    cam->_frustum.side_planes.elem2D[2][1] = world_right.z;
    cam->_frustum.side_planes.elem2D[3][1] = d_right;

    cam->_frustum.side_planes.elem2D[0][2] = world_bottom.x;
    cam->_frustum.side_planes.elem2D[1][2] = world_bottom.y;
    cam->_frustum.side_planes.elem2D[2][2] = world_bottom.z;
    cam->_frustum.side_planes.elem2D[3][2] = d_bottom;

    cam->_frustum.side_planes.elem2D[0][3] = world_top.x;
    cam->_frustum.side_planes.elem2D[1][3] = world_top.y;
    cam->_frustum.side_planes.elem2D[2][3] = world_top.z;
    cam->_frustum.side_planes.elem2D[3][3] = d_top;

    cam->_frustum.near_plane = shz_vec4_init(world_near.x, world_near.y, world_near.z, d_near);
    cam->_frustum.far_plane  = shz_vec4_init(world_far.x,  world_far.y,  world_far.z,  d_far);
}

void dc_camera_update(DCCamera* cam) {
    float fov_rad = SHZ_DEG_TO_RAD(cam->fov);
    float aspect = SCR_W / SCR_H;

    /* Build projection_view matrix into XMTRX, then store */
    shz_xmtrx_init_identity();
    shz_xmtrx_apply_permutation_wxyz();
    shz_xmtrx_apply_screen(SCR_W, SCR_H);
    shz_xmtrx_apply_perspective(fov_rad, aspect, NEAR_Z);
    shz_xmtrx_apply_rotation_x(cam->pitch);
    shz_xmtrx_apply_rotation_y(cam->yaw);
    shz_xmtrx_store_4x4(&cam->_pv_matrix);

    /* Build world-space frustum planes */
    build_frustum(cam, fov_rad, aspect);
}

/* ================================================================
 * Built-in camera modes
 * ================================================================ */

void dc_camera_fps(DCCamera* cam, const DCInput* inp,
                   float move_speed, float look_speed, float dt) {
    if (!inp || !inp->connected) return;

    /* Look — stick */
    cam->yaw   += inp->stick_x * look_speed * dt;
    cam->pitch += inp->stick_y * look_speed * 0.75f * dt;

    if (cam->pitch >  DC_PITCH_LIMIT) cam->pitch =  DC_PITCH_LIMIT;
    if (cam->pitch < -DC_PITCH_LIMIT) cam->pitch = -DC_PITCH_LIMIT;

    /* Movement — DPAD or stick2 */
    shz_sincos_t sc = shz_sincosf(cam->yaw);
    float fwd_x   =  sc.sin;
    float fwd_z   =  sc.cos;
    float right_x =  sc.cos;
    float right_z = -sc.sin;

    float mx = 0.0f, mz = 0.0f;

    /* DPAD */
    if (inp->buttons & CONT_DPAD_UP)    { mx += fwd_x;   mz += fwd_z; }
    if (inp->buttons & CONT_DPAD_DOWN)  { mx -= fwd_x;   mz -= fwd_z; }
    if (inp->buttons & CONT_DPAD_LEFT)  { mx -= right_x; mz -= right_z; }
    if (inp->buttons & CONT_DPAD_RIGHT) { mx += right_x; mz += right_z; }

    /* Second stick (additive) */
    mx += fwd_x * (-inp->stick2_y) + right_x * inp->stick2_x;
    mz += fwd_z * (-inp->stick2_y) + right_z * inp->stick2_x;

    /* Vertical — triggers (R = up, L = down) */
    float my = inp->rtrig - inp->ltrig;

    cam->pos.x += mx * move_speed * dt;
    cam->pos.y += my * move_speed * dt;
    cam->pos.z += mz * move_speed * dt;
}

void dc_camera_orbit(DCCamera* cam, shz_vec3_t target, float distance,
                     const DCInput* inp, float look_speed, float dt) {
    if (!inp || !inp->connected) return;

    cam->yaw   += inp->stick_x * look_speed * dt;
    cam->pitch += inp->stick_y * look_speed * 0.75f * dt;

    if (cam->pitch >  DC_PITCH_LIMIT) cam->pitch =  DC_PITCH_LIMIT;
    if (cam->pitch < -DC_PITCH_LIMIT) cam->pitch = -DC_PITCH_LIMIT;

    shz_sincos_t sc_y = shz_sincosf(cam->yaw);
    shz_sincos_t sc_p = shz_sincosf(cam->pitch);

    cam->pos.x = target.x + distance * sc_p.cos * sc_y.sin;
    cam->pos.y = target.y + distance * sc_p.sin;
    cam->pos.z = target.z + distance * sc_p.cos * sc_y.cos;
}

/* ================================================================
 * Setters
 * ================================================================ */

void dc_camera_set_position(DCCamera* cam, shz_vec3_t pos) {
    cam->pos = pos;
}

void dc_camera_look_at(DCCamera* cam, shz_vec3_t target) {
    float dx = target.x - cam->pos.x;
    float dy = target.y - cam->pos.y;
    float dz = target.z - cam->pos.z;
    float xz_dist = shz_sqrtf_fsrra(dx * dx + dz * dz);

    cam->yaw   = shz_atanf(dx / (dz + 0.0001f));
    cam->pitch = shz_atanf(dy / (xz_dist + 0.0001f));

    /* Fix quadrant for atan */
    if (dz < 0.0f) cam->yaw += SHZ_F_PI;
}

/* ================================================================
 * Frustum queries
 * ================================================================ */

int dc_frustum_cull_sphere(const DCCamera* cam, shz_vec3_t center, float radius) {
    shz_vec4_t pos = shz_vec4_init(center.x, center.y, center.z, 1.0f);

    float near_dist = shz_vec4_dot(cam->_frustum.near_plane, pos);
    if (near_dist < -radius) return -1;

    float far_dist = shz_vec4_dot(cam->_frustum.far_plane, pos);
    if (far_dist < -radius) return -1;

    shz_xmtrx_load_4x4((shz_mat4x4_t*)&cam->_frustum.side_planes);
    shz_vec4_t dists = shz_xmtrx_transform_vec4(pos);

    if (dists.x < -radius) return -1;
    if (dists.y < -radius) return -1;
    if (dists.z < -radius) return -1;
    if (dists.w < -radius) return -1;

    return 0;
}

int dc_frustum_near_intersect(const DCCamera* cam, shz_vec3_t center, float radius) {
    shz_vec4_t pos = shz_vec4_init(center.x, center.y, center.z, 1.0f);
    float nd = shz_vec4_dot(cam->_frustum.near_plane, pos);
    return (nd < radius && nd > -radius);
}

const shz_mat4x4_t* dc_camera_get_pv(const DCCamera* cam) {
    return &cam->_pv_matrix;
}

const WorldFrustum* dc_camera_get_frustum(const DCCamera* cam) {
    return &cam->_frustum;
}
