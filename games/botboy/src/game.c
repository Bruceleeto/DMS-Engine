/* The game: a level with the robot in it. Level 1 is the hub, a road with
 * six doors along the back, one to each level; the rest are the levels.
 * Each is a glb with its things placed as Empties, read back by name
 * (props.c and enemies.c say which names). Levels are seen side-on: the
 * camera stays in front of the robot, a little above, and looks at him. */
#include "sound.h"
#include "level.h"
#include "demo.h"
#include "scene.h"
#include "save.h"
#include "ui.h"
#include "dms/dc_camera.h"
#include "dms/dc_particles.h"

int  game_level_id;
int  game_players = 1;
bool game_coop;
extern bool menu_starts_with_fade_in;

/* ---- Levels ---- */

/* Maps draw as they are in the glb, the camera on the +z side. `towards`
 * is the way to the light; a DMS sun is the way it shines, so it is negated. */
static const LevelDef LEVELS[] = {
    { "level1", BODY_FB,    { .x = 0.2f, .y = 0.5f, .z = 1.0f }, 80.0f / 255.0f, 0, 0, 0, 0xFF000000, "scrap1" },
    { "level2", BODY_TORSO, { .x = 0.2f, .y = 0.5f, .z = 1.0f }, 124.0f / 255.0f, 0xFF8C28, 100.0f, 600.0f, 0xFF000000, "CalmJunglescrap" },
    { "level3", BODY_TORSO, { .x = 0.2f, .y = 0.5f, .z = 1.0f }, 80.0f / 255.0f, 0x5A1E78, 100.0f, 600.0f, 0xFF280F37, "androidjungle1" },
    { "level4", BODY_ARMS,  { .x = 0.2f, .y = 0.5f, .z = 1.0f }, 80.0f / 255.0f, 0x8CAAC8, 200.0f, 900.0f, 0xFF000000, "scrap1-level1" },
    { "level5", BODY_ARMS,  { .x = 0.2f, .y = 0.5f, .z = 1.0f }, 80.0f / 255.0f, 0xC8DCF0, 100.0f, 600.0f, 0xFF87CEEB, "N64Chiller3" },
    { "level6", BODY_FB,    { .x = 0.2f, .y = 0.5f, .z = 1.0f }, 80.0f / 255.0f, 0xC83C14, 100.0f, 600.0f, 0xFF000000, "N64JetForceTrack" },
    { "level7", BODY_FB,    { .x = -0.577f, .y = 0.577f, .z = -0.577f }, 80.0f / 255.0f, 0, 0, 0, 0xFF000000, "scrap1" },
};
#define LEVEL_COUNT (int)(sizeof(LEVELS) / sizeof(LEVELS[0]))

#define CAM_BACK      150.0f            /* the middle of the three zooms */
#define CAM_NEAR      110.0f
#define CAM_FAR       200.0f
#define CAM_ZOOM_RATE (0.08f * 30.0f)   /* the distance closes on the zoom this fraction a second */
#define CAM_SMOOTH    (0.06f * 30.0f)   /* and the whole camera on where it wants to be */
#define CAM_DEATH_IN  60.0f             /* it comes this much closer as he dies */
#define CAM_UP        49.0f
#define CAM_LEAD      0.1f              /* seconds of speed to look ahead */
#define CAM_FOLLOW_X  (0.08f * 30.0f)
#define CAM_FOLLOW_Y  (0.06f * 30.0f)
#define CAM_AIM_X     (0.15f * 30.0f)
#define CAM_FOV       70.0f
/* Co-op: each half of the screen has its own camera, closer and wider, as
 * the N64 had it (0.4 of the distance, FOV 80) */
#define COOP_CAM_K    0.4f
#define COOP_FOV      80.0f
#define COOP_SPAWN_X  30.0f             /* the two start this far either side of the spawn */
#define COOP_TOO_FAR  400.0f            /* the one left behind is put beside the leader past this */
#define COOP_CATCH_X  30.0f
#define COOP_CATCH_Y  20.0f
#define COUNT_NUM     0.8f              /* the countdown: each number, then GO */
#define COUNT_GO      0.6f
#define COUNT_TOTAL   (3.0f * COUNT_NUM + COUNT_GO)
#define COUNT_SCALE   (4.0f * 1.5f)     /* the 16x16 numbers at 4x, popped to 1.5 of that */
/* The halves in 320x240 space: 32 pixel tiles clip the screen, so 224 rows
 * each with a black band between */
