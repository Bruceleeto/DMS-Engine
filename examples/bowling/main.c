#include <kos.h>
#include <sh4zam/shz_sh4zam.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "dms/dc_engine.h"
#include "dms/dc_input.h"
#include "dms/dc_camera.h"
#include "dms/dc_model.h"
#include "dms/dc_draw.h"
#include "dms/dc_player.h"
#include "dms/dc_draw2d.h"
#include "dms/dc_debug.h"
#include "dms/collision.h"
#include "dms/dc_scene.h"

/* Where assets are read from: /pc/ over dcload; on a disc the engine finds them at /cd/ */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

#define PICOPHYSICS_USE_SH4ZAM
#define PICOPHYSICS_MAX_OBJECTS 64      /* 20 pins + the thrown balls */
#define PICOPHYSICS_IMPLEMENTATION
#include "dms/picophysics.h"

/* The alley (World), the pins (every object named Pin...) and where the
 * camera starts are scenes/main.json. The balls are thrown from code. */

/* ---- App state ---- */
static DCCamera camera;
static DCPlayer player;

/* Level */
static DCScene*  scene = NULL;
static ColWorld* col_world = NULL;

/* A prop is a model driven by a picophysics body. Models are not always built
 * around their origin, so the middle of the model's box is kept: the body sits
 * there, and the model is drawn offset back from it. */
typedef struct {
    DMSModel*  model;
    shz_vec3_t centre;      /* middle of the model's box, model space */
    shz_vec3_t half;        /* half size of the box */
    float      scale;       /* drawn and collided at this size */
} Prop;

static Prop ball;

/* The alley has two lanes running along z. The bowler stands at the near end
 * (z close to 0) and the pins stand in front of the pinsetter at the far end. */
#define LANE_Y       0.0f       /* lane surface */
#define BALL_RADIUS  0.29f      /* a real ball against a 0.98 tall pin */
#define BALL_SPEED   22.0f

/* A pin is a scene object moved by its body; it stands up straight where the
 * scene puts it (its turn in the scene is not read) */
#define MAX_PINS 48
static DCSceneObject* pin_obj[MAX_PINS];
static Prop    pin[MAX_PINS];
static PPBody* pin_body[MAX_PINS];
static PPVec3  pin_start[MAX_PINS];
static int     pin_count;

/* Every throw adds a ball; when the pool is full the oldest one goes */
#define MAX_THROWN 8
static PPBody* thrown[MAX_THROWN];
static int     thrown_count, thrown_next;

#define PHYS_DT (1.0f / 60.0f)

/* ========== PHYSICS ========== */

/* picophysics asks for the level's triangles near a shape; they come from the
 * collision grid. Its triangles are one sided and downloaded models wind
 * either way, so each one is turned to face the shape that asked. */
static int phys_tri_query(const PPVec3* centre, float reach, PPTriangle* out, int max, void* user) {
    (void)user;
    ColTri tris[COL_QUERY_MAX];
    shz_vec3_t c = shz_vec3_init(centre->x, centre->y, centre->z);
    int n = col_query(col_world, c, reach, tris, max);

    for (int i = 0; i < n; i++) {
        const ColTri* t = &tris[i];
        shz_vec3_t normal = shz_vec3_cross(t->edge1, t->edge2);
        int flip = shz_vec3_dot(normal, shz_vec3_sub(c, t->v0)) < 0.0f;
        shz_vec3_t p1 = shz_vec3_add(t->v0, flip ? t->edge2 : t->edge1);
        shz_vec3_t p2 = shz_vec3_add(t->v0, flip ? t->edge1 : t->edge2);
        PPVec3 a = {.xyz = {t->v0.x, t->v0.y, t->v0.z}};
        PPVec3 b = {.xyz = {p1.x, p1.y, p1.z}};
        PPVec3 d = {.xyz = {p2.x, p2.y, p2.z}};
        pp_triangle_init(&out[i], &a, &b, &d, 0);
    }
    return n;
}

