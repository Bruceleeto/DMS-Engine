/* The menu: a room the robot walks round. The terminal is the main menu, the
 * computer is the options, the jukebox is the sound test. */
#include "sound.h"
#include "dms/dc_audio.h"
#include "game.h"
#include "scene.h"
#include "ui.h"
#include "save.h"
#include "dms/dc_particles.h"
#include "dms/dc_model.h"
#include "dms/dc_camera.h"
#include "dms/collision.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

extern bool menu_starts_with_fade_in;
extern int  game_level_id;

/* ---- The room, from the level data ---- */

#define ROOM_CLEAR   0xFF202040
/* The artist sent the room turned round. A quarter turn lays its long side
 * along z, as the N64 level notes say (x -147..147, z -371..138). The glb also
 * carries a 71 unit node offset along the short side that tiny3d never applied;
 * our converter bakes it in, so the room is shifted back to centre it on x 0. */
#define ROOM_YAW     -1.57079633f
#define ROOM_POS     shz_vec3_init(-71.0f, 0.0f, 0.0f)
#define AMBIENT      (80.0f / 255.0f)
#define FADE_IN_TIME 1.0f

/* Where things stand, in N64 units */
typedef struct { float x, y, z, rotY, scale; int scriptId; } Place;
static const Place TRIGGERS[3] = {
    {    0.0f,   0.1f, -124.0f, 3.14f, 1.00f, 0 },   /* terminal: main menu */
    { -105.0f, -16.0f,  -70.0f, 2.36f, 1.20f, 3 },   /* computer: options */
    {   95.0f, -20.0f,  -90.0f, 3.93f, 1.00f, 4 },   /* jukebox: sound test */
};
static const Place JUKEBOX   = {  96.0f,   0.0f, -86.5f,  0.65f, 1.40f, 0 };
static const Place TABLE     = { -96.0f,   0.0f, -43.8f, -2.45f, 1.00f, 0 };
static const Place DISCOBALL = {   0.0f, 200.0f, -50.0f,  0.00f, 2.50f, 0 };
static const shz_vec3_t PLAYER_START = { .x = 0.0f, .y = 0.0f, .z = 50.0f };
static const shz_vec3_t CAM_POS      = { .x = 0.0f, .y = 100.0f, .z = 200.0f };
static const shz_vec3_t CAM_TARGET   = { .x = 0.0f, .y = 0.0f, .z = -117.0f };

#define INTERACT_RADIUS       50.0f
#define PLAYER_SCALE          0.65f
#define ROBOT_RADIUS          8.0f
#define ROBOT_BODY_Y          10.0f       /* the middle of a 20 high robot */
#define PLAYER_SPEED          (3.0f * 30.0f)     /* units per second */
#define PLAYER_GRAVITY        (0.3f * 30.0f * 30.0f)
#define PLAYER_STEP           12.0f
#define CAMERA_BARRIER        (CAM_POS.z - 80.0f)

#define DISCOBALL_DESCENT_SPEED  30.0f
#define DISCOBALL_DESCENT_AMOUNT 80.0f
#define PARTY_RADIUS             120.0f  /* the coloured lights circle this far out */
#define PARTY_HEIGHT             35.0f   /* above the floor, low so the pools are tight */
#define PARTY_RANGE              200.0f  /* each fades to nothing this far away */
#define PARTY_AMBIENT            (50.0f / 255.0f)   /* the room between the pools */
#define PARTY_GAIN               1.6f    /* over 1: a tile under a light saturates */
#define PARTY_Z                  40.0f   /* the middle of the floor, which runs -137..220 in z */
#define DISCOBALL_ROTATION_SPEED 2.0f
#define DISCO_FLOOR_HIDDEN_Y     -5.0f
#define DISCO_FLOOR_VISIBLE_Y    2.0f
#define DISCO_FLOOR_RISE_SPEED   3.0f

/* ---- State ---- */

static DMSModel *room, *robot, *jukebox, *jukeboxFx, *table, *screen, *discoball, *discoFloor;
static ColWorld* col;
static ColWorld* propCol[2];     /* jukebox, table */
static DCCamera  camera;
static DCLight   light;

static shz_vec3_t playerPos;
static float playerAngle, playerVelY;
static bool  playerMoving, playerWasMoving;
static int   animIdle, animWalk;
static bool  lerpAngle;
static float targetAngle, savedAngle;

static int   triggerInRange;      /* which trigger the robot stands at, or -1 */
static int   activeTrigger;       /* the one being talked to, or -1 */

static DialogueBox  dialogue;
static OptionPrompt prompt;

