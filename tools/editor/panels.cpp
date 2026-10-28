/* The panels round the scene view: Hierarchy, Inspector, Project, Console. */
#include "editor.h"
#include "ui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <map>
#include <math.h>

/* Any widget's change, recorded once per drag or edit for undo */
static bool edited(bool changed) {
    if (changed) ed_edited();
    return changed;
}

static bool colour(const char* label, uint32_t* rgb) {
    float c[3] = { ((*rgb >> 16) & 255) / 255.0f, ((*rgb >> 8) & 255) / 255.0f, (*rgb & 255) / 255.0f };
    ui::row_label(label);
    ImGui::SetNextItemWidth(-1);
    ImGui::PushID(label);
    bool ch = ImGui::ColorEdit3("##c", c, ImGuiColorEditFlags_DisplayHex);
    ImGui::PopID();
    if (!edited(ch)) return false;
    *rgb = ((uint32_t)lroundf(c[0] * 255) << 16) | ((uint32_t)lroundf(c[1] * 255) << 8) | (uint32_t)lroundf(c[2] * 255);
    return true;
}

static bool drag(const char* label, float* v, float speed, float lo, float hi, const char* fmt) {
    ui::row_label(label);
    ImGui::SetNextItemWidth(-1);
    ImGui::PushID(label);
    bool ch = ImGui::DragFloat("##d", v, speed, lo, hi, fmt);
    ImGui::PopID();
    return edited(ch);
}

static bool slider(const char* label, float* v, float lo, float hi, const char* fmt) {
    ui::row_label(label);
    ImGui::SetNextItemWidth(-1);
    ImGui::PushID(label);
    bool ch = ImGui::SliderFloat("##s", v, lo, hi, fmt);
    ImGui::PopID();
    return edited(ch);
}

static bool check(const char* label, bool* v) {
    ui::row_label(label);
    ImGui::PushID(label);
    bool ch = ImGui::Checkbox("##k", v);
    ImGui::PopID();
    return edited(ch);
}

static bool xyz(const char* label, shz_vec3_t* v, float speed, const char* fmt, float reset) {
    return edited(ui::drag_xyz(label, &v->x, speed, fmt, reset));
}

static bool matches(const char* name, const char* filter) {
    if (!*filter) return true;
    std::string a = name, b = filter;
    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    std::transform(b.begin(), b.end(), b.begin(), ::tolower);
    return a.find(b) != std::string::npos;
}

/* A big icon in a rounded square, as the Inspector's title has */
static void badge(const char* icon, ImU32 col) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetFrameHeight() * 1.6f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + h, p.y + h), IM_COL32(44, 46, 52, 255), 8);
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.5f);
    ImVec2 ts = ImGui::CalcTextSize(icon);
    dl->AddText(ImVec2(p.x + (h - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f + 2), col, icon);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(h, h));
    ImGui::SameLine();
}

/* ================================================================
 * Hierarchy
 * ================================================================ */

static void add_menu() {
    if (ImGui::MenuItem(ICON_BOX "  Cube")) ed_add_model("@cube");
    if (ImGui::MenuItem(ICON_LIGHT "  Light")) ed_add_light();
    if (ImGui::MenuItem(ICON_MARKER "  Marker")) ed_add_marker();
    if (ImGui::BeginMenu(ICON_MODEL "  Model", !ed.models.empty())) {
        for (auto& m : ed.models)
            if (ImGui::MenuItem(m.c_str())) ed_add_model(m);
        ImGui::EndMenu();
    }
}

/* One row: an icon in its colour, the name, and an eye at the right */
static bool row(const char* id, const char* icon, ImU32 icon_col, const char* name, bool selected, bool dim, bool* eye) {
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetFrameHeight();
    float w = ImGui::GetContentRegionAvail().x;
    bool clicked = ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(w, h));
    bool hover = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected) dl->AddRectFilled(p, ImVec2(p.x + 3, p.y + h), ui::ACCENT, 1);
    float ty = p.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    dl->AddText(ImVec2(p.x + 10, ty), dim ? IM_COL32(110, 112, 118, 255) : icon_col, icon);
    dl->AddText(ImVec2(p.x + 34, ty), dim ? IM_COL32(120, 123, 130, 255) : ImGui::GetColorU32(ImGuiCol_Text), name);
    if (eye && (hover || !*eye)) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - h, p.y));
        if (ui::icon_button("eye", *eye ? ICON_EYE : ICON_EYE_OFF, *eye ? "Hide" : "Show")) { ed_edited(); *eye = !*eye; }
    }
    ImGui::PopID();
    return clicked;
}

