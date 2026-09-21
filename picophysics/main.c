#include <kos.h>
#include <dc/perfctr.h>
#include <sh4zam/shz_sh4zam.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "dms/dc_engine.h"
#include "dms/dc_input.h"
#include "dms/dc_camera.h"
#include "dms/dc_model.h"
#include "dms/dc_player.h"
#include "dms/dc_draw2d.h"
#include "dms/dc_debug.h"
#include "dms/collision.h"

/* Where assets are read from: /pc/ over dcload, /cd/ when built with make disc */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

#define PICOPHYSICS_USE_SH4ZAM
#define PICOPHYSICS_MAX_OBJECTS 64      /* 2 dropped + 24 thrown + 15 in the chain */
#define PICOPHYSICS_IMPLEMENTATION
#include "dms/picophysics.h"

/* ---- App state ---- */
static DCCamera camera;
static DCPlayer player;


/* Level */
static DMSModel* world_model = NULL;
static ColWorld* col_world = NULL;

/* A prop is a model driven by a picophysics body. Models are not always built
 * around their origin, so the middle of the model's box is kept: the body sits
 * there, and the model is drawn offset back from it. */
typedef struct {
    DMSModel*  model;
    PPBody*    body;
    shz_vec3_t centre;      /* middle of the model's box, model space */
    shz_vec3_t half;        /* half size of the box */
    shz_vec3_t start;       /* where the body is dropped from */
    float      radius;      /* above 0: the body is a sphere, otherwise a box */
} Prop;

#define DROP_HEIGHT 10.0f   /* above the ground */
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
#define CHAIN_TOP     15.0f                 /* pivot height above the ground */
#define CHAIN_SWING   0.7f                  /* starting angle, radians */
static Prop         link_prop, bob_prop;
static PPBody*      chain_body[CHAIN_BODIES];
static PPVec3       chain_pos[CHAIN_BODIES];
static PPQuaternion chain_rot[CHAIN_BODIES];

#define PHYS_DT (1.0f / 60.0f)

/* ========== PROFILING ========== */

typedef struct {
    uint64_t anim_ns;
    uint64_t camera_ns;
    uint64_t render_ns;
    uint64_t frame_total_ns;

    uint32_t world_meshes_drawn;
    uint32_t world_meshes_culled;
    uint32_t world_verts_xformed;
    uint32_t world_verts_clipped;

    uint32_t samples;
} FrameProfile;

static FrameProfile prof = {0};

#define PROF_INTERVAL 60

/* On-screen profile text (updated every PROF_INTERVAL frames) */
static char prof_lines[6][48];

static void prof_print_and_reset(void) {
    if (prof.samples == 0) return;
    float n = (float)prof.samples;

    float anim_us   = (float)prof.anim_ns / n / 1000.0f;
    float cam_us    = (float)prof.camera_ns / n / 1000.0f;
    float render_us = (float)prof.render_ns / n / 1000.0f;
    float total_ms  = (float)prof.frame_total_ns / n / 1000000.0f;
    uint32_t drawn  = prof.world_meshes_drawn / prof.samples;
    uint32_t culled = prof.world_meshes_culled / prof.samples;
    uint32_t xform  = prof.world_verts_xformed / prof.samples;
    uint32_t clip   = prof.world_verts_clipped / prof.samples;

    snprintf(prof_lines[0], sizeof(prof_lines[0]), "FRAME: %.2f ms", total_ms);
    snprintf(prof_lines[1], sizeof(prof_lines[1]), "ANIM: %.0f us", anim_us);
    snprintf(prof_lines[2], sizeof(prof_lines[2]), "CAM:  %.0f us", cam_us);
    snprintf(prof_lines[3], sizeof(prof_lines[3]), "DRAW: %.0f us", render_us);
    snprintf(prof_lines[4], sizeof(prof_lines[4]), "drawn %lu culled %lu", (unsigned long)drawn, (unsigned long)culled);
    snprintf(prof_lines[5], sizeof(prof_lines[5]), "xform %lu clip %lu", (unsigned long)xform, (unsigned long)clip);

    memset(&prof, 0, sizeof(prof));
}

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