typedef enum {
    MENU_IDLE, MENU_MAIN_CHOICE, MENU_SELECT_SAVE, MENU_SAVE_DETAILS, MENU_LEVEL_SELECT,
    MENU_OPTIONS, MENU_DELETE_SAVE, MENU_JUKEBOX, MENU_FADE_OUT
} MenuState;
static MenuState menuState;
static bool  gameSessionActive;
static bool  menuIsNewGame;
static int   menuSelectedSlot;
static int   selectedLevel;

static float fadeAlpha;
static bool  fadingIn;

/* Party */
static bool  jukeboxPlaying;
static int   jukeboxMusicIndex, jukeboxSfxIndex;
static float discoOffsetY, discoRotation;
static bool  discoActive, discoSpinning;
static float discoFloorY;
static float partyHue;
static DCParticles* sparkles;           /* the disco ball's glints */
static float sparkleTimer, badgeTime;

static const char* const JUKEBOX_MUSIC_NAMES[] = { "Menu", "Scrapyard", "Factory", "Stream", "Foundry", "Hive", "Cliffs", "Boss" };
static const char* const JUKEBOX_SFX_NAMES[]   = { "Beep", "Boop", "Damage", "Land", "Tumble", "Jump" };
static const char* const JUKEBOX_MUSIC[] = { "scrap1-menu", "scrap1", "CalmJunglescrap", "androidjungle1", "scrap1-level1", "N64Chiller3", "N64JetForceTrack", "android64break" };
static const int JUKEBOX_SFX[] = { SFX_BEEP, SFX_BOOP, SFX_DAMAGE, SFX_LAND, SFX_TUMBLE, SFX_JUMP };
#define JUKEBOX_MUSIC_COUNT 8
#define JUKEBOX_SFX_COUNT   6

/* ---- Menu flows (what the terminal, computer and jukebox say) ---- */

static void show_main_menu(void);
static void show_save_selection(bool isNewGame);
static void show_options_menu(void);
static void show_jukebox_menu(void);

static void build_save_label(int slot, char* buf, size_t n) {
    if (!save_slot_has_data(slot)) snprintf(buf, n, "Slot %d: Empty", slot + 1);
    else snprintf(buf, n, "Slot %d: %d%%", slot + 1, save_percent(slot));
}

static void start_fade_out(int levelId) {
    selectedLevel = levelId;
    game_coop = levelId == 1 && game_coop;      /* co-op only ever begins on level 1 */
    menuState = MENU_FADE_OUT;
    fadeAlpha = 0.0f;
}

static void start_game_with_save(int slot, bool isNewGame) {
    if (isNewGame) {
        save_create_new(slot);
        selectedLevel = 0;
    } else {
        save_load(slot);
        SaveFile* s = save_active();
        selectedLevel = s ? s->currentLevel : 0;
        if (s) for (int i = 0; i < SAVE_MAX_LEVELS; i++) if (!s->completed[i]) { selectedLevel = i; break; }
    }
    gameSessionActive = true;
    start_fade_out(selectedLevel);
}

static void on_confirm_load(int choice) {
    if (choice == 0) start_game_with_save(menuSelectedSlot, menuIsNewGame);
    else show_save_selection(menuIsNewGame);
}

static void show_save_details(int slot) {
    static char title[32];
    menuSelectedSlot = slot;
    snprintf(title, sizeof(title), "Slot %d: %d%%", slot + 1, save_percent(slot));
    menuState = MENU_SAVE_DETAILS;
    option_set_title(&prompt, title);
    option_add(&prompt, menuIsNewGame ? "Yes, overwrite" : "Yes, load");
    option_add(&prompt, "No, go back");
    option_show(&prompt, on_confirm_load, NULL);
}

static void on_save_slot_selected(int slot) {
    menuSelectedSlot = slot;
    if (menuIsNewGame) {
        if (save_slot_has_data(slot)) show_save_details(slot);
        else start_game_with_save(slot, true);
    } else {
        if (save_slot_has_data(slot)) show_save_details(slot);
        else { dialogue_show(&dialogue, "This slot is empty!", "System"); show_save_selection(false); }
    }
}

static void show_save_selection(bool isNewGame) {
    char l0[32], l1[32], l2[32];
    menuIsNewGame = isNewGame;
    menuState = MENU_SELECT_SAVE;
    build_save_label(0, l0, sizeof(l0));
    build_save_label(1, l1, sizeof(l1));
    build_save_label(2, l2, sizeof(l2));
    option_set_title(&prompt, isNewGame ? "Select Slot" : "Load Game");
    option_add(&prompt, l0); option_add(&prompt, l1); option_add(&prompt, l2);
    option_show(&prompt, on_save_slot_selected, NULL);
}

