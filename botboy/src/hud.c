/* The bolt counter that slides in from the right when one is picked up, the
 * golden screw that slides in from the left, and the stars that circle his
 * head after a hit or a long fall. All as the N64 did them. */
#include <stdio.h>
#include <math.h>
#include "game.h"
#include "level.h"
#include "save.h"
#include "ui.h"
#include "dms/dc_camera.h"

#define BOLT_FRAMES   6
#define BOLT_SHOW_X   (VIEW_W - 76.0f)  /* the sprite and the count fit on the right */
#define BOLT_HIDE_X   (VIEW_W + 14.0f)
#define BOLT_FPS      8.0f
#define BOLT_SHOWN    2.5f              /* seconds after a pickup */
#define SCREW_FRAMES  8
#define SCREW_SHOW_X  8.0f
#define SCREW_HIDE_X  -50.0f
#define SCREW_FPS     10.0f
#define SCREW_SHOWN   3.0f
#define HUD_Y         8.0f
#define HUD_SPEED     8.0f              /* the gap closes this fraction a second */

#define HEALTH_SHOW_Y 3.0f              /* the hearts, top centre */
#define HEALTH_HIDE_Y -70.0f
#define HEALTH_SHOWN  2.0f
#define HEALTH_BLINK  0.8f              /* they blink this long after a change, 10 times a second */
#define REWARD_SHOWN  4.0f

#define STAR_COUNT    4
#define STAR_RADIUS   12.0f
#define STAR_HEIGHT   25.0f             /* above his feet */
#define STAR_TIME     2.0f
#define STAR_FADE     0.5f              /* the last of it */
#define STAR_SPIN     5.0f              /* radians a second */

typedef struct { DCImage* frames[8]; int count; float x, target, shown, animTime; int frame; float fps; float showX, hideX; } Slider;

static Slider   boltHud, screwHud;      /* the bolts and the screw are shared in co-op */
static DCImage* star;
static DCImage* hearts[HEALTH + 1];     /* full down to none */
static float    rewardTime;
/* Each player's own: his hearts and his stars */
typedef struct { float healthY, healthShown, healthBlink, starTime, starAngle; } Own;
static Own      own[MAX_PLAYERS];
#define O (own[robot_index()])

static void slider_load(Slider* s, const char* fmt, const int* numbers, int count, float fps, float showX, float hideX) {
    s->count = count; s->fps = fps; s->showX = showX; s->hideX = hideX;
    s->x = s->target = hideX; s->shown = 0.0f; s->frame = 0; s->animTime = 0.0f;
    for (int i = 0; i < count; i++) {
        char path[64];
        snprintf(path, sizeof(path), fmt, numbers[i]);
        s->frames[i] = dc_image_load(path);
    }
}

static void slider_free(Slider* s) {
    for (int i = 0; i < s->count; i++) { dc_image_free(s->frames[i]); s->frames[i] = NULL; }
    s->count = 0;
}

static void slider_update(Slider* s, float dt, bool hold) {
    if (s->shown > 0.0f) s->shown -= dt;
    s->target = (hold || s->shown > 0.0f) ? s->showX : s->hideX;
    float diff = s->target - s->x;
    s->x += diff * HUD_SPEED * dt;
    if (fabsf(diff) < 0.5f) s->x = s->target;
    if (s->x != s->hideX) {                   /* it spins whenever it is on screen */
        s->animTime += dt;
        while (s->animTime >= 1.0f / s->fps) { s->animTime -= 1.0f / s->fps; s->frame = (s->frame + 1) % s->count; }
    }
}

void hud_init(void) {
    static const int boltNumbers[BOLT_FRAMES]   = { 1, 3, 5, 7, 9, 11 };
    static const int screwNumbers[SCREW_FRAMES] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    slider_load(&boltHud,  ASSETS "ui/ScrewUI%d.dt", boltNumbers,  BOLT_FRAMES,  BOLT_FPS,  BOLT_SHOW_X,  BOLT_HIDE_X);
    slider_load(&screwHud, ASSETS "ui/GBOLT%d.dt",   screwNumbers, SCREW_FRAMES, SCREW_FPS, SCREW_SHOW_X, SCREW_HIDE_X);
    star = dc_image_load(ASSETS "ui/star.dt");
    for (int i = 0; i <= HEALTH; i++) {
        char path[64];
        snprintf(path, sizeof(path), ASSETS "ui/health%d.dt", i + 1);
        hearts[i] = dc_image_load(path);
    }
    for (int i = 0; i < MAX_PLAYERS; i++) own[i] = (Own){ .healthY = HEALTH_HIDE_Y };
    rewardTime = 0.0f;
}

void hud_free(void) {
    slider_free(&boltHud);
    slider_free(&screwHud);
    dc_image_free(star); star = NULL;
    for (int i = 0; i <= HEALTH; i++) { dc_image_free(hearts[i]); hearts[i] = NULL; }
}

void hud_bolt(void)  { boltHud.shown  = BOLT_SHOWN; }
void hud_screw(void) { screwHud.shown = SCREW_SHOWN; }
void fx_stars(void)  { O.starTime = STAR_TIME; }
void hud_health(void) { O.healthShown = HEALTH_SHOWN; O.healthBlink = HEALTH_BLINK; }
void hud_reward(void) { rewardTime = REWARD_SHOWN; }

