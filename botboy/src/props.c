/* The things placed in a level, from the Empties in its glb. A name is a
 * kind: "bolt", "pad", "fan"... Some carry a number: "button2" works the
 * switch 2, "coal2" and "laser2" wait on it ("coal" is 1, "coal0" runs from
 * the start); "rock60", "hive40", "mlaser80" move at that speed. A mover's
 * "..._to" Empties are where it goes, in order. Also "spawn", "exit",
 * "checkpoint", "screw" and the hub's "door1".."door6". */
#include "sound.h"
#include "level.h"
#include "demo.h"
#include "save.h"

#define MAX_PROPS       96
#define MAX_MOVERS      24
#define MAX_BOLTS       8
#define MAX_DOORS       8
#define BOLT_SPIN       3.0f
#define BOLT_PULL_RANGE 80.0f
#define BOLT_PULL_SPEED 150.0f
#define BOLT_TAKE_RANGE 20.0f
#define SCREW_SPIN      4.0f
#define SCREW_PULL_RANGE 100.0f
#define SCREW_PULL_SPEED 180.0f
#define PAD_RANGE       20.0f           /* times its scale */
#define BUTTON_RANGE    (0.63f * WORLD_SCALE)
#define BUTTON_TOP      (0.23f * WORLD_SCALE)
#define BUTTON_SINK     (0.08f * WORLD_SCALE)
#define EXIT_RANGE      40.0f
#define CHECKPOINT_HALF 32.0f           /* a box Empty, times its scale */
#define CHECKPOINT_UP   81.8f           /* he comes back this far up it, times its y scale */
#define ROCK_DROP_MULT  3.0f            /* a rock falls three times as fast as it travels */
#define HIVE_RETURN     1.0f            /* seconds after he leaves before it goes back */
#define BELT_PUSH       30.0f           /* units a second along the belt */
#define COG_SPIN_RANGE  700.0f          /* the cog only turns with him this close */
#define SINK_SPEED      30.0f
#define SWING_AMP       0.2618f         /* radians either way */
#define SWING_RATE      1.5f
#define FAN_REACH       70.0f           /* times its x scale */
#define FAN_TOP         10.0f           /* times its y scale: the stream starts here */
#define FAN_BELOW       30.0f           /* and reaches this far under it and this far over */
#define FAN_ABOVE       80.0f
#define FAN_EASE        30.0f           /* eases off this close to the top */
#define FAN_LIFT        (50.0f * 30.0f) /* times its y scale */
#define FAN_LIFT_RATE   (0.2f * 30.0f)  /* of the lift, a second */

/* A model's box, from dc_model_bounds(), in its own units (metres): times
 * WORLD_SCALE and the Empty's scale for the world. What he stands on, what
 * hurts him, all read off the model rather than typed in. */
typedef struct { float top, bottom, hx, hy, hz, cx, cy, cz; } Box;
static Box box_of(const DMSModel* m) {
    DCBounds b = dc_model_bounds(m, NULL);
    return (Box){ .top = b.max.y, .bottom = b.min.y,
                  .hx = (b.max.x - b.min.x) * 0.5f, .hy = (b.max.y - b.min.y) * 0.5f, .hz = (b.max.z - b.min.z) * 0.5f,
                  .cx = (b.max.x + b.min.x) * 0.5f, .cy = (b.max.y + b.min.y) * 0.5f, .cz = (b.max.z + b.min.z) * 0.5f };
}
static float box_radius(const Box* b) { return b->hx < b->hz ? b->hx : b->hz; }   /* a disc: the tighter of the two */

/* ---- Props: things that stay where they are put ---- */

typedef enum { P_PAD, P_BUTTON, P_BELT, P_PIPE, P_FAN, P_HANG, P_GRINDER, P_LAVAFLOOR, P_LAVAFALL, P_LAVARIVER,
               P_FLOATROCK, P_FLOATROCK2, P_SPINROCK, P_SPINROCK2, P_SINKPLAT, P_SPINNER, P_SWING, P_LASER,
               P_STREAM, P_STAGE, P_COG, P_KINDS } PropKind;
