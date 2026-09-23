/* The things that fight back: rats, bulldozers, slimes, security droids and
 * two kinds of turret, from the Empties "rat", "dozer", "slime" and
 * "lavaslime", "droid"/"droidN" (its death switches on N), "turret" and
 * "pturret". His spin attack is what hurts them. */
#include "sound.h"
#include "level.h"

#define MAX_RATS        4
#define MAX_DOZERS      4
#define MAX_SLIMES      12              /* two placed, each splits into three */
#define MAX_DROIDS      2
#define MAX_TURRETS     8
#define MAX_PULSES      12

#define RAT_AGGRO       100.0f
#define RAT_DEAGGRO     200.0f
#define RAT_ATTACK      35.0f
#define RAT_BITE        45.0f           /* still this close when the bite lands */
#define RAT_SPEED       75.0f
#define RAT_COOLDOWN    1.0f
#define RAT_RADIUS      6.0f
#define RAT_BODY_Y      8.0f
#define RAT_STEP_DOWN   20.0f           /* a drop past this and it will not walk off */

#define DOZER_SPEED     40.0f
#define DOZER_TURN      0.5f            /* radians a second, on the spot */
#define DOZER_AIM       0.6f            /* drives once within this of facing him */
#define DOZER_RANGE     200.0f
#define DOZER_CLIFF_AHEAD 70.0f
#define DOZER_CLIFF_DROP  25.0f
#define DOZER_CLIFF_PAUSE 0.5f
#define DOZER_REVERSE   1.0f
#define DOZER_GRAVITY   400.0f
#define DOZER_RADIUS    15.0f
#define DOZER_TOUCH     (10.4f + 20.0f) /* times its scale, from the feet */
#define DOZER_SHOVE     300.0f

#define SLIME_AGGRO     120.0f
#define SLIME_DEAGGRO   200.0f
#define SLIME_WINDUP    0.5f
#define SLIME_JUMP_Y    (10.0f * 30.0f)
#define SLIME_GRAVITY   (1.2f * 30.0f * 30.0f)
#define SLIME_SPEED     (40.0f * 0.08f * 30.0f) /* over the square root of its scale */
#define SLIME_TOUCH     30.0f
#define SLIME_HEIGHT    25.0f
#define SLIME_STOMP_LO  5.0f            /* feet this far above its base: a stomp */
#define SLIME_STOMP_HI  25.0f
#define SLIME_BOUNCE    (8.0f * 30.0f)
#define SLIME_KNOCK     40.0f           /* units away, at once */
#define SLIME_KNOCK_UP  (2.0f * 30.0f)
#define SLIME_COOLDOWN  1.0f
#define SLIME_DIE_TIME  0.4f
#define SLIME_SMALL     0.5f
#define SLIME_SPLIT_AT  0.7f
#define SLIME_BOTTOM    (0.42f * WORLD_SCALE)   /* the model is centred; times its scale */
#define SLIME_MERGE_REACH 20.0f         /* times the two scales' mean: two of a kind this close join */
#define SLIME_MERGE_COOL  3.0f

#define DROID_HP        3
#define DROID_RANGE     300.0f
#define DROID_BLOCK     100.0f
#define DROID_COOL      1.5f
#define DROID_COOL_EDGE 0.75f           /* fires faster with a drop in front of it */
#define DROID_SHOT_AT   0.625f          /* into the clip */
#define DROID_WALK      30.0f
#define DROID_TURN      2.0f
#define DROID_FACING    0.35f           /* fires within this of facing him */
#define DROID_AHEAD     25.0f           /* looks for a drop this far ahead */
#define DROID_DROP      50.0f
#define DROID_RADIUS    12.0f
#define DROID_HEIGHT    30.0f
#define DROID_STOP      0.06f           /* chance a second of stopping mid patrol */

#define TURRET_RANGE    300.0f
#define TURRET_TURN     2.3f
#define TURRET_AIM      0.05f           /* fires once this close to on target */
#define TURRET_LOCK     2.0f            /* seconds of seeing him before the first shot */
#define TURRET_RECOVER  1.0f            /* the lock starts this far back after a shot */
#define TURRET_SHOT_AT  0.17f           /* the shot leaves this far into the clip */
#define TURRET_HIT_AT   0.25f           /* and lands here */
#define TURRET_HIT_R    12.0f
#define TURRET_SHOT_SPEED 1400.0f
#define TURRET_SHOT_LIFE  1.5f
#define TURRET_CANNON_Y 18.0f           /* times scale */
#define TURRET_PITCH_LO -1.047f
#define TURRET_PITCH_HI 0.785f
#define PTURRET_RANGE   400.0f
#define PTURRET_TURN    1.5f
#define PTURRET_COOL    2.5f
#define PTURRET_FIRST   0.5f
#define PTURRET_CANNON_Y 20.0f

/* A droid's pulse flies straight; a turret's is slow and bends toward him */
#define PULSE_SPEED     100.0f
#define PULSE_LIFE      3.0f
#define PULSE_HIT_R     12.0f
#define PULSE_GROW      1.5f            /* seconds to full size */
#define HOMING_SPEED    80.0f
#define HOMING_LIFE     5.0f
#define HOMING_TURN     0.4f            /* how hard it bends, a second */
#define HOMING_LAG      1.5f            /* its idea of where he is catches up at this rate */
#define HOMING_HIT      15.0f

