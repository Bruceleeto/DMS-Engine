#include "ui.h"
#include "imgui_internal.h"
#include <stdio.h>
#include <string>

namespace ui {

ImFont* font_bold;
ImFont* font_big_icons;

static ImFont* add(const std::string& dir, const char* file, float size, bool icons) {
    ImGuiIO& io = ImGui::GetIO();
    /* Inter has glyphs of its own where Lucide's icons are (the private use
     * area): leave that to Lucide */
    static const ImWchar pua[] = { 0xE000, 0xF8FF, 0 };
    ImFontConfig text;
    text.GlyphExcludeRanges = pua;
    ImFont* f = io.Fonts->AddFontFromFileTTF((dir + file).c_str(), size, &text);
    if (!f) {
        fprintf(stderr, "editor: no font %s%s, using ImGui's own\n", dir.c_str(), file);
        return nullptr;
    }
    if (icons) {
        ImFontConfig c;
        c.MergeMode = true;
        c.GlyphMinAdvanceX = size;
        c.GlyphOffset = ImVec2(0, size * 0.12f);
        io.Fonts->AddFontFromFileTTF((dir + "lucide.ttf").c_str(), size * 0.95f, &c);
    }
    return f;
}

void load_fonts(const char* dir_c, float scale) {
    std::string dir = dir_c;
    float size = 15.0f * scale;
    if (!add(dir, "Inter-Regular.otf", size, true)) {
        ImGui::GetIO().Fonts->AddFontDefault();
        return;
    }
    font_bold = add(dir, "Inter-SemiBold.otf", size, true);
    font_big_icons = add(dir, "Inter-SemiBold.otf", size * 1.15f, true);
}

static ImVec4 rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }

void apply_theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(10, 10);
    s.FramePadding = ImVec2(8, 5);
    s.CellPadding = ImVec2(6, 4);
    s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.IndentSpacing = 16;
    s.ScrollbarSize = 11;
    s.GrabMinSize = 9;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;
    s.TabBorderSize = 0;
    s.WindowRounding = 6;
    s.ChildRounding = 6;
    s.FrameRounding = 5;
    s.PopupRounding = 6;
    s.ScrollbarRounding = 8;
    s.GrabRounding = 4;
    s.TabRounding = 5;
    s.DockingSeparatorSize = 3;
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.SeparatorTextBorderSize = 1;
    s.SeparatorTextPadding = ImVec2(0, 4);

    ImVec4* c = s.Colors;
    ImVec4 bg0 = rgb(22, 23, 26), bg1 = rgb(30, 31, 35), bg2 = rgb(38, 40, 45), bg3 = rgb(50, 52, 59), bg4 = rgb(62, 65, 73);
    ImVec4 accent = rgb(66, 133, 244), accent_hi = rgb(96, 156, 255);
    c[ImGuiCol_Text] = rgb(222, 224, 228);
    c[ImGuiCol_TextDisabled] = rgb(128, 132, 140);
    c[ImGuiCol_WindowBg] = bg1;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = rgb(34, 35, 40, 0.98f);
    c[ImGuiCol_Border] = rgb(255, 255, 255, 0.07f);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = bg0;
    c[ImGuiCol_FrameBgHovered] = rgb(28, 29, 33);
    c[ImGuiCol_FrameBgActive] = rgb(32, 34, 40);
    c[ImGuiCol_TitleBg] = bg0;
    c[ImGuiCol_TitleBgActive] = bg0;
    c[ImGuiCol_TitleBgCollapsed] = bg0;
    c[ImGuiCol_MenuBarBg] = bg0;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = bg3;
    c[ImGuiCol_ScrollbarGrabHovered] = bg4;
    c[ImGuiCol_ScrollbarGrabActive] = accent;
    c[ImGuiCol_CheckMark] = accent_hi;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_hi;
    c[ImGuiCol_Button] = bg2;
    c[ImGuiCol_ButtonHovered] = bg3;
    c[ImGuiCol_ButtonActive] = bg4;
    c[ImGuiCol_Header] = rgb(66, 133, 244, 0.35f);
    c[ImGuiCol_HeaderHovered] = rgb(255, 255, 255, 0.06f);
    c[ImGuiCol_HeaderActive] = rgb(66, 133, 244, 0.5f);
    c[ImGuiCol_Separator] = rgb(255, 255, 255, 0.07f);
    c[ImGuiCol_SeparatorHovered] = accent;
    c[ImGuiCol_SeparatorActive] = accent_hi;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = accent;
    c[ImGuiCol_ResizeGripActive] = accent_hi;
    c[ImGuiCol_Tab] = bg0;
    c[ImGuiCol_TabHovered] = bg2;
    c[ImGuiCol_TabSelected] = bg1;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = bg0;
    c[ImGuiCol_TabDimmedSelected] = bg1;
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_DockingPreview] = rgb(66, 133, 244, 0.5f);
    c[ImGuiCol_DockingEmptyBg] = bg0;
    c[ImGuiCol_TableHeaderBg] = bg2;
    c[ImGuiCol_TableBorderStrong] = rgb(255, 255, 255, 0.06f);
    c[ImGuiCol_TableBorderLight] = rgb(255, 255, 255, 0.04f);
    c[ImGuiCol_TextSelectedBg] = rgb(66, 133, 244, 0.4f);
    c[ImGuiCol_DragDropTarget] = accent_hi;
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.55f);
}

