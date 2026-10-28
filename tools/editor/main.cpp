/* DMS editor: the app, the files, undo and the jobs it runs.
 *
 *   dms-editor <project> [--scene scenes/main.json]
 *   dms-editor <project> --scene scenes/main.json --shot out.png [--frames N]
 *   dms-editor <project> --ui-shot out.png [--frames N] [--select Name]
 *
 * --shot renders the scene from its own camera without a window and writes
 * a PNG: the same picture the Dreamcast would draw. --ui-shot opens the
 * editor, runs N frames (with Name selected) and writes the whole window. */
#include "editor.h"
#include "ui.h"
#include "embed.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "ImGuizmo.h"
#include "png.h"
#include "pvr_host.h"
#include <SDL3/SDL.h>
#include <dirent.h>
#include <mutex>
#include <stdarg.h>
#include <sys/stat.h>
#include <thread>
#include <algorithm>
#include <functional>

extern "C" {
/* dcsim: the engine's heap (the Dreamcast's RAM) */
void* dcsim_realloc(void* p, size_t size);
void dcsim_free(void* p);
void DCSim_Info(int* inuse, int* freebytes);
}

Editor ed;

/* The engine reads the pad through these; the editor has none. */
extern "C" maple_device_t* maple_enum_type(int port, int function) {
    (void)port; (void)function;
    return nullptr;
}
extern "C" void* maple_dev_status(maple_device_t* device) {
    (void)device;
    return nullptr;
}

/* ================================================================
 * Log, and jobs run in the background (make)
 * ================================================================ */

static std::mutex job_lock;
static std::vector<std::string> job_lines;
static bool job_running, job_done, job_ok;
static std::function<void(bool)> job_then;

void ed_log_async(const char* line) {
    std::lock_guard<std::mutex> g(job_lock);
    job_lines.push_back(line);
}

void ed_log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    ed.log.push_back(buf);
    if (ed.log.size() > 2000) ed.log.erase(ed.log.begin(), ed.log.begin() + 500);
}

void ed_run(const std::string& what, std::function<void(bool ok)> then) {
    if (job_running) { ed_log("A job is running already"); return; }
    job_running = true;
    job_then = then;
    std::string cmd = "make -C '" + ed.project + "' " + what + " 2>&1";
    ed_log("$ %s", cmd.c_str());
    std::thread([cmd] {
        FILE* p = popen(cmd.c_str(), "r");
        char line[1024];
        while (p && fgets(line, sizeof(line), p)) {
            line[strcspn(line, "\n")] = 0;
            std::lock_guard<std::mutex> g(job_lock);
            job_lines.push_back(line);
        }
        int status = p ? pclose(p) : -1;
        std::lock_guard<std::mutex> g(job_lock);
        job_lines.push_back(status == 0 ? "-- done" : "-- failed");
        job_ok = status == 0;
        job_done = true;
    }).detach();
}

static void job_poll() {
    std::function<void(bool)> then;
    bool ok = false;
    {
        std::lock_guard<std::mutex> g(job_lock);
        for (auto& l : job_lines) ed_log("%s", l.c_str());
        job_lines.clear();
        if (!job_done) return;
        job_done = job_running = false;
        then.swap(job_then);
        ok = job_ok;
    }
    ed_scan_assets();
    /* Newly converted models: objects that had none get another try */
    if (ed.scene) dc_scene_load_models(ed.scene);
    if (then) then(ok);
}

/* ================================================================
 * Assets
 * ================================================================ */

static void scan(const std::string& dir, const std::string& rel, const char* ext,
                 std::vector<std::string>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string name = e->d_name, path = dir + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st)) continue;
        if (S_ISDIR(st.st_mode)) scan(path, rel + name + "/", ext, out);
        else if (name.size() > strlen(ext) && name.compare(name.size() - strlen(ext), strlen(ext), ext) == 0)
            out.push_back(rel + name);
    }
    closedir(d);
}

void ed_scan_assets() {
    ed.models.clear();
    ed.sources.clear();
    ed.scenes.clear();
    ed.textures.clear();
    ed.sounds.clear();
    scan(ed.project + "pc", "", ".dms", ed.models);
    scan(ed.project + "pc", "", ".dt", ed.textures);
    scan(ed.project + "pc", "", ".dca", ed.sounds);
    scan(ed.project + "assets", "", ".glb", ed.sources);
    scan(ed.project + "assets", "", ".json", ed.scenes);
    std::sort(ed.models.begin(), ed.models.end());
    std::sort(ed.sources.begin(), ed.sources.end());
    std::sort(ed.scenes.begin(), ed.scenes.end());
    ed.asset_gen++;
}