typedef enum { RAT_IDLE, RAT_CHASE, RAT_ATTACKING } RatState;
typedef struct {
    DMSModel*  model;
    shz_vec3_t pos, home;
    float      yaw, timer, cooldown, scale;
    RatState   state;
    bool       bitten, dead;
    int        aIdle, aRun, aAttack, lastSpin;
} Rat;
typedef struct {
    DMSModel*  model;
    shz_vec3_t pos, home;
    float      yaw, scale, velY, pause, reverse, hurtCool;
    bool       grounded;
} Dozer;
typedef struct {
    bool       active, grounded, aggro, lava;
    shz_vec3_t pos;
    float      scale, yaw, velX, velY, velZ, jumpTimer, windup, stretch, stretchVel, cool, dying, invuln, mergeCool;
    int        hp, lastSpin;
} Slime;
typedef struct {
    DMSModel*  model;
    shz_vec3_t pos, home;
    float      yaw, cool, idle, shootTime, velY;
    int        hp, id, lastSpin, anim, aIdle, aWalk, aBlock, aShoot;
    bool       alive, grounded, shooting, walking;
} Droid;
typedef struct {
    DMSModel*  cannon;
    shz_vec3_t pos, shotPos, shotDir;
    float      yaw, scale, aimYaw, aimPitch, lock, fireTime, shotLife, cool;
    bool       alive, firing, pulse;    /* pulse: the slow homing kind */
    int        lastSpin;
} Turret;
typedef struct { shz_vec3_t pos, vel, target; float life; bool homing; } Pulse;

static DMSModel  *slimeModel, *lavaSlimeModel, *turretBase, *pturretBase, *shotModel, *pulseModel;
static Rat        rats[MAX_RATS];
static Dozer      dozers[MAX_DOZERS];
static Slime      slimes[MAX_SLIMES], slimes0[MAX_SLIMES];   /* slimes0: as placed, for the respawn */
static Droid      droids[MAX_DROIDS];
static Turret     turrets[MAX_TURRETS];
static Pulse      pulses[MAX_PULSES];
static int        ratCount, dozerCount, slimeCount, slimeCount0, droidCount, turretCount;

/* A spin attack lands on the thing at pos, once per spin. Each player's
 * spins count separately: the id is his spin count with his number on top */
static bool spin_hits(shz_vec3_t pos, float yReach, int* lastSpin) {
    bool hit = false;
    FOR_PLAYERS(i) {
        int id = bot->spinId * MAX_PLAYERS + i;
        if (hit || !bot->spinning || *lastSpin == id || dist_xz(bot->pos, pos) >= SPIN_REACH || fabsf(bot->pos.y - pos.y) >= yReach) continue;
        *lastSpin = id;
        hit = true;
    }
    return hit;
}

/* The enemy's target: the nearest living player, selected; false with none */
static bool target(shz_vec3_t from) { return robot_select_nearest(from); }

/* Sideways through the walls with the feet at pos, then down with velY
 * onto the floor, looked for from `up` above the feet; true when on it */
static bool move_body(shz_vec3_t* pos, float vx, float vz, float* velY, float radius, float up, float dt) {
    shz_vec3_t body = shz_vec3_init(pos->x, pos->y + radius, pos->z);
    shz_vec3_t to   = shz_vec3_init(body.x + vx * dt, body.y, body.z + vz * dt);
    shz_vec3_t out  = col ? world_move(body, to, radius) : to;
    pos->x = out.x; pos->z = out.z;
    pos->y += *velY * dt;
    if (!col) return false;
    ColGroundHit g = world_ground(shz_vec3_init(pos->x, pos->y + up, pos->z), up + 400.0f);
    if (!g.hit || *velY > 0.0f || pos->y > g.y + 1.0f) return false;
    pos->y = g.y; *velY = 0.0f;
    return true;
}

static int entities(const char* name, const DMSEntity** out, int max) {
    int n = level ? dc_model_entities(level, name, out, max) : 0;
    return n > max ? max : n;
}

/* ---- Setting up ---- */

void enemies_init(void) {
    const DMSEntity* e[MAX_SLIMES];
    ratCount = dozerCount = slimeCount = droidCount = turretCount = 0;
    slimeModel = lavaSlimeModel = turretBase = pturretBase = shotModel = pulseModel = NULL;
    memset(pulses, 0, sizeof(pulses));

    ratCount = entities("rat", e, MAX_RATS);
    for (int i = 0; i < ratCount; i++) {
        Rat* r = &rats[i];
        *r = (Rat){ .model = load("game", "rat"), .pos = ent_pos(e[i]), .yaw = ent_yaw(e[i]), .scale = e[i]->scale.x };
        ColGroundHit g = col ? col_ground(col, shz_vec3_init(r->pos.x, r->pos.y + STEP_UP, r->pos.z), 400.0f) : (ColGroundHit){ 0 };
        if (g.hit) r->pos.y = g.y;      /* placed in the air: down to the floor */
        r->home = r->pos;
        r->aIdle   = dc_model_anim_index(r->model, "rat_idle");
        r->aRun    = dc_model_anim_index(r->model, "rat_running");
        r->aAttack = dc_model_anim_index(r->model, "rat_attack");
        dc_model_set_anim(r->model, r->aIdle);
    }

    dozerCount = entities("dozer", e, MAX_DOZERS);
    for (int i = 0; i < dozerCount; i++) {
        Dozer* d = &dozers[i];
        *d = (Dozer){ .model = load("game", "Bulldozer"), .pos = ent_pos(e[i]), .home = ent_pos(e[i]), .yaw = PI, .scale = e[i]->scale.x };
        dc_model_set_anim(d->model, dc_model_anim_index(d->model, "dozer_loop"));
    }

    for (int lava = 0; lava < 2; lava++) {
        int n = entities(lava ? "lavaslime" : "slime", e, MAX_SLIMES - slimeCount);
        for (int i = 0; i < n; i++, slimeCount++) {
            float sc = e[i]->scale.x;
            slimes[slimeCount] = (Slime){ .active = true, .lava = lava, .pos = ent_pos(e[i]), .scale = sc, .yaw = ent_yaw(e[i]),
                                          .jumpTimer = 0.5f + rand_f(), .stretch = 1.0f, .hp = sc >= 1.5f ? 3 : sc >= 1.0f ? 2 : 1 };
        }
        if (n) *(lava ? &lavaSlimeModel : &slimeModel) = load("game", lava ? "Slime_Lava" : "slime");
    }
    memcpy(slimes0, slimes, sizeof(slimes));
    slimeCount0 = slimeCount;

    for (int id = 1; id < MAX_IDS; id++) {
        char name[16];
        snprintf(name, sizeof(name), "droid%d", id);
        int n = entities(name, e, MAX_DROIDS - droidCount);
        if (id == 1) n += entities("droid", e + n, MAX_DROIDS - droidCount - n);
        for (int i = 0; i < n; i++, droidCount++) {
            Droid* d = &droids[droidCount];
            *d = (Droid){ .model = load("game", "droid_sec"), .pos = ent_pos(e[i]), .home = ent_pos(e[i]), .yaw = ent_yaw(e[i]),
                          .id = id, .hp = DROID_HP, .alive = true, .idle = 1.0f, .anim = -1 };
            d->aIdle  = dc_model_anim_index(d->model, "sec_idle");
            d->aWalk  = dc_model_anim_index(d->model, "sec_walk");
            d->aBlock = dc_model_anim_index(d->model, "sec_block");
            d->aShoot = dc_model_anim_index(d->model, "sec_shoot");
        }
    }

    for (int pulse = 0; pulse < 2; pulse++) {
        int n = entities(pulse ? "pturret" : "turret", e, MAX_TURRETS - turretCount);
        if (n && !pulse) { turretBase = load("game", "RTurret_Base"); shotModel = load("game", "RTurret_Rail"); }
        if (n && pulse)  pturretBase = load("game", "RTurret_P_Base");
        for (int i = 0; i < n; i++, turretCount++) {
            Turret* t = &turrets[turretCount];
            *t = (Turret){ .cannon = load("game", pulse ? "RTurret_P_Cannon" : "RTurret_Cannon"), .pos = ent_pos(e[i]),
                           .yaw = ent_yaw(e[i]), .scale = e[i]->scale.x, .alive = true, .pulse = pulse, .cool = PTURRET_FIRST };
            t->aimYaw = t->yaw;
            dc_model_set_anim(t->cannon, dc_model_anim_index(t->cannon, "turret_fire"));
        }
    }
    if (droidCount || pturretBase) pulseModel = load("game", "projectile_pulse");
}

