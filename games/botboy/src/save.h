/* Save slots and settings, on the VMU: one file, BOTBOY, in the first memory
 * card found, holding the three slots and the volumes. Read once at start;
 * written when a slot is made or deleted, a level is cleared, a golden screw
 * is taken, or the volumes change (a VMU write takes a moment, so not on
 * every bolt). With no VMU it all lives in RAM for the session. */
#ifndef BOTBOY_SAVE_H
#define BOTBOY_SAVE_H

#include <stdbool.h>
#include <stdint.h>

#define SAVE_SLOTS      3
#define SAVE_MAX_LEVELS 7
#define REAL_LEVEL_COUNT 7

typedef struct {
    bool used;
    int  currentLevel;
    bool completed[SAVE_MAX_LEVELS];
    int  bolts[SAVE_MAX_LEVELS];        /* the most taken in one run of the level */
    int  boltTotal[SAVE_MAX_LEVELS];    /* how many the level has; 0 until it is played */
    bool screw[SAVE_MAX_LEVELS];        /* the golden screw taken */
    bool hasScrew[SAVE_MAX_LEVELS];     /* the level has one */
    char rank[SAVE_MAX_LEVELS];         /* the best rank, 'S'..'D', or 0 for none */
    bool seenTorsoTutorial;
    bool seenArmsTutorial;
    bool seenFbTutorial;
    bool seenFactoryIntro;      /* the slideshows before levels 2 and 4 play once */
    bool seenFoundryIntro;
    bool seenReward;            /* the 100% box has been shown */
} SaveFile;

extern const char* const LEVEL_NAMES[REAL_LEVEL_COUNT];

void      save_init(void);                  /* reads the VMU the first time */
bool      save_slot_has_data(int slot);
SaveFile* save_slot(int slot);
int       save_percent(int slot);
int       save_completed_count(const SaveFile* s);
void      save_create_new(int slot);        /* also makes it the active one */
void      save_load(int slot);              /* makes it the active one */
void      save_delete(int slot);
SaveFile* save_active(void);                /* NULL until one is loaded or created */
void      save_write(void);                 /* everything to the VMU now */
bool      save_on_vmu(void);                /* a VMU was found */

/* The active slot's record of a level */
void save_level_bolts(int level, int taken, int total);
void save_level_screw(int level, bool taken, bool has);
char save_level_rank(int level, char rank);     /* returns the best before this one */
int  save_s_ranks(const SaveFile* s);           /* how many levels at S */
bool save_all_collected(const SaveFile* s);     /* every bolt and screw of every level played */

int  save_music_volume(void);
int  save_sfx_volume(void);
void save_set_music_volume(int v);
void save_set_sfx_volume(int v);
void save_write_settings(void);

#endif
