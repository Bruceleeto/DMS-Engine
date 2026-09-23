/* Splash: the studio and library logos fade in and out one after another over
 * black. A, B or Start skips to the fade out of the one showing. */
#include "game.h"
#include "scene.h"
#include <stddef.h>

#define INITIAL_DELAY 1.5f
#define FADE_IN       1.0f
#define HOLD          2.0f
#define FADE_OUT      1.0f

typedef enum { PHASE_DELAY, PHASE_FADE_IN, PHASE_HOLD, PHASE_FADE_OUT } Phase;

/* Each card: the images it shows and how they are laid out */
typedef enum { CARD_PSYOPS, CARD_POLLYGONE, CARD_STATYCTYR, CARD_LIBS, CARD_COUNT } Card;

static const char* const card_files[CARD_COUNT][4] = {
    { "Psyops1", "Psyops2", "Psyops3", "Psyops4" },
    { "PollyGone" },
    { "StatycTyr" },
    { "LibDragon1", "LibDragon2", "Tiny3D1", "Tiny3D2" },
};

static DCImage* images[4];
static Card  card;
static Phase phase;
static float alpha;
static float timer;

static void load_card(Card c) {
    for (int i = 0; i < 4; i++) {
        images[i] = NULL;
        if (!card_files[c][i]) continue;
        char path[64];
        snprintf(path, sizeof path, ASSETS "splash/%s.dt", card_files[c][i]);
        images[i] = dc_image_load(path);
    }
}

static void free_card(void) {
    for (int i = 0; i < 4; i++) {
        dc_image_free(images[i]);
        images[i] = NULL;
    }
}

static void init(void) {
    card = CARD_PSYOPS;
    phase = PHASE_DELAY;
    alpha = 0.0f;
    timer = 0.0f;
    load_card(card);
}

static void deinit(void) {
    free_card();
}

static void update(float dt) {
    const DCInput* inp = dc_input_get(0);
    if (inp && (inp->pressed & BTN_CONFIRM)) {
        switch (phase) {
        case PHASE_DELAY:   phase = PHASE_FADE_IN; timer = 0.0f; break;
        case PHASE_FADE_IN: phase = PHASE_FADE_OUT; if (alpha < 0.5f) alpha = 0.5f; break;
        case PHASE_HOLD:    phase = PHASE_FADE_OUT; break;
        default: break;
        }
    }

    switch (phase) {
    case PHASE_DELAY:
        timer += dt;
        if (timer >= INITIAL_DELAY) { phase = PHASE_FADE_IN; timer = 0.0f; }
        break;
    case PHASE_FADE_IN:
        alpha += dt / FADE_IN;
        if (alpha >= 1.0f) { alpha = 1.0f; phase = PHASE_HOLD; timer = 0.0f; }
        break;
    case PHASE_HOLD:
        timer += dt;
        if (timer >= HOLD) phase = PHASE_FADE_OUT;
        break;
    case PHASE_FADE_OUT:
        alpha -= dt / FADE_OUT;
        if (alpha <= 0.0f) {
            alpha = 0.0f;
            free_card();
            if (++card == CARD_COUNT) { scene_change(SCENE_LOGO); return; }
            load_card(card);
            phase = PHASE_FADE_IN;
        }
        break;
    }
}

static void draw(void) {
    if (alpha < 0.01f) return;

    switch (card) {
    case CARD_PSYOPS: {
        /* Four 128x128 quarters of a 256x256 logo */
        float x = (VIEW_W - 256) / 2, y = (VIEW_H - 256) / 2;
        ui_image(images[0], x,       y + 1,   1, 1, alpha, false);
        ui_image(images[1], x + 128, y,       1, 1, alpha, false);
        ui_image(images[2], x,       y + 128, 1, 1, alpha, false);
        ui_image(images[3], x + 128, y + 128, 1, 1, alpha, false);
        break;
    }
    case CARD_POLLYGONE:
    case CARD_STATYCTYR:
        ui_image(images[0], (VIEW_W - 128) / 2, (VIEW_H - 128) / 2, 1, 1, alpha, false);
        break;
    case CARD_LIBS: {
        /* LibDragon 256x128 in two halves, Tiny3D under it at 0.7 */
        const float tiny = 0.7f;
        float tiny_w = 256 * tiny, tiny_h = 99 * tiny;
        float gap = 8;
        float start_y = (VIEW_H - (128 + gap + tiny_h)) / 2;
        float lib_x = (VIEW_W - 256) / 2;
        ui_image(images[0], lib_x,       start_y, 1, 1, alpha, false);
        ui_image(images[1], lib_x + 128, start_y, 1, 1, alpha, false);
        float tiny_x = (VIEW_W - tiny_w) / 2, tiny_y = start_y + 128 + gap;
        ui_image(images[2], tiny_x,              tiny_y, tiny, tiny, alpha, false);
        ui_image(images[3], tiny_x + 128 * tiny, tiny_y, tiny, tiny, alpha, false);
        break;
    }
    default: break;
    }
}

const SceneFuncs splash_scene = { init, deinit, update, draw };
