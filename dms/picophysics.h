/**
 * # Picophysics
 *
 * Picophysics is a very basic physics engine for games. It is not supposed to be fully realistic
 * and is designed to be used particularly on low power systems (N64, Dreamcast). It was built
 * for the N64 port of the Dreamcast game Driving Strikers, where every cycle counts and I needed
 * full control and understanding of what was happening.
 *
 * # Features
 *
 * - Composite bodies made of multiple sphere and box shapes with local offsets
 * - Create fully dynamic spheres and boxes and apply linear and angular forces
 * - Create static environments using triangles
 * - Easy to use collision callback system to respond to detected collisions and to
 *   choose whether to respond at at all (return true to respond)
 * - Ray casting
 * - Axis-locking of rotations
 * - Bodies have friction and bounciness coefficients
 * - Very fast, no dynamic memory allocations
 *
 * # Overview
 *
 * Bodies are composable: each body can hold zero or more shapes (spheres and/or boxes), each
 * with a local offset from the body center. The mass and inertia tensor are automatically
 * computed from the shapes using the parallel axis theorem.
 *
 * Shape primitives:
 *
 * - Spheres (fully dynamic and responsive)
 * - Boxes (fully dynamic; box/box and box/triangle use SAT with a multi-point
 *   contact manifold, so boxes rest and stack stably)
 * - Triangles (static environment, not a body shape)
 *
 * Bodies can also be joined together through constraints: fixed distance, ball
 * joints, and hinges with optional angle limits and a motor.
 *
 * Ray-casting the world is also supported.
 *
 * There is only one global "world", all things are created with in it, and you can empty
 * it with pp_physics_clear(). You also don't need to initialise the world, just start creating
 * bodies and call pp_physics_step(dt, iterations) to update.
 *
 * The library statically allocates memory, by default you can have:
 *
 *  - 32 bodies (spheres + boxes)
 *  - 128 triangles
 *
 * If you need more than that you can define PICOPHYSICS_MAX_OBJECTS or PICOPHYSICS_MAX_TRIANGLES
 * before including physics.h.
 *
 * # Support
 *
 * Although this is an open-source project, I do not have the time to provide support for it. If you send me an MR
 * I'll review it and merge it if it's good - that's about as much as I can do. This has been written for my own purposes
 * primarily.
 *
 * If you find this project useful in some way, please consider buying me a coffee at https://ko-fi.com/kazade or supporting me
 * on Patreon at https://www.patreon.com/kazade
 *
 * # Help needed!
 *
 * I am *not* a mathematician! Collision response is something I'm finding quite
 * difficult to understand (particularly angular/torque responses). There are definitely
 * issues in the collision response code. If you can help fix it, I'd appreciate it!
 *
 * # Roadmap
 *
 * - Capsules
 * - Broad-phase collision detection (spatial hashing)
 * - Add fixed and spring joints (links) between objects
 * - Slab allocation (so it's possible to overflow the static array)
 * - Make structs opaque
 *
 * I have no intention of adding more than this! If you want something more there are a bunch
 * of great open-source physics engines out there (e.g. Bullet, Box2D, ODE, Bounce..)
 *
 * # Usage
 *
 * Picophysics is a single-file header library (in the spirit of stb). To use it
 * you must do this in a single .c/.cpp file:
 *
 * #define PICOPHYSICS_IMPLEMENTATION
 * #include "picophysics.h"
 *
 * You must regularly call `pp_physics_step(step, vel_iterations, pos_iterations)` to run the simulation.
 *
 * # Examples
 *
 * The examples are written using the Simulant engine. To build them you'll need Docker, Python
 * and [Simulant Tools](https://gitlab.com/simulant/simulant-tools) which you can install with:
 *
 * - pip3 install -U --user git+https://gitlab.com/simulant/simulant-tools.git
 *
 * Once installed, from the example directory run:
 *
 *  - simulant update -b next
 *  - simulant run --rebuild
 *
 * CHANGELOG
 *
 * - ALPHA - no releases yet
 *
 * LICENSE
 *
 * See the end of the file for license information
 *
 * AI USAGE
 *
 * Although the majority of the API and codebase has been written by hand, I've used AI assistence for the following things:
 *
 * - Replacing my hand rolled GJK/EPA algorithm with SAT
 * - Box vs triangle collisions
 * - Test writing
 * - Bug fixing and debugging
 * - Clean up
 *
 * This is primarily due to lack of time on my part; I believe generally that code is better created by humans.
 *
 * Because of this, I've released this code into the public domain (CC0 license).
 */

#ifndef PICOPHYSICS_H
#define PICOPHYSICS_H

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* See "Maths back-end" below. */
#ifdef PICOPHYSICS_USE_SH4ZAM
#include <sh4zam/shz_scalar.h>
#include <sh4zam/shz_trig.h>
#include <sh4zam/shz_vector.h>
#include <sh4zam/shz_quat.h>
#include <sh4zam/shz_matrix.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _PPVec3
{
    union {
        struct
        {
            float x, y, z;
        };
        float xyz[3];
    };
} PPVec3;

typedef struct _PPQuaternion
{
    union {
        struct
        {
            float x, y, z, w;
        };
        float xyzw[4];
    };
} PPQuaternion;

typedef struct _PPMat3
{
    float m[9];
} PPMat3;

typedef struct _PPPlane
{
    PPVec3 n;
    float d;
} PPPlane;

typedef uint8_t BodyKind;

typedef enum _PPObjectType {
    PP_OBJECT_TYPE_SPHERE,
    PP_OBJECT_TYPE_BOX,
    PP_OBJECT_TYPE_TRIANGLE,
    PP_OBJECT_TYPE_CAPSULE,
} PPObjectType;

#ifndef PP_MAX_SHAPES_PER_BODY
#define PP_MAX_SHAPES_PER_BODY 8
#endif

typedef struct _PPShape {
    PPObjectType type;
    PPVec3 offset;
    float mass;
    BodyKind kind;
    union {
        struct { float radius; } sphere;
        struct { PPVec3 whd; PPVec3 half_extents; float bounding_radius; } box;
        /* A sphere of `radius` swept along the body's local Y axis, from
         * -half_height to +half_height about the shape's offset. */
        struct { float radius; float half_height; } capsule;
    };
} PPShape;

typedef struct _PPBody PPBody;

typedef struct _PPCollision
{
    PPVec3 p;
    PPVec3 n;
    PPBody *obj1;
    PPBody *obj2;

    // These are stored separately as obj1/obj2 will
    // be null if we collided with geometry, but these
    // values are still necessary in the null case
    float obj1_bounce;
    float obj2_bounce;
    float obj1_friction;
    float obj2_friction;

    PPObjectType type1;
    PPObjectType type2;

    BodyKind kind1;
    BodyKind kind2;

    float dist; // Penetration depth (positive when overlapping)

    // Signed gap between the two surfaces: negative when penetrating, positive
    // when there is still a gap. A positive value indicates a *speculative*
    // contact -- the bodies are not touching yet but are predicted to make
    // contact within the current step. The velocity solver uses this to brake
    // an approaching body so it closes the gap without overshooting (passing
    // through), which is what prevents fast movers from tunnelling.
    float separation;

    /* ---- solver state, filled in by the step; not meaningful in callbacks ---
     *
     * t1/t2 are a fixed tangent basis. Friction has to accumulate along axes
     * that do not move during the solve, otherwise its direction chases the
     * instantaneous tangent velocity and pumps energy into a resting body.
     *
     * jn/jt1/jt2 are accumulated impulses. The solver clamps the accumulated
     * value, not each increment, and applies only the change; clamping the
     * increment lets it push bodies apart but never take an over-push back. */
    PPVec3 t1, t2;
    float jn;       /* accumulated normal impulse, >= 0 */
    float jt1, jt2; /* accumulated friction impulse along t1/t2 */
    float max_jn;   /* largest jn seen this step: did the contact ever push? */
    bool impact;    /* arrived with real approach speed, i.e. not a resting contact */

    PPVec3 r1, r2;                   /* contact point relative to each body, at step start */
    float mass_n, mass_t1, mass_t2;  /* effective masses along n, t1, t2 */
    float roll_radius;               /* set by capsule contacts: the radius it rolls on */
    float roll_limit;                /* rolling resistance torque per unit normal impulse */
    PPVec3 jroll;                    /* rolling resistance applied this substep */

    /* Restitution target, captured once per step from the approach velocity
     * before any solving. It cannot be re-derived later: once the contact has
     * been solved it is no longer approaching. */
    float v_bias;
} PPCollision;

typedef enum _PPAxisLock {
    PP_AXIS_LOCK_NONE,
    PP_AXIS_LOCK_PITCH = 0x1,
    PP_AXIS_LOCK_YAW = 0x2,
    PP_AXIS_LOCK_ROLL = 0x4,
    PP_AXIS_LOCK_PITCH_AND_ROLL = PP_AXIS_LOCK_PITCH | PP_AXIS_LOCK_ROLL,
    PP_AXIS_LOCK_PITCH_AND_YAW = PP_AXIS_LOCK_PITCH | PP_AXIS_LOCK_YAW,
    PP_AXIS_LOCK_YAW_AND_ROLL = PP_AXIS_LOCK_YAW | PP_AXIS_LOCK_ROLL,
    PP_AXIS_LOCK_ALL = PP_AXIS_LOCK_PITCH | PP_AXIS_LOCK_YAW | PP_AXIS_LOCK_ROLL
} PPAxisLock;

typedef enum _PPConstraintType {
    PP_CONSTRAINT_TYPE_FIXED_DISTANCE,
    PP_CONSTRAINT_TYPE_FIXED,
    PP_CONSTRAINT_TYPE_BALL,
    PP_CONSTRAINT_TYPE_HINGE,
} PPConstraintType;

typedef struct _PPFixedDistanceConstraint {
    float distance;
} PPFixedDistanceConstraint;

/* Ball and hinge joints. Anchors, axes and reference directions are stored in
 * each body's local space; everything from `impulse` down is solver state. */
typedef struct _PPJointConstraint {
    PPVec3 local_anchor1, local_anchor2;
    PPVec3 local_axis1, local_axis2;
    PPVec3 local_ref1, local_ref2;

    bool collide_connected;

    bool limit_enabled;
    float lower_angle, upper_angle;

    bool motor_enabled;
    float motor_speed, max_motor_torque;

    PPVec3 impulse;
    float axial_impulse[2];
    float motor_impulse, lower_impulse, upper_impulse;

    PPVec3 r1, r2;
    PPVec3 separation;
    PPMat3 inv_k;
    PPVec3 axis;
    PPVec3 u[2];
    float u_error[2];
    float u_mass[2];
    float axis_mass;
    float angle;
} PPJointConstraint;

typedef struct _PPConstraint {
    bool is_alive;
    PPConstraintType type;
    PPBody* body1;
    PPBody* body2;

    union {
        PPFixedDistanceConstraint fixed_distance;
        PPJointConstraint joint;
    };

    // Combined mass of both bodies.
    float inv_mass_sum;
} PPConstraint;

typedef struct _PPBody
{
    PPVec3 pos;
    PPQuaternion rot;
    PPVec3 vel;
    PPVec3 acc;
    PPVec3 a_vel;
    PPVec3 a_acc;

    float bounce;
    float friction;
    float rolling_resistance;
    float damping;
    float a_damping;

    float mass;
    float inv_mass;

    PPMat3 inertia;
    PPMat3 inv_inertia;

    /* inv_inertia rotated into world space (R * I^-1 * R^T). The solver works
     * with world-space torques and angular velocities, so this is what it
     * must use; refreshed at the start of every step. */
    PPMat3 inv_inertia_world;
    PPVec3 com_local; /* centre of mass in the body's frame; zero unless shapes are lopsided */

    BodyKind kind;

    PPAxisLock lock;

    bool is_alive;
    void *user_data;

    float vel_limit;
    float a_vel_limit;

    // Used to reduce or increase gravity applied
    // to a particular body
    float gravity_multiplier;

    PPShape shapes[PP_MAX_SHAPES_PER_BODY];
    int shape_count;

    /* How far the body has moved and turned since the start of the current
     * step. Contacts use these to keep their separation up to date across
     * substeps without re-running collision detection. */
    PPVec3 solve_dpos;
    PPVec3 solve_dtheta;

    /* Sleeping. A body that has been still for long enough, along with
     * everything it is touching, stops being simulated until something
     * disturbs it. sleep_group ties together the bodies that went to sleep as
     * one resting pile, so that waking any of them wakes them all. */
    bool is_asleep;
    bool never_sleeps;
    float sleep_time;
    uint32_t sleep_group;

} PPBody;

typedef struct _PPTriangle
{
    PPVec3 v[3];
    PPVec3 n;
    PPPlane p;
    BodyKind kind;
    float bounce;
    float friction;
    // Precomputed AABB for fast sphere culling
    float aabb_min_x, aabb_min_y, aabb_min_z;
    float aabb_max_x, aabb_max_y, aabb_max_z;
} PPTriangle;

PPVec3 *pp_vec3_init(PPVec3 *v);
PPVec3 *pp_vec3_set(PPVec3 *v, float x, float y, float z);
PPVec3 *pp_vec3_assign(PPVec3 *target, const PPVec3 *source);
bool pp_vec3_normalize(PPVec3 *target);

/* ---- Maths back-end -------------------------------------------------------
 *
 * Every square root, reciprocal, trig call and the hot vector operations go
 * through the pp_ wrappers below, so that a platform's fast paths can be
 * swapped in from one place.
 *
 * Define PICOPHYSICS_USE_SH4ZAM (and have SH4ZAM's include directory on the
 * include path) to route them through SH4ZAM: on a Dreamcast that means FSRRA
 * for 1/sqrt and 1/x and FIPR for dot products; anywhere else SH4ZAM falls
 * back to plain C, which is what lets the same build be unit tested on a PC.
 * Without the define this header depends on nothing but libm. */
#ifdef PICOPHYSICS_USE_SH4ZAM

#define pp_sqrtf(x)     shz_sqrtf(x)
#define pp_inv_sqrtf(x) shz_inv_sqrtf(x)
#define pp_invf(x)      shz_invf(x)
#define pp_sinf(x)      shz_sinf(x)
#define pp_cosf(x)      shz_cosf(x)
/* shz_acosf() is an approximation good to about 0.01 rad. Nothing in the step
 * calls acos -- only slerp and the angle helpers -- so accuracy wins here. */
#define pp_acosf(x)     acosf(x)
#define pp_atan2f(y, x) shz_atan2f((y), (x))

static inline shz_vec3_t pp_to_shz(const PPVec3 *v)
{
    return shz_vec3_init(v->x, v->y, v->z);
}

static inline float pp_vec3_length(const PPVec3 *v1)
{
    // Not shz_vec3_magnitude(): that is 1/sqrt(x) * x with no guard, which is
    // NaN for a zero vector, and a body at rest has a zero velocity.
    shz_vec3_t v = pp_to_shz(v1);
    return shz_sqrtf(shz_vec3_dot(v, v));
}

static inline float pp_vec3_dot(const PPVec3 *v1, const PPVec3 *v2)
{
    return shz_vec3_dot(pp_to_shz(v1), pp_to_shz(v2));
}

#else

#define pp_sqrtf(x)     sqrtf(x)
#define pp_inv_sqrtf(x) (1.0f / sqrtf(x))
#define pp_invf(x)      (1.0f / (x))
#define pp_sinf(x)      sinf(x)
#define pp_cosf(x)      cosf(x)
#define pp_acosf(x)     acosf(x)
#define pp_atan2f(y, x) atan2f((y), (x))

static inline float pp_vec3_length(const PPVec3 *v1)
{
    return sqrtf(v1->x * v1->x + v1->y * v1->y + v1->z * v1->z);
}

static inline float pp_vec3_dot(const PPVec3 *v1, const PPVec3 *v2)
{
    return v1->x * v2->x + v1->y * v2->y + v1->z * v2->z;
}

#endif

static inline PPVec3 *pp_vec3_scale(const PPVec3 *v1, float t, PPVec3 *out)
{
    out->x = v1->x * t;
    out->y = v1->y * t;
    out->z = v1->z * t;
    return out;
}


PPQuaternion *pp_quat_init(PPQuaternion *q);
void pp_quat_between(const PPVec3 *v0, const PPVec3 *q1, PPQuaternion *result);
void pp_quat_slerp(const PPQuaternion *q0, const PPQuaternion *q1, float t, PPQuaternion *result);
PPQuaternion *pp_quat_assign(PPQuaternion *target, const PPQuaternion *source);
float pp_quat_angle_between(const PPQuaternion *q0, const PPQuaternion *q1);

/**
 * Run the world physics step.
 *
 * @t time step, this should be something fixed (E.g. 1 / 50)
 * @iterations The number of iterations to run in the solver. 8 is a fairly reliable value
 *             but more == slower.
 */
void pp_physics_step(float t, int vel_iterations, int pos_iterations);
bool pp_physics_ray_intersect(const PPVec3 *origin,
                              const PPVec3 *direction,
                              BodyKind *ignore_kinds,
                              const PPBody **body_hit,
                              const PPTriangle **tri_hit,
                              float *distance,
                              PPVec3 *intersection);

/* What a ray hit. `body` is NULL when it was the static world. `kind` is the
 * triangle's or the shape's kind, so it can double as a surface type (tarmac,
 * gravel, ...). `normal` is unit length and faces back towards the ray. */
typedef struct _PPRayHit {
    const PPBody *body;
    BodyKind kind;
    float distance;
    PPVec3 point;
    PPVec3 normal;
} PPRayHit;

/* Nearest hit along a ray no longer than max_distance; `direction` need not be
 * unit length. Unlike pp_physics_ray_intersect() this goes through the
 * triangle query when one is set, asking only for the triangles around the
 * ray, so its cost depends on the ray's length and not on the size of the
 * world: a short ray under a wheel touches a handful of triangles. A query
 * that comes back full is split in two and retried, nearest half first.
 *
 * ignore_kinds is a 0-terminated array or NULL. ignore_body (may be NULL) is
 * skipped entirely: the ray's owner, usually. A ray that starts inside a
 * body does not hit that body. */
bool pp_physics_ray_cast(const PPVec3 *origin,
                         const PPVec3 *direction,
                         float max_distance,
                         BodyKind *ignore_kinds,
                         const PPBody *ignore_body,
                         PPRayHit *hit);
void pp_physics_clear();
void pp_physics_set_gravity(const PPVec3 *v);
bool pp_physics_collision_map_add(BodyKind kind1,
                                  BodyKind kind2,
                                  void *user_data,
                                  bool (*callback)(const void *,
                                                   const void *,
                                                   BodyKind,
                                                   BodyKind,
                                                   const PPCollision *c,
                                                   const void *));

PPTriangle *pp_physics_create_triangle(const PPVec3 *v1,
                                       const PPVec3 *v2,
                                       const PPVec3 *v3,
                                       BodyKind kind);
size_t pp_physics_triangle_count();
const PPTriangle *pp_physics_triangle_at(size_t i);

/* Fill `tri` from three corners without adding it to the global `tris[]`
 * array. Used by hosts that keep their own static world (a BVH, a grid) and
 * feed picophysics only the triangles near each shape via a PPTriangleQuery. */
void pp_triangle_init(PPTriangle *tri,
                      const PPVec3 *v1,
                      const PPVec3 *v2,
                      const PPVec3 *v3,
                      BodyKind kind);

/* Host-supplied broadphase over the static world.
 *
 * Called once per shape per step. The host writes up to `max` triangles whose
 * bounds touch the sphere (`centre`, `reach`) into `out` and returns how many
 * it wrote. Returning more than `max` is not allowed; the host should stop at
 * `max` and let picophysics count the overflow.
 *
 * While a query is set, the global `tris[]` array is not consulted at all. */
typedef int (*PPTriangleQuery)(const PPVec3 *centre,
                               float reach,
                               PPTriangle *out,
                               int max,
                               void *user);

/* Pass fn = NULL to go back to iterating the global `tris[]` array. */
void pp_physics_set_triangle_query(PPTriangleQuery fn, void *user);

/* Number of queries in the last step that returned exactly `max` triangles,
 * i.e. that may have been truncated. Reset at the top of each step. */
int pp_physics_query_overflow_count();

/* Most triangles returned by a single query in the last step. */
int pp_physics_query_max_candidates();

PPBody *pp_physics_create_sphere(float radius, const PPVec3 *pos, float mass, BodyKind kind);

PPBody *pp_physics_create_box(
    float width, float height, float depth, const PPVec3 *pos, float mass, BodyKind kind);

/* A capsule: a cylinder with rounded ends, standing along the body's local Y
 * axis. `height` is the overall height, caps included, so a 1.8 tall character
 * of radius 0.4 is (0.4, 1.8); anything not taller than 2 * radius is a sphere.
 * The rounded bottom rides over the seams and small steps that catch a box,
 * which makes it the usual shape for a character (lock pitch and roll with
 * pp_body_lock_axis() to keep it upright). */
PPBody *pp_physics_create_capsule(float radius, float height, const PPVec3 *pos, float mass, BodyKind kind);
PPShape *pp_body_add_capsule(PPBody *body, float radius, float height, float mass, BodyKind kind, float offset_x, float offset_y, float offset_z);

PPBody *pp_physics_create_body(const PPVec3 *pos);
PPShape *pp_body_add_box(PPBody *body, float w, float h, float d, float mass, BodyKind kind, float offset_x, float offset_y, float offset_z);
PPShape *pp_body_add_sphere(PPBody *body, float radius, float mass, BodyKind kind, float offset_x, float offset_y, float offset_z);
size_t pp_body_get_shape_count(const PPBody *body);
const PPShape *pp_body_get_shape(const PPBody *body, size_t index);

size_t pp_body_get_constraint_count(PPBody *body);
PPConstraint* pp_physics_create_fixed_distance_constraint(PPBody* body1, PPBody* body2, float distance);

/* Joints. A ball joint pins a point of body1 to a point of body2 and leaves
 * them free to turn about it (rope links, ragdoll shoulders, a wrecking ball).
 * A hinge also keeps one axis of each body lined up, so the only freedom left
 * is turning about that axis (doors, wheels, flippers, drawbridges).
 *
 * The anchor and axis are given in world space, for the bodies as they are
 * posed at the time of the call. Either body may be NULL to pin the other to
 * the world. Jointed bodies do not collide with each other unless
 * pp_constraint_set_collide_connected() says so. Returns NULL when
 * PICOPHYSICS_MAX_CONSTRAINTS is reached.
 *
 * Joints have no friction of their own, so a chain swings for a long time on
 * the default body damping; raise pp_body_set_damping() and
 * pp_body_set_angular_damping() on the links if it should settle. */
PPConstraint* pp_physics_create_ball_joint(PPBody* body1, PPBody* body2, const PPVec3* world_anchor);
PPConstraint* pp_physics_create_hinge_joint(PPBody* body1, PPBody* body2, const PPVec3* world_anchor, const PPVec3* world_axis);
void pp_physics_destroy_constraint(PPConstraint* c);
void pp_constraint_set_collide_connected(PPConstraint* c, bool collide);

/* The hinge angle is how far body2 has turned about the axis relative to
 * body1 since the joint was made, in radians, within (-pi, pi]. Limits have to
 * lie inside that interval, with lower <= upper. */
float pp_constraint_get_hinge_angle(const PPConstraint* c);
void pp_constraint_set_hinge_limits(PPConstraint* c, float lower, float upper);
void pp_constraint_disable_hinge_limits(PPConstraint* c);
/* Drives the hinge towards `speed` rad/s using at most `max_torque`. A speed
 * of zero makes a stiff, braked hinge; a max_torque of zero turns it off. */
void pp_constraint_set_hinge_motor(PPConstraint* c, float speed, float max_torque);

void pp_physics_destroy_body(PPBody *b);

/* Sleeping: bodies that come to rest stop costing anything until disturbed.
 * On by default. Every pp_body_* call that changes a body's motion, position
 * or shape wakes it; if you write to a PPBody's fields directly, call
 * pp_body_wake() yourself. Adding or removing static triangles under sleeping
 * bodies does not wake them -- call pp_physics_wake_all(). */
void pp_physics_set_sleeping_enabled(bool enabled);
bool pp_physics_get_sleeping_enabled();
/* A body sleeps after moving slower than `linear` (units/s) and turning slower
 * than `angular` (rad/s) for `seconds`. Defaults: 0.05, 0.05, 0.5. */
void pp_physics_set_sleep_thresholds(float linear, float angular, float seconds);
void pp_physics_wake_all();
void pp_body_wake(PPBody *b);
bool pp_body_is_asleep(const PPBody *b);
/* Exempt a body from sleeping (e.g. the player). Bodies resting on it can
 * still sleep only if it could too, so use sparingly. */
void pp_body_set_never_sleeps(PPBody *b, bool never);

const PPBody *pp_physics_body_at(size_t i);
size_t pp_physics_body_count();
size_t pp_physics_body_total_count();
bool pp_body_set_bounce(PPBody *s, float b);
void pp_body_add_force(PPBody *s, float x, float y, float z);
void pp_body_add_force_at_position(PPBody *b, const PPVec3 *world_pos, const PPVec3 *force);
void pp_body_add_angular_force(PPBody *s, float x, float y, float z);
void pp_body_lock_axis(PPBody *s, PPAxisLock lock);
void pp_body_set_angular_damping(PPBody *s, float d);
void pp_body_set_damping(PPBody *s, float d);

void pp_body_get_forward(PPBody *s, PPVec3 *f);
void pp_body_get_right(PPBody *s, PPVec3 *r);
void pp_body_get_up(PPBody *s, PPVec3 *u);

void pp_body_set_position(PPBody *s, float x, float y, float z);
void pp_body_get_position(PPBody *s, PPVec3 *pos);
void pp_body_set_rotation(PPBody *s, float x, float y, float z, float w);
void pp_body_get_rotation(PPBody *s, PPQuaternion *rot);
float pp_body_get_radius(PPBody *s);
void pp_body_set_user_data(PPBody *s, void *data);
void *pp_body_get_user_data(const PPBody *s);
void pp_body_set_kind(PPBody *b, BodyKind kind);
BodyKind pp_body_get_kind(const PPBody *b);
bool pp_body_set_friction(PPBody *s, float f);
/* How quickly a rolling sphere comes to rest: 0 rolls forever, the default
 * 0.05 stops a ball over a few tens of metres on the flat. Only affects sphere
 * shapes. */
void pp_body_set_rolling_resistance(PPBody *s, float r);
void pp_body_set_mass(PPBody* b, float mass);
float pp_body_get_mass(const PPBody* b);
void pp_body_get_velocity(const PPBody *s, PPVec3 *vel);
void pp_body_get_velocity_at_position(const PPBody *b, const PPVec3 *p, PPVec3 *ret);
void pp_body_set_angular_velocity(PPBody *s, float x, float y, float z);
void pp_body_set_velocity(PPBody *s, float x, float y, float z);

/* Kinematic bodies: a body with zero mass that is given a velocity (linear,
 * angular or both) moves at exactly that velocity. Gravity, collisions and
 * constraints never alter it, but it pushes dynamic bodies out of its way and
 * carries along whatever rests on it -- a lift, a sliding door, a moving
 * platform. It keeps moving until you set its velocity back to zero.
 *
 * pp_body_move_kinematic() is for animation-driven movers: it sets the
 * velocity that takes the body to `target` over the next step of length `dt`,
 * so riders feel the motion instead of being teleported through. Call it
 * every frame before pp_physics_step(). */
