#include "dc_audio.h"
#include "pvrtex.h"
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/stream.h>
#include <kos/thread.h>
#include <dc/sound/aica_comm.h>
#define AICA_MEM_CMD_QUEUE 0x010000   /* kernel-private: sound/arm/aica_cmd_iface.h */
#include <dc/spu.h>
#include <dc/g2bus.h>
#include <arch/timer.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * .dca header (see converter/dcaconv/file_dca.h)
 * ================================================================ */

#define DCA_ALIGN        32
#define DCA_FMT_PCM16    0
#define DCA_FMT_PCM8     1
#define DCA_FMT_ADPCM    2
#define DCA_FLAG_LOOPING (1 << 9)

typedef struct {
    char     fourcc[4];         /* "DcAF" */
    uint32_t chunk_size;
    uint8_t  version;
    uint8_t  pad0[3];
    uint16_t flags;             /* bits 0-2 channels, bits 7-8 format, bit 9 looping */
    uint16_t rate_aica;         /* AICA's 15-bit float sample rate */
    uint32_t total_length;      /* samples per channel */
    uint32_t loop_start;
    uint32_t loop_end;
    uint32_t pad1;
} DcaHeader;

static int dca_channels(const DcaHeader* h) { return h->flags & 7; }
static int dca_format(const DcaHeader* h)   { return (h->flags >> 7) & 3; }

static uint32_t dca_rate_hz(const DcaHeader* h) {
    unsigned f = h->rate_aica & 0x7fff;
    int hi = (f >> 11) & 0xf;
    if (hi & 8) hi -= 16;
    float rate = 44100.0f * (1.0f + (float)(f & 0x3ff) / 1024.0f);
    return (uint32_t)(hi >= 0 ? rate * (float)(1 << hi) : rate / (float)(1 << -hi));
}

/* Bytes one channel's samples take, before the 32-byte padding */
static size_t dca_channel_bytes(const DcaHeader* h) {
    switch (dca_format(h)) {
    case DCA_FMT_PCM16: return h->total_length * 2;
    case DCA_FMT_PCM8:  return h->total_length;
    default:            return (h->total_length + 1) / 2;
    }
}

static size_t dca_channel_stride(const DcaHeader* h) {
    return (dca_channel_bytes(h) + DCA_ALIGN - 1) & ~(size_t)(DCA_ALIGN - 1);
}

static int dca_bitsize(const DcaHeader* h) {
    switch (dca_format(h)) {
    case DCA_FMT_PCM16: return 16;
    case DCA_FMT_PCM8:  return 8;
    default:            return 4;
    }
}

static bool dca_read_header(FILE* f, DcaHeader* h, const char* path) {
    if (fread(h, sizeof(*h), 1, f) != 1 || memcmp(h->fourcc, "DcAF", 4) != 0) {
        printf("dc_audio: %s is not a .dca file\n", path);
        return false;
    }
    if (dca_format(h) > DCA_FMT_ADPCM || dca_channels(h) < 1) {
        printf("dc_audio: %s: unsupported format\n", path);
        return false;
    }
    return true;
}

/* ================================================================
 * State
 * ================================================================ */

static bool  g_ready;
static void  music_apply_volume(void);
static float g_sfx_volume = 1.0f, g_music_volume = 1.0f;

struct DCSound {
    sfxhnd_t hnd;
    uint32_t rate;
    uint32_t loop_start, loop_end;  /* 0,0: the whole sound */
};

static int to_kos_volume(float v) {
    int i = (int)(v * 255.0f + 0.5f);
    return i < 0 ? 0 : i > 255 ? 255 : i;
}

static int to_kos_pan(float pan) {
    int i = (int)(128.0f + pan * 127.0f);
    return i < 0 ? 0 : i > 255 ? 255 : i;
}

/* ================================================================
 * Init
 * ================================================================ */