typedef struct {
    const char *name, *file, *file2;    /* the Empty, its model, a second model drawn with it */
    bool  byId, solid, wheel;           /* numbered by switch; has walls; turns about its own z */
    bool  stand, disc, emptyTop;        /* he stands on its top; the top is a disc, not a box; the top
                                         * is at the Empty (a platform on a chain: the box is the chain too) */
    float sink, spin;                   /* sinks this far under him; turns this fast */
    const char* scroll;                 /* a texture that crawls, on the last model */
    float scrollV;
    const char* anim;
} PropDef;
static const PropDef PROPS[P_KINDS] = {
    [P_PAD]        = { "pad", "Chargepad" },
    [P_BUTTON]     = { "button", "RoundButtonBottom", "RoundButtonTop", .byId = true },
    [P_BELT]       = { "conveyor", "ConveyerLargeFrame", "ConveyerLargeBelt", .stand = true, .scroll = "Belt", .scrollV = -1.5f },
    [P_PIPE]       = { "pipe", "Toxic_Level2_Pipe", "Toxic_Level2_Running", .solid = true, .scroll = "FX_Stream", .scrollV = -0.5f },
    [P_FAN]        = { "fan", "DECO_FAN2", .solid = true, .anim = "gust" },
    [P_HANG]       = { "hang", "Cliff_Hanging_Platform_L", .solid = true },
    [P_GRINDER]    = { "grinder", "DECO_GRINDER", .solid = true, .wheel = true, .spin = 2.4f },
    [P_LAVAFLOOR]  = { "lavafloor", "DECO_LAVAFLOOR", .scroll = "env_lava", .scrollV = -0.075f },
    [P_LAVAFALL]   = { "lavafall", "DECO_LAVAFALLS", .scroll = "FX_LavaFall", .scrollV = 0.2f },
    [P_LAVARIVER]  = { "lavariver", "DECO_LAVA_RIVER" },
    [P_FLOATROCK]  = { "floatrock", "DECO_FLOATING_ROCK", .stand = true, .disc = true, .sink = 200.0f },
    [P_FLOATROCK2] = { "floatrock2", "DECO_FLOATING_ROCK_2", .stand = true, .disc = true, .sink = 200.0f },
    [P_SPINROCK]   = { "spinrock", "DECO_SPIN_ROCK", .stand = true, .disc = true, .sink = 200.0f, .spin = 0.5f },
    [P_SPINROCK2]  = { "spinrock2", "DECO_SPIN_ROCK_2", .stand = true, .disc = true, .sink = 200.0f, .spin = 0.5f },
    [P_SINKPLAT]   = { "sinkplat", "DECO_SINK_PLAT", .stand = true, .sink = 200.0f },
    [P_SPINNER]    = { "spinner", "DECO_BEYBLADE", .stand = true, .disc = true, .spin = 2.0f },
    [P_SWING]      = { "hangs", "Cliff_Hanging_Platform_S", .stand = true, .emptyTop = true },
    [P_LASER]      = { "laser", "LaserWall", "LaserWallOff", .byId = true },
    [P_STREAM]     = { "stream", "DECO_LVL3_STREAM", .scroll = "FX_Stream_F64", .scrollV = 0.08f },
    [P_STAGE]      = { "stage7", "DECO_STAGE7" },
    [P_COG]        = { "cog", "cog", .wheel = true, .stand = true, .spin = 0.8f },
};
static Box boxes[P_KINDS];              /* of the model he stands on or is hurt by, once it is loaded */
typedef struct {
    PropKind   kind;
    shz_vec3_t pos, scale;
    float      yaw, spin, sunk, phase;
    int        id;
    bool       on;                      /* a button: someone stands on it now */
    bool       pressed;                 /* a button: someone did this frame (while the players are looked at) */
    ColWorld*  col;
} Prop;

/* ---- Movers: things that go between their Empty and its "_to"s ---- */

/* A coal cart or a platform waits for its switch, then goes to and fro. A
 * rock loops round its three and drops fast. A hive goes while he stands on
 * it and drifts back after. A moving laser goes to and fro and hurts. */
typedef enum { M_COAL, M_PLAT, M_ROCK, M_HIVE, M_LASER, M_KINDS } MoverKind;
typedef struct {
    const char *name, *file;
    bool  byId, loop, hurts;            /* hurts: touching its box takes a heart, and it cannot be stood on */
    int   tos;                          /* "_to"s each */
    float scale, speed;                 /* 0: the Empty's scale, the speed in the name */
} MoverDef;
static const MoverDef MOVERS[M_KINDS] = {
    [M_COAL]  = { "coal", "DECO_MOVE_COAL", .byId = true, .tos = 1, .scale = 0.5f, .speed = 60.0f },
    [M_PLAT]  = { "plat", "DECO_MOVE_PLAT", .byId = true, .tos = 1, .scale = 0.5f, .speed = 60.0f },
    [M_ROCK]  = { "rock", "DECO_MOVING_ROCK", .loop = true, .tos = 3 },
    [M_HIVE]  = { "hive", "DECO_HIVE_MOVING", .tos = 1 },
    [M_LASER] = { "mlaser", "DECO_MOVING_LASER", .tos = 1, .hurts = true },
};
static Box moverBoxes[M_KINDS];
typedef struct {
    MoverKind  kind;
    shz_vec3_t wp[4], pos, scale;
    int        count, target, dir, id;
    float      yaw, speed, t, off;      /* off: seconds since he left a hive */
    bool       go;
} Mover;

typedef struct { shz_vec3_t pos; float spin; bool taken; } Pickup;
typedef struct { shz_vec3_t pos; float half; int level; } Door;