static long long mtime_of(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) ? 0 : (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
}

static void mkdirs(const std::string& file) {
    for (size_t i = 1; i < file.size(); i++)
        if (file[i] == '/') mkdir(file.substr(0, i).c_str(), 0755);
}

static bool write_file(const std::string& path, const char* text) {
    mkdirs(path);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    return fclose(f) == 0 && ok;
}

/* ================================================================
 * Scene files
 * ================================================================ */

static void set_scene(DCScene* s) {
    if (ed.scene) dc_scene_free(ed.scene);
    ed.scene = s;
    ed.sel = Selection();
    ed.undo.clear();
    ed.redo.clear();
    ed.dirty = false;
}

bool ed_open(const std::string& rel) {
    std::string path = ed.project + "assets/" + rel;
    DCScene* s = dc_scene_load(path.c_str(), ed.asset_root().c_str());
    if (!s) { ed_log("Cannot open %s", path.c_str()); return false; }
    ed.scene_rel = rel;
    set_scene(s);
    ed.scene_mtime = mtime_of(path);
    for (int i = 0; i < s->object_count; i++)
        if (!s->objects[i].m) ed_log("%s: no model %s (convert assets?)", s->objects[i].name, s->objects[i].model);
    ed_log("Opened %s: %d objects, %d lights", rel.c_str(), s->object_count, s->light_count);
    return true;
}

void ed_new(const std::string& rel) {
    ed.scene_rel = rel;
    set_scene(dc_scene_new(ed.asset_root().c_str()));
    ed.scene->background = 0x202830;
    DCSceneLight* l = &ed.scene->lights[ed.scene->light_count++];
    snprintf(l->name, sizeof(l->name), "Sun");
    l->light.pos = shz_vec3_init(-0.4f, -1.0f, -0.3f);
    l->light.sun = true;
    l->light.r = l->light.g = l->light.b = 1.0f;
    l->light.range = 500.0f;
    l->light.ambient = 0.35f;
    ed.scene_mtime = 0;
    ed.dirty = true;
    ed_log("New scene %s (not saved yet)", rel.c_str());
}

bool ed_save() {
    char* text = dc_scene_write(ed.scene);
    if (!text) return false;
    bool ok = write_file(ed.source_path(), text) && write_file(ed.built_path(), text);
    dcsim_free(text);
    if (!ok) { ed_log("Cannot write %s", ed.source_path().c_str()); return false; }
    ed.scene_mtime = mtime_of(ed.source_path());
    ed.dirty = false;
    ed_scan_assets();
    ed_log("Saved assets/%s", ed.scene_rel.c_str());
    return true;
}

/* Someone else (an editor, Claude) changed the file: take theirs. */
static void watch_file() {
    static double next;
    double now = SDL_GetTicks() / 1000.0;
    if (now < next) return;
    next = now + 0.5;
    long long t = mtime_of(ed.source_path());
    if (!t || t == ed.scene_mtime) return;
    if (ed.dirty) {
        static long long warned;
        if (warned != t) ed_log("assets/%s changed on disk, but you have unsaved changes: File > Reload takes theirs", ed.scene_rel.c_str());
        warned = t;
        return;
    }
    Selection keep = ed.sel;
    if (ed_open(ed.scene_rel)) {
        ed.sel = keep;
        if ((ed.sel.kind == SelKind::Object && ed.sel.index >= ed.scene->object_count) ||
            (ed.sel.kind == SelKind::Light && ed.sel.index >= ed.scene->light_count))
            ed.sel = Selection();
        /* pc/ follows the source, as make assets would */
        FILE* f = fopen(ed.source_path().c_str(), "rb");
        if (f) {
            std::string all;
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
            fclose(f);
            write_file(ed.built_path(), all.c_str());
        }
        ed_log("Reloaded assets/%s (changed on disk)", ed.scene_rel.c_str());
    }
}

/* ================================================================
 * Undo
 * ================================================================ */

static Snapshot take() {
    Snapshot s;
    s.head = *ed.scene;
    s.objects.assign(ed.scene->objects, ed.scene->objects + ed.scene->object_count);
    s.sel = ed.sel;
    return s;
}