void panel_hierarchy() {
    if (!ImGui::Begin(WIN_HIERARCHY)) { ImGui::End(); return; }
    DCScene* s = ed.scene;
    static char filter[64];

    if (ui::icon_button("add", ICON_PLUS, "Add to the scene")) ImGui::OpenPopup("add");
    if (ImGui::BeginPopup("add")) { add_menu(); ImGui::EndPopup(); }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", ICON_SEARCH "  Search", filter, sizeof(filter));
    ImGui::Spacing();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 1));
    ImGui::BeginChild("rows");
    std::string scene_name = ed.scene_rel.substr(ed.scene_rel.find_last_of('/') + 1) + (ed.dirty ? " *" : "");
    if (ui::font_bold) ImGui::PushFont(ui::font_bold, 0.0f);
    if (row("scene", ICON_SCENE, IM_COL32(150, 180, 255, 255), scene_name.c_str(), ed.sel.kind == SelKind::None, false, nullptr))
        ed.sel = Selection();
    if (ui::font_bold) ImGui::PopFont();
    ImGui::Indent(14);
    for (int i = 0; i < s->light_count; i++) {
        DCSceneLight& l = s->lights[i];
        if (!matches(l.name, filter)) continue;
        char id[16];
        snprintf(id, sizeof(id), "l%d", i);
        if (row(id, l.light.sun ? ICON_SUN : ICON_LIGHT, IM_COL32(250, 210, 90, 255), l.name, ed.sel.is(SelKind::Light, i), false, nullptr)) {
            ed.sel = { SelKind::Light, i };
            if (ImGui::IsMouseDoubleClicked(0)) view_focus_selection();
        }
    }
    for (int i = 0; i < s->object_count; i++) {
        DCSceneObject& o = s->objects[i];
        if (!matches(o.name, filter)) continue;
        char id[16];
        snprintf(id, sizeof(id), "o%d", i);
        bool visible = !o.hidden;
        bool marker = dc_scene_is_marker(&o);
        bool clicked = row(id, marker ? ICON_MARKER : o.model[0] == '@' ? ICON_BOX : ICON_MODEL,
                           marker ? IM_COL32(120, 220, 200, 255) : o.m ? IM_COL32(120, 170, 255, 255) : ui::ERROR,
                           o.name, ed.sel.is(SelKind::Object, i), o.hidden, &visible);
        o.hidden = !visible;
        if (clicked) {
            ed.sel = { SelKind::Object, i };
            if (ImGui::IsMouseDoubleClicked(0)) view_focus_selection();
        }
        if (ImGui::BeginPopupContextItem()) {
            ed.sel = { SelKind::Object, i };
            if (ImGui::MenuItem(ICON_INSPECT "  Frame", "F")) view_focus_selection();
            if (ImGui::MenuItem(ICON_COPY "  Duplicate", "Ctrl+D")) ed_duplicate();
            if (ImGui::MenuItem(ICON_TRASH "  Delete", "Del")) ed_delete();
            ImGui::EndPopup();
        }
    }
    ImGui::Unindent(14);
    if (ImGui::BeginPopupContextWindow("hier_ctx", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        add_menu();
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::End();
}

/* ================================================================
 * Inspector
 * ================================================================ */

/* The top: badge, the name, and the visible toggle */
static void title(const char* icon, ImU32 col, char* name, size_t size, bool* enabled) {
    badge(icon, col);
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, ImGui::GetFrameHeight() * 0.3f));
    if (ui::font_bold) ImGui::PushFont(ui::font_bold, 0.0f);
    ImGui::SetNextItemWidth(enabled ? -ImGui::GetFrameHeight() - 8 : -1);
    edited(ImGui::InputText("##name", name, size));
    if (ui::font_bold) ImGui::PopFont();
    if (enabled) {
        ImGui::SameLine();
        edited(ImGui::Checkbox("##on", enabled));
        ui::tooltip("Visible");
    }
    ImGui::EndGroup();
}