int  boltCount, boltsTaken;
bool screwTaken;
static DMSModel*  models[P_KINDS][2];
static DMSModel*  moverModels[M_KINDS];
static DMSModel  *boltModel, *screwModel;
static DCImage   *boltShine;            /* steel: what the bolts reflect; the level's gold is the screw's */
static Prop       props[MAX_PROPS];
static Mover      movers[MAX_MOVERS];
static Pickup     bolts[MAX_BOLTS], screw;
static Door       doors[MAX_DOORS];
static int        propCount, moverCount, doorCount;
static bool       haveScrew, haveExit, haveCheckpoint, haveCheckpointBox;
static shz_vec3_t spawn, exitPos, checkpoint, checkpointBox, checkpointHalf;
static float      checkpointUp;

/* The level and the solid props together */
ColGroundHit world_ground(shz_vec3_t origin, float reach) {
    ColGroundHit best = { 0 };
    if (col) best = col_ground(col, origin, reach);
    for (int i = 0; i < propCount; i++) {
        if (!props[i].col) continue;
        ColGroundHit g = col_ground(props[i].col, origin, reach);
        if (g.hit && (!best.hit || g.y > best.y)) best = g;
    }
    return best;
}

shz_vec3_t world_move(shz_vec3_t from, shz_vec3_t to, float radius) {
    shz_vec3_t out = col ? col_move(col, from, to, radius) : to;
    for (int i = 0; i < propCount; i++)
        if (props[i].col) out = col_move(props[i].col, from, out, radius);
    return out;
}

/* A swing's yaw swings; a turning disc's turns */
static float prop_yaw(const Prop* p) {
    if (p->kind == P_SWING) return p->yaw + sinf(p->phase) * SWING_AMP;
    return PROPS[p->kind].wheel ? p->yaw : p->yaw + p->spin;
}

static bool riding_prop(int i)  { return bot->rideKind == SURF_PROP && bot->riding == i; }
static bool riding_mover(int i) { return bot->rideKind == SURF_MOVER && bot->riding == i; }

/* ---- Reading the Empties ---- */

/* The distinct numbers after a prefix in the Empties' names: "rock60",
 * "rock40" ("_to"s skipped) */
static int name_numbers(const char* prefix, int* out, int max) {
    int len = strlen(prefix), count = 0;
    for (uint32_t i = 0; i < level->entity_count; i++) {
        const char* nm = level->entities[i].name;
        if (strncmp(nm, prefix, len) || nm[len] < '0' || nm[len] > '9' || strstr(nm, "_to")) continue;
        int num = atoi(nm + len), seen = 0;
        for (int k = 0; k < count; k++) if (out[k] == num) seen = 1;
        if (!seen && count < max) out[count++] = num;
    }
    return count;
}

static int entities(const char* name, const DMSEntity** out, int max) {
    int n = dc_model_entities(level, name, out, max);
    return n > max ? max : n;
}

/* The Empties of a numbered thing, "rock60_to" say. By switch, "coal" and
 * "coal1" are both switch 1 */
static int numbered(const char* prefix, int num, bool byId, const char* suffix, const DMSEntity** out, int max) {
    char name[32];
    int n = 0;
    if (byId || num) {                  /* a plain kind has no number in its name */
        snprintf(name, sizeof(name), "%s%d%s", prefix, num, suffix);
        n = entities(name, out, max);
    }
    if (byId ? num == 1 : num == 0) {   /* the plain name: the kind itself, or switch 1 */
        snprintf(name, sizeof(name), "%s%s", prefix, suffix);
        n += entities(name, out + n, max - n);
    }
    return n;
}

static void props_add(PropKind k) {
    const PropDef* d = &PROPS[k];
    const DMSEntity* e[MAX_PROPS];
    int first = d->byId ? 1 : 0, last = d->byId ? MAX_IDS : 1, ofKind = 0;
    for (int id = first; id < last; id++) {
        int n = numbered(d->name, id, d->byId, "", e, MAX_PROPS - propCount);
        if (n && !models[k][0]) {
            models[k][0] = load("game", d->file);
            if (d->file2) models[k][1] = load("game", d->file2);
            DMSModel* crawl = models[k][d->file2 ? 1 : 0];
            if (d->scroll && crawl) dc_model_scroll(crawl, d->scroll, 0.0f, d->scrollV);
            if (d->anim && models[k][0]) dc_model_set_anim(models[k][0], dc_model_anim_index(models[k][0], d->anim));
            /* The box is the second model's when that is what he stands on (the belt, not its frame) */
            boxes[k] = box_of(d->stand && models[k][1] ? models[k][1] : models[k][0]);
            if (d->emptyTop) boxes[k].top = 0.0f;
        }
        for (int i = 0; i < n; i++, ofKind++) {
            Prop* p = &props[propCount++];
            *p = (Prop){ .kind = k, .pos = ent_pos(e[i]), .scale = e[i]->scale, .yaw = ent_yaw(e[i]), .id = id, .phase = ofKind * 1.7f };
            if (d->solid && models[k][0]) p->col = col_build_rotated(models[k][0], p->pos, WORLD_SCALE * p->scale.x, p->yaw);
        }
    }
}

