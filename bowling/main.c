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
#define PICOPHYSICS_MAX_OBJECTS 64      /* 20 pins + the thrown balls */
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
    shz_vec3_t centre;      /* middle of the model's box, model space */
    shz_vec3_t half;        /* half size of the box */
    float      scale;       /* drawn and collided at this size */
} Prop;

static Prop ball, pin;

/* The alley has two lanes running along z. The bowler stands at the near end
 * (z close to 0) and the pins stand in front of the pinsetter at the far end. */
#define LANES        2
static const float lane_x[LANES] = { -2.615f, 2.685f };    /* lane centres */
#define LANE_Y       0.0f       /* lane surface */
#define PIN_BACK_Z   -60.5f     /* back row of pins */
#define PIN_SPACING  0.79f      /* between neighbours: 12 inches at this pin's size */
#define BALL_RADIUS  0.29f      /* a real ball against a 0.98 tall pin */
#define BALL_SPEED   22.0f
#define START_Z      -5.0f      /* where the camera starts */

#define PINS_PER_LANE 10
#define PIN_COUNT     (LANES * PINS_PER_LANE)
static PPBody* pin_body[PIN_COUNT];
static PPVec3  pin_start[PIN_COUNT];

/* Every throw adds a ball; when the pool is full the oldest one goes */
#define MAX_THROWN 8
static PPBody* thrown[MAX_THROWN];
static int     thrown_count, thrown_next;

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
    p->scale  = 1.0f;
    printf("%s: box %.2f x %.2f x %.2f, centre %.2f %.2f %.2f\n", path,
           p->half.x * 2.0f, p->half.y * 2.0f, p->half.z * 2.0f,
           p->centre.x, p->centre.y, p->centre.z);
}

/* Stand the pins up: a triangle on each lane, point towards the bowler */
static void pins_reset(void) {
    for (int i = 0; i < PIN_COUNT; i++) {
        if (!pin_body[i]) continue;
        pp_body_set_position(pin_body[i], pin_start[i].x, pin_start[i].y, pin_start[i].z);
        pp_body_set_rotation(pin_body[i], 0.0f, 0.0f, 0.0f, 1.0f);
        pp_body_set_velocity(pin_body[i], 0.0f, 0.0f, 0.0f);
        pp_body_set_angular_velocity(pin_body[i], 0.0f, 0.0f, 0.0f);
    }
}

/* Pins are boxes: a box stands on its own, a capsule would balance on a point */
static void pins_build(void) {
    if (!pin.model) return;
    int n = 0;
    for (int l = 0; l < LANES; l++) {
        for (int row = 0; row < 4; row++) {
            float z = PIN_BACK_Z + (3 - row) * PIN_SPACING * 0.866f;   /* row 0 is the head pin */
            for (int i = 0; i <= row; i++, n++) {
                PPVec3 at = {.xyz = {lane_x[l] + (i - row * 0.5f) * PIN_SPACING,
                                     LANE_Y + pin.half.y + 0.01f, z}};
                pin_start[n] = at;
                pin_body[n]  = pp_physics_create_box(pin.half.x * 2.0f, pin.half.y * 2.0f,
                                                     pin.half.z * 2.0f, &at, 1.0f, 0);
            }
        }
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
    shz_vec3_t c = shz_vec3_scale(p->centre, p->scale);
    shz_vec3_t origin = shz_vec3_init(pos.x - (rot[0] * c.x + rot[3] * c.y + rot[6] * c.z),
                                      pos.y - (rot[1] * c.x + rot[4] * c.y + rot[7] * c.z),
                                      pos.z - (rot[2] * c.x + rot[5] * c.y + rot[8] * c.z));
    dc_model_draw_list_oriented(p->model, origin, p->scale, rot, &camera, list);
}

/* All props of one list */
static void props_draw(int list) {
    for (int i = 0; i < PIN_COUNT; i++)
        prop_draw(&pin, pin_body[i], list);
    for (int i = 0; i < thrown_count; i++)
        prop_draw(&ball, thrown[i], list);
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
    player.move_speed = 0.25f;  /* per frame; the alley is small */

    world_model = dc_model_load(ASSETS "world/test.dms");
    if (world_model)
        col_world = col_build(world_model, shz_vec3_init(0.0f, 0.0f, 0.0f), 1.0f);

    PPVec3 gravity = {.xyz = {0.0f, -9.8f, 0.0f}};
    pp_physics_set_gravity(&gravity);
    pp_physics_set_triangle_query(phys_tri_query, NULL);

    prop_load(&pin, ASSETS "pin/pin.dms");
    prop_load(&ball, ASSETS "ball/ball.dms");
    if (ball.model) ball.scale = BALL_RADIUS / ball.half.y;    /* the model is not ball sized */
    pins_build();

    /* Start at the near end of the first lane, looking down it */
    shz_vec3_t view_start = shz_vec3_init(lane_x[0], LANE_Y + 1.6f, START_Z);
    camera.pos = view_start;
    camera.yaw = F_PI;

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

        /* Reset (Y button): camera back to the start, pins up, balls gone */
        if (inp && dc_input_pressed(inp, CONT_Y)) {
            camera.pos = view_start;
            camera.yaw = F_PI;
            camera.pitch = 0.0f;
            pins_reset();
            thrown_clear();
        }

        /* Roll a ball (A) from the camera */
        if (inp && dc_input_pressed(inp, CONT_A)) ball_throw();

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
    dc_model_free(pin.model);
    dc_shutdown();

    return 0;
}