void enemies_free(void) {
    for (int i = 0; i < ratCount; i++) dc_model_free(rats[i].model);
    for (int i = 0; i < dozerCount; i++) dc_model_free(dozers[i].model);
    for (int i = 0; i < droidCount; i++) dc_model_free(droids[i].model);
    for (int i = 0; i < turretCount; i++) dc_model_free(turrets[i].cannon);
    dc_model_free(slimeModel);  dc_model_free(lavaSlimeModel);
    dc_model_free(turretBase);  dc_model_free(pturretBase);
    dc_model_free(shotModel);   dc_model_free(pulseModel);
    ratCount = dozerCount = droidCount = turretCount = 0;
}

/* Back as placed; a dead droid stays dead, so its switch stays on */
void enemies_reset(void) {
    memcpy(slimes, slimes0, sizeof(slimes));
    slimeCount = slimeCount0;
    memset(pulses, 0, sizeof(pulses));
    for (int i = 0; i < ratCount; i++) { rats[i].dead = false; rats[i].pos = rats[i].home; rats[i].state = RAT_IDLE; }
    for (int i = 0; i < dozerCount; i++) { dozers[i].pos = dozers[i].home; dozers[i].pause = dozers[i].reverse = 0.0f; }
    for (int i = 0; i < turretCount; i++) {
        Turret* t = &turrets[i];
        t->alive = true; t->firing = false; t->lock = t->shotLife = t->aimPitch = 0.0f; t->aimYaw = t->yaw; t->cool = PTURRET_FIRST;
    }
    for (int i = 0; i < droidCount; i++) {
        Droid* d = &droids[i];
        if (!d->alive) continue;
        d->pos = d->home; d->hp = DROID_HP; d->shooting = d->walking = false; d->idle = 1.0f; d->cool = 0.0f;
    }
}

/* ---- Pulses ---- */

static void pulse_fire(shz_vec3_t from, shz_vec3_t dir, bool homing) {
    float speed = homing ? HOMING_SPEED : PULSE_SPEED;
    target(from);
    for (int i = 0; i < MAX_PULSES; i++) {
        Pulse* p = &pulses[i];
        if (p->life > 0.0f) continue;
        *p = (Pulse){ .pos = from, .vel = shz_vec3_init(dir.x * speed, dir.y * speed, dir.z * speed),
                      .target = shz_vec3_init(bot->pos.x, bot->pos.y + 10.0f, bot->pos.z), .life = homing ? HOMING_LIFE : PULSE_LIFE, .homing = homing };
        sfx(SFX_ZAP);
        return;
    }
}

