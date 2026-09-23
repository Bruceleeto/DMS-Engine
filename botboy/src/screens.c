/* What is drawn over the level: the level's name sliding in, the controls
 * box the first time a body is played, the pause menu, the fireworks and
 * results when a level is cleared, and the slideshows: one before the
 * factory, one before the foundry, and the one that ends the game. */
#include "sound.h"
#include "dms/dc_audio.h"
#include "level.h"
#include "scene.h"
#include "save.h"
#include "ui.h"

extern bool menu_starts_with_fade_in;

#define BANNER_SLIDE 0.4f
#define BANNER_HOLD  3.0f
#define BANNER_H     26
#define FW_MAX       6
#define SPARK_MAX    48
#define FW_INTERVAL  0.35f
#define FW_DURATION  2.5f
#define SLIDE_SCALE  1.5f

static const char* TUTORIAL_TORSO[] = {
    "TORSO CONTROLS\n\nHold A to charge jump.\nRelease for single, double,\nor triple jump (shown by\ndifferent arc colors).\n\nTap A for a quick hop." };
static const char* TUTORIAL_ARMS[] = {
    "ARMS CONTROLS\n\nA - Jump\nB - Spin Attack\n\nSpin in the air to\n'hover' short distances!",
    "ARMS CONTROLS (2/2)\n\nWall Jump: Jump into a\nwall, then press A again\nto bounce off!" };
static const char* TUTORIAL_FB[] = {
    "FULL BODY CONTROLS\n\nA - Jump\nL - Crouch\nB - Spin Attack\nL + X - Kick Attack",
    "FULL BODY (2/2)\n\nRunning + L + A = Long Jump\nStanding + L + A = Hover Jump" };

typedef struct { bool active; float x, y, velY; int color; } Firework;
typedef struct { bool active; float x, y, velX, velY, life, maxLife; int color; } Spark;
static const uint32_t CELEB_COLORS[7] = { 0xFFFF4444, 0xFF44FF44, 0xFF4488FF, 0xFFFFFF44, 0xFFFF44FF, 0xFF44FFFF, 0xFFFF8844 };

/* Each slide's time in thirtieths; the ending's last one is long */
static const uint8_t FACTORY_FRAMES[37] = {
    33,9,5,19,5,14,5,30,38,11, 6,8,6,6,14,6,8,6,6,25, 10,6,19,8,8,6,5,15,8,8, 8,8,19,21,21,14,60 };
static const uint8_t FOUNDRY_FRAMES[2] = { 75, 75 };
static const uint8_t ENDING_FRAMES[40] = {
    19,7,17,16,27,21,7,32,33,9, 6,19,25,24,7,17,14,16,7,5,
    16,6,15,19,13,21,9,11,7,7, 14,8,14,18,18,7,8,8,33,220 };
typedef struct { const char* file; int count; const uint8_t* frames; } Slideshow;
static const Slideshow SLIDESHOWS[] = {
    { "cutscene/cs2", 37, FACTORY_FRAMES }, { "cutscene/cs4", 2, FOUNDRY_FRAMES }, { "ending/cs3", 40, ENDING_FRAMES } };

static float        bannerTime = -1.0f;         /* seconds since shown, <0 = hidden */
static char         bannerText[48];
static const char** tutorialPages;
static int          tutorialPage, tutorialPageCount;
static bool         tutorialActive;
static float        blink;
static Firework     fws[FW_MAX];
static Spark        sparks[SPARK_MAX];
static float        celebTime, fwTimer;
static char         rank, bestRank;     /* this run's, and the best before it */
static DCImage*     rankImg;
static const Slideshow* show;
static int          slide;
static float        slideTime;
static DCImage*     slideImage;
static OptionPrompt pauseMenu;
static bool         pauseMenuReady;

static void centred(const char* text, float y, uint32_t argb) {
    ui_text(text, 160 - ui_text_width(text, 8) * 0.5f, y, 8, argb);
}

void banner_show(const char* text) {
    snprintf(bannerText, sizeof(bannerText), "%s", text);
    bannerTime = 0.0f;
}