static void on_main_menu_choice(int choice) {
    if (choice == 0) {
        bool any = false;
        for (int i = 0; i < SAVE_SLOTS; i++) if (save_slot_has_data(i)) any = true;
        if (any) show_save_selection(false);
        else { dialogue_show(&dialogue, "No saved games found. Start a New Game first!", "System"); menuState = MENU_IDLE; }
    } else if (choice == 1) {
        show_save_selection(true);
    } else {                            /* co-op starts on the first level, no save */
        game_coop = true;
        start_fade_out(1);
    }
}

static int get_continue_level(void) {
    SaveFile* s = save_active();
    if (!s) return 0;
    for (int i = 0; i < REAL_LEVEL_COUNT; i++) if (!s->completed[i]) return i;
    return REAL_LEVEL_COUNT - 1;
}

static void on_level_selected(int choice) {
    SaveFile* s = save_active();
    if (!s) { menuState = MENU_IDLE; return; }
    int n = 0;
    for (int i = 0; i < REAL_LEVEL_COUNT; i++) {
        if (!s->completed[i]) continue;
        if (n++ == choice) { start_fade_out(i); return; }
    }
    menuState = MENU_IDLE;
}

static void show_level_select(void) {
    SaveFile* s = save_active();
    menuState = MENU_LEVEL_SELECT;
    if (save_completed_count(s) == 0) {
        dialogue_show(&dialogue, "No levels completed!", "System");
        menuState = MENU_IDLE;
        return;
    }
    option_set_title(&prompt, "Level Select");
    for (int i = 0; i < REAL_LEVEL_COUNT; i++) if (s->completed[i]) option_add(&prompt, LEVEL_NAMES[i]);
    option_show(&prompt, on_level_selected, NULL);
}

static void on_active_game_menu_choice(int choice) {
    if (choice == 0) start_fade_out(get_continue_level());
    else if (choice == 1) show_level_select();
    else { game_coop = true; start_fade_out(1); }
}

static void show_main_menu(void) {
    menuState = MENU_MAIN_CHOICE;
    option_set_title(&prompt, "");
    if (gameSessionActive) {
        option_add(&prompt, "Continue");
        option_add(&prompt, "Level Select");
        option_add(&prompt, "2 Player");
        option_show(&prompt, on_active_game_menu_choice, NULL);
    } else {
        option_add(&prompt, "Load Game");
        option_add(&prompt, "New Game");
        option_add(&prompt, "2 Player");
        option_show(&prompt, on_main_menu_choice, NULL);
    }
}

/* Options */

static void update_volume_labels(void) {
    snprintf(prompt.options[0], UI_MAX_OPTION_LENGTH, "Music Volume: %d", save_music_volume());
    snprintf(prompt.options[1], UI_MAX_OPTION_LENGTH, "SFX Volume: %d", save_sfx_volume());
}

static void on_options_leftright(int encoded) {
    int dir, index = option_decode_leftright(encoded, &dir);
    if (index == 0) save_set_music_volume(save_music_volume() + dir);
    else if (index == 1) save_set_sfx_volume(save_sfx_volume() + dir);
    update_volume_labels();
}

static void on_options_cancel(int choice) {
    (void)choice;
    save_write_settings();
    menuState = MENU_IDLE;
}

static void on_confirm_delete(int choice) {
    if (choice == 0) {
        save_delete(menuSelectedSlot);
        if (!save_active()) gameSessionActive = false;
        dialogue_show(&dialogue, "Save deleted.", "System");
    }
    menuState = MENU_IDLE;
}

static void on_delete_save_slot(int slot) {
    if (slot >= SAVE_SLOTS || !save_slot_has_data(slot)) { menuState = MENU_IDLE; return; }
    static char title[40];
    menuSelectedSlot = slot;
    snprintf(title, sizeof(title), "Delete Slot %d (%d%%)?", slot + 1, save_percent(slot));
    option_set_title(&prompt, title);
    option_add(&prompt, "Yes, delete");
    option_add(&prompt, "No, go back");
    option_show(&prompt, on_confirm_delete, NULL);
}

