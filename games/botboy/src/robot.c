/* The robot: the torso alone, the arms on it, or the whole of him, each
 * with its own moves. What they share: walking with the stick, gravity,
 * the walls, the floor and whatever moving thing is under him, being hurt
 * and dying, and the clip to play. */
#include "sound.h"
#include "level.h"
#include "demo.h"

#define ROBOT_SCALE     0.40f
#define P2_TINT         0xC896FF        /* the second player, purple, as the N64 */
#define WALK_SPEED_X    (6.0f * 30.0f)
#define WALK_SPEED_Z    (6.0f * 0.7f * 30.0f)
#define STEER_RATE      15.0f           /* how fast the speed follows the stick */
#define RUN_STICK       0.5f            /* stick past this plays the run clip */
#define JUMP_SPEED      (10.0f * 30.0f)
#define FALL_SPEED_MAX  (40.0f * 30.0f)
#define FALL_LIMIT      -500.0f         /* below the level: dead */
#define HURT_TIME       1.5f            /* cannot be hurt again for this long */
#define PAIN_TIME       0.5f            /* when the body has no pain clip */
#define DEATH_TIME      1.5f
#define WALL_TIME       (8.0f / 30.0f)  /* a wall touched this recently can be kicked */
#define COYOTE          0.3f            /* a jump this soon after walking off an edge still goes */
#define JUMP_BUFFER     0.25f           /* a jump pressed this soon before landing still goes */

/* The torso jumps by charging: A stops him, holding charges, letting go
 * jumps. A tap is a hop. Landing and jumping again inside the combo window
 * charges faster and jumps harder, up to three times. */
#define HOP_TIME        0.15f
#define HOP_SPEED       (3.0f * 1.41421356f * 30.0f)
#define CHARGE_MAX      1.5f
#define CHARGE_BASE     (3.0f * 1.41421356f * 30.0f)
#define CHARGE_RATE     (2.0f * 1.41421356f * 30.0f)
#define CHARGE_FORWARD  (2.0f * 0.4f * 30.0f)        /* times (3 + 2 * charge) */
#define COMBO_WINDOW    0.5f
#define AIM_GRACE       0.1f            /* the stick let go this recently still aims the jump */
static const float COMBO_CHARGE[3] = { 1.0f, 1.5f, 2.0f };
static const float COMBO_POWER[3]  = { 1.0f, 1.3f, 1.6f };

/* The arms: a fixed jump, a kick off a wall, a spin attack on the ground and
 * one spin in the air that floats him. A charge pad doubles the float. */
#define ARMS_JUMP       (8.0f * 30.0f)
#define KICK_OUT        (4.0f * 30.0f)
#define KICK_UP         (8.0f * 30.0f)
#define KICK_GRACE      (5.0f / 30.0f)  /* not straight off the ground */
#define GLIDE_UP        (2.0f * 30.0f)
#define GLIDE_PUSH      (4.0f * 30.0f)
#define GLIDE_FALL      0.25f           /* of the gravity; half that with the pad */
#define SPIN_LOCK       0.55f           /* of the clip; after it he stops */

/* The whole robot: the arms' jump and wall kick, a spin while moving or
 * charged up from a crouch, a kick from a crouch, and from a crouch with A
 * a long jump when running or a hover when still. A charge pad gives him
 * twice the speed and nothing hurts him for a while. */
#define FB_LONG_MIN     (0.1f * 30.0f)  /* moving faster than this: a long jump, else a hover */
#define FB_LONG_UP      (9.0f * 30.0f)
#define FB_LONG_SPEED   (6.0f * 30.0f)
#define FB_LONG_END     (1.0f * 30.0f)
#define FB_HOVER_WINDUP 0.3f
#define FB_HOVER_UP     (7.0f * 30.0f)
#define FB_HOVER_TIME   1.0f
#define FB_HOVER_FALL   (2.5f * 30.0f)
#define FB_HOVER_TILT   0.6f            /* full tilt takes this much off the lift */
#define FB_AIRSPIN_FALL (3.0f * 30.0f)
#define FB_BUFF_TIME    10.0f
#define BUFF_FLASH      0.25f           /* the glow texture comes and goes this often */

/* Slopes: a floor whose normal leans past 45 degrees cannot be walked up.
 * He struggles for a moment, slowing to a stop, then slides: pulled
 * downhill by how steep it is, held back by friction, steered a little by
 * the stick, capped; on gentle ground the friction wins and he stops. */
#define SLOPE_STEEP     0.707f          /* normal.y under this: steep */
#define SLOPE_GENTLE    0.4f            /* steepness under this: he can come to a stop */
#define SLIDE_STRUGGLE  0.3f
#define SLIDE_START     (15.0f * 30.0f)
#define SLIDE_ACCEL     (25.0f * 30.0f * 30.0f)      /* times the steepness */
#define SLIDE_FRICTION  0.97f           /* left each thirtieth */
#define SLIDE_FRICTION_FLAT 0.80f
#define SLIDE_MAX       (120.0f * 30.0f)
#define SLIDE_STOP      (5.0f * 30.0f)
#define SLIDE_STEER     (0.05f * 30.0f) /* radians a second at full stick */
#define SLIDE_TURN      8.0f            /* he turns to face downhill this fast */
#define SLIDE_DUST_EVERY 0.1f

enum { A_IDLE, A_WALK, A_RUN, A_JUMP, A_PAIN, A_DEATH, A_LAND, A_JUMP2, A_JUMP3, A_CHARGE,
       A_SPIN, A_SPIN_AIR, A_SPIN_CHARGE, A_CROUCH, A_CROUCH_JUMP, A_HOVER, A_NINJA, A_KICK, A_LONG, A_WAIT,
       A_PAIN2, A_SLIDE, A_SLIDE_UP, A_COUNT };