static void inspect_scene() {
    DCScene* s = ed.scene;
    badge(ICON_SCENE, IM_COL32(150, 180, 255, 255));
    ImGui::BeginGroup();
    if (ui::font_bold) ImGui::PushFont(ui::font_bold, 0.0f);
    ImGui::TextUnformatted(ed.scene_rel.c_str());
    if (ui::font_bold) ImGui::PopFont();
    ImGui::TextDisabled("%d objects, %d lights%s", s->object_count, s->light_count, ed.dirty ? ", unsaved" : "");
    ImGui::EndGroup();

    if (ui::component(ICON_PALETTE, "Environment")) colour("Background", &s->background);
    if (ui::component(ICON_FOG, "Fog")) {
        check("Enabled", &s->fog);
        if (s->fog) {
            colour("Colour", &s->fog_rgb);
            drag("Near", &s->fog_near, 0.5f, 0.0f, 10000.0f, "%.1f");
            drag("Far", &s->fog_far, 0.5f, 0.0f, 10000.0f, "%.1f");
        }
    }
    if (ui::component(ICON_CAMERA, "Start camera")) {
        xyz("Position", &s->cam_pos, 0.05f, "%.2f", 0.0f);
        drag("Yaw", &s->cam_yaw, 0.5f, -360.0f, 360.0f, "%.1f\xc2\xb0");
        drag("Pitch", &s->cam_pitch, 0.5f, -89.0f, 89.0f, "%.1f\xc2\xb0");
        drag("FOV", &s->cam_fov, 0.5f, 10.0f, 150.0f, "%.1f\xc2\xb0");
        ImGui::TextDisabled("Yaw 0 looks along +z; positive pitch looks down.");
    }
    if (ui::component(ICON_MAGNET, "Snapping", false)) {
        ui::row_label("Move");
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##sm", &ed.snap_move, 0.05f, 0.01f, 100.0f, "%.2f");
        ui::row_label("Rotate");
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##sr", &ed.snap_rot, 0.5f, 1.0f, 180.0f, "%.0f\xc2\xb0");
        ui::row_label("Scale");
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##ss", &ed.snap_scale, 0.01f, 0.01f, 10.0f, "%.2f");
    }
}

static void set_model(DCSceneObject& o, const char* model) {
    ed_edited();
    snprintf(o.model, sizeof(o.model), "%s", model);
    o.m = nullptr;
    dc_scene_load_models(ed.scene);
}

static void inspect_object(DCSceneObject& o) {
    bool visible = !o.hidden;
    bool marker = dc_scene_is_marker(&o);
    title(marker ? ICON_MARKER : o.model[0] == '@' ? ICON_BOX : ICON_MODEL,
          marker ? IM_COL32(120, 220, 200, 255) : IM_COL32(120, 170, 255, 255), o.name, sizeof(o.name), &visible);
    o.hidden = !visible;

    if (ui::component(ICON_MOVE, "Transform")) {
        xyz("Position", &o.pos, 0.05f, "%.2f", 0.0f);
        xyz("Rotation", &o.rot, 0.5f, "%.1f", 0.0f);
        xyz("Scale", &o.scale, 0.01f, "%.2f", 1.0f);
        ImGui::Dummy(ImVec2(0, 2));
        if (ImGui::Button(ICON_REFRESH "  Reset")) { ed_edited(); o.rot = shz_vec3_init(0, 0, 0); o.scale = shz_vec3_init(1, 1, 1); }
        ImGui::SameLine();
        if (ImGui::Button(ICON_LAYERS "  Drop to ground") && o.m) {
            DCBounds b = dc_model_bounds(o.m, nullptr);
            if (b.min.y <= b.max.y) { ed_edited(); o.pos.y = -b.min.y * o.scale.y; }
        }
        if (o.m && o.m->skeleton && (o.rot.x != 0.0f || o.rot.z != 0.0f))
            ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Skinned: only the turn about y is drawn");
    }

    if (marker) {
        if (ui::component(ICON_MARKER, "Marker"))
            ImGui::TextWrapped("A named place the game reads, as a Blender Empty is: its name says what goes here "
                               "(\"bolt\", \"rat.002\", \"spawn\"). Nothing is drawn; its box is its size.");
        return;
    }

    if (ui::component(ICON_MODEL, "Model")) {
        ui::row_label("Mesh");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##model", o.model)) {
            if (ImGui::Selectable(ICON_BOX "  @cube", !strcmp(o.model, "@cube"))) set_model(o, "@cube");
            for (auto& m : ed.models)
                if (ImGui::Selectable((std::string(ICON_MODEL "  ") + m).c_str(), m == o.model)) set_model(o, m.c_str());
            ImGui::EndCombo();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("DMS_MODEL"))
                set_model(o, std::string((const char*)p->Data, p->DataSize).c_str());
            ImGui::EndDragDropTarget();
        }
        if (o.m && o.m->skeleton) {
            ui::row_label("Clips from");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##anims", o.anims[0] ? o.anims : "its own")) {
                std::string pick;
                bool chose = false;
                if (ImGui::Selectable("its own", !o.anims[0])) chose = true;
                for (auto& m : ed.models)
                    if (m != o.model && ImGui::Selectable((std::string(ICON_MODEL "  ") + m).c_str(), m == o.anims)) { pick = m; chose = true; }
                ImGui::EndCombo();
                if (chose && pick != o.anims) {
                    ed_edited();
                    snprintf(o.anims, sizeof(o.anims), "%s", pick.c_str());
                    /* Loaded again, with its own clips or the other's */
                    dc_scene_reload_model(ed.scene, o.model);
                }
            }
            ui::tooltip("Another model's animations, played on this one's bones by name: a set of player models "
                        "shares one set of clips");
        }
        if (o.m) {
            DCBounds b = dc_model_bounds(o.m, nullptr);
            uint32_t verts = 0;
            for (uint32_t i = 0; i < o.m->mesh_count; i++) verts += o.m->meshes[i].vertex_count;
            ImGui::TextDisabled("%u meshes   %u vertices   %s", o.m->mesh_count, verts, o.m->skeleton ? "skinned" : "static");
            ImGui::TextDisabled("%.2f x %.2f x %.2f units", b.max.x - b.min.x, b.max.y - b.min.y, b.max.z - b.min.z);
            if (o.m->entity_count) {
                int have = ed_marker_count();
                ImGui::Spacing();
                ImGui::BeginDisabled(have > 0);
                char label[96];
                snprintf(label, sizeof(label), ICON_IMPORT "  Bring its %u Empties into the scene", (unsigned)o.m->entity_count);
                if (ImGui::Button(label, ImVec2(-1, 0))) ed_import_entities((int)(&o - ed.scene->objects));
                ImGui::EndDisabled();
                ui::tooltip(have ? "The scene has markers already: the game reads those in place of the model's"
                                 : "As markers you can see and move here; the game then reads them, not the model's");
            }
        } else {
            ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "Not loaded: convert assets (" ICON_PACKAGE " in Project)");
        }
    }

    if (ui::component(ICON_PALETTE, "Look")) {
        uint32_t tint = o.tint ? o.tint : 0xFFFFFF;
        if (colour("Tint", &tint)) o.tint = tint;
        check("Additive", &o.add);
        ui::tooltip("Added to what is behind it: glows, fire, flashes");
        if (o.tint && (o.tint & 0xFFFFFF) != 0xFFFFFF && o.m && !o.m->skeleton)
            ImGui::TextDisabled("On the DC a tint costs a static model\nthe lit path's per-vertex work.");
    }
}