static void update_pulse(Pulse* p, float dt) {
    if (p->life <= 0.0f) return;
    p->life -= dt;
    if (p->homing) {                        /* bends toward where it thinks he is; walls stop it */
        float k = HOMING_LAG * dt;
        if (k > 1.0f) k = 1.0f;
        if (target(p->pos)) p->target = lerp3(p->target, shz_vec3_init(bot->pos.x, bot->pos.y + 10.0f, bot->pos.z), k);
        shz_vec3_t to = shz_vec3_init(p->target.x - p->pos.x, p->target.y - p->pos.y, p->target.z - p->pos.z);
        float tl = sqrtf(to.x * to.x + to.y * to.y + to.z * to.z);
        if (tl > 0.01f) {
            float b = HOMING_TURN * dt * HOMING_SPEED / tl;
            p->vel = shz_vec3_init(p->vel.x + to.x * b, p->vel.y + to.y * b, p->vel.z + to.z * b);
            float vl = sqrtf(p->vel.x * p->vel.x + p->vel.y * p->vel.y + p->vel.z * p->vel.z);
            if (vl > 0.01f) p->vel = shz_vec3_init(p->vel.x / vl * HOMING_SPEED, p->vel.y / vl * HOMING_SPEED, p->vel.z / vl * HOMING_SPEED);
        }
        if (col && col_raycast(col, p->pos, shz_vec3_init(p->vel.x / HOMING_SPEED, p->vel.y / HOMING_SPEED, p->vel.z / HOMING_SPEED), HOMING_SPEED * dt + 1.0f).hit) {
            p->life = 0.0f;
            fx_sparks(p->pos, 8);
            return;
        }
    }
    p->pos = shz_vec3_init(p->pos.x + p->vel.x * dt, p->pos.y + p->vel.y * dt, p->pos.z + p->vel.z * dt);
    if (target(p->pos) && dist3(p->pos, bot->pos) < 500.0f) fx_trail(p->pos, 1);   /* the wisps behind it, only where they can be seen */
    FOR_PLAYERS(i) {
        if (robot_dead() || p->life <= 0.0f) continue;
        float dx = p->pos.x - bot->pos.x, dz = p->pos.z - bot->pos.z;
        bool hit;
        if (p->homing) {
            float r = ROBOT_RADIUS + HOMING_HIT;
            hit = dx * dx + dz * dz < r * r && p->pos.y > bot->pos.y - HOMING_HIT && p->pos.y < bot->pos.y + ROBOT_HEIGHT + HOMING_HIT;
        } else {
            float dy = p->pos.y - (bot->pos.y + 10.0f);
            hit = dx * dx + dy * dy + dz * dz < PULSE_HIT_R * PULSE_HIT_R;
        }
        if (hit) { p->life = 0.0f; robot_hurt(1); fx_sparks(p->pos, 8); }
    }
}

/* ---- Each kind ---- */

static void update_rat(Rat* r, float dt) {
    if (r->dead) return;
    if (spin_hits(r->pos, 30.0f, &r->lastSpin)) {                              /* the spin kills a rat outright */
        r->dead = true; fx_oil(r->pos, 8); fx_hitstop(0.12f); return;
    }
    bool  alive = target(r->pos);
    float d = alive ? dist3(r->pos, bot->pos) : 1.0e9f;
    float dx = bot->pos.x - r->pos.x, dz = bot->pos.z - r->pos.z;
    if (r->cooldown > 0.0f) r->cooldown -= dt;
    switch (r->state) {
    case RAT_IDLE:
        if (d < RAT_AGGRO && alive) r->state = RAT_CHASE;
        dc_model_set_anim(r->model, r->aIdle);
        break;
    case RAT_CHASE: {
        if (d > RAT_DEAGGRO || !alive) { r->state = RAT_IDLE; break; }
        r->yaw = atan2f(-dx, dz);
        if (d < RAT_ATTACK) {
            if (r->cooldown <= 0.0f) {
                r->state = RAT_ATTACKING;
                r->timer = 0.0f;
                r->bitten = false;
                dc_model_set_anim(r->model, r->aAttack);
                dc_model_anim_restart(r->model);
            }
            break;
        }
        float h = sqrtf(dx * dx + dz * dz);
        if (h > 0.001f) {                   /* after him, through the walls, but not off a drop */
            float step = RAT_SPEED * dt;
            shz_vec3_t body = shz_vec3_init(r->pos.x, r->pos.y + RAT_BODY_Y, r->pos.z);
            shz_vec3_t to   = shz_vec3_init(r->pos.x + dx / h * step, body.y, r->pos.z + dz / h * step);
            shz_vec3_t out  = col ? col_move(col, body, to, RAT_RADIUS) : to;
            ColGroundHit g = col ? col_ground(col, shz_vec3_init(out.x, r->pos.y + STEP_UP, out.z), STEP_UP + 60.0f) : (ColGroundHit){ 0 };
            if (!col) { r->pos.x = out.x; r->pos.z = out.z; }
            else if (g.hit && r->pos.y - g.y <= RAT_STEP_DOWN) { r->pos.x = out.x; r->pos.z = out.z; r->pos.y = g.y; }
        }
        dc_model_set_anim(r->model, r->aRun);
        break;
    }
    case RAT_ATTACKING: {
        float len = dc_model_anim_length(r->model, r->aAttack);
        r->timer += dt;
        if (!r->bitten && r->timer >= len * 0.5f) {
            r->bitten = true;
            if (d < RAT_BITE && alive) robot_hurt(1);
        }
        if (dc_model_anim_done(r->model)) { r->state = RAT_CHASE; r->cooldown = RAT_COOLDOWN; }
        break;
    }
    }
    dc_model_animate(r->model, dt);
}

/* Tank-like: sits until he is near, then turns on the spot to face him and
 * drives at him. Stops short of a drop, backs up, tries again. Not solid;
 * touching it shoves him away and hurts. */