static void restore(const Snapshot& s) {
    DCScene* sc = ed.scene;
    int n = (int)s.objects.size();
    if (n > sc->object_cap) {
        DCSceneObject* o = (DCSceneObject*)dcsim_realloc(sc->objects, n * sizeof(DCSceneObject));
        if (!o) return;
        sc->objects = o;
        sc->object_cap = n;
    }
    std::copy(s.objects.begin(), s.objects.end(), sc->objects);
    sc->object_count = n;
    sc->background = s.head.background;
    sc->fog = s.head.fog;
    sc->fog_rgb = s.head.fog_rgb;
    sc->fog_near = s.head.fog_near;
    sc->fog_far = s.head.fog_far;
    sc->cam_pos = s.head.cam_pos;
    sc->cam_yaw = s.head.cam_yaw;
    sc->cam_pitch = s.head.cam_pitch;
    sc->cam_fov = s.head.cam_fov;
    memcpy(sc->lights, s.head.lights, sizeof(sc->lights));
    sc->light_count = s.head.light_count;
    ed.sel = s.sel;
    ed.dirty = true;
}

void ed_checkpoint() {
    ed.undo.push_back(take());
    if (ed.undo.size() > 200) ed.undo.erase(ed.undo.begin());
    ed.redo.clear();
}

void ed_changed() { ed.dirty = true; }

/* The scene as it was before the widget now held began changing it */
static Snapshot at_rest;
static bool rest_pushed;

void ed_edit_frame() {
    if (ImGui::IsAnyItemActive()) return;
    at_rest = take();
    rest_pushed = false;
}

void ed_edited() {
    if (!rest_pushed) {
        ed.undo.push_back(at_rest);
        if (ed.undo.size() > 200) ed.undo.erase(ed.undo.begin());
        ed.redo.clear();
        rest_pushed = true;
    }
    ed.dirty = true;
}

void ed_undo() {
    if (ed.undo.empty()) return;
    ed.redo.push_back(take());
    restore(ed.undo.back());
    ed.undo.pop_back();
}

void ed_redo() {
    if (ed.redo.empty()) return;
    ed.undo.push_back(take());
    restore(ed.redo.back());
    ed.redo.pop_back();
}

/* ================================================================
 * Edits
 * ================================================================ */

std::string ed_unique_name(const char* base) {
    for (int n = 1;; n++) {
        char name[32];
        if (n == 1) snprintf(name, sizeof(name), "%s", base);
        else snprintf(name, sizeof(name), "%.24s %d", base, n);
        bool used = false;
        for (int i = 0; i < ed.scene->object_count && !used; i++) used = !strcmp(ed.scene->objects[i].name, name);
        for (int i = 0; i < ed.scene->light_count && !used; i++) used = !strcmp(ed.scene->lights[i].name, name);
        if (!used) return name;
    }
}

/* In front of the view, on the ground if the view looks down at it */
static shz_vec3_t drop_point() {
    float sy = sinf(ed.cam.yaw), cy = cosf(ed.cam.yaw), sp = sinf(ed.cam.pitch), cp = cosf(ed.cam.pitch);
    shz_vec3_t f = shz_vec3_init(sy * cp, -sp, cy * cp);
    float d = 6.0f;
    if (f.y < -0.05f && -ed.cam.pos.y / f.y < 40.0f && ed.cam.pos.y > 0.0f) d = -ed.cam.pos.y / f.y;
    return shz_vec3_init(ed.cam.pos.x + f.x * d, ed.cam.pos.y + f.y * d, ed.cam.pos.z + f.z * d);
}

void ed_add_model(const std::string& model) {
    ed_checkpoint();
    std::string base = model.substr(model.find_last_of('/') + 1);
    base = base.substr(0, base.find_last_of('.'));
    DCSceneObject* o = dc_scene_add(ed.scene, ed_unique_name(base.c_str()).c_str(), model.c_str());
    if (!o) return;
    o->pos = drop_point();
    ed.sel = { SelKind::Object, ed.scene->object_count - 1 };
    ed_changed();
    if (!o->m) ed_log("%s: could not load %s", o->name, model.c_str());
}

void ed_add_marker() {
    ed_checkpoint();
    DCSceneObject* o = dc_scene_add(ed.scene, ed_unique_name("Marker").c_str(), "");
    if (!o) return;
    o->pos = drop_point();
    ed.sel = { SelKind::Object, ed.scene->object_count - 1 };
    ed_changed();
}