static void inspect_light(DCSceneLight& l) {
    DCLight& d = l.light;
    title(d.sun ? ICON_SUN : ICON_LIGHT, IM_COL32(250, 210, 90, 255), l.name, sizeof(l.name), nullptr);
    if (!ui::component(ICON_LIGHT, "Light")) return;
    ui::row_label("Type");
    ui::segment_begin();
    bool to_sun = ui::segment_button("sun", ICON_SUN, "Sun: far away, shines one way", d.sun, true, false);
    bool to_point = ui::segment_button("point", ICON_LIGHT, "Point: shines out from where it is", !d.sun, false, true);
    ui::segment_end();
    if (to_sun && !d.sun) {
        ed_edited();
        d.sun = true;
        d.pos = shz_vec3_init(-0.4f, -1.0f, -0.3f);
    }
    if (to_point && d.sun) {
        ed_edited();
        d.sun = false;
        d.pos = shz_vec3_init(0.0f, 3.0f, 0.0f);
    }
    xyz(d.sun ? "Direction" : "Position", &d.pos, d.sun ? 0.01f : 0.05f, "%.2f", 0.0f);
    float c[3] = { d.r, d.g, d.b };
    ui::row_label("Colour");
    ImGui::SetNextItemWidth(-1);
    if (edited(ImGui::ColorEdit3("##lc", c, ImGuiColorEditFlags_DisplayHex))) { d.r = c[0]; d.g = c[1]; d.b = c[2]; }
    if (!d.sun) drag("Range", &d.range, 0.1f, 0.0f, 10000.0f, d.range > 0 ? "%.1f" : "500 (0)");
    slider("Ambient", &d.ambient, 0.0f, 1.0f, d.ambient > 0 ? "%.2f" : "0.25 (0)");
    ui::row_label("Bands");
    ImGui::SetNextItemWidth(-1);
    edited(ImGui::SliderInt("##bands", &d.bands, 0, 8, d.bands ? "%d (cel)" : "smooth"));
}