#define HALF_H        112.0f
#define HALF2_Y       128.0f
#define FADE_TIME     0.5f
#define RESPAWN_HOLD  0.5f              /* black after the iris, then the fade in; no controls until it is done */
#define IRIS_START    400.0f
#define IRIS_HOLD     25.0f             /* it pauses at this size */
#define IRIS_HOLD_FOR 0.33f
/* The hub's far end: black over two seconds, the slide whistle, the crash,
 * the logo, then the level */
#define TR_FADE       2.0f
#define TR_WHISTLE    2.0f
#define TR_THUD       3.5f
#define TR_LOGO       4.0f
#define TR_LOGO_FADE  0.3f
#define TR_DONE       5.5f

/* Boxes that change the light or the fog, or hurt, from Empties whose names
 * carry the numbers: "lp_060a21_59b5ff" sets the light for good on entry,
 * ambient then colour in hex; "lt_..." the same blended in by distance;
 * "fog_0d7bff" a fog colour; "pain" kills, "hurt" takes a heart, "ending"
 * starts the end slideshow. */
#define MAX_ZONES     24
#define BOX_HALF      32.0f             /* a box Empty, times its scale */
#define LIGHT_RADIUS  32.0f             /* a permanent light fires inside this, times its widest scale */
#define LIGHT_BLEND   (32.0f * 1.5f)    /* a fading one reaches this far */
#define LIGHT_FADE    0.5f              /* seconds, a permanent zone's light arriving */
#define FOG_FADE      0.5f              /* seconds, a fog zone's colour arriving */
typedef enum { ZONE_LIGHT_PERM, ZONE_LIGHT_FADE, ZONE_FOG, ZONE_KILL, ZONE_HURT, ZONE_END } ZoneKind;
typedef struct {
    ZoneKind   kind;
    shz_vec3_t pos, half;
    float      radius;                  /* the light boxes are spheres */
    float      ambient, r, g, b;
    bool       inside;
} Zone;

const LevelDef* def;
DMSModel*  level;
ColWorld*  col;
GameState  state;
bool       ids[MAX_IDS];
float      levelTime;
int        deaths;

/* Each player's camera and his death: one view in single player, two in
 * co-op, each over its own part of the screen */
typedef struct {
    DCCamera   cam;
    float      x, y, aimX;              /* where it follows, smoothed */
    float      back;                    /* how far in front of him it wants to be */
    shz_vec3_t now;                     /* where it is, smoothed */
    bool       snap;
    float      deathZoom, irisR, irisHoldTime, respawnHold;
    bool       irisOn;
    float      dark;                    /* his own fade in, coming back beside his partner */
    bool       buffWorn;                /* his texture is the charge pad's glow */
    float      top, height;             /* his part of the screen, 320x240 space */
} View;
static View      views[MAX_PLAYERS];
static DCLight   light;
static int       zoom = 1;              /* 0 near, 1 as the N64 started, 2 far; the same for both in co-op */
static float     fade;
static float     countTime;             /* co-op: the 3 2 1 GO, from the fade in */
static bool      countDone;
static DCImage*  countImg[4];
static int       pauser;                /* co-op: whose pad the pause menu answers to */
static int       doorLevel;
static float     trTimer;               /* the hub's far end sequence */
static DCImage*  trLogo;
static DCImage*  buffImg;               /* the glow the charge pad puts on him */
static bool      cpLight;               /* the light a permanent zone set: back to it on a respawn */
static float     cpAmbient, cpR, cpG, cpB;
static Zone      zones[MAX_ZONES];
static int       zoneCount;
static uint32_t  fogWant;               /* the fog the last fog zone asked for */
static bool      lightFading;           /* a fading zone had the light last frame */
static float     hitstop, shake, shakeX, shakeY;
static DCParticles* oil;
static DCImage*  shine;             /* what the bolts and the screw reflect */

DMSModel* load(const char* dir, const char* name) {
    char path[64];
    snprintf(path, sizeof(path), ASSETS "%s/%s.dms", dir, name);
    DMSModel* m = dc_model_load(path);
    if (!m) printf("game: cannot load %s\n", path);
    return m;
}

/* ---- Hits: the freeze, the jolt, the oil ---- */

#define SHAKE_DECAY 0.85f               /* what is left each thirtieth */

void fx_hitstop(float seconds) { hitstop = seconds; }
void fx_shake(float units)     { if (units > shake) shake = units; }
void fx_oil(shz_vec3_t at, int drops) {
    if (!oil) return;
    dc_particles_move(oil, shz_vec3_init(at.x, at.y + 5.0f, at.z));
    dc_particles_burst(oil, drops);
}

static void shake_update(float dt) {
    shake *= powf(SHAKE_DECAY, dt * 30.0f);
    if (shake < 0.1f) { shake = shakeX = shakeY = 0.0f; return; }
    shakeX = (rand_f() * 2.0f - 1.0f) * shake;
    shakeY = (rand_f() * 2.0f - 1.0f) * shake;
}

