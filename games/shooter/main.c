#include <kos.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <sh4zam/shz_sh4zam.h>
#include "dms/dc_engine.h"
#include "dms/dc_input.h"
#include "dms/dc_camera.h"
#include "dms/dc_draw.h"
#include "dms/dc_draw2d.h"
#include "dms/dc_player.h"
#include "dms/dc_scene.h"
#include "dms/dc_particles.h"
#include "dms/dc_beam.h"
#include "dms/dc_corona.h"
#include "dms/collision.h"
#include "nav.h"
#include "ragdoll.h"

/* Where assets are read from: /pc/ over dcload; on a disc the engine finds them at /cd/ */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

/* A first person shooter: Half-Life 2's SMG in your hands on Civilization
 * Defense, and Left 4 Dead's infected coming at you in waves. B fires (left
 * Ctrl or the mouse on a PC), Y reloads (R), A jumps (Space) and starts again
 * once you are dead, Start quits.
 *
 * The gun is drawn with a camera of its own that never moves, after the
 * world: Source's view models are made with the eye at their origin, looking
 * down +Z once converted, so the gun stays where the hands hold it however
 * the player turns, and nothing in the world can come between. */

#define MAG       45
#define SPARE     180
#define FIRE_GAP  0.075f        /* seconds between shots, about 13 a second */

typedef enum { GUN_DRAW, GUN_IDLE, GUN_FIRE, GUN_RELOAD } GunState;

typedef struct {
    DMSModel* m;
    int a_idle, a_fire, a_draw, a_reload;
    GunState state;
    float wait;                 /* until the next shot can go */
    float kick;                 /* the view jolting up after a shot */
    int mag, spare;
} Gun;

static void gun_play(Gun* g, GunState s, int anim) {
    g->state = s;
    dc_model_set_anim(g->m, anim);
    dc_model_anim_restart(g->m);
}

static bool gun_update(Gun* g, const DCInput* inp, DCCamera* cam, float dt) {
    g->wait -= dt;
    bool shot = false;
    bool fire = inp && dc_input_held(inp, CONT_B);

    switch (g->state) {
    case GUN_DRAW:
    case GUN_RELOAD:
        if (dc_model_anim_done(g->m)) {
            if (g->state == GUN_RELOAD) {
                int take = MAG - g->mag;
                if (take > g->spare) take = g->spare;
                g->mag += take;
                g->spare -= take;
            }
            gun_play(g, GUN_IDLE, g->a_idle);
        }
        break;
    case GUN_IDLE:
    case GUN_FIRE:
        if (fire && g->mag > 0 && g->wait <= 0.0f) {
            gun_play(g, GUN_FIRE, g->a_fire);
            g->wait = FIRE_GAP;
            g->mag--;
            g->kick += 0.012f;
            shot = true;
        } else if (g->state == GUN_FIRE && !fire && dc_model_anim_done(g->m)) {
            gun_play(g, GUN_IDLE, g->a_idle);
        }
        if (inp && dc_input_pressed(inp, CONT_Y) && g->mag < MAG && g->spare > 0)
            gun_play(g, GUN_RELOAD, g->a_reload);
        else if (fire && g->mag == 0 && g->spare > 0 && g->wait <= 0.0f)
            gun_play(g, GUN_RELOAD, g->a_reload);
        break;
    }

    /* Each shot lifts the view a little (a pitch below 0 looks up) */
    cam->pitch -= g->kick;
    g->kick *= 0.6f;

    dc_model_animate(g->m, dt);
    return shot;
}

/* ---- The infected ----
 *
 * Each has a model of its own, so each has its own pose; they share the
 * first one's clips. They find their way to you over the map (nav.c), keep
 * a little apart from each other, and hit you when close. */

#define ZOMBIES      4          /* at once */
#define Z_HEALTH     60
#define Z_DAMAGE     25         /* a shot */
#define Z_SPEED      4.2f       /* metres a second, give or take 0.4 each */
#define Z_CLIP_SPEED 6.0f       /* how fast the run clip's feet go over the ground */
#define Z_TURN       6.0f       /* radians a second */
#define Z_SWIVEL     4.0f       /* attacking, feet planted */
#define Z_RADIUS     0.35f
#define Z_HEIGHT     1.8f
#define Z_REACH      1.3f
#define Z_HIT        10         /* to you, a second while it is at you */
#define Z_DEATH_RATE 1.4f       /* the death clips, faster than made */
#define Z_DEAD_TIME  8.0f       /* a body lies this long */
#define Z_LIMP       0.45f      /* seconds into its death clip before it goes limp */
#define Z_KNOCK      3.0f       /* how hard a killing shot shoves the body, m/s */

/* Left 4 Dead's death clips stagger and go down, then leave the rest to a
 * ragdoll, as here: a moment of the clip, then limp (ragdoll.c), going on
 * as the clip was moving with the shot's shove on top */
