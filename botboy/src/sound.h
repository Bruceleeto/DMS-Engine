/* The game's sounds: every effect loaded once at start, music by name. */
#ifndef SOUND_H
#define SOUND_H

enum {
    SFX_JUMP, SFX_BOLT, SFX_TURRET_FIRE, SFX_ZAP,
    SFX_FIREWORK_SETOFF, SFX_FIREWORK_BANG1, SFX_FIREWORK_BANG2, SFX_FIREWORK_BANG3,
    SFX_LOGO, SFX_UI_HOVER, SFX_UI_OPEN, SFX_UI_PRESS,
    SFX_KEY1, SFX_KEY2, SFX_KEY3, SFX_KEY4, SFX_KEY5,
    SFX_BEEP, SFX_BOOP, SFX_DAMAGE, SFX_LAND, SFX_TUMBLE,
    SFX_WHISTLE, SFX_THUD,
    SFX_COUNT
};

void sound_init(void);              /* after dc_init: starts audio, loads every effect */
void sfx(int id);                   /* play an effect at full volume, centred */
void sfx_pick(int first, int count);/* one of a run of effects at random */
void music(const char* name);       /* stream assets/music/<name>, looping; NULL stops */

#endif