static void movers_add(MoverKind k) {
    const MoverDef* d = &MOVERS[k];
    const DMSEntity *e[MAX_MOVERS], *to[MAX_MOVERS * 3];
    int nums[8], n = d->byId ? MAX_IDS : name_numbers(d->name, nums, 8);
    for (int ni = 0; ni < n; ni++) {
        int num = d->byId ? ni : nums[ni];
        int have = numbered(d->name, num, d->byId, "", e, MAX_MOVERS - moverCount);
        int tos  = numbered(d->name, num, d->byId, "_to", to, MAX_MOVERS * 3);
        if (have && !moverModels[k]) { moverModels[k] = load("game", d->file); moverBoxes[k] = box_of(moverModels[k]); }
        for (int i = 0; i < have; i++) {
            Mover* m = &movers[moverCount++];
            *m = (Mover){ .kind = k, .yaw = ent_yaw(e[i]), .count = 1, .target = 1, .dir = 1,
                          .id = d->byId ? num : 0, .go = !d->byId || num == 0,
                          .speed = d->speed > 0.0f ? d->speed : (float)num,
                          .scale = d->scale > 0.0f ? shz_vec3_init(d->scale, d->scale, d->scale) : e[i]->scale };
            m->wp[0] = ent_pos(e[i]);
            for (int j = 0; j < d->tos && i * d->tos + j < tos; j++) m->wp[m->count++] = ent_pos(to[i * d->tos + j]);
            m->pos = m->wp[0];
        }
    }
}

void props_init(void) {
    const DMSEntity* e[MAX_BOLTS];
    propCount = moverCount = doorCount = boltCount = boltsTaken = 0;
    haveCheckpoint = false;
    screwTaken = false;
    memset(models, 0, sizeof(models));
    memset(moverModels, 0, sizeof(moverModels));
    boltModel = screwModel = NULL;
    spawn = shz_vec3_init(0.0f, 0.0f, 0.0f);
    if (!level) return;

    if (entities("spawn", e, 1)) spawn = ent_pos(e[0]);
    haveExit = entities("exit", e, 1) > 0;
    if (haveExit) exitPos = ent_pos(e[0]);
    haveCheckpointBox = entities("checkpoint", e, 1) > 0;
    if (haveCheckpointBox) {
        checkpointBox  = ent_pos(e[0]);
        checkpointHalf = shz_vec3_init(CHECKPOINT_HALF * e[0]->scale.x, CHECKPOINT_HALF * e[0]->scale.y, CHECKPOINT_HALF * e[0]->scale.z);
        checkpointUp   = CHECKPOINT_UP * e[0]->scale.y;
    }
    for (int lv = 1; lv <= 6; lv++) {   /* the hub's doors: the number is the level */
        char name[8];
        snprintf(name, sizeof(name), "door%d", lv);
        int n = entities(name, e, MAX_DOORS - doorCount);
        for (int i = 0; i < n; i++) doors[doorCount++] = (Door){ .pos = ent_pos(e[i]), .half = WORLD_SCALE * e[i]->scale.x, .level = lv };
    }
    boltCount = entities("bolt", e, MAX_BOLTS);
    for (int i = 0; i < boltCount; i++) bolts[i] = (Pickup){ .pos = ent_pos(e[i]) };
    if (boltCount) { boltModel = load("game", "Screw"); boltShine = dc_image_load(ASSETS "game/env_silver.dt"); }
    haveScrew = entities("screw", e, 1) > 0;
    if (haveScrew) { screw = (Pickup){ .pos = ent_pos(e[0]) }; screwModel = load("game", "DECO_SCREWG"); }

    for (int k = 0; k < P_KINDS; k++) props_add(k);
    for (int k = 0; k < M_KINDS; k++) movers_add(k);
}

void props_free(void) {
    for (int i = 0; i < propCount; i++) if (props[i].col) col_free(props[i].col);
    for (int k = 0; k < P_KINDS; k++) { dc_model_free(models[k][0]); dc_model_free(models[k][1]); }
    for (int k = 0; k < M_KINDS; k++) dc_model_free(moverModels[k]);
    dc_model_free(boltModel); dc_model_free(screwModel);
    dc_image_free(boltShine); boltShine = NULL;
    propCount = moverCount = 0;
}