void panel_inspector() {
    ed_edit_frame();
    if (!ImGui::Begin(WIN_INSPECTOR)) { ImGui::End(); return; }
    if (ed.sel.kind == SelKind::Object) inspect_object(ed.scene->objects[ed.sel.index]);
    else if (ed.sel.kind == SelKind::Light) inspect_light(ed.scene->lights[ed.sel.index]);
    else inspect_scene();

    if (ed.sel.kind != SelKind::None) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button(ICON_COPY "  Duplicate", ImVec2(w, 0))) ed_duplicate();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(170, 60, 55, 255));
        if (ImGui::Button(ICON_TRASH "  Delete", ImVec2(w, 0))) ed_delete();
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

/* ================================================================
 * Project: the folders on the left, their files as tiles
 * ================================================================ */

enum EntryKind { ENTRY_MODEL, ENTRY_SCENE, ENTRY_TEXTURE, ENTRY_SOUND, ENTRY_SOURCE, ENTRY_CODE, ENTRY_LIBRARY };
/* The game's code is a second tree beside assets/: its folders are "code:", "code:/src"... */
#define CODE_ROOT "code:"
static bool is_code(const std::string& dir) { return dir.compare(0, 5, CODE_ROOT) == 0; }
/* The Library, the .glb of the other projects, a third: "lib:", "lib:/examples/vase"... */
#define LIB_ROOT "lib:"
static bool is_lib(const std::string& dir) { return dir.compare(0, 4, LIB_ROOT) == 0; }
/* In a namespace of its own: gmod.cpp has an Entry too */
namespace { struct Entry { std::string name, rel; EntryKind kind; }; }
static std::map<std::string, std::vector<Entry>> folders;   /* "" is the top */
static int indexed_gen = -1;
static std::string cwd;

static std::string parent_of(const std::string& dir) {
    size_t s = dir.find_last_of('/');
    return s == std::string::npos ? "" : dir.substr(0, s);
}

static void index_assets() {
    folders.clear();
    folders[""];
    auto add = [](const std::string& rel, EntryKind kind) {
        std::string dir = parent_of(rel);
        for (std::string d = dir; !d.empty(); d = parent_of(d)) folders[d];
        folders[dir].push_back({ rel.substr(dir.empty() ? 0 : dir.size() + 1), rel, kind });
    };
    for (auto& m : ed.models) add(m, ENTRY_MODEL);
    for (auto& s : ed.scenes) add(s, ENTRY_SCENE);
    for (auto& t : ed.textures) add(t, ENTRY_TEXTURE);
    for (auto& s : ed.sounds) add(s, ENTRY_SOUND);
    for (auto& s : ed.sources) {
        std::string built = s.substr(0, s.size() - 4) + ".dms";
        if (std::find(ed.models.begin(), ed.models.end(), built) == ed.models.end()) add(s, ENTRY_SOURCE);
    }
    for (auto& c : ed.code) {
        std::string dir = parent_of(c);
        std::string key = dir.empty() ? CODE_ROOT : CODE_ROOT "/" + dir;
        for (std::string d = key; d != CODE_ROOT; d = parent_of(d)) folders[d];
        folders[CODE_ROOT];
        folders[key].push_back({ c.substr(dir.empty() ? 0 : dir.size() + 1), c, ENTRY_CODE });
    }
    /* examples/vase/assets/vase/vase.glb in lib:/examples/vase */
    for (auto& g : ed_library()) {
        size_t a = g.find('/'), b = a == std::string::npos ? a : g.find('/', a + 1);
        if (b == std::string::npos) continue;
        std::string key = LIB_ROOT "/" + g.substr(0, b);
        folders[LIB_ROOT];
        folders[LIB_ROOT "/" + g.substr(0, a)];
        folders[key].push_back({ g.substr(g.find_last_of('/') + 1), g, ENTRY_LIBRARY });
    }
    for (auto& f : folders)
        std::sort(f.second.begin(), f.second.end(), [](const Entry& a, const Entry& b) {
            return a.kind != b.kind ? a.kind < b.kind : a.name < b.name; });
    if (!folders.count(cwd)) cwd = "";
    indexed_gen = ed.asset_gen;
}

static std::vector<std::string> subfolders(const std::string& dir) {
    std::vector<std::string> kids;
    for (auto& f : folders)
        if (!f.first.empty() && f.first != dir && f.first != CODE_ROOT && f.first != LIB_ROOT && parent_of(f.first) == dir) kids.push_back(f.first);
    return kids;
}