static const char* const CLIPS[3][A_COUNT] = {
    [BODY_FB]    = { "fb_idle", "fb_walk", "fb_run", "fb_jump", "fb_pain_1", "fb_death", 0, 0, 0, 0,
                     "fb_spin_atk", "fb_spin_air", "fb_spin_charge", "fb_crouch", "fb_crouch_jump",
                     "fb_crouch_jump_hover", "fb_run_ninja", "fb_crouch_attack", "fb_long_jump", "fb_wait",
                     "fb_pain_2", "fb_slide", 0 },
    [BODY_TORSO] = { "torso_idle", "torso_walk_slow", "torso_walk_fast", "torso_jump_launch", "torso_pain_1", "torso_death",
                     "torso_jump_land", "torso_double", "torso_tripple", "torso_jump_charge",
                     0, 0, 0, 0, 0, 0, 0, 0, 0, "torso_wait",
                     "torso_pain_2", "torso_slide_front", "torso_slide_front_recover" },
    [BODY_ARMS]  = { "arms_idle", "arms_walk_1", "arms_walk_2", "arms_jump", "arms_pain_1", "arms_death",
                     "arms_jump_land", 0, 0, 0, "arms_atk_spin", 0, 0, 0, 0, 0, 0, 0, 0, 0,
                     "arms_pain_2", "arms_slide", 0 },
};

Robot  bots[MAX_PLAYERS];
Robot* bot = &bots[0];
static Body   body;
static int    a[A_COUNT];               /* the clips, the same for both players */

/* Squash and stretch: the draw scale, not the collision. Landing squashes
 * him by how far he fell and springs back fast; a jump stretches him and
 * eases back slower; the torso's charge sits on a squash. The fidget is
 * the wait clip after standing still a while. */
#define SQUASH_SPRING   16.0f
#define SQUASH_DAMP     8.0f
#define STRETCH_SPRING  8.0f
#define STRETCH_DAMP    4.0f
#define FIDGET_AFTER    7.0f
#define HARD_LANDING    200.0f          /* fallen this far, the screen shakes */

/* What each robot keeps to himself between frames, besides the Robot every
 * file sees: one of these per player, `s` the selected one's */
typedef struct {
    int    animNow;
    float  squash, landSquash, chargeSquash, squashVel, peakY;
    float  idleTime, fidgetTime;
    bool   fidget;
    /* torso */
    bool   charging, jumpBuff;
    float  chargeTime, comboTimer, holdVelX, holdVelZ, aimSx, aimSy, aimMag;
    float  lastSx, lastSy, lastMag, sinceStick;    /* the stick as it was last pushed, and how long ago */
    int    combo, jumpAnim;
    int    painClip;
    float  lockTimer;               /* no controls until this runs out */
    float  buffFlash;               /* the charge pad's glow: on for a quarter second, off for the next */
    bool   buffGlow;
    /* slopes */
    float  slideVX, slideVZ, steepTimer, recoverTimer, slideDust;
    float  groundNx, groundNy, groundNz;   /* of what he stands on */
    float  spinChargeTime, electricTimer;
    /* arms */
    bool   gliding, glided, kickLock, glideBuff;
    float  spinTime, wallTimer, wallNx, wallNz, airTime, coyote, jumpBuffer;
    /* whole */
    bool   crouching, kicking, airSpin, spinCharging, longJump;
    int    hover;                   /* 0 none, 1 winding up, 2 rising, 3 falling */
    float  hoverTime, hoverTilt, longSpeed, speedBuff;
    bool   lWas;                    /* the left trigger last frame */
} Priv;
static Priv  P[MAX_PLAYERS];
static Priv* s = &P[0];
static int   selected;
static DMSModel* arcCube;               /* the charge arc: one white cube, tinted */
static const uint32_t ARC_TINT[3] = { 0x64C8FF, 0xFFA532, 0xFF3232 };   /* blue, orange, red by combo */
#define ARC_LAND_TINT 0xFFFF64

void robot_select(int i) { selected = i; bot = &bots[i]; s = &P[i]; }
int  robot_index(void)   { return selected; }
static int loopWas;
int  robot_loop_begin(void) { loopWas = selected; robot_select(0); return 0; }
int  robot_loop_next(int i) {
    if (++i < game_players) { robot_select(i); return i; }
    robot_select(loopWas);
    return -1;
}

bool robot_select_nearest(shz_vec3_t at) {
    int best = -1;
    float bestD = 0.0f;
    for (int i = 0; i < game_players; i++) {
        if (bots[i].dead) continue;
        float d = dist3(bots[i].pos, at);
        if (best < 0 || d < bestD) { best = i; bestD = d; }
    }
    if (best < 0) return false;
    robot_select(best);
    return true;
}

static void set_anim(int clip) {
    if (clip < 0 || clip == s->animNow) return;
    dc_model_set_anim(bot->model, clip);
    s->animNow = clip;
}

static float clip_len(int clip) { float len = dc_model_anim_length(bot->model, clip); return len > 0.0f ? len : 1.0f; }

static void start_spin(bool air) {
    bot->spinning = true; s->airSpin = air; s->spinTime = 0.0f; bot->spinId++;
}

static void jump(float up) {
    bot->velY = up;
    robot_airborne();
    s->jumpBuffer = 0.0f;
    s->landSquash = s->chargeSquash = 0.0f;
    s->squash = 1.1f; s->squashVel = 1.0f;
    bot->sliding = false; s->recoverTimer = 0.0f;
    fx_dust(bot->pos, 2);
    sfx(SFX_JUMP);
}

void robot_init(Body b) {
    body = b;
    const char *dir = b == BODY_FB ? "menu" : "game", *name = b == BODY_FB ? "Robo_fb" : b == BODY_TORSO ? "Robo_torso" : "Robo_arms";
    for (int i = 0; i < game_players; i++) {    /* a model each: the clip playing lives in the model */
        bots[i].model = load(dir, name);
        P[i].animNow = -1;
        P[i].speedBuff = P[i].buffFlash = 0.0f; P[i].buffGlow = false;
    }
    for (int i = 0; i < A_COUNT; i++) a[i] = dc_model_anim_index(bots[0].model, CLIPS[b][i]);
    if (b == BODY_TORSO) arcCube = dc_model_cube();
    robot_select(0);
}

void robot_free(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) { dc_model_free(bots[i].model); bots[i].model = NULL; }
    dc_model_free(arcCube); arcCube = NULL;
    robot_select(0);
}