static const char* DEATHS[] = { "@Death_03", "@Death_06", "@Death_09", "@Death_10ab" };
#define N_DEATHS (int)(sizeof(DEATHS) / sizeof(DEATHS[0]))

typedef enum { Z_FREE, Z_RUN, Z_ATTACK, Z_DEAD } ZState;

typedef struct {
    DMSModel* m;
    ZState state;
    shz_vec3_t pos;             /* its feet */
    float yaw;
    float speed;
    int health;
    float flash;                /* red while hit */
    float timer;                /* dead: until it goes; attacking: to the next hit */
    shz_vec3_t goal;            /* where it is heading next (nav_steer) */
    float repath;               /* until it looks again */
    shz_vec3_t check;           /* where it was when last checked for being stuck */
    float stuck;                /* since then */
    bool again;                 /* gone without being killed: one more to come */
    int death;                  /* which of DEATHS */
    float wx, wz;               /* the way it wants to go, eased */
    bool limp;                  /* dead and gone to a ragdoll */
    shz_vec3_t knock;           /* the killing shot's shove */
    Ragdoll rd;
} Zombie;

static Zombie zombies[ZOMBIES];
static int a_run, a_attack, a_idle, a_death[N_DEATHS];
static Nav nav;

static float frand(float lo, float hi) { return lo + (hi - lo) * (float)rand() / (float)RAND_MAX; }

static void z_play(Zombie* z, int anim) {
    if (dc_model_get_anim(z->m) != anim) dc_model_set_anim(z->m, anim);
}

/* Somewhere it can get to you from, not too close */
static bool z_spawn(Zombie* z, shz_vec3_t you) {
    shz_vec3_t p;
    if (!nav_random_spot(&nav, 12.0f, 35.0f, &p)) return false;
    z->state = Z_RUN;
    z->pos = z->check = z->goal = p;
    z->yaw = atan2f(you.x - p.x, you.z - p.z);
    z->wx = sinf(z->yaw);
    z->wz = cosf(z->yaw);
    z->speed = Z_SPEED + frand(-0.4f, 0.4f);
    z->health = Z_HEALTH;
    z->flash = z->timer = z->repath = z->stuck = 0.0f;
    z_play(z, a_run);
    return true;
}

static float turn_to(float yaw, float want, float most) {
    float d = want - yaw;
    while (d > 3.14159f) d -= 6.28318f;
    while (d < -3.14159f) d += 6.28318f;
    if (d > most) d = most;
    if (d < -most) d = -most;
    return yaw + d;
}