int ed_marker_count() {
    int n = 0;
    for (int i = 0; i < ed.scene->object_count; i++) n += dc_scene_is_marker(&ed.scene->objects[i]);
    return n;
}

int ed_import_entities(int index) {
    if (index < 0 || index >= ed.scene->object_count) return 0;
    ed_checkpoint();
    std::string host = ed.scene->objects[index].name;
    int n = dc_scene_import_entities(ed.scene, &ed.scene->objects[index]);
    if (n) ed_changed();
    ed_log("%s: %d Empties now markers in the scene", host.c_str(), n);
    return n;
}

void ed_add_light() {
    if (ed.scene->light_count >= DC_MAX_LIGHTS) { ed_log("The engine takes %d lights at most", DC_MAX_LIGHTS); return; }
    ed_checkpoint();
    DCSceneLight* l = &ed.scene->lights[ed.scene->light_count++];
    memset(l, 0, sizeof(*l));
    snprintf(l->name, sizeof(l->name), "%s", ed_unique_name("Light").c_str());
    l->light.pos = drop_point();
    l->light.pos.y += 2.0f;
    l->light.r = l->light.g = l->light.b = 1.0f;
    l->light.range = 20.0f;
    l->light.ambient = 0.25f;
    ed.sel = { SelKind::Light, ed.scene->light_count - 1 };
    ed_changed();
}

void ed_duplicate() {
    if (ed.sel.kind == SelKind::Object) {
        ed_checkpoint();
        DCSceneObject copy = ed.scene->objects[ed.sel.index];
        DCSceneObject* o = dc_scene_add(ed.scene, ed_unique_name(copy.name).c_str(), copy.model);
        if (!o) return;
        char name[32];
        memcpy(name, o->name, sizeof(name));
        *o = copy;
        memcpy(o->name, name, sizeof(name));
        ed.sel.index = ed.scene->object_count - 1;
        ed_changed();
    } else if (ed.sel.kind == SelKind::Light && ed.scene->light_count < DC_MAX_LIGHTS) {
        ed_checkpoint();
        DCSceneLight* l = &ed.scene->lights[ed.scene->light_count];
        *l = ed.scene->lights[ed.sel.index];
        snprintf(l->name, sizeof(l->name), "%s", ed_unique_name(l->name).c_str());
        ed.sel.index = ed.scene->light_count++;
        ed_changed();
    }
}

void ed_delete() {
    if (ed.sel.kind == SelKind::Object) {
        ed_checkpoint();
        dc_scene_remove(ed.scene, ed.sel.index);
    } else if (ed.sel.kind == SelKind::Light) {
        ed_checkpoint();
        DCScene* s = ed.scene;
        memmove(&s->lights[ed.sel.index], &s->lights[ed.sel.index + 1],
                (s->light_count - ed.sel.index - 1) * sizeof(DCSceneLight));
        s->light_count--;
    } else {
        return;
    }
    ed.sel = Selection();
    ed_changed();
}

/* ================================================================
 * The engine's frame
 * ================================================================ */

static uint8_t frame_rgb[640 * 480 * 3];

static void render_engine(const DCCamera& cam_in) {
    DCCamera cam = cam_in;
    dc_frame_begin();
    dc_scene_apply(ed.scene);
    dc_camera_update(&cam);
    dc_set_camera(&cam);
    dc_scene_draw(ed.scene);
    dc_frame_end();
    pvr_host_read_front(frame_rgb, 640 * 3);
}

static int shot(const char* out, int frames) {
    DCCamera cam;
    dc_scene_camera(ed.scene, &cam);
    for (int i = 0; i < frames; i++) render_engine(cam);
    if (write_png(out, frame_rgb, 640, 480)) { fprintf(stderr, "Cannot write %s\n", out); return 1; }
    printf("shot: %s\n", out);
    return 0;
}

/* ================================================================
 * UI
 * ================================================================ */