void pp_body_move_kinematic(PPBody *s, const PPVec3 *target, float dt);
void pp_body_set_angular_acceleration(PPBody *s, float x, float y, float z);
void pp_body_set_acceleration(PPBody *s, float x, float y, float z);
void pp_body_look_at(PPBody *s, float x, float y, float z);
void pp_body_set_gravity_multiplier(PPBody *b, float multiplier);
float pp_body_get_gravity_multiplier(const PPBody *b);
/**
 * Limit the maximium velocity of the object. The limit will be applied after
 * adding acceleration forces. Passing 0.0f will remove the limit.
 *
 * @param body The body whose velocity will be restricted
 * @param speed A the desired velocity limit.
 */
void pp_body_limit_velocity(PPBody *body, float speed);

/**
 * Limit the maximium angular velocity of the object. The limit will be applied after
 * adding acceleration forces. Passing 0.0f will remove the limit.
 *
 * @param body The body whose angular velocity will be restricted
 * @param speed A the desired angular velocity limit.
 */
void pp_body_limit_angular_velocity(PPBody *body, float speed);

#ifdef __cplusplus
}
#endif

#endif

#ifdef PICOPHYSICS_IMPLEMENTATION

#ifndef PICOPHYSICS_MAX_OBJECTS
#define PICOPHYSICS_MAX_OBJECTS 32
#endif

#ifndef PICOPHYSICS_MAX_TRIANGLES
#define PICOPHYSICS_MAX_TRIANGLES 128
#endif

#ifndef PICOPHYSICS_MAX_CONSTRAINTS
#define PICOPHYSICS_MAX_CONSTRAINTS 256
#endif

#ifndef PICOPHYSICS_MAX_MANIFOLDS
#define PICOPHYSICS_MAX_MANIFOLDS 256
#endif

/* Size of the per-shape candidate buffer filled by a PPTriangleQuery. This
 * buffer is a local in pp_physics_step, so it costs
 * PICOPHYSICS_MAX_QUERY_TRIANGLES * sizeof(PPTriangle) bytes of stack. */
#ifndef PICOPHYSICS_MAX_QUERY_TRIANGLES
#define PICOPHYSICS_MAX_QUERY_TRIANGLES 128
#endif

static PPBody objects[PICOPHYSICS_MAX_OBJECTS];
static int object_count = 0;
static int dead_object_count = 0;
static PPConstraint constraints[PICOPHYSICS_MAX_CONSTRAINTS];
static int constraint_count = 0;
static int dead_constraint_count = 0;

/* This body is used to represent the entire tri-mesh. It's static,
 * has a mass of zero and an inv_mass of zero and so it should never change
 * position. The only thing to be careful of is that the position and rotation
 * is totally irrelevant for the simulation! Always take this into account when
 * working on the collision response code. */
static PPBody trimesh_body = {
    .is_alive = true,
};

static PPTriangle tris[PICOPHYSICS_MAX_TRIANGLES];
static int tri_count = 0;

/* Last step's solved contacts, kept so this step's solver can start from the
 * impulses that were holding everything up a moment ago (warm starting)
 * instead of rediscovering them from zero. Without it a stack N boxes deep
 * needs on the order of N iterations just to carry the load to the ground. */
typedef struct _PPContactCache {
    PPBody *obj1;
    PPBody *obj2;
    PPVec3 r1;   /* contact point relative to obj1->pos, world axes */
    PPVec3 n;
    float jn, jt1, jt2;
    bool used;
} PPContactCache;

static PPContactCache contact_cache[PICOPHYSICS_MAX_MANIFOLDS];
static int contact_cache_count = 0;

static PPTriangleQuery tri_query = NULL;
static void *tri_query_user = NULL;
static int tri_query_overflows = 0;
static int tri_query_max_candidates = 0;




static struct _PPCollisionMapEntry
{
    BodyKind kind1;
    BodyKind kind2;
    void *user_data;
    bool (*collision_callback)(
        const void *, const void *, BodyKind, BodyKind, const PPCollision *c, const void *);
} collision_map[32];

static int collision_map_count = 0;

static PPVec3 gravity = {.xyz = {0.0f, 0.0f, 0.0f}};
static float gravity_magnitude = 0.0f;

/* Contacts are generated for shapes within this distance of touching, so that
 * a resting contact persists from step to step instead of flickering. */
#define PP_CONTACT_MARGIN 0.02f

static bool sleeping_enabled = true;
static float sleep_linear_threshold = 0.05f;
static float sleep_angular_threshold = 0.05f;
static float sleep_delay = 0.5f;
static uint32_t sleep_group_counter = 0;

PPVec3 *pp_vec3_init(PPVec3 *v)
{
    v->x = 0.0f;
    v->y = 0.0f;
    v->z = 0.0f;
    return v;
}

PPVec3 *pp_vec3_set(PPVec3 *v, float x, float y, float z)
{
    v->x = x;
    v->y = y;
    v->z = z;
    return v;
}

PPVec3 *pp_vec3_assign(PPVec3 *target, const PPVec3 *source)
{
    pp_vec3_set(target, source->x, source->y, source->z);
    return target;
}

static inline PPVec3 *pp_vec3_add(const PPVec3 *v1, const PPVec3 *v2, PPVec3 *out)
{
    out->x = v1->x + v2->x;
    out->y = v1->y + v2->y;
    out->z = v1->z + v2->z;
    return out;
}

static inline PPVec3 *pp_vec3_sub(const PPVec3 *v1, const PPVec3 *v2, PPVec3 *out)
{
    out->x = v1->x - v2->x;
    out->y = v1->y - v2->y;
    out->z = v1->z - v2->z;
    return out;
}

static inline PPVec3 *pp_vec3_neg(const PPVec3 *v1, PPVec3 *out)
{
    out->x = -v1->x;
    out->y = -v1->y;
    out->z = -v1->z;
    return out;
}

static inline float pp_vec3_length_sq(const PPVec3 *v1)
{
    return v1->x * v1->x + v1->y * v1->y + v1->z * v1->z;
}

static inline float pp_vec3_dist(const PPVec3 *v1, const PPVec3 *v2)
{
    PPVec3 tmp;
    pp_vec3_sub(v2, v1, &tmp);
    return pp_vec3_length(&tmp);
}

static inline float pp_vec3_dist_sq(const PPVec3 *v1, const PPVec3 *v2)
{
    PPVec3 tmp;
    pp_vec3_sub(v2, v1, &tmp);
    return pp_vec3_length_sq(&tmp);
}

static inline PPVec3 *pp_vec3_cross(const PPVec3 *v1, const PPVec3 *v2, PPVec3 *out)
{
#ifdef PICOPHYSICS_USE_SH4ZAM
    shz_vec3_t c = shz_vec3_cross(pp_to_shz(v1), pp_to_shz(v2));
    out->x = c.x;
    out->y = c.y;
    out->z = c.z;
    return out;
#else
    assert(v1 != out);
    assert(v2 != out);

    out->x = v1->y * v2->z - v1->z * v2->y;
    out->y = v1->z * v2->x - v1->x * v2->z;
    out->z = v1->x * v2->y - v1->y * v2->x;
    return out;
#endif
}

bool pp_vec3_normalize(PPVec3 *v)
{
    float length_sq = pp_vec3_dot(v, v);

    // Check for zero-length vector to avoid division by zero
    if (length_sq > 0.0f) {
        float inv_length = pp_inv_sqrtf(length_sq);
        v->x *= inv_length;
        v->y *= inv_length;
        v->z *= inv_length;
        return true;
    } else {
        pp_vec3_init(v);
        return false;
    }
}

void pp_mat3_inverse(const PPMat3 *in, PPMat3 *out)
{
    const float *m = in->m;

    float a = m[0], d = m[3], g = m[6];
    float b = m[1], e = m[4], h = m[7];
    float c = m[2], f = m[5], i = m[8];

    // Cofactors
    float A = (e * i - f * h);
    float B = -(d * i - f * g);
    float C = (d * h - e * g);
    float D = -(b * i - c * h);
    float E = (a * i - c * g);
    float F = -(a * h - b * g);
    float G = (b * f - c * e);
    float H = -(a * f - c * d);
    float I = (a * e - b * d);

    // Determinant
    float det = a * A + d * D + g * G;

    float inv_det = pp_invf(det);

    // Adjugate (still column-major)
    out->m[0] = A * inv_det;
    out->m[1] = D * inv_det;
    out->m[2] = G * inv_det;

    out->m[3] = B * inv_det;
    out->m[4] = E * inv_det;
    out->m[5] = H * inv_det;

    out->m[6] = C * inv_det;
    out->m[7] = F * inv_det;
    out->m[8] = I * inv_det;
}

void pp_mat3_mult(const PPMat3 *m, const PPVec3 *p, PPVec3 *pout)
{
#ifdef PICOPHYSICS_USE_SH4ZAM
    // Same column-major layout, so the matrix can be used where it lies.
    shz_vec3_t r = shz_mat3x3_transform_vec3((const shz_mat3x3_t *) m->m, pp_to_shz(p));
    pout->x = r.x;
    pout->y = r.y;
    pout->z = r.z;
#else
    float x = p->x;
    float y = p->y;
    float z = p->z;

    pout->x = m->m[0] * x + m->m[3] * y + m->m[6] * z;
    pout->y = m->m[1] * x + m->m[4] * y + m->m[7] * z;
    pout->z = m->m[2] * x + m->m[5] * y + m->m[8] * z;
#endif
}

PPQuaternion *pp_quat_init(PPQuaternion *q)
{
    q->x = 0.0f;
    q->y = 0.0f;
    q->z = 0.0f;
    q->w = 1.0f;
    return q;
}

PPQuaternion *pp_quat_set(PPQuaternion *q, float x, float y, float z, float w)
{
    q->x = x;
    q->y = y;
    q->z = z;
    q->w = w;
    return q;
}

PPQuaternion *pp_quat_assign(PPQuaternion *target, const PPQuaternion *source)
{
    pp_quat_set(target, source->x, source->y, source->z, source->w);
    return target;
}

void pp_quat_from_angular_velocity(const PPVec3 *a_vel, float dt, PPQuaternion *q_rot)
{
    float angle = pp_vec3_length(a_vel) * dt; // Calculate the rotation angle
    if (angle > 0.0f) {
        PPVec3 axis;
        pp_vec3_assign(&axis, a_vel);
        pp_vec3_normalize(&axis); // Normalize the angular velocity to get the axis of rotation

        // Calculate sine and cosine of the half angle
        float sin_half_angle = pp_sinf(angle / 2);
        float cos_half_angle = pp_cosf(angle / 2);

        // Create the quaternion
        q_rot->x = axis.xyz[0] * sin_half_angle;
        q_rot->y = axis.xyz[1] * sin_half_angle;
        q_rot->z = axis.xyz[2] * sin_half_angle;
        q_rot->w = cos_half_angle;
    } else {
        // If there is no rotation
        pp_quat_init(q_rot);
    }
}

void pp_quat_multiply(const PPQuaternion *q1, const PPQuaternion *q2, PPQuaternion *result)
{
    PPQuaternion tmp;
    tmp.xyzw[0] = q1->w * q2->x + q1->x * q2->w + q1->y * q2->z - q1->z * q2->y;
    tmp.xyzw[1] = q1->w * q2->y - q1->x * q2->z + q1->y * q2->w + q1->z * q2->x;
    tmp.xyzw[2] = q1->w * q2->z + q1->x * q2->y - q1->y * q2->x + q1->z * q2->w;
    tmp.xyzw[3] = q1->w * q2->w - q1->x * q2->x - q1->y * q2->y - q1->z * q2->z;

    *result = tmp;
}

float pp_quat_angle_between(const PPQuaternion *q1, const PPQuaternion *q2)
{
    float dot = q1->w * q2->w + q1->x * q2->x + q1->y * q2->y + q1->z * q2->z;
    return pp_acosf(dot) * 2.0f;
}

void pp_quat_transform(const PPQuaternion *q, const PPVec3 *v, PPVec3 *ret)
{
#ifdef PICOPHYSICS_USE_SH4ZAM
    shz_vec3_t r = shz_quat_transform_vec3(shz_quat_init(q->w, q->x, q->y, q->z), pp_to_shz(v));
    pp_vec3_set(ret, r.x, r.y, r.z);
#else
    // v + 2w (u x v) + 2 u x (u x v), with u the vector part: the same result
    // as q v q* for a unit quaternion at a third of the multiplies.
    float tx = q->y * v->z - q->z * v->y;
    float ty = q->z * v->x - q->x * v->z;
    float tz = q->x * v->y - q->y * v->x;
    float ux = q->y * tz - q->z * ty;
    float uy = q->z * tx - q->x * tz;
    float uz = q->x * ty - q->y * tx;
    pp_vec3_set(ret,
                v->x + 2.0f * (q->w * tx + ux),
                v->y + 2.0f * (q->w * ty + uy),
                v->z + 2.0f * (q->w * tz + uz));
#endif
}

void pp_quat_normalize(PPQuaternion *q)
{
    float norm_sq = q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w;
    if (norm_sq > 0) {
        float inv_norm = pp_inv_sqrtf(norm_sq);
        q->x *= inv_norm;
        q->y *= inv_norm;
        q->z *= inv_norm;
        q->w *= inv_norm;
    }
}

static inline void pp_quat_conjugate(const PPQuaternion *q, PPQuaternion *out)
{
    out->x = -q->x;
    out->y = -q->y;
    out->z = -q->z;
    out->w = q->w;
}

void pp_quat_forward(const PPQuaternion *q, PPVec3 *out)
{
    float x = q->x;
    float y = q->y;
    float z = q->z;
    float w = q->w;

    out->x = 2.0f * (x * z + w * y);
    out->y = 2.0f * (y * z - w * x);
    out->z = 1.0f - 2.0f * (x * x + y * y);
}

void pp_quat_up(const PPQuaternion *q, PPVec3 *out)
{
    float x = q->x;
    float y = q->y;
    float z = q->z;
    float w = q->w;

    out->x = 2.0f * (x * y - w * z);
    out->y = 1.0f - 2.0f * (x * x + z * z);
    out->z = 2.0f * (y * z + w * x);
}

void pp_quat_right(const PPQuaternion *q, PPVec3 *out)
{
    float x = q->x;
    float y = q->y;
    float z = q->z;
    float w = q->w;

    out->x = 1.0f - 2.0f * (y * y + z * z);
    out->y = 2.0f * (x * y + w * z);
    out->z = 2.0f * (x * z - w * y);
}

void pp_quat_between(const PPVec3 *v0, const PPVec3 *v1, PPQuaternion *result)
{
    float dot = pp_vec3_dot(v0, v1);

    // If the vectors are exactly opposite, return a 180-degree rotation around an arbitrary axis
    if (dot < -1.0f + 1e-6f) {
        // Rotate around the Y axis
        result->x = 0.0f;
        result->y = 1.0f;
        result->z = 0.0f;
        result->w = 0.0f;
        return;
    }

    // If the vectors are exactly the same, return the identity quaternion
    if (dot > 1.0f - 1e-6f) {
        result->x = 0.0f;
        result->y = 0.0f;
        result->z = 0.0f;
        result->w = 1.0f;
        return;
    }

    // Calculate the axis of rotation
    PPVec3 axis;
    axis.xyz[0] = v0->y * v1->z - v0->z * v1->y;
    axis.xyz[1] = v0->z * v1->x - v0->x * v1->z;
    axis.xyz[2] = v0->x * v1->y - v0->y * v1->x;

    pp_vec3_normalize(&axis);

    // Calculate the angle of rotation
    float angle = pp_acosf(dot);

    // Calculate the quaternion
    float half_angle = angle * 0.5f;
    float sin_half_angle = pp_sinf(half_angle);
    result->x = axis.xyz[0] * sin_half_angle;
    result->y = axis.xyz[1] * sin_half_angle;
    result->z = axis.xyz[2] * sin_half_angle;
    result->w = pp_cosf(half_angle);
}

void pp_quat_slerp(const PPQuaternion *q0, const PPQuaternion *q1, float t, PPQuaternion *result)
{
    float dot = q0->x * q1->x + q0->y * q1->y + q0->z * q1->z + q0->w * q1->w;

    PPQuaternion q1_temp;
    if (dot < 0.0f) {
        q1_temp.xyzw[0] = -q1->x;
        q1_temp.xyzw[1] = -q1->y;
        q1_temp.xyzw[2] = -q1->z;
        q1_temp.xyzw[3] = -q1->w;
        dot = -dot;
    } else {
        q1_temp = *q1;
    }

    // If the quaternions are very close, use linear interpolation
    if (dot > 0.9995f) {
        result->x = q0->x + t * (q1_temp.xyzw[0] - q0->x);
        result->y = q0->y + t * (q1_temp.xyzw[1] - q0->y);
        result->z = q0->z + t * (q1_temp.xyzw[2] - q0->z);
        result->w = q0->w + t * (q1_temp.xyzw[3] - q0->w);
        return;
    }

    // Calculate the angle between the quaternions
    float theta = pp_acosf(dot);

    // Calculate the coefficients for spherical linear interpolation
    float sin_theta = pp_sinf(theta);
    float s0 = pp_sinf((1.0f - t) * theta) / sin_theta;
    float s1 = pp_sinf(t * theta) / sin_theta;

    // Perform the interpolation
    result->x = s0 * q0->x + s1 * q1_temp.xyzw[0];
    result->y = s0 * q0->y + s1 * q1_temp.xyzw[1];
    result->z = s0 * q0->z + s1 * q1_temp.xyzw[2];
    result->w = s0 * q0->w + s1 * q1_temp.xyzw[3];
}

bool pp_box_intersect(
    const PPVec3 *box_pos, const PPQuaternion *box_rot, const PPVec3 *whd, const PPVec3 *o, const PPVec3 *d, PPVec3 *out, float *distance);
bool pp_sphere_intersect(
    const PPVec3 *sphere_pos, float radius, const PPVec3 *o, const PPVec3 *d, PPVec3 *out, float *distance);
bool pp_tri_intersect(
    const PPTriangle *tri, const PPVec3 *o, const PPVec3 *d, PPVec3 *out, float *distance);
bool pp_capsule_intersect(const PPVec3 *a, const PPVec3 *b, float radius,
                          const PPVec3 *o, const PPVec3 *d, PPVec3 *out, float *distance);

/* Ray against a capsule with end points a and b. `d` must be unit length. */
bool pp_capsule_intersect(const PPVec3 *a, const PPVec3 *b, float radius,
                          const PPVec3 *o, const PPVec3 *d, PPVec3 *out, float *distance)
{
    float best = FLT_MAX;
    PPVec3 best_hit = {.xyz = {0.0f, 0.0f, 0.0f}};

    // The two caps.
    const PPVec3 *ends[2] = {a, b};
    for (int i = 0; i < 2; ++i) {
        PPVec3 hit;
        float t;
        if (pp_sphere_intersect(ends[i], radius, o, d, &hit, &t) && t < best) {
            best = t;
            best_hit = hit;
        }
    }

    // The side: an infinite cylinder, accepted only between the two ends.
    PPVec3 u, m;
    pp_vec3_sub(b, a, &u);
    float len = pp_vec3_length(&u);
    if (len > 1e-6f) {
        pp_vec3_scale(&u, pp_invf(len), &u);
        pp_vec3_sub(o, a, &m);

        float mu = pp_vec3_dot(&m, &u);
        float du = pp_vec3_dot(d, &u);
        PPVec3 mp = {.xyz = {m.x - u.x * mu, m.y - u.y * mu, m.z - u.z * mu}};
        PPVec3 dp = {.xyz = {d->x - u.x * du, d->y - u.y * du, d->z - u.z * du}};

        float A = pp_vec3_dot(&dp, &dp);
        float B = pp_vec3_dot(&mp, &dp);
        float C = pp_vec3_dot(&mp, &mp) - radius * radius;
        float disc = B * B - A * C;
        if (A > 1e-8f && disc >= 0.0f) {
            float t = (-B - pp_sqrtf(disc)) / A;
            float along = mu + du * t;
            if (t > 0.0f && t < best && along >= 0.0f && along <= len) {
                best = t;
                pp_vec3_scale(d, t, &best_hit);
                pp_vec3_add(o, &best_hit, &best_hit);
            }
        }
    }

    if (distance) *distance = best;
    if (best == FLT_MAX) return false;
    if (out) *out = best_hit;
    return true;
}

bool pp_contains_kind(BodyKind *kinds, BodyKind kind)
{
    if (!kinds) {
        return false;
    }

    BodyKind *k = kinds;
    while (*k) {
        if (*k == kind) {
            return true;
        }
        k++;
    }

    return false;
}

/**
 * Intersects the world with the specified ray. Returns true if something was hit.
 *
 * If a hit was detected, then either sphere_hit or tri_hit will be populated (depending on what was hit) and the distance from the
 * origin to the hit will be returned.
 *
 * ignore_kinds is an array of kinds to ignore, with a 0 terminated final entry.
 *
 * The ray is unbounded, so this tests every triangle in the global tris[]
 * array and cannot see a world supplied through pp_physics_set_triangle_query().
 * Use pp_physics_ray_cast() for that, and for anything called every frame.
 */
bool pp_physics_ray_intersect(const PPVec3 *origin,
                              const PPVec3 *direction,
                              BodyKind *ignore_kinds,
                              const PPBody **body_hit,
                              const PPTriangle **tri_hit,
                              float *distance,
                              PPVec3 *intersection)
{
    float closest_dist = FLT_MAX;
    const PPTriangle *closest_tri = NULL;
    const PPBody *closest_body = NULL;
    PPVec3 closest_intersection = {.xyz={0.0f, 0.0f, 0.0f}};

    for (size_t i = 0; i < pp_physics_triangle_count(); ++i) {
        const PPTriangle *t = pp_physics_triangle_at(i);

        PPVec3 hit;
        float dist;
        if (pp_tri_intersect(t, origin, direction, &hit, &dist)) {
            if (pp_contains_kind(ignore_kinds, t->kind)) {
                continue;
            }

            if (dist < closest_dist) {
                closest_tri = t;
                closest_dist = dist;
                pp_vec3_assign(&closest_intersection, &hit);
            }
        }
    }

    for (size_t i = 0; i < pp_physics_body_total_count(); ++i) {
        const PPBody *body = pp_physics_body_at(i);
        if (!body->is_alive) {
            continue;
        }

        for (int si = 0; si < body->shape_count; ++si) {
            const PPShape *shape = &body->shapes[si];
            PPVec3 shape_world_pos;
            PPVec3 rotated_offset;
            pp_quat_transform(&body->rot, &shape->offset, &rotated_offset);
            pp_vec3_add(&body->pos, &rotated_offset, &shape_world_pos);

            if (pp_contains_kind(ignore_kinds, shape->kind)) {
                continue;
            }

            PPVec3 hit;
            float dist;
            bool did_hit = false;

            if (shape->type == PP_OBJECT_TYPE_SPHERE) {
                did_hit = pp_sphere_intersect(&shape_world_pos, shape->sphere.radius, origin, direction, &hit, &dist);
            } else if (shape->type == PP_OBJECT_TYPE_BOX) {
                did_hit = pp_box_intersect(&shape_world_pos, &body->rot, &shape->box.whd, origin, direction, &hit, &dist);
            } else if (shape->type == PP_OBJECT_TYPE_CAPSULE) {
                PPVec3 up = {.xyz = {0.0f, shape->capsule.half_height, 0.0f}}, axis, a, b;
                pp_quat_transform(&body->rot, &up, &axis);
                pp_vec3_sub(&shape_world_pos, &axis, &a);
                pp_vec3_add(&shape_world_pos, &axis, &b);
                did_hit = pp_capsule_intersect(&a, &b, shape->capsule.radius, origin, direction, &hit, &dist);
            }

            if (did_hit && dist < closest_dist) {
                closest_body = body;
                closest_tri = NULL;
                closest_dist = dist;
                pp_vec3_assign(&closest_intersection, &hit);
            }
        }
    }

    if (closest_dist == FLT_MAX) {
        return false;
    }

    if (body_hit) {
        *body_hit = closest_body;
    }

    if (tri_hit) {
        *tri_hit = (PPTriangle *) closest_tri;
    }
    if (distance) {
        *distance = closest_dist;
    }
    if (intersection) {
        pp_vec3_assign(intersection, &closest_intersection);
    }
    return true;
}

bool pp_aabb_intersect(const PPVec3 *pos,
                       const PPVec3 *whd,
                       const PPVec3 *origin,
                       const PPVec3 *direction,
                       PPVec3 *out,
                       float *distance)
{
    PPVec3 extents = {.xyz={whd->x * 0.5f, whd->y * 0.5f, whd->z * 0.5f}};
    PPVec3 min = {.xyz={pos->x - extents.x, pos->y - extents.y, pos->z - extents.z}};
    PPVec3 max = {.xyz={pos->x + extents.x, pos->y + extents.y, pos->z + extents.z}};
    PPVec3 n_inv = {.xyz={1.0f / direction->x, 1.0f / direction->y, 1.0f / direction->z}};

    const float t1 = (min.x - origin->x) * n_inv.x;
    const float t2 = (max.x - origin->x) * n_inv.x;
    const float t3 = (min.y - origin->y) * n_inv.y;
    const float t4 = (max.y - origin->y) * n_inv.y;
    const float t5 = (min.z - origin->z) * n_inv.z;
    const float t6 = (max.z - origin->z) * n_inv.z;

    const float tmin = fmaxf(fmaxf(fminf(t1, t2), fminf(t3, t4)), fminf(t5, t6));
    const float tmax = fminf(fminf(fmaxf(t1, t2), fmaxf(t3, t4)), fmaxf(t5, t6));

    // if tmax < 0, ray (line) is intersecting AABB, but whole AABB is behind us
    if (tmax < 0) {
        return false;
    }

    // if tmin > tmax, ray doesn't intersect AABB
    if (tmin > tmax) {
        return false;
    }

    *distance = tmin;
    pp_vec3_scale(direction, tmin, out);
    pp_vec3_add(origin, out, out);
    return true;
}

bool pp_box_intersect(
    const PPVec3 *box_pos, const PPQuaternion *box_rot, const PPVec3 *whd, const PPVec3 *o, const PPVec3 *d, PPVec3 *out, float *distance)
{
    // Into the box's own frame, where it is axis aligned about the origin.
    PPQuaternion inv;
    PPVec3 rel, lo, ld, lhit;
    PPVec3 zero = {.xyz = {0.0f, 0.0f, 0.0f}};
    pp_quat_conjugate(box_rot, &inv);
    pp_vec3_sub(o, box_pos, &rel);
    pp_quat_transform(&inv, &rel, &lo);
    pp_quat_transform(&inv, d, &ld);

    if (!pp_aabb_intersect(&zero, whd, &lo, &ld, &lhit, distance)) {
        return false;
    }

    pp_quat_transform(box_rot, &lhit, out);
    pp_vec3_add(box_pos, out, out);
    return true;
}

bool pp_sphere_intersect(
    const PPVec3 *sphere_pos, float radius, const PPVec3 *o, const PPVec3 *d, PPVec3 *out, float *distance)
{
    PPVec3 oc;
    pp_vec3_sub(sphere_pos, o, &oc);

    float b = pp_vec3_dot(&oc, d);
    float c = pp_vec3_dot(&oc, &oc) - radius * radius;

    float discriminant = b * b - c;

    // No matter what, always init the distance
    if (distance) {
        *distance = FLT_MAX;
    }

    // If the discriminant is negative, there are no real roots, so no intersection
    if (discriminant < 0) {
        return false;
    }

    // Calculate the two points of intersection. oc points from the ray origin
    // to the sphere centre, so b = oc·d is positive when the sphere is ahead of
    // the origin and the roots are t = b ± sqrt(disc). (Using -b here would only
    // ever return hits for spheres *behind* the ray.)
    float sqrtDiscriminant = pp_sqrtf(discriminant);
    float t1 = b - sqrtDiscriminant;
    float t2 = b + sqrtDiscriminant;

    // Check if the points of intersection are in front of the ray origin
    if (t1 > 0 && (t2 <= 0 || t1 < t2)) {
        PPVec3 add;
        pp_vec3_scale(d, t1, &add);
        pp_vec3_add(o, &add, out);
        if (distance) {
            *distance = t1;
        }
        return true;
    } else if (t2 > 0) {
        PPVec3 add;
        pp_vec3_scale(d, t2, &add);
        pp_vec3_add(o, &add, out);
        if (distance) {
            *distance = t2;
        }
        return true;
    }

    return false;
}