/* Moves one; how much it hurt you (you: your feet) */
static int z_update(Zombie* z, ColWorld* col, shz_vec3_t you, float dt) {
    int hurt = 0;
    z->flash -= dt;
    float dx = you.x - z->pos.x, dz = you.z - z->pos.z;
    float dist = sqrtf(dx * dx + dz * dz);
    float rate = 1.0f;

    switch (z->state) {
    case Z_FREE:
        return 0;
    case Z_RUN: {
        /* Close, level and facing you: it swings */
        if (dist < Z_REACH && fabsf(you.y - z->pos.y) < 1.2f &&
            fabsf(turn_to(0.0f, atan2f(dx, dz) - z->yaw, 10.0f)) < 0.6f) {
            z->state = Z_ATTACK;
            z->timer = 0.5f;
            z_play(z, a_attack);
            dc_model_anim_restart(z->m);
            break;
        }
        /* The way, looked at again a few times a second */
        z->repath -= dt;
        if (z->repath <= 0.0f) {
            z->repath = frand(0.2f, 0.35f);
            if (!nav_steer(&nav, col, z->pos, you, Z_RADIUS, &z->goal)) z->goal = you;
        }
        float gx = z->goal.x - z->pos.x, gz = z->goal.z - z->pos.z;
        float glen = sqrtf(gx * gx + gz * gz);
        if (glen < 0.2f) { z->repath = 0.0f; gx = dx; gz = dz; glen = dist; }
        float mx = gx / glen, mz = gz / glen;

        /* Not on top of each other: one close ahead and nearer you, it waits
         * behind; one beside, it edges away from, sideways only (a push
         * straight back would turn it round) */
        float hold = 1.0f;
        for (int i = 0; i < ZOMBIES; i++) {
            Zombie* o = &zombies[i];
            if (o == z || (o->state != Z_RUN && o->state != Z_ATTACK)) continue;
            float ox = z->pos.x - o->pos.x, oz = z->pos.z - o->pos.z;
            float d2 = ox * ox + oz * oz;
            if (d2 > 0.9f * 0.9f || d2 < 1e-6f) continue;
            float d = sqrtf(d2), near = (0.9f - d) / 0.9f;
            float ahead = -(ox * mx + oz * mz) / d;        /* 1: right in front */
            float ux = you.x - o->pos.x, uz = you.z - o->pos.z;
            if (ahead > 0.5f && ux * ux + uz * uz < dist * dist) hold = fminf(hold, 1.0f - near);
            float side = ox * -mz + oz * mx;                /* which side it is on */
            float push = near * 0.6f * (side >= 0.0f ? 1.0f : -1.0f);
            float px = -mz * push, pz = mx * push;
            mx += px;
            mz += pz;
        }
        float mlen = sqrtf(mx * mx + mz * mz);
        if (mlen > 1e-4f) { mx /= mlen; mz /= mlen; }
        /* Eased over a tenth of a second or so: no twitching as the way ahead,
         * you and the others shift */
        float ease = 1.0f - expf(-10.0f * dt);
        z->wx += (mx - z->wx) * ease;
        z->wz += (mz - z->wz) * ease;
        mlen = sqrtf(z->wx * z->wx + z->wz * z->wz);
        if (mlen > 1e-4f) { mx = z->wx / mlen; mz = z->wz / mlen; }

        /* It goes the way it faces, never sideways or back: turning hard, it
         * slows, so it comes round on the spot more than it slides */
        float want = atan2f(mx, mz);
        z->yaw = turn_to(z->yaw, want, Z_TURN * dt);
        float fx = sinf(z->yaw), fz = cosf(z->yaw);
        float along = fx * mx + fz * mz;
        if (along < 0.2f) along = 0.2f;
        float step = z->speed * along * hold * dt;
        /* Walls push it off; a step it walks up, and off an edge it drops */
        shz_vec3_t to = shz_vec3_init(z->pos.x + fx * step, z->pos.y + NAV_BODY_Y, z->pos.z + fz * step);
        to = col_move_steps(col, to, Z_RADIUS, z->pos.y, NAV_STEP);
        ColGroundHit g = col_ground(col, shz_vec3_init(to.x, z->pos.y + NAV_STEP + 0.1f, to.z), 10.0f);
        /* A wall slid it off the way it faces: it turns to the way it went,
         * and its legs go as fast as it really moved */
        float ax = to.x - z->pos.x, az = to.z - z->pos.z, moved = sqrtf(ax * ax + az * az);
        if (moved > 1e-4f) z->yaw = turn_to(z->yaw, atan2f(ax, az), Z_TURN * 2.0f * dt);
        z->pos = shz_vec3_init(to.x, g.hit ? g.y : z->pos.y, to.z);
        rate = moved / dt / Z_CLIP_SPEED;

        /* Hardly moved in 2.5 seconds, and not because it is at you behind the
         * others: it goes, and another comes */
        z->stuck += dt;
        if (z->stuck > 2.5f) {
            float sx = z->pos.x - z->check.x, sz = z->pos.z - z->check.z;
            if (sx * sx + sz * sz < 1.0f && dist > 4.0f) { z->state = Z_FREE; z->again = true; }
            z->check = z->pos;
            z->stuck = 0.0f;
        }
        break;
    }
    case Z_ATTACK:
        /* The clip swings on and on; a hit lands each second */
        z->yaw = turn_to(z->yaw, atan2f(dx, dz), Z_SWIVEL * dt);
        z->timer += dt;
        if (z->timer >= 1.0f) {
            z->timer -= 1.0f;
            if (dist < Z_REACH + 0.3f) hurt = Z_HIT;
        }
        /* Gone out of reach, or round past its shoulder: it runs after you
         * (its legs turn it, rather than it swivelling on the spot) */
        if (dist >= Z_REACH + 0.5f || fabsf(turn_to(0.0f, atan2f(dx, dz) - z->yaw, 10.0f)) > 1.2f) {
            z->state = Z_RUN;
            z_play(z, a_run);
            z->wx = sinf(z->yaw);
            z->wz = cosf(z->yaw);
            z->repath = z->stuck = 0.0f;
            z->check = z->pos;
        }
        break;
    case Z_DEAD: {
        z->timer -= dt;
        if (z->timer <= 0.0f) z->state = Z_FREE;
        /* A moment of its clip (never past the end, where it would loop to
         * standing), the joints followed; then limp */
        if (!z->limp) {
            float left = dc_model_anim_length(z->m, a_death[z->death]) - 0.02f - dc_model_anim_time(z->m);
            dc_model_animate(z->m, fmaxf(fminf(dt * Z_DEATH_RATE, left), 0.0f));
            rd_track(&z->rd, z->m, z->pos, -z->yaw);
            if (Z_DEAD_TIME - z->timer >= Z_LIMP || left <= 0.0f) {
                rd_start(&z->rd, z->m, z->pos, -z->yaw, z->knock);
                z->limp = true;
            }
        } else if (dt > 0.0f) {
            rd_step(&z->rd, col, dt);
            rd_pose(&z->rd, z->m, z->pos, -z->yaw);
        }
        return 0;
    }
    }
    dc_model_animate(z->m, dt * rate);
    return hurt;
}

