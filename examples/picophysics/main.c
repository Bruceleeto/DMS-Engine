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
#include "dms/collision.h"
#include "dms/dc_scene.h"

/* Where assets are read from: /pc/ over dcload; on a disc the engine finds them at /cd/ */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

#define PICOPHYSICS_USE_SH4ZAM
#define PICOPHYSICS_MAX_OBJECTS 64      /* 2 dropped + 24 thrown + 15 in the chain */
#define PICOPHYSICS_IMPLEMENTATION
#include "dms/picophysics.h"

/* The level (World), the crate and ball dropped at the start (Crate, Ball:
 * physics then moves them), the wrecking ball's pivot (marker Chain) and the
 * start view are scenes/main.json. Thrown props and the chain stay in code. */

/* ---- App state ---- */
static DCCamera camera;
static DCPlayer player;
static DCScene* scene = NULL;
static ColWorld* col_world = NULL;

/* A prop is a model driven by a picophysics body. Models are not always built
 * around their origin, so the middle of the model's box is kept: the body sits
 * there, and the model is drawn offset back from it. */
typedef struct {
    DMSModel*  model;
    PPBody*    body;
    DCSceneObject* obj;     /* the scene's, which the body moves; NULL: drawn here */
    shz_vec3_t centre;      /* middle of the model's box, model space */
    shz_vec3_t half;        /* half size of the box */
    shz_vec3_t start;       /* where the body is dropped from */
    PPQuaternion start_rot;
    float      radius;      /* above 0: the body is a sphere, otherwise a box */
} Prop;

static Prop ball, crate;

/* Every throw adds a body; when the pool is full the oldest one goes */
#define MAX_THROWN 24
typedef struct {
    Prop*   kind;
    PPBody* body;
} Thrown;
static Thrown thrown[MAX_THROWN];
static int    thrown_count, thrown_next;

/* Wrecking ball: links on ball joints, top one pinned to the world, bob on
 * the end. The link model's long axis is z, so each link is stood on end,
 * and every other one turned a quarter turn like a real chain. */
#define CHAIN_LINKS   14
#define CHAIN_BODIES  (CHAIN_LINKS + 1)     /* links, then the bob */
#define CHAIN_PITCH   0.8364f               /* centre to centre */
#define CHAIN_BOB_GAP 1.05f                 /* last link centre to bob centre */
#define CHAIN_SWING   0.7f                  /* starting angle, radians */
static Prop         link_prop, bob_prop;
static PPBody*      chain_body[CHAIN_BODIES];
static PPVec3       chain_pos[CHAIN_BODIES];
static PPQuaternion chain_rot[CHAIN_BODIES];

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
static void prop_measure(Prop* p, const char* path) {
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
    printf("%s: box %.2f x %.2f x %.2f, centre %.2f %.2f %.2f\n", path,
           p->half.x * 2.0f, p->half.y * 2.0f, p->half.z * 2.0f,
           p->centre.x, p->centre.y, p->centre.z);
}

/* Load a prop's model and measure its box */
static void prop_load(Prop* p, const char* path) {
    memset(p, 0, sizeof(*p));
    p->model = dc_model_load(path);
    prop_measure(p, path);
}

/* A prop placed in the scene: its model, and its body's start where the scene
 * puts the middle of its box, turned as it is there */