/* ---- Light, fog and the zones ---- */

void light_reset(void) {
    light = (DCLight){ .pos = { .x = -def->towards.x, .y = -def->towards.y, .z = -def->towards.z },
                       .sun = true, .ambient = def->ambient, .r = 1.0f, .g = 1.0f, .b = 1.0f };
    dc_set_light(&light);
    lightFading = false;
    fogWant = def->fog;
    dc_set_clear_color(def->clear);
    if (def->fog) dc_set_fog(def->fog, def->fogNear, def->fogFar);
    else dc_set_fog_off();
    for (int i = 0; i < zoneCount; i++) zones[i].inside = false;
}

static bool hex_rgb(const char* h, float* r, float* g, float* b) {
    unsigned v;
    if (sscanf(h, "%6x", &v) != 1) return false;
    *r = ((v >> 16) & 255) / 255.0f; *g = ((v >> 8) & 255) / 255.0f; *b = (v & 255) / 255.0f;
    return true;
}

static void zones_init(void) {
    zoneCount = 0;
    for (uint32_t i = 0; level && i < level->entity_count && zoneCount < MAX_ZONES; i++) {
        const DMSEntity* e = &level->entities[i];
        const char* nm = e->name;
        Zone z = { .pos = ent_pos(e) };
        float widest = e->scale.x > e->scale.z ? e->scale.x : e->scale.z;
        float ar, ag, ab;
        if (!strncmp(nm, "lp_", 3) || !strncmp(nm, "lt_", 3)) {
            if (!hex_rgb(nm + 3, &ar, &ag, &ab) || !hex_rgb(nm + 10, &z.r, &z.g, &z.b)) continue;
            z.kind = nm[1] == 'p' ? ZONE_LIGHT_PERM : ZONE_LIGHT_FADE;
            z.ambient = (ar + ag + ab) / 3.0f;
            z.radius = (z.kind == ZONE_LIGHT_PERM ? LIGHT_RADIUS : LIGHT_BLEND) * widest;
        } else if (!strncmp(nm, "fog_", 4)) {
            if (!hex_rgb(nm + 4, &z.r, &z.g, &z.b)) continue;
            z.kind = ZONE_FOG;
        } else if (!strncmp(nm, "pain", 4)) z.kind = ZONE_KILL;
        else if (!strncmp(nm, "hurt", 4)) z.kind = ZONE_HURT;
        else if (!strncmp(nm, "ending", 6)) z.kind = ZONE_END;
        else continue;
        z.half = shz_vec3_init(BOX_HALF * fabsf(e->scale.x), BOX_HALF * fabsf(e->scale.y), BOX_HALF * fabsf(e->scale.z));
        zones[zoneCount++] = z;
    }
}

/* A permanent light box sets the light and the engine eases there over
 * LIGHT_FADE; a fading one blends the light in as he nears its middle and
 * out again, set each frame. A fog box eases the fog to its colour over
 * FOG_FADE and the sky follows; the fog stays that colour after leaving,
 * until the next box or a death. */
static void zones_update(float dt) {
    (void)dt;
    DCLight now = light;
    bool fading = false;
    for (int i = 0; i < zoneCount; i++) {
        Zone* z = &zones[i];
        if (z->kind == ZONE_KILL || z->kind == ZONE_HURT || z->kind == ZONE_END) {
            FOR_PLAYERS(p) {
                bool in = fabsf(bot->pos.x - z->pos.x) < z->half.x + ROBOT_RADIUS && fabsf(bot->pos.z - z->pos.z) < z->half.z + ROBOT_RADIUS
                       && bot->pos.y + ROBOT_HEIGHT > z->pos.y - z->half.y && bot->pos.y < z->pos.y + z->half.y;
                if (!in || robot_dead() || robot_god) continue;
                if (z->kind == ZONE_KILL) robot_hurt(HEALTH);
                else if (z->kind == ZONE_HURT) robot_hurt(1);
                else if (state == PLAYING) cutscene_start(true);
            }
        } else if (z->kind == ZONE_FOG) {       /* the light and the fog are one level's: player 1 sets them */
            uint32_t c = ((uint32_t)(z->r * 255) << 16) | ((uint32_t)(z->g * 255) << 8) | (uint32_t)(z->b * 255);
            if (def->fog && c != fogWant
                && fabsf(bots[0].pos.x - z->pos.x) < z->half.x && fabsf(bots[0].pos.y - z->pos.y) < z->half.y && fabsf(bots[0].pos.z - z->pos.z) < z->half.z) {
                fogWant = c;
                dc_set_fog_over(c, def->fogNear, def->fogFar, FOG_FADE);
                dc_set_clear_color_over(0xFF000000u | c, FOG_FADE);
            }
        } else if (z->kind == ZONE_LIGHT_PERM) {
            bool in = dist3(bots[0].pos, z->pos) < z->radius;
            if (in && !z->inside) {
                light.ambient = z->ambient; light.r = z->r; light.g = z->g; light.b = z->b;
                now = light;
                dc_set_light_over(&light, LIGHT_FADE);
                cpLight = true; cpAmbient = z->ambient; cpR = z->r; cpG = z->g; cpB = z->b;
            }
            z->inside = in;
        } else {
            float t = 1.0f - dist3(bots[0].pos, z->pos) / z->radius;
            if (t <= 0.0f) continue;
            if (t > 1.0f) t = 1.0f;
            t = t * t * (3.0f - 2.0f * t);
            now.ambient = light.ambient + (z->ambient - light.ambient) * t;
            now.r = light.r + (z->r - light.r) * t; now.g = light.g + (z->g - light.g) * t; now.b = light.b + (z->b - light.b) * t;
            fading = true;
        }
    }
    /* A fading zone sets the light as it stands each frame; the frame it
     * lets go, the light goes back to its own */
    if (fading) dc_set_light(&now);
    else if (lightFading) dc_set_light(&light);
    lightFading = fading;
}