bool dc_audio_init(void) {
    if (g_ready) return true;
    if (snd_init() < 0) {
        printf("dc_audio: snd_init failed\n");
        return false;
    }
    /* snd_init uploads the AICA's program and sleeps 10 ms; on hardware the
     * command queue is not valid yet and the first command asserts. Wait on
     * the flag the assert reads, up to half a second. */
    uint64_t deadline = timer_us_gettime64() + 500000;
    while (!g2_read_32_raw(SPU_RAM_UNCACHED_BASE + AICA_MEM_CMD_QUEUE + offsetof(aica_queue_t, valid))) {
        if (timer_us_gettime64() > deadline) {
            printf("dc_audio: the AICA never came up\n");
            snd_shutdown();
            return false;
        }
        thd_sleep(1);
    }
    snd_stream_init();
    g_ready = true;
    return true;
}

void dc_audio_shutdown(void) {
    if (!g_ready) return;
    dc_music_stop();
    snd_sfx_unload_all();
    snd_stream_shutdown();
    snd_shutdown();
    g_ready = false;
}

void dc_audio_set_volume(float sfx, float music) {
    g_sfx_volume   = sfx   < 0.0f ? 0.0f : sfx   > 1.0f ? 1.0f : sfx;
    g_music_volume = music < 0.0f ? 0.0f : music > 1.0f ? 1.0f : music;
    music_apply_volume();
}

float dc_audio_sfx_volume(void)   { return g_sfx_volume; }
float dc_audio_music_volume(void) { return g_music_volume; }

/* ================================================================
 * Sound effects
 * ================================================================ */

DCSound* dc_sound_load(const char* path) {
    if (!g_ready) return NULL;
    size_t size;
    uint8_t* file = dc_file_read(path, &size);
    if (!file) return NULL;
    DcaHeader h;
    if (size < sizeof(h)) { free(file); return NULL; }
    memcpy(&h, file, sizeof(h));
    if (memcmp(h.fourcc, "DcAF", 4) != 0 || dca_format(&h) > DCA_FMT_ADPCM || dca_channels(&h) < 1) {
        printf("dc_audio: %s is not a .dca file\n", path);
        free(file);
        return NULL;
    }

    int channels = dca_channels(&h);
    if (channels > 2) {
        printf("dc_audio: %s: %d channels, want 1 or 2\n", path, channels);
        free(file);
        return NULL;
    }
    if (h.total_length > 65534) {
        printf("dc_audio: %s: %lu samples, too long for a sound effect (convert without --long)\n",
               path, (unsigned long)h.total_length);
        free(file);
        return NULL;
    }

    /* KOS wants the channels back to back at len / channels each, which is
     * how the file lays them out (each padded to 32 bytes); a mono sound is
     * given its exact byte count. */
    size_t stride = channels == 1 ? dca_channel_bytes(&h) : dca_channel_stride(&h);
    size_t len = stride * channels;
    if (sizeof(h) + len > size) {
        printf("dc_audio: %s: short file\n", path);
        free(file);
        return NULL;
    }
    sfxhnd_t hnd = snd_sfx_load_raw_buf((char*)file + sizeof(h), len, dca_rate_hz(&h), dca_bitsize(&h), channels);
    free(file);
    if (hnd == SFXHND_INVALID) {
        printf("dc_audio: %s: sound RAM full?\n", path);
        return NULL;
    }

    DCSound* s = calloc(1, sizeof(*s));
    s->hnd  = hnd;
    s->rate = dca_rate_hz(&h);
    if (h.flags & DCA_FLAG_LOOPING) { s->loop_start = h.loop_start; s->loop_end = h.loop_end; }
    return s;
}

void dc_sound_free(DCSound* s) {
    if (!s) return;
    snd_sfx_unload(s->hnd);
    free(s);
}

static int sound_play(DCSound* s, float volume, float pan, bool loop) {
    if (!s || !g_ready) return -1;
    sfx_play_data_t d = {
        .chn = -1, .idx = s->hnd,
        .vol = to_kos_volume(volume * g_sfx_volume), .pan = to_kos_pan(pan),
        .loop = loop, .freq = (int)s->rate,
        .loopstart = loop ? s->loop_start : 0, .loopend = loop ? s->loop_end : 0,
    };
    return snd_sfx_play_ex(&d);
}

