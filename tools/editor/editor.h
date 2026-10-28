/* DMS editor: a scene editor drawn by the engine itself, on gpu_pvr (the
 * software PVR), so what it shows is what the Dreamcast draws. The editing
 * UI is Dear ImGui around that picture. */
#pragma once

#include <kos.h>
#include <sh4zam/shz_sh4zam.h>
extern "C" {
#include "dms/dc_engine.h"
#include "dms/dc_draw.h"
#include "dms/dc_scene.h"
}
#include <functional>
#include <string>
#include <vector>

struct ImVec2;

enum class SelKind { None, Object, Light };

struct Selection {
    SelKind kind = SelKind::None;
    int index = -1;
    bool is(SelKind k, int i) const { return kind == k && index == i; }
};

/* What undo puts back: everything in the scene but the loaded models, which
 * stay in the scene's model list for as long as it lives. */
struct Snapshot {
    DCScene head;
    std::vector<DCSceneObject> objects;
    Selection sel;
};

struct Editor {
    std::string root;           /* the repository, with a / */
    std::string project;        /* the project folder, absolute, with a / */
    std::string scene_rel;      /* the scene, under assets/ (and pc/) */
    DCScene* scene = nullptr;
    Selection sel;
    bool dirty = false;
    long long scene_mtime = 0;  /* of assets/<scene_rel> as last read or written */

    /* The view's camera: a DCCamera, so the engine draws through it as is */
    DCCamera cam;
    float move_speed = 8.0f;

    int gizmo_op = 0;           /* 0 move, 1 rotate, 2 scale */
    bool gizmo_local = false;
    bool snap = false;
    float snap_move = 0.5f, snap_rot = 15.0f, snap_scale = 0.1f;
    bool show_grid = true, show_icons = true;

    void* frame_texture = nullptr;  /* the engine's frame (SDL_Texture) */

    std::vector<Snapshot> undo, redo;
    std::vector<std::string> log;
    std::vector<std::string> models;    /* .dms under pc/ */
    std::vector<std::string> sources;   /* .glb under assets/ */
    std::vector<std::string> code;      /* the game's own files: .c, .h, Makefile (not assets/, pc/) */
    std::vector<std::string> scenes;    /* .json under assets/ */
    std::vector<std::string> textures;  /* .dt under pc/ */
    std::vector<std::string> sounds;    /* .dca under pc/ */
    int asset_gen = 0;                  /* counts rescans */

    std::string asset_root() const { return project + "pc/"; }
    std::string source_path() const { return project + "assets/" + scene_rel; }
    std::string built_path() const { return project + "pc/" + scene_rel; }
};

extern Editor ed;

/* main.cpp */
void ed_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void ed_checkpoint();           /* call before a change, for undo */
void ed_changed();              /* call after one */
/* Widgets: ed_edit_frame() before them each frame, ed_edited() when one
 * changes something. One undo step per drag or edit, however many frames. */
void ed_edit_frame();
void ed_edited();
void ed_undo();
void ed_redo();
bool ed_save();
void ed_switch_scene(const std::string& rel);
void ed_open_code(const std::string& rel);      /* in VS Code ($DMS_CODE_EDITOR); "" the project */   /* save, stop the game, open */
bool ed_open(const std::string& scene_rel);
void ed_new(const std::string& scene_rel);
void ed_scan_assets();
/* make <what> in the project, in the background; then() after, on this thread */
void ed_run(const std::string& what, std::function<void(bool ok)> then = nullptr);
void ed_log_async(const char* line);
const char* ed_job();           /* the make target running, or NULL */
void ed_job_stop();
std::string ed_project_name();  /* its folder's name, as make's NAME */
/* On the Dreamcast: make run (KOS_LOADER: dc-tool-ip and the like); again stops it */
void dc_run_toggle();
/* In Flycast: make disc, then the .cdi in it; again closes it */
void flycast_toggle();
bool flycast_running();    /* from another thread */
void ed_add_model(const std::string& model);
void ed_add_light();
void ed_add_marker();
/* The Empties of object index's model into the scene as markers; how many */
int  ed_import_entities(int index);
int  ed_marker_count();
void ed_duplicate();
void ed_delete();
std::string ed_unique_name(const char* base);

/* view.cpp: the scene view and its camera */
void view_window();
void view_focus_selection();
/* The engine's projection, as the gizmo wants it (OpenGL convention) */
void view_matrices(const DCCamera& cam, float view[16], float proj[16]);
/* World point to the 640x480 frame; false if behind the camera */
bool view_project(const DCCamera& cam, shz_vec3_t p, float* x, float* y);

/* play.cpp: the game, built for Linux, running in the Game tab */
enum class PlayState { Stopped, Building, Running, Paused };
struct DMSEmbed;
PlayState play_state();
const DMSEmbed* play_shared();      /* while it runs */
void play_start();
void play_stop();
void play_pause();
void play_step();
void play_poll();
bool play_has_keyboard();
void game_window(void* renderer);
void play_shutdown();

/* panels.cpp */
void panel_hierarchy();
void panel_inspector();
void panel_project();
void panel_console();