/* The switches off, the movers home, the sunk ones up */
void props_reset(void) {
    for (int i = 0; i < moverCount; i++) {
        Mover* m = &movers[i];
        m->go = !MOVERS[m->kind].byId || m->id == 0;
        m->t = m->off = 0.0f; m->dir = 1; m->target = 1; m->pos = m->wp[0];
    }
    for (int i = 0; i < propCount; i++) {
        props[i].on = false;
        props[i].pos.y += props[i].sunk; props[i].sunk = 0.0f;
    }
}

shz_vec3_t props_spawn(void) { return haveCheckpoint ? checkpoint : spawn; }
bool       props_exit(shz_vec3_t* at) { if (haveExit) *at = exitPos; return haveExit; }

int door_touched(void) {
    for (int i = 0; i < doorCount; i++) {
        const Door* d = &doors[i];
        float reach = d->half + ROBOT_RADIUS;
        if (fabsf(bot->pos.x - d->pos.x) > reach || fabsf(bot->pos.z - d->pos.z) > reach) continue;
        if (bot->pos.y + ROBOT_HEIGHT < d->pos.y - d->half || bot->pos.y > d->pos.y + d->half) continue;
        return d->level;
    }
    return -1;
}

/* The moving or standable thing under this spot whose top is between `from`
 * and `low`, the highest of them; kind SURF_NONE when nothing */
float surface_under(float x, float z, float from, float low, SurfKind* kind, int* which) {
    float best = -1.0f, grow = ROBOT_RADIUS * 0.5f;
    *kind = SURF_NONE; *which = -1;
#define TAKE(k, i, top) do { if ((top) <= from && (top) >= low && (*kind == SURF_NONE || (top) > best)) { best = (top); *kind = (k); *which = (i); } } while (0)
    for (int i = 0; i < moverCount; i++) {
        const Mover* m = &movers[i];
        const MoverDef* d = &MOVERS[m->kind];
        const Box* b = &moverBoxes[m->kind];
        if (d->hurts) continue;
        float lx, lz, sx = WORLD_SCALE * m->scale.x, sz = WORLD_SCALE * m->scale.z;
        to_local(shz_vec3_init(x, 0.0f, z), m->pos, m->yaw, &lx, &lz);
        if (fabsf(lx - b->cx * sx) > b->hx * sx + grow || fabsf(lz - b->cz * sz) > b->hz * sz + grow) continue;
        TAKE(SURF_MOVER, i, m->pos.y + b->top * WORLD_SCALE * m->scale.y);
    }
    for (int i = 0; i < propCount; i++) {
        const Prop* p = &props[i];
        const PropDef* d = &PROPS[p->kind];
        const Box* b = &boxes[p->kind];
        if (!d->stand) continue;
        float sx = WORLD_SCALE * p->scale.x, sz = WORLD_SCALE * p->scale.z;
        if (d->wheel) {                             /* the cog, seen edge on: its top is a circle in x */
            float r = b->hx * sx, dx = x - p->pos.x;
            if (fabsf(dx) < r && fabsf(z - p->pos.z) <= b->hz * sz + grow) TAKE(SURF_PROP, i, p->pos.y + sqrtf(r * r - dx * dx));
            continue;
        }
        if (d->disc) {
            float dx = x - p->pos.x - b->cx * sx, dz = z - p->pos.z - b->cz * sz, r = box_radius(b) * sx + grow;
            if (dx * dx + dz * dz > r * r) continue;
        } else {
            float lx, lz;
            to_local(shz_vec3_init(x, 0.0f, z), p->pos, prop_yaw(p), &lx, &lz);
            if (fabsf(lx - b->cx * sx) > b->hx * sx + grow || fabsf(lz - b->cz * sz) > b->hz * sz + grow) continue;
        }
        TAKE(SURF_PROP, i, p->pos.y + b->top * WORLD_SCALE * p->scale.y);
    }
#undef TAKE
    return best;
}

/* ---- Every frame ---- */

/* Any player standing on it */
static bool any_riding_prop(int i)  { bool r = false; FOR_PLAYERS(p) r |= riding_prop(i);  return r; }
static bool any_riding_mover(int i) { bool r = false; FOR_PLAYERS(p) r |= riding_mover(i); return r; }

/* The nearest living player's flat distance to a spot; a long way with none */
static float nearest_xz(shz_vec3_t at) {
    float best = 1.0e9f;
    for (int i = 0; i < game_players; i++) {
        if (bots[i].dead) continue;
        float d = dist_xz(bots[i].pos, at);
        if (d < best) best = d;
    }
    return best;
}

/* Taken by, and pulled to, whichever living player is nearest */
static void update_pickup(Pickup* p, float spin, float range, float pull, float dt) {
    if (p->taken) return;
    p->spin += spin * dt;
    int was = robot_index();
    if (robot_select_nearest(p->pos)) {
        shz_vec3_t body = shz_vec3_init(bot->pos.x, bot->pos.y + ROBOT_BODY_Y, bot->pos.z);
        float d = dist3(body, p->pos);
        if (d < BOLT_TAKE_RANGE + ROBOT_RADIUS) { p->taken = true; sfx(SFX_BOLT); }
        else if (d < range && d > 0.001f) p->pos = lerp3(p->pos, body, (1.0f - d / range) * pull * dt / d);
    }
    robot_select(was);
}