static void prop_from_scene(Prop* p, const char* name) {
    memset(p, 0, sizeof(*p));
    p->obj = dc_scene_find(scene, name);
    if (!p->obj || !p->obj->m) { printf("%s: not in the scene\n", name); p->obj = NULL; return; }
    p->model = p->obj->m;
    prop_measure(p, name);

    float r[9];
    dc_scene_object_rot(p->obj, r);
    shz_vec3_t c = p->centre;
    p->start = shz_vec3_init(p->obj->pos.x + r[0] * c.x + r[3] * c.y + r[6] * c.z,
                             p->obj->pos.y + r[1] * c.x + r[4] * c.y + r[7] * c.z,
                             p->obj->pos.z + r[2] * c.x + r[5] * c.y + r[8] * c.z);
    /* the turn's columns as a quaternion */
    PPQuaternion* q = &p->start_rot;
    float t = r[0] + r[4] + r[8];
    if (t > 0.0f) {
        float k = 0.5f / sqrtf(t + 1.0f);
        q->w = 0.25f / k; q->x = (r[5] - r[7]) * k; q->y = (r[6] - r[2]) * k; q->z = (r[1] - r[3]) * k;
    } else if (r[0] > r[4] && r[0] > r[8]) {
        float k = 2.0f * sqrtf(1.0f + r[0] - r[4] - r[8]);
        q->w = (r[5] - r[7]) / k; q->x = 0.25f * k; q->y = (r[3] + r[1]) / k; q->z = (r[6] + r[2]) / k;
    } else if (r[4] > r[8]) {
        float k = 2.0f * sqrtf(1.0f + r[4] - r[0] - r[8]);
        q->w = (r[6] - r[2]) / k; q->x = (r[3] + r[1]) / k; q->y = 0.25f * k; q->z = (r[7] + r[5]) / k;
    } else {
        float k = 2.0f * sqrtf(1.0f + r[8] - r[0] - r[4]);
        q->w = (r[1] - r[3]) / k; q->x = (r[6] + r[2]) / k; q->y = (r[7] + r[5]) / k; q->z = 0.25f * k;
    }
}

/* Put a prop back at its start, at rest */
static void prop_reset(Prop* p) {
    if (!p->body) return;
    pp_body_set_position(p->body, p->start.x, p->start.y, p->start.z);
    pp_body_set_rotation(p->body, p->start_rot.x, p->start_rot.y, p->start_rot.z, p->start_rot.w);
    pp_body_set_velocity(p->body, 0.0f, 0.0f, 0.0f);
    pp_body_set_angular_velocity(p->body, 0.0f, 0.0f, 0.0f);
}

/* A new body for a prop: sphere or box, from the model's measured size */
static PPBody* prop_body(const Prop* p, shz_vec3_t at) {
    PPVec3 pos = {.xyz = {at.x, at.y, at.z}};
    if (p->radius > 0.0f)
        return pp_physics_create_sphere(p->radius, &pos, 1.0f, 0);
    return pp_physics_create_box(p->half.x * 2.0f, p->half.y * 2.0f, p->half.z * 2.0f,
                                 &pos, 10.0f, 0);
}

/* Throw a new one from the camera, the way it faces, with some tumble */
static void prop_throw(Prop* p, float spin) {
    if (!p->model) return;
    Thrown* t = &thrown[thrown_next];
    if (thrown_count == MAX_THROWN) pp_physics_destroy_body(t->body);
    else thrown_count++;
    thrown_next = (thrown_next + 1) % MAX_THROWN;

    shz_sincos_t sc = shz_sincosf(camera.yaw);
    float clear = 3.0f + p->half.z + p->half.x;     /* start clear of the camera */
    t->kind = p;
    t->body = prop_body(p, shz_vec3_init(camera.pos.x + sc.sin * clear, camera.pos.y,
                                         camera.pos.z + sc.cos * clear));
    pp_body_set_velocity(t->body, sc.sin * 20.0f, 4.0f, sc.cos * 20.0f);
    pp_body_set_angular_velocity(t->body, spin, spin * 0.5f, 0.0f);
}

/* Remove everything that was thrown */
static void thrown_clear(void) {
    for (int i = 0; i < thrown_count; i++)
        pp_physics_destroy_body(thrown[i].body);
    thrown_count = thrown_next = 0;
}

/* Draw a prop's model where a body is: the scene's object moved there, or
 * drawn here */