static void folder_tree(const std::string& dir) {
    for (auto& k : subfolders(dir)) {
        bool leaf = subfolders(k).empty();
        std::string label = std::string(cwd == k ? ICON_FOLDER_OPEN : ICON_FOLDER) + "  " + k.substr(k.find_last_of('/') + 1);
        ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow |
                                (leaf ? ImGuiTreeNodeFlags_Leaf : 0) | (cwd == k ? ImGuiTreeNodeFlags_Selected : 0);
        bool open = ImGui::TreeNodeEx(k.c_str(), fl, "%s", label.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) cwd = k;
        if (open) { folder_tree(k); ImGui::TreePop(); }
    }
}

/* A tile: the big icon on a card, the name under it in two lines at most */
static bool tile(const char* id, const char* icon, ImU32 icon_col, float icon_k, const std::string& name, ImU32 text_col,
                 bool lit, float size, const char* tip) {
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float label_h = ImGui::GetTextLineHeight() * 2 + 4;
    ImGui::InvisibleButton("##t", ImVec2(size, size + label_h));
    bool hover = ImGui::IsItemHovered();
    bool open = hover && ImGui::IsMouseDoubleClicked(0);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hover || lit) dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size + label_h), hover ? IM_COL32(255, 255, 255, 14) : ui::ACCENT_SOFT, 8);
    if (icon_k < 0.5f) dl->AddRectFilled(ImVec2(p.x + 8, p.y + 6), ImVec2(p.x + size - 8, p.y + size - 6), IM_COL32(40, 42, 48, 255), 8);
    ImGui::PushFont(nullptr, size * icon_k);
    ImVec2 ts = ImGui::CalcTextSize(icon);
    dl->AddText(ImVec2(p.x + (size - ts.x) * 0.5f, p.y + (size - ts.y) * 0.5f + 3), icon_col, icon);
    ImGui::PopFont();
    float maxw = size - 6;
    std::string line1 = name, line2;
    while (ImGui::CalcTextSize(line1.c_str()).x > maxw && line1.size() > 1) line1.pop_back();
    if (line1.size() < name.size()) {
        line2 = name.substr(line1.size());
        bool cut = false;
        while (ImGui::CalcTextSize((line2 + "..").c_str()).x > maxw && line2.size() > 1) { line2.pop_back(); cut = true; }
        if (cut) line2 += "..";
    }
    float y = p.y + size;
    dl->AddText(ImVec2(p.x + (size - ImGui::CalcTextSize(line1.c_str()).x) * 0.5f, y), text_col, line1.c_str());
    if (!line2.empty())
        dl->AddText(ImVec2(p.x + (size - ImGui::CalcTextSize(line2.c_str()).x) * 0.5f, y + ImGui::GetTextLineHeight()), text_col, line2.c_str());
    if (hover && tip && !ImGui::IsMouseDragging(0)) ImGui::SetTooltip("%s", tip);
    ImGui::PopID();
    return open;
}