bool pp_tri_intersect(
    const PPTriangle *tri, const PPVec3 *o, const PPVec3 *d, PPVec3 *out, float *distance)
{
    const float e = FLT_EPSILON;

    // Möller-Trumbore ray-triangle intersection, optimized with direct arithmetic
    float edge1_x = tri->v[1].x - tri->v[0].x;
    float edge1_y = tri->v[1].y - tri->v[0].y;
    float edge1_z = tri->v[1].z - tri->v[0].z;
    float edge2_x = tri->v[2].x - tri->v[0].x;
    float edge2_y = tri->v[2].y - tri->v[0].y;
    float edge2_z = tri->v[2].z - tri->v[0].z;

    // cross_e2 = d × edge2
    float cross_e2_x = d->y * edge2_z - d->z * edge2_y;
    float cross_e2_y = d->z * edge2_x - d->x * edge2_z;
    float cross_e2_z = d->x * edge2_y - d->y * edge2_x;

    // det = edge1 · cross_e2
    float det = edge1_x * cross_e2_x + edge1_y * cross_e2_y + edge1_z * cross_e2_z;

    if (det > -e && det < e) {
        return false;
    }

    float inv_det = pp_invf(det);

    // s = o - v0
    float s_x = o->x - tri->v[0].x;
    float s_y = o->y - tri->v[0].y;
    float s_z = o->z - tri->v[0].z;

    float u = inv_det * (s_x * cross_e2_x + s_y * cross_e2_y + s_z * cross_e2_z);
    if ((u < 0.0f && -u > e) || (u > 1.0f && fabsf(u - 1.0f) > e)) {
        return false;
    }

    // cross_e1 = s × edge1
    float cross_e1_x = s_y * edge1_z - s_z * edge1_y;
    float cross_e1_y = s_z * edge1_x - s_x * edge1_z;
    float cross_e1_z = s_x * edge1_y - s_y * edge1_x;

    float v = inv_det * (d->x * cross_e1_x + d->y * cross_e1_y + d->z * cross_e1_z);
    if ((v < 0.0f && -v > e) || (u + v > 1.0f && fabsf(u + v - 1.0f) > e)) {
        return false;
    }

    float t = inv_det * (edge2_x * cross_e1_x + edge2_y * cross_e1_y + edge2_z * cross_e1_z);
    if (t <= e) {
        return false;
    }

    if (out) {
        out->x = o->x + d->x * t;
        out->y = o->y + d->y * t;
        out->z = o->z + d->z * t;
    }

    if (distance) {
        *distance = t;
    }

    return true;
}

void pp_body_set_kind(PPBody *b, BodyKind kind)
{
    b->kind = kind;
    for (int i = 0; i < b->shape_count; ++i) {
        b->shapes[i].kind = kind;
    }
}

BodyKind pp_body_get_kind(const PPBody *b)
{
    return b->kind;
}

void pp_body_set_angular_velocity(PPBody *s, float x, float y, float z)
{
    pp_body_wake(s);
    pp_vec3_set(&s->a_vel, x, y, z);
}

void pp_body_set_velocity(PPBody *s, float x, float y, float z)
{
    pp_body_wake(s);
    pp_vec3_set(&s->vel, x, y, z);
}

void pp_body_set_angular_acceleration(PPBody *s, float x, float y, float z)
{
    pp_body_wake(s);
    pp_vec3_set(&s->a_acc, x, y, z);
}

void pp_body_set_acceleration(PPBody *s, float x, float y, float z)
{
    pp_body_wake(s);
    pp_vec3_set(&s->acc, x, y, z);
}

void pp_body_set_gravity_multiplier(PPBody *b, float multiplier) {
    pp_body_wake(b);
    b->gravity_multiplier = multiplier;
}

float pp_body_get_gravity_multiplier(const PPBody *b) {
    return b->gravity_multiplier;
}

void pp_body_set_angular_damping(PPBody *s, float d)
{
    if (d < 0.0f || d > 1.0f) {
        return;
    }

    s->a_damping = d;
}

void pp_body_set_damping(PPBody *s, float d)
{
    if (d < 0.0f || d > 1.0f) {
        return;
    }

    s->damping = d;
}

void pp_body_set_rolling_resistance(PPBody *s, float r)
{
    s->rolling_resistance = (r < 0.0f) ? 0.0f : r;
}

void pp_body_move_kinematic(PPBody *s, const PPVec3 *target, float dt)
{
    if (dt <= 0.0f) return;

    pp_body_set_velocity(s,
                         (target->x - s->pos.x) / dt,
                         (target->y - s->pos.y) / dt,
                         (target->z - s->pos.z) / dt);
}

void pp_body_wake(PPBody *b)
{
    if (!b) return;

    b->sleep_time = 0.0f;
    if (!b->is_asleep) return;

    // Everything that fell asleep as part of the same resting pile wakes
    // together: they were leaning on each other.
    uint32_t group = b->sleep_group;
    for (int i = 0; i < object_count; ++i) {
        PPBody *o = &objects[i];
        if (o->is_alive && o->is_asleep && o->sleep_group == group) {
            o->is_asleep = false;
            o->sleep_time = 0.0f;
        }
    }
}

bool pp_body_is_asleep(const PPBody *b)
{
    return b->is_asleep;
}

void pp_body_set_never_sleeps(PPBody *b, bool never)
{
    b->never_sleeps = never;
    if (never) pp_body_wake(b);
}

void pp_physics_wake_all()
{
    for (int i = 0; i < object_count; ++i) {
        objects[i].is_asleep = false;
        objects[i].sleep_time = 0.0f;
    }
}

void pp_physics_set_sleeping_enabled(bool enabled)
{
    sleeping_enabled = enabled;
    if (!enabled) pp_physics_wake_all();
}

bool pp_physics_get_sleeping_enabled()
{
    return sleeping_enabled;
}

void pp_physics_set_sleep_thresholds(float linear, float angular, float seconds)
{
    sleep_linear_threshold = linear;
    sleep_angular_threshold = angular;
    sleep_delay = seconds;
}

static void pp_body_recompute_mass_inertia(PPBody *body);
static inline void pp_body_world_com(const PPBody *body, PPVec3 *out);

void pp_body_set_mass(PPBody* b, float mass) {
    pp_body_wake(b);
    // Scale all shape masses proportionally
    if (b->mass > 0.0f && b->shape_count > 0) {
        float ratio = mass / b->mass;
        for (int i = 0; i < b->shape_count; ++i) {
            b->shapes[i].mass *= ratio;
        }
        pp_body_recompute_mass_inertia(b);
    } else {
        b->mass = mass;
        b->inv_mass = (mass == 0.0f) ? 0.0f : 1.0f / mass;
    }
}

float pp_body_get_mass(const PPBody* b) {
    return b->mass;
}

void pp_body_get_forward(PPBody *s, PPVec3 *f)
{
    pp_quat_forward(&s->rot, f);
}

void pp_body_get_up(PPBody *s, PPVec3 *u)
{
    pp_quat_up(&s->rot, u);
}

void pp_body_get_right(PPBody *s, PPVec3 *r)
{
    pp_quat_right(&s->rot, r);
}

size_t pp_body_get_shape_count(const PPBody *body)
{
    return body->shape_count;
}

const PPShape *pp_body_get_shape(const PPBody *body, size_t index)
{
    assert(index < (size_t)body->shape_count);
    return &body->shapes[index];
}

size_t pp_body_get_constraint_count(PPBody *body) {
    size_t count = 0;
    for(int i = 0; i < constraint_count; ++i) {
        if(!constraints[i].is_alive) {
            continue;
        }

        if (constraints[i].body1 == body || constraints[i].body2 == body) {
            count++;
        }
    }

    return count;
}

float pp_body_get_radius(PPBody *s)
{
    float max_r = 0.0f;
    for (int i = 0; i < s->shape_count; ++i) {
        const PPShape *shape = &s->shapes[i];
        float offset_len = pp_vec3_length(&shape->offset);
        float r = 0.0f;
        if (shape->type == PP_OBJECT_TYPE_SPHERE) {
            r = shape->sphere.radius + offset_len;
        } else if (shape->type == PP_OBJECT_TYPE_BOX) {
            r = shape->box.bounding_radius + offset_len;
        } else if (shape->type == PP_OBJECT_TYPE_CAPSULE) {
            r = shape->capsule.radius + shape->capsule.half_height + offset_len;
        }
        if (r > max_r) max_r = r;
    }
    return max_r;
}

void pp_quat_from_axis_angle(PPQuaternion *q, const PPVec3 *axis, float angle)
{
    PPVec3 a;
    pp_vec3_assign(&a, axis);
    pp_vec3_normalize(&a);

    float half = angle * 0.5f;
    float s = pp_sinf(half); // sin(θ/2)
    float c = pp_cosf(half); // cos(θ/2)

    q->x = a.xyz[0] * s; // axis.x * sin(θ/2)
    q->y = a.xyz[1] * s; // axis.y * sin(θ/2)
    q->z = a.xyz[2] * s; // axis.z * sin(θ/2)
    q->w = c;            // cos(θ/2)

    pp_quat_normalize(q);
}

void pp_body_look_at(PPBody *s, float x, float y, float z)
{
    pp_body_wake(s);
    PPVec3 t, f, c;
    pp_vec3_set(&t, x, y, z);
    pp_body_get_forward(s, &f);
    pp_vec3_cross(&f, &t, &c);
    float d = pp_vec3_dot(&f, &t);

    // Build error quaternion
    PPQuaternion q_err;
    if (d < -0.9999f) { // opposite direction
        PPVec3 ortho, axis;
        if (fabsf(f.xyz[0]) < 0.9f) {
            pp_vec3_set(&ortho, 1, 0, 0);
        } else {
            pp_vec3_set(&ortho, 0, 1, 0);
        }

        pp_vec3_cross(&f, &ortho, &axis);
        pp_vec3_normalize(&axis);

        // 180° rotation
        pp_quat_from_axis_angle(&q_err, &axis, M_PI);
    } else {
        PPVec3 axis;
        float s = pp_sqrtf((1.0f + d) * 2.0f);
        axis.xyz[0] = c.xyz[0] / s;
        axis.xyz[1] = c.xyz[1] / s;
        axis.xyz[2] = c.xyz[2] / s;

        pp_quat_set(&q_err, axis.xyz[0], axis.xyz[1], axis.xyz[2], s * 0.5f);
    }

    pp_quat_normalize(&q_err);

    // Angular error vector (approximation)
    PPVec3 error;
    pp_vec3_set(&error, 2.0f * q_err.xyzw[0], 2.0f * q_err.xyzw[1], 2.0f * q_err.xyzw[2]);

    PPVec3 torque, a, b;

    // Configurable
    float kp = 10.0f;
    float kd = 1.0f;

    // Vector3 torque = -cfg.kp * error - cfg.kd * a_vel;
    pp_vec3_scale(&error, -kp, &a);
    pp_vec3_scale(&s->a_vel, kd, &b);
    pp_vec3_sub(&a, &b, &torque);
    pp_body_add_angular_force(s, torque.xyz[0], torque.xyz[1], torque.xyz[2]);
}

void pp_body_limit_velocity(PPBody *body, float speed)
{
    if (speed < 0.0f) {
        return;
    }

    body->vel_limit = speed;
}

void pp_body_limit_angular_velocity(PPBody *body, float speed)
{
    if (speed < 0.0f) {
        return;
    }

    body->a_vel_limit = speed;
}

static void pp_body_init(PPBody *body, const PPVec3 *pos, float mass, BodyKind kind)
{
    memset(body, 0, sizeof(PPBody));
    body->is_alive = true;
    body->user_data = NULL;
    body->shape_count = 0;

    pp_vec3_init(&body->vel);
    pp_vec3_init(&body->acc);
    pp_vec3_init(&body->a_vel);
    pp_vec3_init(&body->a_acc);
    pp_quat_init(&body->rot);

    if (pos) {
        pp_vec3_set(&body->pos, pos->x, pos->y, pos->z);
    } else {
        pp_vec3_set(&body->pos, 0.0f, 0.0f, 0.0f);
    }

    body->kind = kind;
    body->mass = mass;
    body->inv_mass = (mass == 0.0f) ? 0.0f : 1.0f / mass;
    body->friction = 0.75f;
    body->rolling_resistance = 0.05f;
    body->damping = 0.01f;
    body->a_damping = 0.02f;
    body->vel_limit = 0.0f;
    body->a_vel_limit = 0.0f;
    body->gravity_multiplier = 1.0f;
    pp_body_set_bounce(body, 0.5f);
}

/* inv_inertia_world = R * inv_inertia * R^T, with R the body's rotation. */
static void pp_body_update_world_inertia(PPBody *body)
{
    static const PPVec3 basis[3] = {
        {.xyz = {1.0f, 0.0f, 0.0f}},
        {.xyz = {0.0f, 1.0f, 0.0f}},
        {.xyz = {0.0f, 0.0f, 1.0f}},
    };

    PPVec3 axes[3];
    for (int i = 0; i < 3; ++i) {
        pp_quat_transform(&body->rot, &basis[i], &axes[i]);
    }

    // tmp column j = R * (column j of inv_inertia)
    PPVec3 tmp[3];
    for (int j = 0; j < 3; ++j) {
        const float *c = &body->inv_inertia.m[j * 3];
        for (int k = 0; k < 3; ++k) {
            tmp[j].xyz[k] = axes[0].xyz[k] * c[0] + axes[1].xyz[k] * c[1] + axes[2].xyz[k] * c[2];
        }
    }

    // out = tmp * R^T: out[r][c] = sum_j tmp[j][r] * axes[j][c]
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) {
            body->inv_inertia_world.m[c * 3 + r] = tmp[0].xyz[r] * axes[0].xyz[c] +
                                                   tmp[1].xyz[r] * axes[1].xyz[c] +
                                                   tmp[2].xyz[r] * axes[2].xyz[c];
        }
    }

    // A locked axis behaves as infinite inertia about that world axis, so
    // contact impulses cannot spin the body around it (rather than spinning
    // it for a step and having the velocity zeroed afterwards).
    if (body->lock) {
        const PPAxisLock locks[3] = {PP_AXIS_LOCK_PITCH, PP_AXIS_LOCK_YAW, PP_AXIS_LOCK_ROLL};
        for (int a = 0; a < 3; ++a) {
            if ((body->lock & locks[a]) != locks[a]) continue;
            for (int k = 0; k < 3; ++k) {
                body->inv_inertia_world.m[a * 3 + k] = 0.0f;
                body->inv_inertia_world.m[k * 3 + a] = 0.0f;
            }
        }
    }
}

static void pp_body_recompute_mass_inertia(PPBody *body)
{
    float total_mass = 0.0f;
    memset(body->inertia.m, 0, sizeof(body->inertia.m));

    // Centre of mass first: the inertia below is taken about it, which is what
    // lets linear and angular motion be integrated independently.
    PPVec3 com = {.xyz = {0.0f, 0.0f, 0.0f}};
    for (int i = 0; i < body->shape_count; ++i) {
        const PPShape *s = &body->shapes[i];
        if (s->mass <= 0.0f) continue;
        com.x += s->mass * s->offset.x;
        com.y += s->mass * s->offset.y;
        com.z += s->mass * s->offset.z;
        total_mass += s->mass;
    }
    if (total_mass > 0.0f) {
        pp_vec3_scale(&com, 1.0f / total_mass, &com);
    }
    body->com_local = com;
    total_mass = 0.0f;

    for (int i = 0; i < body->shape_count; ++i) {
        const PPShape *s = &body->shapes[i];
        float m = s->mass;
        total_mass += m;

        if (m <= 0.0f) continue;

        // Compute local inertia for this shape
        float Ixx = 0.0f, Iyy = 0.0f, Izz = 0.0f;
        if (s->type == PP_OBJECT_TYPE_SPHERE) {
            float r2 = s->sphere.radius * s->sphere.radius;
            Ixx = Iyy = Izz = (2.0f / 5.0f) * m * r2;
        } else if (s->type == PP_OBJECT_TYPE_BOX) {
            float w2 = s->box.whd.x * s->box.whd.x;
            float h2 = s->box.whd.y * s->box.whd.y;
            float d2 = s->box.whd.z * s->box.whd.z;
            const float oot = 1.0f / 12.0f;
            Ixx = oot * m * (h2 + d2);
            Iyy = oot * m * (w2 + d2);
            Izz = oot * m * (w2 + h2);
        } else if (s->type == PP_OBJECT_TYPE_CAPSULE) {
            // A cylinder plus the two halves of a sphere, sharing the mass in
            // proportion to their volumes.
            float r = s->capsule.radius;
            float h = 2.0f * s->capsule.half_height;
            float r2 = r * r;
            float v_cyl = h;                   // common factor pi * r^2 dropped
            float v_sph = (4.0f / 3.0f) * r;
            float m_cyl = m * v_cyl / (v_cyl + v_sph);
            float m_sph = m - m_cyl;

            Iyy = 0.5f * m_cyl * r2 + 0.4f * m_sph * r2;
            Ixx = Izz = m_cyl * (h * h / 12.0f + r2 / 4.0f) +
                        m_sph * (0.4f * r2 + h * h / 4.0f + 0.375f * h * r);
        }

        // Parallel axis theorem: I_total += I_local + m * (d·d * I3 - outer(d,d))
        float dx = s->offset.x - com.x, dy = s->offset.y - com.y, dz = s->offset.z - com.z;
        float d_dot_d = dx*dx + dy*dy + dz*dz;

        body->inertia.m[0] += Ixx + m * (d_dot_d - dx*dx);
        body->inertia.m[4] += Iyy + m * (d_dot_d - dy*dy);
        body->inertia.m[8] += Izz + m * (d_dot_d - dz*dz);
        body->inertia.m[1] += -m * dx * dy;
        body->inertia.m[3] += -m * dx * dy;
        body->inertia.m[2] += -m * dx * dz;
        body->inertia.m[6] += -m * dx * dz;
        body->inertia.m[5] += -m * dy * dz;
        body->inertia.m[7] += -m * dy * dz;
    }

    body->mass = total_mass;
    body->inv_mass = (total_mass == 0.0f) ? 0.0f : 1.0f / total_mass;

    if (total_mass > 0.0f) {
        pp_mat3_inverse(&body->inertia, &body->inv_inertia);
    } else {
        memset(body->inertia.m, 0, sizeof(body->inertia.m));
        memset(body->inv_inertia.m, 0, sizeof(body->inv_inertia.m));
    }

    pp_body_update_world_inertia(body);
}

PPShape *pp_body_add_sphere(PPBody *body, float radius, float mass, BodyKind kind, float offset_x, float offset_y, float offset_z)
{
    pp_body_wake(body);
    assert(body->shape_count < PP_MAX_SHAPES_PER_BODY);
    PPShape *s = &body->shapes[body->shape_count++];
    memset(s, 0, sizeof(PPShape));
    s->type = PP_OBJECT_TYPE_SPHERE;
    s->sphere.radius = radius;
    s->mass = mass;
    s->kind = kind;
    pp_vec3_set(&s->offset, offset_x, offset_y, offset_z);

    // Update body kind to match first shape if not yet set
    if (body->shape_count == 1) {
        body->kind = kind;
    }

    pp_body_recompute_mass_inertia(body);
    return s;
}

PPShape *pp_body_add_box(PPBody *body, float w, float h, float d, float mass, BodyKind kind, float offset_x, float offset_y, float offset_z)
{
    pp_body_wake(body);
    assert(body->shape_count < PP_MAX_SHAPES_PER_BODY);
    PPShape *s = &body->shapes[body->shape_count++];
    memset(s, 0, sizeof(PPShape));
    s->type = PP_OBJECT_TYPE_BOX;
    pp_vec3_set(&s->box.whd, w, h, d);
    pp_vec3_set(&s->box.half_extents, w * 0.5f, h * 0.5f, d * 0.5f);
    s->box.bounding_radius = pp_vec3_length(&s->box.half_extents);
    s->mass = mass;
    s->kind = kind;
    pp_vec3_set(&s->offset, offset_x, offset_y, offset_z);

    if (body->shape_count == 1) {
        body->kind = kind;
    }

    pp_body_recompute_mass_inertia(body);
    return s;
}

void pp_body_set_user_data(PPBody *s, void *data)
{
    s->user_data = data;
}

void *pp_body_get_user_data(const PPBody *s)
{
    return s->user_data;
}

void pp_body_lock_axis(PPBody *s, PPAxisLock lock)
{
    pp_body_wake(s);
    s->lock = lock;
}

void pp_body_get_position(PPBody *s, PPVec3 *pos)
{
    pp_vec3_assign(pos, &s->pos);
}

void pp_body_set_position(PPBody *s, float x, float y, float z)
{
    pp_body_wake(s);
    pp_vec3_set(&s->pos, x, y, z);
}

void pp_body_set_rotation(PPBody *s, float x, float y, float z, float w)
{
    pp_body_wake(s);
    pp_quat_set(&s->rot, x, y, z, w);
}

void pp_body_get_velocity_at_position(const PPBody *b, const PPVec3 *p, PPVec3 *ret)
{
    PPVec3 rel_pos, a_vel_contrib;
    pp_vec3_sub(p, &b->pos, &rel_pos);
    pp_vec3_cross(&b->a_vel, &rel_pos, &a_vel_contrib);
    pp_vec3_add(&b->vel, &a_vel_contrib, ret);
}

void pp_body_get_velocity(const PPBody *s, PPVec3 *vel)
{
    pp_vec3_assign(vel, &s->vel);
}

void pp_body_get_rotation(PPBody *s, PPQuaternion *rot)
{
    pp_quat_assign(rot, &s->rot);
}

bool pp_body_set_friction(PPBody *s, float f)
{
    if (!s || f < 0.0f || f > 1.0f) {
        return false;
    }

    s->friction = f;
    return true;
}

bool pp_body_set_bounce(PPBody *s, float b)
{
    if (!s || b < 0.0f || b > 1.0f) {
        return false;
    }

    s->bounce = b;
    return true;
}

void pp_body_add_force(PPBody *s, float x, float y, float z)
{
    pp_body_wake(s);
    if(!s) {
        return;
    }

    PPVec3 force;
    pp_vec3_set(&force, x, y, z);

    PPVec3 acceleration;

    // Ensure you do not divide by zero
    if (s->mass > 0) {
        // a = F / m
        pp_vec3_scale(&force, 1.0f / s->mass, &acceleration);

        // Add acceleration to the sphere's current acceleration
        pp_vec3_add(&s->acc, &acceleration, &s->acc);
    }
}

void pp_body_add_angular_force(PPBody *s, float tx, float ty, float tz)
{
    pp_body_wake(s);
    PPVec3 torque;
    pp_vec3_set(&torque, tx, ty, tz);

    if (s->mass <= 0.0f)
        return; // nothing to do for mass‑less objects

    // The torque is world-space; the rotation may have been changed by the
    // caller since the last step, so refresh the world tensor first.
    pp_body_update_world_inertia(s);

    PPVec3 ang_acc;
    pp_mat3_mult(&s->inv_inertia_world, &torque, &ang_acc);
    pp_vec3_add(&s->a_acc, &ang_acc, &s->a_acc);
}

void pp_body_add_force_at_position(PPBody *b, const PPVec3 *world_pos, const PPVec3 *force)
{
    // Apply linear force
    pp_body_add_force(b, force->x, force->y, force->z);

    // Calculate torque from offset position
    // torque = (position - center_of_mass) × force
    PPVec3 rel_pos, com;
    pp_body_world_com(b, &com);
    pp_vec3_sub(world_pos, &com, &rel_pos);

    PPVec3 torque;
    pp_vec3_cross(&rel_pos, force, &torque);

    // Apply angular force (torque)
    pp_body_add_angular_force(b, torque.x, torque.y, torque.z);
}

const struct _PPCollisionMapEntry *pp_physics_collision_map_search(BodyKind kind1, BodyKind kind2)
{
    for (int i = 0; i < collision_map_count; ++i) {
        struct _PPCollisionMapEntry *entry = &collision_map[i];
        if ((entry->kind1 == kind1 && entry->kind2 == kind2)
            || (entry->kind2 == kind1 && entry->kind1 == kind2)) {
            return entry;
        }
    }

    return NULL;
}

bool pp_physics_collision_map_add(BodyKind kind1,
                                  BodyKind kind2,
                                  void *user_data,
                                  bool (*callback)(const void *,
                                                   const void *,
                                                   BodyKind,
                                                   BodyKind,
                                                   const PPCollision *c,
                                                   const void *))
{
    if (!pp_physics_collision_map_search(kind1, kind2)) {
        struct _PPCollisionMapEntry *entry = &collision_map[collision_map_count++];
        entry->kind1 = kind1;
        entry->kind2 = kind2;
        entry->user_data = user_data;
        entry->collision_callback = callback;
        return true;
    }

    return false;
}

static inline void pp_fill_collision_info_sphere_box(PPBody *lhs_body,
                                       const PPShape *lhs_shape,
                                       PPBody *rhs_body,
                                       const PPShape *rhs_shape,
                                       const PPVec3 *contact_point,
                                       const PPVec3 *n,
                                       PPCollision *c,
                                       float d)
{
    c->dist = d;
    c->separation = -d; // only reported while overlapping, so never speculative
    c->p.x = contact_point->x; c->p.y = contact_point->y; c->p.z = contact_point->z;
    c->n.x = n->x; c->n.y = n->y; c->n.z = n->z;
    c->obj1 = lhs_body;
    c->obj2 = rhs_body;
    c->obj1_bounce = lhs_body->bounce;
    c->obj2_bounce = rhs_body->bounce;
    c->obj1_friction = lhs_body->friction;
    c->obj2_friction = rhs_body->friction;
    c->type1 = PP_OBJECT_TYPE_SPHERE;
    c->type2 = PP_OBJECT_TYPE_BOX;
    c->kind1 = lhs_shape->kind;
    c->kind2 = rhs_shape->kind;
}

static inline void pp_fill_collision_info_sphere_sphere(PPBody *lhs_body,
                                          const PPShape *lhs_shape,
                                          const PPVec3 *lhs_pos,
                                          PPBody *rhs_body,
                                          const PPShape *rhs_shape,
                                          const PPVec3 *rhs_pos,
                                          float dist,
                                          PPCollision *c)
{
    float total_radius = lhs_shape->sphere.radius + rhs_shape->sphere.radius;
    c->n.x = rhs_pos->x - lhs_pos->x;
    c->n.y = rhs_pos->y - lhs_pos->y;
    c->n.z = rhs_pos->z - lhs_pos->z;
    c->dist = dist - total_radius;
    c->separation = dist - total_radius; // only reported while overlapping
    if (dist > 0) {
        float inv = pp_invf(dist);
        c->n.x *= inv; c->n.y *= inv; c->n.z *= inv;
    }

    float wr1 = lhs_shape->sphere.radius / total_radius;
    float wr2 = rhs_shape->sphere.radius / total_radius;

    c->p.x = lhs_pos->x * wr1 + rhs_pos->x * wr2;
    c->p.y = lhs_pos->y * wr1 + rhs_pos->y * wr2;
    c->p.z = lhs_pos->z * wr1 + rhs_pos->z * wr2;

    c->obj1 = lhs_body;
    c->obj2 = rhs_body;
    c->obj1_bounce = lhs_body->bounce;
    c->obj2_bounce = rhs_body->bounce;
    c->obj1_friction = lhs_body->friction;
    c->obj2_friction = rhs_body->friction;
    c->type1 = PP_OBJECT_TYPE_SPHERE;
    c->type2 = PP_OBJECT_TYPE_SPHERE;
    c->kind1 = lhs_shape->kind;
    c->kind2 = rhs_shape->kind;
}