static void update_mover(Mover* m, int i, float dt) {
    const MoverDef* d = &MOVERS[m->kind];
    bool on = any_riding_mover(i);
    int dir = m->go ? m->dir : 0;
    if (m->kind == M_HIVE) {
        if (on) { m->off = 0.0f; dir = 1; }
        else { m->off += dt; dir = m->off >= HIVE_RETURN ? -1 : 0; }
    }
    if (dir && m->count > 1) {
        shz_vec3_t a = m->wp[m->target - 1], b = m->wp[m->target];
        float len = dist3(a, b);
        float speed = m->speed * (d->loop && b.y - a.y < -1.0f ? ROCK_DROP_MULT : 1.0f);
        m->t += dir * speed * dt / (len > 0.1f ? len : 0.1f);
        if (m->t >= 1.0f) {
            if (d->loop) {                  /* the next leg, or round to the start */
                m->t = 0.0f;
                if (++m->target >= m->count) m->target = 1;
                a = m->wp[m->target - 1]; b = m->wp[m->target];
            } else { m->t = 1.0f; m->dir = -1; }
        }
        if (m->t <= 0.0f) { m->t = 0.0f; m->dir = 1; }
        shz_vec3_t was = m->pos;
        m->pos = lerp3(a, b, m->t);
        if (on && dist3(was, m->pos) < 100.0f) {   /* they ride along, but not round the rock's jump back */
            FOR_PLAYERS(p) if (riding_mover(i)) {
                bot->pos.x += m->pos.x - was.x; bot->pos.y += m->pos.y - was.y; bot->pos.z += m->pos.z - was.z;
            }
        }
    }
    if (d->hurts) FOR_PLAYERS(p) {
        if (robot_dead()) continue;
        const Box* b = &moverBoxes[m->kind];
        float lx, lz, sx = WORLD_SCALE * m->scale.x, sy = WORLD_SCALE * m->scale.y, sz = WORLD_SCALE * m->scale.z;
        to_local(bot->pos, m->pos, m->yaw, &lx, &lz);
        if (fabsf(lx - b->cx * sx) < b->hx * sx + ROBOT_RADIUS && fabsf(lz - b->cz * sz) < b->hz * sz + ROBOT_RADIUS
            && bot->pos.y + ROBOT_HEIGHT > m->pos.y + b->bottom * sy && bot->pos.y < m->pos.y + b->top * sy)
            robot_hurt(1);
    }
}

/* What a prop does to the selected player. before and after: a swing's
 * yaw this frame; sunk: how much a sinking one went down this frame */
