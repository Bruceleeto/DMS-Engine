#include "save.h"
#include "game.h"
#include "dms/dc_audio.h"
#include <kos.h>
#include <dc/vmu_pkg.h>
#include <dc/fs_vmu.h>
#include <stdio.h>
#include <string.h>

const char* const LEVEL_NAMES[REAL_LEVEL_COUNT] = {
    "Scrapyard", "Factory", "Stream", "Foundry", "Hive", "Cliffs", "Stage 7"
};

/* What goes on the card, as one block. The magic changes when the layout
 * does; an old file is then ignored, not converted. */
#define SAVE_MAGIC 0x42424331u          /* "BBC1" */
typedef struct {
    uint32_t magic;
    SaveFile slots[SAVE_SLOTS];
    int32_t  musicVolume, sfxVolume;
} SaveBlob;

static SaveBlob blob;
static int      active = -1;
static bool     loaded, onVmu;
static char     vmuPath[32];

/* ---- The VMU ---- */

/* The file's icon: a hex nut, 32x32 in 4 bits a pixel, from a 16x16 drawing
 * doubled. 0 is the see-through background, 1 the nut, 2 its hole. */
static const char* const ICON[16] = {
    "................",
    ".....111111.....",
    "....11111111....",
    "...1111111111...",
    "..111111111111..",
    "..111122221111..",
    "..111222222111..",
    "..111222222111..",
    "..111222222111..",
    "..111222222111..",
    "..111122221111..",
    "..111111111111..",
    "...1111111111...",
    "....11111111....",
    ".....111111.....",
    "................",
};

static void icon_build(uint8_t* out) {
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x += 2) {
            char a = ICON[y / 2][x / 2], b = ICON[y / 2][(x + 1) / 2];
            uint8_t pa = a == '1' ? 1 : a == '2' ? 2 : 0, pb = b == '1' ? 1 : b == '2' ? 2 : 0;
            out[y * 16 + x / 2] = (pa << 4) | pb;
        }
}

/* What the VMU's screen shows while the game runs: 48x32, a "." is a dark
 * pixel, the top row first (KOS turns it round). Drawn by a script once. */
static const char LCD[] =
    "                                                "
    "                                                "
    "       ........      ......    ..........       "
    "       ........      ......    ..........       "
    "       ..      ..  ..      ..      ..           "
    "       ..      ..  ..      ..      ..           "
    "       ..      ..  ..      ..      ..           "
    "       ..      ..  ..      ..      ..           "
    "       ........    ..      ..      ..           "
    "       ........    ..      ..      ..           "
    "       ..      ..  ..      ..      ..           "
    "       ..      ..  ..      ..      ..           "
    "       ..      ..  ..      ..      ..           "
    "       ..      ..  ..      ..      ..           "
    "       ........      ......        ..           "
    "       ........      ......        ..           "
    "                                                "
    "       ........      ......    ..      ..       "
    "       ........      ......    ..      ..       "
    "       ..      ..  ..      ..  ..      ..       "
    "       ..      ..  ..      ..  ..      ..       "
    "       ..      ..  ..      ..    ..  ..         "
    "       ..      ..  ..      ..    ..  ..         "
    "       ........    ..      ..      ..           "
    "       ........    ..      ..      ..           "
    "       ..      ..  ..      ..      ..           "
    "       ..      ..  ..      ..      ..           "
    "       ..      ..  ..      ..      ..           "
    "       ..      ..  ..      ..      ..           "
    "       ........      ......        ..           "
    "       ........      ......        ..           "
    "                                                ";

/* The first memory card plugged in, as a path for fs_vmu */
static bool find_vmu(void) {
    maple_device_t* dev = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
    if (!dev) return false;
    snprintf(vmuPath, sizeof(vmuPath), "/vmu/%c%d/BOTBOY", 'a' + dev->port, dev->unit);
    return true;
}

/* KOS's VMU filesystem wraps a file in the header the VMU wants (icon, name)
 * on write and hides it on read, so the game only ever sees the blob */
static bool vmu_read(void) {
    file_t f = fs_open(vmuPath, O_RDONLY);
    if (f == FILEHND_INVALID) return false;
    SaveBlob in;
    bool ok = fs_read(f, &in, sizeof(in)) == (ssize_t)sizeof(in) && in.magic == SAVE_MAGIC;
    fs_close(f);
    if (ok) blob = in;
    else printf("save: %s is another layout, starting fresh\n", vmuPath);
    return ok;
}

void save_write(void) {
    if (!onVmu || game_coop) return;
    static uint8_t icon[512];
    icon_build(icon);
    vmu_pkg_t pkg = {
        .desc_short = "BotBoy",
        .desc_long  = "BotBoy: saves and settings",
        .app_id     = "BOTBOY",
        .icon_cnt   = 1,
        .icon_anim_speed = 0,
        .eyecatch_type = VMUPKG_EC_NONE,
        .icon_pal   = { 0x0000, 0xFFC8, 0xF444 },   /* clear, gold, dark */
        .icon_data  = icon,
    };
    file_t f = fs_open(vmuPath, O_WRONLY | O_TRUNC);
    if (f == FILEHND_INVALID) { printf("save: cannot open %s to write\n", vmuPath); return; }
    fs_vmu_set_header(f, &pkg);
    if (fs_write(f, &blob, sizeof(blob)) != (ssize_t)sizeof(blob)) printf("save: short write to %s\n", vmuPath);
    if (fs_close(f) < 0) printf("save: could not write %s\n", vmuPath);
    else printf("save: wrote %s\n", vmuPath);
}

