/* Attract mode: a level played from a recording behind the logo, as the N64
 * did. Recording is done in the game: hold X and press Y to start, again to
 * stop; it writes /pc/demo<level>.bin over dcload (-c pc). Put the file in
 * assets/demos/ and make assets copies it; the logo plays every one it
 * finds, in turn. A frame holds the stick, the buttons, that frame's dt and
 * where he ended up, so a replay follows the same path even if the physics
 * drift a little: the inputs drive the animation and the state, the
 * position is put right after each step. Enemies stay out of a demo. */
#ifndef BOTBOY_DEMO_H
#define BOTBOY_DEMO_H

#include "game.h"

extern bool game_demo;                  /* the game scene is playing a demo */

/* Recording, in the game */
void demo_record_toggle(void);          /* start, or stop and write the file */
bool demo_recording(void);
void demo_record_frame(const DCInput* inp, float dt);   /* after robot_update, each frame */

/* Playing, from the logo */
int        demo_count(void);            /* how many demo files there are (looked for once) */
bool       demo_play(int i);            /* loads one; sets game_level_id */
void       demo_stop(void);
bool       demo_playing(void);          /* false once it has run out */
const DCInput* demo_input(float* dt);   /* the next frame's input and dt, NULL when done */
void       demo_after_update(void);     /* puts him where he was on that frame */
shz_vec3_t demo_start_pos(void);
float      demo_start_angle(void);

#endif