static inline void pp_fill_collision_info_sphere_triangle(
    PPBody *lhs_body, const PPShape *lhs_shape, const PPTriangle *tri, const PPVec3 *p, float dist, PPCollision *c)
{
    c->n.x = -tri->n.x;
    c->n.y = -tri->n.y;
    c->n.z = -tri->n.z;
    c->p.x = p->x;
    c->p.y = p->y;
    c->p.z = p->z;
    c->dist = dist;
    c->separation = -dist; // dist is penetration (radius - perp distance)
    c->obj1 = lhs_body;
    c->obj2 = &trimesh_body;
    c->obj1_bounce = lhs_body->bounce;
    c->obj2_bounce = 0.0f;
    c->obj1_friction = lhs_body->friction;
    c->obj2_friction = tri->friction;
    c->type1 = PP_OBJECT_TYPE_SPHERE;
    c->type2 = PP_OBJECT_TYPE_TRIANGLE;
    c->kind1 = lhs_shape->kind;
    c->kind2 = tri->kind;
}

PPBody *pp_physics_create_body(const PPVec3 *pos)
{
    PPBody *ret = NULL;

    if (dead_object_count) {
        for (int i = 0; i < object_count; ++i) {
            PPBody *body = &objects[i];
            if (!body->is_alive) {
                dead_object_count--;
                ret = body;
                break;
            }
        }
    }

    if (!ret) {
        ret = &objects[object_count++];
    }

    pp_body_init(ret, pos, 0.0f, 0);
    return ret;
}

PPBody *pp_physics_create_sphere(float radius, const PPVec3 *pos, float mass, BodyKind kind)
{
    PPBody *body = pp_physics_create_body(pos);
    pp_body_add_sphere(body, radius, mass, kind, 0.0f, 0.0f, 0.0f);
    return body;
}

static PPConstraint* pp_constraint_alloc(void) {
    PPConstraint* entry = NULL;

    if(dead_constraint_count) {
        for (int i = 0; i < constraint_count; ++i) {
            PPConstraint* c = &constraints[i];
            if (!c->is_alive) {
                dead_constraint_count--;
                entry = c;
                break;
            }
        }
    } else if(constraint_count < PICOPHYSICS_MAX_CONSTRAINTS) {
        entry = &constraints[constraint_count++];
    }

    if(entry) {
        memset(entry, 0, sizeof(PPConstraint));
    }

    return entry;
}

PPConstraint* pp_physics_create_fixed_distance_constraint(PPBody* body1, PPBody* body2, float distance) {
    PPConstraint* entry = pp_constraint_alloc();
    if(!entry) {
        return NULL;
    }

    entry->is_alive = true;
    entry->body1 = body1;
    entry->body2 = body2;
    entry->type = PP_CONSTRAINT_TYPE_FIXED_DISTANCE;
    entry->fixed_distance.distance = distance;
    entry->inv_mass_sum = body1->inv_mass + body2->inv_mass;
    return entry;
}

PPShape *pp_body_add_capsule(PPBody *body, float radius, float height, float mass, BodyKind kind, float offset_x, float offset_y, float offset_z)
{
    pp_body_wake(body);
    assert(body->shape_count < PP_MAX_SHAPES_PER_BODY);
    PPShape *s = &body->shapes[body->shape_count++];
    memset(s, 0, sizeof(PPShape));
    s->type = PP_OBJECT_TYPE_CAPSULE;
    s->capsule.radius = radius;
    s->capsule.half_height = fmaxf(0.0f, 0.5f * height - radius);
    s->mass = mass;
    s->kind = kind;
    pp_vec3_set(&s->offset, offset_x, offset_y, offset_z);

    if (body->shape_count == 1) {
        body->kind = kind;
    }

    pp_body_recompute_mass_inertia(body);
    return s;
}

PPBody *pp_physics_create_capsule(float radius, float height, const PPVec3 *pos, float mass, BodyKind kind)
{
    PPBody *body = pp_physics_create_body(pos);
    if (!body) {
        return NULL;
    }

    pp_body_add_capsule(body, radius, height, mass, kind, 0.0f, 0.0f, 0.0f);
    return body;
}

PPBody *pp_physics_create_box(
    float width, float height, float depth, const PPVec3 *pos, float mass, BodyKind kind)
{
    PPBody *body = pp_physics_create_body(pos);
    pp_body_add_box(body, width, height, depth, mass, kind, 0.0f, 0.0f, 0.0f);
    return body;
}

void pp_triangle_init(PPTriangle *tri,
                      const PPVec3 *v1,
                      const PPVec3 *v2,
                      const PPVec3 *v3,
                      BodyKind kind)
{
    pp_vec3_assign(&tri->v[0], v1);
    pp_vec3_assign(&tri->v[1], v2);
    pp_vec3_assign(&tri->v[2], v3);

    PPVec3 e1, e2;
    pp_vec3_sub(v2, v1, &e1);
    pp_vec3_sub(v3, v1, &e2);

    pp_vec3_cross(&e1, &e2, &tri->n);
    pp_vec3_normalize(&tri->n);

    tri->bounce = 0.0f;
    tri->friction = 0.9f;
    tri->kind = kind;

    // Precompute AABB for fast sphere culling
    tri->aabb_min_x = fminf(v1->x, fminf(v2->x, v3->x));
    tri->aabb_min_y = fminf(v1->y, fminf(v2->y, v3->y));
    tri->aabb_min_z = fminf(v1->z, fminf(v2->z, v3->z));
    tri->aabb_max_x = fmaxf(v1->x, fmaxf(v2->x, v3->x));
    tri->aabb_max_y = fmaxf(v1->y, fmaxf(v2->y, v3->y));
    tri->aabb_max_z = fmaxf(v1->z, fmaxf(v2->z, v3->z));
}

PPTriangle *pp_physics_create_triangle(const PPVec3 *v1,
                                       const PPVec3 *v2,
                                       const PPVec3 *v3,
                                       BodyKind kind)
{
    PPTriangle *tri = &tris[tri_count++];
    pp_triangle_init(tri, v1, v2, v3, kind);
    return tri;
}

void pp_physics_set_triangle_query(PPTriangleQuery fn, void *user)
{
    tri_query = fn;
    tri_query_user = user;
}

int pp_physics_query_overflow_count()
{
    return tri_query_overflows;
}

int pp_physics_query_max_candidates()
{
    return tri_query_max_candidates;
}

void pp_physics_clear()
{
    tri_count = 0;
    object_count = 0;
    dead_object_count = 0;
    constraint_count = 0;
    dead_constraint_count = 0;
    contact_cache_count = 0;
    sleep_group_counter = 0;
    memset(tris, 0, sizeof(tris));
    memset(objects, 0, sizeof(objects));
    memset(constraints, 0, sizeof(constraints));
}

size_t pp_physics_triangle_count()
{
    return tri_count;
}

size_t pp_physics_body_count()
{
    return object_count - dead_object_count;
}

size_t pp_physics_body_total_count()
{
    return object_count;
}

const PPBody *pp_physics_body_at(size_t i)
{
    return &objects[i];
}

const PPTriangle *pp_physics_triangle_at(size_t i)
{
    return tris + i;
}

void pp_physics_destroy_body(PPBody *b)
{
    if (!b) {
        return;
    }

    // Destroy any constraints
    for(int i = 0; i < constraint_count; ++i) {
        PPConstraint* c = &constraints[i];
        if(c->is_alive && (c->body1 == b || c->body2 == b)) {
            c->is_alive = false;
            ++dead_constraint_count;
        }
    }

    // The slot may be handed to a new body; don't let it inherit impulses.
    for (int i = 0; i < contact_cache_count; ++i) {
        if (contact_cache[i].obj1 == b || contact_cache[i].obj2 == b) {
            contact_cache[i].used = true;
        }
    }

    // Whatever was asleep leaning on this body has lost its support.
    pp_body_wake(b);
    if (b->shape_count > 0) {
        float r = pp_body_get_radius(b) + PP_CONTACT_MARGIN;
        for (int i = 0; i < object_count; ++i) {
            PPBody *o = &objects[i];
            if (o == b || !o->is_alive || !o->is_asleep || o->shape_count == 0) continue;
            float reach = r + pp_body_get_radius(o);
            if (pp_vec3_dist_sq(&o->pos, &b->pos) <= reach * reach) {
                pp_body_wake(o);
            }
        }
    }

    b->is_alive = false;
    ++dead_object_count;
}

void pp_physics_set_gravity(const PPVec3 *v)
{
    pp_vec3_assign(&gravity, v);
    gravity_magnitude = pp_vec3_length(&gravity);
    pp_physics_wake_all();
}

/* Called once per substep with the substep length `t`. `step_t` is the length
 * of the whole step and `last` marks the final substep: damping is applied
 * once per step (so it decays exactly as documented whatever the substep
 * count), and accumulated forces are held across the substeps and cleared at
 * the end. */
static void pp_integrate_forces(float t, float step_t, bool last)
{
    PPVec3 scaled_vel;
    // Apply acceleration to velocity
    for (int i = 0; i < object_count; ++i) {
        PPBody *body = &objects[i];

        if (!body->is_alive || body->inv_mass == 0.0f || body->is_asleep) {
            continue;
        }

        // Apply gravity to acceleration before applying acceleration
        // to velocity
        PPVec3 total_acc, grv;
        pp_vec3_scale(&gravity, body->gravity_multiplier, &grv);
        pp_vec3_add(&body->acc, &grv, &total_acc);

        pp_vec3_scale(&total_acc, t, &scaled_vel);
        pp_vec3_add(&body->vel, &scaled_vel, &body->vel);
        if (last) pp_vec3_init(&body->acc);

        // Apply linear damping (linear approximation of exp(-x) ≈ 1-x for small x)
        if (last) {
            float damp = 1.0f - body->damping * step_t;
            pp_vec3_scale(&body->vel, damp, &body->vel);
        }

        if (body->lock) {
            if ((body->lock & PP_AXIS_LOCK_PITCH) == PP_AXIS_LOCK_PITCH) {
                body->a_acc.xyz[0] = 0.0f;
                body->a_vel.xyz[0] = 0.0f;
            }

            if ((body->lock & PP_AXIS_LOCK_YAW) == PP_AXIS_LOCK_YAW) {
                body->a_acc.xyz[1] = 0.0f;
                body->a_vel.xyz[1] = 0.0f;
            }

            if ((body->lock & PP_AXIS_LOCK_ROLL) == PP_AXIS_LOCK_ROLL) {
                body->a_acc.xyz[2] = 0.0f;
                body->a_vel.xyz[2] = 0.0f;
            }
        }

        PPVec3 scaled_ang_vel; // Temporary variable to store scaled angular velocity
        pp_vec3_scale(&body->a_acc, t, &scaled_ang_vel);
        pp_vec3_add(&body->a_vel, &scaled_ang_vel, &body->a_vel);

        // Apply angular damping (linear approximation of exp(-x) ≈ 1-x for small x)
        if (last) {
            float a_damp = 1.0f - body->a_damping * step_t;
            pp_vec3_scale(&body->a_vel, a_damp, &body->a_vel);
        }

        // Reset the acceleration
        if (last) pp_vec3_init(&body->a_acc);

        float vl_sq = body->vel_limit * body->vel_limit;
        float avl_sq = body->a_vel_limit * body->a_vel_limit;

        // Apply any limits
        if (body->vel_limit != 0.0f && pp_vec3_length_sq(&body->vel) > vl_sq) {
            pp_vec3_normalize(&body->vel);
            pp_vec3_scale(&body->vel, body->vel_limit, &body->vel);
        }

        if (body->a_vel_limit != 0.0f && pp_vec3_length_sq(&body->a_vel) > avl_sq) {
            pp_vec3_normalize(&body->a_vel);
            pp_vec3_scale(&body->a_vel, body->a_vel_limit, &body->a_vel);
        }
    }
}

static void pp_solve_constraint_fixed_distance_velocities(PPBody* b1, PPBody* b2, float inv_sum, float target, float dt) {
    float inv_m1 = b1->inv_mass;
    float inv_m2 = b2->inv_mass;

    PPVec3 delta;
    pp_vec3_sub(&b2->pos, &b1->pos, &delta);

    float dist = pp_vec3_length(&delta);
    if(dist < 1e-6f){
        // Already close enough
        return;
    }

    PPVec3 n;
    pp_vec3_scale(&delta, pp_invf(dist), &n);

    PPVec3 rel_vel;
    pp_vec3_sub(&b2->vel, &b1->vel, &rel_vel);
    float v_rel = pp_vec3_dot(&rel_vel, &n);

    float Cpos = dist - target;

    /* Baumgarte bias */
    const float beta = 0.2f;             // bias factor
    const float slop_vel = 0.001f;       // tiny dead zone for bias
    float bias = 0.0f;
    if (fabsf(Cpos) > slop_vel && dt > 0.0f) {
        // Correct sign: positive Cpos (too far apart) should produce a
        // positive bias so that the computed impulse pulls bodies together.
        bias = (beta / dt) * Cpos;
    }

    /* effective mass (linear only) */
    float K = inv_sum;

    if (K < 1e-8f) {
        return;
    }

    float J = -(v_rel + bias) / K;

    /* clamp impulse to avoid instability */
    const float maxJ = 1000.0f;
    if (J > maxJ) J = maxJ;
    else if (J < -maxJ) J = -maxJ;

    /* apply linear impulses */
    PPVec3 impulse;
    pp_vec3_scale(&n, J, &impulse);

    PPVec3 tmp;
    // b1 gets negative impulse
    pp_vec3_scale(&impulse, inv_m1, &tmp);
    pp_vec3_sub(&b1->vel, &tmp, &b1->vel);

    // b2 gets positive impulse
    pp_vec3_scale(&impulse, inv_m2, &tmp);
    pp_vec3_add(&b2->vel, &tmp, &b2->vel);
}

static void pp_solve_constraint_velocities(PPConstraint* entry, float dt) {
    if(!entry || !entry->is_alive) {
        return;
    }

    PPBody* b1 = entry->body1;
    PPBody* b2 = entry->body2;
    if(!b1 || !b2 || !b1->is_alive || !b2->is_alive || entry->inv_mass_sum < 1e-8f){
        return;
    }

    if (entry->type == PP_CONSTRAINT_TYPE_BALL || entry->type == PP_CONSTRAINT_TYPE_HINGE) {
        // Joints are solved alongside the contacts, see pp_joint_solve().
    } else if (entry->type == PP_CONSTRAINT_TYPE_FIXED_DISTANCE) {
        pp_solve_constraint_fixed_distance_velocities(b1,
                                                      b2,
                                                      entry->inv_mass_sum,
                                                      entry->fixed_distance.distance,
                                                      dt);
    } else {
        fprintf(stderr, "Constraint unimplemented\n");
    }
}

static void pp_solve_constraint_fixed_distance_positions(PPBody* b1, PPBody* b2, float inv_mass_sum, float target, float dt) {
    float inv_m1 = b1->inv_mass;
    float inv_m2 = b2->inv_mass;

    PPVec3 delta;
    pp_vec3_sub(&b2->pos, &b1->pos, &delta);

    float current = pp_vec3_length(&delta);
    if (current < 1e-6f) {
        // degenerate; nothing sensible to do
        return;
    }

    PPVec3 n;
    pp_vec3_scale(&delta, pp_invf(current), &n);

    float Cpos = current - target;

    const float slop = 0.01f;   // positional dead zone
    if (fabsf(Cpos) <= slop) {
        return;
    }

    const float percent = 0.2f; // apply 20% of correction per iteration
    float correction = (Cpos - (Cpos > 0.0f ? slop : -slop)) * percent;

    // distribute correction by inverse mass
    PPVec3 corr;
    // Move bodies TOWARD each other: b1 moves along +n, b2 moves along -n
    pp_vec3_scale(&n, correction * (inv_m1 / inv_mass_sum), &corr);
    pp_vec3_add(&b1->pos, &corr, &b1->pos);

    pp_vec3_scale(&n, correction * (inv_m2 / inv_mass_sum), &corr);
    pp_vec3_sub(&b2->pos, &corr, &b2->pos);
}

static void pp_solve_constraint_positions(PPConstraint *entry, float dt)
{
    if(!entry || !entry->is_alive) {
        return;
    }

    PPBody* b1 = entry->body1;
    PPBody* b2 = entry->body2;
    if(!b1 || !b2 || !b1->is_alive || !b2->is_alive || entry->inv_mass_sum < 1e-8f){
        return;
    }

    if (entry->type == PP_CONSTRAINT_TYPE_BALL || entry->type == PP_CONSTRAINT_TYPE_HINGE) {
        // Soft constraints: the velocity solve already closes the gap.
    } else if(entry->type == PP_CONSTRAINT_TYPE_FIXED_DISTANCE){
        pp_solve_constraint_fixed_distance_positions(b1,
                                                     b2,
                                                     entry->inv_mass_sum,
                                                     entry->fixed_distance.distance,
                                                     dt);
    } else {
        fprintf(stderr, "Constraint unimplemented\n");
    }
}

/* Build an orthonormal tangent basis around n. Called once per manifold per
 * step so friction accumulates along axes that do not move between the
 * velocity iterations. */
static void pp_contact_tangents(const PPVec3 *n, PPVec3 *t1, PPVec3 *t2)
{
    if (fabsf(n->x) >= 0.57735f) {
        t1->x =  n->y; t1->y = -n->x; t1->z = 0.0f;
    } else {
        t1->x = 0.0f;  t1->y =  n->z; t1->z = -n->y;
    }
    pp_vec3_normalize(t1);
    pp_vec3_cross(n, t1, t2);
}

/* Effective mass along `dir` for the contact at manifold->p. */
static float pp_contact_effective_mass(const PPBody *lhs, const PPBody *rhs,
                                       const PPVec3 *lhs_pcp, const PPVec3 *rhs_pcp,
                                       const PPVec3 *dir, float inv_mass_sum)
{
    PPVec3 lhs_rxd, rhs_rxd, i_lhs, i_rhs, lhs_term, rhs_term;

    pp_vec3_cross(lhs_pcp, dir, &lhs_rxd);
    pp_vec3_cross(rhs_pcp, dir, &rhs_rxd);
    pp_mat3_mult(&lhs->inv_inertia_world, &lhs_rxd, &i_lhs);
    pp_mat3_mult(&rhs->inv_inertia_world, &rhs_rxd, &i_rhs);
    pp_vec3_cross(&i_lhs, lhs_pcp, &lhs_term);
    pp_vec3_cross(&i_rhs, rhs_pcp, &rhs_term);

    return inv_mass_sum + pp_vec3_dot(&lhs_term, dir) + pp_vec3_dot(&rhs_term, dir);
}

/* Apply impulse P at the contact: -P on lhs, +P on rhs, linear and angular. */
static void pp_apply_impulse(PPBody *lhs, PPBody *rhs,
                             const PPVec3 *lhs_pcp, const PPVec3 *rhs_pcp,
                             const PPVec3 *P)
{
    PPVec3 tmp, ang;

    pp_vec3_scale(P, lhs->inv_mass, &tmp);
    pp_vec3_sub(&lhs->vel, &tmp, &lhs->vel);

    pp_vec3_scale(P, rhs->inv_mass, &tmp);
    pp_vec3_add(&rhs->vel, &tmp, &rhs->vel);

    pp_vec3_cross(lhs_pcp, P, &ang);
    pp_mat3_mult(&lhs->inv_inertia_world, &ang, &ang);
    pp_vec3_sub(&lhs->a_vel, &ang, &lhs->a_vel);

    pp_vec3_cross(rhs_pcp, P, &ang);
    pp_mat3_mult(&rhs->inv_inertia_world, &ang, &ang);
    pp_vec3_add(&rhs->a_vel, &ang, &rhs->a_vel);
}

/* Relative velocity of the contact point, rhs minus lhs. */
static void pp_contact_rel_vel(const PPBody *lhs, const PPBody *rhs,
                               const PPVec3 *lhs_pcp, const PPVec3 *rhs_pcp,
                               PPVec3 *out)
{
    PPVec3 lhs_vel, rhs_vel;

    pp_vec3_cross(&lhs->a_vel, lhs_pcp, &lhs_vel);
    pp_vec3_add(&lhs_vel, &lhs->vel, &lhs_vel);

    pp_vec3_cross(&rhs->a_vel, rhs_pcp, &rhs_vel);
    pp_vec3_add(&rhs_vel, &rhs->vel, &rhs_vel);

    pp_vec3_sub(&rhs_vel, &lhs_vel, out);
}

/* Soft constraint coefficients (Catto, "Solver2D" / Box2D v3). A contact is
 * treated as a stiff damped spring rather than a rigid stop: overlap is pushed
 * out at a controlled rate, and the push never overshoots. */
typedef struct _PPSoftness {
    float bias_rate;
    float mass_scale;
    float impulse_scale;
} PPSoftness;

static PPSoftness pp_make_soft(float hertz, float zeta, float h)
{
    PPSoftness s;
    float omega = 2.0f * 3.14159265f * hertz;
    float a1 = 2.0f * zeta + h * omega;
    float a2 = h * omega * a1;
    float a3 = 1.0f / (1.0f + a2);
    s.bias_rate = omega / a1;
    s.mass_scale = a2 * a3;
    s.impulse_scale = a3;
    return s;
}

/* World-space centre of mass. Contact lever arms and torques are measured from
 * here, not from the body origin. */
static inline void pp_body_world_com(const PPBody *body, PPVec3 *out)
{
    if (body->com_local.x == 0.0f && body->com_local.y == 0.0f && body->com_local.z == 0.0f) {
        *out = body->pos;
        return;
    }

    PPVec3 r;
    pp_quat_transform(&body->rot, &body->com_local, &r);
    pp_vec3_add(&body->pos, &r, out);
}

/* ---- Ball and hinge joints ------------------------------------------------
 *
 * Solved the same way as the contacts: once per substep, as soft constraints
 * with the accumulated impulse carried from one substep (and step) to the
 * next. body1 is the lhs of every impulse, body2 the rhs. */

static inline bool pp_constraint_is_joint(const PPConstraint *c)
{
    return c->type == PP_CONSTRAINT_TYPE_BALL || c->type == PP_CONSTRAINT_TYPE_HINGE;
}

/* trimesh_body stands in for the world. Its rotation is not a valid
 * quaternion, so it is treated as identity here. */
static void pp_joint_dir_to_world(const PPBody *b, const PPVec3 *local, PPVec3 *out)
{
    if (b == &trimesh_body) {
        *out = *local;
        return;
    }

    pp_quat_transform(&b->rot, local, out);
}

static void pp_joint_dir_to_local(const PPBody *b, const PPVec3 *world, PPVec3 *out)
{
    if (b == &trimesh_body) {
        *out = *world;
        return;
    }

    PPQuaternion inv;
    pp_quat_conjugate(&b->rot, &inv);
    pp_quat_transform(&inv, world, out);
}

static PPConstraint* pp_joint_create(PPConstraintType type, PPBody* body1, PPBody* body2, const PPVec3* world_anchor)
{
    if (!body1) body1 = &trimesh_body;
    if (!body2) body2 = &trimesh_body;
    assert(body1 != body2);

    PPConstraint* entry = pp_constraint_alloc();
    if (!entry) {
        return NULL;
    }

    entry->is_alive = true;
    entry->type = type;
    entry->body1 = body1;
    entry->body2 = body2;
    entry->inv_mass_sum = body1->inv_mass + body2->inv_mass;

    PPVec3 rel;
    pp_vec3_sub(world_anchor, &body1->pos, &rel);
    pp_joint_dir_to_local(body1, &rel, &entry->joint.local_anchor1);
    pp_vec3_sub(world_anchor, &body2->pos, &rel);
    pp_joint_dir_to_local(body2, &rel, &entry->joint.local_anchor2);

    pp_body_wake(body1);
    pp_body_wake(body2);
    return entry;
}

PPConstraint* pp_physics_create_ball_joint(PPBody* body1, PPBody* body2, const PPVec3* world_anchor)
{
    return pp_joint_create(PP_CONSTRAINT_TYPE_BALL, body1, body2, world_anchor);
}

PPConstraint* pp_physics_create_hinge_joint(PPBody* body1, PPBody* body2, const PPVec3* world_anchor, const PPVec3* world_axis)
{
    PPConstraint* entry = pp_joint_create(PP_CONSTRAINT_TYPE_HINGE, body1, body2, world_anchor);
    if (!entry) {
        return NULL;
    }

    PPJointConstraint *j = &entry->joint;

    PPVec3 axis = *world_axis;
    float len = pp_vec3_length(&axis);
    if (len < 1e-6f) {
        pp_vec3_set(&axis, 0.0f, 1.0f, 0.0f);
    } else {
        pp_vec3_scale(&axis, 1.0f / len, &axis);
    }

    // Any direction square to the axis will do as the zero-angle reference.
    PPVec3 ref, other;
    if (fabsf(axis.x) < 0.57f) pp_vec3_set(&other, 1.0f, 0.0f, 0.0f);
    else pp_vec3_set(&other, 0.0f, 1.0f, 0.0f);
    pp_vec3_cross(&axis, &other, &ref);
    pp_vec3_scale(&ref, 1.0f / pp_vec3_length(&ref), &ref);

    pp_joint_dir_to_local(entry->body1, &axis, &j->local_axis1);
    pp_joint_dir_to_local(entry->body2, &axis, &j->local_axis2);
    pp_joint_dir_to_local(entry->body1, &ref, &j->local_ref1);
    pp_joint_dir_to_local(entry->body2, &ref, &j->local_ref2);
    return entry;
}

void pp_physics_destroy_constraint(PPConstraint* c)
{
    if (!c || !c->is_alive) {
        return;
    }

    pp_body_wake(c->body1);
    pp_body_wake(c->body2);
    c->is_alive = false;
    ++dead_constraint_count;
}

void pp_constraint_set_collide_connected(PPConstraint* c, bool collide)
{
    c->joint.collide_connected = collide;
}

void pp_constraint_set_hinge_limits(PPConstraint* c, float lower, float upper)
{
    assert(c->type == PP_CONSTRAINT_TYPE_HINGE);
    c->joint.limit_enabled = true;
    c->joint.lower_angle = fminf(lower, upper);
    c->joint.upper_angle = fmaxf(lower, upper);
    c->joint.lower_impulse = 0.0f;
    c->joint.upper_impulse = 0.0f;
    pp_body_wake(c->body1);
    pp_body_wake(c->body2);
}

void pp_constraint_disable_hinge_limits(PPConstraint* c)
{
    c->joint.limit_enabled = false;
    c->joint.lower_impulse = 0.0f;
    c->joint.upper_impulse = 0.0f;
    pp_body_wake(c->body1);
    pp_body_wake(c->body2);
}

void pp_constraint_set_hinge_motor(PPConstraint* c, float speed, float max_torque)
{
    assert(c->type == PP_CONSTRAINT_TYPE_HINGE);
    c->joint.motor_enabled = max_torque > 0.0f;
    c->joint.motor_speed = speed;
    c->joint.max_motor_torque = max_torque;
    c->joint.motor_impulse = 0.0f;
    pp_body_wake(c->body1);
    pp_body_wake(c->body2);
}

static float pp_joint_hinge_angle(const PPConstraint* c, const PPVec3 *axis, const PPVec3 *ref1)
{
    PPVec3 ref2, s;
    pp_joint_dir_to_world(c->body2, &c->joint.local_ref2, &ref2);
    pp_vec3_cross(ref1, &ref2, &s);
    return pp_atan2f(pp_vec3_dot(&s, axis), pp_vec3_dot(ref1, &ref2));
}