int dc_sound_play(DCSound* s, float volume, float pan) { return sound_play(s, volume, pan, false); }
int dc_sound_loop(DCSound* s, float volume, float pan) { return sound_play(s, volume, pan, true); }

void dc_sound_stop(int channel) {
    if (g_ready && channel >= 0) snd_sfx_stop(channel);
}

void dc_sound_stop_all(void) {
    if (g_ready) snd_sfx_stop_all();
}

/* ================================================================
 * Music
 * ================================================================ */

/* The disc is read by a thread into a RAM ring in big reads, and the KOS
 * stream (polled from dc_frame_end) copies out of that ring. A read straight
 * from the GD-ROM in the poll costs the drive's command latency every time,
 * which showed as a stutter every other frame. */

#define MUSIC_BUF_SIZE  SND_STREAM_BUFFER_MAX
#define MUSIC_RING_SIZE (256 << 10)      /* ~3 s of 44.1 kHz PCM16 mono */
#define MUSIC_READ_SIZE (64 << 10)       /* one disc read */

static struct {
    snd_stream_hnd_t hnd;
    FILE*     file;
    long      data_start;
    char      path[128];
    bool      playing, paused, loop, ended;
    volatile bool eof;                    /* the file is all in the ring */
    size_t    ring_bytes, silence_fed;
    uint8_t*  ring;
    volatile size_t head, tail;           /* producer writes head, consumer reads tail */
    kthread_t* thread;
    volatile bool run;
} g_music = { .hnd = SND_STREAM_INVALID };

static uint8_t g_music_buf[MUSIC_BUF_SIZE] __attribute__((aligned(32)));

static void music_apply_volume(void) {
    if (g_music.playing) snd_stream_volume(g_music.hnd, to_kos_volume(g_music_volume));
}

static size_t ring_used(void) { return g_music.head - g_music.tail; }

/* Read the disc into the ring while there is room for a whole read. */
static void music_fill(void) {
    while (g_music.run && !g_music.eof && MUSIC_RING_SIZE - ring_used() >= MUSIC_READ_SIZE) {
        size_t at = g_music.head % MUSIC_RING_SIZE;
        size_t want = MUSIC_READ_SIZE;
        if (at + want > MUSIC_RING_SIZE) want = MUSIC_RING_SIZE - at;
        size_t n = fread(g_music.ring + at, 1, want, g_music.file);
        if (n == 0) {
            if (!g_music.loop) { g_music.eof = true; return; }
            fseek(g_music.file, g_music.data_start, SEEK_SET);
            n = fread(g_music.ring + at, 1, want, g_music.file);
            if (n == 0) { g_music.eof = true; return; }
        }
        g_music.head += n;
    }
}

static void* music_thread(void* arg) {
    (void)arg;
    while (g_music.run) {
        music_fill();
        thd_sleep(20);
    }
    return NULL;
}

static void* music_callback(snd_stream_hnd_t hnd, int req, int* got) {
    (void)hnd;
    if (req > (int)sizeof(g_music_buf)) req = sizeof(g_music_buf);
    *got = 0;
    if (!g_music.playing || g_music.paused || g_music.ended) return g_music_buf;

    size_t n = ring_used();
    if (n > (size_t)req) n = req;
    if (n == 0) {
        if (g_music.eof) g_music.ended = true;   /* KOS fills the rest with silence */
        return g_music_buf;                     /* or the disc fell behind: a gap */
    }
    size_t at = g_music.tail % MUSIC_RING_SIZE;
    size_t first = n;
    if (at + first > MUSIC_RING_SIZE) first = MUSIC_RING_SIZE - at;
    memcpy(g_music_buf, g_music.ring + at, first);
    if (first < n) memcpy(g_music_buf + first, g_music.ring, n - first);
    g_music.tail += n;

    if (n & 31) {                     /* the tail: pad to what the stream wants */
        memset(g_music_buf + n, 0, 32 - (n & 31));
        n = (n + 31) & ~(size_t)31;
    }
    *got = (int)n;
    return g_music_buf;
}

