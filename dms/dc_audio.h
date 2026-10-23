/* dc_audio: sound effects and streamed music over the KOS sound driver.
 *
 * Files are .dca (TapamN's dcaconv, converter/dcaconv). Sound effects are
 * resident in sound RAM and must fit one AICA channel (dcaconv's default
 * output, <2^16 samples, ADPCM). Music is converted with --long and is
 * streamed off the disc a buffer at a time, mono, PCM16 or ADPCM.
 *
 * Nothing here is touched by dc_init(): call dc_audio_init() if the game
 * wants sound. dc_frame_end() keeps the music stream fed. */
#ifndef DC_AUDIO_H
#define DC_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

typedef struct DCSound DCSound;

/* Start the sound driver. False if it failed (no sound, calls become no-ops). */
bool dc_audio_init(void);
void dc_audio_shutdown(void);

/* Master volumes, 0..1. Music takes effect at once, sound effects at their next play. */
void  dc_audio_set_volume(float sfx, float music);
float dc_audio_sfx_volume(void);
float dc_audio_music_volume(void);

/* Sound effects. Loaded into sound RAM; NULL on failure. */
DCSound* dc_sound_load(const char* path);
void     dc_sound_free(DCSound* s);

/* Play once. volume 0..1 (scaled by the master), pan -1 left .. 0 .. 1 right.
 * Returns the AICA channel for dc_sound_stop(), or -1. */
int  dc_sound_play(DCSound* s, float volume, float pan);
/* Same but looping (the whole sound, or the file's loop points if it has them). */
int  dc_sound_loop(DCSound* s, float volume, float pan);
void dc_sound_stop(int channel);
void dc_sound_stop_all(void);

/* Streamed music. One track at a time; playing another stops the current.
 * Playing the track that is already playing is a no-op. */
bool dc_music_play(const char* path, bool loop);
void dc_music_stop(void);
void dc_music_pause(bool paused);
bool dc_music_playing(void);
/* The path given to dc_music_play, or NULL. */
const char* dc_music_current(void);

/* Feeds the stream. dc_frame_end() calls this; only call it yourself when
 * a frame will not end for a while (a long load). */
void dc_audio_update(void);

#endif