static void on_options_choice(int choice) {
    switch (choice) {
    case 0: save_set_music_volume((save_music_volume() + 1) % 11); show_options_menu(); break;
    case 1: save_set_sfx_volume((save_sfx_volume() + 1) % 11); show_options_menu(); break;
    case 2: {
        char l0[32], l1[32], l2[32];
        menuState = MENU_DELETE_SAVE;
        build_save_label(0, l0, sizeof(l0));
        build_save_label(1, l1, sizeof(l1));
        build_save_label(2, l2, sizeof(l2));
        option_set_title(&prompt, "Delete Save");
        option_add(&prompt, l0); option_add(&prompt, l1); option_add(&prompt, l2);
        option_add(&prompt, "Cancel");
        option_show(&prompt, on_delete_save_slot, NULL);
        break;
    }
    case 3:
        dialogue_queue_add(&dialogue, "BotBot!64", "Credits");
        dialogue_queue_add(&dialogue, "Programmers: Cypress, Nupi", NULL);
        dialogue_queue_add(&dialogue, "3D Modelers: DC.all, StatycTyr", NULL);
        dialogue_queue_add(&dialogue, "Composer/Sound: DakodaComposer", NULL);
        dialogue_queue_add(&dialogue, "Created for N64brew Game Jam 2025", NULL);
        dialogue_queue_add(&dialogue, "Dreamcast port on DMS", NULL);
        dialogue_queue_add(&dialogue, "Thanks for playing!", NULL);
        dialogue_queue_start(&dialogue);
        menuState = MENU_IDLE;
        break;
    default:
        save_write_settings();
        menuState = MENU_IDLE;
        break;
    }
}

static void show_options_menu(void) {
    char music[32], sfx[32];
    menuState = MENU_OPTIONS;
    option_set_title(&prompt, "Options");
    snprintf(music, sizeof(music), "Music Volume: %d", save_music_volume());
    snprintf(sfx, sizeof(sfx), "SFX Volume: %d", save_sfx_volume());
    option_add(&prompt, music);
    option_add(&prompt, sfx);
    option_add(&prompt, "Delete Save");
    option_add(&prompt, "Credits");
    option_add(&prompt, "Back");
    option_set_leftright(&prompt, on_options_leftright);
    option_show(&prompt, on_options_choice, on_options_cancel);
}

/* Jukebox */

static void update_jukebox_labels(void) {
    snprintf(prompt.options[0], UI_MAX_OPTION_LENGTH, "Music: %s", JUKEBOX_MUSIC_NAMES[jukeboxMusicIndex]);
    snprintf(prompt.options[1], UI_MAX_OPTION_LENGTH, "SFX: %s", JUKEBOX_SFX_NAMES[jukeboxSfxIndex]);
}

static void jukebox_exit(void) {
    dc_sound_stop_all();
    music("scrap1-menu");
    prompt.width = 160; prompt.x = 80; prompt.y = 80; prompt.itemSpacing = 20;
    prompt.stayOpenOnSelect = false;
    menuState = MENU_IDLE;
}

static void on_jukebox_leftright(int encoded) {
    int dir, index = option_decode_leftright(encoded, &dir);
    if (index == 0) jukeboxMusicIndex = (jukeboxMusicIndex + dir + JUKEBOX_MUSIC_COUNT) % JUKEBOX_MUSIC_COUNT;
    else if (index == 1) jukeboxSfxIndex = (jukeboxSfxIndex + dir + JUKEBOX_SFX_COUNT) % JUKEBOX_SFX_COUNT;
    update_jukebox_labels();
}

static void on_jukebox_cancel(int choice);
static void on_jukebox_choice(int choice) {
    if (choice == 0) {
        music(JUKEBOX_MUSIC[jukeboxMusicIndex]);
        if (!jukeboxPlaying) {
            jukeboxPlaying = true;
            dc_model_set_anim(jukebox, dc_model_anim_index(jukebox, "jb_dance"));
            dc_model_set_anim(jukeboxFx, dc_model_anim_index(jukeboxFx, "jb_dance"));
        }
    } else if (choice == 1) {
        sfx(JUKEBOX_SFX[jukeboxSfxIndex]);
    } else {                            /* Back: leave once the box has closed, like B */
        prompt.pendingCallback = on_jukebox_cancel;
        prompt.pendingSelection = choice;
        option_close(&prompt);
    }
}

static void on_jukebox_cancel(int choice) { (void)choice; jukebox_exit(); }

static void show_jukebox_menu(void) {
    char music[32], sfx[32];
    menuState = MENU_JUKEBOX;
    option_set_title(&prompt, "Jukebox");
    prompt.width = 260; prompt.x = (VIEW_W - 260) * 0.5f; prompt.y = 60; prompt.itemSpacing = 25;
    snprintf(music, sizeof(music), "Music: %s", JUKEBOX_MUSIC_NAMES[jukeboxMusicIndex]);
    snprintf(sfx, sizeof(sfx), "SFX: %s", JUKEBOX_SFX_NAMES[jukeboxSfxIndex]);
    option_add(&prompt, music);
    option_add(&prompt, sfx);
    option_add(&prompt, "Back");
    option_set_leftright(&prompt, on_jukebox_leftright);
    prompt.stayOpenOnSelect = true;
    option_show(&prompt, on_jukebox_choice, on_jukebox_cancel);
}