void robot_place(shz_vec3_t at) {
    ColGroundHit g = world_ground(shz_vec3_init(at.x, at.y + 50.0f, at.z), 300.0f);
    bot->pos = at;
    if (g.hit) bot->pos.y = g.y;
    bot->velX = bot->velY = bot->velZ = bot->angle = 0.0f;
    bot->grounded = true;
    bot->riding = -1; bot->rideKind = SURF_NONE;
    bot->health = HEALTH;
    bot->hurtTimer = bot->painTimer = bot->deathTimer = bot->landTimer = 0.0f;
    bot->dead = false; bot->sliding = false;
    s->slideVX = s->slideVZ = s->steepTimer = s->recoverTimer = s->slideDust = 0.0f;
    s->groundNx = s->groundNz = 0.0f; s->groundNy = 1.0f;
    s->spinChargeTime = s->electricTimer = 0.0f;
    s->lastSx = s->lastSy = s->lastMag = 0.0f; s->sinceStick = 1.0f;
    s->lockTimer = 0.0f;
    s->squash = 1.0f; s->landSquash = s->chargeSquash = s->squashVel = 0.0f; s->peakY = at.y;
    s->idleTime = s->fidgetTime = 0.0f; s->fidget = false;
    bot->spinning = false;
    s->charging = s->jumpBuff = s->glideBuff = false;
    s->chargeTime = s->comboTimer = 0.0f;
    s->combo = 0;
    s->jumpAnim = a[A_JUMP];
    s->gliding = s->glided = s->kickLock = false;
    s->spinTime = s->wallTimer = s->airTime = s->coyote = s->jumpBuffer = 0.0f;
    s->crouching = s->kicking = s->airSpin = s->spinCharging = s->longJump = false;
    s->hover = 0; s->hoverTime = s->hoverTilt = s->longSpeed = 0.0f;
    set_anim(a[A_IDLE]);
}

void robot_animate(float dt) { if (bot->model) dc_model_animate(bot->model, dt); }
void robot_airborne(void) { bot->grounded = false; bot->rideKind = SURF_NONE; bot->riding = -1; }
void robot_stop(void)     { bot->velX = bot->velZ = 0.0f; s->charging = false; }
bool robot_charged(void)  { return s->jumpBuff || s->glideBuff || s->speedBuff > 0.0f; }
bool robot_buff_glow(void) { return s->buffGlow; }
void robot_lock(float seconds) { s->lockTimer = seconds; }

void robot_pad(void) {
    if (body == BODY_ARMS) s->glideBuff = true;
    else if (body == BODY_FB) s->speedBuff = FB_BUFF_TIME;
    else s->jumpBuff = true;
}

bool robot_god;

void robot_hurt(int damage) {
    if (robot_god || game_demo || bot->hurtTimer > 0.0f || robot_dead() || s->speedBuff > 0.0f) return;
    bot->health -= damage;
    s->charging = false;
    bot->sliding = false;
    fx_hitstop(0.08f);
    fx_shake(8.0f);
    fx_stars();
    fx_flash(0xFF0000, 0.15f, 100.0f / 255.0f);
    sfx(SFX_DAMAGE);
    hud_health();
    if (bot->health <= 0) {
        deaths++;
        fx_shake(15.0f);
        bot->dead = true;
        bot->deathTimer = a[A_DEATH] >= 0 ? clip_len(a[A_DEATH]) : DEATH_TIME;
        bot->velX = bot->velZ = 0.0f;
    } else {                            /* one of the two pain clips, and no controls until it is over */
        bot->hurtTimer = HURT_TIME;
        s->painClip = a[A_PAIN2] >= 0 && rand_f() < 0.5f ? a[A_PAIN2] : a[A_PAIN];
        bot->painTimer = s->painClip >= 0 ? clip_len(s->painClip) : PAIN_TIME;
        s->animNow = -1;
    }
}

/* Shoved straight away from a spot, through the walls check, and lifted a little */
void robot_knockback(shz_vec3_t from, float units, float up) {
    float dx = bot->pos.x - from.x, dz = bot->pos.z - from.z;
    float d = sqrtf(dx * dx + dz * dz);
    if (d < 0.001f) { dx = 0.0f; dz = 1.0f; d = 1.0f; }
    shz_vec3_t at  = shz_vec3_init(bot->pos.x, bot->pos.y + ROBOT_BODY_Y, bot->pos.z);
    shz_vec3_t to  = shz_vec3_init(at.x + dx / d * units, at.y, at.z + dz / d * units);
    shz_vec3_t out = world_move(at, to, ROBOT_RADIUS);
    bot->pos.x = out.x; bot->pos.z = out.z;
    bot->velY = up;
    robot_airborne();
}

/* ---- The bodies' moves, before the walking and falling ---- */

static void wall_kick(void);

/* A starts the charge: on landing inside the window it is the next of the
 * combo, and A pressed during the landing clip cuts the clip short */
static void start_charge(void) {
    s->charging = true;
    s->chargeTime = 0.0f;
    s->combo = s->comboTimer > 0.0f && s->combo < 2 ? s->combo + 1 : 0;
    s->comboTimer = 0.0f;
    bot->landTimer = 0.0f;
    s->holdVelX = bot->velX; s->holdVelZ = bot->velZ;
    bot->velX = bot->velZ = 0.0f;
}

static void update_torso(const DCInput* inp, float sx, float sy, float mag, float dt) {
    bool aPress = dc_input_pressed(inp, CONT_A);
    if (!bot->grounded) {                /* a kick off a wall he just touched, rising or falling */
        if (aPress && s->wallTimer > 0.0f && s->airTime > KICK_GRACE) {
            wall_kick();
            s->combo = 0; s->comboTimer = 0.0f;
            s->jumpAnim = a[A_JUMP];
        }
        return;
    }
    if (!s->charging && aPress && !bot->sliding && s->recoverTimer <= 0.0f) start_charge();
    if (!s->charging) return;
    s->chargeTime += dt * COMBO_CHARGE[s->combo];
    if (mag > 0.3f) bot->angle = atan2f(-sx, -sy);
    /* the stick let go just before the release still aims the jump */
    if (mag < 0.1f && s->sinceStick < AIM_GRACE) { sx = s->lastSx; sy = s->lastSy; mag = s->lastMag; }
    s->aimSx = sx; s->aimSy = sy; s->aimMag = mag;
    if (dc_input_pressed(inp, CONT_B)) { s->charging = false; return; }
    if (!(inp->released & CONT_A)) return;
    s->charging = false;
    robot_airborne();
    s->landSquash = s->chargeSquash = 0.0f;
    if (s->chargeTime < HOP_TIME) {
        bot->velY = HOP_SPEED;
        bot->velX = s->holdVelX; bot->velZ = s->holdVelZ;
        s->jumpAnim = a[A_JUMP];
        s->squash = 1.1f; s->squashVel = 1.0f;
    } else {
        float t = s->chargeTime < CHARGE_MAX ? s->chargeTime : CHARGE_MAX;
        float power = COMBO_POWER[s->combo];
        bot->velY = (CHARGE_BASE + t * CHARGE_RATE) * power;
        if (s->jumpBuff) { bot->velY *= 2.0f; s->jumpBuff = false; }
        float forward = (3.0f + 2.0f * t) * CHARGE_FORWARD * mag * power;
        bot->velX = mag > 0.1f ?  sx / mag * forward : 0.0f;
        bot->velZ = mag > 0.1f ? -sy / mag * forward : 0.0f;
        s->jumpAnim = s->combo == 2 && a[A_JUMP3] >= 0 ? a[A_JUMP3] : s->combo == 1 && a[A_JUMP2] >= 0 ? a[A_JUMP2] : a[A_JUMP];
        s->squash = 1.1f + 0.15f * t / CHARGE_MAX; s->squashVel = 1.0f;   /* stretched by the charge */
        fx_dust(bot->pos, 2);
        sfx(SFX_JUMP);
    }
    set_anim(s->jumpAnim);
}