/* Measure a prop's model's box */
static void prop_measure(Prop* p, DMSModel* model, const char* name) {
    memset(p, 0, sizeof(*p));
    p->model = model;
    if (!p->model) return;

    shz_vec3_t lo = shz_vec3_init( 1e9f,  1e9f,  1e9f);
    shz_vec3_t hi = shz_vec3_init(-1e9f, -1e9f, -1e9f);
    for (uint32_t m = 0; m < p->model->mesh_count; m++) {
        const DMSMesh* mesh = &p->model->meshes[m];
        for (uint32_t v = 0; v < mesh->vertex_count; v++) {
            const DMSVertex* vt = &mesh->vertices[v];
            if (vt->x < lo.x) lo.x = vt->x;
            if (vt->y < lo.y) lo.y = vt->y;
            if (vt->z < lo.z) lo.z = vt->z;
            if (vt->x > hi.x) hi.x = vt->x;
            if (vt->y > hi.y) hi.y = vt->y;
            if (vt->z > hi.z) hi.z = vt->z;
        }
    }
    p->centre = shz_vec3_scale(shz_vec3_add(lo, hi), 0.5f);
    p->half   = shz_vec3_scale(shz_vec3_sub(hi, lo), 0.5f);
    p->scale  = 1.0f;
    printf("%s: box %.2f x %.2f x %.2f, centre %.2f %.2f %.2f\n", name,
           p->half.x * 2.0f, p->half.y * 2.0f, p->half.z * 2.0f,
           p->centre.x, p->centre.y, p->centre.z);
}

/* Stand the pins up where the scene has them */
static void pins_reset(void) {
    for (int i = 0; i < pin_count; i++) {
        if (!pin_body[i]) continue;
        pp_body_set_position(pin_body[i], pin_start[i].x, pin_start[i].y, pin_start[i].z);
        pp_body_set_rotation(pin_body[i], 0.0f, 0.0f, 0.0f, 1.0f);
        pp_body_set_velocity(pin_body[i], 0.0f, 0.0f, 0.0f);
        pp_body_set_angular_velocity(pin_body[i], 0.0f, 0.0f, 0.0f);
    }
}

/* Pins are boxes: a box stands on its own, a capsule would balance on a point */
static void pins_build(void) {
    for (int i = 0; i < scene->object_count && pin_count < MAX_PINS; i++) {
        DCSceneObject* o = &scene->objects[i];
        if (strncmp(o->name, "Pin", 3) || !o->m) continue;
        Prop* p = &pin[pin_count];
        if (pin_count && o->m == pin[pin_count - 1].model) *p = pin[pin_count - 1];
        else prop_measure(p, o->m, o->model);
        float k = p->scale = o->scale.x;
        /* the body is at the middle of the box */
        PPVec3 at = {.xyz = {o->pos.x + p->centre.x * k, o->pos.y + p->centre.y * k, o->pos.z + p->centre.z * k}};
        o->rot = shz_vec3_init(0.0f, 0.0f, 0.0f);
        pin_obj[pin_count]   = o;
        pin_start[pin_count] = at;
        pin_body[pin_count]  = pp_physics_create_box(p->half.x * 2.0f * k, p->half.y * 2.0f * k,
                                                     p->half.z * 2.0f * k, &at, 1.0f, 0);
        pin_count++;
    }
}

/* Roll a new ball from the camera, the way it faces, starting on the floor */
static void ball_throw(void) {
    if (!ball.model) return;
    if (thrown_count == MAX_THROWN) pp_physics_destroy_body(thrown[thrown_next]);
    else thrown_count++;

    shz_sincos_t sc = shz_sincosf(camera.yaw);
    float x = camera.pos.x + sc.sin * 2.0f, z = camera.pos.z + sc.cos * 2.0f;
    float ground = LANE_Y;
    ColGroundHit hit = col_ground(col_world, shz_vec3_init(x, camera.pos.y, z), 100.0f);
    if (hit.hit) ground = hit.y;

    PPVec3 at = {.xyz = {x, ground + BALL_RADIUS + 0.02f, z}};
    PPBody* b = pp_physics_create_sphere(BALL_RADIUS, &at, 5.0f, 0);
    thrown[thrown_next] = b;
    thrown_next = (thrown_next + 1) % MAX_THROWN;
    if (!b) return;
    pp_body_set_velocity(b, sc.sin * BALL_SPEED, 0.0f, sc.cos * BALL_SPEED);
    /* Already rolling, so it does not skid first */
    pp_body_set_angular_velocity(b, sc.cos * BALL_SPEED / BALL_RADIUS, 0.0f,
                                 -sc.sin * BALL_SPEED / BALL_RADIUS);
}

/* Remove every ball */
static void thrown_clear(void) {
    for (int i = 0; i < thrown_count; i++)
        if (thrown[i]) pp_physics_destroy_body(thrown[i]);
    thrown_count = thrown_next = 0;
}