/* The shot along the crosshair: the nearest infected it meets before a
 * wall, or -1. Each is a standing cylinder. *wall is what it met of the
 * world, *t how far it went. */
static int z_shot(ColWorld* col, shz_vec3_t o, shz_vec3_t d, ColRayHit* wall, float* t_out) {
    *wall = col_raycast(col, o, d, 100.0f);
    float best = wall->t;
    int hit = -1;
    float a = d.x * d.x + d.z * d.z;
    if (a < 1e-6f) return -1;
    for (int i = 0; i < ZOMBIES; i++) {
        Zombie* z = &zombies[i];
        if (z->state == Z_FREE || z->state == Z_DEAD) continue;
        float ox = o.x - z->pos.x, oz = o.z - z->pos.z;
        float b = 2.0f * (ox * d.x + oz * d.z);
        float c = ox * ox + oz * oz - Z_RADIUS * Z_RADIUS;
        float disc = b * b - 4.0f * a * c;
        if (disc < 0.0f) continue;
        float t = (-b - sqrtf(disc)) / (2.0f * a);
        if (t < 0.0f || t >= best) continue;
        float y = o.y + d.y * t;
        if (y < z->pos.y || y > z->pos.y + Z_HEIGHT) continue;
        best = t;
        hit = i;
    }
    *t_out = best;
    return hit;
}

/* ---- What a shot looks like ----
 *
 * A flash at the muzzle (drawn with the gun's camera, so it stays on the
 * barrel), sparks and a puff of dust where it hits the world, blood where it
 * hits one of them. Each kind is one set of particles moved to the place and
 * burst. */

typedef struct {
    DCParticles *flash, *smoke;     /* in the gun's space */
    DCParticles *sparks, *dust;     /* the world */
    DCParticles *blood, *drops;
    int muzzle;                     /* the gun's muzzle bone, or -1 */
} Fx;

static Fx fx_init(const DMSModel* gun) {
    Fx f;
    f.flash = dc_particles_create(16, &(DCParticleOpts){
        .speed = { 0, 0, 0.6f }, .speed_spread = { 0.15f, 0.15f, 0.3f },
        .spread = { 0.01f, 0.01f, 0.02f },
        .life = 0.06f, .life_spread = 0.015f, .size = 0.16f, .size_spread = 0.04f, .grow = 1.4f,
        .spin = 4.0f, .start = 0xFFF8E0, .middle = 0xFFB040, .end = 0x602000 });
    f.smoke = dc_particles_create(32, &(DCParticleOpts){
        .speed = { 0, 0.08f, 0.15f }, .speed_spread = { 0.04f, 0.04f, 0.05f }, .drag = 1.5f,
        .life = 0.5f, .life_spread = 0.15f, .size = 0.04f, .grow = 4.0f,
        .start = 0x303030, .end = 0x000000 });
    f.sparks = dc_particles_create(160, &(DCParticleOpts){
        .speed_spread = { 2.2f, 2.2f, 2.2f }, .gravity = 9.8f,
        .life = 0.25f, .life_spread = 0.1f, .size = 0.035f, .size_spread = 0.01f, .grow = 0.4f,
        .start = 0xFFF0B0, .middle = 0xFF9020, .end = 0x200000 });
    f.dust = dc_particles_create(64, &(DCParticleOpts){
        .spread = { 0.03f, 0.03f, 0.03f }, .speed_spread = { 0.25f, 0.25f, 0.25f }, .drag = 2.0f,
        .gravity = 0.4f, .life = 0.7f, .life_spread = 0.2f, .size = 0.12f, .grow = 3.0f,
        .start = 0x8A8278, .end = 0x5A554F, .smoke = true });
    f.blood = dc_particles_create(64, &(DCParticleOpts){
        .spread = { 0.05f, 0.05f, 0.05f }, .speed_spread = { 0.3f, 0.3f, 0.3f }, .drag = 3.0f,
        .life = 0.35f, .life_spread = 0.1f, .size = 0.15f, .grow = 2.5f,
        .start = 0x8A0A06, .end = 0x400302, .smoke = true });
    f.drops = dc_particles_create(160, &(DCParticleOpts){
        .speed_spread = { 1.2f, 1.2f, 1.2f }, .gravity = 9.8f,
        .life = 0.5f, .life_spread = 0.15f, .size = 0.035f, .size_spread = 0.015f,
        .start = 0x900805, .end = 0x500302, .smoke = true });
    f.muzzle = -1;
    const DMSSkeleton* sk = gun->skeleton;
    for (int i = 0; sk && i < sk->boneCount; i++)
        if (!strcmp(sk->bones[i].name, "ValveBiped.muzzle")) f.muzzle = i;
    if (f.muzzle < 0) printf("shooter: the gun has no muzzle bone, no flash\n");
    return f;
}