/* Kicks off the wall he just touched: facing mirrored over it */
static void wall_kick(void) {
    float fx = -sinf(bot->angle), fz = cosf(bot->angle);
    float d = fx * s->wallNx + fz * s->wallNz;
    float rx = fx - 2.0f * d * s->wallNx, rz = fz - 2.0f * d * s->wallNz;
    float len = sqrtf(rx * rx + rz * rz);
    if (len < 0.001f) { rx = s->wallNx; rz = s->wallNz; len = 1.0f; }
    bot->velX = rx / len * KICK_OUT; bot->velZ = rz / len * KICK_OUT;
    bot->velY = KICK_UP;
    bot->angle = atan2f(-rx, rz);
    s->kickLock = true;
    s->wallTimer = 0.0f;
    bot->sliding = false;
    s->animNow = -1;                       /* the jump clip from the start */
    fx_dust(shz_vec3_init(bot->pos.x - s->wallNx * 5.0f, bot->pos.y, bot->pos.z - s->wallNz * 5.0f), 3);
    sfx(SFX_JUMP);
}

/* Sparks of electricity round him while a spin runs, and a burst as a
 * charged spin lets go */
static void spin_electric(float dt) {
    if (bot->spinning) {
        s->electricTimer += dt;
        if (s->electricTimer >= 0.1f) { s->electricTimer -= 0.1f; fx_electric(bot->pos, 2); }
    } else s->electricTimer = 0.0f;
}

/* The spin's clock and its slowing him down; true while it runs */
static void run_spin(float dt) {
    if (!bot->spinning) return;
    int clip = s->kicking ? a[A_KICK] : s->airSpin ? a[A_SPIN_AIR] : a[A_SPIN];
    float len = clip_len(clip);
    s->spinTime += dt;
    if (s->kicking) { float k = powf(0.5f, 30.0f * dt); bot->velX *= k; bot->velZ *= k; }
    else if (!s->airSpin && !s->gliding && bot->grounded && s->spinTime > len * SPIN_LOCK) {
        float k = powf(0.8f, 30.0f * dt);
        bot->velX *= k; bot->velZ *= k;
    }
    if (s->spinTime >= len || (s->airSpin && bot->grounded)) {
        if (!s->airSpin && !s->gliding) bot->velX = bot->velZ = 0.0f;
        bot->spinning = s->kicking = s->airSpin = false;
    }
}

static void update_arms(const DCInput* inp, float dt) {
    bool aPress = dc_input_pressed(inp, CONT_A), bPress = dc_input_pressed(inp, CONT_B);
    run_spin(dt);
    spin_electric(dt);
    if (s->gliding && (!bot->spinning || bot->grounded)) { s->gliding = false; s->glideBuff = false; }
    if (bot->grounded) {
        s->kickLock = s->glided = false;
        if (bPress && !bot->spinning) start_spin(false);
        if (aPress && !bot->spinning) s->jumpBuffer = JUMP_BUFFER;
        if ((s->jumpBuffer > 0.0f || aPress) && bot->landTimer <= 0.0f) { jump(ARMS_JUMP); bot->landTimer = 0.0f; }
        return;
    }
    /* in the air: a jump off the ground he just left, a kick off a wall he
     * just touched, or the one spin that floats him */
    if (aPress && s->coyote > 0.0f && !bot->spinning) { bot->velY = ARMS_JUMP; s->coyote = 0.0f; return; }
    if (aPress && s->wallTimer > 0.0f && s->airTime > KICK_GRACE) { wall_kick(); s->glided = false; return; }
    if (aPress) s->jumpBuffer = JUMP_BUFFER;
    if (bPress && !s->glided && !s->gliding) {
        s->gliding = s->glided = true;
        start_spin(false);
        s->kickLock = false;
        bot->velY = GLIDE_UP;
        bot->velX += -sinf(bot->angle) * GLIDE_PUSH;
        bot->velZ +=  cosf(bot->angle) * GLIDE_PUSH;
    }
}

