#pragma once
/* The KOS sound driver as the DMS host build has it: sound effects are
 * counted against the AICA's 2 MB so what runs out there runs out here, and
 * streams call their callback when polled. Nothing is heard yet. */
#include <stddef.h>
#include <stdint.h>

int  snd_init(void);
void snd_shutdown(void);

typedef uint32_t sfxhnd_t;
#define SFXHND_INVALID 0
typedef struct sfx_play_data {
    int chn;
    sfxhnd_t idx;
    int vol, pan, loop, freq;
    unsigned int loopstart, loopend;
} sfx_play_data_t;
sfxhnd_t snd_sfx_load_raw_buf(char* buf, size_t len, uint32_t rate, uint16_t bitsize, uint32_t channels);
void snd_sfx_unload(sfxhnd_t idx);
void snd_sfx_unload_all(void);
int  snd_sfx_play_ex(sfx_play_data_t* data);
void snd_sfx_stop(int chn);
void snd_sfx_stop_all(void);

typedef int snd_stream_hnd_t;
#define SND_STREAM_INVALID -1
#define SND_STREAM_BUFFER_MAX_PCM16 (128 << 10)
#define SND_STREAM_BUFFER_MAX_PCM8  (64 << 10)
#define SND_STREAM_BUFFER_MAX_ADPCM ((32 << 10) - 64)
#define SND_STREAM_BUFFER_MAX       (64 << 10)
typedef void* (*snd_stream_callback_t)(snd_stream_hnd_t hnd, int smp_req, int* smp_recv);
int  snd_stream_init(void);
void snd_stream_shutdown(void);
snd_stream_hnd_t snd_stream_alloc(snd_stream_callback_t cb, int bufsize);
void snd_stream_destroy(snd_stream_hnd_t hnd);
void snd_stream_start(snd_stream_hnd_t hnd, uint32_t freq, int st);
void snd_stream_start_pcm8(snd_stream_hnd_t hnd, uint32_t freq, int st);
void snd_stream_start_adpcm(snd_stream_hnd_t hnd, uint32_t freq, int st);
void snd_stream_stop(snd_stream_hnd_t hnd);
int  snd_stream_poll(snd_stream_hnd_t hnd);
void snd_stream_volume(snd_stream_hnd_t hnd, int vol);

/* The AICA's command queue, which dc_audio waits on after snd_init. */
typedef struct { uint32_t head, tail, size, valid, process_ok, data; } aica_queue_t;
#define SPU_RAM_UNCACHED_BASE 0
static inline uint32_t g2_read_32_raw(uintptr_t address) { (void)address; return 1; }