/* ---- The scene ---- */

/* The selected player's camera snapped to him */
static void cam_snap(void) {
    View* v = &views[robot_index()];
    v->x = v->aimX = bot->pos.x; v->y = bot->pos.y + CAM_UP;
    v->snap = true;
    v->deathZoom = 0.0f; v->irisOn = false; v->respawnHold = 0.0f; v->dark = 0.0f;
}

/* Where they come back, with the cameras snapped to them: in co-op either
 * side of the spot */
static void place_robots(void) {
    shz_vec3_t at = props_spawn();
    FOR_PLAYERS(p) {
        robot_place(game_coop ? shz_vec3_init(at.x + (p ? COOP_SPAWN_X : -COOP_SPAWN_X), at.y, at.z) : at);
        cam_snap();
    }
}

static void init(void) {
    def = &LEVELS[game_level_id < LEVEL_COUNT ? game_level_id : 0];
    if (game_demo) game_coop = false;
    game_players = game_coop ? 2 : 1;
    level = load("game", def->file);
    col = level ? col_build(level, shz_vec3_init(0.0f, 0.0f, 0.0f), WORLD_SCALE) : NULL;
    memset(ids, 0, sizeof(ids));
    levelTime = 0.0f;
    deaths = 0;

    robot_init(def->body);
    props_init();
    enemies_init();
    zones_init();
    cpLight = false;
    light_reset();
    place_robots();
    if (game_demo) {                    /* from where the recording began */
        robot_place(demo_start_pos()); bot->angle = demo_start_angle();
        cam_snap();
    }
    for (int p = 0; p < game_players; p++) {
        View* v = &views[p];
        dc_camera_init(&v->cam);
        v->cam.fov = game_coop ? COOP_FOV : CAM_FOV;
        v->back = (zoom == 0 ? CAM_NEAR : zoom == 2 ? CAM_FAR : CAM_BACK) * (game_coop ? COOP_CAM_K : 1.0f);
        v->buffWorn = false;
        v->top = p ? HALF2_Y : 0.0f;
        v->height = game_coop ? HALF_H : VIEW_H;
        /* Its part of the screen, in pixels: the top and the bottom 224 rows */
        if (game_coop) v->cam.view = (DCViewport){ 0.0f, v->top * VIEW_SCALE, VIEW_W * VIEW_SCALE, v->height * VIEW_SCALE };
    }
    hitstop = shake = 0.0f;
    trTimer = 0.0f; trLogo = NULL;
    buffImg = dc_image_load(ASSETS "game/robot_player_fx.dt");
    countTime = -FADE_TIME; countDone = !game_coop; pauser = 0;
    if (game_coop) {                    /* the countdown, nobody moving until GO */
        static const char* NAMES[4] = { "Three", "Two", "One", "Go" };
        for (int i = 0; i < 4; i++) { char path[48]; snprintf(path, sizeof(path), ASSETS "ui/%s.dt", NAMES[i]); countImg[i] = dc_image_load(path); }
        FOR_PLAYERS(p) robot_lock(FADE_TIME + COUNT_TOTAL);
    }
    hud_init();
    fx_init();
    shine = dc_image_load(ASSETS "game/env_gold.dt");
    dc_set_environment(shine);
    oil = dc_particles_create(64, &(DCParticleOpts){
        .spread = shz_vec3_init(8.0f, 0.0f, 8.0f), .speed = shz_vec3_init(0.0f, 180.0f, 0.0f),
        .speed_spread = shz_vec3_init(100.0f, 60.0f, 100.0f),
        .gravity = 360.0f, .life = 0.75f, .life_spread = 0.15f, .size = 4.0f, .size_spread = 1.0f,
        .start = 0x503C1E, .end = 0x281E0F, .smoke = true });
    state = FADE_IN;
    fade = 1.0f;
    char text[48];
    snprintf(text, sizeof(text), "Level %d: %s", game_level_id, game_level_id < REAL_LEVEL_COUNT ? LEVEL_NAMES[game_level_id] : "?");
    if (!game_demo) {                   /* a demo has no banner, box or slideshow; co-op the banner only */
        banner_show(text);
        if (!game_coop) { tutorial_start(); cutscene_start(false); }
    }
    music(def->music);                  /* a demo plays its level's track, as the N64 did */
}