static void update_dozer(Dozer* d, float dt) {
    bool  alive = target(d->pos);
    float dx = bot->pos.x - d->pos.x, dz = bot->pos.z - d->pos.z;
    float dist = alive ? sqrtf(dx * dx + dz * dz) : 1.0e9f;
    float velX = 0.0f, velZ = 0.0f;
    bool  forward = false;
    if (d->pause > 0.0f) {
        d->pause -= dt;
        if (d->pause <= 0.0f) d->reverse = DOZER_REVERSE;
    } else if (d->reverse > 0.0f) {
        d->reverse -= dt;
        velX = sinf(d->yaw) * DOZER_SPEED * 0.5f;
        velZ = -cosf(d->yaw) * DOZER_SPEED * 0.5f;
    } else if (dist < DOZER_RANGE && alive) {
        float want = atan2f(-dx, dz);
        if (fabsf(wrap_angle(want - d->yaw)) > DOZER_AIM) turn_toward(&d->yaw, want, DOZER_TURN * dt);
        else { velX = -sinf(d->yaw) * DOZER_SPEED; velZ = cosf(d->yaw) * DOZER_SPEED; forward = true; }
    }
    if (!d->grounded) d->velY -= DOZER_GRAVITY * dt;
    if (forward && col) {                   /* a drop ahead: stop, then back off */
        float len = sqrtf(velX * velX + velZ * velZ);
        shz_vec3_t ahead = shz_vec3_init(d->pos.x + velX / len * DOZER_CLIFF_AHEAD, d->pos.y + 50.0f, d->pos.z + velZ / len * DOZER_CLIFF_AHEAD);
        ColGroundHit here = col_ground(col, shz_vec3_init(d->pos.x, d->pos.y + 50.0f, d->pos.z), 400.0f);
        ColGroundHit there = col_ground(col, ahead, 400.0f);
        if (here.hit && (!there.hit || here.y - there.y > DOZER_CLIFF_DROP)) { velX = velZ = 0.0f; d->pause = DOZER_CLIFF_PAUSE; }
    }
    d->grounded = move_body(&d->pos, velX, velZ, &d->velY, DOZER_RADIUS, 50.0f, dt);
    if (d->pos.y < -500.0f) { d->pos = d->home; d->velY = 0.0f; d->pause = 0.5f; d->reverse = 0.0f; }

    if (d->hurtCool > 0.0f) d->hurtCool -= dt;
    FOR_PLAYERS(i) {
        if (robot_dead() || dist3(bot->pos, d->pos) >= DOZER_TOUCH * d->scale) continue;
        float px = bot->pos.x - d->pos.x, pz = bot->pos.z - d->pos.z, len = sqrtf(px * px + pz * pz);
        if (len < 0.001f) len = 1.0f;
        bot->velX = px / len * DOZER_SHOVE;
        bot->velZ = pz / len * DOZER_SHOVE;
        if (d->hurtCool <= 0.0f) { robot_hurt(1); d->hurtCool = 0.5f; }
    }
    dc_model_animate(d->model, dt);
}

static void slime_split(const Slime* parent) {
    float start = rand_f() * TAU;
    for (int k = 0; k < 3; k++) {
        int j;
        for (j = 0; j < MAX_SLIMES && slimes[j].active; j++) ;
        if (j >= MAX_SLIMES) return;
        if (j >= slimeCount) slimeCount = j + 1;
        float a = start + k * TAU / 3.0f;
        slimes[j] = (Slime){ .active = true, .lava = parent->lava, .scale = SLIME_SMALL, .hp = 1, .stretch = 1.0f,
                             .pos = shz_vec3_init(parent->pos.x + cosf(a) * 25.0f, parent->pos.y + 15.0f, parent->pos.z + sinf(a) * 25.0f),
                             .velX = cosf(a) * 150.0f, .velY = 240.0f, .velZ = sinf(a) * 150.0f,
                             .jumpTimer = 0.5f + rand_f(), .invuln = 1.0f };
    }
}

static void slime_hit(Slime* s, float squash) {
    if (s->invuln > 0.0f) return;
    s->hp--;
    s->invuln = 0.3f;
    s->stretch = squash; s->stretchVel = -3.0f;
    fx_oil(s->pos, 15);
    if (s->hp <= 0) {
        s->dying = SLIME_DIE_TIME; fx_hitstop(0.12f);
        fx_splash(s->pos, 20, s->lava);
        fx_decal(s->pos, s->scale, s->lava);
    }
}

/* Two slimes of a kind on the ground, near enough, become one: the bigger
 * takes the smaller in, its size the root of the sum of their squares, its
 * health by its new size. A merged one waits a while before the next. */
static void slime_merge(Slime* a, Slime* b) {
    if (a->scale < b->scale) { Slime* t = a; a = b; b = t; }
    a->scale = sqrtf(a->scale * a->scale + b->scale * b->scale);
    a->pos = lerp3(a->pos, b->pos, 0.5f);
    a->hp = a->scale >= 1.5f ? 3 : a->scale >= 1.0f ? 2 : 1;
    a->mergeCool = SLIME_MERGE_COOL;
    a->stretch = 1.5f; a->stretchVel = 2.0f;
    b->active = false;
    fx_shake(0.5f);
}

static void slimes_merge(void) {
    for (int i = 0; i < slimeCount; i++) {
        Slime* a = &slimes[i];
        if (!a->active || !a->grounded || a->dying > 0.0f || a->mergeCool > 0.0f) continue;
        for (int j = i + 1; j < slimeCount; j++) {
            Slime* b = &slimes[j];
            if (!b->active || !b->grounded || b->dying > 0.0f || b->mergeCool > 0.0f || b->lava != a->lava) continue;
            if (dist_xz(a->pos, b->pos) < SLIME_MERGE_REACH * (a->scale + b->scale) * 0.5f) { slime_merge(a, b); break; }
        }
    }
}

/* Sits until he is near, then hops at him every few seconds with a squat
 * first. Landing on it from above squashes it; touching it otherwise hurts
 * and throws him back. A dead one of any size splits into three small
 * ones; small ones just go. */