/* Once per save for each body; the whole robot's on the first level that has him */
void tutorial_start(void) {
    SaveFile* sf = save_active();
    Body b = def->body;
    tutorialActive = sf && ((b == BODY_TORSO && !sf->seenTorsoTutorial) || (b == BODY_ARMS && !sf->seenArmsTutorial)
                         || (b == BODY_FB && game_level_id == LEGS_LEVEL && !sf->seenFbTutorial));
    tutorialPages = b == BODY_ARMS ? TUTORIAL_ARMS : b == BODY_FB ? TUTORIAL_FB : TUTORIAL_TORSO;
    tutorialPageCount = b == BODY_TORSO ? 1 : 2;
    tutorialPage = 0;
    blink = 0.0f;
}

bool screens_update(const DCInput* inp, float dt) {
    if (bannerTime >= 0.0f && state != PAUSED && state != CUTSCENE) bannerTime += dt;
    if (!tutorialActive || state == CUTSCENE) return false;
    blink += dt;
    if (inp && dc_input_pressed(inp, CONT_A) && ++tutorialPage >= tutorialPageCount) {
        tutorialActive = false;
        SaveFile* sf = save_active();
        if (!sf) return true;
        if (def->body == BODY_ARMS) sf->seenArmsTutorial = true;
        else if (def->body == BODY_FB) sf->seenFbTutorial = true;
        else sf->seenTorsoTutorial = true;
    }
    return true;
}

/* ---- Level clear: he stops, fireworks go off for a while, then the results box with a rank ---- */

static char calc_rank(void) {
    bool  all   = boltsTaken == boltCount && boltCount > 0;
    float ratio = boltCount > 0 ? (float)boltsTaken / (float)boltCount : 1.0f;
    if (deaths == 0 && all && levelTime < 300.0f) return 'S';
    if ((deaths == 0 && ratio >= 0.75f) || (all && deaths <= 1 && levelTime < 420.0f)) return 'A';
    if ((deaths <= 2 && ratio >= 0.5f) || deaths == 0) return 'B';
    if (deaths <= 5 && ratio >= 0.25f) return 'C';
    return 'D';
}

void celebration_start(void) {
    state = CELEBRATE;
    celebTime = fwTimer = blink = 0.0f;
    robot_stop();
    for (int i = 0; i < FW_MAX; i++) fws[i].active = false;
    for (int i = 0; i < SPARK_MAX; i++) sparks[i].active = false;
    rank = calc_rank();
    bestRank = save_level_rank(game_level_id, rank);
    save_write();
    char path[64];
    snprintf(path, sizeof(path), ASSETS "ui/%c_Score.dt", rank);
    dc_image_free(rankImg);
    rankImg = dc_image_load(path);
}

static void explode_firework(Firework* fw) {
    fw->active = false;
    sfx_pick(SFX_FIREWORK_BANG1, 3);
    int n = 8 + (int)(rand_f() * 5.99f);
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < SPARK_MAX; j++) {
            if (sparks[j].active) continue;
            float theta = rand_f() * TAU, phi = rand_f() * PI, speed = 40.0f + rand_f() * 40.0f;
            sparks[j] = (Spark){ .active = true, .x = fw->x, .y = fw->y, .color = fw->color,
                                 .velX = sinf(phi) * cosf(theta) * speed, .velY = cosf(phi) * speed };
            sparks[j].maxLife = sparks[j].life = 0.5f + rand_f() * 0.5f;
            break;
        }
    }
}

void celebration_update(const DCInput* inp, float dt) {
    celebTime += dt;
    blink += dt;
    fwTimer += dt;
    if (fwTimer >= FW_INTERVAL) {
        fwTimer = 0.0f;
        for (int i = 0; i < FW_MAX; i++) {
            if (fws[i].active) continue;
            fws[i] = (Firework){ .active = true, .x = 160.0f + (rand_f() - 0.5f) * 280.0f, .y = 200.0f,
                                 .velY = 150.0f + rand_f() * 80.0f, .color = (int)(rand_f() * 6.99f) };
            sfx(SFX_FIREWORK_SETOFF);
            break;
        }
    }
    for (int i = 0; i < FW_MAX; i++) {
        if (!fws[i].active) continue;
        fws[i].y -= fws[i].velY * dt;
        fws[i].velY -= 120.0f * dt;
        if (fws[i].velY <= 20.0f || fws[i].y < 50.0f) explode_firework(&fws[i]);
    }
    for (int i = 0; i < SPARK_MAX; i++) {
        if (!sparks[i].active) continue;
        sparks[i].x += sparks[i].velX * dt;
        sparks[i].y -= sparks[i].velY * dt;
        sparks[i].velY -= 60.0f * dt;
        sparks[i].life -= dt;
        if (sparks[i].life <= 0.0f) sparks[i].active = false;
    }
    if (celebTime < FW_DURATION || !inp) return;
    if (dc_input_pressed(inp, CONT_A)) level_leave(game_coop ? game_level_id + 1 : HUB_LEVEL);    /* co-op chains the levels */
    if (dc_input_pressed(inp, CONT_B)) { menu_starts_with_fade_in = true; scene_change(SCENE_MENU); }
}