static void prop_touch(Prop* p, int i, float before, float after, float sunk, float dt) {
    const PropDef* d = &PROPS[p->kind];
    bool on = riding_prop(i), dead = robot_dead();
    float lx, lz;
    if (on) bot->pos.y -= sunk;
    switch (p->kind) {
    case P_PAD:
        if (bot->grounded && dist_xz(bot->pos, p->pos) < PAD_RANGE * p->scale.x && fabsf(bot->pos.y - p->pos.y) < STEP_UP) robot_pad();
        break;
    case P_BUTTON: {                        /* each step on it switches its id; the movers on it start */
        bool press = bot->grounded && dist_xz(bot->pos, p->pos) < BUTTON_RANGE * p->scale.x
                  && fabsf(bot->pos.y - (p->pos.y + BUTTON_TOP * p->scale.x)) < STEP_UP;
        if (press && !p->on && !p->pressed) {
            ids[p->id] = !ids[p->id];
            for (int k = 0; k < moverCount; k++) if (movers[k].id == p->id && ids[p->id]) movers[k].go = true;
        }
        p->pressed |= press;
        break;
    }
    case P_BELT:                            /* carries him along its length */
        if (on && bot->grounded) { bot->pos.x -= cosf(p->yaw) * BELT_PUSH * dt; bot->pos.z -= sinf(p->yaw) * BELT_PUSH * dt; }
        break;
    case P_FAN: {                           /* his stream lifts him hard; it eases off near the top */
        if (dead) break;
        float sy = p->scale.y, top = p->pos.y + FAN_TOP * sy;
        if (dist_xz(bot->pos, p->pos) > FAN_REACH * p->scale.x || bot->pos.y < top - FAN_BELOW * sy || bot->pos.y >= top + FAN_ABOVE * sy) break;
        float want = FAN_LIFT * sy, left = top + FAN_ABOVE * sy - bot->pos.y;
        if (left < FAN_EASE) want *= left / FAN_EASE;
        if (bot->velY < want) { bot->velY += want * FAN_LIFT_RATE * dt; if (bot->velY > want) bot->velY = want; }
        robot_airborne();
        break;
    }
    case P_GRINDER: {                       /* the drum hurts to touch */
        if (dead) break;
        const Box* b = &boxes[P_GRINDER];
        float r = b->hx * WORLD_SCALE * p->scale.x;
        to_local(bot->pos, p->pos, p->yaw, &lx, &lz);
        if (fabsf(lx) < r + ROBOT_RADIUS && fabsf(lz) < b->hz * WORLD_SCALE * p->scale.z + ROBOT_RADIUS
            && bot->pos.y + ROBOT_HEIGHT > p->pos.y - r && bot->pos.y < p->pos.y + r) robot_hurt(1);
        break;
    }
    case P_SWING:                           /* swings about its chain; he goes round with it */
        if (!on) break;
        to_local(bot->pos, p->pos, before, &lx, &lz);
        bot->pos.x = p->pos.x + lx * cosf(after) - lz * sinf(after);
        bot->pos.z = p->pos.z + lx * sinf(after) + lz * cosf(after);
        bot->angle += after - before;
        break;
    case P_COG: {                           /* a wheel that turns while he is near; he goes round with it */
        if (!on || dist_xz(bot->pos, p->pos) >= COG_SPIN_RANGE) break;
        float da = d->spin * dt, rx = bot->pos.x - p->pos.x, ry = bot->pos.y - p->pos.y;
        bot->pos.x = p->pos.x + rx * cosf(da) - ry * sinf(da);
        bot->pos.y = p->pos.y + rx * sinf(da) + ry * cosf(da);
        break;
    }
    case P_LASER: {                         /* a wall of it, until its switch: touching it kills */
        if (dead || ids[p->id]) break;
        const Box* b = &boxes[P_LASER];
        to_local(bot->pos, p->pos, p->yaw, &lx, &lz);
        if (fabsf(lx) <= b->hx * WORLD_SCALE * p->scale.x + ROBOT_RADIUS && fabsf(lz) <= b->hz * WORLD_SCALE * p->scale.z + ROBOT_RADIUS
            && bot->pos.y + ROBOT_HEIGHT >= p->pos.y && bot->pos.y <= p->pos.y + b->top * WORLD_SCALE * p->scale.y) robot_hurt(HEALTH);
        break;
    }
    case P_STREAM: {                        /* the poison stream hurts: its box, off to one side of its Empty */
        if (dead) break;
        const Box* b = &boxes[P_STREAM];
        float sx = WORLD_SCALE * p->scale.x, sy = WORLD_SCALE * p->scale.y, sz = WORLD_SCALE * p->scale.z;
        to_local(bot->pos, p->pos, p->yaw, &lx, &lz);
        float y = bot->pos.y - p->pos.y;
        if (fabsf(lx - b->cx * sx) <= b->hx * sx + ROBOT_RADIUS && fabsf(lz - b->cz * sz) <= b->hz * sz + ROBOT_RADIUS
            && y + ROBOT_HEIGHT >= b->bottom * sy && y <= b->top * sy) robot_hurt(1);
        break;
    }
    default: break;
    }
}

/* The prop's own motion once, then what it does to each player */
static void update_prop(Prop* p, int i, float dt) {
    const PropDef* d = &PROPS[p->kind];
    bool on = any_riding_prop(i);
    if (p->kind != P_COG || nearest_xz(p->pos) < COG_SPIN_RANGE) p->spin += d->spin * dt;
    float sunk = 0.0f;
    if (d->sink > 0.0f) {                   /* sinks under him, floats back once he is off; he sinks with it */
        float was = p->sunk, deep = d->sink * p->scale.y;
        p->sunk += (on ? SINK_SPEED : -SINK_SPEED) * dt;
        if (p->sunk > deep) p->sunk = deep;
        if (p->sunk < 0.0f) p->sunk = 0.0f;
        sunk = p->sunk - was;
        p->pos.y -= sunk;
    }
    float before = 0.0f, after = 0.0f;
    if (p->kind == P_SWING) {
        before = prop_yaw(p);
        p->phase += SWING_RATE * dt;
        after = prop_yaw(p);
    }
    p->pressed = false;
    FOR_PLAYERS(q) prop_touch(p, i, before, after, sunk, dt);
    if (p->kind == P_BUTTON) p->on = p->pressed;    /* down while anyone stands on it */
}

/* The last bolt or screw in the game: the box says so, once per save */
static void all_collected(void) {
    SaveFile* s = save_active();
    if (game_coop || !s || s->seenReward || !save_all_collected(s)) return;
    s->seenReward = true;
    hud_reward();
}

/* Any living player within reach of a spot */
static bool anyone_near(shz_vec3_t at, float reach) {
    for (int i = 0; i < game_players; i++)
        if (!bots[i].dead && dist3(bots[i].pos, at) < reach) return true;
    return false;
}