static void update_slime(Slime* s, float dt) {
    if (!s->active) return;
    if (s->invuln > 0.0f) s->invuln -= dt;
    if (s->cool > 0.0f) s->cool -= dt;
    if (s->mergeCool > 0.0f) s->mergeCool -= dt;
    if (s->dying > 0.0f) {
        s->dying -= dt;
        if (s->dying <= 0.0f) {
            s->active = false;
            if (s->scale >= SLIME_SPLIT_AT) slime_split(s);
        }
        return;
    }
    bool  alive = target(s->pos);
    float dx = bot->pos.x - s->pos.x, dz = bot->pos.z - s->pos.z;
    float dist = alive ? sqrtf(dx * dx + dz * dz) : 1.0e9f;
    if (!s->aggro && dist < SLIME_AGGRO) s->aggro = true;
    if (s->aggro && dist > SLIME_DEAGGRO) s->aggro = false;

    if (s->grounded && s->aggro && alive) {
        s->yaw = atan2f(-dx, dz);
        if (s->windup > 0.0f) {
            s->windup -= dt;
            float p = 1.0f - s->windup / SLIME_WINDUP;
            s->stretch = 1.0f - p * p * 0.65f;
            if (s->windup <= 0.0f) {
                float speed = SLIME_SPEED / sqrtf(s->scale), len = dist > 0.001f ? dist : 1.0f;
                s->velX = dx / len * speed; s->velZ = dz / len * speed;
                s->velY = SLIME_JUMP_Y;
                s->grounded = false;
                s->jumpTimer = 2.0f + rand_f();
                s->stretch = 1.4f; s->stretchVel = 5.0f;
            }
        } else {
            s->jumpTimer -= dt;
            if (s->jumpTimer <= 0.0f) s->windup = SLIME_WINDUP;
        }
    }
    if (!s->grounded) {
        s->velY -= SLIME_GRAVITY * dt;
        float prevVelY = s->velY;
        if (move_body(&s->pos, s->velX, s->velZ, &s->velY, 8.0f, 20.0f, dt)) {
            s->grounded = true;
            s->velX = s->velZ = 0.0f;
            float impact = -prevVelY / 240.0f;
            if (impact > 1.0f) impact = 1.0f;
            if (impact > 0.0f) { s->stretch = 1.0f - 0.5f * impact; s->stretchVel = -2.0f * impact; }
            fx_splash(s->pos, 4 + (int)(impact * 4.0f), s->lava);
        }
        if (s->pos.y < -500.0f) { s->active = false; return; }
    }
    /* the squash spring */
    s->stretchVel += (-80.0f * (s->stretch - 1.0f) - 4.5f * s->stretchVel) * dt;
    s->stretch += s->stretchVel * dt;
    if (s->stretch < 0.25f) s->stretch = 0.25f;
    if (s->stretch > 1.8f) s->stretch = 1.8f;

    /* each player: his spin, or landing on it, or walking into it */
    FOR_PLAYERS(i) {
        if (robot_dead() || s->dying > 0.0f) continue;
        float d = dist_xz(bot->pos, s->pos);
        float above = bot->pos.y - s->pos.y;
        int id = bot->spinId * MAX_PLAYERS + i;
        if (bot->spinning && s->lastSpin != id && d < SPIN_REACH && fabsf(above + 5.0f - 2.0f * s->scale) < 10.0f + 5.0f * s->scale) {
            s->lastSpin = id;
            slime_hit(s, 0.6f);
            continue;
        }
        if (d > SLIME_TOUCH) continue;
        if (above > SLIME_STOMP_LO && above < SLIME_STOMP_HI && bot->velY <= 0.0f && !bot->grounded) {
            slime_hit(s, 0.5f);
            bot->velY = SLIME_BOUNCE;
            robot_airborne();
        } else if (above > -SLIME_HEIGHT && above < SLIME_HEIGHT && s->cool <= 0.0f) {
            s->cool = SLIME_COOLDOWN;
            s->stretch = 0.5f; s->stretchVel = -1.5f;
            robot_hurt(1);
            robot_knockback(s->pos, SLIME_KNOCK, SLIME_KNOCK_UP);
        }
    }
}

static void droid_anim(Droid* d, int a) {
    if (a < 0 || d->anim == a) return;
    d->anim = a;
    dc_model_set_anim(d->model, a);
}

/* Walks about, shoots pulses at him from a distance, blocks up close, and
 * only the spin while it is shooting hurts it. Solid to him. */
static void update_droid(Droid* d, float dt) {
    if (!d->alive) return;
    dc_model_animate(d->model, dt);
    if (d->cool > 0.0f) d->cool -= dt;
    float fx = -sinf(d->yaw), fz = cosf(d->yaw);
    bool  alive = target(d->pos);
    float dist = alive ? dist_xz(bot->pos, d->pos) : 1.0e9f;
    bool aggro = dist < DROID_RANGE && alive;
    bool edge = !col || !world_ground(shz_vec3_init(d->pos.x + fx * DROID_AHEAD, d->pos.y + STEP_UP, d->pos.z + fz * DROID_AHEAD), STEP_UP + DROID_DROP).hit;

    if (d->shooting && spin_hits(d->pos, 30.0f, &d->lastSpin)) {
        d->hp--;
        d->shooting = false;
        d->cool = DROID_COOL;
        droid_anim(d, d->aIdle);
        fx_hitstop(0.12f);
        shz_vec3_t mid = shz_vec3_init(d->pos.x, d->pos.y + 10.0f, d->pos.z);
        if (d->hp <= 0) { d->alive = false; ids[d->id] = true; fx_oil(mid, 12); return; }
        fx_oil(mid, 4);
    }

    bool walk = false;
    if (d->shooting) {
        float was = d->shootTime;
        d->shootTime += dt;
        if (was < DROID_SHOT_AT && d->shootTime >= DROID_SHOT_AT) {
            float rx = cosf(d->yaw), rz = sinf(d->yaw);
            shz_vec3_t from = shz_vec3_init(d->pos.x + fx * 15.0f - rx * 8.0f, d->pos.y + 20.0f, d->pos.z + fz * 15.0f - rz * 8.0f);
            shz_vec3_t aim  = shz_vec3_init(bot->pos.x - from.x, bot->pos.y + ROBOT_MIDDLE - from.y, bot->pos.z - from.z);
            float len = sqrtf(aim.x * aim.x + aim.y * aim.y + aim.z * aim.z);
            if (len < 0.01f) len = 1.0f;
            pulse_fire(from, shz_vec3_init(aim.x / len, aim.y / len, aim.z / len), false);
        }
        if (dc_model_anim_done(d->model)) {
            d->shooting = false;
            d->cool = edge && aggro ? DROID_COOL_EDGE : DROID_COOL;
        }
    } else if (aggro) {
        float diff = turn_toward(&d->yaw, atan2f(-(bot->pos.x - d->pos.x), bot->pos.z - d->pos.z), DROID_TURN * dt);
        if (d->cool <= 0.0f && fabsf(diff) < DROID_FACING) { d->shooting = true; d->shootTime = 0.0f; d->anim = -1; droid_anim(d, d->aShoot); dc_model_anim_restart(d->model); }
        else if (dist < DROID_BLOCK) droid_anim(d, d->aBlock);
        else if (!edge && d->grounded) { walk = true; droid_anim(d, d->aWalk); }
        else droid_anim(d, d->aIdle);
    } else if (d->walking) {
        if (edge || !d->grounded || rand_f() < DROID_STOP * dt) { d->walking = false; d->idle = 1.0f + rand_f(); droid_anim(d, d->aIdle); }
        else walk = true;
    } else {
        d->idle -= dt;
        if (d->idle <= 0.0f) { d->walking = true; d->yaw = rand_f() * TAU; droid_anim(d, d->aWalk); }
        else droid_anim(d, d->aIdle);
    }

    fx = -sinf(d->yaw); fz = cosf(d->yaw);
    float vx = walk ? fx * DROID_WALK : 0.0f, vz = walk ? fz * DROID_WALK : 0.0f;
    if (!d->grounded) d->velY -= FALL_ACCEL * dt;
    d->grounded = move_body(&d->pos, vx, vz, &d->velY, DROID_RADIUS, STEP_UP, dt);
    if (d->pos.y < -500.0f) { d->pos = d->home; d->velY = 0.0f; d->walking = false; d->idle = 1.0f; }

    FOR_PLAYERS(i) {                        /* solid to each of them */
        if (robot_dead() || bot->pos.y >= d->pos.y + DROID_HEIGHT || bot->pos.y + ROBOT_HEIGHT <= d->pos.y) continue;
        float dx = bot->pos.x - d->pos.x, dz = bot->pos.z - d->pos.z, len = sqrtf(dx * dx + dz * dz);
        float want = DROID_RADIUS + ROBOT_RADIUS;
        if (len < want) {
            if (len < 0.01f) { dx = 1.0f; dz = 0.0f; len = 1.0f; }
            bot->pos.x = d->pos.x + dx / len * want;
            bot->pos.z = d->pos.z + dz / len * want;
        }
    }
}