/* A shot leaving the gun */
static void fx_fire(Fx* f, const DMSModel* gun) {
    if (f->muzzle < 0) return;
    const shz_mat4x4_t* w = &gun->skeleton->bones[f->muzzle].worldPose;
    shz_vec3_t p = shz_vec3_init(w->elem2D[3][0], w->elem2D[3][1], w->elem2D[3][2]);
    dc_particles_move(f->flash, p);
    dc_particles_burst(f->flash, 4);
    dc_particles_move(f->smoke, p);
    dc_particles_burst(f->smoke, 1);
}

/* The torch: clipped under the barrel, a little back from the muzzle, in
 * the gun's space */
#define TORCH_DARK   0.16f          /* light left outside the beam */
#define TORCH_SPREAD 0.36f          /* half the beam's angle, radians */
#define TORCH_REACH  40.0f          /* how far it lights: past the fog */
#define TORCH_SHAFT_START 1.5f      /* the shaft shows from this far past the lens */
static float wrap_pi(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}
static shz_vec3_t torch_lens(const Fx* f, const DMSModel* gun) {
    if (f->muzzle < 0) return shz_vec3_init(-0.16f, -0.17f, 0.72f);
    const shz_mat4x4_t* w = &gun->skeleton->bones[f->muzzle].worldPose;
    return shz_vec3_init(w->elem2D[3][0], w->elem2D[3][1] - 0.05f, w->elem2D[3][2] - 0.16f);
}

/* Where the torch lands, a glow: the light coming back off what it hits */
static __attribute__((noinline)) float torch_spot(ColWorld* col, shz_vec3_t eye, shz_vec3_t dir) {
    ColRayHit spot = col_raycast(col, eye, dir, TORCH_REACH);
    if (!spot.hit) return TORCH_REACH;
    dc_corona(1, shz_vec3_add(eye, shz_vec3_scale(dir, spot.t)), 0x806E58,
              0.5f + spot.t * 0.12f, DC_CORONA_OWN_CAMERA);
    return spot.t;
}

/* The shaft, from the lens: where the lens is on the screen (the gun's
 * camera sees wider), at that depth in the world, aimed to meet the middle
 * of the view 12m out, or where the light lands if that is nearer, and
 * ending there: not through the wall */
static __attribute__((noinline)) void torch_shaft(const DCCamera* cam, const DCCamera* gun_cam,
                                                  const Fx* fx, const DMSModel* gun_m, shz_vec3_t dir,
                                                  float hit) {
    shz_vec3_t lens = torch_lens(fx, gun_m);
    float wide = tanf(cam->fov * 0.5f * 0.0174533f) / tanf(gun_cam->fov * 0.5f * 0.0174533f);
    shz_sincos_t cy = shz_sincosf(cam->yaw), cp = shz_sincosf(cam->pitch);
    shz_vec3_t f = shz_vec3_init(cy.sin * cp.cos, -cp.sin, cy.cos * cp.cos);
    shz_vec3_t right = shz_vec3_init(-cy.cos, 0.0f, cy.sin);
    shz_vec3_t up = shz_vec3_init(cy.sin * cp.sin, cp.cos, cy.cos * cp.sin);
    shz_vec3_t from = shz_vec3_add(cam->pos, shz_vec3_add(shz_vec3_scale(f, lens.z),
        shz_vec3_add(shz_vec3_scale(right, -lens.x * wide), shz_vec3_scale(up, lens.y * wide))));
    shz_vec3_t aim = shz_vec3_add(cam->pos, shz_vec3_scale(dir, hit < 12.0f ? hit : 12.0f));
    shz_vec3_t to = shz_vec3_sub(aim, from);
    float reach = sqrtf(shz_vec3_dot(to, to)) + (hit < 12.0f ? 0.0f : 4.0f);
    dc_beam_shaft(from, to, 0.02f, TORCH_SPREAD * 0.75f, TORCH_SHAFT_START,
                  reach < 16.0f ? reach : 16.0f, 0xFFE6BE, 0.9f);
}

/* The torch, lit: the dark round its beam, the beam from the eye where you
 * aim (the light's picture is the screen's middle), the glow where it lands
 * and the shaft from the lens. In pieces: all inlined into one function it
 * was more than GCC 15 could compile (an internal compiler error). */
static void torch_draw(const DCCamera* cam, const DCCamera* gun_cam, const Fx* fx,
                       const DMSModel* gun_m, ColWorld* col, float torch_yaw, float torch_pitch) {
    shz_sincos_t y = shz_sincosf(torch_yaw), p = shz_sincosf(torch_pitch);
    shz_vec3_t dir = shz_vec3_init(y.sin * p.cos, -p.sin, y.cos * p.cos);
    dc_beam_dark(TORCH_DARK, 1.0f);
    dc_beam(cam->pos, dir, TORCH_SPREAD, TORCH_REACH);
    float hit = torch_spot(col, cam->pos, dir);
    torch_shaft(cam, gun_cam, fx, gun_m, dir, hit);
}


