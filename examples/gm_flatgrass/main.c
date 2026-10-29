#include <kos.h>
#include <sh4zam/shz_sh4zam.h>
#include <stdlib.h>
#include <string.h>
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

/* Garry's Mod's gm_flatgrass, brought in from the editor's Source tab, with
 * Alyx and a headcrab (Half-Life 2's) wandering about near where the scene
 * puts them: each walks to a spot near home, stands a while, and again. A
 * free camera flies through it all from where the map starts a player. */

/* ---- App state ---- */
static DCCamera camera;
static DCPlayer player;
static ColWorld* col_world = NULL;

static DCScene* scene = NULL;

/* ---- Wanderers: walk to a spot near home, stand a while, again ---- */
typedef struct {
    DCSceneObject* obj;
    shz_vec3_t home, target;
    float speed, range, radius;     /* m/s; how far from home; how wide */
    int walk, idle;                 /* animations */
    float wait;                     /* seconds left standing */
    float yaw;                      /* radians, 0 along +z */
    float stuck;                    /* seconds walking without getting nearer */
    float last_dist;
} Wanderer;

static float frand(void) { return (float)rand() / (float)RAND_MAX; }

/* The ground under a point, or below by more than a step: none */
static bool ground_at(float x, float y, float z, float* out) {
    ColGroundHit g = col_ground(col_world, shz_vec3_init(x, y + 1.0f, z), 2.0f);
    if (g.hit && g.normal.y > 0.7f) *out = g.y;
    return g.hit && g.normal.y > 0.7f;
}

/* Somewhere on the ground within range of home, not much higher or lower */
static void pick_target(Wanderer* w) {
    for (int tries = 0; tries < 12; tries++) {
        float a = frand() * 6.2831853f, d = w->range * (0.3f + 0.7f * frand());
        float x = w->home.x + shz_sinf(a) * d, z = w->home.z + shz_cosf(a) * d, y;
        if (ground_at(x, w->obj->pos.y, z, &y)) {
            w->target = shz_vec3_init(x, y, z);
            break;
        }
    }
    w->stuck = 0.0f;
    w->last_dist = 1e9f;
}

static void wanderer_init(Wanderer* w, const char* name, const char* walk, const char* idle, float speed,
                          float range, float radius) {
    w->obj = dc_scene_find(scene, name);
    if (!w->obj || !w->obj->m) {
        printf("%s: not in the scene (bring it in)\n", name);
        w->obj = NULL;
        return;
    }
    float y;
    if (ground_at(w->obj->pos.x, w->obj->pos.y + 1.0f, w->obj->pos.z, &y)) w->obj->pos.y = y;
    w->home = w->target = w->obj->pos;
    w->speed = speed;
    w->range = range;
    w->radius = radius;
    w->walk = dc_model_anim_index(w->obj->m, walk);
    w->idle = dc_model_anim_index(w->obj->m, idle);
    if (w->walk < 0 || w->idle < 0) printf("%s: no %s or %s animation\n", name, walk, idle);
    w->yaw = SHZ_DEG_TO_RAD(w->obj->rot.y);
    w->wait = 1.0f + 2.0f * frand();
}

static void wanderer_update(Wanderer* w, float dt) {
    if (!w->obj) return;
    DCSceneObject* o = w->obj;
    bool walking = false;
    if (w->wait > 0.0f) {
        w->wait -= dt;
        if (w->wait <= 0.0f) pick_target(w);
    } else {
        float dx = w->target.x - o->pos.x, dz = w->target.z - o->pos.z;
        float dist = shz_sqrtf(dx * dx + dz * dz);
        /* There, or not getting there (a wall): stand a while */
        w->stuck = dist < w->last_dist - 0.2f * w->speed * dt ? 0.0f : w->stuck + dt;
        w->last_dist = fminf(w->last_dist, dist);
        if (dist < 0.3f || w->stuck > 1.0f) {
            w->wait = 1.5f + 3.0f * frand();
        } else {
            /* Turn towards it; walk once roughly facing it */
            float want = atan2f(dx, dz), diff = want - w->yaw;
            while (diff > 3.14159265f) diff -= 6.2831853f;
            while (diff < -3.14159265f) diff += 6.2831853f;
            float turn = 4.0f * dt;
            w->yaw += diff > turn ? turn : diff < -turn ? -turn : diff;
            if (fabsf(diff) < 1.0f) {
                walking = true;
                float step = w->speed * dt;
                shz_vec3_t to = shz_vec3_init(o->pos.x + shz_sinf(w->yaw) * step, o->pos.y + 0.5f,
                                              o->pos.z + shz_cosf(w->yaw) * step);
                to = col_move_walls(col_world, o->pos, to, w->radius);
                float y;
                if (ground_at(to.x, o->pos.y, to.z, &y)) o->pos = shz_vec3_init(to.x, y, to.z);
                else w->stuck += 1.0f;  /* an edge: go somewhere else */
            }
        }
    }
    o->rot.y = SHZ_RAD_TO_DEG(w->yaw);
    int anim = walking ? w->walk : w->idle;
    if (anim >= 0 && dc_model_get_anim(o->m) != anim) dc_model_set_anim(o->m, anim);
    dc_model_animate(o->m, dt);
}

static Wanderer alyx, headcrab;

/* ========== MAIN ========== */

static bool check_exit(void) {
    const DCInput* inp = dc_input_get(0);
    if (!inp) return false;
    return (inp->buttons & CONT_START) != 0;
}

int main(int argc, char* argv[]) {
    dc_init((DCInitParams){ 0 });
    dc_draw2d_init();

    scene = dc_scene_play(ASSETS, "scenes/main.json");
    if (!scene) return 1;
    dc_scene_apply(scene);
    dc_scene_camera(scene, &camera);
    dc_player_init(&player);
    player.cam_mode = DC_CAM_NOCLIP;
    player.pos = camera.pos;
    player.yaw = camera.yaw;
    shz_vec3_t start = camera.pos;
    float start_yaw = camera.yaw;

    /* Collision for the world, where the scene puts it */
    DCSceneObject* world = dc_scene_find(scene, "World");
    if (world && world->m && !world->m->skeleton)
        col_world = col_build(world->m, world->pos, world->scale.x);

    wanderer_init(&alyx, "Alyx", "a_WalkN", "@LineIdle01", 1.4f, 5.0f, 0.3f);
    wanderer_init(&headcrab, "Headcrab", "@Run1", "@Idle01", 2.2f, 4.0f, 0.2f);

    printf("VRAM free after loading: %luKB\n", (unsigned long)(pvr_mem_available() / 1024));

    while (!check_exit()) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        /* ---- Input ---- */
        /* Back to the start (Y button) */
        if (inp && dc_input_held(inp, CONT_Y)) {
            player.pos = start;
            player.yaw = start_yaw;
            player.vy = 0.0f;
            camera.pos = start;
            camera.yaw = start_yaw;
            camera.pitch = 0.0f;
        }

        wanderer_update(&alyx, dt);
        wanderer_update(&headcrab, dt);

        dc_player_update(&player, &camera, inp, col_world, dt);
        dc_camera_update(&camera);

        /* ---- Rendering ---- */
        dc_set_camera(&camera);
        dc_scene_draw(scene);
        dc_frame_end();
    }

    dc_scene_free(scene);
    col_free(col_world);
    dc_shutdown();

    return 0;
}
