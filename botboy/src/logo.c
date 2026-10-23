/* Logo: the title logo breathes while the intro sound plays, then "press
 * start" bounces under it. Start fades out to the menu. Behind it the game
 * scene plays the recorded demos in turn (demo.h), as the N64's attract
 * mode did; with no recordings it is black. */
#include "sound.h"
#include "game.h"
#include "scene.h"
#include "demo.h"
#include <math.h>
#include <stddef.h>

#define INTRO_LENGTH   3.0f                    /* how long the intro sound runs */
#define LOGO_APPEARS   (INTRO_LENGTH * 0.5f)   /* the logo pops in halfway through */
#define FADE_OUT       0.5f
#define FLASH_SPEED    2.0f

typedef enum { PHASE_INTRO, PHASE_WAITING, PHASE_FADE_OUT } Phase;

static DCImage* logo;
static DCImage* press_start;
static Phase phase;
static float alpha;
static float intro_timer;
static float anim_timer;      /* drives the breathing and the bounce */
static bool  logo_shown;

bool menu_starts_with_fade_in;   /* the menu reads this */

static int  demoIdx;
static bool demoOn;

static void demo_begin(int i) {
    if (demo_count() <= 0 || !demo_play(i)) return;
    game_demo = true;
    game_coop = false;
    game_scene.init();
    demoOn = true;
    demoIdx = i;
}

static void demo_end(void) {
    if (!demoOn) return;
    game_scene.deinit();
    demo_stop();
    game_demo = false;
    demoOn = false;
    dc_set_clear_color(0xFF000000);
}

static void init(void) {
    phase = PHASE_INTRO;
    alpha = 0.0f;
    intro_timer = 0.0f;
    anim_timer = 0.0f;
    logo_shown = false;
    logo = dc_image_load(ASSETS "logo/logo.dt");
    press_start = dc_image_load(ASSETS "logo/PressStart.dt");
    sfx(SFX_LOGO);
    demoOn = false;
    demo_begin(0);
}

static void deinit(void) {
    demo_end();
    dc_image_free(logo);
    dc_image_free(press_start);
    logo = press_start = NULL;
}

static void update(float dt) {
    if (demoOn) {
        game_scene.update(dt);              /* takes its own dt from the recording */
        if (!demo_playing()) { demo_end(); demo_begin((demoIdx + 1) % demo_count()); }
    }
    anim_timer += dt * FLASH_SPEED;
    const DCInput* inp = dc_input_get(0);
    bool start = inp && (inp->pressed & CONT_START);

    switch (phase) {
    case PHASE_INTRO:
        intro_timer += dt;
        if (!logo_shown && intro_timer >= LOGO_APPEARS) { logo_shown = true; alpha = 1.0f; }
        if (intro_timer >= INTRO_LENGTH) phase = PHASE_WAITING;
        break;
    case PHASE_WAITING:
        if (start) phase = PHASE_FADE_OUT;
        break;
    case PHASE_FADE_OUT:
        alpha -= dt / FADE_OUT;
        if (alpha <= 0.0f) {
            alpha = 0.0f;
            menu_starts_with_fade_in = true;
            scene_change(SCENE_MENU);
        }
        break;
    }
}

static float ease_out_elastic(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const float p = 0.3f;
    return powf(2.0f, -10.0f * t) * sinf((t - p / 4.0f) * (2.0f * 3.14159f) / p) + 1.0f;
}

static void draw(void) {
    if (demoOn) game_scene.draw();
    if (!logo_shown || alpha < 0.01f) return;

    /* The logo breathes */
    float scale = 1.0f + sinf(anim_timer * 1.5f) * 0.02f;
    ui_image(logo, VIEW_W / 2, VIEW_H / 2 - 20, scale, scale, alpha, true);

    if (phase != PHASE_WAITING) return;

    /* Press start: squash, pop up, settle, wobble, every 1.8 seconds */
    float bounce = fmodf(anim_timer, 1.8f);
    float s, y = 0.0f;
    if (bounce < 0.3f) {
        float t = bounce / 0.3f;
        s = 1.0f - 0.15f * sinf(t * 3.14159f * 0.5f);
        y = 3.0f * sinf(t * 3.14159f * 0.5f);
    } else if (bounce < 0.7f) {
        float t = (bounce - 0.3f) / 0.4f;
        s = ease_out_elastic(t) * 1.15f;
        y = -8.0f * ease_out_elastic(t) + 8.0f * t;
    } else if (bounce < 1.0f) {
        float t = (bounce - 0.7f) / 0.3f;
        s = 1.15f - 0.15f * t + 0.05f * sinf(t * 3.14159f);
    } else {
        float t = (bounce - 1.0f) / 0.8f;
        s = 1.0f + 0.02f * sinf(t * 3.14159f * 4.0f);
    }
    float squish = sinf(anim_timer * 8.0f) * 0.015f;
    float sx = s * (1.0f + squish), sy = s * (1.0f - squish);
    sx = fminf(fmaxf(sx, 0.1f), 2.0f);
    sy = fminf(fmaxf(sy, 0.1f), 2.0f);

    float flash = 0.7f + 0.3f * sinf(anim_timer * 2.5f);
    if (bounce > 0.3f && bounce < 0.7f) flash = 1.0f;

    ui_image(press_start, VIEW_W / 2, VIEW_H / 2 + 55 + y, sx, sy, alpha * flash, true);
}

const SceneFuncs logo_scene = { init, deinit, update, draw };