static void update_fb(const DCInput* inp, float sx, float sy, float mag, float dt) {
    bool aPress = dc_input_pressed(inp, CONT_A), bPress = dc_input_pressed(inp, CONT_B);
    bool bHeld  = dc_input_held(inp, CONT_B), xPress = dc_input_pressed(inp, CONT_X);
    bool lHeld  = inp->ltrig > 0.5f;
    bool lPress = lHeld && !s->lWas;
    s->lWas = lHeld;

    if (s->speedBuff > 0.0f) s->speedBuff -= dt;
    run_spin(dt);
    spin_electric(dt);
    if (s->spinCharging) {                 /* it crackles more the longer it is held */
        s->spinChargeTime += dt;
        s->electricTimer += dt;
        if (s->electricTimer >= 2.0f / 30.0f) {
            s->electricTimer = 0.0f;
            int n = 3 + (int)(s->spinChargeTime * 2.0f);
            fx_electric(bot->pos, n > 8 ? 8 : n);
        }
    }

    if (bot->grounded) {
        s->kickLock = s->longJump = false;
        if (s->hover == 1) {                       /* winding up the hover */
            bot->velX = bot->velZ = 0.0f;
            s->hoverTime += dt;
            if (lPress) { s->hover = 0; return; }
            if (s->hoverTime >= FB_HOVER_WINDUP) {
                s->hover = 2; s->hoverTime = s->hoverTilt = 0.0f;
                jump(FB_HOVER_UP);
            }
            return;
        }
        s->hover = 0;
        s->crouching = lHeld && !bot->spinning;
        if (s->crouching) {
            float k = powf(0.95f, 30.0f * dt);
            bot->velX *= k; bot->velZ *= k;
            if (xPress) { start_spin(false); s->kicking = true; s->crouching = false; }
            else if (bHeld) s->spinCharging = true;
            else if (s->spinCharging) {
                s->spinCharging = false; start_spin(false);
                fx_electric(bot->pos, 10 + (int)(s->spinChargeTime * 5.0f));
                s->spinChargeTime = 0.0f;
            }
            else if (aPress) {
                if (sqrtf(bot->velX * bot->velX + bot->velZ * bot->velZ) > FB_LONG_MIN) {
                    s->longJump = true; s->longSpeed = FB_LONG_SPEED;
                    bot->velX = -sinf(bot->angle) * s->longSpeed; bot->velZ = cosf(bot->angle) * s->longSpeed;
                    s->crouching = false;
                    jump(FB_LONG_UP);
                    fx_dust(bot->pos, 2);
                } else { s->hover = 1; s->hoverTime = 0.0f; }
            }
            return;
        }
        s->spinCharging = false; s->spinChargeTime = 0.0f;
        if (bPress && !bot->spinning && mag > 0.0f) start_spin(false);
        if (aPress && !bot->spinning) s->jumpBuffer = JUMP_BUFFER;
        if ((s->jumpBuffer > 0.0f || aPress) && bot->landTimer <= 0.0f && !bot->spinning) jump(JUMP_SPEED);
        return;
    }

    /* in the air */
    s->crouching = s->spinCharging = false;
    if (s->hover == 2) {                           /* rising: the stick tilts him and steers */
        s->hoverTime += dt;
        float k = 8.0f * dt; if (k > 1.0f) k = 1.0f;
        s->hoverTilt += (mag - s->hoverTilt) * k;
        bot->velY = FB_HOVER_UP * (1.0f - FB_HOVER_TILT * s->hoverTilt);
        if (mag > 0.5f) {
            bot->velX = sx / mag * FB_HOVER_UP * s->hoverTilt;
            bot->velZ = -sy / mag * FB_HOVER_UP * s->hoverTilt;
            bot->angle = atan2f(-bot->velX, bot->velZ);
        } else {
            float f = powf(0.9f, 30.0f * dt);
            bot->velX *= f; bot->velZ *= f;
        }
        if (s->hoverTime >= FB_HOVER_TIME) s->hover = 3;
        if (lPress) s->hover = 0;
        return;
    }
    if (s->hover == 3 && lPress) s->hover = 0;
    if (s->longJump) {
        s->longSpeed *= powf(0.97f, 30.0f * dt);
        bot->velX = -sinf(bot->angle) * s->longSpeed; bot->velZ = cosf(bot->angle) * s->longSpeed;
        if (s->longSpeed < FB_LONG_END) s->longJump = false;
        if (bPress && !bot->spinning) { s->longJump = false; start_spin(true); }
        return;
    }
    if (aPress && s->coyote > 0.0f && !bot->spinning) { bot->velY = JUMP_SPEED; s->coyote = 0.0f; return; }
    if (aPress && s->wallTimer > 0.0f && s->airTime > KICK_GRACE && s->hover == 0) { wall_kick(); return; }
    if (aPress) s->jumpBuffer = JUMP_BUFFER;
    if (bPress && !bot->spinning && s->hover == 0) start_spin(true);
}

/* ---- Every body ---- */