static void start_script(int scriptId) {
    switch (scriptId) {
    case 0: show_main_menu(); break;
    case 3: show_options_menu(); break;
    case 4: show_jukebox_menu(); break;
    default: dialogue_show(&dialogue, "Script not found!", "Error"); break;
    }
}

/* ---- Scene ---- */

static DMSModel* load(const char* name) {
    char path[64];
    snprintf(path, sizeof(path), ASSETS "menu/%s.dms", name);
    DMSModel* m = dc_model_load(path);
    if (!m) printf("menu: cannot load %s\n", path);
    return m;
}

static void init(void) {
    room       = load("MenuScene");
    robot      = load("Robo_fb");
    jukebox    = load("JukeBox");
    jukeboxFx  = load("JukeBox_fx");
    table      = load("Table");
    screen     = load("MonitorScreen");
    discoball  = load("discoball");
    discoFloor = load("disco_floor");
    /* the textures that move, N64 texture widths a second */
    if (screen)     dc_model_scroll(screen, "table_screen", 0.0f, 1.5f);
    if (jukeboxFx)  dc_model_scroll(jukeboxFx, "jukebox_unlit", 0.8f, 0.0f);
    if (discoFloor) dc_model_scroll(discoFloor, "Disco_Floor", 0.5f, 0.0f);
    ui_load();
    save_init();

    if (room) col = col_build_rotated(room, ROOM_POS, WORLD_SCALE, ROOM_YAW);
    /* The furniture is its own wall, as on the N64 */
    if (jukebox) propCol[0] = col_build_rotated(jukebox, shz_vec3_init(JUKEBOX.x, JUKEBOX.y, JUKEBOX.z), WORLD_SCALE * JUKEBOX.scale, JUKEBOX.rotY);
    if (table)   propCol[1] = col_build_rotated(table,   shz_vec3_init(TABLE.x, TABLE.y, TABLE.z),       WORLD_SCALE * TABLE.scale,   TABLE.rotY);

    playerPos = PLAYER_START;
    if (col) {
        ColGroundHit g = col_ground(col, shz_vec3_init(playerPos.x, playerPos.y + 50.0f, playerPos.z), 200.0f);
        if (g.hit) playerPos.y = g.y;
    }
    playerAngle = 0.0f; playerVelY = 0.0f;
    playerMoving = playerWasMoving = false;
    lerpAngle = false;
    animIdle = dc_model_anim_index(robot, "fb_idle");
    animWalk = dc_model_anim_index(robot, "fb_walk");
    dc_model_set_anim(robot, animIdle);
    dc_model_set_anim(jukebox,   dc_model_anim_index(jukebox,   "jb_rest"));
    dc_model_set_anim(jukeboxFx, dc_model_anim_index(jukeboxFx, "jb_rest"));

    dc_camera_init(&camera);
    camera.pos = CAM_POS;
    camera.fov = 70.0f;
    dc_camera_look_at(&camera, CAM_TARGET);

    /* The N64 gave the way to the light; a DMS sun is the way it shines */
    light = (DCLight){ .pos = { .x = -1.0f, .y = -1.0f, .z = -1.0f }, .sun = true, .ambient = AMBIENT };

    dialogue_init(&dialogue);
    option_init(&prompt);
    menuState = MENU_IDLE;
    /* The first time the room opens: what the three things are for */
    static bool welcomed;
    if (!welcomed) {
        dialogue_queue_add(&dialogue, "Welcome! Use the computer to change options.", "System");
        dialogue_queue_add(&dialogue, "Use the jukebox to play music.", NULL);
        dialogue_queue_add(&dialogue, "Use the door to start the game.", NULL);
        dialogue_queue_start(&dialogue);
        welcomed = true;
    }
    triggerInRange = activeTrigger = -1;

    fadingIn = menu_starts_with_fade_in;
    fadeAlpha = fadingIn ? 1.0f : 0.0f;
    menu_starts_with_fade_in = false;

    jukeboxPlaying = false;
    discoActive = discoSpinning = false;
    discoOffsetY = discoRotation = 0.0f;
    discoFloorY = DISCO_FLOOR_HIDDEN_Y;
    partyHue = 0.0f;
    /* White glints born round the ball, growing then gone, while it spins */
    sparkles = dc_particles_create(64, &(DCParticleOpts){
        .pos = { .x = DISCOBALL.x, .y = DISCOBALL.y - DISCOBALL_DESCENT_AMOUNT, .z = DISCOBALL.z },
        .spread = { .x = 25.0f, .y = 25.0f, .z = 25.0f }, .speed_spread = { .x = 4.0f, .y = 4.0f, .z = 4.0f },
        .life = 0.8f, .life_spread = 0.24f, .size = 3.0f, .size_spread = 1.0f, .grow = 2.0f,
        .start = 0xFFFFFF, .middle = 0xFFFFFF, .end = 0x000000 });
    sparkleTimer = badgeTime = 0.0f;

    dc_set_clear_color(ROOM_CLEAR);
    music("scrap1-menu");
}