/* ---- Pause: Start in the level. Restart reloads the level from the top ---- */

static void on_pause(int choice) {
    dc_music_pause(false);
    if (choice == 1) { scene_change(SCENE_GAME); return; }
    if (choice == 2) { menu_starts_with_fade_in = true; scene_change(SCENE_MENU); return; }
    state = PLAYING;
}

void pause_open(void) {
    if (!pauseMenuReady) { option_init(&pauseMenu); pauseMenuReady = true; }
    bannerTime = 0.0f;                  /* the level's name slides in again */
    option_set_title(&pauseMenu, "PAUSED");
    option_add(&pauseMenu, "Resume");
    option_add(&pauseMenu, "Restart Level");
    option_add(&pauseMenu, "Quit to Menu");
    option_show(&pauseMenu, on_pause, on_pause);
    dc_music_pause(true);
    state = PAUSED;
    robot_stop();
}

void pause_update(const DCInput* inp, float dt) {
    if (!option_update(&pauseMenu, inp, dt) && state == PAUSED) state = PLAYING;
}

/* ---- Slideshows: a picture at a time, each for its count of thirtieths ---- */

static void load_slide(int i) {
    dc_image_free(slideImage);
    char path[64];
    snprintf(path, sizeof(path), ASSETS "%s_%d.dt", show->file, i + 1);
    slideImage = dc_image_load(path);
}

/* The factory's and the foundry's intros play the first time a save gets
 * there; the ending marks the level done and goes on to the menu */
void cutscene_start(bool ending) {
    SaveFile* s = game_coop ? NULL : save_active();
    if (game_coop && !ending) return;   /* co-op has no intros */
    if (ending) {
        show = &SLIDESHOWS[2];
        if (s && game_level_id < SAVE_MAX_LEVELS) s->completed[game_level_id] = true;
    } else if (game_level_id == 1 && s && !s->seenFactoryIntro) { show = &SLIDESHOWS[0]; s->seenFactoryIntro = true; }
    else if (game_level_id == 3 && s && !s->seenFoundryIntro) { show = &SLIDESHOWS[1]; s->seenFoundryIntro = true; }
    else return;
    state = CUTSCENE;
    slide = 0; slideTime = 0.0f;
    robot_stop();
    load_slide(0);
}

void cutscene_update(float dt) {
    slideTime += dt;
    if (slideTime < show->frames[slide] / 30.0f) return;
    slideTime = 0.0f;
    if (++slide < show->count) { load_slide(slide); return; }
    dc_image_free(slideImage); slideImage = NULL;
    if (show != &SLIDESHOWS[2]) { state = FADE_IN; return; }
    menu_starts_with_fade_in = true;
    scene_change(SCENE_MENU);
}

/* ---- Drawing ---- */

static void draw_banner(void) {
    if (bannerTime < 0.0f) return;
    float t = bannerTime, k;                /* 0 = off screen, 1 = in place */
    if (t < BANNER_SLIDE) { k = t / BANNER_SLIDE; k = 1.0f - (1.0f - k) * (1.0f - k); }
    else if (t < BANNER_SLIDE + BANNER_HOLD) k = 1.0f;
    else if (t < BANNER_SLIDE * 2 + BANNER_HOLD) { k = (BANNER_SLIDE * 2 + BANNER_HOLD - t) / BANNER_SLIDE; k = k * k; }
    else { bannerTime = -1.0f; return; }
    float w = ui_text_width(bannerText, 8) + 24;
    if (w < 60) w = 60;
    float y = (VIEW_H + 10) + ((VIEW_H - BANNER_H - 4) - (VIEW_H + 10)) * k;
    ui_draw_box(160 - w * 0.5f, y, w, BANNER_H);
    centred(bannerText, y + (BANNER_H - 8) * 0.5f, DC_COLOR_WHITE);
}

