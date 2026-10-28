#include "sound.h"
#include "game.h"
#include "save.h"
#include "dms/dc_audio.h"
#include <stdio.h>
#include <stdlib.h>

static const char* const FILES[SFX_COUNT] = {
    [SFX_JUMP] = "JumpSound", [SFX_BOLT] = "BoltCollected", [SFX_TURRET_FIRE] = "Turret_Fire", [SFX_ZAP] = "Misc_Zap_1",
    [SFX_FIREWORK_SETOFF] = "FireworkSetOff2", [SFX_FIREWORK_BANG1] = "FireworkExplosion1",
    [SFX_FIREWORK_BANG2] = "FireworkExplosion2", [SFX_FIREWORK_BANG3] = "FireworkExplosion3",
    [SFX_LOGO] = "BOTTUBOI5", [SFX_UI_HOVER] = "N64UIHover4", [SFX_UI_OPEN] = "N64_Press_A_5", [SFX_UI_PRESS] = "N64_Press_A_5_reversed",
    [SFX_KEY1] = "N64SingleKey1", [SFX_KEY2] = "N64SingleKey2", [SFX_KEY3] = "N64SingleKey3", [SFX_KEY4] = "N64SingleKey4", [SFX_KEY5] = "N64SingleKey5",
    [SFX_BEEP] = "RobotBeep", [SFX_BOOP] = "RobotBoop", [SFX_DAMAGE] = "RobotTakeDamage", [SFX_LAND] = "Landing1", [SFX_TUMBLE] = "RobotTumble",
    [SFX_WHISTLE] = "SlideWhistle", [SFX_THUD] = "MetallicThud2",
};

static DCSound* sounds[SFX_COUNT];

void sound_init(void) {
    if (!dc_audio_init()) return;
    dc_audio_set_volume(save_sfx_volume() / 10.0f, save_music_volume() / 10.0f);
    char path[64];
    for (int i = 0; i < SFX_COUNT; i++) {
        snprintf(path, sizeof(path), ASSETS "sfx/%s.dca", FILES[i]);
        sounds[i] = dc_sound_load(path);
    }
}

void sfx(int id) {
    if (id >= 0 && id < SFX_COUNT) dc_sound_play(sounds[id], 1.0f, 0.0f);
}

void sfx_pick(int first, int count) {
    sfx(first + rand() % count);
}

void music(const char* name) {
    if (!name) { dc_music_stop(); return; }
    char path[64];
    snprintf(path, sizeof(path), ASSETS "music/%s.dca", name);
    dc_music_play(path, true);
}