static void deinit(void) {
    if (col) col_free(col);
    dc_particles_free(sparkles); sparkles = NULL;
    col = NULL;
    for (int k = 0; k < 2; k++) { if (propCol[k]) col_free(propCol[k]); propCol[k] = NULL; }
    dc_model_free(room); dc_model_free(robot); dc_model_free(jukebox); dc_model_free(jukeboxFx);
    dc_model_free(table); dc_model_free(screen); dc_model_free(discoball); dc_model_free(discoFloor);
    room = robot = jukebox = jukeboxFx = table = screen = discoball = discoFloor = NULL;
    ui_free();
}

static bool ui_open(void) {
    return dialogue_is_active(&dialogue) || option_is_active(&prompt);
}

static void update_player(const DCInput* inp, float dt) {
    float sx = inp ? inp->stick_x : 0.0f, sy = inp ? -inp->stick_y : 0.0f;
    float mag = sqrtf(sx * sx + sy * sy);
    if (mag > 1.0f) { sx /= mag; sy /= mag; mag = 1.0f; }
    if (mag < 0.2f) { sx = sy = mag = 0.0f; }
    playerMoving = mag > 0.0f;

    shz_vec3_t want = playerPos;
    if (playerMoving) {
        /* The stick is relative to where the camera looks */
        float dx = CAM_TARGET.x - CAM_POS.x, dz = CAM_TARGET.z - CAM_POS.z;
        float len = sqrtf(dx * dx + dz * dz);
        if (len > 0.001f) { dx /= len; dz /= len; }
        float rx = -dz, rz = dx;
        float mx = dx * sy + rx * sx, mz = dz * sy + rz * sx;
        playerAngle = atan2f(-mx, mz);
        want.x += mx * PLAYER_SPEED * mag * dt;
        want.z += mz * PLAYER_SPEED * mag * dt;
        if (want.z > CAMERA_BARRIER) want.z = CAMERA_BARRIER;
    }

    if (col) {
        /* The sphere that meets the walls sits at the robot's middle, so the
         * floor never pushes it (the position is the feet). Small steps so a
         * fast move cannot pass through a thin wall. */
        shz_vec3_t moved = playerPos;
        float dx = want.x - playerPos.x, dz = want.z - playerPos.z;
        int steps = (int)(sqrtf(dx * dx + dz * dz) / (ROBOT_RADIUS * 0.5f)) + 1;
        if (steps > 8) steps = 8;
        for (int i = 0; i < steps; i++) {
            shz_vec3_t body = shz_vec3_init(moved.x, moved.y + ROBOT_BODY_Y, moved.z);
            shz_vec3_t to   = shz_vec3_init(moved.x + dx / steps, moved.y + ROBOT_BODY_Y, moved.z + dz / steps);
            shz_vec3_t out  = col_move(col, body, to, ROBOT_RADIUS);
            for (int k = 0; k < 2; k++) if (propCol[k]) out = col_move_walls(propCol[k], body, out, ROBOT_RADIUS);
            moved.x = out.x; moved.z = out.z;
        }
        ColGroundHit g = col_ground(col, shz_vec3_init(moved.x, moved.y + PLAYER_STEP, moved.z), PLAYER_STEP * 2.0f);
        if (g.hit && playerVelY <= 0.0f) {
            moved.y = g.y;
            playerVelY = 0.0f;
        } else {
            playerVelY -= PLAYER_GRAVITY * dt;
            moved.y += playerVelY * dt;
            if (moved.y < -500.0f) { moved = PLAYER_START; playerVelY = 0.0f; }
        }
        playerPos = moved;
    } else {
        playerPos = want;
    }
}