/* Load a prop's model and measure its box */
static void prop_load(Prop* p, const char* path) {
    memset(p, 0, sizeof(*p));
    p->model = dc_model_load(path);
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

/* Drop point: `height` above the ground at (x, z) */
static shz_vec3_t drop_point(float x, float z, float height) {
    float ground = 0.0f;
    ColGroundHit hit = col_ground(col_world, shz_vec3_init(x, 1000.0f, z), 2000.0f);
    if (hit.hit) ground = hit.y;
    printf("Drop at %.1f %.1f: ground %.2f\n", x, z, ground);
    return shz_vec3_init(x, ground + height, z);
}

/* Put a prop back at its start, at rest */
static void prop_reset(Prop* p) {
    if (!p->body) return;
    pp_body_set_position(p->body, p->start.x, p->start.y, p->start.z);
    pp_body_set_rotation(p->body, 0.0f, 0.0f, 0.0f, 1.0f);
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

/* Draw a prop's model where a body is */
static void prop_draw(const Prop* p, PPBody* body, int list) {
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
    dc_model_draw_list_oriented(p->model, origin, 1.0f, rot, &camera, list);
}

/* All props of one list: the two dropped at the start, the thrown ones, the chain */
static void props_draw(int list) {
    prop_draw(&ball, ball.body, list);
    prop_draw(&crate, crate.body, list);
    for (int i = 0; i < thrown_count; i++)
        prop_draw(thrown[i].kind, thrown[i].body, list);
    for (int i = 0; i < CHAIN_BODIES; i++)
        prop_draw(i < CHAIN_LINKS ? &link_prop : &bob_prop, chain_body[i], list);
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
    dc_debug_init();

    dc_camera_init(&camera);
    dc_player_init(&player);
    player.cam_mode = DC_CAM_NOCLIP;
    player.move_speed = 1.0f;   /* per frame */

    world_model = dc_model_load(ASSETS "world/test.dms");
    if (world_model)
        col_world = col_build(world_model, shz_vec3_init(0.0f, 0.0f, 0.0f), 1.0f);

    PPVec3 gravity = {.xyz = {0.0f, -9.8f, 0.0f}};
    pp_physics_set_gravity(&gravity);
    pp_physics_set_triangle_query(phys_tri_query, NULL);

    /* The crate drops first; the ball starts higher and over the crate's
     * edge, so it lands on the crate and rolls off */
    prop_load(&crate, ASSETS "crate/crate.dms");
    prop_load(&ball, ASSETS "ball/ball.dms");
    crate.start = drop_point(-18.0f, -15.0f, DROP_HEIGHT + crate.half.y);
    ball.start  = shz_vec3_init(crate.start.x + crate.half.x * 0.8f,
                                crate.start.y + crate.half.y + DROP_HEIGHT,
                                crate.start.z);
    ball.radius = ball.half.x > ball.half.y ? ball.half.x : ball.half.y;
    if (ball.half.z > ball.radius) ball.radius = ball.half.z;
    if (crate.model) crate.body = prop_body(&crate, crate.start);
    if (ball.model)  ball.body  = prop_body(&ball, ball.start);

    /* Wrecking ball to the side of the drop, swinging across in front of it */
    prop_load(&link_prop, ASSETS "link/link.dms");
    prop_load(&bob_prop, ASSETS "bob/bob.dms");
    bob_prop.radius = bob_prop.half.y;
    shz_vec3_t under = drop_point(crate.start.x + 8.0f, crate.start.z - 6.0f, 0.0f);
    chain_build(shz_vec3_init(under.x, under.y + CHAIN_TOP, under.z));

    /* Start back from the props, looking at them */
    shz_vec3_t view_start = shz_vec3_init(crate.start.x, crate.start.y - DROP_HEIGHT * 0.5f,
                                          crate.start.z - 25.0f);
    camera.pos = view_start;

    printf("VRAM free after loading: %luKB\n", (unsigned long)(pvr_mem_available() / 1024));

    char fps_str[32];
    snprintf(fps_str, sizeof(fps_str), "FPS: --");

    while (!check_exit()) {
        uint64_t frame_start = perf_cntr_timer_ns();

        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        uint64_t t0;

        /* ---- Input ---- */
        t0 = perf_cntr_timer_ns();

        /* Reset (Y button): camera and both props back to the start, thrown ones gone */
        if (inp && dc_input_pressed(inp, CONT_Y)) {
            camera.pos = view_start;
            camera.yaw = 0.0f;
            camera.pitch = 0.0f;
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
        prof.camera_ns += perf_cntr_timer_ns() - t0;

        pp_physics_step(PHYS_DT, 8, 3);

        /* ---- FPS string ---- */
        snprintf(fps_str, sizeof(fps_str), "FPS: %.1f", dc_fps());

        /* ---- Rendering (by PVR list: OP -> TR -> PT) ---- */
        t0 = perf_cntr_timer_ns();
        dc_model_reset_stats();

        /* Pass 1: Opaque */
        dc_list_begin(PVR_LIST_OP_POLY);
        dc_model_draw_list(world_model, shz_vec3_init(0.0f, 0.0f, 0.0f), 1.0f, &camera, PVR_LIST_OP_POLY);
        props_draw(PVR_LIST_OP_POLY);

        /* Pass 2: Transparent */
        dc_list_begin(PVR_LIST_TR_POLY);
        dc_model_draw_list(world_model, shz_vec3_init(0.0f, 0.0f, 0.0f), 1.0f, &camera, PVR_LIST_TR_POLY);
        props_draw(PVR_LIST_TR_POLY);

        /* Pass 3: Punch-through + HUD */
        dc_list_begin(PVR_LIST_PT_POLY);
        dc_model_draw_list(world_model, shz_vec3_init(0.0f, 0.0f, 0.0f), 1.0f, &camera, PVR_LIST_PT_POLY);
        props_draw(PVR_LIST_PT_POLY);

        dc_draw_text(fps_str, 10, 10, 16, DC_COLOR_GREEN);

        /* On-screen profile */
        if (prof_lines[0][0]) {
            int py = 480 - 16 * 6 - 4;
            for (int i = 0; i < 6; i++)
                dc_draw_text(prof_lines[i], 10, py + i * 16, 16, DC_COLOR_GREEN);
        }

        const DCModelStats* ms = dc_model_get_stats();
        prof.world_meshes_drawn  += ms->meshes_drawn;
        prof.world_meshes_culled += ms->meshes_culled;
        prof.world_verts_xformed += ms->verts_xformed;
        prof.world_verts_clipped += ms->verts_clipped;

        prof.render_ns += perf_cntr_timer_ns() - t0;

        dc_frame_end();

        prof.frame_total_ns += perf_cntr_timer_ns() - frame_start;
        prof.samples++;

        if (prof.samples >= PROF_INTERVAL)
            prof_print_and_reset();
    }

    pp_physics_clear();
    col_free(col_world);
    dc_model_free(world_model);
    dc_model_free(ball.model);
    dc_model_free(crate.model);
    dc_model_free(link_prop.model);
    dc_model_free(bob_prop.model);
    dc_shutdown();

    return 0;
}