static void deinit(void) {
    if (demo_recording()) demo_record_toggle();     /* the level ending writes what there is */
    enemies_free();
    props_free();
    robot_free();
    screens_free();
    if (col) col_free(col);
    dc_particles_free(oil); oil = NULL;
    dc_set_environment(NULL);
    dc_image_free(shine); shine = NULL;
    dc_image_free(buffImg); buffImg = NULL;
    for (int i = 0; i < 4; i++) { dc_image_free(countImg[i]); countImg[i] = NULL; }
    dc_image_free(trLogo); trLogo = NULL;
    fx_free();
    hud_free();
    dc_model_free(level);
    level = NULL; col = NULL;
    dc_set_fog_off();
}

/* Back to the checkpoint or the start with the level as it was: the
 * switches off, the movers home, the light the level's, the enemies back */
void level_respawn(void) {
    memset(ids, 0, sizeof(ids));
    props_reset();
    enemies_reset();
    light_reset();
    if (cpLight) {                      /* the light as the checkpoint's zone left it, at once */
        light.ambient = cpAmbient; light.r = cpR; light.g = cpG; light.b = cpB;
        dc_set_light(&light);
    }
    place_robots();
    fade = 1.0f;
    FOR_PLAYERS(p) { views[p].respawnHold = RESPAWN_HOLD; robot_lock(RESPAWN_HOLD + FADE_TIME); }
    state = FADE_IN;
}

/* Co-op, one of them dead with the other going on: he comes back beside his
 * partner, his half of the screen fading in on its own */
static void respawn_beside(int p, int partner) {
    View* v = &views[p];
    robot_select(p);
    robot_place(shz_vec3_init(bots[partner].pos.x, bots[partner].pos.y + COOP_CATCH_Y, bots[partner].pos.z));
    cam_snap();
    v->dark = 1.0f;
    v->respawnHold = RESPAWN_HOLD;
    robot_lock(RESPAWN_HOLD + FADE_TIME);
}

/* Co-op: whoever falls too far behind on x is put beside the leader, the
 * one nearer the exit; the N64 put the one with the higher x beside the
 * other, which is the same way round only in the levels that run to -x */
static void catch_up(void) {
    if (!game_coop || bots[0].dead || bots[1].dead || state != PLAYING) return;
    if (fabsf(bots[0].pos.x - bots[1].pos.x) <= COOP_TOO_FAR) return;
    shz_vec3_t exit;
    int leader = bots[0].pos.x < bots[1].pos.x ? 0 : 1;
    if (props_exit(&exit)) leader = fabsf(bots[0].pos.x - exit.x) < fabsf(bots[1].pos.x - exit.x) ? 0 : 1;
    int behind = 1 - leader;
    robot_select(behind);
    bot->pos = shz_vec3_init(bots[leader].pos.x + COOP_CATCH_X, bots[leader].pos.y + COOP_CATCH_Y, bots[leader].pos.z);
    bot->velX = bot->velY = bot->velZ = 0.0f;
    robot_airborne();
    cam_snap();
}

/* Out of the level: to the hub, or to the door's level */
void level_leave(int toLevel) {
    doorLevel = toLevel;
    state = DOOR_FADE_OUT;
}

void level_transition(int toLevel) {
    doorLevel = toLevel;
    trTimer = 0.0f;
    fade = 0.0f;
    state = TRANSITION;
    robot_stop();
}

static void fade_in(float dt) {
    bool holding = false;               /* black a moment after the iris */
    for (int p = 0; p < game_players; p++)
        if (views[p].respawnHold > 0.0f) { views[p].respawnHold -= dt; holding = true; }
    if (holding) return;
    fade -= dt / FADE_TIME;
    if (fade <= 0.0f) { fade = 0.0f; state = PLAYING; }
}