static void prop_draw(const Prop* p, PPBody* body) {
    if (!p->model || !body) return;
    PPVec3 pos, x, y, z;
    pp_body_get_position(body, &pos);
    pp_body_get_right(body, &x);
    pp_body_get_up(body, &y);
    pp_body_get_forward(body, &z);
    float rot[9] = { x.x, x.y, x.z,  y.x, y.y, y.z,  z.x, z.y, z.z };

    /* The body is at the middle of the box; step back to the model's origin */
    shz_vec3_t c = p->centre;
    shz_vec3_t origin = shz_vec3_init(pos.x - (rot[0] * c.x + rot[3] * c.y + rot[6] * c.z),
                                      pos.y - (rot[1] * c.x + rot[4] * c.y + rot[7] * c.z),
                                      pos.z - (rot[2] * c.x + rot[5] * c.y + rot[8] * c.z));
    if (p->obj && body == p->body) {
        p->obj->pos = origin;
        p->obj->rot = dc_scene_rot_degrees(rot);
    } else {
        dc_draw_ex(p->model, &(DCDrawOpts){ .pos = origin, .rot = rot });
    }
}

/* All props: the two dropped at the start (the scene draws them), the
 * thrown ones, the chain */
static void props_draw(void) {
    prop_draw(&ball, ball.body);
    prop_draw(&crate, crate.body);
    for (int i = 0; i < thrown_count; i++)
        prop_draw(thrown[i].kind, thrown[i].body);
    for (int i = 0; i < CHAIN_BODIES; i++)
        prop_draw(i < CHAIN_LINKS ? &link_prop : &bob_prop, chain_body[i]);
}

/* Put the chain back where it was built, at rest */
static void chain_reset(void) {
    for (int i = 0; i < CHAIN_BODIES; i++) {
        if (!chain_body[i]) continue;
        pp_body_set_position(chain_body[i], chain_pos[i].x, chain_pos[i].y, chain_pos[i].z);
        pp_body_set_rotation(chain_body[i], chain_rot[i].x, chain_rot[i].y, chain_rot[i].z,
                             chain_rot[i].w);
        pp_body_set_velocity(chain_body[i], 0.0f, 0.0f, 0.0f);
        pp_body_set_angular_velocity(chain_body[i], 0.0f, 0.0f, 0.0f);
    }
}

/* Build the chain hanging from `pivot`, held out at CHAIN_SWING so it starts
 * swinging straight away */
