#include "demo.h"
#include "level.h"
#include <kos.h>

#define DEMO_MAGIC   0x31444242u        /* "BBD1" */
#define DEMO_FRAMES  3600               /* a minute at 60 fps; recording stops itself there */
#define DEMO_LTRIG   (1u << 31)         /* the left trigger, as a button bit */
#define DEMO_LEVELS  7

typedef struct { uint32_t magic; int32_t level, frames; float x, y, z, angle; } DemoHead;
typedef struct { int8_t sx, sy; uint16_t pad; uint32_t buttons; float dt, x, y, z, angle; } DemoFrame;

bool game_demo;

static DemoHead  head;
static DemoFrame frames[DEMO_FRAMES];
static int       count, at;
static bool      recording, playing;
static float     recSeconds; static int recSaid;
static DCInput   in;
static uint32_t  prevButtons;

/* ---- Recording ---- */

static void record_write(void) {
    char path[32];
    snprintf(path, sizeof(path), "/pc/demo%d.bin", (int)head.level);
    file_t f = fs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (f == FILEHND_INVALID) { printf("REC: cannot open %s to write (run with dc-tool -c pc)\n", path); return; }
    head.magic = DEMO_MAGIC; head.frames = count;
    bool ok = fs_write(f, &head, sizeof(head)) == (ssize_t)sizeof(head)
           && fs_write(f, frames, count * sizeof(DemoFrame)) == (ssize_t)(count * sizeof(DemoFrame));
    fs_close(f);
    if (ok) printf("REC: wrote %s, %d frames, %.1f s, %d bytes. Copy it to assets/demos/\n",
                   path, count, recSeconds, (int)(sizeof(head) + count * sizeof(DemoFrame)));
    else printf("REC: short write to %s\n", path);
}

void demo_record_toggle(void) {
    if (game_demo) return;
    if (recording) {
        recording = false;
        printf("REC: stopped\n");
        if (count) record_write();
        return;
    }
    recording = true;
    count = 0; recSeconds = 0.0f; recSaid = 0;
    head = (DemoHead){ .level = game_level_id, .x = bot->pos.x, .y = bot->pos.y, .z = bot->pos.z, .angle = bot->angle };
    printf("REC: level %d, from (%.0f, %.0f, %.0f). X+Y again to stop\n", (int)head.level, bot->pos.x, bot->pos.y, bot->pos.z);
}

bool demo_recording(void) { return recording; }

void demo_record_frame(const DCInput* inp, float dt) {
    if (!recording) return;
    DemoFrame* f = &frames[count++];
    float sx = inp ? inp->stick_x : 0.0f, sy = inp ? inp->stick_y : 0.0f;
    uint32_t b = inp ? inp->buttons & ~(uint32_t)(CONT_START | CONT_Y) : 0;
    if (inp && inp->ltrig > 0.5f) b |= DEMO_LTRIG;
    *f = (DemoFrame){ .sx = (int8_t)(sx * 127.0f), .sy = (int8_t)(sy * 127.0f), .buttons = b, .dt = dt,
                      .x = bot->pos.x, .y = bot->pos.y, .z = bot->pos.z, .angle = bot->angle };
    recSeconds += dt;
    if ((int)recSeconds > recSaid) { recSaid = (int)recSeconds; printf("REC: %d s\n", recSaid); }
    if (count >= DEMO_FRAMES) { printf("REC: full\n"); demo_record_toggle(); }
}

/* ---- Playing ---- */

static int  levels[DEMO_LEVELS], found = -1;

static void demo_path(int level, char* path, size_t n) { snprintf(path, n, ASSETS "demos/demo%d.bin", level); }

int demo_count(void) {
    if (found >= 0) return found;
    found = 0;
    for (int lv = 0; lv < DEMO_LEVELS; lv++) {
        char path[48];
        demo_path(lv, path, sizeof(path));
        file_t f = fs_open(path, O_RDONLY);
        if (f == FILEHND_INVALID) continue;
        fs_close(f);
        levels[found++] = lv;
    }
    printf("demo: %d recording%s found\n", found, found == 1 ? "" : "s");
    return found;
}

bool demo_play(int i) {
    if (i < 0 || i >= demo_count()) return false;
    char path[48];
    demo_path(levels[i], path, sizeof(path));
    size_t size = 0;
    uint8_t* data = dc_file_read(path, &size);
    if (!data) return false;
    bool ok = size >= sizeof(DemoHead);
    if (ok) {
        memcpy(&head, data, sizeof(head));
        ok = head.magic == DEMO_MAGIC && head.frames > 0 && head.frames <= DEMO_FRAMES
          && size >= sizeof(DemoHead) + head.frames * sizeof(DemoFrame);
    }
    if (ok) memcpy(frames, data + sizeof(DemoHead), head.frames * sizeof(DemoFrame));
    free(data);
    if (!ok) { printf("demo: %s is not a demo file\n", path); return false; }
    count = head.frames; at = 0;
    prevButtons = 0;
    playing = true;
    game_level_id = head.level;
    return true;
}

void demo_stop(void)    { playing = false; }
bool demo_playing(void) { return playing; }

const DCInput* demo_input(float* dt) {
    if (!playing || at >= count) { playing = false; return NULL; }
    const DemoFrame* f = &frames[at];
    in = (DCInput){ .stick_x = f->sx / 127.0f, .stick_y = f->sy / 127.0f, .connected = true,
                    .buttons = f->buttons & ~DEMO_LTRIG, .ltrig = (f->buttons & DEMO_LTRIG) ? 1.0f : 0.0f };
    in.pressed  = in.buttons & ~prevButtons;
    in.released = prevButtons & ~in.buttons;
    prevButtons = in.buttons;
    *dt = f->dt;
    return &in;
}

void demo_after_update(void) {
    if (!playing || at >= count) return;
    const DemoFrame* f = &frames[at++];
    bot->pos = shz_vec3_init(f->x, f->y, f->z);
    bot->angle = f->angle;
}

shz_vec3_t demo_start_pos(void)   { return shz_vec3_init(head.x, head.y, head.z); }
float      demo_start_angle(void) { return head.angle; }