/* Any living player inside a box round a spot */
static bool anyone_in(shz_vec3_t at, shz_vec3_t half) {
    for (int i = 0; i < game_players; i++) {
        const Robot* r = &bots[i];
        if (!r->dead && fabsf(r->pos.x - at.x) < half.x && fabsf(r->pos.y - at.y) < half.y && fabsf(r->pos.z - at.z) < half.z) return true;
    }
    return false;
}

void props_update(float dt) {
    for (int i = 0; i < boltCount; i++) {
        bool was = bolts[i].taken;
        update_pickup(&bolts[i], BOLT_SPIN, BOLT_PULL_RANGE, BOLT_PULL_SPEED, dt);
        if (bolts[i].taken && !was) {
            boltsTaken++; hud_bolt();
            fx_flash(0xFFFFFF, 0.1f, 80.0f / 255.0f); fx_shake(3.0f);
            fx_sparks(bolts[i].pos, 16);
            save_level_bolts(game_level_id, boltsTaken, boltCount);
            all_collected();
        }
    }
    if (haveScrew) {
        update_pickup(&screw, SCREW_SPIN, SCREW_PULL_RANGE, SCREW_PULL_SPEED, dt);
        if (screw.taken && !screwTaken) {
            hud_screw();
            fx_flash(0xFFFFFF, 0.1f, 80.0f / 255.0f); fx_shake(5.0f);
            fx_sparks(screw.pos, 24);
            save_level_screw(game_level_id, true, true);
            all_collected();
            if (!game_demo) save_write();   /* a screw is rare enough to write at once */
        }
        screwTaken = screw.taken;
    }
    for (int k = 0; k < P_KINDS; k++) if (PROPS[k].anim && models[k][0]) dc_model_animate(models[k][0], dt);
    for (int i = 0; i < propCount; i++) update_prop(&props[i], i, dt);
    for (int i = 0; i < moverCount; i++) update_mover(&movers[i], i, dt);

    if (haveCheckpointBox && anyone_in(checkpointBox, checkpointHalf)) {
        checkpoint = shz_vec3_init(checkpointBox.x, checkpointBox.y + checkpointUp, checkpointBox.z);
        haveCheckpoint = true;
    }
    if (haveExit && state == PLAYING && !game_demo && anyone_near(exitPos, EXIT_RANGE)) {
        SaveFile* s = save_active();
        if (s && game_level_id < SAVE_MAX_LEVELS) s->completed[game_level_id] = true;
        save_level_bolts(game_level_id, boltsTaken, boltCount);
        save_level_screw(game_level_id, screwTaken, haveScrew);
        celebration_start();                /* records the rank and writes the card */
    }
}

/* Turned about its own z (a wheel or a drum), then its yaw: 3 columns */
static void spin_rot(float angle, float yaw, float rot[9]) {
    float ca = cosf(angle), sa = sinf(angle), cy = cosf(yaw), sy = sinf(yaw);
    rot[0] = ca * cy;  rot[1] = sa;   rot[2] = ca * sy;
    rot[3] = -sa * cy; rot[4] = ca;   rot[5] = -sa * sy;
    rot[6] = -sy;      rot[7] = 0.0f; rot[8] = cy;
}

void props_draw(void) {
    const DCImage* levelShine = dc_get_environment();
    dc_set_environment(boltShine);
    for (int i = 0; i < boltCount; i++)         /* the N64's pulse: 15% either way, three times a turn */
        if (!bolts[i].taken) draw_at(boltModel, bolts[i].pos, 1.0f + sinf(bolts[i].spin * 3.0f) * 0.15f, bolts[i].spin);
    dc_set_environment(levelShine);
    if (haveScrew && !screw.taken) draw_at(screwModel, screw.pos, 1.0f, screw.spin);
    for (int i = 0; i < moverCount; i++) draw_stretched(moverModels[movers[i].kind], movers[i].pos, movers[i].scale, movers[i].yaw);
    for (int i = 0; i < propCount; i++) {
        const Prop* p = &props[i];
        const PropDef* d = &PROPS[p->kind];
        float rot[9];
        DCDrawOpts o = { .pos = p->pos, .scale = WORLD_SCALE, .stretch = p->scale, .yaw = prop_yaw(p) };
        if (d->wheel) { spin_rot(p->spin, p->yaw, rot); o.rot = rot; }
        if (p->kind == P_LASER) { if (models[p->kind][ids[p->id]]) dc_draw_ex(models[p->kind][ids[p->id]], &o); continue; }
        if (models[p->kind][0]) dc_draw_ex(models[p->kind][0], &o);
        if (!models[p->kind][1]) continue;
        if (p->kind == P_BUTTON && p->on) o.pos.y -= BUTTON_SINK * p->scale.x;
        dc_draw_ex(models[p->kind][1], &o);
    }
}