void panel_project() {
    if (!ImGui::Begin(WIN_PROJECT)) { ImGui::End(); return; }
    if (indexed_gen != ed.asset_gen) index_assets();
    static char filter[64];

    /* The bar: breadcrumbs, then search and the asset buttons */
    bool in_code = is_code(cwd), in_lib = is_lib(cwd);
    if (in_lib ? ImGui::SmallButton(ICON_IMPORT " Library") : in_code ? ImGui::SmallButton(ICON_CODE " Code") : ImGui::SmallButton(ICON_FOLDER " Assets"))
        cwd = in_lib ? LIB_ROOT : in_code ? CODE_ROOT : "";
    for (size_t i = in_lib ? sizeof(LIB_ROOT) : in_code ? sizeof(CODE_ROOT) : 0; i < cwd.size();) {
        size_t j = cwd.find('/', i);
        if (j == std::string::npos) j = cwd.size();
        ImGui::SameLine(0, 4);
        ImGui::TextDisabled("/");
        ImGui::SameLine(0, 4);
        std::string part = cwd.substr(i, j - i);
        if (ImGui::SmallButton((part + "##" + std::to_string(i)).c_str())) cwd = cwd.substr(0, j);
        i = j + 1;
    }
    float right = 220 + 3 * (ImGui::GetFrameHeight() + 8);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 10, ImGui::GetWindowWidth() - right));
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##pf", ICON_SEARCH "  Search", filter, sizeof(filter));
    ImGui::SameLine();
    if (ui::icon_button("conv", ICON_PACKAGE, "Convert assets: assets/ to pc/ (make assets)")) ed_run("assets");
    ImGui::SameLine();
    if (ui::icon_button("rescan", ICON_REFRESH, "Rescan")) ed_scan_assets();
    ImGui::SameLine();
    if (ui::icon_button("vscode", ICON_VSCODE, "Open the project in VS Code")) ed_open_code("");
    ImGui::Spacing();

    if (ImGui::BeginTable("proj", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("tree", ImGuiTableColumnFlags_WidthFixed, 190);
        ImGui::TableSetupColumn("files", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextColumn();
        ImGui::BeginChild("tree");
        bool open = ImGui::TreeNodeEx("##root", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth |
                                      ImGuiTreeNodeFlags_OpenOnArrow | (cwd.empty() ? ImGuiTreeNodeFlags_Selected : 0),
                                      ICON_FOLDER_OPEN "  Assets");
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) cwd = "";
        if (open) { folder_tree(""); ImGui::TreePop(); }
        if (folders.count(CODE_ROOT)) {
            bool copen = ImGui::TreeNodeEx("##code", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth |
                                           ImGuiTreeNodeFlags_OpenOnArrow | (subfolders(CODE_ROOT).empty() ? ImGuiTreeNodeFlags_Leaf : 0) |
                                           (cwd == CODE_ROOT ? ImGuiTreeNodeFlags_Selected : 0), ICON_CODE "  Code");
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) cwd = CODE_ROOT;
            if (copen) { folder_tree(CODE_ROOT); ImGui::TreePop(); }
        }
        if (folders.count(LIB_ROOT)) {
            bool lopen = ImGui::TreeNodeEx("##lib", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow |
                                           (cwd == LIB_ROOT ? ImGuiTreeNodeFlags_Selected : 0), ICON_IMPORT "  Library");
            ui::tooltip("The .glb of the other examples and games: drag one into the scene to import it");
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) cwd = LIB_ROOT;
            if (lopen) { folder_tree(LIB_ROOT); ImGui::TreePop(); }
        }
        ImGui::EndChild();

        ImGui::TableNextColumn();
        ImGui::BeginChild("files");
        float size = 84, gap = 6;
        int per_row = std::max(1, (int)((ImGui::GetContentRegionAvail().x + gap) / (size + gap)));
        int n = 0;
        if (!*filter) {
            for (auto& d : subfolders(cwd)) {
                if (n++ % per_row) ImGui::SameLine(0, gap);
                if (tile(d.c_str(), ICON_FOLDER, IM_COL32(230, 190, 90, 255), 0.55f, d.substr(d.find_last_of('/') + 1),
                         ImGui::GetColorU32(ImGuiCol_Text), false, size, "Double-click to open"))
                    cwd = d;
            }
        }
        static const char* icons[] = { ICON_MODEL, ICON_SCENE, ICON_IMAGE, ICON_CONSOLE, ICON_PACKAGE, ICON_CODE, ICON_MODEL };
        static const ImU32 cols[] = { IM_COL32(120, 170, 255, 255), IM_COL32(150, 180, 255, 255), IM_COL32(120, 210, 160, 255),
                                      IM_COL32(220, 150, 240, 255), ui::WARN, IM_COL32(90, 200, 230, 255), IM_COL32(170, 150, 255, 255) };
        static const char* tips[] = { "Drag into the scene, or double-click to add it\nRight-click: open in the Model tab", "Double-click to open",
                                      "Texture (.dt)", "Sound (.dca)", "Not converted yet: press Convert assets\nDouble-click: open in the Model tab",
                                      "Double-click to open in VS Code",
                                      "Library: drag into the scene, or double-click, to copy it into this project and add it" };
        for (auto& f : folders) {
            if (*filter ? false : f.first != cwd) continue;
            for (const Entry& e : f.second) {
                if (!matches(e.name.c_str(), filter)) continue;
                if (n++ % per_row) ImGui::SameLine(0, gap);
                std::string tip = e.rel + "\n" + tips[e.kind];
                bool lit = e.kind == ENTRY_SCENE && e.rel == ed.scene_rel;
                if (tile(e.rel.c_str(), icons[e.kind], cols[e.kind], 0.4f, e.name,
                         e.kind == ENTRY_SOURCE ? ui::WARN : ImGui::GetColorU32(ImGuiCol_Text), lit, size, tip.c_str())) {
                    if (e.kind == ENTRY_MODEL) ed_add_model(e.rel);
                    else if (e.kind == ENTRY_SOURCE) model_open(e.rel);
                    else if (e.kind == ENTRY_LIBRARY) ed_import_file(ed.root + e.rel);
                    else if (e.kind == ENTRY_CODE) ed_open_code(e.rel);
                    else if (e.kind == ENTRY_SCENE && e.rel != ed.scene_rel) {
                        if (ed.dirty) ed_log("Save first (or File > Reload): %s has unsaved changes", ed.scene_rel.c_str());
                        else ed_open(e.rel);
                    }
                }
                if ((e.kind == ENTRY_MODEL || e.kind == ENTRY_SOURCE) && ImGui::BeginPopupContextItem("model")) {
                    std::string glb = e.kind == ENTRY_SOURCE ? e.rel : e.rel.substr(0, e.rel.size() - 4) + ".glb";
                    bool have = std::find(ed.sources.begin(), ed.sources.end(), glb) != ed.sources.end();
                    if (ImGui::MenuItem(ICON_MODEL "  Open in Model tab", nullptr, false, have)) model_open(glb);
                    if (!have) ui::tooltip("No .glb for it under assets/");
                    ImGui::EndPopup();
                }
                if (e.kind == ENTRY_MODEL && ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("DMS_MODEL", e.rel.data(), e.rel.size());
                    ImGui::Text(ICON_MODEL "  %s", e.rel.c_str());
                    ImGui::EndDragDropSource();
                }
                if (e.kind == ENTRY_LIBRARY && ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("DMS_LIBRARY", e.rel.data(), e.rel.size());
                    ImGui::Text(ICON_IMPORT "  %s", e.rel.c_str());
                    ImGui::EndDragDropSource();
                }
            }
        }
        if (!n) ImGui::TextDisabled(*filter ? "Nothing matches." : "Empty. Put .glb, .png and .wav files in assets/ and convert.");
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::End();
}