float pp_constraint_get_hinge_angle(const PPConstraint* c)
{
    if (c->type != PP_CONSTRAINT_TYPE_HINGE) {
        return 0.0f;
    }

    PPVec3 axis, ref1;
    pp_joint_dir_to_world(c->body1, &c->joint.local_axis1, &axis);
    pp_joint_dir_to_world(c->body1, &c->joint.local_ref1, &ref1);
    return pp_joint_hinge_angle(c, &axis, &ref1);
}

/* True when a joint says these two should pass through each other. */
static bool pp_bodies_jointed(const PPBody *a, const PPBody *b)
{
    for (int i = 0; i < constraint_count; ++i) {
        const PPConstraint *c = &constraints[i];
        if (!c->is_alive || !pp_constraint_is_joint(c) || c->joint.collide_connected) continue;
        if ((c->body1 == a && c->body2 == b) || (c->body1 == b && c->body2 == a)) {
            return true;
        }
    }

    return false;
}

static bool pp_joint_active(const PPConstraint *c)
{
    if (!c->is_alive || !pp_constraint_is_joint(c)) return false;

    const PPBody *a = c->body1;
    const PPBody *b = c->body2;
    if (!a->is_alive || !b->is_alive || a->is_asleep || b->is_asleep) return false;
    return a->inv_mass + b->inv_mass > 0.0f;
}

/* 1 / (d . (I1^-1 + I2^-1) d), the angular effective mass about d. */
static float pp_joint_angular_mass(const PPBody *a, const PPBody *b, const PPVec3 *d)
{
    PPVec3 k1, k2;
    pp_mat3_mult(&a->inv_inertia_world, d, &k1);
    pp_mat3_mult(&b->inv_inertia_world, d, &k2);
    float k = pp_vec3_dot(&k1, d) + pp_vec3_dot(&k2, d);
    return (k > 1e-9f) ? pp_invf(k) : 0.0f;
}

/* Angular impulse `amount` about d: minus on body1, plus on body2. */
static void pp_joint_apply_angular(PPBody *a, PPBody *b, const PPVec3 *d, float amount)
{
    PPVec3 L, w;
    pp_vec3_scale(d, amount, &L);
    pp_mat3_mult(&a->inv_inertia_world, &L, &w);
    pp_vec3_sub(&a->a_vel, &w, &a->a_vel);
    pp_mat3_mult(&b->inv_inertia_world, &L, &w);
    pp_vec3_add(&b->a_vel, &w, &b->a_vel);
}

/* Start of each substep: lever arms and effective masses for the bodies as
 * they are now posed, then put back the impulses the joint was carrying. */
static void pp_joint_prepare(PPConstraint *c)
{
    PPJointConstraint *j = &c->joint;
    PPBody *a = c->body1;
    PPBody *b = c->body2;

    PPVec3 tmp, com1, com2;
    pp_vec3_sub(&j->local_anchor1, &a->com_local, &tmp);
    pp_joint_dir_to_world(a, &tmp, &j->r1);
    pp_vec3_sub(&j->local_anchor2, &b->com_local, &tmp);
    pp_joint_dir_to_world(b, &tmp, &j->r2);

    pp_body_world_com(a, &com1);
    pp_body_world_com(b, &com2);
    pp_vec3_add(&com2, &j->r2, &j->separation);
    pp_vec3_sub(&j->separation, &com1, &j->separation);
    pp_vec3_sub(&j->separation, &j->r1, &j->separation);

    // K = (m1 + m2) - [r1]x I1^-1 [r1]x - [r2]x I2^-1 [r2]x, a column at a
    // time: column e is the velocity the anchor gains from a unit impulse e.
    PPMat3 k;
    float inv_mass_sum = a->inv_mass + b->inv_mass;
    for (int col = 0; col < 3; ++col) {
        PPVec3 e = {.xyz = {0.0f, 0.0f, 0.0f}};
        PPVec3 rxe, w, v;
        e.xyz[col] = 1.0f;

        PPVec3 out;
        pp_vec3_scale(&e, inv_mass_sum, &out);

        pp_vec3_cross(&j->r1, &e, &rxe);
        pp_mat3_mult(&a->inv_inertia_world, &rxe, &w);
        pp_vec3_cross(&w, &j->r1, &v);
        pp_vec3_add(&out, &v, &out);

        pp_vec3_cross(&j->r2, &e, &rxe);
        pp_mat3_mult(&b->inv_inertia_world, &rxe, &w);
        pp_vec3_cross(&w, &j->r2, &v);
        pp_vec3_add(&out, &v, &out);

        k.m[col * 3 + 0] = out.x;
        k.m[col * 3 + 1] = out.y;
        k.m[col * 3 + 2] = out.z;
    }
    pp_mat3_inverse(&k, &j->inv_k);

    pp_apply_impulse(a, b, &j->r1, &j->r2, &j->impulse);

    if (c->type != PP_CONSTRAINT_TYPE_HINGE) {
        return;
    }

    // body2's axis has to stay square to two directions that are themselves
    // square to body1's axis. That is two rows; the axis itself stays free.
    PPVec3 axis2, ref1, ref1b;
    pp_joint_dir_to_world(a, &j->local_axis1, &j->axis);
    pp_joint_dir_to_world(b, &j->local_axis2, &axis2);
    pp_joint_dir_to_world(a, &j->local_ref1, &ref1);
    pp_vec3_cross(&j->axis, &ref1, &ref1b);

    pp_vec3_cross(&axis2, &ref1, &j->u[0]);
    pp_vec3_cross(&axis2, &ref1b, &j->u[1]);
    j->u_error[0] = pp_vec3_dot(&ref1, &axis2);
    j->u_error[1] = pp_vec3_dot(&ref1b, &axis2);
    j->u_mass[0] = pp_joint_angular_mass(a, b, &j->u[0]);
    j->u_mass[1] = pp_joint_angular_mass(a, b, &j->u[1]);
    j->axis_mass = pp_joint_angular_mass(a, b, &j->axis);

    if (j->limit_enabled) {
        j->angle = pp_joint_hinge_angle(c, &j->axis, &ref1);
    }

    pp_joint_apply_angular(a, b, &j->u[0], j->axial_impulse[0]);
    pp_joint_apply_angular(a, b, &j->u[1], j->axial_impulse[1]);
    pp_joint_apply_angular(a, b, &j->axis, j->motor_impulse + j->lower_impulse - j->upper_impulse);
}

static void pp_joint_solve(PPConstraint *c, float h, float inv_h, bool use_bias, const PPSoftness *soft)
{
    PPJointConstraint *j = &c->joint;
    PPBody *a = c->body1;
    PPBody *b = c->body2;

    float mass_scale = use_bias ? soft->mass_scale : 1.0f;
    float impulse_scale = use_bias ? soft->impulse_scale : 0.0f;
    float bias_rate = use_bias ? soft->bias_rate : 0.0f;

    if (c->type == PP_CONSTRAINT_TYPE_HINGE) {
        PPVec3 w;

        if (j->motor_enabled) {
            pp_vec3_sub(&b->a_vel, &a->a_vel, &w);
            float cdot = pp_vec3_dot(&w, &j->axis) - j->motor_speed;
            float limit = j->max_motor_torque * h;
            float old = j->motor_impulse;
            j->motor_impulse = fmaxf(-limit, fminf(limit, old - j->axis_mass * cdot));
            pp_joint_apply_angular(a, b, &j->axis, j->motor_impulse - old);
        }

        if (j->limit_enabled) {
            // Still short of the stop: let it close exactly that far this
            // substep and no further. Past it: push back softly.
            for (int side = 0; side < 2; ++side) {
                float sign = side == 0 ? 1.0f : -1.0f;
                float *accum = side == 0 ? &j->lower_impulse : &j->upper_impulse;
                float err = side == 0 ? j->angle - j->lower_angle : j->upper_angle - j->angle;

                float bias = 0.0f, ms = 1.0f, is = 0.0f;
                if (err > 0.0f) {
                    bias = err * inv_h;
                } else if (use_bias) {
                    bias = soft->bias_rate * err;
                    ms = soft->mass_scale;
                    is = soft->impulse_scale;
                }

                pp_vec3_sub(&b->a_vel, &a->a_vel, &w);
                float cdot = sign * pp_vec3_dot(&w, &j->axis);
                float old = *accum;
                *accum = fmaxf(0.0f, old - j->axis_mass * ms * (cdot + bias) - is * old);
                pp_joint_apply_angular(a, b, &j->axis, sign * (*accum - old));
            }
        }

        for (int i = 0; i < 2; ++i) {
            pp_vec3_sub(&b->a_vel, &a->a_vel, &w);
            float cdot = pp_vec3_dot(&w, &j->u[i]);
            float impulse = -j->u_mass[i] * mass_scale * (cdot + bias_rate * j->u_error[i]) -
                            impulse_scale * j->axial_impulse[i];
            j->axial_impulse[i] += impulse;
            pp_joint_apply_angular(a, b, &j->u[i], impulse);
        }
    }

    // The pin goes last so that it is the row left most nearly exact.
    PPVec3 cdot, bias, P, tmp;
    pp_contact_rel_vel(a, b, &j->r1, &j->r2, &cdot);
    pp_vec3_scale(&j->separation, bias_rate, &bias);
    pp_vec3_add(&cdot, &bias, &cdot);
    pp_mat3_mult(&j->inv_k, &cdot, &P);
    pp_vec3_scale(&P, -mass_scale, &P);
    pp_vec3_scale(&j->impulse, impulse_scale, &tmp);
    pp_vec3_sub(&P, &tmp, &P);
    pp_vec3_add(&j->impulse, &P, &j->impulse);
    pp_apply_impulse(a, b, &j->r1, &j->r2, &P);
}

/* Once per step, before any solving: fix a tangent basis, the lever arms and
 * effective masses, zero the accumulators, and capture the restitution target
 * from the approach velocity. */
static void pp_prepare_contact(PPCollision *m, float t)
{
    pp_contact_tangents(&m->n, &m->t1, &m->t2);
    m->jn = 0.0f;
    m->jt1 = 0.0f;
    m->jt2 = 0.0f;
    m->max_jn = 0.0f;
    m->impact = false;
    m->v_bias = 0.0f;
    m->mass_n = m->mass_t1 = m->mass_t2 = 0.0f;
    m->roll_limit = 0.0f;
    pp_vec3_init(&m->jroll);

    PPBody *lhs = m->obj1, *rhs = m->obj2;
    PPVec3 com1, com2;
    pp_body_world_com(lhs, &com1);
    pp_body_world_com(rhs, &com2);
    pp_vec3_sub(&m->p, &com1, &m->r1);
    pp_vec3_sub(&m->p, &com2, &m->r2);

    // Rolling resistance: a round shape resists turning with a torque of up to
    // (coefficient * radius) per unit of normal load. The lever arm stands in
    // for the radius, which is what it is for a lone sphere.
    if (m->type1 == PP_OBJECT_TYPE_SPHERE) {
        m->roll_limit = lhs->rolling_resistance * pp_vec3_length(&m->r1);
    }
    if (m->type2 == PP_OBJECT_TYPE_SPHERE) {
        m->roll_limit = fmaxf(m->roll_limit, rhs->rolling_resistance * pp_vec3_length(&m->r2));
    }
    if (m->type1 == PP_OBJECT_TYPE_CAPSULE) {
        m->roll_limit = fmaxf(m->roll_limit, lhs->rolling_resistance * m->roll_radius);
    }
    if (m->type2 == PP_OBJECT_TYPE_CAPSULE) {
        m->roll_limit = fmaxf(m->roll_limit, rhs->rolling_resistance * m->roll_radius);
    }

    if (!lhs->is_alive || !rhs->is_alive) return;

    float inv_mass_sum = lhs->inv_mass + rhs->inv_mass;
    if (inv_mass_sum < 1e-8f) return;

    float k;
    k = pp_contact_effective_mass(lhs, rhs, &m->r1, &m->r2, &m->n, inv_mass_sum);
    m->mass_n = (k > 1e-8f) ? pp_invf(k) : 0.0f;
    k = pp_contact_effective_mass(lhs, rhs, &m->r1, &m->r2, &m->t1, inv_mass_sum);
    m->mass_t1 = (k > 1e-8f) ? pp_invf(k) : 0.0f;
    k = pp_contact_effective_mass(lhs, rhs, &m->r1, &m->r2, &m->t2, inv_mass_sum);
    m->mass_t2 = (k > 1e-8f) ? pp_invf(k) : 0.0f;

    PPVec3 rel_vel;
    pp_contact_rel_vel(lhs, rhs, &m->r1, &m->r2, &rel_vel);
    float vn = pp_vec3_dot(&rel_vel, &m->n);

    // A resting body meets its contact with up to a couple of steps' worth of
    // gravity as approach velocity. The threshold must sit above that, or a
    // body at rest re-bounces off its own weight every step and never settles.
    const float bounce_threshold = fmaxf(0.1f, 3.0f * gravity_magnitude * t);
    if (vn > -bounce_threshold) {
        // Barely moving, or separating: no bounce.
        return;
    }

    m->impact = true;

    float r = fmaxf(m->obj1_bounce, m->obj2_bounce);
    m->v_bias = -r * vn;   /* vn < 0 when approaching, so this is positive (or 0) */
}

/* Seed a contact's accumulators from the matching contact of the previous
 * step. A match is the same body pair, a near-identical normal, and the
 * nearest contact point (relative to obj1) within a small radius. Each cached
 * contact seeds at most one new contact. */
static void pp_match_contact(PPCollision *m)
{
    if (m->mass_n == 0.0f) return;

    const float match_radius_sq = 0.05f * 0.05f;
    float best = match_radius_sq;
    PPContactCache *hit = NULL;

    for (int i = 0; i < contact_cache_count; ++i) {
        PPContactCache *c = &contact_cache[i];
        if (c->used || c->obj1 != m->obj1 || c->obj2 != m->obj2) continue;
        if (pp_vec3_dot(&c->n, &m->n) < 0.99f) continue;

        float d = pp_vec3_dist_sq(&c->r1, &m->r1);
        if (d < best) {
            best = d;
            hit = c;
        }
    }

    if (!hit) return;
    hit->used = true;

    m->jn = hit->jn;
    m->jt1 = hit->jt1;
    m->jt2 = hit->jt2;
}

/* Apply a contact's accumulated impulse. Done at the start of every substep:
 * the accumulators hold the impulse needed per substep, so the solver begins
 * each one already holding the load and only has to correct the difference. */
static void pp_warm_start_contact(PPCollision *m)
{
    if (m->mass_n == 0.0f) return;

    // Rolling resistance is limited by this substep's normal impulse, so it
    // is accumulated per substep rather than carried over.
    pp_vec3_init(&m->jroll);

    PPVec3 P, tmp;
    pp_vec3_scale(&m->n, m->jn, &P);
    pp_vec3_scale(&m->t1, m->jt1, &tmp);
    pp_vec3_add(&P, &tmp, &P);
    pp_vec3_scale(&m->t2, m->jt2, &tmp);
    pp_vec3_add(&P, &tmp, &P);

    pp_apply_impulse(m->obj1, m->obj2, &m->r1, &m->r2, &P);
}

/* Friction for one contact, limited by the normal impulse found so far. Every
 * contact's friction is solved before any contact's normal impulse (see
 * pp_physics_step): the normal pass over a face leaves transient spin between
 * one corner and the next, and friction solved in the middle of it would turn
 * that into real sideways momentum. It also means a contact that has not
 * pushed yet has no grip, so first touch cannot kick a body sideways. */
static void pp_solve_contact_friction(PPCollision *m)
{
    if (m->mass_n == 0.0f) return;

    PPBody *lhs = m->obj1;
    PPBody *rhs = m->obj2;
    PPVec3 rel_vel, P, tmp;

    float max_friction = m->obj1_friction * m->obj2_friction * m->jn;
    if (max_friction > 0.0f && m->mass_t1 > 0.0f && m->mass_t2 > 0.0f) {
        pp_contact_rel_vel(lhs, rhs, &m->r1, &m->r2, &rel_vel);

        float old1 = m->jt1, old2 = m->jt2;
        float new1 = old1 - pp_vec3_dot(&rel_vel, &m->t1) * m->mass_t1;
        float new2 = old2 - pp_vec3_dot(&rel_vel, &m->t2) * m->mass_t2;

        // Clamp to the friction cone as a 2D vector, so the limit is circular
        // rather than square.
        float len_sq = new1 * new1 + new2 * new2;
        if (len_sq > max_friction * max_friction) {
            float scale = max_friction / pp_sqrtf(len_sq);
            new1 *= scale;
            new2 *= scale;
        }

        m->jt1 = new1;
        m->jt2 = new2;

        pp_vec3_scale(&m->t1, new1 - old1, &P);
        pp_vec3_scale(&m->t2, new2 - old2, &tmp);
        pp_vec3_add(&P, &tmp, &P);
        pp_apply_impulse(lhs, rhs, &m->r1, &m->r2, &P);
    } else if (m->jt1 != 0.0f || m->jt2 != 0.0f) {
        // The normal impulse has gone; so must the friction it supported.
        pp_vec3_scale(&m->t1, -m->jt1, &P);
        pp_vec3_scale(&m->t2, -m->jt2, &tmp);
        pp_vec3_add(&P, &tmp, &P);
        pp_apply_impulse(lhs, rhs, &m->r1, &m->r2, &P);
        m->jt1 = m->jt2 = 0.0f;
    }

    if (m->roll_limit > 0.0f && m->jn > 0.0f) {
        // An angular impulse opposing the relative spin, no larger than the
        // load allows. +L turns obj2, -L turns obj1, like the linear impulses.
        PPVec3 w_rel, dir, k1, k2, L;
        pp_vec3_sub(&rhs->a_vel, &lhs->a_vel, &w_rel);
        float w = pp_vec3_length(&w_rel);
        if (w > 1e-6f) {
            pp_vec3_scale(&w_rel, pp_invf(w), &dir);
            pp_mat3_mult(&lhs->inv_inertia_world, &dir, &k1);
            pp_mat3_mult(&rhs->inv_inertia_world, &dir, &k2);
            float k = pp_vec3_dot(&dir, &k1) + pp_vec3_dot(&dir, &k2);
            if (k > 1e-8f) {
                PPVec3 old = m->jroll;
                pp_vec3_scale(&dir, -w / k, &L);
                pp_vec3_add(&m->jroll, &L, &m->jroll);

                float limit = m->roll_limit * m->jn;
                float len = pp_vec3_length(&m->jroll);
                if (len > limit) {
                    pp_vec3_scale(&m->jroll, limit / len, &m->jroll);
                }

                pp_vec3_sub(&m->jroll, &old, &L);
                pp_mat3_mult(&lhs->inv_inertia_world, &L, &k1);
                pp_mat3_mult(&rhs->inv_inertia_world, &L, &k2);
                pp_vec3_sub(&lhs->a_vel, &k1, &lhs->a_vel);
                pp_vec3_add(&rhs->a_vel, &k2, &rhs->a_vel);
            }
        }
    }
}

/* Normal impulse for one contact. With use_bias the contact also pushes out
 * overlap (softly); without it the pass only removes approach velocity, which
 * is how the "relax" pass takes back the velocity the push-out added. `inv_h`
 * is one over the substep length. */
static void pp_solve_contact_normal(PPCollision *m, float inv_h, bool use_bias,
                                    const PPSoftness *soft, const PPSoftness *static_soft)
{
    if (m->mass_n == 0.0f) return;

    PPBody *lhs = m->obj1;
    PPBody *rhs = m->obj2;
    const PPVec3 *n = &m->n;
    PPVec3 rel_vel, P;

    // Current separation: the value measured by collision detection, plus
    // whatever relative movement the substeps so far have produced at the
    // contact point (small-angle: a turn of dtheta moves the point by
    // dtheta x r).
    PPVec3 d, c1, c2;
    pp_vec3_cross(&lhs->solve_dtheta, &m->r1, &c1);
    pp_vec3_cross(&rhs->solve_dtheta, &m->r2, &c2);
    pp_vec3_sub(&rhs->solve_dpos, &lhs->solve_dpos, &d);
    pp_vec3_add(&d, &c2, &d);
    pp_vec3_sub(&d, &c1, &d);
    float s = m->separation + pp_vec3_dot(&d, n);

    float bias = 0.0f, mass_scale = 1.0f, impulse_scale = 0.0f;
    if (s > 0.0f) {
        // Speculative: not touching yet. Allow exactly the approach speed
        // that closes the gap this substep, so a fast mover lands on the
        // surface rather than stopping short of it or passing through.
        bias = s * inv_h;
    } else if (use_bias) {
        const float slop = 0.005f;
        const float max_push_speed = 3.0f;
        const PPSoftness *k = (lhs->inv_mass == 0.0f || rhs->inv_mass == 0.0f) ? static_soft : soft;

        bias = fmaxf(k->bias_rate * fminf(s + slop, 0.0f), -max_push_speed);
        mass_scale = k->mass_scale;
        impulse_scale = k->impulse_scale;
    }

    pp_contact_rel_vel(lhs, rhs, &m->r1, &m->r2, &rel_vel);
    float vn = pp_vec3_dot(&rel_vel, n);

    float dJ = -m->mass_n * mass_scale * (vn + bias) - impulse_scale * m->jn;

    float old_jn = m->jn;
    m->jn = fmaxf(old_jn + dJ, 0.0f);
    if (m->jn > m->max_jn) m->max_jn = m->jn;

    pp_vec3_scale(n, m->jn - old_jn, &P);
    pp_apply_impulse(lhs, rhs, &m->r1, &m->r2, &P);
}

/* After the substeps: give bouncing contacts their rebound. Only contacts that
 * actually pushed during the step qualify, which is what stops a speculative
 * contact that was never reached from launching anything. */
static void pp_apply_restitution(PPCollision *m)
{
    if (m->mass_n == 0.0f || m->v_bias == 0.0f || m->max_jn == 0.0f) return;

    PPVec3 rel_vel, P;
    pp_contact_rel_vel(m->obj1, m->obj2, &m->r1, &m->r2, &rel_vel);
    float vn = pp_vec3_dot(&rel_vel, &m->n);

    float old_jn = m->jn;
    m->jn = fmaxf(old_jn + m->mass_n * (m->v_bias - vn), 0.0f);

    pp_vec3_scale(&m->n, m->jn - old_jn, &P);
    pp_apply_impulse(m->obj1, m->obj2, &m->r1, &m->r2, &P);
}

static inline bool flt_close(const float a, const float b)
{
    return (a + FLT_EPSILON > b) && (a - FLT_EPSILON) < b;
}

bool pp_sphere_box_intersect(
    const PPVec3 *sphere_pos, float sphere_radius,
    const PPVec3 *box_pos, const PPQuaternion *box_rot, const PPVec3 *box_half_extents, float box_bounding_radius,
    PPVec3 *contact_point, PPVec3 *n, float *intersection)
{
    // Early-out: bounding sphere check (cheap squared distance)
    PPVec3 diff;
    pp_vec3_sub(sphere_pos, box_pos, &diff);
    float diff_dist_sq = pp_vec3_dot(&diff, &diff);
    float sum_radius = sphere_radius + box_bounding_radius;
    if (diff_dist_sq > sum_radius * sum_radius) {
        return false;
    }

    // Transform sphere center to box local space
    PPVec3 sphere_center_local;
    PPVec3 tmp;
    pp_vec3_sub(sphere_pos, box_pos, &tmp);

    PPQuaternion box_rot_inv;
    pp_quat_conjugate(box_rot, &box_rot_inv);
    pp_quat_transform(&box_rot_inv, &tmp, &sphere_center_local);

    // Clamp to box half_extents (local space)
    float hx = box_half_extents->x, hy = box_half_extents->y, hz = box_half_extents->z;
    float cx = sphere_center_local.x;
    float cy = sphere_center_local.y;
    float cz = sphere_center_local.z;
    if (cx < -hx) cx = -hx; else if (cx > hx) cx = hx;
    if (cy < -hy) cy = -hy; else if (cy > hy) cy = hy;
    if (cz < -hz) cz = -hz; else if (cz > hz) cz = hz;

    // Delta from closest point to sphere center (local space)
    float dx = sphere_center_local.x - cx;
    float dy = sphere_center_local.y - cy;
    float dz = sphere_center_local.z - cz;
    float dist_sq = dx*dx + dy*dy + dz*dz;

    if (dist_sq > sphere_radius * sphere_radius) {
        return false;
    }

    // Transform contact point back to world space
    PPVec3 contact_world;
    pp_vec3_set(&sphere_center_local, cx, cy, cz);
    pp_quat_transform(box_rot, &sphere_center_local, &contact_world);
    pp_vec3_add(&contact_world, box_pos, contact_point);

    // Penetration depth
    float dist = pp_sqrtf(dist_sq);
    if (intersection) {
        *intersection = sphere_radius - dist;
    }

    if (n) {
        PPVec3 normal_local;
        if (dist > 1e-6f) {
            float inv_dist = -pp_invf(dist);
            pp_vec3_set(&normal_local, dx * inv_dist, dy * inv_dist, dz * inv_dist);
        } else {
            // Sphere center is inside box
            pp_vec3_set(&normal_local, -1.0f, 0.0f, 0.0f);
        }
        pp_quat_transform(box_rot, &normal_local, n);
    }

    return true;
}

static void pp_integrate_velocities(float t)
{
    // Move all bodies by their velocity
    for (int i = 0; i < object_count; ++i) {
        PPBody *body = &objects[i];
        if (!body->is_alive || body->is_asleep) {
            continue;
        }

        // A body with no mass is moved only if it has been given a velocity:
        // that is a kinematic body (a lift, a door, a moving platform). It
        // goes exactly where its velocity takes it and nothing pushes back.
        if (body->inv_mass == 0.0f && pp_vec3_length_sq(&body->vel) == 0.0f &&
            pp_vec3_length_sq(&body->a_vel) == 0.0f) {
            continue;
        }

        // vel is the velocity of the centre of mass. When that is not the body
        // origin, the origin swings around it as the body turns.
        bool offset_com = body->com_local.x != 0.0f || body->com_local.y != 0.0f ||
                          body->com_local.z != 0.0f;
        PPVec3 com = body->pos;
        if (offset_com) {
            pp_body_world_com(body, &com);
        }

        PPVec3 scaled_vel;
        pp_vec3_scale(&body->vel, t, &scaled_vel);
        pp_vec3_add(&body->pos, &scaled_vel, &body->pos);
        pp_vec3_add(&body->solve_dpos, &scaled_vel, &body->solve_dpos);
        PPVec3 scaled_lin = scaled_vel;

        pp_vec3_scale(&body->a_vel, t, &scaled_vel);
        pp_vec3_add(&body->solve_dtheta, &scaled_vel, &body->solve_dtheta);

        // q += dt/2 * w * q, then renormalise, rather than building the exact
        // rotation from sin and cos of the half angle. Over a substep the
        // angle is tiny, so the two agree to a few parts per million -- and
        // tiny angles are exactly where a fast sin/cos falls down: the SH4's
        // FSCA works in 1/65536ths of a turn, which at a quarter of a 60 Hz
        // step silently drops several percent of every body's spin.
        PPQuaternion q_rot;
        float half_t = 0.5f * t;
        pp_quat_set(&q_rot, body->a_vel.x * half_t, body->a_vel.y * half_t, body->a_vel.z * half_t, 1.0f);
        pp_quat_multiply(&q_rot, &body->rot, &body->rot); // a_vel is world-space: pre-multiply
        pp_quat_normalize(&body->rot);

        if (offset_com) {
            PPVec3 r;
            pp_vec3_add(&com, &scaled_lin, &com);
            pp_quat_transform(&body->rot, &body->com_local, &r);
            pp_vec3_sub(&com, &r, &body->pos);
        }
    }
}