bool save_on_vmu(void) { return onVmu; }

/* ---- Slots ---- */

void save_init(void) {
    if (loaded) return;
    loaded = true;
    memset(&blob, 0, sizeof(blob));
    blob.magic = SAVE_MAGIC;
    blob.musicVolume = blob.sfxVolume = 7;
    active = -1;
    onVmu = find_vmu();
    vmu_set_icon(LCD);                  /* on every VMU screen */
    if (onVmu) {
        if (vmu_read()) printf("save: read %s\n", vmuPath);
        else printf("save: no file at %s yet\n", vmuPath);
    } else printf("save: no VMU, saves stay in RAM\n");
}

bool      save_slot_has_data(int slot) { return slot >= 0 && slot < SAVE_SLOTS && blob.slots[slot].used; }
SaveFile* save_slot(int slot)          { return (slot >= 0 && slot < SAVE_SLOTS) ? &blob.slots[slot] : NULL; }
SaveFile* save_active(void)            { return active >= 0 ? &blob.slots[active] : NULL; }

int save_completed_count(const SaveFile* s) {
    int n = 0;
    if (!s) return 0;
    for (int i = 0; i < SAVE_MAX_LEVELS; i++) if (s->completed[i]) n++;
    return n;
}

int save_percent(int slot) {
    const SaveFile* s = save_slot(slot);
    if (!s || !s->used) return 0;
    return save_completed_count(s) * 100 / REAL_LEVEL_COUNT;
}

void save_create_new(int slot) {
    if (slot < 0 || slot >= SAVE_SLOTS) return;
    memset(&blob.slots[slot], 0, sizeof(SaveFile));
    blob.slots[slot].used = true;
    active = slot;
    save_write();
}

void save_load(int slot) {
    if (slot < 0 || slot >= SAVE_SLOTS || !blob.slots[slot].used) return;
    active = slot;
}

void save_delete(int slot) {
    if (slot < 0 || slot >= SAVE_SLOTS) return;
    memset(&blob.slots[slot], 0, sizeof(SaveFile));
    if (active == slot) active = -1;
    save_write();
}

/* ---- What a level leaves behind ---- */

void save_level_bolts(int level, int taken, int total) {
    SaveFile* s = save_active();
    if (game_coop || !s || level < 0 || level >= SAVE_MAX_LEVELS) return;
    s->boltTotal[level] = total;
    if (taken > s->bolts[level]) s->bolts[level] = taken;
}

void save_level_screw(int level, bool taken, bool has) {
    SaveFile* s = save_active();
    if (game_coop || !s || level < 0 || level >= SAVE_MAX_LEVELS) return;
    s->hasScrew[level] = has;
    if (taken) s->screw[level] = true;
}

static int rank_order(char r) { return r == 'S' ? 5 : r == 'A' ? 4 : r == 'B' ? 3 : r == 'C' ? 2 : r == 'D' ? 1 : 0; }

char save_level_rank(int level, char rank) {
    SaveFile* s = save_active();
    if (game_coop || !s || level < 0 || level >= SAVE_MAX_LEVELS) return 0;
    char was = s->rank[level];
    if (rank_order(rank) > rank_order(was)) s->rank[level] = rank;
    return was;
}

int save_s_ranks(const SaveFile* s) {
    int n = 0;
    if (!s) return 0;
    for (int i = 0; i < SAVE_MAX_LEVELS; i++) if (s->rank[i] == 'S') n++;
    return n;
}

/* Every level that has been played has all its bolts and its screw taken,
 * and every level that is not the hub has been cleared, so nothing is left
 * unseen */
bool save_all_collected(const SaveFile* s) {
    if (!s) return false;
    bool any = false;
    for (int i = 1; i < REAL_LEVEL_COUNT; i++) {
        if (!s->completed[i]) return false;
        if (s->boltTotal[i] > 0) { any = true; if (s->bolts[i] < s->boltTotal[i]) return false; }
        if (s->hasScrew[i] && !s->screw[i]) return false;
    }
    return any;
}

/* ---- Settings ---- */

int  save_music_volume(void)      { return blob.musicVolume; }
int  save_sfx_volume(void)        { return blob.sfxVolume; }
static void apply_volume(void) { dc_audio_set_volume(blob.sfxVolume / 10.0f, blob.musicVolume / 10.0f); }
void save_set_music_volume(int v) { blob.musicVolume = v < 0 ? 0 : v > 10 ? 10 : v; apply_volume(); }
void save_set_sfx_volume(int v)   { blob.sfxVolume   = v < 0 ? 0 : v > 10 ? 10 : v; apply_volume(); }
void save_write_settings(void)    { save_write(); }
