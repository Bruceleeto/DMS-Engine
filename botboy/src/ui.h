/* The game's dialogue box and option prompt: 9-slice boxes that pop open,
 * typewriter text, a highlighted choice. Everything is in 320x240 space. */
#ifndef BOTBOY_UI_H
#define BOTBOY_UI_H

#include <stdbool.h>
#include "dms/dc_input.h"

#define UI_MAX_DIALOGUE_LENGTH 256
#define UI_MAX_OPTIONS         8
#define UI_MAX_OPTION_LENGTH   32
#define UI_MAX_QUEUE           16

#define UI_COLOR_BG        0xE0051A05   /* terminal green */
#define UI_COLOR_TEXT      0xFFFFFFFF
#define UI_COLOR_HIGHLIGHT 0xFF4060A0
#define UI_FONT_SIZE       8            /* the N64 debug font was 8px */

typedef enum { UI_ANIM_NONE, UI_ANIM_OPENING, UI_ANIM_IDLE, UI_ANIM_CLOSING } UIAnimState;

/* Load and free the box sprites */
void ui_load(void);
void ui_free(void);
void ui_button(char which, float x, float y);   /* the A or B button icon, 16 px, where text would go */

/* A box: tinted fill with the sprite frame round it */
void ui_draw_box(float x, float y, float w, float h);

/* Text at a size of UI_FONT_SIZE, y being the top of the glyphs */
void ui_print(const char* text, float x, float y);
float ui_print_width(const char* text);
/* Word wrapped to maxWidth, newlines kept. Returns the number of lines */
int   ui_print_wrapped(const char* text, float x, float y, float maxWidth, float lineHeight);

/* ---- Dialogue box ---- */
typedef struct {
    char  text[UI_MAX_DIALOGUE_LENGTH];
    char  speaker[32];
    int   textLength;
    float charIndex;            /* typewriter: how many letters show */
    bool  complete;
    bool  active;
    float blink;                /* seconds into the blink cycle */
    float x, y, width, height, padding, lineHeight;
    UIAnimState animState;
    float animTime;             /* 0 to 1 */
    float scale;
    char  queueText[UI_MAX_QUEUE][UI_MAX_DIALOGUE_LENGTH];
    char  queueSpeaker[UI_MAX_QUEUE][32];
    int   queueCount, queueIndex;
} DialogueBox;

void dialogue_init(DialogueBox* dlg);
void dialogue_show(DialogueBox* dlg, const char* text, const char* speaker);
void dialogue_close(DialogueBox* dlg);
bool dialogue_is_active(const DialogueBox* dlg);
void dialogue_queue_add(DialogueBox* dlg, const char* text, const char* speaker);
void dialogue_queue_start(DialogueBox* dlg);
/* True while it has the controls */
bool dialogue_update(DialogueBox* dlg, const DCInput* inp, float dt);
void dialogue_draw(const DialogueBox* dlg);

/* ---- Option prompt ---- */
typedef void (*OptionCallback)(int selectedIndex);

typedef struct {
    char  title[64];
    char  options[UI_MAX_OPTIONS][UI_MAX_OPTION_LENGTH];
    int   optionCount;
    bool  active;
    int   selectedIndex;
    float x, y, width, height, padding, itemSpacing;
    bool  centered;
    UIAnimState animState;
    float animTime;
    float scale;
    OptionCallback onSelect, onCancel, onLeftRight;
    bool  stayOpenOnSelect;
    OptionCallback pendingCallback;
    int   pendingSelection;
    bool  stickHeldUp, stickHeldDown, stickHeldLeft, stickHeldRight;
} OptionPrompt;

void option_init(OptionPrompt* opt);
void option_set_title(OptionPrompt* opt, const char* title);   /* also clears the options */
void option_add(OptionPrompt* opt, const char* label);
void option_set_leftright(OptionPrompt* opt, OptionCallback onLeftRight);
void option_show(OptionPrompt* opt, OptionCallback onSelect, OptionCallback onCancel);
void option_close(OptionPrompt* opt);
bool option_is_active(const OptionPrompt* opt);
/* onLeftRight gets index * 100 + (0 left, 1 right) */
int  option_decode_leftright(int value, int* direction);
bool option_update(OptionPrompt* opt, const DCInput* inp, float dt);
void option_draw(const OptionPrompt* opt);

#endif