void robot_update(const DCInput* inp, float dt) {
    static const DCInput none;
    if (s->lockTimer > 0.0f) s->lockTimer -= dt;
    /* No controls while dead, stunned by a hit, or locked out after a respawn */
    if (!inp || robot_dead() || bot->painTimer > 0.0f || s->lockTimer > 0.0f) inp = &none;
    bool torso = body == BODY_TORSO, arms = body == BODY_ARMS, fb = body == BODY_FB;
    bool dead = robot_dead();

    /* The charge pad's glow comes and goes on him */
    if (robot_charged() && !dead) {
        s->buffFlash += dt;
        if (s->buffFlash >= BUFF_FLASH) { s->buffFlash -= BUFF_FLASH; s->buffGlow = !s->buffGlow; }
    } else { s->buffFlash = 0.0f; s->buffGlow = false; }

    if (robot_god) {                    /* fly: stick moves, A up, B down, X fast */
        float fs = inp->stick_x, fz = inp->stick_y;
        float speed = dc_input_held(inp, CONT_X) ? 600.0f : 250.0f;
        bot->pos.x += fs * speed * dt;
        bot->pos.z += fz * speed * dt;
        if (dc_input_held(inp, CONT_A)) bot->pos.y += speed * dt;
        if (dc_input_held(inp, CONT_B)) bot->pos.y -= speed * dt;
        if (fs * fs + fz * fz > 0.04f) bot->angle = atan2f(fs, fz);
        bot->velX = bot->velY = bot->velZ = 0.0f;
        bot->grounded = true; bot->riding = -1; bot->rideKind = SURF_NONE;
        bot->health = HEALTH; bot->hurtTimer = 0.0f;
        s->charging = false; bot->spinning = false;
        return;
    }

    /* The camera is at +z looking back along -z: stick right is +x, up is -z */
    float sx = inp->stick_x, sy = -inp->stick_y;
    float mag = sqrtf(sx * sx + sy * sy);
    if (mag > 1.0f) { sx /= mag; sy /= mag; mag = 1.0f; }
    if (mag < 0.2f) { sx = sy = 0.0f; mag = 0.0f; }
    if (mag > 0.1f) { s->lastSx = sx; s->lastSy = sy; s->lastMag = mag; s->sinceStick = 0.0f; }
    else s->sinceStick += dt;

    if (s->wallTimer > 0.0f) s->wallTimer -= dt;
    if (s->recoverTimer > 0.0f) s->recoverTimer -= dt;
    if (s->coyote > 0.0f) s->coyote -= dt;
    if (s->jumpBuffer > 0.0f) s->jumpBuffer -= dt;
    s->airTime = bot->grounded ? 0.0f : s->airTime + dt;
    if (!dead) {
        if (torso) update_torso(inp, sx, sy, mag, dt);
        if (arms)  update_arms(inp, dt);
        if (fb)    update_fb(inp, sx, sy, mag, dt);
    }

    /* The torso cannot steer in the air or while charging; the arms can,
     * but not after a wall kick or late in a spin; the whole robot the same
     * and not in the middle of a special move */
    bool spinLocked = bot->spinning && !s->gliding && !s->airSpin && s->spinTime > clip_len(a[A_SPIN]) * SPIN_LOCK;
    bool steer = !dead && !s->charging && (!torso || bot->grounded) && !bot->sliding && s->recoverTimer <= 0.0f
              && !(arms && (s->kickLock || spinLocked))
              && !(fb && (s->kickLock || spinLocked || s->crouching || s->kicking || s->spinCharging || s->longJump || s->hover == 1 || s->hover == 2));
    if (steer) {
        float boost = fb && s->speedBuff > 0.0f ? 2.0f : 1.0f;
        float wantX = sx * WALK_SPEED_X * boost, wantZ = -sy * WALK_SPEED_Z * boost;
        float k = STEER_RATE * dt;
        if (k > 1.0f) k = 1.0f;
        bot->velX += (wantX - bot->velX) * k;
        bot->velZ += (wantZ - bot->velZ) * k;
        if (mag > 0.0f) { bot->angle = atan2f(-wantX, wantZ); bot->landTimer = 0.0f; }
    }

    /* Steep ground: the struggle, then the slide. The slide's speed is his
     * speed, so the walls and the ground below are the same as walking. */
    float steep = sqrtf(s->groundNx * s->groundNx + s->groundNz * s->groundNz);
    float downX = steep > 0.001f ? s->groundNx / steep : 0.0f, downZ = steep > 0.001f ? s->groundNz / steep : 0.0f;
    if (bot->grounded && !dead && !bot->sliding && s->recoverTimer <= 0.0f && s->groundNy < SLOPE_STEEP) {
        s->steepTimer += dt;
        float t = s->steepTimer / SLIDE_STRUGGLE;
        if (t > 1.0f) t = 1.0f;
        float k = powf(1.0f - t, 30.0f * dt);
        bot->velX *= k; bot->velZ *= k;
        if (s->steepTimer >= SLIDE_STRUGGLE) {
            bot->sliding = true;
            if (s->charging) { s->charging = false; s->landSquash = s->chargeSquash = 0.0f; s->squash = 1.0f; s->squashVel = 0.0f; }
            s->slideVX = downX * SLIDE_START; s->slideVZ = downZ * SLIDE_START;
        }
    } else if (!bot->sliding) s->steepTimer = 0.0f;
    if (bot->sliding && bot->grounded) {
        float speed = sqrtf(s->slideVX * s->slideVX + s->slideVZ * s->slideVZ);
        float downhill = s->slideVX * downX + s->slideVZ * downZ;
        if (steep > 0.05f && downhill < 0.0f && speed < 20.0f * 30.0f) {  /* going up, slowly: turned downhill */
            float min = (10.0f + steep * 30.0f) * 30.0f;
            s->slideVX = downX * min; s->slideVZ = downZ * min;
            speed = min;
        }
        if (mag > 0.1f && speed > 30.0f) {  /* the stick turns him, the speed stays */
            float k = mag * SLIDE_STEER * dt, ox = s->slideVX, oz = s->slideVZ;
            s->slideVX += oz * sx * k; s->slideVZ -= ox * (-sy) * k;
            float n = sqrtf(s->slideVX * s->slideVX + s->slideVZ * s->slideVZ);
            if (n > 0.1f) { s->slideVX *= speed / n; s->slideVZ *= speed / n; }
        }
        float accel = SLIDE_ACCEL, friction = SLIDE_FRICTION;
        if (steep < SLOPE_GENTLE) {
            float t = steep / SLOPE_GENTLE;
            accel *= t * t;
            friction = SLIDE_FRICTION_FLAT + t * (SLIDE_FRICTION - SLIDE_FRICTION_FLAT);
        }
        if (steep > 0.01f) { s->slideVX += accel * steep * downX * dt; s->slideVZ += accel * steep * downZ * dt; }
        float f = powf(friction, 30.0f * dt);
        s->slideVX *= f; s->slideVZ *= f;
        speed = sqrtf(s->slideVX * s->slideVX + s->slideVZ * s->slideVZ);
        if (speed > SLIDE_MAX) { s->slideVX *= SLIDE_MAX / speed; s->slideVZ *= SLIDE_MAX / speed; speed = SLIDE_MAX; }
        if (speed > 1.0f) turn_toward(&bot->angle, atan2f(-s->slideVX, s->slideVZ), SLIDE_TURN * dt);
        if (steep < SLOPE_GENTLE && speed < SLIDE_STOP) {   /* the friction won */
            s->slideVX = s->slideVZ = 0.0f;
            bot->sliding = false; s->steepTimer = 0.0f;
            if (a[A_SLIDE_UP] >= 0) { s->recoverTimer = clip_len(a[A_SLIDE_UP]); s->animNow = -1; }
        } else if (speed > SLIDE_STOP) {
            s->slideDust += dt;
            if (s->slideDust >= SLIDE_DUST_EVERY) {
                s->slideDust = 0.0f;
                fx_dust(shz_vec3_init(bot->pos.x - s->slideVX / speed * 8.0f, bot->pos.y, bot->pos.z - s->slideVZ / speed * 8.0f), 1);
            }
        }
        bot->velX = s->slideVX; bot->velZ = s->slideVZ;
    }
    if (bot->sliding && !bot->grounded) { bot->velX = s->slideVX; bot->velZ = s->slideVZ; }   /* off the edge with the speed kept */
    if (s->recoverTimer > 0.0f) bot->velX = bot->velZ = 0.0f;

    /* Sideways through the walls in short steps */
    shz_vec3_t moved = bot->pos;
    float dx = bot->velX * dt, dz = bot->velZ * dt;
    if (col) {
        int steps = (int)(sqrtf(dx * dx + dz * dz) / (ROBOT_RADIUS * 0.5f)) + 1;
        if (steps > 8) steps = 8;
        for (int i = 0; i < steps; i++) {
            shz_vec3_t at  = shz_vec3_init(moved.x, moved.y + ROBOT_BODY_Y, moved.z);
            shz_vec3_t to  = shz_vec3_init(moved.x + dx / steps, at.y, moved.z + dz / steps);
            shz_vec3_t out = world_move(at, to, ROBOT_RADIUS);
            if (!bot->grounded) {            /* pushed out of a wall: remember which way it faces */
                float nx = out.x - to.x, nz = out.z - to.z, len = sqrtf(nx * nx + nz * nz);
                if (len > 0.3f) { s->wallNx = nx / len; s->wallNz = nz / len; s->wallTimer = WALL_TIME; }
            }
            moved.x = out.x; moved.z = out.z;
        }
    } else {
        moved.x += dx; moved.z += dz;
    }

    /* Fall, then look for the ground from a step above the feet down to
     * where they would end up: a floor crossed this frame is landed on. On
     * the ground he follows it down a step too, so a slope does not count
     * as leaving it. */
    if (!bot->grounded && s->hover != 2) {
        bot->velY -= FALL_ACCEL * (s->gliding ? (s->glideBuff ? GLIDE_FALL * 0.5f : GLIDE_FALL) : 1.0f) * dt;
        float cap = s->hover == 3 ? FB_HOVER_FALL : s->airSpin ? FB_AIRSPIN_FALL : FALL_SPEED_MAX;
        if (bot->velY < -cap) bot->velY = -cap;
    }
    float newY = moved.y + (bot->grounded ? -STEP_UP : bot->velY * dt);
    bool landed = false;
    int  on = -1;
    SurfKind onKind = SURF_NONE;
    if (col) {
        float from = moved.y + STEP_UP;
        ColGroundHit g = world_ground(shz_vec3_init(moved.x, from, moved.z), from - newY + 1.0f);
        if (g.hit && bot->velY <= 0.0f && g.y >= newY) {
            newY = g.y; landed = true;
            s->groundNx = g.normal.x; s->groundNy = g.normal.y; s->groundNz = g.normal.z;
        }
        if (bot->velY <= 0.0f) {             /* a moving thing counts as ground too, the higher of the two */
            float top = surface_under(moved.x, moved.z, from, newY - 1.0f, &onKind, &on);
            if (onKind != SURF_NONE && top >= newY) { newY = top; landed = true; s->groundNx = s->groundNz = 0.0f; s->groundNy = 1.0f; }
            else { onKind = SURF_NONE; on = -1; }
        }
        if (bot->velY > 0.0f) {              /* a ceiling met by the top of the head stops the jump */
            ColRayHit h = col_raycast(col, shz_vec3_init(moved.x, moved.y + ROBOT_HEIGHT, moved.z),
                                      shz_vec3_init(0.0f, 1.0f, 0.0f), newY - moved.y + 1.0f);
            if (h.hit && h.pos.y - ROBOT_HEIGHT < newY) { newY = h.pos.y - ROBOT_HEIGHT; bot->velY = 0.0f; }
        }
    }
    if (!landed && bot->grounded) { newY = moved.y; s->coyote = COYOTE; }   /* walked off an edge */
    moved.y = newY;
    if (!landed && moved.y > s->peakY) s->peakY = moved.y;
    if (landed && !bot->grounded) {
        s->kickLock = false;
        float fell = s->peakY - newY, hard = (fell - 30.0f) / 170.0f, fallSpeed = -bot->velY;
        if (hard > 1.0f) hard = 1.0f;
        if (hard > 0.0f) { s->landSquash = hard * 0.75f; s->chargeSquash = 0.0f; s->squash = 1.0f - s->landSquash; s->squashVel = 0.0f; }
        if (fell > 20.0f) fx_dust(shz_vec3_init(moved.x, newY, moved.z), 2 + (int)(hard * 2.0f));
        if (fell > HARD_LANDING) { fx_shake(6.0f + (fell - HARD_LANDING) / 50.0f); fx_stars(); sfx(SFX_LAND); }
        if (fb) {
            if (s->hover || s->longJump || s->airSpin) bot->landTimer = 0.1f;   /* no bounce straight off a special jump */
            s->hover = 0; s->longJump = false;
        }
        if (arms && s->jumpBuffer <= 0.0f && !dc_input_held(inp, CONT_A) && mag == 0.0f) bot->landTimer = a[A_LAND] >= 0 ? clip_len(a[A_LAND]) : 0.0f;
        if (torso) {                        /* lands hard: stops, and a jump inside the window combos */
            bot->velX = bot->velZ = 0.0f;
            s->comboTimer = COMBO_WINDOW;
            bot->landTimer = a[A_LAND] >= 0 ? clip_len(a[A_LAND]) : 0.0f;
            if (dc_input_held(inp, CONT_A) && !dead) { start_charge(); s->chargeTime = 0.01f; }   /* A held: straight into the next */
        }
        /* Onto a steep slope: the fall becomes a slide, harder the steeper it is */
        if (s->groundNy < SLOPE_STEEP && s->recoverTimer <= 0.0f && !dead) {
            float sp = sqrtf(s->groundNx * s->groundNx + s->groundNz * s->groundNz);
            float dx = sp > 0.001f ? s->groundNx / sp : 0.0f, dz = sp > 0.001f ? s->groundNz / sp : 0.0f;
            float m = fallSpeed / 30.0f * (0.5f + (1.0f - s->groundNy)) * 2.0f;
            if (m < 15.0f) m = 15.0f;
            float along = (bot->velX * dx + bot->velZ * dz) / 30.0f;
            if (along > 0.0f) m += along * 0.5f;
            s->slideVX = dx * m * 30.0f; s->slideVZ = dz * m * 30.0f;
            bot->sliding = true; s->steepTimer = SLIDE_STRUGGLE;
            s->charging = false;
            bot->velX = s->slideVX; bot->velZ = s->slideVZ;
        }
    }
    if (landed) bot->velY = 0.0f;
    bot->grounded = landed;
    bot->riding = on;
    bot->rideKind = onKind;
    bot->pos = moved;

    if (landed) s->peakY = newY;
    if (s->charging) s->chargeSquash = (s->chargeTime < CHARGE_MAX ? s->chargeTime / CHARGE_MAX : 1.0f) * 0.25f;
    if (bot->grounded || s->charging) {
        s->squashVel += -s->landSquash * SQUASH_SPRING * dt;
        s->squashVel *= 1.0f - SQUASH_DAMP * dt;
        s->landSquash += s->squashVel * dt;
        if (s->landSquash < 0.0f) s->landSquash = 0.0f;
        s->squash = 1.0f - s->landSquash - s->chargeSquash;
    } else {
        s->squashVel += (1.0f - s->squash) * STRETCH_SPRING * dt;
        s->squashVel *= 1.0f - STRETCH_DAMP * dt;
        s->squash += s->squashVel * dt;
    }
    if (s->squash < 0.05f) s->squash = 0.05f;
    if (s->squash > 1.3f) s->squash = 1.3f;

    if (s->comboTimer > 0.0f) s->comboTimer -= dt;
    if (bot->landTimer > 0.0f) bot->landTimer -= dt;
    if (bot->hurtTimer > 0.0f) bot->hurtTimer -= dt;
    if (bot->painTimer > 0.0f) bot->painTimer -= dt;
    if (bot->pos.y < FALL_LIMIT) robot_hurt(HEALTH);
    if (dead && bot->deathTimer > 0.0f) bot->deathTimer -= dt;     /* the clip; game.c takes it from there */

    /* The clip follows the stick, not the speed; every change restarts it */
    if (dead)                                       set_anim(a[A_DEATH]);
    else if (bot->painTimer > 0.0f && s->painClip >= 0)  set_anim(s->painClip);
    else if (bot->sliding && a[A_SLIDE] >= 0)        set_anim(a[A_SLIDE]);
    else if (s->recoverTimer > 0.0f && a[A_SLIDE_UP] >= 0) set_anim(a[A_SLIDE_UP]);
    else if (s->charging)                              set_anim(s->chargeTime >= HOP_TIME && a[A_CHARGE] >= 0 ? a[A_CHARGE] : a[A_IDLE]);
    else if (s->hover == 1)                            set_anim(a[A_CROUCH_JUMP]);
    else if (s->hover)                                 set_anim(a[A_HOVER]);
    else if (s->longJump)                              set_anim(a[A_LONG]);
    else if (bot->spinning)                          set_anim(s->kicking ? a[A_KICK] : s->airSpin ? a[A_SPIN_AIR] : a[A_SPIN]);
    else if (s->spinCharging)                          set_anim(a[A_SPIN_CHARGE]);
    else if (s->crouching)                             set_anim(a[A_CROUCH]);
    else if (!bot->grounded)                         set_anim(torso ? s->jumpAnim : a[A_JUMP]);
    else if (bot->landTimer > 0.0f && mag == 0.0f && a[A_LAND] >= 0) set_anim(a[A_LAND]);
    else if (mag > 0.0f) set_anim(mag > RUN_STICK ? (fb && s->speedBuff > 0.0f && a[A_NINJA] >= 0 ? a[A_NINJA] : a[A_RUN]) : a[A_WALK]);
    else {                                          /* standing: idle, and the fidget once in a while */
        s->idleTime += dt;
        if (s->fidget && (s->fidgetTime += dt) >= clip_len(a[A_WAIT])) { s->fidget = false; s->idleTime = 0.0f; }
        else if (!s->fidget && s->idleTime >= FIDGET_AFTER && a[A_WAIT] >= 0) { s->fidget = true; s->fidgetTime = 0.0f; }
        set_anim(s->fidget ? a[A_WAIT] : a[A_IDLE]);
    }
    if (s->animNow != a[A_IDLE] && s->animNow != a[A_WAIT]) { s->idleTime = 0.0f; s->fidget = false; }
}