static void chain_build(shz_vec3_t pivot) {
    if (!link_prop.model || !bob_prop.model) return;

    shz_sincos_t sw = shz_sincosf(CHAIN_SWING);
    PPVec3 along = {.xyz = {sw.sin, -sw.cos, 0.0f}};    /* pivot towards the bob */

    /* Stand the link on end (z to y), quarter turn for odd links, then lean
     * the whole thing over by the swing angle */
    PPVec3 ax = {.xyz = {1.0f, 0.0f, 0.0f}}, ay = {.xyz = {0.0f, 1.0f, 0.0f}};
    PPVec3 az = {.xyz = {0.0f, 0.0f, 1.0f}};
    PPQuaternion stand, quarter, lean;
    pp_quat_from_axis_angle(&stand, &ax, 1.5707963f);
    pp_quat_from_axis_angle(&quarter, &ay, 1.5707963f);
    pp_quat_from_axis_angle(&lean, &az, CHAIN_SWING);

    float half_link = CHAIN_PITCH * 0.5f;
    for (int i = 0; i < CHAIN_BODIES; i++) {
        int   is_bob = i == CHAIN_LINKS;
        float d = is_bob ? half_link + (CHAIN_LINKS - 1) * CHAIN_PITCH + CHAIN_BOB_GAP
                         : half_link + i * CHAIN_PITCH;
        PPVec3 at = {.xyz = {pivot.x + along.x * d, pivot.y + along.y * d, pivot.z}};

        PPQuaternion q = stand;
        if (!is_bob && (i & 1)) pp_quat_multiply(&quarter, &q, &q);
        pp_quat_multiply(&lean, &q, &q);

        /* Links are spheres for collision: they slide over crates and the
         * level without catching. Masses stay close together, a heavy bob
         * on light links stretches the chain */
        PPBody* b = is_bob ? pp_physics_create_sphere(bob_prop.radius, &at, 6.0f, 0)
                           : pp_physics_create_sphere(0.35f, &at, 1.0f, 0);
        if (!b) return;
        pp_body_set_rotation(b, q.x, q.y, q.z, q.w);
        pp_body_set_angular_damping(b, 0.3f);
        pp_body_set_never_sleeps(b, true);
        chain_body[i] = b;
        chain_pos[i]  = at;
        chain_rot[i]  = q;

        /* Joint to the one above (or the world), halfway between the two */
        float jd = i == 0 ? 0.0f : is_bob ? d - CHAIN_BOB_GAP * 0.5f : d - half_link;
        PPVec3 anchor = {.xyz = {pivot.x + along.x * jd, pivot.y + along.y * jd, pivot.z}};
        pp_physics_create_ball_joint(i == 0 ? NULL : chain_body[i - 1], b, &anchor);
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

    dc_player_init(&player);
    player.cam_mode = DC_CAM_NOCLIP;
    player.move_speed = 1.0f;   /* per frame */

    scene = dc_scene_play(ASSETS, "scenes/main.json");
    if (!scene) return 1;
    dc_scene_apply(scene);
    dc_scene_camera(scene, &camera);    /* back from the props, looking at them */
    DCCamera view_start = camera;

    DCSceneObject* world = dc_scene_find(scene, "World");
    if (world && world->m)
        col_world = col_build(world->m, world->pos, world->scale.x);

    PPVec3 gravity = {.xyz = {0.0f, -9.8f, 0.0f}};
    pp_physics_set_gravity(&gravity);
    pp_physics_set_triangle_query(phys_tri_query, NULL);

    /* The crate drops first; the ball starts higher and over the crate's
     * edge, so it lands on the crate and rolls off */
    prop_from_scene(&crate, "Crate");
    prop_from_scene(&ball, "Ball");
    ball.radius = ball.half.x > ball.half.y ? ball.half.x : ball.half.y;
    if (ball.half.z > ball.radius) ball.radius = ball.half.z;
    if (crate.model) { crate.body = prop_body(&crate, crate.start); prop_reset(&crate); }
    if (ball.model)  { ball.body  = prop_body(&ball, ball.start);   prop_reset(&ball); }

    /* Wrecking ball to the side of the drop, swinging across in front of it */
    prop_load(&link_prop, ASSETS "link/link.dms");
    prop_load(&bob_prop, ASSETS "bob/bob.dms");
    bob_prop.radius = bob_prop.half.y;
    DCSceneObject* pivot = dc_scene_find(scene, "Chain");
    if (pivot) chain_build(pivot->pos);

    printf("VRAM free after loading: %luKB\n", (unsigned long)(pvr_mem_available() / 1024));

    while (!check_exit()) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        /* Reset (Y button): camera and both props back to the start, thrown ones gone */
        if (inp && dc_input_pressed(inp, CONT_Y)) {
            camera.pos = view_start.pos;
            camera.yaw = view_start.yaw;
            camera.pitch = view_start.pitch;
            prop_reset(&crate);
            prop_reset(&ball);
            thrown_clear();
            chain_reset();
        }

        /* Throw a new ball (A) or crate (X) from the camera */
        if (inp && dc_input_pressed(inp, CONT_A)) prop_throw(&ball, 0.0f);
        if (inp && dc_input_pressed(inp, CONT_X)) prop_throw(&crate, 3.0f);

        dc_player_update(&player, &camera, inp, NULL, dt);
        dc_camera_update(&camera);

        pp_physics_step(PHYS_DT, 8, 3);

        dc_set_camera(&camera);
        props_draw();
        dc_scene_draw(scene);
        dc_frame_end();
    }

    pp_physics_clear();
    col_free(col_world);
    dc_scene_free(scene);
    dc_model_free(link_prop.model);
    dc_model_free(bob_prop.model);
    dc_shutdown();

    return 0;
}