/* Turns to face him once he is near. The rail turret takes a couple of
 * seconds to lock on and fires when on target: the shot is drawn flying,
 * and it hits if he is still in the line a moment later. The pulse turret
 * lobs a homing pulse every few seconds with a clear line. Either dies to
 * the spin. */
static void update_turret(Turret* t, float dt) {
    if (!t->alive) return;
    bool pulse = t->pulse;
    dc_model_animate(t->cannon, t->firing ? dt : 0.0f);
    shz_vec3_t cannon = shz_vec3_init(t->pos.x, t->pos.y + (pulse ? PTURRET_CANNON_Y : TURRET_CANNON_Y) * t->scale, t->pos.z);
    bool  alive = target(cannon);
    float dx = bot->pos.x - cannon.x, dy = bot->pos.y + ROBOT_MIDDLE - cannon.y, dz = bot->pos.z - cannon.z;
    float flat = alive ? sqrtf(dx * dx + dz * dz) : 1.0e9f, len = sqrtf(flat * flat + dy * dy);
    if (spin_hits(t->pos, 40.0f, &t->lastSpin)) {
        t->alive = false;
        fx_hitstop(0.12f);
        for (int i = 0; i < 3; i++) fx_oil(shz_vec3_init(t->pos.x, t->pos.y + 5.0f + 15.0f * i, t->pos.z), i == 1 ? 20 : 15);
        return;
    }
    if (t->cool > 0.0f) t->cool -= dt;
    if (t->shotLife > 0.0f) {
        t->shotLife -= dt;
        t->shotPos = shz_vec3_init(t->shotPos.x + t->shotDir.x * TURRET_SHOT_SPEED * dt, t->shotPos.y + t->shotDir.y * TURRET_SHOT_SPEED * dt,
                                   t->shotPos.z + t->shotDir.z * TURRET_SHOT_SPEED * dt);
    }
    if (t->firing) {
        float was = t->fireTime;
        t->fireTime += dt;
        if (!pulse && was < TURRET_SHOT_AT && t->fireTime >= TURRET_SHOT_AT) {
            t->shotPos = cannon; t->shotLife = TURRET_SHOT_LIFE; sfx(SFX_TURRET_FIRE);
            float m = 50.0f * t->scale;     /* the muzzle flash, out along the barrel */
            fx_sparks(shz_vec3_init(cannon.x + t->shotDir.x * m, cannon.y + t->shotDir.y * m, cannon.z + t->shotDir.z * m), 12);
        }
        if (!pulse && was < TURRET_HIT_AT && t->fireTime >= TURRET_HIT_AT) FOR_PLAYERS(i) {
            if (robot_dead()) continue;
            float hx = bot->pos.x - cannon.x, hy = bot->pos.y + ROBOT_MIDDLE - cannon.y, hz = bot->pos.z - cannon.z;
            float along = hx * t->shotDir.x + hy * t->shotDir.y + hz * t->shotDir.z;   /* how far he is from the locked line */
            float px = hx - t->shotDir.x * along, py = hy - t->shotDir.y * along, pz = hz - t->shotDir.z * along;
            if (along > 0.0f && px * px + py * py + pz * pz < TURRET_HIT_R * TURRET_HIT_R && !(col && col_raycast(col, cannon, t->shotDir, along).hit))
                robot_hurt(1);
        }
        if (dc_model_anim_done(t->cannon)) { t->firing = false; t->lock = -TURRET_RECOVER; }
        if (!pulse) return;
    }
    bool near = flat < (pulse ? PTURRET_RANGE : TURRET_RANGE) && alive;
    if (!near) {                            /* the lock wears off */
        if (t->lock != 0.0f) { t->lock += t->lock > 0.0f ? -dt : dt; if (fabsf(t->lock) < dt) t->lock = 0.0f; }
        return;
    }
    float wantPitch = atan2f(dy, flat);
    if (wantPitch < TURRET_PITCH_LO) wantPitch = TURRET_PITCH_LO;
    if (wantPitch > TURRET_PITCH_HI) wantPitch = TURRET_PITCH_HI;
    float step = (pulse ? PTURRET_TURN : TURRET_TURN) * dt;
    float d  = turn_toward(&t->aimYaw, atan2f(-dx, dz), step);
    float dp = turn_toward(&t->aimPitch, wantPitch, step);
    bool sees = !col || !col_raycast(col, cannon, shz_vec3_init(dx / len, dy / len, dz / len), len).hit;
    float cp = cosf(t->aimPitch);
    shz_vec3_t aim = shz_vec3_init(-sinf(t->aimYaw) * cp, sinf(t->aimPitch), cosf(t->aimYaw) * cp);
    bool fire = false;
    if (pulse) {
        if (t->cool > 0.0f || fabsf(d) > 0.3f || !sees) return;
        pulse_fire(cannon, aim, true);
        t->cool = PTURRET_COOL;
        fire = true;
    } else if (sees) {
        if (t->lock < TURRET_LOCK) t->lock += dt;
        else if (fabsf(d) < TURRET_AIM && fabsf(dp) < TURRET_AIM) { t->shotDir = aim; fire = true; }
    } else if (t->lock > 0.0f) t->lock -= 0.5f * dt;
    else if (t->lock < 0.0f) t->lock += dt;
    if (fire) {
        t->firing = true; t->fireTime = 0.0f;
        dc_model_set_anim(t->cannon, dc_model_anim_index(t->cannon, "turret_fire"));
        dc_model_anim_restart(t->cannon);
    }
}