/* ================================================================
 * Console
 * ================================================================ */

static int kind_of(const std::string& l) {
    if (l.find("rror") != std::string::npos || l.find("failed") != std::string::npos ||
        l.find("Cannot") != std::string::npos || l.find("cannot") != std::string::npos || l.find("crash") != std::string::npos)
        return 2;
    if (l.find("arning") != std::string::npos || l.find("no model") != std::string::npos) return 1;
    return 0;
}

void panel_console() {
    if (!ImGui::Begin(WIN_CONSOLE)) { ImGui::End(); return; }
    static char filter[64];
    static bool show[3] = { true, true, true };
    int counts[3] = { 0, 0, 0 };
    for (auto& l : ed.log) counts[kind_of(l)]++;

    if (ui::icon_button("clear", ICON_TRASH, "Clear")) ed.log.clear();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##cf", ICON_SEARCH "  Filter", filter, sizeof(filter));
    static const char* tips[3] = { "Messages", "Warnings", "Errors" };
    static const ImVec4 tints[3] = { ImVec4(0.8f, 0.82f, 0.86f, 1), ImVec4(0.95f, 0.7f, 0.3f, 1), ImVec4(0.95f, 0.45f, 0.4f, 1) };
    static const char* icons[3] = { ICON_CONSOLE, "!", ICON_X };
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 10, ImGui::GetWindowWidth() - 3 * 68 - 12));
    for (int k = 0; k < 3; k++) {
        if (k) ImGui::SameLine(0, 4);
        char label[32];
        snprintf(label, sizeof(label), "%s  %d##k%d", icons[k], counts[k], k);
        ImGui::PushStyleColor(ImGuiCol_Button, show[k] ? ImGui::GetStyleColorVec4(ImGuiCol_Button) : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, show[k] ? tints[k] : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Button(label, ImVec2(64, 0))) show[k] = !show[k];
        ImGui::PopStyleColor(2);
        ui::tooltip(tips[k]);
    }
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(18, 19, 22, 255));
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    static size_t seen;
    bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int row_no = 0;
    for (auto& l : ed.log) {
        int k = kind_of(l);
        if (!show[k] || !matches(l.c_str(), filter)) continue;
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = ImGui::GetTextLineHeightWithSpacing();
        if (row_no++ & 1) dl->AddRectFilled(ImVec2(p.x - 8, p.y), ImVec2(p.x + 4000, p.y + h), IM_COL32(255, 255, 255, 5));
        bool cmd = l.rfind("$ ", 0) == 0 || l.rfind("-- ", 0) == 0;
        ImGui::TextColored(cmd && k == 0 ? ImVec4(0.5f, 0.68f, 1.0f, 1) : tints[k], "%s", l.c_str());
    }
    if (seen != ed.log.size()) {
        if (at_bottom || !seen) ImGui::SetScrollHereY(1.0f);
        seen = ed.log.size();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::End();
}