static void update(float dt) {
    if (dt > 0.1f) dt = 0.1f;
    const DCInput* inp = dc_input_get(0);

    if (fadingIn) {
        fadeAlpha -= dt / FADE_IN_TIME;
        if (fadeAlpha <= 0.0f) { fadeAlpha = 0.0f; fadingIn = false; }
    }

    if (menuState == MENU_FADE_OUT) {
        fadeAlpha += dt * 2.5f;
        if (fadeAlpha >= 1.0f) {
            game_level_id = selectedLevel;
            scene_change(SCENE_GAME);
        }
        goto world;
    }

    /* Done talking: turn back the way the robot was facing */
    if (activeTrigger >= 0 && !ui_open()) {
        targetAngle = savedAngle;
        lerpAngle = true;
        activeTrigger = -1;
    }

    /* Which trigger the robot stands at */
    triggerInRange = -1;
    for (int i = 0; i < 3; i++) {
        float dx = playerPos.x - TRIGGERS[i].x, dy = playerPos.y - TRIGGERS[i].y, dz = playerPos.z - TRIGGERS[i].z;
        float r = INTERACT_RADIUS * TRIGGERS[i].scale;
        if (dx * dx + dy * dy + dz * dz < r * r) { triggerInRange = i; break; }
    }

    if (!ui_open() && triggerInRange >= 0 && inp && dc_input_pressed(inp, CONT_A)) {
        savedAngle = playerAngle;
        targetAngle = TRIGGERS[triggerInRange].rotY;
        lerpAngle = true;
        activeTrigger = triggerInRange;
        start_script(TRIGGERS[triggerInRange].scriptId);
        sfx(SFX_UI_OPEN);
    }

    if (dialogue_update(&dialogue, inp, dt)) { playerMoving = false; }
    else if (option_update(&prompt, inp, dt)) { playerMoving = false; }
    else update_player(inp, dt);

    if (lerpAngle) {
        float diff = targetAngle - playerAngle;
        while (diff > 3.14159f) diff -= 6.28318f;
        while (diff < -3.14159f) diff += 6.28318f;
        if (fabsf(diff) < 0.05f) { playerAngle = targetAngle; lerpAngle = false; }
        else playerAngle += diff * 8.0f * dt;
    }

world:
    if (playerMoving != playerWasMoving) {
        dc_model_set_anim(robot, playerMoving ? animWalk : animIdle);
        playerWasMoving = playerMoving;
    }
    dc_model_animate(robot, dt);
    dc_model_animate(jukebox, dt);
    dc_model_animate(jukeboxFx, dt);

    /* The disco ball comes down when the jukebox plays, then spins */
    if (jukeboxPlaying && !discoActive) { discoActive = true; discoSpinning = false; }
    if (discoActive) {
        if (!discoSpinning) {
            discoOffsetY -= DISCOBALL_DESCENT_SPEED * dt;
            if (discoOffsetY <= -DISCOBALL_DESCENT_AMOUNT) { discoOffsetY = -DISCOBALL_DESCENT_AMOUNT; discoSpinning = true; }
        }
        discoRotation += DISCOBALL_ROTATION_SPEED * (discoSpinning ? 1.0f : 0.2f) * dt;
        if (discoRotation > 6.28318f) discoRotation -= 6.28318f;
    }
    float floorTarget = discoSpinning ? DISCO_FLOOR_VISIBLE_Y : DISCO_FLOOR_HIDDEN_Y;
    if (discoFloorY < floorTarget) { discoFloorY += DISCO_FLOOR_RISE_SPEED * dt; if (discoFloorY > floorTarget) discoFloorY = floorTarget; }
    if (discoFloorY > floorTarget) { discoFloorY -= DISCO_FLOOR_RISE_SPEED * dt; if (discoFloorY < floorTarget) discoFloorY = floorTarget; }
    if (discoSpinning) {
        partyHue += dt * 2.0f;
        sparkleTimer += dt;
        if (sparkleTimer >= 0.15f) { sparkleTimer -= 0.15f; dc_particles_burst(sparkles, 8); }
    }
    badgeTime += dt;
}

