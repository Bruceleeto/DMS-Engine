/* Inside a level: what the game's files share. game.c runs the scene, the
 * camera and the light; robot.c the robot; props.c the things placed in the
 * level (the Empties in the glb); enemies.c the things that fight back;
 * screens.c the boxes drawn over it all. */
#ifndef BOTBOY_LEVEL_H
#define BOTBOY_LEVEL_H

#include "game.h"
#include "dms/dc_model.h"
#include "dms/dc_camera.h"
#include "dms/collision.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI  3.14159265f
#define TAU 6.2831853f

typedef enum { BODY_FB, BODY_TORSO, BODY_ARMS } Body;
typedef struct {
    const char* file;
    Body        body;
    shz_vec3_t  towards;        /* the way to the light */
    float       ambient;
    uint32_t    fog;            /* 0 for none */
    float       fogNear, fogFar;
    uint32_t    clear;          /* the sky */
    const char* music;
} LevelDef;
typedef enum { PLAYING, DOOR_FADE_OUT, DOOR_MESSAGE, FADE_IN, CELEBRATE, CUTSCENE, PAUSED, TRANSITION } GameState;

/* The robot's size, and what every thing measures him by. Speeds were per
 * frame at 30 fps and are per second here. */
#define ROBOT_RADIUS  8.0f
#define ROBOT_HEIGHT  20.0f
#define ROBOT_BODY_Y  10.0f             /* where the walls push him */
#define ROBOT_MIDDLE  18.0f             /* what the guns aim at */
#define STEP_UP       15.0f             /* he steps up and down this much */
#define FALL_ACCEL    (0.6f * 30.0f * 30.0f)
#define HEALTH        3
#define MAX_PLAYERS   2
#define SPIN_REACH    55.0f             /* his spin attack reaches this far */
#define MAX_IDS       4                 /* the buttons' switches; 0 is always on */

/* What he stands on that is not the level: a mover or a prop, by index */
typedef enum { SURF_NONE = -1, SURF_MOVER, SURF_PROP } SurfKind;

typedef struct {
    DMSModel*  model;
    shz_vec3_t pos;                     /* the feet */
    float      velX, velY, velZ, angle;
    bool       grounded;
    SurfKind   rideKind;
    int        riding;
    int        health;
    float      hurtTimer, painTimer, deathTimer, landTimer;
    bool       dead;                    /* the death clip, then the iris; robot_death_done() when the clip is over */
    bool       sliding;                 /* down a steep slope */
    bool       spinning;                /* the spin attack, arms or whole */
    int        spinId;                  /* counts spins: a thing is hit once per spin */
} Robot;

extern const LevelDef* def;
extern DMSModel*  level;
extern ColWorld*  col;                  /* the level's walls and floors */
/* Two player is co-op, the screen split top and bottom, one robot each on
 * the same level. `bot` is the selected one: every file that reads him
 * reads the selected one, and what touches him loops the players
 * (FOR_PLAYERS) or picks the nearest (robot_select_nearest). */
extern Robot      bots[MAX_PLAYERS];
extern Robot*     bot;
extern int        game_players;         /* 1, or 2 in co-op */
extern bool       game_coop;            /* set by the menu before the game scene starts */
void robot_select(int i);
int  robot_index(void);
/* Each player selected in turn; the one selected before is selected again
 * after (not after a break). Not for one loop inside another. */
int  robot_loop_begin(void);
int  robot_loop_next(int i);
#define FOR_PLAYERS(i) for (int i = robot_loop_begin(); i >= 0; i = robot_loop_next(i))
extern GameState  state;
extern bool       ids[MAX_IDS];         /* what the buttons have switched on */
extern float      levelTime;
extern int        deaths, boltCount, boltsTaken, game_level_id;
extern bool       screwTaken;
#define HUB_LEVEL  0
#define LEGS_LEVEL 5                    /* the first with the whole robot: its tutorial shows here */