/* Where a prop's model goes for a body to be where it is: its origin and its
 * axes (rot) */
static shz_vec3_t prop_place(const Prop* p, PPBody* body, float rot[9]) {
    PPVec3 pos, x, y, z;
    pp_body_get_position(body, &pos);
    pp_body_get_right(body, &x);
    pp_body_get_up(body, &y);
    pp_body_get_forward(body, &z);
    rot[0] = x.x; rot[1] = x.y; rot[2] = x.z;
    rot[3] = y.x; rot[4] = y.y; rot[5] = y.z;
    rot[6] = z.x; rot[7] = z.y; rot[8] = z.z;

    /* The body is at the middle of the box; step back to the model's origin */
    shz_vec3_t c = shz_vec3_scale(p->centre, p->scale);
    return shz_vec3_init(pos.x - (rot[0] * c.x + rot[3] * c.y + rot[6] * c.z),
                         pos.y - (rot[1] * c.x + rot[4] * c.y + rot[7] * c.z),
                         pos.z - (rot[2] * c.x + rot[5] * c.y + rot[8] * c.z));
}

/* Pins: their scene objects follow their bodies (dc_scene_draw draws them) */
static void pins_move(void) {
    for (int i = 0; i < pin_count; i++) {
        if (!pin_body[i]) continue;
        float rot[9];
        pin_obj[i]->pos = prop_place(&pin[i], pin_body[i], rot);
        pin_obj[i]->rot = dc_scene_rot_degrees(rot);
    }
}

/* Balls are not in the scene: drawn here */
static void balls_draw(void) {
    if (!ball.model) return;
    for (int i = 0; i < thrown_count; i++) {
        if (!thrown[i]) continue;
        float rot[9];
        shz_vec3_t origin = prop_place(&ball, thrown[i], rot);
        dc_draw_ex(ball.model, &(DCDrawOpts){ .pos = origin, .scale = ball.scale, .rot = rot });
    }
}

/* ========== MAIN ========== */

static bool check_exit(void) {
    const DCInput* inp = dc_input_get(0);
    if (!inp) return false;
    return (inp->buttons & CONT_START) != 0;
}

int main(int argc, char* argv[]) {
    /* Vertex buffer is double-buffered by KOS, so this costs 2x in VRAM */
    dc_init((DCInitParams){ .vram_size = 2300 * 1024 });
    dc_draw2d_init();
    dc_debug_init();

    dc_player_init(&player);
    player.cam_mode = DC_CAM_NOCLIP;
    player.move_speed = 0.25f;  /* per frame; the alley is small */

    scene = dc_scene_play(ASSETS, "scenes/main.json");
    if (!scene) {
        dc_shutdown();
        return 1;
    }
    dc_scene_apply(scene);
    dc_scene_camera(scene, &camera);

    DCSceneObject* world = dc_scene_find(scene, "World");
    if (world && world->m)
        col_world = col_build(world->m, world->pos, world->scale.x);

    PPVec3 gravity = {.xyz = {0.0f, -9.8f, 0.0f}};
    pp_physics_set_gravity(&gravity);
    pp_physics_set_triangle_query(phys_tri_query, NULL);

    pins_build();
    prop_measure(&ball, dc_model_load(ASSETS "ball/ball.dms"), "ball/ball.dms");
    if (ball.model) ball.scale = BALL_RADIUS / ball.half.y;    /* the model is not ball sized */

    printf("VRAM free after loading: %luKB\n", (unsigned long)(pvr_mem_available() / 1024));

    while (!check_exit()) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        /* ---- Input ---- */
        /* Reset (Y button): camera back to the start, pins up, balls gone */
        if (inp && dc_input_pressed(inp, CONT_Y)) {
            dc_scene_camera(scene, &camera);
            pins_reset();
            thrown_clear();
        }

        /* Roll a ball (A) from the camera */
        if (inp && dc_input_pressed(inp, CONT_A)) ball_throw();

        dc_player_update(&player, &camera, inp, NULL, dt);
        dc_camera_update(&camera);

        pp_physics_step(PHYS_DT, 8, 3);
        pins_move();

        /* ---- Rendering ---- */
        dc_set_camera(&camera);
        dc_scene_draw(scene);
        balls_draw();
        dc_debug_stats();
        dc_frame_end();
    }

    pp_physics_clear();
    col_free(col_world);
    dc_scene_free(scene);
    dc_model_free(ball.model);
    dc_shutdown();

    return 0;
}