static void draw(void) {
    dc_camera_update(&camera);
    dc_set_camera(&camera);

    if (discoSpinning) {
        /* The N64's party mode: four coloured lights a quarter turn apart
         * going round together. The N64's were suns, which on a flat floor
         * all land at once and add up to white; ours are lights in a place,
         * circling low over the middle of the floor, so each throws its own
         * pool on the floor and up the walls and the room dims between them.
         * The ball hangs near the floor's back edge, so the circle is not
         * under it. */
        static const float cols[4][3] = { { 1.0f, 0.15f, 0.15f }, { 0.15f, 1.0f, 0.15f }, { 0.15f, 0.15f, 1.0f }, { 1.0f, 1.0f, 0.15f } };
        DCLight party[4];
        for (int i = 0; i < 4; i++) {
            float a = partyHue + i * 1.5708f;
            party[i] = (DCLight){
                .pos = { .x = DISCOBALL.x + cosf(a) * PARTY_RADIUS, .y = PARTY_HEIGHT, .z = PARTY_Z + sinf(a) * PARTY_RADIUS },
                .range = PARTY_RANGE, .ambient = PARTY_AMBIENT,
                .r = cols[i][0] * PARTY_GAIN, .g = cols[i][1] * PARTY_GAIN, .b = cols[i][2] * PARTY_GAIN };
        }
        dc_set_lights(party, 4);
    } else {
        light.r = light.g = light.b = 0.0f;
        light.pos = shz_vec3_init(-1.0f, -1.0f, -1.0f);
        dc_set_light(&light);
    }

    if (room) dc_draw_ex(room, &(DCDrawOpts){ .pos = ROOM_POS, .scale = WORLD_SCALE, .yaw = ROOM_YAW });
    if (discoFloor) dc_draw_ex(discoFloor, &(DCDrawOpts){ .pos = { .x = 0.0f, .y = discoFloorY, .z = 100.0f }, .scale = WORLD_SCALE, .stretch = { .x = 5.0f, .y = 1.0f, .z = 5.0f } });
    if (jukebox) dc_draw_ex(jukebox, &(DCDrawOpts){ .pos = { .x = JUKEBOX.x, .y = JUKEBOX.y, .z = JUKEBOX.z }, .scale = WORLD_SCALE * JUKEBOX.scale, .yaw = JUKEBOX.rotY });
    if (jukeboxFx) dc_draw_ex(jukeboxFx, &(DCDrawOpts){ .pos = { .x = JUKEBOX.x, .y = JUKEBOX.y, .z = JUKEBOX.z }, .scale = WORLD_SCALE * JUKEBOX.scale, .yaw = JUKEBOX.rotY, .add = true });
    if (table) dc_draw_ex(table, &(DCDrawOpts){ .pos = { .x = TABLE.x, .y = TABLE.y, .z = TABLE.z }, .scale = WORLD_SCALE * TABLE.scale, .yaw = TABLE.rotY });
    if (screen) dc_draw_ex(screen, &(DCDrawOpts){ .pos = { .x = TABLE.x, .y = TABLE.y, .z = TABLE.z }, .scale = WORLD_SCALE * TABLE.scale, .yaw = TABLE.rotY });
    if (discoball) dc_draw_ex(discoball, &(DCDrawOpts){ .pos = { .x = DISCOBALL.x, .y = DISCOBALL.y + discoOffsetY, .z = DISCOBALL.z }, .scale = WORLD_SCALE * DISCOBALL.scale, .yaw = discoRotation });
    if (robot) dc_draw_ex(robot, &(DCDrawOpts){ .pos = playerPos, .scale = WORLD_SCALE * PLAYER_SCALE, .yaw = playerAngle });
    dc_particles_draw(sparkles);

    /* The badges: every level at S, and everything in the game found. The
     * N64 showed a QR code to a reward page for the second; there is no QR
     * library here, so it is the word alone */
    SaveFile* sv = save_active();
    if (sv && save_s_ranks(sv) >= REAL_LEVEL_COUNT - 1) {
        float pulse = (sinf(badgeTime * 2.0f) + 1.0f) * 0.5f;
        uint32_t g = 200 + (uint32_t)(pulse * 55.0f), b = 50 + (uint32_t)(pulse * 100.0f);
        char line[32];
        ui_text("S-RANK MASTER!", 10, 10, 8, 0xFF000000 | (255u << 16) | (g << 8) | b);
        snprintf(line, sizeof(line), "%d/%d S Ranks", save_s_ranks(sv), REAL_LEVEL_COUNT - 1);
        ui_text(line, 10, 22, 8, 0xC8FFFFFF);
    }
    if (sv && save_all_collected(sv)) ui_text("Congrats!", 160 - ui_text_width("Congrats!", 8) * 0.5f, 34, 8, DC_COLOR_YELLOW);

    /* 2D */
    if (!ui_open() && triggerInRange >= 0 && menuState != MENU_FADE_OUT) {
        ui_button('A', 288, 220);           /* bottom right, as the N64 (its 230 of 256) */
    }
    dialogue_draw(&dialogue);
    option_draw(&prompt);
    ui_fade(fadeAlpha);
}

const SceneFuncs menu_scene = { init, deinit, update, draw };
