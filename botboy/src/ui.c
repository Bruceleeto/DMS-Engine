#include "ui.h"
#include "sound.h"
#include "game.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* The N64 game ran its UI per frame at 30fps: these are those numbers in
 * seconds */
#define ANIM_SPEED        (0.12f * 30.0f)   /* open in about 0.28 s */
#define TYPEWRITER_SPEED  (2.0f * 30.0f)    /* letters per second */
#define BLINK_CYCLE       (40.0f / 30.0f)   /* seconds, half on, half off */
#define BORDER            16
#define TEXT_TOP          -7                /* the N64 placed text by baseline */

static DCImage* frame[8];   /* TL, T, TR, L, R, BL, B, BR */
static DCImage* button[2];  /* the A and B button icons, the N64's button font's a and b */
static const char* const frameNames[8] = {
    "Top_Left", "Top_Center", "Top_Right", "Left_Center",
    "Right_Center", "Bottom_Left", "Bottom_Center", "Bottom_Right"
};

void ui_load(void) {
    char path[64];
    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof(path), ASSETS "ui/UI_%s.dt", frameNames[i]);
        frame[i] = dc_image_load(path);
    }
    button[0] = dc_image_load(ASSETS "ui/btn_a.dt");
    button[1] = dc_image_load(ASSETS "ui/btn_b.dt");
}

void ui_free(void) {
    for (int i = 0; i < 8; i++) { dc_image_free(frame[i]); frame[i] = NULL; }
    for (int i = 0; i < 2; i++) { dc_image_free(button[i]); button[i] = NULL; }
}

/* A button icon where a line of text would go: x, y is the text's place */
void ui_button(char which, float x, float y) {
    ui_image(button[which == 'B' ? 1 : 0], x, y + TEXT_TOP - 4, 1.0f, 1.0f, 1.0f, false);
}