/* game.c */
DMSModel*  load(const char* dir, const char* name);
void       light_reset(void);
void       level_respawn(void);
void       level_leave(int toLevel);    /* fade out to a level; the hub is 0 */
void       fx_hitstop(float seconds);   /* the game holds still for a moment: a hit landing */
void       fx_shake(float units);       /* the camera jolts, dying off */
void       fx_oil(shz_vec3_t at, int drops);   /* a robot's blood */
void       level_transition(int toLevel); /* the hub's far end: the whistle, the thud, the logo, then the level */
/* fx.c */
void       fx_init(void);
void       fx_free(void);
void       fx_update(float dt);
void       fx_draw(void);                       /* the particles, after the level */
void       fx_draw_flash(void);                 /* the screen flash, with the 2D things */
void       fx_dust(shz_vec3_t at, int n);       /* at his feet */
void       fx_sparks(shz_vec3_t at, int n);
void       fx_electric(shz_vec3_t at, int n);   /* round his chest, at his feet */
void       fx_trail(shz_vec3_t at, int n);
void       fx_splash(shz_vec3_t at, int n, bool lava);
void       fx_decal(shz_vec3_t at, float scale, bool lava);   /* a slime's mark on the floor */
void       fx_blob(shz_vec3_t at, float floorY, shz_vec3_t normal);  /* his shadow in the air */
void       fx_flash(uint32_t rgb, float seconds, float alpha);
/* hud.c */
void       hud_init(void);
void       hud_free(void);
void       hud_update(float dt, bool paused);
void       hud_draw(const DCCamera* cam, float top, float height);   /* the selected player's, in his part of the screen */
void       hud_bolt(void);              /* the bolt counter slides in for a while */
void       hud_screw(void);             /* the golden screw slides in for a while */
void       fx_stars(void);              /* stars circle his head: a hit, a long fall (the selected player) */
void       hud_health(void);            /* the hearts slide in, blinking, for a while (the selected player) */
void       hud_reward(void);            /* the 100% box, for a few seconds */
void       hud_draw_reward(void);       /* over everything else, drawn last */
/* robot.c */
void robot_init(Body body);             /* a robot per player */
void robot_free(void);
void robot_place(shz_vec3_t at);        /* the selected one */
void robot_update(const DCInput* inp, float dt);
void robot_animate(float dt);
void robot_draw(void);
/* The nearest living player to a spot is selected; false with none alive */
bool robot_select_nearest(shz_vec3_t at);
void robot_hurt(int damage);
extern bool robot_god;                  /* Y: no damage, flies through everything (for testing) */
void robot_knockback(shz_vec3_t from, float units, float up);
void robot_airborne(void);              /* lifted off whatever he stood on */
void robot_pad(void);                   /* a charge pad */
void robot_stop(void);
bool robot_charged(void);
static inline bool robot_dead(void) { return bot->dead; }
static inline bool robot_death_done(void) { return bot->dead && bot->deathTimer <= 0.0f; }
bool robot_buff_glow(void);             /* the charge pad's glow is on him this frame */
void robot_lock(float seconds);         /* no controls for a while (the respawn) */
/* props.c */
void props_init(void);
void props_free(void);
void props_reset(void);
void props_update(float dt);
void props_draw(void);
shz_vec3_t   props_spawn(void);         /* where he comes back: the checkpoint or the start */
bool         props_exit(shz_vec3_t* at); /* where the level's exit is, if it has one */
int          door_touched(void);        /* the level a hub door leads to, or -1 */
float        surface_under(float x, float z, float from, float low, SurfKind* kind, int* which);
ColGroundHit world_ground(shz_vec3_t origin, float reach);
shz_vec3_t   world_move(shz_vec3_t from, shz_vec3_t to, float radius);
/* enemies.c */
void enemies_init(void);
void enemies_free(void);
void enemies_reset(void);
void enemies_update(float dt);
void enemies_draw(void);
/* screens.c */
void banner_show(const char* text);
void tutorial_start(void);
bool screens_update(const DCInput* inp, float dt);    /* true while the tutorial holds the game */
void celebration_start(void);
void celebration_update(const DCInput* inp, float dt);
void cutscene_start(bool ending);              /* the level's intro slideshow, or the one that ends the game */
void cutscene_update(float dt);
void pause_open(void);
void pause_update(const DCInput* inp, float dt);
void screens_draw(void);
void screens_free(void);

/* ---- Small shared helpers ---- */

static inline float dist_xz(shz_vec3_t a, shz_vec3_t b) {
    float dx = a.x - b.x, dz = a.z - b.z;
    return sqrtf(dx * dx + dz * dz);
}
static inline float dist3(shz_vec3_t a, shz_vec3_t b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}
static inline shz_vec3_t lerp3(shz_vec3_t a, shz_vec3_t b, float t) {
    return shz_vec3_init(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}
static inline float wrap_angle(float a) {
    while (a > PI) a -= TAU;
    while (a < -PI) a += TAU;
    return a;
}
/* Turns `angle` toward `want` by at most `step`; returns how far off it was */
static inline float turn_toward(float* angle, float want, float step) {
    float d = wrap_angle(want - *angle);
    *angle += d > step ? step : d < -step ? -step : d;
    return d;
}
/* The Empty's place in world units, and its turn about y */
static inline shz_vec3_t ent_pos(const DMSEntity* e) {
    return shz_vec3_init(e->pos.x * WORLD_SCALE, e->pos.y * WORLD_SCALE, e->pos.z * WORLD_SCALE);
}
static inline float ent_yaw(const DMSEntity* e) { return atan2f(-e->rot[2], e->rot[0]); }
/* Into a prop's own space: dc_draw turns a model's x axis to (cos, 0, sin)
 * and its z axis to (-sin, 0, cos), so this is that turned back */
static inline void to_local(shz_vec3_t at, shz_vec3_t centre, float yaw, float* lx, float* lz) {
    float dx = at.x - centre.x, dz = at.z - centre.z;
    float cs = cosf(yaw), sn = sinf(yaw);
    *lx =  dx * cs + dz * sn;
    *lz = -dx * sn + dz * cs;
}
static inline float rand_f(void) {
    static uint32_t rng = 12345;
    rng = rng * 1103515245u + 12345u;
    return (float)((rng >> 16) & 0x7FFF) / 32767.0f;
}
static inline void draw_at(DMSModel* m, shz_vec3_t pos, float scale, float yaw) {
    if (m) dc_draw_ex(m, &(DCDrawOpts){ .pos = pos, .scale = WORLD_SCALE * scale, .yaw = yaw });
}
static inline void draw_stretched(DMSModel* m, shz_vec3_t pos, shz_vec3_t scale, float yaw) {
    if (m) dc_draw_ex(m, &(DCDrawOpts){ .pos = pos, .scale = WORLD_SCALE, .stretch = scale, .yaw = yaw });
}

#endif