/* A player's own fade in, after coming back beside his partner */
static void dark_update(View* v, float dt) {
    if (v->respawnHold > 0.0f) { v->respawnHold -= dt; return; }
    if (v->dark > 0.0f) { v->dark -= dt / FADE_TIME; if (v->dark < 0.0f) v->dark = 0.0f; }
}

/* He dies: the camera drifts in; the clip over, a black circle closes on
 * the screen, pauses, closes the rest of the way, and the level resets. In
 * co-op it closes on his half and he comes back beside his partner; the
 * level resets when both are dead. */
static void update_death(float dt) {
    int p = robot_index();
    View* v = &views[p];
    if (!robot_dead()) return;
    v->deathZoom += 0.8f * dt;
    if (v->deathZoom > 1.0f) v->deathZoom = 1.0f;
    if (!robot_death_done()) return;
    if (!v->irisOn) { v->irisOn = true; v->irisR = IRIS_START; v->irisHoldTime = 0.0f; }
    float steps = dt * 30.0f;
    if (v->irisR > IRIS_HOLD) {
        float d = v->irisR * 0.06f; if (d < 3.0f) d = 3.0f;
        v->irisR -= d * steps;
        if (v->irisR < IRIS_HOLD) v->irisR = IRIS_HOLD;
    } else if (v->irisHoldTime < IRIS_HOLD_FOR) v->irisHoldTime += dt;
    else {
        float d = v->irisR * 0.12f; if (d < 2.0f) d = 2.0f;
        v->irisR -= d * steps;
        if (v->irisR <= 0.0f) {
            v->irisR = 0.0f;
            int partner = 1 - p;
            if (game_coop && !bots[partner].dead) respawn_beside(p, partner);
            else level_respawn();
        }
    }
}

static float smooth_k(float rate, float dt) { float k = rate * dt; return k > 1.0f ? 1.0f : k; }

/* Follows him with a lag, at one of three distances (d-pad up and down),
 * nearer as he dies, and never behind a wall: a ray from his middle to
 * where it wants to be, and it stops short of whatever that hits */
#define CAM_PUSH 20.0f                  /* how far back the camera tries to get clear of a wall */

/* A wall on the line from his middle to the camera */
static bool cam_blocked(shz_vec3_t from, shz_vec3_t to) {
    shz_vec3_t d = shz_vec3_sub(to, from);
    float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len < 1.0f) return false;
    return col_raycast(col, from, shz_vec3_init(d.x / len, d.y / len, d.z / len), len).hit;
}

/* The selected player's camera */
static void update_camera(const DCInput* inp, float dt) {
    View* v = &views[robot_index()];
    if (inp && state == PLAYING && !robot_dead()) {
        if (dc_input_pressed(inp, CONT_DPAD_UP)   && zoom > 0) zoom--;
        if (dc_input_pressed(inp, CONT_DPAD_DOWN) && zoom < 2) zoom++;
    }
    float k = game_coop ? COOP_CAM_K : 1.0f;
    float wantBack = (zoom == 0 ? CAM_NEAR : zoom == 2 ? CAM_FAR : CAM_BACK) * k;
    v->back += (wantBack - v->back) * smooth_k(CAM_ZOOM_RATE, dt);
    v->x += (bot->pos.x + bot->velX * CAM_LEAD - v->x) * smooth_k(CAM_FOLLOW_X, dt);
    v->y += (bot->pos.y + CAM_UP - v->y) * smooth_k(CAM_FOLLOW_Y, dt);
    v->aimX += (bot->pos.x - v->aimX) * smooth_k(CAM_AIM_X, dt);
    shz_vec3_t want = shz_vec3_init(v->x, v->y, bot->pos.z + v->back - CAM_DEATH_IN * k * v->deathZoom + bot->velZ * CAM_LEAD);
    /* As the N64: when a wall is between him and the camera, the camera tries
     * backing further away in steps of 5 up to 20 and takes the first spot
     * with a clear line, 5 further; if none clears (the hub's text wall,
     * which the camera is meant to look through) it stays put */
    if (col) {
        shz_vec3_t from = shz_vec3_init(bot->pos.x, bot->pos.y + ROBOT_BODY_Y, bot->pos.z);
        if (cam_blocked(from, want)) {
            for (int i = 1; i <= 4; i++) {
                shz_vec3_t test = shz_vec3_init(want.x, want.y, want.z + CAM_PUSH * i / 4.0f);
                if (!cam_blocked(from, test)) { want.z = test.z + 5.0f; break; }
            }
        }
    }
    if (v->snap) { v->now = want; v->snap = false; }
    else {
        float s = smooth_k(CAM_SMOOTH, dt);
        v->now.x += (want.x - v->now.x) * s; v->now.y += (want.y - v->now.y) * s; v->now.z += (want.z - v->now.z) * s;
    }
    v->cam.pos = shz_vec3_init(v->now.x + shakeX, v->now.y + shakeY, v->now.z);
    dc_camera_look_at(&v->cam, shz_vec3_init(v->aimX, bot->pos.y, bot->pos.z));
}

