/* The editor's look: fonts, colours, icons (Lucide, merged into the text
 * font so an icon is just a string) and the few widgets ImGui hasn't got. */
#pragma once
#include "imgui.h"

/* Lucide 0.469 codepoints (host/third_party/fonts/lucide.ttf) */
#define ICON_PLAY        ""
#define ICON_PAUSE       ""
#define ICON_STOP        ""
#define ICON_STEP        ""
#define ICON_MOVE        ""
#define ICON_ROTATE      ""
#define ICON_SCALE       ""
#define ICON_BOX         ""
#define ICON_BOXES       ""
#define ICON_SUN         ""
#define ICON_LIGHT       ""
#define ICON_CAMERA      ""
#define ICON_FOLDER      ""
#define ICON_FOLDER_OPEN ""
#define ICON_FILE        ""
#define ICON_MODEL       ""
#define ICON_SAVE        ""
#define ICON_UNDO        ""
#define ICON_REDO        ""
#define ICON_BUILD       ""
#define ICON_CPU         ""
#define ICON_GAMEPAD     ""
#define ICON_GRID        ""
#define ICON_EYE         ""
#define ICON_EYE_OFF     ""
#define ICON_PLUS        ""
#define ICON_TRASH       ""
#define ICON_COPY        ""
#define ICON_SEARCH      ""
#define ICON_MAGNET      ""
#define ICON_GLOBE       ""
#define ICON_LOCAL       ""
#define ICON_CONSOLE     ""
#define ICON_LAYERS      ""
#define ICON_INSPECT     ""
#define ICON_SCENE       ""
#define ICON_FOG         ""
#define ICON_PALETTE     ""
#define ICON_TREE        ""
#define ICON_IMAGE       ""
#define ICON_DISC        ""
#define ICON_GAME        ""
#define ICON_X           ""
#define ICON_REFRESH     ""
#define ICON_PACKAGE     ""
#define ICON_MARKER      "\ue114"
#define ICON_IMPORT      "\ue22e"

/* Window titles: the icon and name shown, the id after ### fixed */
#define WIN_HIERARCHY ICON_TREE "  Hierarchy###Hierarchy"
#define WIN_SCENE     ICON_SCENE "  Scene###Scene"
#define WIN_GAME      ICON_GAMEPAD "  Game###Game"
#define WIN_INSPECTOR ICON_INSPECT "  Inspector###Inspector"
#define WIN_PROJECT   ICON_FOLDER "  Project###Project"
#define WIN_CONSOLE   ICON_CONSOLE "  Console###Console"

namespace ui {

/* The accent and the colours things are known by */
constexpr ImU32 ACCENT      = IM_COL32(66, 133, 244, 255);
constexpr ImU32 ACCENT_SOFT = IM_COL32(66, 133, 244, 70);
constexpr ImU32 PLAY_TINT   = IM_COL32(66, 133, 244, 255);
constexpr ImU32 AXIS_X      = IM_COL32(226, 82, 82, 255);
constexpr ImU32 AXIS_Y      = IM_COL32(120, 196, 80, 255);
constexpr ImU32 AXIS_Z      = IM_COL32(72, 128, 236, 255);
constexpr ImU32 WARN        = IM_COL32(240, 170, 60, 255);
constexpr ImU32 ERROR       = IM_COL32(240, 90, 80, 255);
constexpr ImU32 TEXT_DIM    = IM_COL32(150, 154, 162, 255);

extern ImFont* font_bold;
extern ImFont* font_big_icons;

void load_fonts(const char* dir, float scale);
void apply_theme();

/* A square icon button; on draws it lit (a tool that is chosen) */
bool icon_button(const char* id, const char* icon, const char* tip, bool on = false, float size = 0);
/* Buttons joined into one bar, as the toolbar's tools are */
void segment_begin();
bool segment_button(const char* id, const char* icon, const char* tip, bool on, bool first, bool last);
/* The same with a swirl for its icon, in col; it turns while busy */
bool segment_swirl(const char* id, ImU32 col, const char* tip, bool on, bool busy, bool first, bool last);
/* A swirl about centre, radius across, turned by angle (radians) */
void swirl(ImDrawList* dl, ImVec2 centre, float radius, ImU32 col, float angle = 0.0f);
constexpr ImU32 DREAMCAST = IM_COL32(240, 132, 40, 255);   /* the console's orange */
constexpr ImU32 FLYCAST   = IM_COL32(150, 90, 245, 255);
void segment_end();
/* x, y, z each with its coloured tag, dragged as one */
bool drag_xyz(const char* label, float v[3], float speed, const char* fmt, float reset);
/* A labelled row: the label in the left column, the widget after it */
void row_label(const char* label);
/* A component header, as Unity's: icon, bold name, open or shut */
bool component(const char* icon, const char* name, bool default_open = true);
/* A bar of how much of something is used; past limit (if any, marked) it is red */
void meter(const char* label, float used, float total, const char* unit, float width, float limit = 0);
void tooltip(const char* text);

}