/* Hierarchy left, Scene and Game middle, Inspector right, Project and Console under */
static void default_layout(ImGuiID dock) {
    ImGui::DockBuilderRemoveNode(dock);
    ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dock, ImGui::GetMainViewport()->WorkSize);
    ImGuiID left, right, bottom, mid = dock;
    right = ImGui::DockBuilderSplitNode(mid, ImGuiDir_Right, 0.24f, nullptr, &mid);
    left = ImGui::DockBuilderSplitNode(mid, ImGuiDir_Left, 0.2f, nullptr, &mid);
    bottom = ImGui::DockBuilderSplitNode(mid, ImGuiDir_Down, 0.3f, nullptr, &mid);
    ImGui::DockBuilderDockWindow(WIN_HIERARCHY, left);
    ImGui::DockBuilderDockWindow(WIN_INSPECTOR, right);
    ImGui::DockBuilderDockWindow(WIN_PROJECT, bottom);
    ImGui::DockBuilderDockWindow(WIN_CONSOLE, bottom);
    ImGui::DockBuilderDockWindow(WIN_SCENE, mid);
    ImGui::DockBuilderDockWindow(WIN_GAME, mid);
    ImGui::DockBuilderFinish(dock);
}

static char new_name[96] = "scenes/new.json";

static void menu_bar(bool* quit) {
    bool open_new = false;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 7));
    if (!ImGui::BeginMainMenuBar()) { ImGui::PopStyleVar(); return; }
    if (ui::font_bold) ImGui::PushFont(ui::font_bold, 0.0f);
    ImGui::TextColored(ImVec4(0.45f, 0.65f, 1.0f, 1.0f), ICON_DISC " DMS");
    if (ui::font_bold) ImGui::PopFont();
    ImGui::Spacing();
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem(ICON_PLUS "  New Scene...")) open_new = true;
        if (ImGui::BeginMenu(ICON_FOLDER_OPEN "  Open Scene", !ed.scenes.empty())) {
            for (auto& s : ed.scenes)
                if (ImGui::MenuItem(s.c_str(), nullptr, s == ed.scene_rel)) ed_open(s);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(ICON_SAVE "  Save", "Ctrl+S")) ed_save();
        if (ImGui::MenuItem(ICON_REFRESH "  Reload from disk")) ed_open(ed.scene_rel);
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_X "  Quit")) *quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem(ICON_UNDO "  Undo", "Ctrl+Z", false, !ed.undo.empty())) ed_undo();
        if (ImGui::MenuItem(ICON_REDO "  Redo", "Ctrl+Y", false, !ed.redo.empty())) ed_redo();
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_COPY "  Duplicate", "Ctrl+D", false, ed.sel.kind != SelKind::None)) ed_duplicate();
        if (ImGui::MenuItem(ICON_TRASH "  Delete", "Del", false, ed.sel.kind != SelKind::None)) ed_delete();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("GameObject")) {
        if (ImGui::BeginMenu(ICON_MODEL "  Model", !ed.models.empty())) {
            for (auto& m : ed.models)
                if (ImGui::MenuItem(m.c_str())) ed_add_model(m);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(ICON_BOX "  Cube")) ed_add_model("@cube");
        if (ImGui::MenuItem(ICON_LIGHT "  Light")) ed_add_light();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Build")) {
        if (ImGui::MenuItem(ICON_PLAY "  Play", "Ctrl+P")) play_start();
        if (ImGui::MenuItem(ICON_PACKAGE "  Convert assets")) ed_run("assets");
        if (ImGui::MenuItem(ICON_CPU "  Build for Dreamcast")) ed_run("all");
        if (ImGui::MenuItem(ICON_DISC "  Build disc image")) ed_run("disc");
        if (ImGui::MenuItem(ICON_GAME "  Run in its own window")) { if (ed.dirty) ed_save(); ed_run("host-run"); }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem(ICON_GRID "  Grid", nullptr, &ed.show_grid);
        ImGui::MenuItem(ICON_LIGHT "  Gizmos", nullptr, &ed.show_icons);
        if (ImGui::MenuItem(ICON_LAYERS "  Reset layout")) default_layout(ImGui::GetID("Dock"));
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
    ImGui::PopStyleVar();

    if (open_new) ImGui::OpenPopup("New scene");
    if (ImGui::BeginPopupModal("New scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("Under assets/");
        ImGui::SetNextItemWidth(280);
        ImGui::InputText("##path", new_name, sizeof(new_name));
        if (ImGui::Button("Create", ImVec2(136, 0))) {
            ed_new(new_name);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(136, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

/* Tools left, Play in the middle, the Dreamcast's RAM right */
static void toolbar() {
    PlayState ps = play_state();
    bool playing = ps == PlayState::Running || ps == PlayState::Paused;
    float h = ImGui::GetFrameHeight() + 14;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, playing ? IM_COL32(24, 36, 58, 255) : IM_COL32(22, 23, 26, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
    ImGuiWindowFlags f = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration;
    if (ImGui::BeginViewportSideBar("##toolbar", ImGui::GetMainViewport(), ImGuiDir_Up, h, f)) {
        ui::segment_begin();
        if (ui::segment_button("move", ICON_MOVE, "Move (W)", ed.gizmo_op == 0, true, false)) ed.gizmo_op = 0;
        if (ui::segment_button("rot", ICON_ROTATE, "Rotate (E)", ed.gizmo_op == 1, false, false)) ed.gizmo_op = 1;
        if (ui::segment_button("scale", ICON_SCALE, "Scale (R)", ed.gizmo_op == 2, false, true)) ed.gizmo_op = 2;
        ui::segment_end();
        ImGui::SameLine(0, 10);
        ui::segment_begin();
        if (ui::segment_button("space", ed.gizmo_local ? ICON_LOCAL : ICON_GLOBE,
                               ed.gizmo_local ? "Local axes (click for world)" : "World axes (click for local)", false, true, false))
            ed.gizmo_local = !ed.gizmo_local;
        if (ui::segment_button("snap", ICON_MAGNET, "Snap to steps (set in the Inspector)", ed.snap, false, true)) ed.snap = !ed.snap;
        ui::segment_end();
        ImGui::SameLine(0, 16);
        ui::segment_begin();
        if (ui::segment_button("undo", ICON_UNDO, "Undo (Ctrl+Z)", false, true, false)) ed_undo();
        if (ui::segment_button("redo", ICON_REDO, "Redo (Ctrl+Y)", false, false, false)) ed_redo();
        if (ui::segment_button("save", ICON_SAVE, ed.dirty ? "Save (Ctrl+S): unsaved changes" : "Save (Ctrl+S)", false, false, true)) ed_save();
        ui::segment_end();
        if (ed.dirty) {
            ImVec2 r = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(r.x - 5, ImGui::GetItemRectMin().y + 5), 3.5f, ui::WARN);
        }

        /* Play, pause, step: in the middle, whatever the width */
        float seg = (ImGui::GetFrameHeight() + 8) * 3;
        ImGui::SameLine(ImGui::GetWindowWidth() * 0.5f - seg * 0.5f);
        ui::segment_begin();
        const char* play_tip = playing ? "Stop (Ctrl+P)" : ps == PlayState::Building ? "Building..." : "Play: build for Linux and run it in the Game tab (Ctrl+P)";
        if (ui::segment_button("play", playing ? ICON_STOP : ICON_PLAY, play_tip, playing || ps == PlayState::Building, true, false)) {
            if (playing) play_stop(); else play_start();
        }
        if (ui::segment_button("pause", ICON_PAUSE, "Pause (Ctrl+Shift+P)", ps == PlayState::Paused, false, false)) play_pause();
        if (ui::segment_button("step", ICON_STEP, "One frame", false, false, true)) play_step();
        ui::segment_end();

        /* Right: build buttons and the RAM meter */
        float meter_w = 230, right = meter_w + 2 * (ImGui::GetFrameHeight() + 8) + 30;
        ImGui::SameLine(ImGui::GetWindowWidth() - right);
        ui::segment_begin();
        if (ui::segment_button("dc", ICON_CPU, "Build for Dreamcast (make all)", false, true, false)) ed_run("all");
        if (ui::segment_button("disc", ICON_DISC, "Build the disc image (make disc)", false, false, true)) ed_run("disc");
        ui::segment_end();
        ImGui::SameLine(0, 10);
        const DMSEmbed* g = play_shared();
        int used, freeb;
        if (g) { used = g->heap_used; freeb = g->heap_free; }
        else DCSim_Info(&used, &freeb);
        ui::meter(g ? "Game RAM" : "Scene RAM", used / 1048576.0f, (used + freeb) / 1048576.0f, "MB", meter_w, 16.0f);
        ui::tooltip(g ? "The running game's Dreamcast heap: malloc'd now, of what is free after its image and KOS"
                      : "This scene's models on the Dreamcast heap, as the editor has them loaded");
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

static void status_bar() {
    ImGuiWindowFlags f = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 3));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(22, 23, 26, 255));
    if (ImGui::BeginViewportSideBar("##status", ImGui::GetMainViewport(), ImGuiDir_Down, ImGui::GetTextLineHeight() + 8, f)) {
        const std::string last = ed.log.empty() ? "" : ed.log.back();
        bool bad = last.find("rror") != std::string::npos || last.find("failed") != std::string::npos ||
                   last.find("crash") != std::string::npos || last.find("Cannot") != std::string::npos;
        ImGui::TextColored(bad ? ImVec4(0.95f, 0.4f, 0.35f, 1) : ImVec4(0.6f, 0.62f, 0.66f, 1), "%s", last.c_str());
        char right[256];
        snprintf(right, sizeof(right), "%s%s    %d objects   %d lights    %s", ed.scene_rel.c_str(), ed.dirty ? " (unsaved)" : "",
                 ed.scene->object_count, ed.scene->light_count, ed.project.c_str());
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(right).x - 12);
        ImGui::TextDisabled("%s", right);
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

static void shortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || play_has_keyboard()) return;
    bool ctrl = io.KeyCtrl;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S)) ed_save();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) { if (io.KeyShift) ed_redo(); else ed_undo(); }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) ed_redo();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D)) ed_duplicate();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_P)) {
        PlayState ps = play_state();
        if (io.KeyShift) play_pause();
        else if (ps == PlayState::Running || ps == PlayState::Paused) play_stop();
        else play_start();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) ed_delete();
}