static inline void pp_shape_world_pos(const PPBody *body, const PPShape *shape, PPVec3 *out)
{
    PPVec3 rotated_offset;
    pp_quat_transform(&body->rot, &shape->offset, &rotated_offset);
    pp_vec3_add(&body->pos, &rotated_offset, out);
}

#define PP_MAX_BOX_CONTACTS 8

typedef struct _PPBoxContact {
    PPVec3 normal;                   /* unit, points from box A toward box B */
    int count;
    PPVec3 points[PP_MAX_BOX_CONTACTS];
    float depths[PP_MAX_BOX_CONTACTS];
} PPBoxContact;

static inline void pp_box_world_axes(const PPQuaternion *rot, PPVec3 axes[3])
{
    PPVec3 x = {.xyz = {1.0f, 0.0f, 0.0f}};
    PPVec3 y = {.xyz = {0.0f, 1.0f, 0.0f}};
    PPVec3 z = {.xyz = {0.0f, 0.0f, 1.0f}};
    pp_quat_transform(rot, &x, &axes[0]);
    pp_quat_transform(rot, &y, &axes[1]);
    pp_quat_transform(rot, &z, &axes[2]);
}

/* Project a box's half-extents onto a (unit) axis: the box's radius along L. */
static inline float pp_box_project_radius(const PPVec3 *he, const PPVec3 axes[3], const PPVec3 *L)
{
    return he->x * fabsf(pp_vec3_dot(&axes[0], L)) +
           he->y * fabsf(pp_vec3_dot(&axes[1], L)) +
           he->z * fabsf(pp_vec3_dot(&axes[2], L));
}

/* dot(P - c, axis) */
static inline float pp_dot_rel(const PPVec3 *P, const PPVec3 *c, const PPVec3 *axis)
{
    return (P->x - c->x) * axis->x + (P->y - c->y) * axis->y + (P->z - c->z) * axis->z;
}

/* Clip polygon `in` (n verts) to the half-space { P : dot(P - c, axis) <= limit }
 * (Sutherland-Hodgman), writing survivors to `out`. Returns the new vertex count. */
static int pp_clip_halfspace(const PPVec3 *in, int n, PPVec3 *out,
                             const PPVec3 *c, const PPVec3 *axis, float limit)
{
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const PPVec3 *A = &in[i];
        const PPVec3 *B = &in[(i + 1) % n];
        float da = pp_dot_rel(A, c, axis) - limit;
        float db = pp_dot_rel(B, c, axis) - limit;
        bool a_in = da <= 0.0f;
        bool b_in = db <= 0.0f;

        if (a_in && m < PP_MAX_BOX_CONTACTS * 2) {
            out[m++] = *A;
        }
        if (a_in != b_in && m < PP_MAX_BOX_CONTACTS * 2) {
            float denom = da - db;
            float u = (denom != 0.0f) ? da / denom : 0.0f;
            PPVec3 P = {.xyz = {A->x + u * (B->x - A->x),
                                A->y + u * (B->y - A->y),
                                A->z + u * (B->z - A->z)}};
            out[m++] = P;
        }
    }
    return m;
}

/* Closest points between segments (p1,q1) and (p2,q2); midpoint written to out. */
static void pp_closest_segment_segment(const PPVec3 *p1, const PPVec3 *q1,
                                       const PPVec3 *p2, const PPVec3 *q2, PPVec3 *out)
{
    PPVec3 d1, d2, r;
    pp_vec3_sub(q1, p1, &d1);
    pp_vec3_sub(q2, p2, &d2);
    pp_vec3_sub(p1, p2, &r);

    float a = pp_vec3_dot(&d1, &d1);
    float e = pp_vec3_dot(&d2, &d2);
    float f = pp_vec3_dot(&d2, &r);

    float s, tt;
    const float EPS = 1e-8f;
    if (a <= EPS && e <= EPS) {
        s = tt = 0.0f;
    } else if (a <= EPS) {
        s = 0.0f;
        tt = f / e;
    } else {
        float c = pp_vec3_dot(&d1, &r);
        if (e <= EPS) {
            tt = 0.0f;
            s = -c / a;
        } else {
            float b = pp_vec3_dot(&d1, &d2);
            float denom = a * e - b * b;
            s = (denom != 0.0f) ? (b * f - c * e) / denom : 0.0f;
            tt = (b * s + f) / e;
        }
    }
    s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
    tt = tt < 0.0f ? 0.0f : (tt > 1.0f ? 1.0f : tt);

    PPVec3 c1 = {.xyz = {p1->x + d1.x * s, p1->y + d1.y * s, p1->z + d1.z * s}};
    PPVec3 c2 = {.xyz = {p2->x + d2.x * tt, p2->y + d2.y * tt, p2->z + d2.z * tt}};
    out->x = 0.5f * (c1.x + c2.x);
    out->y = 0.5f * (c1.y + c2.y);
    out->z = 0.5f * (c1.z + c2.z);
}

static bool pp_sat_box_box(const PPVec3 *posA, const PPQuaternion *rotA, const PPVec3 *heA, float brA,
                           const PPVec3 *posB, const PPQuaternion *rotB, const PPVec3 *heB, float brB,
                           float margin, PPBoxContact *out)
{
    /* `margin` is how far apart the boxes may be and still produce contacts.
     * Those contacts carry a negative depth (a gap) and the solver treats them
     * speculatively: it only removes approach velocity that would close more
     * than the gap this step. Without it two boxes are invisible to each other
     * until they already overlap, so a box landing on another sinks in first
     * and gets shoved back out afterwards. */

    /* Cheap bounding-sphere reject. */
    PPVec3 dc;
    pp_vec3_sub(posB, posA, &dc);
    float sum_br = brA + brB + margin;
    if (pp_vec3_dot(&dc, &dc) > sum_br * sum_br) {
        return false;
    }

    PPVec3 ua[3], ub[3];
    pp_box_world_axes(rotA, ua);
    pp_box_world_axes(rotB, ub);

    float min_overlap = FLT_MAX;
    PPVec3 best_axis = {.xyz = {0.0f, 0.0f, 0.0f}};
    int best_type = -1;          /* 0: face of A, 1: face of B, 2: edge-edge */
    int best_index = -1;         /* face index for type 0/1 */
    int best_ea = -1, best_eb = -1;

    /* Face axes of A. */
    for (int i = 0; i < 3; ++i) {
        PPVec3 L = ua[i];
        float rA = heA->xyz[i];
        float rB = pp_box_project_radius(heB, ub, &L);
        float overlap = rA + rB - fabsf(pp_vec3_dot(&dc, &L));

        if (overlap <= -margin) {
            return false;
        }

        if (overlap < min_overlap) {
            min_overlap = overlap;
            best_axis = L;
            best_type = 0;
            best_index = i;
        }
    }
    /* Face axes of B. */
    for (int j = 0; j < 3; ++j) {
        PPVec3 L = ub[j];
        float rA = pp_box_project_radius(heA, ua, &L);
        float rB = heB->xyz[j];
        float overlap = rA + rB - fabsf(pp_vec3_dot(&dc, &L));

        if (overlap <= -margin) {
            return false;
        }

        // Prefer A's faces unless B's is clearly better, so two aligned boxes
        // don't swap reference face (and shift every contact point) from one
        // step to the next on float noise.
        if (overlap < min_overlap - 0.02f * fabsf(min_overlap)) {
            min_overlap = overlap;
            best_axis = L;
            best_type = 1;
            best_index = j;
        }
    }
    /* Edge-edge axes. A face axis is kept unless an edge axis is clearly
     * smaller: an edge contact is a single point, so letting one narrowly win
     * on near-parallel faces collapses a stable 4-point face manifold for a
     * step and the box above starts to rock. */
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            PPVec3 L;
            pp_vec3_cross(&ua[i], &ub[j], &L);
            float len = pp_vec3_length(&L);
            if (len < 0.01f) {
                // Near-parallel edges: the axis is numerically noisy, and the
                // face axes already cover this direction.
                continue;
            }

            pp_vec3_scale(&L, pp_invf(len), &L);
            float rA = pp_box_project_radius(heA, ua, &L);
            float rB = pp_box_project_radius(heB, ub, &L);
            float overlap = rA + rB - fabsf(pp_vec3_dot(&dc, &L));
            if (overlap <= -margin) {
                return false;
            }

            if (overlap < min_overlap - 0.05f * fabsf(min_overlap) - 0.002f) {
                min_overlap = overlap;
                best_axis = L;
                best_type = 2;
                best_index = -1;
                best_ea = i;
                best_eb = j;
            }
        }
    }

    /* Orient the normal from A toward B. */
    if (pp_vec3_dot(&dc, &best_axis) < 0.0f) {
        pp_vec3_neg(&best_axis, &best_axis);
    }
    out->normal = best_axis;
    out->count = 0;

    if (best_type == 2) {
        /* Edge-edge: one contact at the closest points of the supporting edges. */
        PPVec3 ptA = *posA;
        for (int k = 0; k < 3; ++k) {
            if (k == best_ea) {
                continue;
            }

            float sgn = (pp_vec3_dot(&ua[k], &best_axis) >= 0.0f) ? 1.0f : -1.0f;
            PPVec3 add;
            pp_vec3_scale(&ua[k], sgn * heA->xyz[k], &add);
            pp_vec3_add(&ptA, &add, &ptA);
        }

        PPVec3 ea;
        pp_vec3_scale(&ua[best_ea], heA->xyz[best_ea], &ea);

        PPVec3 a1, a2;
        pp_vec3_sub(&ptA, &ea, &a1);
        pp_vec3_add(&ptA, &ea, &a2);

        PPVec3 ptB = *posB;
        for (int k = 0; k < 3; ++k) {
            if (k == best_eb) {
                continue;
            }

            float sgn = (pp_vec3_dot(&ub[k], &best_axis) >= 0.0f) ? -1.0f : 1.0f;

            PPVec3 add;
            pp_vec3_scale(&ub[k], sgn * heB->xyz[k], &add);
            pp_vec3_add(&ptB, &add, &ptB);
        }

        PPVec3 eb;
        pp_vec3_scale(&ub[best_eb], heB->xyz[best_eb], &eb);

        PPVec3 b1, b2;
        pp_vec3_sub(&ptB, &eb, &b1);
        pp_vec3_add(&ptB, &eb, &b2);

        pp_closest_segment_segment(&a1, &a2, &b1, &b2, &out->points[0]);
        out->depths[0] = min_overlap;
        out->count = 1;
        return true;
    }

    /* Face case: clip the incident face against the reference face. */
    const PPVec3 *refPos, *refHe, *incPos, *incHe;
    const PPVec3 *refAxes, *incAxes;
    int ref_idx = best_index;
    PPVec3 refN; /* outward normal of the reference face */

    if (best_type == 0) {
        refPos = posA; refHe = heA; refAxes = ua;
        incPos = posB; incHe = heB; incAxes = ub;
        refN = best_axis;                 /* A's contacting face faces B (+normal) */
    } else {
        refPos = posB; refHe = heB; refAxes = ub;
        incPos = posA; incHe = heA; incAxes = ua;
        pp_vec3_neg(&best_axis, &refN);   /* B's contacting face faces A (-normal) */
    }

    /* Reference face centre and its two tangent axes. */
    PPVec3 faceCenter;
    {
        PPVec3 off;
        pp_vec3_scale(&refN, refHe->xyz[ref_idx], &off);
        pp_vec3_add(refPos, &off, &faceCenter);
    }
    int ta_i = (ref_idx + 1) % 3, tb_i = (ref_idx + 2) % 3;
    PPVec3 ta = refAxes[ta_i], tb = refAxes[tb_i];
    float hta = refHe->xyz[ta_i], htb = refHe->xyz[tb_i];

    /* Incident face: the face of the other box most anti-parallel to refN. */
    int inc_idx = 0;
    float min_dot = 0.0f;
    for (int k = 0; k < 3; ++k) {
        float d = pp_vec3_dot(&incAxes[k], &refN);
        if (k == 0 || fabsf(d) > fabsf(min_dot)) {
            min_dot = d;
            inc_idx = k;
        }
    }
    PPVec3 incN = incAxes[inc_idx];
    if (pp_vec3_dot(&incN, &refN) > 0.0f) {
        pp_vec3_neg(&incN, &incN); /* outward normal pointing back toward the ref box */
    }
    PPVec3 incCenter;
    {
        PPVec3 off;
        pp_vec3_scale(&incN, incHe->xyz[inc_idx], &off);
        pp_vec3_add(incPos, &off, &incCenter);
    }

    int ia_i = (inc_idx + 1) % 3;
    int ib_i = (inc_idx + 2) % 3;
    float hia = incHe->xyz[ia_i];
    float hib = incHe->xyz[ib_i];

    /* Four incident-face vertices, in order around the quad. */
    PPVec3 poly[PP_MAX_BOX_CONTACTS * 2];
    const float sa[4] = { 1.0f, 1.0f, -1.0f, -1.0f };
    const float sb[4] = { 1.0f, -1.0f, -1.0f, 1.0f };
    for (int v = 0; v < 4; ++v) {
        PPVec3 p = incCenter, add;
        pp_vec3_scale(&incAxes[ia_i], sa[v] * hia, &add);
        pp_vec3_add(&p, &add, &p);

        pp_vec3_scale(&incAxes[ib_i], sb[v] * hib, &add);
        pp_vec3_add(&p, &add, &p);
        poly[v] = p;
    }

    /* Clip against the reference face's four side planes. */
    PPVec3 buf[PP_MAX_BOX_CONTACTS * 2];
    PPVec3 ta_neg, tb_neg;
    pp_vec3_neg(&ta, &ta_neg);
    pp_vec3_neg(&tb, &tb_neg);
    int n = 4;
    n = pp_clip_halfspace(poly, n, buf, &faceCenter, &ta,     hta);
    n = pp_clip_halfspace(buf,  n, poly, &faceCenter, &ta_neg, hta);
    n = pp_clip_halfspace(poly, n, buf, &faceCenter, &tb,     htb);
    n = pp_clip_halfspace(buf,  n, poly, &faceCenter, &tb_neg, htb);

    /* Keep clipped points behind the reference face, or within the contact
     * margin in front of it. The latter carry a negative depth, which the
     * solver treats as a speculative contact; they keep all corners of a
     * resting face in the manifold from step to step. */
    /* Two near-aligned faces with a little relative yaw clip to an octagon
     * whose vertices come in tight pairs, one pair per corner. The pairs merge
     * and split from step to step on float noise; every such change orphans a
     * warm-start impulse and jolts the solve. Merge points closer than a small
     * fraction of the face so the manifold stays the same four corners. */
    float merge = 0.1f * fminf(hta, htb);
    float merge_sq = merge * merge;

    for (int i = 0; i < n && out->count < PP_MAX_BOX_CONTACTS; ++i) {
        float pen = -pp_dot_rel(&poly[i], &faceCenter, &refN);
        if (pen < -margin) {
            continue;
        }

        bool merged = false;
        for (int k = 0; k < out->count; ++k) {
            if (pp_vec3_dist_sq(&out->points[k], &poly[i]) < merge_sq) {
                if (pen > out->depths[k]) out->depths[k] = pen;
                merged = true;
                break;
            }
        }
        if (merged) {
            continue;
        }

        out->points[out->count] = poly[i];
        out->depths[out->count] = pen;
        out->count++;
    }

    /* Degenerate fallback: if clipping produced nothing, use the deepest point. */
    if (out->count == 0) {
        out->points[0] = faceCenter;
        out->depths[0] = min_overlap;
        out->count = 1;
    }
    return true;
}

/* ---- capsules ------------------------------------------------------------
 *
 * A capsule is a sphere swept along a segment, so every capsule test comes
 * down to choosing points on that segment and treating each as a sphere. Up to
 * two are kept per pair, one towards each end of the overlap, which is what
 * lets a capsule lie flat on something without see-sawing on a single point. */

#define PP_MAX_CAPSULE_CONTACTS 2

typedef struct _PPCapsuleContact {
    int count;
    PPVec3 points[PP_MAX_CAPSULE_CONTACTS];      /* on the other shape's surface */
    PPVec3 normals[PP_MAX_CAPSULE_CONTACTS];     /* unit, capsule -> other shape */
    float separations[PP_MAX_CAPSULE_CONTACTS];  /* negative when overlapping */
} PPCapsuleContact;

/* World-space end points of a capsule shape's inner segment. */
static inline void pp_capsule_segment(const PPBody *body, const PPShape *shape, const PPVec3 *shape_pos,
                                      PPVec3 *a, PPVec3 *b)
{
    PPVec3 up = {.xyz = {0.0f, shape->capsule.half_height, 0.0f}}, axis;
    pp_quat_transform(&body->rot, &up, &axis);
    pp_vec3_sub(shape_pos, &axis, a);
    pp_vec3_add(shape_pos, &axis, b);
}

static inline void pp_segment_point(const PPVec3 *a, const PPVec3 *b, float t, PPVec3 *out)
{
    out->x = a->x + (b->x - a->x) * t;
    out->y = a->y + (b->y - a->y) * t;
    out->z = a->z + (b->z - a->z) * t;
}