/* Runs while paused too: the pause screen keeps both counters up */
void hud_update(float dt, bool paused) {
    slider_update(&boltHud,  dt, paused);
    slider_update(&screwHud, dt, paused);
    for (int i = 0; i < game_players; i++) {
        Own* o = &own[i];
        if (o->healthShown > 0.0f) o->healthShown -= dt;
        float wantY = (paused || o->healthShown > 0.0f) ? HEALTH_SHOW_Y : HEALTH_HIDE_Y;
        o->healthY += (wantY - o->healthY) * HUD_SPEED * dt;
        if (fabsf(wantY - o->healthY) < 0.5f) o->healthY = wantY;
        if (paused) continue;
        if (o->healthBlink > 0.0f) o->healthBlink -= dt;
        if (o->starTime > 0.0f) { o->starTime -= dt; o->starAngle += STAR_SPIN * dt; }
    }
    if (!paused && rewardTime > 0.0f) rewardTime -= dt;
}

/* A world point to the 320x240 screen; false behind the camera */
static bool project(const DCCamera* cam, shz_vec3_t p, float* sx, float* sy) {
    shz_xmtrx_load_4x4((shz_mat4x4_t*)dc_camera_get_pv(cam));
    shz_vec4_t t = shz_xmtrx_transform_vec4(shz_vec4_init(p.x - cam->pos.x, p.y - cam->pos.y, -(p.z - cam->pos.z), 1.0f));
    t = shz_vec4_swizzle(t, 1, 2, 3, 0);
    if (t.w <= 0.001f) return false;
    float inv = 1.0f / t.w;
    *sx = t.x * inv / VIEW_SCALE;
    *sy = t.y * inv / VIEW_SCALE;
    return true;
}

/* In co-op each half gets the HUD at half size, the hearts sliding in from
 * that half's outer edge */
void hud_draw(const DCCamera* cam, float top, float height) {
    const Own* o = &O;
    bool  half = height < VIEW_H;
    float k = half ? 0.5f : 1.0f;
    if (o->starTime > 0.0f && star) {
        float alpha = (220.0f / 255.0f) * (o->starTime < STAR_FADE ? o->starTime / STAR_FADE : 1.0f);
        for (int i = 0; i < STAR_COUNT; i++) {
            float a = o->starAngle + i * (6.2831853f / STAR_COUNT);
            shz_vec3_t p = shz_vec3_init(bot->pos.x + cosf(a) * STAR_RADIUS, bot->pos.y + STAR_HEIGHT, bot->pos.z + sinf(a) * STAR_RADIUS);
            float sx, sy;
            if (project(cam, p, &sx, &sy)) ui_image(star, sx, sy, 0.5f, 0.5f, alpha, true);
        }
    }
    if (boltHud.x < BOLT_HIDE_X - 1.0f && boltHud.frames[boltHud.frame]) {
        float x = VIEW_W - (VIEW_W - boltHud.x) * k;
        ui_image(boltHud.frames[boltHud.frame], x, top + HUD_Y * k, 1.5f * k, 1.5f * k, 1.0f, false);
        char line[16];
        snprintf(line, sizeof(line), "%d/%d", boltsTaken, boltCount);
        ui_text(line, x + 50.0f * k, top + (HUD_Y + 20.0f) * k, 8, DC_COLOR_WHITE);
    }
    if (screwHud.x > SCREW_HIDE_X + 1.0f && screwHud.frames[screwHud.frame])
        ui_image(screwHud.frames[screwHud.frame], screwHud.x * k, top + HUD_Y * k, 2.0f * k, 2.0f * k, 1.0f, false);
    if (o->healthY > HEALTH_HIDE_Y + 1.0f) {   /* the hearts, blinking after a change */
        int lost = HEALTH - bot->health;
        if (lost < 0) lost = 0;
        if (lost > HEALTH) lost = HEALTH;
        bool off = o->healthBlink > 0.0f && ((int)(o->healthBlink * 10.0f) & 1);
        float hh = 64.0f * k;               /* the sprite is 32 at 2x */
        float y = top > 0.0f ? top + height - HEALTH_SHOW_Y - hh - (o->healthY - HEALTH_SHOW_Y)   /* the lower half: from below */
                             : top + o->healthY;
        if (!off && hearts[lost]) ui_image(hearts[lost], VIEW_W / 2 - 32.0f * k, y, 2.0f * k, 2.0f * k, 1.0f, false);
    }
}

/* The 100% box, over everything: every bolt and screw in the game is his */
void hud_draw_reward(void) {
    if (rewardTime <= 0.0f) return;
    const float w = 220, h = 56;
    float x = (VIEW_W - w) * 0.5f, y = 40;
    ui_draw_box(x, y, w, h);
    const char* title = "100% COMPLETE!";
    const char* sub = "Check Main Menu for reward!";
    ui_text(title, x + (w - ui_text_width(title, 8)) * 0.5f, y + 14, 8, 0xFFFFD764);
    ui_text(sub, x + (w - ui_text_width(sub, 8)) * 0.5f, y + 30, 8, DC_COLOR_WHITE);
}