/* ================================================================ */

static std::string absolute_dir(const char* path) {
    char buf[PATH_MAX];
    if (!realpath(path, buf)) return "";
    std::string s = buf;
    if (s.back() != '/') s += '/';
    return s;
}

int main(int argc, char** argv) {
    const char* project = ".";
    const char* scene = "scenes/main.json";
    const char* shot_out = nullptr;
    const char* ui_shot = nullptr;
    const char* select = nullptr;
    const char* import = nullptr;
    bool play = false;
    int frames = 3;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scene") && i + 1 < argc) scene = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot_out = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ui-shot") && i + 1 < argc) ui_shot = argv[++i];
        else if (!strcmp(argv[i], "--select") && i + 1 < argc) select = argv[++i];
        else if (!strcmp(argv[i], "--play")) play = true;
        else if (!strcmp(argv[i], "--import-empties") && i + 1 < argc) import = argv[++i];
        else if (argv[i][0] != '-') project = argv[i];
        else {
            fprintf(stderr, "Usage: %s <project> [--scene scenes/main.json] [--shot out.png | --ui-shot out.png]"
                            " [--frames N] [--select Name] [--play] [--import-empties Object]\n", argv[0]);
            return 2;
        }
    }
    ed.project = absolute_dir(project);
    if (ed.project.empty()) { fprintf(stderr, "No project folder %s\n", project); return 2; }
    ed.root = DMS_ROOT_DIR "/";

    if (!SDL_Init(SDL_INIT_EVENTS | (shot_out || import ? 0 : SDL_INIT_VIDEO))) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    DCInitParams params{};
    dc_init(params);
    ed_scan_assets();
    struct stat st;
    if (!stat((ed.project + "assets/" + scene).c_str(), &st)) ed_open(scene);
    else if (shot_out) { fprintf(stderr, "No scene assets/%s\n", scene); return 1; }
    else ed_new(scene);

    /* The object's model's Empties made markers, saved, and done: no window */
    if (import) {
        DCSceneObject* o = dc_scene_find(ed.scene, import);
        if (!o) { fprintf(stderr, "No object %s in %s\n", import, scene); return 1; }
        if (ed_marker_count()) { fprintf(stderr, "%s has markers already\n", scene); return 1; }
        int idx = (int)(o - ed.scene->objects);
        std::vector<DMSEntity> was(o->m ? o->m->entities : nullptr, o->m ? o->m->entities + o->m->entity_count : nullptr);
        int n = ed_import_entities(idx);
        for (auto& l : ed.log) printf("%s\n", l.c_str());
        /* back again, as the game reads them, against what the model had */
        DCSceneObject* host = &ed.scene->objects[idx];
        int back = dc_scene_entities(ed.scene, host);
        float worst = 0.0f;
        int names = 0;
        for (int i = 0; i < back && i < (int)was.size(); i++) {
            const DMSEntity &a = was[i], &b = host->m->entities[i];
            names += strcmp(a.name, b.name) != 0;
            float d[] = { a.pos.x - b.pos.x, a.pos.y - b.pos.y, a.pos.z - b.pos.z,
                          a.scale.x - b.scale.x, a.scale.y - b.scale.y, a.scale.z - b.scale.z };
            for (float v : d) worst = fmaxf(worst, fabsf(v));
            for (int k = 0; k < 9; k++) worst = fmaxf(worst, fabsf(a.rot[k] - b.rot[k]));
        }
        printf("round trip: %d of %d back, %d names differ, worst difference %g\n", back, (int)was.size(), names, worst);
        if (back != (int)was.size() || names || worst > 1e-3f) return 1;
        return n && ed_save() ? 0 : 1;
    }

    if (shot_out) {
        int r = shot(shot_out, frames < 1 ? 1 : frames);
        dc_shutdown();
        SDL_Quit();
        return r;
    }

    dc_scene_camera(ed.scene, &ed.cam);
    if (select) {
        for (int i = 0; i < ed.scene->object_count; i++)
            if (!strcmp(ed.scene->objects[i].name, select)) ed.sel = { SelKind::Object, i };
        for (int i = 0; i < ed.scene->light_count; i++)
            if (!strcmp(ed.scene->lights[i].name, select)) ed.sel = { SelKind::Light, i };
    }
    int frame_no = 0;

    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0.0f) scale = 1.0f;
    SDL_Window* window = SDL_CreateWindow("DMS Editor", (int)(1600 * scale), (int)(960 * scale), SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderVSync(renderer, 1);
    SDL_Texture* frame = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, 640, 480);
    SDL_SetTextureScaleMode(frame, SDL_SCALEMODE_NEAREST);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    static std::string ini = std::string(SDL_GetBasePath()) + "dms-editor-layout.ini"   /* renamed when the windows change */;
    io.IniFilename = ui_shot ? nullptr : ini.c_str();   /* a ui shot always has the default layout */
    ui::apply_theme();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ui::load_fonts(DMS_ROOT_DIR "/host/third_party/fonts/", scale);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    if (play) play_start();

    bool quit = false, first = true;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) quit = true;
        }
        job_poll();
        play_poll();
        watch_file();

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();

        menu_bar(&quit);
        toolbar();
        status_bar();
        ImGuiID dock = ImGui::GetID("Dock");
        if (first && (!ImGui::DockBuilderGetNode(dock) || !ImGui::DockBuilderGetNode(dock)->IsSplitNode())) default_layout(dock);
        first = false;
        ImGui::DockSpaceOverViewport(dock, ImGui::GetMainViewport(), ImGuiDockNodeFlags_NoWindowMenuButton);
        shortcuts();

        render_engine(ed.cam);
        SDL_UpdateTexture(frame, nullptr, frame_rgb, 640 * 3);
        ed.frame_texture = frame;

        panel_hierarchy();
        view_window();
        game_window(renderer);
        panel_inspector();
        panel_project();
        panel_console();

        char title[256];
        snprintf(title, sizeof(title), "DMS Editor - %s%s - %s", ed.scene_rel.c_str(), ed.dirty ? " *" : "", ed.project.c_str());
        SDL_SetWindowTitle(window, title);

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 22, 23, 26, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        /* A ui shot waits for a played game's first frames */
        const DMSEmbed* g = play_shared();
        bool waiting = play && (play_state() == PlayState::Building || (g && g->frame < 30));
        if (ui_shot && ++frame_no >= frames && !waiting) {
            SDL_Surface* s = SDL_RenderReadPixels(renderer, nullptr);
            SDL_Surface* rgb = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGB24) : nullptr;
            if (rgb) {
                std::vector<uint8_t> rows(rgb->w * rgb->h * 3);
                for (int y = 0; y < rgb->h; y++) memcpy(&rows[y * rgb->w * 3], (uint8_t*)rgb->pixels + y * rgb->pitch, rgb->w * 3);
                if (!write_png(ui_shot, rows.data(), rgb->w, rgb->h)) printf("ui shot: %s\n", ui_shot);
            }
            SDL_DestroySurface(rgb);
            SDL_DestroySurface(s);
            quit = true;
        }
        SDL_RenderPresent(renderer);
    }

    play_shutdown();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    dc_scene_free(ed.scene);
    dc_shutdown();
    SDL_DestroyTexture(frame);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