bool dc_music_play(const char* path, bool loop) {
    if (!g_ready || !path) return false;
    if (g_music.playing && strcmp(g_music.path, path) == 0) return true;
    dc_music_stop();

    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("dc_audio: cannot open %s\n", path);
        return false;
    }
    DcaHeader h;
    if (!dca_read_header(f, &h, path)) { fclose(f); return false; }
    if (dca_channels(&h) != 1) {
        printf("dc_audio: %s: music must be mono (dcaconv without -S)\n", path);
        fclose(f);
        return false;
    }
    int fmt = dca_format(&h);
    int ring = fmt == DCA_FMT_ADPCM ? SND_STREAM_BUFFER_MAX_ADPCM
             : fmt == DCA_FMT_PCM8  ? SND_STREAM_BUFFER_MAX_PCM8 : SND_STREAM_BUFFER_MAX;

    snd_stream_hnd_t hnd = snd_stream_alloc(music_callback, ring);
    if (hnd == SND_STREAM_INVALID) {
        printf("dc_audio: no free stream\n");
        fclose(f);
        return false;
    }

    g_music.hnd = hnd;
    g_music.file = f;
    g_music.data_start = sizeof(h);
    strncpy(g_music.path, path, sizeof(g_music.path) - 1);
    g_music.path[sizeof(g_music.path) - 1] = 0;
    g_music.playing = true;
    g_music.paused = g_music.ended = false;
    g_music.loop = loop;
    g_music.ring_bytes = ring;
    g_music.silence_fed = 0;
    g_music.eof = false;
    g_music.head = g_music.tail = 0;
    g_music.ring = malloc(MUSIC_RING_SIZE);
    if (!g_music.ring) {
        snd_stream_destroy(hnd);
        fclose(f);
        g_music.playing = false;
        return false;
    }
    g_music.run = true;
    music_fill();                      /* the first reads happen now, in the load */
    g_music.thread = thd_create(0, music_thread, NULL);
    thd_set_prio(g_music.thread, PRIO_DEFAULT + 1);   /* under the game */

    uint32_t rate = dca_rate_hz(&h);
    if (fmt == DCA_FMT_ADPCM)     snd_stream_start_adpcm(hnd, rate, 0);
    else if (fmt == DCA_FMT_PCM8) snd_stream_start_pcm8(hnd, rate, 0);
    else                          snd_stream_start(hnd, rate, 0);
    snd_stream_volume(hnd, to_kos_volume(g_music_volume));
    return true;
}

void dc_music_stop(void) {
    if (!g_music.playing) return;
    g_music.run = false;
    thd_join(g_music.thread, NULL);
    g_music.thread = NULL;
    snd_stream_stop(g_music.hnd);
    snd_stream_destroy(g_music.hnd);
    fclose(g_music.file);
    free(g_music.ring);
    g_music.ring = NULL;
    g_music.hnd = SND_STREAM_INVALID;
    g_music.file = NULL;
    g_music.playing = false;
    g_music.path[0] = 0;
}

void dc_music_pause(bool paused) { g_music.paused = paused; }
bool dc_music_playing(void)      { return g_music.playing && !g_music.ended; }
const char* dc_music_current(void) { return g_music.playing ? g_music.path : NULL; }

void dc_audio_update(void) {
    if (!g_music.playing) return;
    if (g_music.ended) {
        /* Let what is queued in the ring play out, then let the stream go */
        g_music.silence_fed += 2048;
        if (g_music.silence_fed >= g_music.ring_bytes) dc_music_stop();
        else snd_stream_poll(g_music.hnd);
        return;
    }
    snd_stream_poll(g_music.hnd);
}