static void draw_tutorial(void) {
    if (!tutorialActive) return;
    const float w = 230, h = 145, pad = 14;
    float x = (VIEW_W - w) * 0.5f, y = (VIEW_H - h) * 0.5f;
    ui_draw_box(x, y, w, h);
    ui_print_wrapped(tutorialPages[tutorialPage], x + pad, y + pad, w - pad * 2, 12);
    if (fmodf(blink, 0.66f) < 0.33f) ui_button('A', x + w - 30, y + h - 22);
}

static void draw_celebration(void) {
    if (state != CELEBRATE) return;
    for (int i = 0; i < SPARK_MAX; i++) {
        if (!sparks[i].active) continue;
        uint32_t a = (uint32_t)(sparks[i].life / sparks[i].maxLife * 255.0f);
        ui_rect(sparks[i].x - 1, sparks[i].y - 1, 3, 3, (a << 24) | (CELEB_COLORS[sparks[i].color] & 0xFFFFFF));
    }
    for (int i = 0; i < FW_MAX; i++)
        if (fws[i].active) ui_rect(fws[i].x - 2, fws[i].y - 2, 5, 5, CELEB_COLORS[fws[i].color]);
    if (celebTime < FW_DURATION) return;
    const float w = 200, h = 150;
    float x = 160 - w * 0.5f, y = 120 - h * 0.5f;
    char line[32];
    bool all = boltsTaken == boltCount && boltCount > 0;
    ui_draw_box(x, y, w, h);
    ui_print("LEVEL COMPLETE!", x + 42, y + 10);
    ui_text(game_level_id < REAL_LEVEL_COUNT ? LEVEL_NAMES[game_level_id] : "?", x + 20, y + 22, 8, DC_COLOR_GRAY);
    snprintf(line, sizeof(line), "Time: %d:%02d", (int)(levelTime / 60.0f), (int)levelTime % 60);
    ui_print(line, x + 20, y + 40);
    snprintf(line, sizeof(line), "Bolts: %d/%d", boltsTaken, boltCount);
    ui_print(line, x + 20, y + 55);
    snprintf(line, sizeof(line), "Deaths: %d", deaths);
    ui_print(line, x + 20, y + 70);
    const char* bonus = deaths == 0 && all ? "PERFECT RUN!" : deaths == 0 ? "Deathless!" : all ? "All Bolts Found!" : NULL;
    if (bonus) ui_text(bonus, x + 20, y + 88, 8, DC_COLOR_YELLOW);
    float rx = x + w - 64, ry = y + 40;
    ui_draw_box(rx, ry, 48, 48);
    if (rankImg) ui_image(rankImg, rx + 8, ry + 8, 2.0f, 2.0f, 1.0f, false);
    else { line[0] = rank; line[1] = '\0'; ui_print(line, rx + 20, ry + 20); }
    if (bestRank && bestRank != rank) {     /* a better run than this one is on the card */
        snprintf(line, sizeof(line), "Best: %c", bestRank);
        ui_text(line, rx + 4, ry + 52, 8, DC_COLOR_GRAY);
    }
    if (fmodf(blink, 0.66f) < 0.33f) {
        ui_button('A', x + 20, y + h - 20);       ui_print("Continue", x + 40, y + h - 20);
        ui_button('B', x + w - 76, y + h - 20);   ui_print("Menu", x + w - 56, y + h - 20);
    }
}

void screens_draw(void) {
    draw_banner();
    draw_tutorial();
    draw_celebration();
    if (state == CUTSCENE) {
        ui_rect(0, 0, VIEW_W, VIEW_H, 0xFF000000);
        ui_image(slideImage, VIEW_W / 2, VIEW_H / 2, SLIDE_SCALE, SLIDE_SCALE, 1.0f, true);
    }
    if (state == PAUSED) {
        ui_rect(0, 0, VIEW_W, VIEW_H, 0xA0000000);
        option_draw(&pauseMenu);
    }
}

void screens_free(void) {
    dc_image_free(rankImg); rankImg = NULL;
    dc_image_free(slideImage); slideImage = NULL;
    pauseMenu.active = false;
    tutorialActive = false;
    bannerTime = -1.0f;
}