void tooltip(const char* text) {
    if (text && *text && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_NoSharedDelay))
        ImGui::SetTooltip("%s", text);
}

bool icon_button(const char* id, const char* icon, const char* tip, bool on, float size) {
    float h = size > 0 ? size : ImGui::GetFrameHeight();
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton("##b", ImVec2(h, h));
    bool hover = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = on ? ACCENT : held ? ImGui::GetColorU32(ImGuiCol_ButtonActive)
             : hover ? ImGui::GetColorU32(ImGuiCol_ButtonHovered) : 0;
    if (bg) dl->AddRectFilled(p, ImVec2(p.x + h, p.y + h), bg, ImGui::GetStyle().FrameRounding);
    ImVec2 ts = ImGui::CalcTextSize(icon);
    dl->AddText(ImVec2(p.x + (h - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f),
                on ? IM_COL32_WHITE : hover ? IM_COL32(240, 240, 244, 255) : IM_COL32(190, 194, 202, 255), icon);
    ImGui::PopID();
    tooltip(tip);
    return pressed;
}

static ImVec2 seg_start;
void segment_begin() {
    seg_start = ImGui::GetCursorScreenPos();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::BeginGroup();
}

bool segment_button(const char* id, const char* icon, const char* tip, bool on, bool first, bool last) {
    float h = ImGui::GetFrameHeight() + 2, w = h + 6;
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton("##s", ImVec2(w, h));
    bool hover = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = ImGui::GetStyle().FrameRounding;
    ImDrawFlags corners = (first ? ImDrawFlags_RoundCornersLeft : 0) | (last ? ImDrawFlags_RoundCornersRight : 0);
    if (!first && !last) corners = ImDrawFlags_RoundCornersNone;
    ImU32 bg = on ? ACCENT : hover ? ImGui::GetColorU32(ImGuiCol_ButtonHovered) : ImGui::GetColorU32(ImGuiCol_Button);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, r, corners);
    if (!last) dl->AddLine(ImVec2(p.x + w - 0.5f, p.y + 5), ImVec2(p.x + w - 0.5f, p.y + h - 5), IM_COL32(0, 0, 0, 90));
    ImVec2 ts = ImGui::CalcTextSize(icon);
    dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), on ? IM_COL32_WHITE : IM_COL32(205, 208, 215, 255), icon);
    ImGui::PopID();
    tooltip(tip);
    if (!last) ImGui::SameLine();
    return pressed;
}

void segment_end() {
    ImGui::EndGroup();
    ImGui::PopStyleVar();
}

void row_label(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s", label);
    ImGui::SameLine(92.0f * ImGui::GetStyle().FramePadding.x / 8.0f);
}

bool drag_xyz(const char* label, float v[3], float speed, const char* fmt, float reset) {
    static const ImU32 tags[3] = { AXIS_X, AXIS_Y, AXIS_Z };
    static const char* names[3] = { "X", "Y", "Z" };
    bool changed = false;
    ImGui::PushID(label);
    row_label(label);
    float avail = ImGui::GetContentRegionAvail().x, tag = 18.0f, gap = 4.0f;
    float w = (avail - 2 * gap) / 3.0f - tag;
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine(0, gap);
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = ImGui::GetFrameHeight();
        ImGui::InvisibleButton("tag", ImVec2(tag, h));
        if (ImGui::IsItemClicked()) { v[i] = reset; changed = true; }
        tooltip("Click to reset");
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + tag, p.y + h), tags[i], ImGui::GetStyle().FrameRounding, ImDrawFlags_RoundCornersLeft);
        ImVec2 ts = ImGui::CalcTextSize(names[i]);
        dl->AddText(ImVec2(p.x + (tag - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), IM_COL32_WHITE, names[i]);
        ImGui::SameLine(0, 0);
        ImGui::SetNextItemWidth(w);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
        changed |= ImGui::DragFloat("##v", &v[i], speed, 0, 0, fmt);
        ImGui::PopStyleVar();
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}

bool component(const char* icon, const char* name, bool default_open) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(44, 46, 52, 255));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(52, 54, 61, 255));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(58, 60, 68, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 7));
    if (font_bold) ImGui::PushFont(font_bold, 0.0f);
    char label[128];
    snprintf(label, sizeof(label), "%s  %s", icon, name);
    bool open = ImGui::CollapsingHeader(label, (default_open ? ImGuiTreeNodeFlags_DefaultOpen : 0) | ImGuiTreeNodeFlags_SpanAvailWidth);
    if (font_bold) ImGui::PopFont();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    if (open) ImGui::Spacing();
    return open;
}

void meter(const char* label, float used, float total, const char* unit, float width) {
    float k = total > 0 ? used / total : 0;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetFrameHeight();
    ImGui::InvisibleButton(label, ImVec2(width, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = h * 0.5f;
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), IM_COL32(44, 46, 52, 255), r);
    ImU32 col = k > 0.9f ? ERROR : k > 0.75f ? WARN : ACCENT;
    if (k > 0) dl->AddRectFilled(p, ImVec2(p.x + width * (k > 1 ? 1 : k), p.y + h), (col & 0x00FFFFFF) | 0x90000000, r);
    char text[96];
    snprintf(text, sizeof(text), "%s  %.1f / %.0f %s", label, used, total, unit);
    ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(p.x + (width - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), IM_COL32(230, 232, 236, 255), text);
}

}