/* Far off, an effect is made bigger (and faster, to keep its shape), so it
 * still shows: as it would be at 8 metres */
static float fx_far(float dist) { return dist > 8.0f ? dist / 8.0f : 1.0f; }

/* It met the world at hit, coming along d, dist away */
static void fx_wall(Fx* f, const ColRayHit* hit, shz_vec3_t d, float dist) {
    dc_particles_scale(f->sparks, fx_far(dist));
    dc_particles_scale(f->dust, fx_far(dist));
    shz_vec3_t n = hit->normal;
    if (d.x * n.x + d.y * n.y + d.z * n.z > 0.0f) n = shz_vec3_scale(n, -1.0f);   /* towards you */
    /* Off the way it came back, reflected, and the face's own way */
    float k = -2.0f * (d.x * n.x + d.y * n.y + d.z * n.z);
    shz_vec3_t r = shz_vec3_init(d.x + n.x * k, d.y + n.y * k, d.z + n.z * k);
    shz_vec3_t at = shz_vec3_add(hit->pos, shz_vec3_scale(n, 0.02f));
    dc_particles_move(f->sparks, at);
    dc_particles_aim(f->sparks, shz_vec3_scale(shz_vec3_add(r, n), 2.0f));
    dc_particles_burst(f->sparks, 7);
    dc_particles_move(f->dust, at);
    dc_particles_aim(f->dust, shz_vec3_scale(n, 0.6f));
    dc_particles_burst(f->dust, 3);
}

/* One of them hit at p by a shot along d, dist away */
static void fx_blood(Fx* f, shz_vec3_t p, shz_vec3_t d, float dist) {
    dc_particles_scale(f->blood, fx_far(dist));
    dc_particles_scale(f->drops, fx_far(dist));
    dc_particles_move(f->blood, p);
    dc_particles_aim(f->blood, shz_vec3_scale(d, -0.5f));
    dc_particles_burst(f->blood, 3);
    dc_particles_move(f->drops, p);
    /* Most of it out the far side, some back at you */
    dc_particles_aim(f->drops, shz_vec3_add(shz_vec3_scale(d, 1.5f), shz_vec3_init(0, 0.8f, 0)));
    dc_particles_burst(f->drops, 6);
    dc_particles_aim(f->drops, shz_vec3_add(shz_vec3_scale(d, -1.0f), shz_vec3_init(0, 0.8f, 0)));
    dc_particles_burst(f->drops, 3);
}

