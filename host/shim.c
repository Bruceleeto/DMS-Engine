/* The KOS sound driver and threads for the host build (see
 * include/dc/sound/sound.h). Sound effects take AICA RAM as on the
 * Dreamcast, so a game that fills it there fills it here. */
#include <kos.h>
#include <dc/sound/sound.h>
#include <pthread.h>
#include <string.h>

/* AICA RAM less the driver program and stream buffers, roughly. */
#define SOUND_RAM ((2u << 20) - (64u << 10))
#define MAX_SFX 256
#define MAX_STREAMS 4

static size_t sfx_size[MAX_SFX + 1];
static size_t sound_used;

int snd_init(void) { return 0; }
void snd_shutdown(void) { snd_sfx_unload_all(); }

sfxhnd_t snd_sfx_load_raw_buf(char* buf, size_t len, uint32_t rate, uint16_t bitsize, uint32_t channels) {
    (void)buf; (void)rate; (void)bitsize; (void)channels;
    size_t size = (len + 31) & ~(size_t)31;
    if (sound_used + size > SOUND_RAM) {
        printf("host: sound RAM full (%zu of %u bytes used, %zu more wanted)\n", sound_used, SOUND_RAM, size);
        return SFXHND_INVALID;
    }
    for (sfxhnd_t i = 1; i <= MAX_SFX; ++i)
        if (!sfx_size[i]) {
            sfx_size[i] = size;
            sound_used += size;
            return i;
        }
    return SFXHND_INVALID;
}

void snd_sfx_unload(sfxhnd_t idx) {
    if (idx < 1 || idx > MAX_SFX) return;
    sound_used -= sfx_size[idx];
    sfx_size[idx] = 0;
}

void snd_sfx_unload_all(void) {
    memset(sfx_size, 0, sizeof(sfx_size));
    sound_used = 0;
}

int snd_sfx_play_ex(sfx_play_data_t* data) {
    return data && data->idx >= 1 && data->idx <= MAX_SFX && sfx_size[data->idx] ? 0 : -1;
}
void snd_sfx_stop(int chn) { (void)chn; }
void snd_sfx_stop_all(void) {}

/* Streams take what the callback gives at the rate the AICA would play it. */
static struct {
    snd_stream_callback_t cb;
    uint32_t bytes_per_sec;
    uint64_t last_us;
    bool used, playing;
} streams[MAX_STREAMS];

int snd_stream_init(void) { return 0; }
void snd_stream_shutdown(void) { memset(streams, 0, sizeof(streams)); }

snd_stream_hnd_t snd_stream_alloc(snd_stream_callback_t cb, int bufsize) {
    (void)bufsize;
    for (int i = 0; i < MAX_STREAMS; ++i)
        if (!streams[i].used) {
            memset(&streams[i], 0, sizeof(streams[i]));
            streams[i].used = true;
            streams[i].cb = cb;
            return i;
        }
    return SND_STREAM_INVALID;
}

void snd_stream_destroy(snd_stream_hnd_t hnd) {
    if (hnd >= 0 && hnd < MAX_STREAMS) streams[hnd].used = streams[hnd].playing = false;
}

static void stream_start(snd_stream_hnd_t hnd, uint32_t bytes_per_sec) {
    if (hnd < 0 || hnd >= MAX_STREAMS) return;
    streams[hnd].bytes_per_sec = bytes_per_sec;
    streams[hnd].last_us = timer_us_gettime64();
    streams[hnd].playing = true;
}
void snd_stream_start(snd_stream_hnd_t hnd, uint32_t freq, int st) { stream_start(hnd, freq * 2 * (st ? 2 : 1)); }
void snd_stream_start_pcm8(snd_stream_hnd_t hnd, uint32_t freq, int st) { stream_start(hnd, freq * (st ? 2 : 1)); }
void snd_stream_start_adpcm(snd_stream_hnd_t hnd, uint32_t freq, int st) { stream_start(hnd, freq / 2 * (st ? 2 : 1)); }
void snd_stream_stop(snd_stream_hnd_t hnd) {
    if (hnd >= 0 && hnd < MAX_STREAMS) streams[hnd].playing = false;
}
void snd_stream_volume(snd_stream_hnd_t hnd, int vol) { (void)hnd; (void)vol; }

int snd_stream_poll(snd_stream_hnd_t hnd) {
    if (hnd < 0 || hnd >= MAX_STREAMS || !streams[hnd].playing || !streams[hnd].cb) return 0;
    uint64_t now = timer_us_gettime64();
    int want = (int)((now - streams[hnd].last_us) * streams[hnd].bytes_per_sec / 1000000u) & ~31;
    if (want <= 0) return 0;
    if (want > SND_STREAM_BUFFER_MAX) want = SND_STREAM_BUFFER_MAX;
    int got = 0;
    streams[hnd].cb(hnd, want, &got);
    streams[hnd].last_us = now;
    return 0;
}

/* Threads */
struct kthread { pthread_t id; };

kthread_t* thd_create(int detach, void* (*routine)(void*), void* param) {
    kthread_t* t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    if (pthread_create(&t->id, NULL, routine, param)) { free(t); return NULL; }
    if (detach) { pthread_detach(t->id); free(t); return NULL; }
    return t;
}

int thd_join(kthread_t* thd, void** value_out) {
    if (!thd) return -1;
    int r = pthread_join(thd->id, value_out);
    free(thd);
    return r ? -1 : 0;
}