static float ease_out_back(float t) {
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

static float ease_in_back(float t) {
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    return c3 * t * t * t - c1 * t * t;
}

void ui_print(const char* text, float x, float y) {
    ui_text(text, x, y + TEXT_TOP, UI_FONT_SIZE, UI_COLOR_TEXT);
}

float ui_print_width(const char* text) {
    return ui_text_width(text, UI_FONT_SIZE);
}

static void tile(const DCImage* img, float x, float y) {
    ui_image(img, x, y, 1.0f, 1.0f, 1.0f, false);
}

void ui_draw_box(float x, float y, float w, float h) {
    ui_rect(x + 2, y + 2, w - 4, h - 4, UI_COLOR_BG);
    if (!frame[0]) return;
    tile(frame[0], x, y);
    tile(frame[2], x + w - BORDER, y);
    tile(frame[5], x, y + h - BORDER);
    for (float tx = x + BORDER; tx < x + w - BORDER; tx += BORDER) {
        tile(frame[1], tx, y);
        tile(frame[6], tx, y + h - BORDER);
    }
    for (float ty = y + BORDER; ty < y + h - BORDER; ty += BORDER) {
        tile(frame[3], x, ty);
        tile(frame[4], x + w - BORDER, ty);
    }
    tile(frame[7], x + w - BORDER, y + h - BORDER);
}

/* Word wrapped to maxWidth, the first maxChars letters only (typewriter).
 * Returns the number of lines. */
static int print_wrapped(const char* text, int maxChars, float x, float y,
                         float maxWidth, float lineHeight) {
    char line[128];
    int  len = (int)strlen(text);
    if (maxChars < len) len = maxChars;
    int pos = 0, lines = 0;
    while (pos < len) {
        int n = 0, lastSpace = -1;
        for (;;) {
            if (pos + n >= len) break;
            char c = text[pos + n];
            if (c == '\n') break;
            if (c == ' ') lastSpace = n;
            if (n >= (int)sizeof(line) - 1) break;
            line[n] = c; line[n + 1] = '\0';
            if (ui_print_width(line) > maxWidth) {
                /* Too far: back up to the last space if there is one */
                if (lastSpace > 0) n = lastSpace;
                break;
            }
            n++;
        }
        memcpy(line, text + pos, n); line[n] = '\0';
        ui_print(line, x, y + lines * lineHeight);
        pos += n;
        bool blank = pos < len && text[pos] == '\n';   /* an empty line is a blank line */
        if (pos < len && (text[pos] == ' ' || text[pos] == '\n')) pos++;
        lines++;
        if (n == 0 && !blank) break;   /* one letter wider than the box: give up */
    }
    return lines;
}

int ui_print_wrapped(const char* text, float x, float y, float maxWidth, float lineHeight) {
    return print_wrapped(text, (int)strlen(text), x, y, maxWidth, lineHeight);
}

/* ---- Dialogue ---- */

void dialogue_init(DialogueBox* dlg) {
    memset(dlg, 0, sizeof(*dlg));
    dlg->x = 20; dlg->y = VIEW_H - 80;
    dlg->width = VIEW_W - 40; dlg->height = 70;
    dlg->padding = 10; dlg->lineHeight = 12;
    dlg->scale = 1.0f;
}

void dialogue_show(DialogueBox* dlg, const char* text, const char* speaker) {
    if (dlg->animState == UI_ANIM_CLOSING) return;
    strncpy(dlg->text, text, UI_MAX_DIALOGUE_LENGTH - 1);
    dlg->text[UI_MAX_DIALOGUE_LENGTH - 1] = '\0';
    dlg->textLength = (int)strlen(dlg->text);
    if (speaker) { strncpy(dlg->speaker, speaker, 31); dlg->speaker[31] = '\0'; }
    else dlg->speaker[0] = '\0';
    dlg->active = true;
    dlg->charIndex = 0.0f;
    dlg->complete = false;
    dlg->blink = 0.0f;
    dlg->animState = UI_ANIM_OPENING;
    dlg->animTime = 0.0f;
    dlg->scale = 0.0f;
    sfx(SFX_UI_OPEN);
}

void dialogue_close(DialogueBox* dlg) {
    if (!dlg->active || dlg->animState == UI_ANIM_CLOSING) return;
    dlg->animState = UI_ANIM_CLOSING;
    dlg->animTime = 0.0f;
    dlg->queueCount = dlg->queueIndex = 0;
}

bool dialogue_is_active(const DialogueBox* dlg) { return dlg->active; }

void dialogue_queue_add(DialogueBox* dlg, const char* text, const char* speaker) {
    if (dlg->queueCount >= UI_MAX_QUEUE) return;
    int i = dlg->queueCount++;
    strncpy(dlg->queueText[i], text, UI_MAX_DIALOGUE_LENGTH - 1);
    dlg->queueText[i][UI_MAX_DIALOGUE_LENGTH - 1] = '\0';
    if (speaker) { strncpy(dlg->queueSpeaker[i], speaker, 31); dlg->queueSpeaker[i][31] = '\0'; }
    else dlg->queueSpeaker[i][0] = '\0';
}

static bool dialogue_queue_next(DialogueBox* dlg) {
    if (dlg->queueIndex >= dlg->queueCount) return false;
    int i = dlg->queueIndex++;
    const char* sp = dlg->queueSpeaker[i][0] ? dlg->queueSpeaker[i] : NULL;
    /* Same box, new text: no pop */
    strncpy(dlg->text, dlg->queueText[i], UI_MAX_DIALOGUE_LENGTH - 1);
    dlg->textLength = (int)strlen(dlg->text);
    if (sp) strncpy(dlg->speaker, sp, 31);
    dlg->charIndex = 0.0f;
    dlg->complete = false;
    dlg->blink = 0.0f;
    return true;
}

void dialogue_queue_start(DialogueBox* dlg) {
    dlg->queueIndex = 0;
    if (dlg->queueCount == 0) return;
    dialogue_show(dlg, dlg->queueText[0], dlg->queueSpeaker[0][0] ? dlg->queueSpeaker[0] : NULL);
    dlg->queueIndex = 1;
}

bool dialogue_update(DialogueBox* dlg, const DCInput* inp, float dt) {
    if (!dlg->active) return false;
    if (dlg->animState == UI_ANIM_OPENING) {
        dlg->animTime += ANIM_SPEED * dt;
        if (dlg->animTime >= 1.0f) { dlg->animTime = 1.0f; dlg->animState = UI_ANIM_IDLE; }
        dlg->scale = ease_out_back(dlg->animTime);
    } else if (dlg->animState == UI_ANIM_CLOSING) {
        dlg->animTime += ANIM_SPEED * 1.5f * dt;
        if (dlg->animTime >= 1.0f) {
            dlg->animState = UI_ANIM_NONE;
            dlg->active = false;
            return false;
        }
        dlg->scale = 1.0f - ease_in_back(dlg->animTime);
        if (dlg->scale < 0.0f) dlg->scale = 0.0f;
    }
    if (dlg->animState != UI_ANIM_IDLE) return true;

    if (!dlg->complete) {
        int was = (int)dlg->charIndex / 3;
        dlg->charIndex += TYPEWRITER_SPEED * dt;
        if (dlg->charIndex >= dlg->textLength) { dlg->charIndex = dlg->textLength; dlg->complete = true; }
        if ((int)dlg->charIndex / 3 != was) sfx_pick(SFX_KEY1, 5);
    }
    dlg->blink += dt;
    if (dlg->blink >= BLINK_CYCLE) dlg->blink -= BLINK_CYCLE;

    if (inp && dc_input_pressed(inp, CONT_A)) {
        sfx(SFX_UI_PRESS);
        if (!dlg->complete) { dlg->charIndex = dlg->textLength; dlg->complete = true; }
        else if (!dialogue_queue_next(dlg)) dialogue_close(dlg);
    }
    return true;
}

void dialogue_draw(const DialogueBox* dlg) {
    if (!dlg->active || dlg->scale <= 0.01f) return;
    float s = dlg->scale;
    float w = dlg->width * s, h = dlg->height * s;
    float x = dlg->x + dlg->width * 0.5f - w * 0.5f;
    float y = dlg->y + dlg->height * 0.5f - h * 0.5f;

    if (dlg->speaker[0]) {
        float nameW = ui_print_width(dlg->speaker);
        float bw = (nameW + BORDER * 2) * s;
        if (bw < 64 * s) bw = 64 * s;
        float bh = 48 * s;
        float bx = x, by = y - bh - 4 * s;
        ui_draw_box(bx, by, bw, bh);
        if (s >= 0.5f) {
            float border = BORDER * s;
            ui_print(dlg->speaker, bx + border + (bw - border * 2 - nameW) * 0.5f, by + bh * 0.5f + 2);
        }
    }
    ui_draw_box(x, y, w, h);
    if (s < 0.5f) return;
    float pad = dlg->padding * s;
    print_wrapped(dlg->text, (int)dlg->charIndex, x + pad + 9 * s, y + pad + 13 * s,
                  w - pad * 2 - 4 * s, dlg->lineHeight);
    if (dlg->complete && dlg->blink < BLINK_CYCLE * 0.5f)
        ui_button('A', x + w - 27 * s, y + h - 18 * s);
}

/* ---- Option prompt ---- */

void option_init(OptionPrompt* opt) {
    memset(opt, 0, sizeof(*opt));
    opt->x = 80; opt->y = 80; opt->width = 160;
    opt->padding = 15; opt->itemSpacing = 20;
    opt->centered = true;
    opt->scale = 1.0f;
}

void option_set_title(OptionPrompt* opt, const char* title) {
    opt->animState = UI_ANIM_NONE; opt->animTime = 0.0f; opt->scale = 1.0f;
    strncpy(opt->title, title, 63); opt->title[63] = '\0';
    opt->optionCount = 0;
}

void option_add(OptionPrompt* opt, const char* label) {
    if (opt->optionCount >= UI_MAX_OPTIONS) return;
    strncpy(opt->options[opt->optionCount], label, UI_MAX_OPTION_LENGTH - 1);
    opt->options[opt->optionCount][UI_MAX_OPTION_LENGTH - 1] = '\0';
    opt->optionCount++;
}

void option_set_leftright(OptionPrompt* opt, OptionCallback onLeftRight) { opt->onLeftRight = onLeftRight; }

void option_show(OptionPrompt* opt, OptionCallback onSelect, OptionCallback onCancel) {
    if (opt->animState == UI_ANIM_CLOSING) return;
    opt->active = true;
    opt->selectedIndex = 0;
    opt->onSelect = onSelect; opt->onCancel = onCancel;
    opt->pendingCallback = NULL;
    opt->animState = UI_ANIM_OPENING; opt->animTime = 0.0f; opt->scale = 0.0f;
    sfx(SFX_UI_OPEN);
    float titleH = opt->title[0] ? 25 : 0;
    opt->height = opt->padding * 2 + titleH + opt->optionCount * opt->itemSpacing;
    if (opt->centered) {
        opt->x = (VIEW_W - opt->width) * 0.5f;
        opt->y = (VIEW_H - opt->height) * 0.5f;
    }
}

void option_close(OptionPrompt* opt) {
    if (!opt->active || opt->animState == UI_ANIM_CLOSING) return;
    opt->animState = UI_ANIM_CLOSING; opt->animTime = 0.0f;
}

bool option_is_active(const OptionPrompt* opt) { return opt->active; }

int option_decode_leftright(int value, int* direction) {
    *direction = (value % 100) ? 1 : -1;
    return value / 100;
}

bool option_update(OptionPrompt* opt, const DCInput* inp, float dt) {
    if (!opt->active) return false;
    if (opt->animState == UI_ANIM_OPENING) {
        opt->animTime += ANIM_SPEED * dt;
        if (opt->animTime >= 1.0f) { opt->animTime = 1.0f; opt->animState = UI_ANIM_IDLE; }
        opt->scale = ease_out_back(opt->animTime);
    } else if (opt->animState == UI_ANIM_CLOSING) {
        opt->animTime += ANIM_SPEED * 1.5f * dt;
        if (opt->animTime >= 1.0f) {
            opt->animState = UI_ANIM_NONE;
            opt->active = false;
            if (opt->pendingCallback) {
                OptionCallback cb = opt->pendingCallback;
                opt->pendingCallback = NULL;
                cb(opt->pendingSelection);
            }
            return false;
        }
        opt->scale = 1.0f - ease_in_back(opt->animTime);
        if (opt->scale < 0.0f) opt->scale = 0.0f;
    }
    if (opt->animState != UI_ANIM_IDLE || !inp) return true;

    bool up = inp->stick_y < -0.4f, down = inp->stick_y > 0.4f;
    bool left = inp->stick_x < -0.4f, right = inp->stick_x > 0.4f;
    if (dc_input_pressed(inp, CONT_DPAD_UP) || (up && !opt->stickHeldUp)) {
        if (--opt->selectedIndex < 0) opt->selectedIndex = opt->optionCount - 1;
        sfx(SFX_UI_HOVER);
    }
    opt->stickHeldUp = up;
    if (dc_input_pressed(inp, CONT_DPAD_DOWN) || (down && !opt->stickHeldDown)) {
        if (++opt->selectedIndex >= opt->optionCount) opt->selectedIndex = 0;
        sfx(SFX_UI_HOVER);
    }
    opt->stickHeldDown = down;
    if (opt->onLeftRight) {
        if (dc_input_pressed(inp, CONT_DPAD_LEFT) || (left && !opt->stickHeldLeft))
            opt->onLeftRight(opt->selectedIndex * 100 + 0);
        opt->stickHeldLeft = left;
        if (dc_input_pressed(inp, CONT_DPAD_RIGHT) || (right && !opt->stickHeldRight))
            opt->onLeftRight(opt->selectedIndex * 100 + 1);
        opt->stickHeldRight = right;
    }
    if (dc_input_pressed(inp, CONT_A)) {
        sfx(SFX_UI_PRESS);
        if (opt->stayOpenOnSelect) {
            if (opt->onSelect) opt->onSelect(opt->selectedIndex);
        } else {
            opt->pendingCallback = opt->onSelect;
            opt->pendingSelection = opt->selectedIndex;
            option_close(opt);
        }
        return true;
    }
    if (dc_input_pressed(inp, CONT_B)) {
        sfx(SFX_UI_PRESS);
        opt->pendingCallback = opt->onCancel;
        opt->pendingSelection = opt->selectedIndex;
        option_close(opt);
        return true;
    }
    return true;
}

void option_draw(const OptionPrompt* opt) {
    if (!opt->active || opt->scale <= 0.01f) return;
    float s = opt->scale;
    float w = opt->width * s, h = opt->height * s;
    float x = opt->x + opt->width * 0.5f - w * 0.5f;
    float y = opt->y + opt->height * 0.5f - h * 0.5f;
    ui_draw_box(x, y, w, h);
    if (s < 0.5f) return;

    float textY = y + opt->padding * s + 10 * s;
    if (opt->title[0]) {
        ui_print(opt->title, x + (w - ui_print_width(opt->title)) * 0.5f, textY);
        textY += 25 * s;
    }
    float spacing = opt->itemSpacing * s;
    char line[UI_MAX_OPTION_LENGTH + 4];
    for (int i = 0; i < opt->optionCount; i++) {
        float itemY = textY + i * spacing;
        bool sel = i == opt->selectedIndex;
        if (sel) ui_rect(x + 4, itemY - 2 + TEXT_TOP, w - 8, 14, UI_COLOR_HIGHLIGHT);
        snprintf(line, sizeof(line), "%s%s", sel ? "> " : "  ", opt->options[i]);
        ui_print(line, x + (w - ui_print_width(line)) * 0.5f, itemY);
    }
}