static void update_cameras(const DCInput* const* in, float dt) {
    FOR_PLAYERS(p) update_camera(in[p], dt);
}

/* Co-op's 3 2 1 GO, from the end of the fade in. Silent: the N64 had four
 * 321Go sounds in its assets and played none of them */
static void update_count(float dt) {
    if (countDone) return;
    countTime += dt;
    if (countTime >= COUNT_TOTAL) countDone = true;
}

/* The hub's far end: the timing is the N64's */
static void update_transition(float dt) {
    float was = trTimer;
    trTimer += dt;
    fade = trTimer / TR_FADE;
    if (fade > 1.0f) fade = 1.0f;
    if (was < TR_WHISTLE && trTimer >= TR_WHISTLE) sfx(SFX_WHISTLE);
    if (was < TR_THUD && trTimer >= TR_THUD) sfx(SFX_THUD);
    if (was < TR_LOGO && trTimer >= TR_LOGO && !trLogo) trLogo = dc_image_load(ASSETS "ui/Logo.dt");
    if (trTimer >= TR_DONE && doorLevel < LEVEL_COUNT) { game_level_id = doorLevel; scene_change(SCENE_GAME); }
}

static void update(float dt) {
    if (dt > 0.1f) dt = 0.1f;
    const DCInput* in[MAX_PLAYERS];
    in[0] = game_demo ? demo_input(&dt) : dc_input_get(0);
    in[1] = game_coop ? dc_input_get(1) : NULL;
    const DCInput* inp = in[0];
    if (game_demo && !inp) return;          /* the demo has run out; the logo moves on */
    if (!game_demo && screens_update(inp, dt)) {    /* the tutorial box holds the game */
        if (state == FADE_IN) fade_in(dt);
        return;
    }
    shake_update(dt);
    hud_update(dt, state == PAUSED);
    fx_update(dt);
    if (hitstop > 0.0f && (state == PLAYING || state == FADE_IN)) {   /* a hit lands: everything holds */
        hitstop -= dt;
        update_cameras(in, dt);
        return;
    }
    switch (state) {
    case FADE_IN:
        fade_in(dt);
        /* fall through: he can move under the fade */
    case PLAYING:
        update_count(dt);
        if (!robot_dead()) levelTime += dt;
        FOR_PLAYERS(p) {
#ifdef BOTBOY_DEV                       /* make DEV=1 */
            if (in[p] && !game_demo && dc_input_pressed(in[p], CONT_Y) && !robot_dead()) {
                if (dc_input_held(in[p], CONT_X)) demo_record_toggle();   /* X+Y: record */
                else robot_god = !robot_god;
            }
#endif
            dark_update(&views[p], dt);
            robot_update(in[p], dt);
            robot_animate(dt);
        }
        if (game_demo) demo_after_update();
        else demo_record_frame(inp, dt);
        props_update(dt);
        if (!game_demo) enemies_update(dt);
        zones_update(dt);
        for (int p = 0; p < game_players; p++) {   /* a reset inside loops the players itself */
            robot_select(p);
            update_death(dt);
            if (state != PLAYING) break;
        }
        robot_select(0);
        catch_up();
        if (state == PLAYING && !game_demo)
            FOR_PLAYERS(p) if (in[p] && dc_input_pressed(in[p], CONT_START) && !robot_dead()) { pauser = p; pause_open(); }
        if (state == PLAYING && game_level_id == HUB_LEVEL && !game_demo) {
            int d = door_touched();
            if (d == 1) level_transition(d);    /* the road's far end */
            else if (d >= 0) level_leave(d);
        }
        break;
    case TRANSITION: update_transition(dt); break;
    case DOOR_FADE_OUT:
        robot_stop();
        fade += dt / FADE_TIME;
        if (fade >= 1.0f) {
            fade = 1.0f;
            state = DOOR_MESSAGE;
            if (doorLevel < LEVEL_COUNT) {  /* a door to a level that exists goes there; the rest say so */
                game_level_id = doorLevel;
                scene_change(SCENE_GAME);
            } else if (game_coop) {         /* past the last level: co-op has no hub to go back to */
                menu_starts_with_fade_in = true;
                scene_change(SCENE_MENU);
            }
        }
        break;
    case DOOR_MESSAGE:
        if (inp && dc_input_pressed(inp, BTN_CONFIRM)) { place_robots(); state = FADE_IN; }
        break;
    case CELEBRATE: celebration_update(inp, dt); break;
    case CUTSCENE:  cutscene_update(dt); break;
    case PAUSED:    pause_update(in[pauser], dt); return;
    }
    update_cameras(in, dt);
    if (state != PLAYING && state != FADE_IN) FOR_PLAYERS(p) robot_animate(dt);
}