/* Parameter of the point on segment ab nearest to p, clamped to [0, 1]. */
static inline float pp_segment_closest_t(const PPVec3 *a, const PPVec3 *b, const PPVec3 *p)
{
    PPVec3 ab, ap;
    pp_vec3_sub(b, a, &ab);
    pp_vec3_sub(p, a, &ap);
    float len_sq = pp_vec3_dot(&ab, &ab);
    if (len_sq < 1e-12f) return 0.0f;
    float t = pp_vec3_dot(&ap, &ab) / len_sq;
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

/* Keep a candidate contact: at most two survive, the deepest and whichever
 * other is farthest from it. Candidates on top of one already kept are
 * dropped. */
static void pp_capsule_contact_add(PPCapsuleContact *out, const PPVec3 *point, const PPVec3 *normal,
                                   float separation, float merge_dist)
{
    for (int i = 0; i < out->count; ++i) {
        PPVec3 d;
        pp_vec3_sub(point, &out->points[i], &d);
        if (pp_vec3_length_sq(&d) < merge_dist * merge_dist) {
            if (separation < out->separations[i]) {
                out->points[i] = *point;
                out->normals[i] = *normal;
                out->separations[i] = separation;
            }
            return;
        }
    }

    if (out->count < PP_MAX_CAPSULE_CONTACTS) {
        int k = out->count++;
        out->points[k] = *point;
        out->normals[k] = *normal;
        out->separations[k] = separation;
        return;
    }

    // Full: slot 0 is kept as the deepest, slot 1 as the farthest from it.
    if (separation < out->separations[0]) {
        PPVec3 tp = out->points[0], tn = out->normals[0];
        float ts = out->separations[0];
        out->points[0] = *point; out->normals[0] = *normal; out->separations[0] = separation;
        point = &tp; normal = &tn; separation = ts;

        PPVec3 d_new, d_old;
        pp_vec3_sub(point, &out->points[0], &d_new);
        pp_vec3_sub(&out->points[1], &out->points[0], &d_old);
        if (pp_vec3_length_sq(&d_new) > pp_vec3_length_sq(&d_old)) {
            out->points[1] = *point; out->normals[1] = *normal; out->separations[1] = separation;
        }
        return;
    }

    PPVec3 d_new, d_old;
    pp_vec3_sub(point, &out->points[0], &d_new);
    pp_vec3_sub(&out->points[1], &out->points[0], &d_old);
    if (pp_vec3_length_sq(&d_new) > pp_vec3_length_sq(&d_old)) {
        out->points[1] = *point; out->normals[1] = *normal; out->separations[1] = separation;
    }
}

/* A sphere of radius ra at ca against a sphere of radius rb at cb (rb may be
 * zero: a point on some surface). */
static void pp_capsule_round_candidate(PPCapsuleContact *out, const PPVec3 *ca, float ra,
                                       const PPVec3 *cb, float rb, float reach, float merge_dist)
{
    PPVec3 n;
    pp_vec3_sub(cb, ca, &n);
    float dist = pp_vec3_length(&n);
    float separation = dist - ra - rb;
    if (separation > reach) return;

    if (dist > 1e-6f) {
        pp_vec3_scale(&n, pp_invf(dist), &n);
    } else {
        pp_vec3_set(&n, 0.0f, 1.0f, 0.0f);
    }

    // On the other shape's surface.
    PPVec3 p = {.xyz = {cb->x - n.x * rb, cb->y - n.y * rb, cb->z - n.z * rb}};
    pp_capsule_contact_add(out, &p, &n, separation, merge_dist);
}

/* Capsule (a, b, radius) against a sphere. */
static void pp_capsule_sphere(const PPVec3 *a, const PPVec3 *b, float radius,
                              const PPVec3 *centre, float sphere_radius, float reach,
                              PPCapsuleContact *out)
{
    out->count = 0;
    PPVec3 c;
    pp_segment_point(a, b, pp_segment_closest_t(a, b, centre), &c);
    pp_capsule_round_candidate(out, &c, radius, centre, sphere_radius, reach, 0.1f * radius);
}

/* Capsule against capsule. The closest pair of points covers capsules that
 * cross; projecting each end onto the other segment covers capsules lying
 * alongside each other, which need a contact towards each end. */
static void pp_capsule_capsule(const PPVec3 *a1, const PPVec3 *b1, float r1,
                               const PPVec3 *a2, const PPVec3 *b2, float r2, float reach,
                               PPCapsuleContact *out)
{
    out->count = 0;
    float merge = 0.1f * fminf(r1, r2);
    PPVec3 c1, c2;

    // Closest pair, by alternating projection from the midpoint.
    float t1 = 0.5f, t2;
    for (int i = 0; i < 4; ++i) {
        pp_segment_point(a1, b1, t1, &c1);
        t2 = pp_segment_closest_t(a2, b2, &c1);
        pp_segment_point(a2, b2, t2, &c2);
        t1 = pp_segment_closest_t(a1, b1, &c2);
    }
    pp_segment_point(a1, b1, t1, &c1);
    pp_capsule_round_candidate(out, &c1, r1, &c2, r2, reach, merge);

    const PPVec3 *ends1[2] = {a1, b1};
    const PPVec3 *ends2[2] = {a2, b2};
    for (int i = 0; i < 2; ++i) {
        pp_segment_point(a2, b2, pp_segment_closest_t(a2, b2, ends1[i]), &c2);
        // Only a true alongside contact: the end must be the nearest point of
        // its own segment to what it found, or it is just a worse duplicate.
        if (fabsf(pp_segment_closest_t(a1, b1, &c2) - (float)i) < 1e-3f) {
            pp_capsule_round_candidate(out, ends1[i], r1, &c2, r2, reach, merge);
        }

        pp_segment_point(a1, b1, pp_segment_closest_t(a1, b1, ends2[i]), &c1);
        if (fabsf(pp_segment_closest_t(a2, b2, &c1) - (float)i) < 1e-3f) {
            pp_capsule_round_candidate(out, &c1, r1, ends2[i], r2, reach, merge);
        }
    }
}

/* A sphere at local-space centre c against a box of half extents he centred on
 * the origin. Gives the closest point on the box, the normal (sphere -> box)
 * and the separation. */
static void pp_sphere_box_local(const PPVec3 *c, float radius, const PPVec3 *he,
                                PPVec3 *point, PPVec3 *normal, float *separation)
{
    PPVec3 q;
    bool inside = true;
    for (int k = 0; k < 3; ++k) {
        float v = c->xyz[k];
        if (v < -he->xyz[k]) { v = -he->xyz[k]; inside = false; }
        else if (v > he->xyz[k]) { v = he->xyz[k]; inside = false; }
        q.xyz[k] = v;
    }

    if (!inside) {
        PPVec3 d;
        pp_vec3_sub(&q, c, &d);
        float dist = pp_vec3_length(&d);
        pp_vec3_scale(&d, pp_invf(dist), normal);
        *point = q;
        *separation = dist - radius;
        return;
    }

    // Centre inside the box: leave by the nearest face.
    int axis = 0;
    float best = FLT_MAX;
    for (int k = 0; k < 3; ++k) {
        float depth = he->xyz[k] - fabsf(c->xyz[k]);
        if (depth < best) { best = depth; axis = k; }
    }

    float sign = (c->xyz[axis] >= 0.0f) ? 1.0f : -1.0f;
    pp_vec3_set(normal, 0.0f, 0.0f, 0.0f);
    normal->xyz[axis] = -sign;
    *point = *c;
    point->xyz[axis] = sign * he->xyz[axis];
    *separation = -(best + radius);
}

/* Capsule against an oriented box. */
static void pp_capsule_box(const PPVec3 *a, const PPVec3 *b, float radius,
                           const PPVec3 *box_pos, const PPQuaternion *box_rot, const PPVec3 *he,
                           float reach, PPCapsuleContact *out)
{
    out->count = 0;

    // Work in the box's frame.
    PPQuaternion inv;
    pp_quat_conjugate(box_rot, &inv);
    PPVec3 la, lb, tmp;
    pp_vec3_sub(a, box_pos, &tmp); pp_quat_transform(&inv, &tmp, &la);
    pp_vec3_sub(b, box_pos, &tmp); pp_quat_transform(&inv, &tmp, &lb);

    // Nearest point of the segment to the box, by alternating projection
    // (both are convex, so this converges; a few rounds is plenty).
    PPVec3 origin = {.xyz = {0.0f, 0.0f, 0.0f}}, c, q;
    float t = pp_segment_closest_t(&la, &lb, &origin);
    for (int i = 0; i < 4; ++i) {
        pp_segment_point(&la, &lb, t, &c);
        for (int k = 0; k < 3; ++k) {
            q.xyz[k] = fminf(fmaxf(c.xyz[k], -he->xyz[k]), he->xyz[k]);
        }
        t = pp_segment_closest_t(&la, &lb, &q);
    }

    float ts[3] = {t, t, t};
    int tn = 1;

    PPVec3 point, normal;
    float separation;
    pp_segment_point(&la, &lb, t, &c);
    pp_sphere_box_local(&c, radius, he, &point, &normal, &separation);
    if (separation > reach) return;

    // The stretch of the segment lying over the face it faces: clip it to the
    // box's extent along the axes the normal is NOT pointing along. Its two
    // ends are the other candidates.
    float t0 = 0.0f, t1 = 1.0f;
    for (int k = 0; k < 3 && t0 <= t1; ++k) {
        if (fabsf(normal.xyz[k]) > 0.7f) continue;

        float pa = la.xyz[k], d = lb.xyz[k] - la.xyz[k], h = he->xyz[k];
        if (fabsf(d) < 1e-8f) {
            if (pa < -h || pa > h) t0 = 2.0f; // wholly outside this slab
            continue;
        }
        float ta = (-h - pa) / d, tb = (h - pa) / d;
        if (ta > tb) { float sw = ta; ta = tb; tb = sw; }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
    }
    if (t0 <= t1) {
        ts[tn++] = t0;
        ts[tn++] = t1;
    }

    float merge = 0.1f * radius;
    for (int i = 0; i < tn; ++i) {
        pp_segment_point(&la, &lb, ts[i], &c);
        pp_sphere_box_local(&c, radius, he, &point, &normal, &separation);
        if (separation > reach) continue;

        PPVec3 wp, wn;
        pp_quat_transform(box_rot, &point, &wp);
        pp_vec3_add(&wp, box_pos, &wp);
        pp_quat_transform(box_rot, &normal, &wn);
        pp_capsule_contact_add(out, &wp, &wn, separation, merge);
    }
}

/* Capsule against a one-sided triangle. Like the sphere test, only the face
 * counts: a point of the segment touches the triangle if it lies over it. That
 * keeps a capsule gliding across the seams of a mesh instead of catching on
 * every shared edge. The segment is clipped to the part lying over the
 * triangle, and the two ends of that part are the candidates. `vel` and
 * `a_vel` about `com` give each candidate's approach speed for the speculative
 * test. */
static void pp_capsule_triangle(const PPVec3 *a, const PPVec3 *b, float radius,
                                const PPTriangle *tri, const PPVec3 *vel, const PPVec3 *a_vel,
                                const PPVec3 *com, float t, PPCapsuleContact *out)
{
    out->count = 0;
    const PPVec3 *n = &tri->n;

    float t0 = 0.0f, t1 = 1.0f;
    for (int e = 0; e < 3; ++e) {
        const PPVec3 *va = &tri->v[e];
        const PPVec3 *vb = &tri->v[(e + 1) % 3];
        const PPVec3 *opp = &tri->v[(e + 2) % 3];

        PPVec3 edge, m, to_opp;
        pp_vec3_sub(vb, va, &edge);
        pp_vec3_cross(&edge, n, &m);
        pp_vec3_sub(opp, va, &to_opp);
        if (pp_vec3_dot(&m, &to_opp) > 0.0f) {
            pp_vec3_neg(&m, &m); // outward
        }

        float fa = pp_dot_rel(a, va, &m);
        float fb = pp_dot_rel(b, va, &m);
        if (fa > 0.0f && fb > 0.0f) return; // wholly outside this edge
        if (fa > 0.0f) {
            float tc = fa / (fa - fb);
            if (tc > t0) t0 = tc;
        } else if (fb > 0.0f) {
            float tc = fa / (fa - fb);
            if (tc < t1) t1 = tc;
        }
        if (t0 > t1) return;
    }

    float ts[2] = {t0, t1};
    for (int i = 0; i < 2; ++i) {
        PPVec3 c;
        pp_segment_point(a, b, ts[i], &c);

        float dist = pp_dot_rel(&c, &tri->v[0], n);
        if (dist <= 0.0f) continue; // behind the face: one-sided

        float separation = dist - radius;
        if (separation > PP_CONTACT_MARGIN) {
            PPVec3 r, pv;
            pp_vec3_sub(&c, com, &r);
            pp_vec3_cross(a_vel, &r, &pv);
            pp_vec3_add(&pv, vel, &pv);
            float approach = -pp_vec3_dot(&pv, n);
            if (approach <= 0.0f || separation > approach * t) continue;
        }

        PPVec3 p = {.xyz = {c.x - n->x * dist, c.y - n->y * dist, c.z - n->z * dist}};
        PPVec3 cn;
        pp_vec3_neg(n, &cn);
        pp_capsule_contact_add(out, &p, &cn, separation, 0.1f * radius);
    }
}

/* A box lying across several coplanar triangles collects a clipped polygon
 * from each one, so a single flat face can end up with a dozen unevenly spread
 * contacts. Besides the solver cost, the lopsided distribution makes a soft,
 * sequential solve push harder on the crowded side and spins the box. Reduce
 * each group of same-normal contacts in manifolds[first, count) to at most
 * four: the deepest, the one farthest from it, and the farthest on either side
 * of the line through those two. Returns the new count. */
static int pp_reduce_box_mesh_contacts(PPCollision *manifolds, int first, int count)
{
    int out = first;
    int i = first;

    while (i < count) {
        // Gather the group sharing manifolds[i]'s normal to the front of [i, count).
        PPVec3 n = manifolds[i].n;
        int end = i + 1;
        for (int k = i + 1; k < count; ++k) {
            if (pp_vec3_dot(&manifolds[k].n, &n) > 0.999f) {
                PPCollision tmp = manifolds[end];
                manifolds[end] = manifolds[k];
                manifolds[k] = tmp;
                end++;
            }
        }

        int group = end - i;
        if (group <= 4) {
            for (int k = i; k < end; ++k) manifolds[out++] = manifolds[k];
            i = end;
            continue;
        }

        int keep[4] = {-1, -1, -1, -1};

        keep[0] = i;
        for (int k = i + 1; k < end; ++k) {
            if (manifolds[k].dist > manifolds[keep[0]].dist) keep[0] = k;
        }

        float best = -1.0f;
        for (int k = i; k < end; ++k) {
            PPVec3 d;
            pp_vec3_sub(&manifolds[k].p, &manifolds[keep[0]].p, &d);
            float l = pp_vec3_length_sq(&d);
            if (l > best) { best = l; keep[1] = k; }
        }

        // Signed distance from the line keep[0]->keep[1], measured in the
        // contact plane: side = n x (p1 - p0).
        PPVec3 axis, side;
        pp_vec3_sub(&manifolds[keep[1]].p, &manifolds[keep[0]].p, &axis);
        pp_vec3_cross(&n, &axis, &side);

        float max_side = 1e-6f, min_side = -1e-6f;
        for (int k = i; k < end; ++k) {
            PPVec3 d;
            pp_vec3_sub(&manifolds[k].p, &manifolds[keep[0]].p, &d);
            float sd = pp_vec3_dot(&d, &side);
            if (sd > max_side) { max_side = sd; keep[2] = k; }
            if (sd < min_side) { min_side = sd; keep[3] = k; }
        }

        PPCollision kept[4];
        int kept_count = 0;
        for (int k = 0; k < 4; ++k) {
            if (keep[k] >= 0) kept[kept_count++] = manifolds[keep[k]];
        }
        for (int k = 0; k < kept_count; ++k) manifolds[out++] = kept[k];

        i = end;
    }

    return out;
}

/* One-sided box vs triangle, matching the sphere/triangle convention: the
 * triangle is a one-sided face and the box is only resolved when its centre is
 * on the front (+normal) side. The box's incident face (the one most directly
 * facing the triangle) is clipped against the triangle's three edge planes, and
 * each surviving corner becomes a contact. Penetrating corners are real
 * contacts; corners still in front become *speculative* contacts when the box
 * is closing fast enough to cross this step, which stops fast boxes tunnelling.
 *
 * The contact normal points from the box into the triangle (-tri->n); the solver
 * then pushes the box out along +tri->n. out->depths[] are SIGNED: positive =
 * penetration, negative = remaining gap (the caller forms dist / separation). */
static int pp_sat_box_triangle(const PPVec3 *box_pos, const PPQuaternion *box_rot,
                               const PPVec3 *box_he, const PPTriangle *tri,
                               const PPVec3 *box_vel, float t, PPBoxContact *out)
{
    const PPVec3 *n = &tri->n;

    PPVec3 ub[3];
    pp_box_world_axes(box_rot, ub);

    /* One-sided: the box centre must be in front of the triangle plane. This
     * both implements the one-sided behaviour (a box approaching from behind is
     * ignored, like a sphere) and guarantees the incident face faces the
     * triangle. */
    float sd = pp_dot_rel(box_pos, &tri->v[0], n);
    if (sd <= 0.0f) {
        out->count = 0;
        return 0;
    }

    /* Incident face: box face whose outward normal is most opposed to n. */
    int inc = 0;
    float best = -1.0f;
    float extent = 0.0f; /* half-width of the box measured along n */
    for (int k = 0; k < 3; ++k) {
        float a = fabsf(pp_vec3_dot(&ub[k], n));
        extent += a * box_he->xyz[k];
        if (a > best) { best = a; inc = k; }
    }

    /* Too far above the plane for even the nearest corner to reach it this
     * step: nothing to clip. Most broadphase candidates end here. */
    {
        float closing = -pp_vec3_dot(box_vel, n);
        if (sd - extent > PP_CONTACT_MARGIN + fmaxf(closing, 0.0f) * t) {
            out->count = 0;
            return 0;
        }
    }
    PPVec3 incN = ub[inc];
    if (pp_vec3_dot(&incN, n) > 0.0f) {
        pp_vec3_neg(&incN, &incN); /* point toward the triangle (-n side) */
    }

    PPVec3 faceCenter;
    {
        PPVec3 off; pp_vec3_scale(&incN, box_he->xyz[inc], &off);
        pp_vec3_add(box_pos, &off, &faceCenter);
    }
    int ia = (inc + 1) % 3, ib = (inc + 2) % 3;
    float hia = box_he->xyz[ia], hib = box_he->xyz[ib];

    /* Four incident-face corners. */
    PPVec3 bufA[PP_MAX_BOX_CONTACTS * 2], bufB[PP_MAX_BOX_CONTACTS * 2];
    const float sa[4] = { 1.0f, 1.0f, -1.0f, -1.0f };
    const float sb[4] = { 1.0f, -1.0f, -1.0f, 1.0f };
    for (int v = 0; v < 4; ++v) {
        PPVec3 p = faceCenter, add;
        pp_vec3_scale(&ub[ia], sa[v] * hia, &add); pp_vec3_add(&p, &add, &p);
        pp_vec3_scale(&ub[ib], sb[v] * hib, &add); pp_vec3_add(&p, &add, &p);
        bufA[v] = p;
    }

    /* Clip against the triangle's three edge half-spaces (each plane is
     * perpendicular to the triangle, through an edge, normal pointing outward).
     * A corner outside any edge is removed, so a box face that does not overlap
     * the triangle laterally yields no contacts. */
    const PPVec3 *V[3] = { &tri->v[0], &tri->v[1], &tri->v[2] };
    PPVec3 *src = bufA, *dst = bufB;
    int cnt = 4;
    for (int e = 0; e < 3; ++e) {
        const PPVec3 *a = V[e];
        const PPVec3 *b = V[(e + 1) % 3];
        const PPVec3 *opp = V[(e + 2) % 3];
        PPVec3 edge; pp_vec3_sub(b, a, &edge);
        PPVec3 m; pp_vec3_cross(&edge, n, &m);
        PPVec3 to_opp; pp_vec3_sub(opp, a, &to_opp);
        if (pp_vec3_dot(&m, &to_opp) > 0.0f) {
            pp_vec3_neg(&m, &m); /* orient outward (away from the interior) */
        }
        cnt = pp_clip_halfspace(src, cnt, dst, a, &m, 0.0f);
        PPVec3 *tmp = src; src = dst; dst = tmp;
        if (cnt == 0) break;
    }

    pp_vec3_neg(n, &out->normal); /* box -> triangle */
    out->count = 0;
    if (cnt == 0) {
        return 0;
    }

    /* Approach speed: velocity component heading into the front face. */
    float approach = -pp_vec3_dot(box_vel, n);

    /* Every clipped corner that is penetrating, or close enough to reach the
     * plane this step, becomes a contact at that corner. Depth is how far
     * behind the plane it is: negative for a gap, which the solver treats as
     * speculative (it only removes the approach speed that would close more
     * than the gap).
     *
     * The reach never drops below the contact margin, so all four corners of a
     * resting face stay in the manifold; with only the corners that happen to
     * be below the plane this step, the box is supported off-centre and rocks.
     * And because each contact sits on the corner it belongs to, a box that
     * lands corner-first is stopped -- and bounced -- at that corner, with the
     * lever arm that implies, rather than as if it were a ball. */
    float travel = (approach > 0.0f) ? approach * t : 0.0f;
    float reach = PP_CONTACT_MARGIN + travel;
    float min_front = FLT_MAX;

    for (int i = 0; i < cnt && out->count < PP_MAX_BOX_CONTACTS; ++i) {
        float front = pp_dot_rel(&src[i], &tri->v[0], n); /* >0 in front, <0 behind */
        if (front < min_front) min_front = front;
        if (front <= reach) {
            out->points[out->count] = src[i];
            out->depths[out->count] = -front;
            out->count++;
        }
    }

    /* The exception is a box moving further in one step than it is wide, caught
     * while still clear of the surface. Stopping that across several corners
     * leaves a small fraction of the impulse behind as spin, and a small
     * fraction of a huge impulse is enough to fling the box away. Corner detail
     * means nothing at that speed anyway, so brake it with one contact at the
     * box centre projected onto the plane: no lever arm, no spin, and the same
     * point for every coplanar triangle. The corners take over next step. */
    float size = 2.0f * fminf(box_he->x, fminf(box_he->y, box_he->z));
    if (out->count > 0 && travel > size && min_front > PP_CONTACT_MARGIN) {
        PPVec3 off;
        pp_vec3_scale(n, sd, &off);
        pp_vec3_sub(box_pos, &off, &out->points[0]);
        out->depths[0] = -min_front;
        out->count = 1;
    }
    return out->count;
}

/* Turn a capsule test's result into solver contacts. The capsule is obj1. */
static int pp_emit_capsule_contacts(PPCollision *manifolds, int manifold_count,
                                    const PPCapsuleContact *cc, const PPCollision *rep)
{
    for (int k = 0; k < cc->count && manifold_count < PICOPHYSICS_MAX_MANIFOLDS; ++k) {
        PPCollision c = *rep;
        c.p = cc->points[k];
        c.n = cc->normals[k];
        c.dist = -cc->separations[k];
        c.separation = cc->separations[k];
        manifolds[manifold_count++] = c;
    }
    return manifold_count;
}

/* The contact handed to a collision callback for a capsule pair: the deepest. */
static void pp_capsule_rep(const PPCapsuleContact *cc, PPBody *capsule_body, const PPShape *capsule_shape,
                           PPBody *other_body, PPObjectType other_type, BodyKind other_kind,
                           float other_bounce, float other_friction, PPCollision *rep)
{
    int deepest = 0;
    for (int k = 1; k < cc->count; ++k) {
        if (cc->separations[k] < cc->separations[deepest]) deepest = k;
    }

    memset(rep, 0, sizeof(*rep));
    rep->p = cc->points[deepest];
    rep->n = cc->normals[deepest];
    rep->dist = -cc->separations[deepest];
    rep->separation = cc->separations[deepest];
    rep->obj1 = capsule_body;
    rep->obj2 = other_body;
    rep->obj1_bounce = capsule_body->bounce;
    rep->obj2_bounce = other_bounce;
    rep->obj1_friction = capsule_body->friction;
    rep->obj2_friction = other_friction;
    rep->type1 = PP_OBJECT_TYPE_CAPSULE;
    rep->type2 = other_type;
    rep->kind1 = capsule_shape->kind;
    rep->kind2 = other_kind;
    rep->roll_radius = capsule_shape->capsule.radius;
}

static int pp_sleep_find(int *parent, int i)
{
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}

static inline int pp_body_index(const PPBody *b)
{
    return (b >= objects && b < objects + PICOPHYSICS_MAX_OBJECTS) ? (int)(b - objects) : -1;
}

/* ---- Bounded ray cast ----------------------------------------------------- */

static void pp_ray_test_triangles(const PPTriangle *list, int count,
                                  const PPVec3 *o, const PPVec3 *d, float t_max,
                                  BodyKind *ignore_kinds, PPRayHit *best)
{
    // Bounds of the part of the ray that could still produce a nearer hit.
    float reach = fminf(t_max, best->distance);
    PPVec3 end = {.xyz = {o->x + d->x * reach, o->y + d->y * reach, o->z + d->z * reach}};
    float min_x = fminf(o->x, end.x), max_x = fmaxf(o->x, end.x);
    float min_y = fminf(o->y, end.y), max_y = fmaxf(o->y, end.y);
    float min_z = fminf(o->z, end.z), max_z = fmaxf(o->z, end.z);

    for (int i = 0; i < count; ++i) {
        const PPTriangle *t = &list[i];
        if (t->aabb_min_x > max_x || t->aabb_max_x < min_x ||
            t->aabb_min_y > max_y || t->aabb_max_y < min_y ||
            t->aabb_min_z > max_z || t->aabb_max_z < min_z) {
            continue;
        }

        PPVec3 point;
        float dist;
        if (!pp_tri_intersect(t, o, d, &point, &dist)) continue;
        if (dist < 0.0f || dist > t_max || dist >= best->distance) continue;
        if (pp_contains_kind(ignore_kinds, t->kind)) continue;

        best->body = NULL;
        best->kind = t->kind;
        best->distance = dist;
        best->point = point;
        best->normal = t->n;
        if (pp_vec3_dot(&t->n, d) > 0.0f) {
            pp_vec3_neg(&t->n, &best->normal);
        }
    }
}

/* Triangles along [t0, t1] of the ray, through the host query. A query that
 * comes back full may have dropped the triangle that matters, so that stretch
 * is halved and asked for again. Nearest half first: a hit there ends it. */
static void pp_ray_query_triangles(const PPVec3 *o, const PPVec3 *d, float t0, float t1,
                                   BodyKind *ignore_kinds, int depth, PPRayHit *best)
{
    static PPTriangle candidates[PICOPHYSICS_MAX_QUERY_TRIANGLES];

    if (t0 >= best->distance) {
        return;
    }

    float mid = 0.5f * (t0 + t1);
    float reach = 0.5f * (t1 - t0) + PP_CONTACT_MARGIN;
    PPVec3 centre = {.xyz = {o->x + d->x * mid, o->y + d->y * mid, o->z + d->z * mid}};

    int count = tri_query(&centre, reach, candidates, PICOPHYSICS_MAX_QUERY_TRIANGLES, tri_query_user);
    if (count > PICOPHYSICS_MAX_QUERY_TRIANGLES) count = PICOPHYSICS_MAX_QUERY_TRIANGLES;

    if (count == PICOPHYSICS_MAX_QUERY_TRIANGLES && depth < 8) {
        pp_ray_query_triangles(o, d, t0, mid, ignore_kinds, depth + 1, best);
        pp_ray_query_triangles(o, d, mid, t1, ignore_kinds, depth + 1, best);
        return;
    }

    // Only hits inside this stretch count: one beyond it might be beaten by a
    // triangle this query had no reason to return.
    pp_ray_test_triangles(candidates, count, o, d, t1, ignore_kinds, best);
}

bool pp_physics_ray_cast(const PPVec3 *origin,
                         const PPVec3 *direction,
                         float max_distance,
                         BodyKind *ignore_kinds,
                         const PPBody *ignore_body,
                         PPRayHit *hit)
{
    PPVec3 d = *direction;
    float len = pp_vec3_length(&d);
    if (len < 1e-12f || max_distance <= 0.0f) {
        return false;
    }
    pp_vec3_scale(&d, pp_invf(len), &d);

    PPRayHit best;
    memset(&best, 0, sizeof(best));
    best.distance = FLT_MAX;

    if (tri_query) {
        pp_ray_query_triangles(origin, &d, 0.0f, max_distance, ignore_kinds, 0, &best);
    } else {
        pp_ray_test_triangles(tris, tri_count, origin, &d, max_distance, ignore_kinds, &best);
    }

    for (int i = 0; i < object_count; ++i) {
        const PPBody *body = &objects[i];
        if (!body->is_alive || body == ignore_body || body->shape_count == 0) {
            continue;
        }

        // Bounding sphere against the (possibly already shortened) ray.
        float reach = fminf(max_distance, best.distance);
        float radius = pp_body_get_radius((PPBody *) body);
        PPVec3 rel;
        pp_vec3_sub(&body->pos, origin, &rel);
        float along = pp_vec3_dot(&rel, &d);
        if (along < -radius || along > reach + radius) continue;
        if (pp_vec3_length_sq(&rel) - along * along > radius * radius) continue;

        for (int si = 0; si < body->shape_count; ++si) {
            const PPShape *shape = &body->shapes[si];
            if (pp_contains_kind(ignore_kinds, shape->kind)) {
                continue;
            }

            PPVec3 spos, point, normal;
            float dist;
            pp_shape_world_pos(body, shape, &spos);

            if (shape->type == PP_OBJECT_TYPE_SPHERE) {
                if (!pp_sphere_intersect(&spos, shape->sphere.radius, origin, &d, &point, &dist)) continue;
                pp_vec3_sub(&point, &spos, &normal);
            } else if (shape->type == PP_OBJECT_TYPE_BOX) {
                if (!pp_box_intersect(&spos, &body->rot, &shape->box.whd, origin, &d, &point, &dist)) continue;

                // The face hit is the one the point sits proudest of.
                PPQuaternion inv;
                PPVec3 rel_hit, local;
                pp_quat_conjugate(&body->rot, &inv);
                pp_vec3_sub(&point, &spos, &rel_hit);
                pp_quat_transform(&inv, &rel_hit, &local);

                int axis = 0;
                float proudest = -FLT_MAX;
                for (int a = 0; a < 3; ++a) {
                    float over = fabsf(local.xyz[a]) - 0.5f * shape->box.whd.xyz[a];
                    if (over > proudest) {
                        proudest = over;
                        axis = a;
                    }
                }

                PPVec3 local_n = {.xyz = {0.0f, 0.0f, 0.0f}};
                local_n.xyz[axis] = local.xyz[axis] < 0.0f ? -1.0f : 1.0f;
                pp_quat_transform(&body->rot, &local_n, &normal);
            } else if (shape->type == PP_OBJECT_TYPE_CAPSULE) {
                PPVec3 a, b, on_axis;
                pp_capsule_segment(body, shape, &spos, &a, &b);
                if (!pp_capsule_intersect(&a, &b, shape->capsule.radius, origin, &d, &point, &dist)) continue;
                pp_segment_point(&a, &b, pp_segment_closest_t(&a, &b, &point), &on_axis);
                pp_vec3_sub(&point, &on_axis, &normal);
            } else {
                continue;
            }

            if (dist < 0.0f || dist > max_distance || dist >= best.distance) continue;

            float n_len = pp_vec3_length(&normal);
            if (n_len > 1e-9f) {
                pp_vec3_scale(&normal, pp_invf(n_len), &normal);
            } else {
                pp_vec3_neg(&d, &normal);
            }

            best.body = body;
            best.kind = shape->kind;
            best.distance = dist;
            best.point = point;
            best.normal = normal;
        }
    }

    if (best.distance == FLT_MAX) {
        return false;
    }

    if (hit) {
        *hit = best;
    }
    return true;
}

/* Decide who goes to sleep. Bodies joined by contacts or constraints form a
 * group, and a group sleeps only when every member has been still for
 * sleep_delay: one restless body keeps the pile it is touching awake, which is
 * what stops a stack from freezing mid-collapse. */
static void pp_update_sleeping(const PPCollision *manifolds, int manifold_count, float t)
{
    static int parent[PICOPHYSICS_MAX_OBJECTS];
    static float group_time[PICOPHYSICS_MAX_OBJECTS];
    static uint32_t group_id[PICOPHYSICS_MAX_OBJECTS];

    const float lin_sq = sleep_linear_threshold * sleep_linear_threshold;
    const float ang_sq = sleep_angular_threshold * sleep_angular_threshold;

    for (int i = 0; i < object_count; ++i) {
        PPBody *b = &objects[i];
        parent[i] = i;

        if (!b->is_alive || b->is_asleep || b->inv_mass == 0.0f) continue;

        if (!b->never_sleeps && pp_vec3_length_sq(&b->vel) < lin_sq &&
            pp_vec3_length_sq(&b->a_vel) < ang_sq) {
            b->sleep_time += t;
        } else {
            b->sleep_time = 0.0f;
        }
    }

    // Only moving bodies tie a group together; a static or kinematic body
    // that two piles both rest on does not make them one pile.
    for (int i = 0; i < manifold_count; ++i) {
        int a = pp_body_index(manifolds[i].obj1);
        int b = pp_body_index(manifolds[i].obj2);
        if (a < 0 || b < 0) continue;
        if (objects[a].inv_mass == 0.0f || objects[b].inv_mass == 0.0f) continue;
        parent[pp_sleep_find(parent, a)] = pp_sleep_find(parent, b);
    }

    for (int i = 0; i < constraint_count; ++i) {
        const PPConstraint *c = &constraints[i];
        if (!c->is_alive) continue;
        int a = pp_body_index(c->body1);
        int b = pp_body_index(c->body2);
        if (a < 0 || b < 0) continue;
        if (objects[a].inv_mass == 0.0f || objects[b].inv_mass == 0.0f) continue;
        parent[pp_sleep_find(parent, a)] = pp_sleep_find(parent, b);
    }

    for (int i = 0; i < object_count; ++i) {
        group_time[i] = FLT_MAX;
        group_id[i] = 0;
    }

    for (int i = 0; i < object_count; ++i) {
        const PPBody *b = &objects[i];
        if (!b->is_alive || b->is_asleep || b->inv_mass == 0.0f) continue;
        int root = pp_sleep_find(parent, i);
        if (b->sleep_time < group_time[root]) group_time[root] = b->sleep_time;
    }

    for (int i = 0; i < object_count; ++i) {
        PPBody *b = &objects[i];
        if (!b->is_alive || b->is_asleep || b->inv_mass == 0.0f) continue;

        int root = pp_sleep_find(parent, i);
        if (group_time[root] < sleep_delay) continue;

        if (group_id[root] == 0) {
            group_id[root] = ++sleep_group_counter;
        }

        b->sleep_group = group_id[root];
        b->is_asleep = true;
        pp_vec3_init(&b->vel);
        pp_vec3_init(&b->a_vel);
    }
}

void pp_physics_step(float t, int vel_iterations, int pos_iterations)
{
    int manifold_count = 0;
    static PPCollision manifolds[PICOPHYSICS_MAX_MANIFOLDS];

    /* Candidate triangles from the host query, refilled once per shape. Static
     * rather than a stack local: PICOPHYSICS_MAX_QUERY_TRIANGLES triangles is
     * several kilobytes, which is more than a Dreamcast thread stack wants to
     * carry. Unused when no query is set. */
    static PPTriangle tri_candidates[PICOPHYSICS_MAX_QUERY_TRIANGLES];

    tri_query_overflows = 0;
    tri_query_max_candidates = 0;

    for (int i = 0; i < object_count; ++i) {
        if (objects[i].is_alive && objects[i].inv_mass != 0.0f && !objects[i].is_asleep) {
            pp_body_update_world_inertia(&objects[i]);
        }
    }

    // Bounding sphere of each body, grown by how far it can move this step, so
    // the pair loop below can discard distant bodies before doing any per-shape
    // work.
    static float body_reach[PICOPHYSICS_MAX_OBJECTS];
    for (int i = 0; i < object_count; ++i) {
        if (objects[i].is_alive && objects[i].shape_count > 0) {
            body_reach[i] = pp_body_get_radius(&objects[i]) + PP_CONTACT_MARGIN +
                            pp_vec3_length(&objects[i].vel) * t;
        }
    }

    // Wake sleepers that something moving is about to reach. Done before
    // detection so that a woken body gets its full set of contacts this step,
    // and on bounding spheres so that it happens a frame ahead of the impact.
    // Only a body that is actually moving counts: one that is merely awake and
    // resting nearby must not keep its neighbours up.
    if (sleeping_enabled) {
        for (int i = 0; i < object_count; ++i) {
            PPBody *mover = &objects[i];
            if (!mover->is_alive || mover->is_asleep || mover->shape_count == 0) continue;

            bool moving;
            if (mover->inv_mass == 0.0f) {
                moving = pp_vec3_length_sq(&mover->vel) > 0.0f ||
                         pp_vec3_length_sq(&mover->a_vel) > 0.0f;
            } else {
                moving = mover->sleep_time == 0.0f;
            }
            if (!moving) continue;

            for (int j = 0; j < object_count; ++j) {
                PPBody *sleeper = &objects[j];
                if (!sleeper->is_alive || !sleeper->is_asleep || sleeper->shape_count == 0) continue;

                float pair_reach = body_reach[i] + body_reach[j];
                if (pp_vec3_dist_sq(&mover->pos, &sleeper->pos) <= pair_reach * pair_reach) {
                    pp_body_wake(sleeper);
                }
            }
        }

        // A sleeper jointed to something that has started moving (a kinematic
        // platform towing it, say) has to come along.
        for (int i = 0; i < constraint_count; ++i) {
            const PPConstraint *c = &constraints[i];
            if (!c->is_alive || c->body1->is_asleep == c->body2->is_asleep) continue;

            PPBody *sleeper = c->body1->is_asleep ? c->body1 : c->body2;
            const PPBody *other = c->body1->is_asleep ? c->body2 : c->body1;
            if (other->inv_mass != 0.0f || pp_vec3_length_sq(&other->vel) > 0.0f ||
                pp_vec3_length_sq(&other->a_vel) > 0.0f) {
                pp_body_wake(sleeper);
            }
        }
    }

    for (int i = 0; i < object_count; ++i) {
        PPBody *lhs_body = &objects[i];

        if (!lhs_body->is_alive || lhs_body->shape_count == 0) {
            continue;
        }

        const struct _PPCollisionMapEntry *cb = NULL;

        // Shape/triangle collisions: iterate shapes of this body. Spheres and
        // boxes both collide with the static triangle soup. A sleeping body
        // is not being simulated, so it has no use for contacts.
        for (int si = 0; si < lhs_body->shape_count && !lhs_body->is_asleep; ++si) {
            const PPShape *lhs_shape = &lhs_body->shapes[si];
            if (lhs_shape->type != PP_OBJECT_TYPE_SPHERE &&
                lhs_shape->type != PP_OBJECT_TYPE_BOX &&
                lhs_shape->type != PP_OBJECT_TYPE_CAPSULE) {
                continue;
            }

            PPVec3 shape_pos;
            pp_shape_world_pos(lhs_body, lhs_shape, &shape_pos);

            float radius;
            if (lhs_shape->type == PP_OBJECT_TYPE_SPHERE) {
                radius = lhs_shape->sphere.radius;
            } else if (lhs_shape->type == PP_OBJECT_TYPE_CAPSULE) {
                radius = lhs_shape->capsule.radius + lhs_shape->capsule.half_height;
            } else {
                radius = lhs_shape->box.bounding_radius;
            }

            PPVec3 cap_a, cap_b, cap_com;
            if (lhs_shape->type == PP_OBJECT_TYPE_CAPSULE) {
                pp_capsule_segment(lhs_body, lhs_shape, &shape_pos, &cap_a, &cap_b);
                pp_body_world_com(lhs_body, &cap_com);
            }
            float pos_x = shape_pos.x;
            float pos_y = shape_pos.y;
            float pos_z = shape_pos.z;

            // Expand the broadphase by the distance the shape may travel this
            // step. A fast mover that the static radius check would cull is
            // still considered, so it can be caught speculatively before it
            // tunnels through a triangle.
            float speed = pp_vec3_length(&lhs_body->vel);
            float reach = radius + PP_CONTACT_MARGIN + speed * t;
            float reach_sq = reach * reach;

            // Box/triangle contacts are deduplicated across triangles for this
            // shape: a box spanning a shared edge of two coplanar triangles
            // would otherwise get the same corner contact from both, and those
            // doubled contacts make the solver converge to a wrong, energy-
            // injecting state. Track emitted (point,normal) pairs and skip
            // near-coincident repeats.
            PPVec3 emitted_pt[32];
            PPVec3 emitted_n[32];
            int emitted_count = 0;
            int shape_first_manifold = manifold_count;

            /* Source of static triangles for this shape: either the host's
             * broadphase, asked once here (not once per solver iteration --
             * the solver runs later, over the manifolds this loop builds), or
             * the global tris[] array when no query is set. */
            const PPTriangle *tri_src = tris;
            int tri_src_count = tri_count;
            if (tri_query) {
                tri_src = tri_candidates;
                tri_src_count = tri_query(&shape_pos, reach, tri_candidates,
                                          PICOPHYSICS_MAX_QUERY_TRIANGLES,
                                          tri_query_user);
                if (tri_src_count < 0) {
                    tri_src_count = 0;
                } else if (tri_src_count >= PICOPHYSICS_MAX_QUERY_TRIANGLES) {
                    tri_src_count = PICOPHYSICS_MAX_QUERY_TRIANGLES;
                    tri_query_overflows++;
                }
                if (tri_src_count > tri_query_max_candidates) {
                    tri_query_max_candidates = tri_src_count;
                }
            }

            int last_kind = -1;
            for (int j = 0; j < tri_src_count; ++j) {
                const PPTriangle *tri = tri_src + j;

                float dx = fmaxf(0.0f, fmaxf(tri->aabb_min_x - pos_x, pos_x - tri->aabb_max_x));
                float dy = fmaxf(0.0f, fmaxf(tri->aabb_min_y - pos_y, pos_y - tri->aabb_max_y));
                float dz = fmaxf(0.0f, fmaxf(tri->aabb_min_z - pos_z, pos_z - tri->aabb_max_z));

                float dist_sq = dx*dx + dy*dy + dz*dz;
                if (dist_sq > reach_sq) {
                    continue;
                }

                if(tri->kind != last_kind) {
                    last_kind = tri->kind;
                    cb = pp_physics_collision_map_search(lhs_shape->kind, tri->kind);
                }

                if(cb && !cb->collision_callback) {
                    continue;
                }

                if (lhs_shape->type == PP_OBJECT_TYPE_SPHERE) {
                    PPVec3 p, d;
                    pp_vec3_scale(&tri->n, -1.0f, &d);
                    float dist; // perpendicular distance from sphere centre to plane
                    if (pp_tri_intersect(tri, &shape_pos, &d, &p, &dist)) {
                        float separation = dist - lhs_shape->sphere.radius;

                        // Either the sphere is already touching/penetrating, or it
                        // has a gap but is approaching fast enough to close it this
                        // step -- a speculative contact that stops fast spheres
                        // tunnelling through the triangle.
                        // A small margin keeps a resting sphere's contact alive
                        // from step to step even when it sits a hair above the
                        // surface; the solver treats the gap speculatively.
                        bool contact = false;
                        if (separation <= PP_CONTACT_MARGIN) {
                            contact = true;
                        } else {
                            float approach = -pp_vec3_dot(&lhs_body->vel, &tri->n);
                            if (approach > 0.0f && separation <= approach * t) {
                                contact = true;
                            }
                        }

                        if (contact) {
                            PPCollision c;
                            pp_fill_collision_info_sphere_triangle(lhs_body, lhs_shape,
                                                                   tri, &p,
                                                                   lhs_shape->sphere.radius - dist,
                                                                   &c);

                            bool respond = true;
                            if (cb) {
                                respond = cb->collision_callback(lhs_body, tri,
                                                                 lhs_shape->kind, tri->kind,
                                                                 &c, cb->user_data);
                            }

                            if (respond) {
                                manifolds[manifold_count++] = c;
                            }
                        }
                    }
                } else if (lhs_shape->type == PP_OBJECT_TYPE_CAPSULE) {
                    PPCapsuleContact cc;
                    pp_capsule_triangle(&cap_a, &cap_b, lhs_shape->capsule.radius, tri,
                                        &lhs_body->vel, &lhs_body->a_vel, &cap_com, t, &cc);
                    if (cc.count > 0) {
                        PPCollision rep;
                        pp_capsule_rep(&cc, lhs_body, lhs_shape, &trimesh_body,
                                       PP_OBJECT_TYPE_TRIANGLE, tri->kind,
                                       tri->bounce, tri->friction, &rep);

                        bool respond = true;
                        if (cb) {
                            respond = cb->collision_callback(lhs_body, tri,
                                                             lhs_shape->kind, tri->kind,
                                                             &rep, cb->user_data);
                        }

                        if (respond) {
                            // A capsule lying across a shared edge gets the
                            // same point from both triangles; keep one.
                            PPCapsuleContact unique;
                            unique.count = 0;
                            for (int k = 0; k < cc.count; ++k) {
                                bool dup = false;
                                for (int e = 0; e < emitted_count; ++e) {
                                    PPVec3 diff;
                                    pp_vec3_sub(&cc.points[k], &emitted_pt[e], &diff);
                                    if (pp_vec3_length_sq(&diff) < 1e-4f &&
                                        pp_vec3_dot(&cc.normals[k], &emitted_n[e]) > 0.999f) {
                                        dup = true;
                                        break;
                                    }
                                }
                                if (dup) continue;

                                if (emitted_count < 32) {
                                    emitted_pt[emitted_count] = cc.points[k];
                                    emitted_n[emitted_count] = cc.normals[k];
                                    emitted_count++;
                                }

                                unique.points[unique.count] = cc.points[k];
                                unique.normals[unique.count] = cc.normals[k];
                                unique.separations[unique.count] = cc.separations[k];
                                unique.count++;
                            }

                            manifold_count = pp_emit_capsule_contacts(manifolds, manifold_count,
                                                                      &unique, &rep);
                        }
                    }
                } else {
                    // Box vs triangle via SAT, one-sided, with a clipped
                    // multi-point manifold and speculative contacts.
                    PPBoxContact bc;
                    int nc = pp_sat_box_triangle(&shape_pos, &lhs_body->rot,
                                                 &lhs_shape->box.half_extents, tri,
                                                 &lhs_body->vel, t, &bc);
                    if (nc > 0) {
                        int deepest = 0;
                        for (int k = 1; k < nc; ++k) {
                            if (bc.depths[k] > bc.depths[deepest]) deepest = k;
                        }

                        PPCollision rep;
                        rep.n = bc.normal;
                        rep.p = bc.points[deepest];
                        rep.dist = bc.depths[deepest];
                        rep.separation = -bc.depths[deepest];
                        rep.obj1 = lhs_body;
                        rep.obj2 = &trimesh_body;
                        rep.obj1_bounce = lhs_body->bounce;
                        rep.obj2_bounce = tri->bounce;
                        rep.obj1_friction = lhs_body->friction;
                        rep.obj2_friction = tri->friction;
                        rep.type1 = PP_OBJECT_TYPE_BOX;
                        rep.type2 = PP_OBJECT_TYPE_TRIANGLE;
                        rep.kind1 = lhs_shape->kind;
                        rep.kind2 = tri->kind;

                        bool respond = true;
                        if (cb) {
                            respond = cb->collision_callback(lhs_body, tri,
                                                             lhs_shape->kind, tri->kind,
                                                             &rep, cb->user_data);
                        }

                        if (respond) {
                            for (int k = 0; k < nc &&
                                            manifold_count < PICOPHYSICS_MAX_MANIFOLDS; ++k) {
                                // Skip a contact coincident with one already
                                // emitted for this shape with the same normal.
                                bool dup = false;
                                for (int e = 0; e < emitted_count; ++e) {
                                    PPVec3 diff;
                                    pp_vec3_sub(&bc.points[k], &emitted_pt[e], &diff);
                                    if (pp_vec3_length_sq(&diff) < 1e-6f &&
                                        pp_vec3_dot(&bc.normal, &emitted_n[e]) > 0.999f) {
                                        dup = true;
                                        break;
                                    }
                                }
                                if (dup) continue;

                                if (emitted_count < 32) {
                                    emitted_pt[emitted_count] = bc.points[k];
                                    emitted_n[emitted_count] = bc.normal;
                                    emitted_count++;
                                }

                                PPCollision c = rep;
                                c.p = bc.points[k];
                                c.dist = bc.depths[k];
                                c.separation = -bc.depths[k];
                                manifolds[manifold_count++] = c;
                            }
                        }
                    }
                }
            }
            if (lhs_shape->type == PP_OBJECT_TYPE_BOX || lhs_shape->type == PP_OBJECT_TYPE_CAPSULE) {
                manifold_count = pp_reduce_box_mesh_contacts(manifolds, shape_first_manifold,
                                                             manifold_count);
            }
        }

        // Body-body collisions
        for (int j = i + 1; j < object_count; ++j) {
            PPBody *rhs_body = &objects[j];

            if (!rhs_body->is_alive || rhs_body->shape_count == 0) {
                continue;
            }

            // A sleeping body against another sleeper, or against something
            // that cannot move, has nothing to resolve.
            if ((lhs_body->is_asleep && (rhs_body->is_asleep || rhs_body->inv_mass == 0.0f)) ||
                (rhs_body->is_asleep && lhs_body->inv_mass == 0.0f)) {
                continue;
            }

            float pair_reach = body_reach[i] + body_reach[j];
            if (pp_vec3_dist_sq(&lhs_body->pos, &rhs_body->pos) > pair_reach * pair_reach) {
                continue;
            }

            if (constraint_count && pp_bodies_jointed(lhs_body, rhs_body)) {
                continue;
            }

            // Iterate shape pairs
            for (int si = 0; si < lhs_body->shape_count; ++si) {
                const PPShape *lhs_shape = &lhs_body->shapes[si];
                PPVec3 lhs_spos;
                pp_shape_world_pos(lhs_body, lhs_shape, &lhs_spos);

                for (int sj = 0; sj < rhs_body->shape_count; ++sj) {
                    const PPShape *rhs_shape = &rhs_body->shapes[sj];
                    PPVec3 rhs_spos;
                    pp_shape_world_pos(rhs_body, rhs_shape, &rhs_spos);

                    cb = pp_physics_collision_map_search(lhs_shape->kind, rhs_shape->kind);
                    if (cb && !cb->collision_callback) {
                        continue;
                    }

                    if (lhs_shape->type == PP_OBJECT_TYPE_CAPSULE || rhs_shape->type == PP_OBJECT_TYPE_CAPSULE) {
                        // The capsule is always obj1 of the contact.
                        bool lhs_is_capsule = lhs_shape->type == PP_OBJECT_TYPE_CAPSULE;
                        PPBody *cap_body = lhs_is_capsule ? lhs_body : rhs_body;
                        PPBody *other_body = lhs_is_capsule ? rhs_body : lhs_body;
                        const PPShape *cap_shape = lhs_is_capsule ? lhs_shape : rhs_shape;
                        const PPShape *other_shape = lhs_is_capsule ? rhs_shape : lhs_shape;
                        const PPVec3 *cap_pos = lhs_is_capsule ? &lhs_spos : &rhs_spos;
                        const PPVec3 *other_pos = lhs_is_capsule ? &rhs_spos : &lhs_spos;

                        PPVec3 rel_v, ca, cbp;
                        pp_vec3_sub(&rhs_body->vel, &lhs_body->vel, &rel_v);
                        float reach = PP_CONTACT_MARGIN + pp_vec3_length(&rel_v) * t;
                        pp_capsule_segment(cap_body, cap_shape, cap_pos, &ca, &cbp);

                        PPCapsuleContact cc;
                        cc.count = 0;
                        if (other_shape->type == PP_OBJECT_TYPE_SPHERE) {
                            pp_capsule_sphere(&ca, &cbp, cap_shape->capsule.radius,
                                              other_pos, other_shape->sphere.radius, reach, &cc);
                        } else if (other_shape->type == PP_OBJECT_TYPE_CAPSULE) {
                            PPVec3 oa, ob;
                            pp_capsule_segment(other_body, other_shape, other_pos, &oa, &ob);
                            pp_capsule_capsule(&ca, &cbp, cap_shape->capsule.radius,
                                               &oa, &ob, other_shape->capsule.radius, reach, &cc);
                        } else if (other_shape->type == PP_OBJECT_TYPE_BOX) {
                            pp_capsule_box(&ca, &cbp, cap_shape->capsule.radius,
                                           other_pos, &other_body->rot, &other_shape->box.half_extents,
                                           reach, &cc);
                        }

                        if (cc.count > 0) {
                            PPCollision rep;
                            pp_capsule_rep(&cc, cap_body, cap_shape, other_body,
                                           other_shape->type, other_shape->kind,
                                           other_body->bounce, other_body->friction, &rep);

                            bool respond = true;
                            if (cb) {
                                respond = cb->collision_callback(cap_body, other_body,
                                                                 cap_shape->kind, other_shape->kind,
                                                                 &rep, cb->user_data);
                            }

                            if (respond) {
                                manifold_count = pp_emit_capsule_contacts(manifolds, manifold_count,
                                                                          &cc, &rep);
                            }
                        }
                    } else if (lhs_shape->type == PP_OBJECT_TYPE_SPHERE && rhs_shape->type == PP_OBJECT_TYPE_SPHERE) {
                        float dist = pp_vec3_dist_sq(&lhs_spos, &rhs_spos);
                        float radius_sum = lhs_shape->sphere.radius + rhs_shape->sphere.radius;
                        float reach_sum = radius_sum + PP_CONTACT_MARGIN;
                        if (dist <= (reach_sum * reach_sum)) {
                            dist = pp_sqrtf(dist);
                            PPCollision c;
                            pp_fill_collision_info_sphere_sphere(lhs_body, lhs_shape, &lhs_spos,
                                                                 rhs_body, rhs_shape, &rhs_spos, dist, &c);

                            bool respond = true;
                            if (cb) {
                                respond = cb->collision_callback(lhs_body,
                                                                 rhs_body,
                                                                 lhs_shape->kind,
                                                                 rhs_shape->kind,
                                                                 &c,
                                                                 cb->user_data);
                            }

                            if (respond) {
                                manifolds[manifold_count++] = c;
                            }
                        }
                    } else if ((lhs_shape->type == PP_OBJECT_TYPE_SPHERE && rhs_shape->type == PP_OBJECT_TYPE_BOX) ||
                               (lhs_shape->type == PP_OBJECT_TYPE_BOX && rhs_shape->type == PP_OBJECT_TYPE_SPHERE)) {
                        const PPShape *sphere_shape, *box_shape;
                        const PPVec3 *sphere_pos, *box_pos;
                        PPBody *sphere_body, *box_body;

                        if (lhs_shape->type == PP_OBJECT_TYPE_SPHERE) {
                            sphere_shape = lhs_shape; sphere_pos = &lhs_spos; sphere_body = lhs_body;
                            box_shape = rhs_shape; box_pos = &rhs_spos; box_body = rhs_body;
                        } else {
                            sphere_shape = rhs_shape; sphere_pos = &rhs_spos; sphere_body = rhs_body;
                            box_shape = lhs_shape; box_pos = &lhs_spos; box_body = lhs_body;
                        }

                        PPVec3 contact, n;
                        float d;
                        if (pp_sphere_box_intersect(sphere_pos, sphere_shape->sphere.radius,
                                                    box_pos, &box_body->rot,
                                                    &box_shape->box.half_extents, box_shape->box.bounding_radius,
                                                    &contact, &n, &d)) {
                            PPCollision c;
                            pp_fill_collision_info_sphere_box(sphere_body, sphere_shape,
                                                              box_body, box_shape,
                                                              &contact, &n, &c, d);

                            bool respond = true;
                            if (cb) {
                                respond = cb->collision_callback(sphere_body,
                                                                 box_body,
                                                                 sphere_shape->kind,
                                                                 box_shape->kind,
                                                                 &c,
                                                                 cb->user_data);
                            }

                            if (respond) {
                                manifolds[manifold_count++] = c;
                            }
                        }
                    } else if (lhs_shape->type == PP_OBJECT_TYPE_BOX && rhs_shape->type == PP_OBJECT_TYPE_BOX) {
                        // Box vs Box via SAT, producing a multi-point manifold.
                        PPBoxContact bc;
                        // Reach far enough to catch anything the pair could
                        // close this step.
                        PPVec3 rel_v;
                        pp_vec3_sub(&rhs_body->vel, &lhs_body->vel, &rel_v);
                        float margin = PP_CONTACT_MARGIN + pp_vec3_length(&rel_v) * t;

                        if (pp_sat_box_box(&lhs_spos, &lhs_body->rot, &lhs_shape->box.half_extents,
                                           lhs_shape->box.bounding_radius,
                                           &rhs_spos, &rhs_body->rot, &rhs_shape->box.half_extents,
                                           rhs_shape->box.bounding_radius, margin, &bc)) {
                            // The callback decides per-pair; query it once using the
                            // deepest contact as the representative collision.
                            int deepest = 0;
                            for (int k = 1; k < bc.count; ++k) {
                                if (bc.depths[k] > bc.depths[deepest]) deepest = k;
                            }

                            bool respond = true;
                            PPCollision rep;
                            rep.p = bc.points[deepest];
                            rep.n = bc.normal;
                            rep.dist = bc.depths[deepest];
                            rep.separation = -bc.depths[deepest];
                            rep.obj1 = lhs_body;
                            rep.obj2 = rhs_body;
                            rep.obj1_bounce = lhs_body->bounce;
                            rep.obj2_bounce = rhs_body->bounce;
                            rep.obj1_friction = lhs_body->friction;
                            rep.obj2_friction = rhs_body->friction;
                            rep.type1 = PP_OBJECT_TYPE_BOX;
                            rep.type2 = PP_OBJECT_TYPE_BOX;
                            rep.kind1 = lhs_shape->kind;
                            rep.kind2 = rhs_shape->kind;

                            if (cb) {
                                respond = cb->collision_callback(lhs_body, rhs_body,
                                                                 lhs_shape->kind, rhs_shape->kind,
                                                                 &rep, cb->user_data);
                            }

                            if (respond) {
                                // Emit one manifold per contact point; they share
                                // the normal but carry their own contact point and
                                // penetration so the solver can resist tipping.
                                for (int k = 0; k < bc.count &&
                                                 manifold_count < PICOPHYSICS_MAX_MANIFOLDS; ++k) {
                                    PPCollision c = rep;
                                    c.p = bc.points[k];
                                    c.dist = bc.depths[k];
                                    c.separation = -bc.depths[k];
                                    manifolds[manifold_count++] = c;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // An awake body found touching a sleeper (one that was itself woken at
    // rest, say, so the pre-pass above did not count it as moving): the
    // sleeper has to take part.
    for (int i = 0; i < manifold_count; ++i) {
        if (manifolds[i].obj1->is_asleep) pp_body_wake(manifolds[i].obj1);
        if (manifolds[i].obj2->is_asleep) pp_body_wake(manifolds[i].obj2);
    }

    for (int i = 0; i < manifold_count; ++i) {
        pp_prepare_contact(&manifolds[i], t);
        pp_match_contact(&manifolds[i]);
    }

    for (int i = 0; i < object_count; ++i) {
        pp_vec3_init(&objects[i].solve_dpos);
        pp_vec3_init(&objects[i].solve_dtheta);
    }

    /* Soft step (after Box2D v3 / Box3D). The step is cut into substeps, each
     * of which integrates forces, makes one solving pass that also pushes out
     * overlap, moves the bodies, then makes a second "relax" pass that takes
     * back the velocity the push-out added. Contacts are not re-detected in
     * between; they track their own separation from how the bodies moved.
     *
     * For the same number of passes this converges far better than iterating
     * on one big step: a load only has to travel one contact per pass, but a
     * substep is short enough that being a few contacts behind costs almost
     * nothing. It is what lets a tall stack stand on a handful of passes. */
    int substeps = vel_iterations / 2;
    if (substeps < 1) substeps = 1;

    float h = t / (float) substeps;
    float inv_h = (h > 0.0f) ? 1.0f / h : 0.0f;

    float contact_hertz = fminf(30.0f, 0.25f * inv_h);
    PPSoftness soft = pp_make_soft(contact_hertz, 10.0f, h);
    PPSoftness static_soft = pp_make_soft(2.0f * contact_hertz, 10.0f, h);
    PPSoftness joint_soft = pp_make_soft(2.0f * contact_hertz, 2.0f, h);

    static PPConstraint *joints[PICOPHYSICS_MAX_CONSTRAINTS];
    int joint_count = 0;
    for (int i = 0; i < constraint_count; ++i) {
        if (pp_joint_active(&constraints[i])) {
            joints[joint_count++] = &constraints[i];
        }
    }

    // A hard landing on several contacts at once leaves a single sequential
    // sweep well short of converged: the first corner takes the whole blow,
    // the rest only partly undo the spin that causes, and friction then turns
    // what is left into sideways motion. Such steps are rare, so spend extra
    // normal sweeps on them and nothing on the usual resting step.
    int normal_sweeps = 1;
    for (int i = 0; i < manifold_count; ++i) {
        if (manifolds[i].impact) {
            normal_sweeps = 4;
            break;
        }
    }

    for (int sub = 0; sub < substeps; ++sub) {
        pp_integrate_forces(h, t, sub == substeps - 1);

        for (int i = 0; i < joint_count; ++i) {
            pp_joint_prepare(joints[i]);
        }
        for (int i = 0; i < manifold_count; ++i) {
            pp_warm_start_contact(&manifolds[i]);
        }

        // Joints before contacts: whatever is solved last is obeyed best, and
        // a door leaning slightly off its hinge beats one sunk into the floor.
        for (int i = 0; i < joint_count; ++i) {
            pp_joint_solve(joints[i], h, inv_h, true, &joint_soft);
        }

        for (int i = 0; i < manifold_count; ++i) {
            pp_solve_contact_friction(&manifolds[i]);
        }
        for (int j = 0; j < normal_sweeps; ++j) {
            for (int i = 0; i < manifold_count; ++i) {
                pp_solve_contact_normal(&manifolds[i], inv_h, true, &soft, &static_soft);
            }
        }

        for (int i = 0; i < constraint_count; ++i) {
            pp_solve_constraint_velocities(&constraints[i], h);
        }

        pp_integrate_velocities(h);

        for (int i = 0; i < joint_count; ++i) {
            pp_joint_solve(joints[i], h, inv_h, false, &joint_soft);
        }
        for (int i = 0; i < manifold_count; ++i) {
            pp_solve_contact_friction(&manifolds[i]);
        }
        for (int j = 0; j < normal_sweeps; ++j) {
            for (int i = 0; i < manifold_count; ++i) {
                pp_solve_contact_normal(&manifolds[i], inv_h, false, &soft, &static_soft);
            }
        }
    }

    // Several sweeps rather than one: contacts on the same body share its
    // rebound, and a single sequential sweep leaves whichever went first
    // short-changed, which shows up as spin on a symmetric impact. Skipped
    // entirely on the (usual) step where nothing is bouncing.
    bool bouncing = false;
    for (int i = 0; i < manifold_count && !bouncing; ++i) {
        bouncing = manifolds[i].v_bias != 0.0f && manifolds[i].max_jn != 0.0f;
    }

    for (int j = 0; bouncing && j < 8; ++j) {
        for (int i = 0; i < manifold_count; ++i) {
            pp_apply_restitution(&manifolds[i]);
        }
    }

    // Remember the solved impulses for the next step's warm start. Entries
    // belonging to sleeping bodies are carried over untouched, so a pile that
    // is woken picks up exactly where it left off.
    int kept = 0;
    for (int i = 0; i < contact_cache_count; ++i) {
        const PPContactCache *c = &contact_cache[i];
        if (!c->used && (c->obj1->is_asleep || c->obj2->is_asleep)) {
            contact_cache[kept++] = *c;
        }
    }

    contact_cache_count = kept;
    for (int i = 0; i < manifold_count && contact_cache_count < PICOPHYSICS_MAX_MANIFOLDS; ++i) {
        const PPCollision *m = &manifolds[i];
        PPContactCache *c = &contact_cache[contact_cache_count++];
        c->obj1 = m->obj1;
        c->obj2 = m->obj2;
        c->r1 = m->r1;
        c->n = m->n;
        // Only a sustained load is worth carrying over. The impulse that
        // stopped an impact (or bounced it) was a one-off; replayed next step
        // it would launch the body back the way it came.
        if (m->impact) {
            c->jn = c->jt1 = c->jt2 = 0.0f;
        } else {
            c->jn = m->jn;
            c->jt1 = m->jt1;
            c->jt2 = m->jt2;
        }
        c->used = false;
    }

    if (sleeping_enabled) {
        pp_update_sleeping(manifolds, manifold_count, t);
    }

    // Contacts are pushed out by the substeps above; only the constraints
    // still have a separate position pass.
    for (int j = 0; j < pos_iterations; ++j) {
        for (int i = 0; i < constraint_count; ++i) {
            pp_solve_constraint_positions(&constraints[i], t);
        }
    }
}

#endif

/*
------------------------------------------------------------------------------
This software is available under 2 licenses -- choose whichever you prefer.
------------------------------------------------------------------------------
ALTERNATIVE A - MIT License
Copyright (c) 2026 Luke Benstead
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
------------------------------------------------------------------------------
ALTERNATIVE B - Public Domain (www.unlicense.org)
This is free and unencumbered software released into the public domain.
Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.
In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
------------------------------------------------------------------------------
*/