int main(int argc, char* argv[]) {
    /* The vertex buffer: this map wants under 1MB a frame; the 2MB default
     * (doubled) left too little texture RAM on the console */
    dc_init((DCInitParams){ .beams = true, .vram_size = 1280 * 1024 });
    dc_beam_dust(0.35f);        /* motes in the torch's beam, faint */
    dc_draw2d_init();

    DCScene* scene = dc_scene_play(ASSETS, "scenes/main.json");
    if (!scene) return 1;
    dc_scene_apply(scene);

    ColWorld* col = NULL;
    DCSceneObject* world = dc_scene_find(scene, "World");
    if (world && world->m) col = col_build(world->m, world->pos, world->scale.x);

    DCCamera camera;
    DCPlayer player;
    dc_scene_camera(scene, &camera);
    /* Past the fog nothing shows, so nothing is drawn: a long tunnel looked
     * down would fill the vertex buffer */
    if (scene->fog) camera.far = scene->fog_far + 2.0f;
    dc_player_init(&player);
    player.cam_mode = DC_CAM_FPS;
    player.move_speed = 0.092f;                /* per frame: 5.5 m/s at 60 */
    player.sprint_speed = player.move_speed;   /* R trigger is free for firing */
    DCSceneObject* spawn = dc_scene_find(scene, "Player");
    shz_vec3_t start = spawn ? spawn->pos : camera.pos;
    float start_yaw = camera.yaw;
    /* Where the infected can walk: what can be walked to from here. The
     * spawn marker is at your feet. */
    if (!nav_load(&nav, col, 0.75f, Z_RADIUS, start, ASSETS "scenes/main.nav")) {
        if (!nav_build(&nav, col, 0.75f, Z_RADIUS, start)) { printf("shooter: nowhere to walk\n"); return 1; }
#ifdef DMS_HOST
        /* Kept for the console, which then need not work it out (make host once after the map changes) */
        if (nav_save(&nav, col, Z_RADIUS, start, ASSETS "scenes/main.nav")) printf("nav: saved for the console\n");
#else
        printf("nav: built here; run the PC build once to keep it in scenes/main.nav\n");
#endif
    }

    Gun gun = { .m = dc_model_load(ASSETS "gmod/smg1/smg1.dms") };
    if (!gun.m) { printf("shooter: no gmod/smg1/smg1.dms (see README)\n"); return 1; }
    gun.a_idle   = dc_model_anim_index(gun.m, "@idle01");
    gun.a_fire   = dc_model_anim_index(gun.m, "@fire01");
    gun.a_draw   = dc_model_anim_index(gun.m, "@draw");
    gun.a_reload = dc_model_anim_index(gun.m, "@reload");

    for (int i = 0; i < ZOMBIES; i++) {
        zombies[i].m = dc_model_load(ASSETS "gmod/infected/infected.dms");
        if (!zombies[i].m) { printf("shooter: no gmod/infected/infected.dms (see README)\n"); return 1; }
        if (i) dc_model_share_anims(zombies[i].m, zombies[0].m);
    }
    a_run    = dc_model_anim_index(zombies[0].m, "a_Run_01");
    a_attack = dc_model_anim_index(zombies[0].m, "@Melee_03");
    for (int i = 0; i < N_DEATHS; i++) a_death[i] = dc_model_anim_index(zombies[0].m, DEATHS[i]);
    if (!rd_init(zombies[0].m)) { printf("shooter: the infected is not on Valve's biped skeleton\n"); return 1; }
    a_idle   = dc_model_anim_index(zombies[0].m, "@Idle_Neutral_01");

    /* The gun's own camera: at the origin, looking down +Z, narrower as
     * Half-Life 2 draws its view models */
    DCCamera gun_cam;
    dc_camera_init(&gun_cam);
    gun_cam.fov = 54.0f;
    dc_camera_update(&gun_cam);
    Fx fx = fx_init(gun.m);

    int health = 0, wave = 0, to_spawn = 0, kills = 0;
    bool torch = true;          /* X switches it */
    float torch_yaw = 0.0f, torch_pitch = 0.0f;
    float wave_break = 0.0f, hurt_flash = 0.0f, hit_mark = 0.0f;
    bool restart = true;

    for (;;) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);
        if (inp && (inp->buttons & CONT_START)) break;
        if (inp && dc_input_pressed(inp, CONT_X)) torch = !torch;
        /* B fires and the R trigger too, on a pad */
        DCInput in;
        if (inp) {
            in = *inp;
            if (in.rtrig > 0.5f) in.buttons |= CONT_B;
            inp = &in;
        }

        if (restart) {
            restart = false;
            player.pos = start;
            player.vy = 0.0f;
            camera.yaw = player.yaw = start_yaw;
            torch_yaw = start_yaw; torch_pitch = 0.0f;
            camera.pitch = 0.0f;
            gun.mag = MAG;
            gun.spare = SPARE;
            gun_play(&gun, GUN_DRAW, gun.a_draw);
            for (int i = 0; i < ZOMBIES; i++) { zombies[i].state = Z_FREE; zombies[i].again = false; }
            health = 100;
            wave = kills = to_spawn = 0;
            wave_break = 2.0f;
        }

        bool alive = health > 0;
        if (alive) {
            dc_player_update(&player, &camera, inp, col, dt);
            if (gun_update(&gun, inp, &camera, dt)) {
                shz_sincos_t y = shz_sincosf(camera.yaw), p = shz_sincosf(camera.pitch);
                shz_vec3_t d = shz_vec3_init(y.sin * p.cos, -p.sin, y.cos * p.cos);
                ColRayHit wall;
                float t;
                int i = z_shot(col, camera.pos, d, &wall, &t);
                fx_fire(&fx, gun.m);
                if (i < 0 && wall.hit) fx_wall(&fx, &wall, d, wall.t);
                if (i >= 0) {
                    Zombie* z = &zombies[i];
                    fx_blood(&fx, shz_vec3_add(camera.pos, shz_vec3_scale(d, t)), d, t);
                    z->health -= Z_DAMAGE;
                    z->flash = 0.1f;
                    hit_mark = 0.15f;
                    if (z->health <= 0) {
                        z->state = Z_DEAD;
                        z->timer = Z_DEAD_TIME;
                        z->death = rand() % N_DEATHS;
                        z->limp = z->rd.tracked = false;
                        z->knock = shz_vec3_scale(d, Z_KNOCK);
                        z_play(z, a_death[z->death]);
                        dc_model_anim_restart(z->m);
                        kills++;
                    }
                }
            }
        } else if (inp && dc_input_pressed(inp, CONT_A)) {
            restart = true;
        }
        dc_camera_update(&camera);
        shz_vec3_t feet = shz_vec3_init(player.pos.x, player.pos.y - player.eye_height, player.pos.z);
        nav_flow(&nav, feet);

        /* Waves: a break, then more each time, at most ZOMBIES at once */
        int living = 0;
        for (int i = 0; i < ZOMBIES; i++)
            if (zombies[i].state == Z_RUN || zombies[i].state == Z_ATTACK) living++;
        if (wave_break > 0.0f) {
            wave_break -= dt;
            if (wave_break <= 0.0f) { wave++; to_spawn = 3 + wave * 2; }
        } else if (to_spawn > 0 && living < ZOMBIES && alive) {
            for (int i = 0; i < ZOMBIES; i++)
                if (zombies[i].state == Z_FREE) {
                    if (z_spawn(&zombies[i], feet)) to_spawn--;
                    break;
                }
        } else if (to_spawn == 0 && living == 0 && alive) {
            wave_break = 4.0f;
        }

        for (int i = 0; i < ZOMBIES; i++) {
            int hurt = z_update(&zombies[i], col, feet, alive ? dt : 0.0f);
            if (hurt && alive) { health -= hurt; hurt_flash = 0.3f; }
            if (zombies[i].again) { zombies[i].again = false; to_spawn++; }
        }
        hurt_flash -= dt;
        hit_mark -= dt;

        dc_set_camera(&camera);
        /* The torch: the map is brought in 4 times as bright, nowhere darker
         * than about half (--light 4 --ambient 0.55), and
         * darkened again outside its beam. Nearer than a metre is left alone,
         * so the gun (drawn by its own camera, nearer than that) is not. */
        /* It is held, not glued to the eye: it trails a turn a little */
        float lag = 1.0f - expf(-14.0f * dt);
        torch_yaw += wrap_pi(camera.yaw - torch_yaw) * lag;
        torch_pitch += (camera.pitch - torch_pitch) * lag;
        if (torch && alive)
            torch_draw(&camera, &gun_cam, &fx, gun.m, col, torch_yaw, torch_pitch);
        else
            dc_beam_dark(alive ? TORCH_DARK * 0.4f : TORCH_DARK, 1.0f);
        dc_scene_draw(scene);
        for (int i = 0; i < ZOMBIES; i++) {
            Zombie* z = &zombies[i];
            if (z->state == Z_FREE) continue;
            /* The model faces +z, and dc_draw_ex's yaw turns +z to (-sin, cos)
             * where z->yaw goes to (sin, cos): drawn at -yaw, it faces the way
             * it goes. A limp one's bones are already where it lies. */
            dc_draw_ex(z->m, &(DCDrawOpts){ .pos = z->pos, .yaw = -z->yaw,
                                            .tint = z->flash > 0.0f ? 0xFF5050 : 0 });
        }
        dc_particles_draw(fx.sparks);
        dc_particles_draw(fx.dust);
        dc_particles_draw(fx.blood);
        dc_particles_draw(fx.drops);
        if (alive) {
            dc_set_camera(&gun_cam);
            dc_draw(gun.m, shz_vec3_init(0.0f, 0.0f, 0.0f));

            dc_particles_draw(fx.smoke);
            dc_particles_draw(fx.flash);
        }

        /* Crosshair, hits, health, ammo, the wave */
        uint32_t cross = hit_mark > 0.0f ? 0xFFFF4040 : 0xC0FFFFFF;
        dc_draw_rect(319, 233, 2, 14, cross);
        dc_draw_rect(313, 239, 14, 2, cross);
        char text[64];
        snprintf(text, sizeof(text), "%d / %d", gun.mag, gun.spare);
        dc_draw_text(text, 640 - 24 - dc_text_width(text, 24), 480 - 48, 24, DC_COLOR_YELLOW);
        snprintf(text, sizeof(text), "+ %d", health > 0 ? health : 0);
        dc_draw_text(text, 24, 480 - 48, 24, health > 30 ? DC_COLOR_YELLOW : DC_COLOR_RED);
        snprintf(text, sizeof(text), "WAVE %d   KILLS %d", wave, kills);
        dc_draw_text(text, 24, 24, 16, 0xFFFFFFFF);
        if (hurt_flash > 0.0f) dc_draw_rect(0, 0, 640, 480, 0x60FF0000);
        if (!alive) {
            dc_draw_rect(0, 0, 640, 480, 0x80400000);
            const char* dead = "YOU DIED";
            dc_draw_text(dead, 320 - dc_text_width(dead, 48) / 2, 190, 48, DC_COLOR_RED);
            snprintf(text, sizeof(text), "wave %d, %d kills - A to go again", wave, kills);
            dc_draw_text(text, 320 - dc_text_width(text, 16) / 2, 260, 16, 0xFFFFFFFF);
        } else if (wave_break > 0.0f && wave_break < 3.0f) {
            snprintf(text, sizeof(text), "WAVE %d", wave + 1);
            dc_draw_text(text, 320 - dc_text_width(text, 32) / 2, 140, 32, 0xFFFFFFFF);
        }
        dc_frame_end();
    }

    DCParticles* all[] = { fx.flash, fx.smoke, fx.sparks, fx.dust, fx.blood, fx.drops };
    for (int i = 0; i < 6; i++) dc_particles_free(all[i]);
    for (int i = ZOMBIES - 1; i >= 0; i--) dc_model_free(zombies[i].m);
    dc_model_free(gun.m);
    nav_free(&nav);
    col_free(col);
    dc_scene_free(scene);
    dc_shutdown();
    return 0;
}