static void centred(const char* text, float y, uint32_t argb) {
    ui_text(text, 160 - ui_text_width(text, 8) * 0.5f, y, 8, argb);
}

/* Black over one player's part of the screen */
static void view_fade(const View* v, float alpha) {
    if (alpha <= 0.0f) return;
    if (alpha > 1.0f) alpha = 1.0f;
    ui_rect(0, v->top, VIEW_W, v->height, (uint32_t)(alpha * 255.0f) << 24);
}

/* A number pops in with an elastic bounce over the first 40% of its time,
 * holds to 70%, then shrinks to half and fades; a small wobble throughout */
static void draw_count(void) {
    if (countDone || countTime < 0.0f) return;
    int i = (int)(countTime / COUNT_NUM);
    if (i > 3) i = 3;
    float u = (countTime - i * COUNT_NUM) / (i == 3 ? COUNT_GO : COUNT_NUM);
    float k, alpha = 1.0f;
    if (u < 0.4f) { float t = u / 0.4f; k = 1.0f + powf(2.0f, -10.0f * t) * sinf((t - 0.075f) * TAU / 0.3f); }
    else if (u < 0.7f) k = 1.0f;
    else { float t = (u - 0.7f) / 0.3f; k = 1.0f - t * 0.5f; alpha = 1.0f - t; }
    k += sinf(countTime * 20.0f) * 0.03f * k;
    float scale = COUNT_SCALE * k;
    ui_image(countImg[i], VIEW_W / 2, VIEW_H / 2, scale, scale, alpha, true);
}

/* Everything in the level, once per view: in co-op each half is a whole
 * scene of its own, drawn twice */
static void draw(void) {
    for (int p = 0; p < game_players; p++) {
        View* v = &views[p];
        dc_camera_update(&v->cam);
        dc_set_camera(&v->cam);
        draw_at(level, shz_vec3_init(0.0f, 0.0f, 0.0f), 1.0f, 0.0f);
        FOR_PLAYERS(q) {
            /* The charge pad's glow: his texture swapped for the glow picture and
             * back, a quarter second each, while it lasts */
            bool glow = robot_buff_glow() && buffImg;
            if (glow != views[q].buffWorn) { dc_model_texture(bot->model, NULL, glow ? buffImg : NULL); views[q].buffWorn = glow; }
            robot_draw();
        }
        props_draw();
        if (!game_demo) enemies_draw();
        if (state != PAUSED) { dc_particles_draw(oil); fx_draw(); }
    }
    if (game_coop) ui_rect(0, HALF_H, VIEW_W, HALF2_Y - HALF_H, 0xFF000000);     /* the band between */
    for (int p = 0; p < game_players; p++) {
        View* v = &views[p];
        robot_select(p);
        if (game_level_id != HUB_LEVEL && !game_demo) hud_draw(&v->cam, v->top, v->height);
        if (v->irisOn && v->irisR < IRIS_START) view_fade(v, 1.0f - v->irisR / IRIS_START);
        view_fade(v, v->dark);
    }
    robot_select(0);
    fx_draw_flash();
    screens_draw();
    if (state != CUTSCENE) ui_fade(fade);
    draw_count();
    if (state == TRANSITION && trLogo && trTimer >= TR_LOGO) {
        float a = (trTimer - TR_LOGO) / TR_LOGO_FADE;
        ui_image(trLogo, VIEW_W / 2, VIEW_H / 2, 1.0f, 1.0f, a > 1.0f ? 1.0f : a, true);
    }
    hud_draw_reward();
    if (robot_god) centred("GOD  stick fly  A up  B down  X fast  Y off", 8, DC_COLOR_YELLOW);
    if (demo_recording()) centred("REC", 8, DC_COLOR_RED);
    if (state == DOOR_MESSAGE) {
        char line[64];
        snprintf(line, sizeof(line), "Level %d: %s", doorLevel + 1, doorLevel < REAL_LEVEL_COUNT ? LEVEL_NAMES[doorLevel] : "?");
        centred(line, 100, DC_COLOR_WHITE);
        centred("not ported yet", 116, DC_COLOR_GRAY);
        centred("A: back to the road", 140, DC_COLOR_GRAY);
    }
}

const SceneFuncs game_scene = { init, deinit, update, draw };