void enemies_update(float dt) {
    for (int i = 0; i < ratCount; i++) update_rat(&rats[i], dt);
    for (int i = 0; i < dozerCount; i++) update_dozer(&dozers[i], dt);
    for (int i = 0; i < slimeCount; i++) update_slime(&slimes[i], dt);
    slimes_merge();
    for (int i = 0; i < droidCount; i++) update_droid(&droids[i], dt);
    for (int i = 0; i < turretCount; i++) update_turret(&turrets[i], dt);
    for (int i = 0; i < MAX_PULSES; i++) update_pulse(&pulses[i], dt);
}

/* Pointing along yaw, tilted up by pitch: 3 columns */
static void aim_rot(float yaw, float pitch, float rot[9]) {
    float c = cosf(pitch), sn = sinf(pitch), cy = cosf(yaw), sy = sinf(yaw);
    rot[0] = cy;        rot[1] = 0.0f; rot[2] = sy;
    rot[3] = sn * sy;   rot[4] = c;    rot[5] = -sn * cy;
    rot[6] = -c * sy;   rot[7] = sn;   rot[8] = c * cy;
}

void enemies_draw(void) {
    for (int i = 0; i < ratCount; i++) if (!rats[i].dead) draw_at(rats[i].model, rats[i].pos, rats[i].scale, rats[i].yaw);
    for (int i = 0; i < dozerCount; i++) draw_at(dozers[i].model, dozers[i].pos, dozers[i].scale, dozers[i].yaw);
    for (int i = 0; i < slimeCount; i++) {
        const Slime* s = &slimes[i];
        DMSModel* m = s->lava ? lavaSlimeModel : slimeModel;
        if (!s->active || !m) continue;
        float sy = s->stretch, sxz = 1.0f + (1.0f - sy) * 0.5f;
        if (s->dying > 0.0f) {
            float p = 1.0f - s->dying / SLIME_DIE_TIME;
            sy = (1.0f - p) * 0.3f + 0.01f;
            sxz = (1.0f + 0.3f * p) * (1.0f - 0.8f * p);
        }
        dc_draw_ex(m, &(DCDrawOpts){ .pos = shz_vec3_init(s->pos.x, s->pos.y + SLIME_BOTTOM * s->scale * sy, s->pos.z),
                   .scale = WORLD_SCALE * s->scale, .stretch = { .x = sxz, .y = sy, .z = sxz }, .yaw = s->yaw });
    }
    for (int i = 0; i < droidCount; i++) if (droids[i].alive) draw_at(droids[i].model, droids[i].pos, 1.0f, droids[i].yaw);
    for (int i = 0; i < turretCount; i++) {
        const Turret* t = &turrets[i];
        if (!t->alive) continue;
        draw_at(t->pulse ? pturretBase : turretBase, t->pos, t->scale, t->yaw);
        shz_vec3_t cannon = shz_vec3_init(t->pos.x, t->pos.y + (t->pulse ? PTURRET_CANNON_Y : TURRET_CANNON_Y) * t->scale, t->pos.z);
        draw_at(t->cannon, cannon, t->scale, t->aimYaw);
        if (t->shotLife > 0.0f && shotModel) {
            float rot[9];
            aim_rot(t->aimYaw, t->aimPitch, rot);
            dc_draw_ex(shotModel, &(DCDrawOpts){ .pos = t->shotPos, .scale = WORLD_SCALE * t->scale * 1.5f, .rot = rot });
        }
    }
    for (int i = 0; i < MAX_PULSES; i++) {  /* a pulse grows as it goes; a homing one throbs and shrinks away at the end */
        const Pulse* p = &pulses[i];
        if (p->life <= 0.0f) continue;
        float age = (p->homing ? HOMING_LIFE : PULSE_LIFE) - p->life, grow = p->homing ? 3.0f : PULSE_GROW;
        float sc = age >= grow ? 1.0f : 0.2f + 0.8f * age / grow;
        if (p->homing) {
            float shrink = p->life < 2.0f ? (p->life > 0.2f ? p->life / 2.0f : 0.1f) : 1.0f;
            sc *= 2.0f * (1.0f + 0.25f * sinf(age * 6.0f * TAU)) * shrink;
        }
        draw_at(pulseModel, p->pos, sc, 0.0f);
    }
}