/* The charge arc: the jump stepped the way robot_update would, at 30 Hz,
 * a dot every few steps and the marker where the ground is crossed */
static void draw_arc(void) {
    if (!s->charging || s->chargeTime < HOP_TIME || !arcCube) return;
    const float STEP = 1.0f / 30.0f;
    float t = s->chargeTime < CHARGE_MAX ? s->chargeTime : CHARGE_MAX;
    float power = COMBO_POWER[s->combo];
    float vy = (CHARGE_BASE + t * CHARGE_RATE) * power * (s->jumpBuff ? 2.0f : 1.0f);
    float forward = (3.0f + 2.0f * t) * CHARGE_FORWARD * s->aimMag * power;
    float vx = s->aimMag > 0.1f ?  s->aimSx / s->aimMag * forward : 0.0f;
    float vz = s->aimMag > 0.1f ? -s->aimSy / s->aimMag * forward : 0.0f;
    shz_vec3_t p = bot->pos;
    float landY = bot->pos.y;
    int dots = 0;
    for (int step = 0; step < 60; step++) {
        p.x += vx * STEP; p.y += vy * STEP; p.z += vz * STEP;
        vy -= FALL_ACCEL * STEP;
        if (col && step % 10 == 0) {
            ColGroundHit g = col_ground(col, shz_vec3_init(p.x, p.y + STEP_UP, p.z), 400.0f);
            if (g.hit) landY = g.y;
        }
        if (p.y <= landY) break;
        if (step > 2 && step % 4 == 0 && dots < 6) {
            dc_draw_ex(arcCube, &(DCDrawOpts){ .pos = p, .scale = 3.0f, .yaw = step * 0.2f, .tint = ARC_TINT[s->combo] });
            dots++;
        }
    }
    dc_draw_ex(arcCube, &(DCDrawOpts){ .pos = shz_vec3_init(p.x, landY + 3.0f, p.z), .scale = 5.0f, .tint = ARC_LAND_TINT });
}

/* In the air his shadow is thrown straight down onto the floor below,
 * fainter the higher he is, so a landing can be judged */
/* In the air, the N64's blob on the floor below him (on the ground he has no
 * shadow, as there). One ground ray a frame while airborne */
static void air_shadow(void) {
    if (bot->grounded || !col) return;
    ColGroundHit g = world_ground(shz_vec3_init(bot->pos.x, bot->pos.y + 1.0f, bot->pos.z), 150.0f);
    if (g.hit) fx_blob(bot->pos, g.y, g.normal);
}

void robot_draw(void) {
    bool blink = bot->hurtTimer > 0.0f && ((int)(bot->hurtTimer * 10.0f) & 1);
    float wide = ROBOT_SCALE * (1.0f + (1.0f - s->squash) * 0.5f);   /* what Y loses, XZ gains */
    if (!blink && bot->model)
        dc_draw_ex(bot->model, &(DCDrawOpts){ .pos = bot->pos, .scale = WORLD_SCALE,
                   .stretch = shz_vec3_init(wide, ROBOT_SCALE * s->squash, wide), .yaw = bot->angle,
                   .tint = selected == 1 ? P2_TINT : 0 });
    air_shadow();
    draw_arc();
}
