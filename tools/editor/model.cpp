/* The Model tab: one of the project's .glb files on its own, as the
 * Dreamcast will draw it, and how it is converted: how many of its
 * triangles are kept, and each texture's size, format, VQ and mipmaps. A
 * change is converted in the background into a preview folder and shown;
 * Apply writes it next to the .glb (<name>.import.json, which strippy and
 * make assets read) and converts the project's own copy. */
#include "editor.h"
#include "ui.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "pvr_host.h"
#include <SDL3/SDL.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <math.h>
#include <thread>
extern "C" {
#include "dms/dc_model.h"
}
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "tools/converter/include/stb_image.h"

static const float DEG = 3.14159265f / 180.0f;

/* ================================================================
 * A little JSON: enough for strippy -info and the import file
 * ================================================================ */

struct Json {
    enum Kind { NUL, BOOL, NUM, STR, ARR, OBJ } kind = NUL;
    double num = 0;
    bool b = false;
    std::string str;
    std::vector<Json> items;
    std::vector<std::string> keys;     /* an object's, with items */

    const Json* get(const char* k) const {
        for (size_t i = 0; i < keys.size(); i++) if (keys[i] == k) return &items[i];
        return nullptr;
    }
    double n(const char* k, double d) const { const Json* j = get(k); return j && j->kind == NUM ? j->num : d; }
    std::string s(const char* k, const char* d) const { const Json* j = get(k); return j && j->kind == STR ? j->str : d; }
    bool flag(const char* k, bool d) const { const Json* j = get(k); return j && j->kind == BOOL ? j->b : d; }
};

static void ws(const char*& p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; }

static bool parse(const char*& p, Json& out) {
    ws(p);
    if (*p == '{' || *p == '[') {
        bool obj = *p++ == '{';
        out.kind = obj ? Json::OBJ : Json::ARR;
        ws(p);
        if (*p == (obj ? '}' : ']')) { p++; return true; }
        for (;;) {
            if (obj) {
                Json k;
                if (!parse(p, k) || k.kind != Json::STR) return false;
                ws(p);
                if (*p++ != ':') return false;
                out.keys.push_back(k.str);
            }
            out.items.emplace_back();
            if (!parse(p, out.items.back())) return false;
            ws(p);
            if (*p == ',') { p++; continue; }
            if (*p++ == (obj ? '}' : ']')) return true;
            return false;
        }
    }
    if (*p == '"') {
        out.kind = Json::STR;
        for (p++; *p && *p != '"'; p++) {
            if (*p == '\\' && p[1]) p++;
            out.str += *p;
        }
        if (*p != '"') return false;
        p++;
        return true;
    }
    if (!strncmp(p, "true", 4)) { out.kind = Json::BOOL; out.b = true; p += 4; return true; }
    if (!strncmp(p, "false", 5)) { out.kind = Json::BOOL; p += 5; return true; }
    if (!strncmp(p, "null", 4)) { p += 4; return true; }
    char* end;
    out.num = strtod(p, &end);
    if (end == p) return false;
    out.kind = Json::NUM;
    p = end;
    return true;
}

static std::string read_all(const std::string& path) {
    std::string all;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return all;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
    fclose(f);
    return all;
}

static long file_size(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) ? -1 : (long)st.st_size;
}

/* ================================================================
 * The model and its settings
 * ================================================================ */

struct Image {
    int index = 0, w = 0, h = 0;
    std::string name, alpha;
    bool used = true;
};

/* A texture's settings: 0 / "" / -1 are strippy's own choice */
struct TexSet {
    int size = 0;
    std::string format = "auto";
    bool vq = true;
    std::string codebook = "full";
    int mips = -1;
    bool operator==(const TexSet& o) const {
        return size == o.size && format == o.format && vq == o.vq && codebook == o.codebook && mips == o.mips;
    }
};

struct Settings {
    float decimate = 1.0f;
    std::vector<TexSet> tex;    /* by image index */
    bool operator==(const Settings& o) const { return decimate == o.decimate && tex == o.tex; }
};

struct Thumb { SDL_Texture* t = nullptr; int w = 0, h = 0; float u = 1; };   /* u: the top level's width of it */

static struct {
    std::string src;            /* under assets/, the .glb */
    bool info_ok = false;
    int meshes = 0, tris = 0, verts = 0;
    bool skinned = false;
    std::vector<Image> images;
    Settings set, saved;        /* as edited, as in the import file */

    /* The preview: converted from set in the background */
    std::string dir;            /* the folder it is in */
    DMSModel* model = nullptr;  /* loaded from dir */
    DMSModel* built = nullptr;  /* the project's own, pc/, as converted now */
    bool show_built = false;
    std::vector<Thumb> thumbs;  /* by image index */
    std::vector<long> tex_bytes, built_tex_bytes;
    long dms_bytes = 0, built_dms_bytes = 0;
    int ptris = 0, btris = 0;

    std::thread worker;
    std::atomic<int> state{0};  /* 0 idle, 1 converting, 2 done, 3 failed */
    std::string worker_log;
    bool want = false;          /* a change waiting to be converted */
    double changed_at = 0;
    int gen = 0;

    /* The view */
    float yaw = 30 * DEG, pitch = 20 * DEG, dist = 0;
    bool spin = false;
    SDL_Texture* frame = nullptr;
    uint8_t rgb[640 * 480 * 3];
} mv;

static std::string built_rel(const std::string& src) {
    return src.substr(0, src.find_last_of('.')) + ".dms";
}
static std::string stem(const std::string& src) {
    std::string s = src.substr(src.find_last_of('/') + 1);
    return s.substr(0, s.find_last_of('.'));
}
static std::string import_path() {
    return ed.project + "assets/" + mv.src.substr(0, mv.src.find_last_of('.')) + ".import.json";
}
static std::string strippy() { return ed.root + "tools/converter/strippy"; }

/* The project bakes its lights into its models (BAKE = 1 in its Makefile) */
static bool project_bakes() {
    std::string mk = read_all(ed.project + "Makefile");
    for (size_t at = mk.find("BAKE"); at != std::string::npos; at = mk.find("BAKE", at + 1)) {
        size_t p = at + 4;
        while (p < mk.size() && (mk[p] == ' ' || mk[p] == '=' || mk[p] == ':' || mk[p] == '?')) p++;
        if (p < mk.size() && mk[p] == '1' && (at == 0 || mk[at - 1] == '\n')) return true;
    }
    return false;
}

static std::string settings_json(const Settings& s) {
    std::string j = "{\n";
    char b[160];
    snprintf(b, sizeof(b), "  \"decimate\": %.3f", s.decimate);
    j += b;
    bool any = false;
    for (size_t i = 0; i < s.tex.size(); i++) {
        const TexSet& t = s.tex[i];
        if (t == TexSet()) continue;
        j += any ? ",\n" : ",\n  \"textures\": {\n";
        any = true;
        snprintf(b, sizeof(b), "    \"%zu\": { \"size\": %d, \"format\": \"%s\", \"vq\": %s, \"codebook\": \"%s\"",
                 i, t.size, t.format.c_str(), t.vq ? "true" : "false", t.codebook.c_str());
        j += b;
        if (t.mips >= 0) j += t.mips ? ", \"mips\": true" : ", \"mips\": false";
        j += " }";
    }
    if (any) j += "\n  }";
    return j + "\n}\n";
}

static Settings settings_read(const std::string& path, size_t images) {
    Settings s;
    s.tex.resize(images);
    std::string text = read_all(path);
    Json j;
    const char* p = text.c_str();
    if (text.empty() || !parse(p, j) || j.kind != Json::OBJ) return s;
    s.decimate = (float)j.n("decimate", 1.0);
    if (const Json* t = j.get("textures"))
        for (size_t i = 0; i < t->keys.size(); i++) {
            size_t idx = (size_t)atoi(t->keys[i].c_str());
            if (idx >= s.tex.size()) continue;
            const Json& e = t->items[i];
            TexSet& ts = s.tex[idx];
            ts.size = (int)e.n("size", 0);
            ts.format = e.s("format", "auto");
            ts.vq = e.flag("vq", true);
            if (const Json* c = e.get("codebook"))
                ts.codebook = c->kind == Json::NUM ? std::to_string((int)c->num) : c->str;
            if (const Json* m = e.get("mips")) ts.mips = m->kind == Json::BOOL ? m->b : -1;
        }
    return s;
}

/* Runs a command; its output, and whether it went */
static bool run(const std::string& cmd, std::string* out) {
    FILE* p = popen((cmd + " 2>&1").c_str(), "r");
    if (!p) return false;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) if (out) out->append(buf, n);
    return pclose(p) == 0;
}

static std::string q(const std::string& s) { return "'" + s + "'"; }

static void thumbs_free() {
    for (Thumb& t : mv.thumbs) if (t.t) SDL_DestroyTexture(t.t);
    mv.thumbs.clear();
}

static void preview_free() {
    if (mv.model) dc_model_free(mv.model);
    mv.model = nullptr;
    thumbs_free();
}

static int tris_of(const DMSModel* m) {
    int n = 0;
    for (uint32_t i = 0; m && i < m->mesh_count; i++) n += (int)m->meshes[i].tri_count;
    return n;
}

/* Each image's texture as the model holds it (the .dt is embedded in the .dms) */
static void texture_sizes(const DMSModel* m, std::vector<long>& out) {
    out.assign(mv.images.size(), -1);
    for (size_t i = 0; m && i < mv.images.size(); i++) {
        int t = mv.images[i].index;
        if (t < m->texture_count && m->textures[t].ptr) out[i] = (long)m->textures[t].hdr.chunk_size;
    }
}

/* The project's own conversion, as pc/ has it now */
static void built_load() {
    if (mv.built) dc_model_free(mv.built);
    std::string path = ed.asset_root() + built_rel(mv.src);
    mv.built = file_size(path) > 0 ? dc_model_load(path.c_str()) : nullptr;
    mv.btris = tris_of(mv.built);
    mv.built_dms_bytes = file_size(path);
    texture_sizes(mv.built, mv.built_tex_bytes);
}

/* Converts the settings into a fresh preview folder, off the UI thread */
static void convert() {
    if (mv.state == 1) { mv.want = true; return; }
    if (mv.worker.joinable()) mv.worker.join();
    mv.want = false;
    std::string dir = ed.root + "build/editor/preview/" + std::to_string(++mv.gen) + "/";
    std::string json = settings_json(mv.set), glb = ed.project + "assets/" + mv.src;
    bool bake = project_bakes();
    mv.state = 1;
    mv.worker = std::thread([dir, json, glb, bake] {
        std::string log;
        run("rm -rf " + q(dir) + " && mkdir -p " + q(dir), nullptr);
        FILE* f = fopen((dir + "import.json").c_str(), "w");
        if (f) { fputs(json.c_str(), f); fclose(f); }
        bool ok = run(q(strippy()) + (bake ? " -bake" : "") + " -keep-dt -import " + q(dir + "import.json") +
                      " -out " + q(dir) + " " + q(glb), &log);
        mv.worker_log = log;
        mv.state = ok ? 2 : 3;
    });
    mv.dir = dir;
}

static SDL_Renderer* renderer;

static void thumbs_load() {
    thumbs_free();
    mv.thumbs.resize(mv.images.size());
    SDL_Renderer* r = renderer;
    if (!r) return;
    for (size_t i = 0; i < mv.images.size(); i++) {
        std::string png = mv.dir + stem(mv.src) + "_texture_" + std::to_string(mv.images[i].index) + ".dt.png";
        int w, h, n;
        stbi_uc* px = stbi_load(png.c_str(), &w, &h, &n, 4);
        if (!px) continue;
        SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
        if (t) {
            SDL_UpdateTexture(t, nullptr, px, w * 4);
            SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
            /* A mipmapped preview has the smaller levels beside the top one */
            float u = w == h + h / 2 ? (float)h / (float)w : 1.0f;
            mv.thumbs[i] = { t, (int)(w * u), h, u };
        }
        stbi_image_free(px);
    }
}

/* A finished conversion: load it */
static void preview_poll() {
    int s = mv.state;
    if (s < 2) return;
    mv.worker.join();
    mv.state = 0;
    if (s == 3) {
        ed_log("Model preview: strippy failed on %s", mv.src.c_str());
        size_t from = mv.worker_log.size() > 600 ? mv.worker_log.size() - 600 : 0;
        ed_log("%s", mv.worker_log.substr(from).c_str());
    } else {
        preview_free();
        std::string path = mv.dir + stem(mv.src) + ".dms";
        mv.model = dc_model_load(path.c_str());
        if (!mv.model) ed_log("Model preview: cannot load %s", path.c_str());
        mv.ptris = tris_of(mv.model);
        mv.dms_bytes = file_size(path);
        texture_sizes(mv.model, mv.tex_bytes);
        thumbs_load();
        /* Older previews are not wanted */
        std::string old = ed.root + "build/editor/preview/" + std::to_string(mv.gen - 1);
        run("rm -rf " + q(old), nullptr);
    }
    if (mv.want) convert();
}

void model_open(const std::string& src) {
    if (mv.state == 1) mv.worker.join();
    else if (mv.worker.joinable()) mv.worker.join();
    mv.state = 0;
    preview_free();
    mv.src = src;
    mv.images.clear();
    mv.info_ok = false;
    std::string out;
    std::string glb = ed.project + "assets/" + src;
    if (run(q(strippy()) + " -info " + q(glb) + " 2>/dev/null", &out)) {
        Json j;
        const char* p = out.c_str();
        while (*p && *p != '{') p++;
        if (parse(p, j) && j.kind == Json::OBJ) {
            mv.info_ok = true;
            mv.meshes = (int)j.n("meshes", 0);
            mv.tris = (int)j.n("triangles", 0);
            mv.verts = (int)j.n("vertices", 0);
            mv.skinned = j.flag("skinned", false);
            if (const Json* a = j.get("images"))
                for (const Json& e : a->items) {
                    Image im;
                    im.index = (int)e.n("index", (double)mv.images.size());
                    im.name = e.s("name", "");
                    im.w = (int)e.n("width", 0);
                    im.h = (int)e.n("height", 0);
                    im.alpha = e.s("alpha", "");
                    im.used = e.flag("used", true);
                    mv.images.push_back(im);
                }
        }
    }
    if (!mv.info_ok) ed_log("Model: strippy -info failed on %s", src.c_str());
    size_t slots = 0;
    for (const Image& im : mv.images) slots = std::max(slots, (size_t)im.index + 1);
    mv.saved = mv.set = settings_read(import_path(), slots);
    mv.dist = 0;
    built_load();
    convert();
    if (ImGuiWindow* w = ImGui::FindWindowByName(WIN_MODEL)) ImGui::FocusWindow(w);
}

static void apply() {
    std::string path = import_path();
    if (mv.set == Settings()) {
        unlink(path.c_str());
        /* make does not see a file gone: the old conversion goes too */
        unlink((ed.asset_root() + built_rel(mv.src)).c_str());
        ed_log("Model: %s converts as it did (no import file)", mv.src.c_str());
    } else {
        FILE* f = fopen(path.c_str(), "w");
        if (!f) { ed_log("Cannot write %s", path.c_str()); return; }
        fputs(settings_json(mv.set).c_str(), f);
        fclose(f);
        ed_log("Wrote %s", path.c_str());
    }
    mv.saved = mv.set;
    ed_run("assets", [](bool ok) {
        if (!ok) return;
        built_load();
        ed_scan_assets();
        /* The scene's objects with it get the new one, unsaved edits and all */
        if (ed.scene && dc_scene_reload_model(ed.scene, built_rel(mv.src).c_str()))
            ed_log("Reloaded %s in the scene", built_rel(mv.src).c_str());
    });
}

/* ================================================================
 * The view
 * ================================================================ */

static void render(DMSModel* m) {
    shz_vec3_t lo = m ? m->bound_min : shz_vec3_init(-1, -1, -1), hi = m ? m->bound_max : shz_vec3_init(1, 1, 1);
    shz_vec3_t c = shz_vec3_init((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f);
    float dx = hi.x - lo.x, dy = hi.y - lo.y, dz = hi.z - lo.z;
    float radius = 0.5f * sqrtf(dx * dx + dy * dy + dz * dz);
    if (radius < 0.01f) radius = 1.0f;
    if (mv.dist <= 0) mv.dist = radius * 2.4f;

    DCCamera cam = ed.cam;
    cam.yaw = mv.yaw;
    cam.pitch = mv.pitch;
    float sy = sinf(cam.yaw), cy = cosf(cam.yaw), sp = sinf(cam.pitch), cp = cosf(cam.pitch);
    cam.pos = shz_vec3_init(c.x - sy * cp * mv.dist, c.y + sp * mv.dist, c.z - cy * cp * mv.dist);
    cam.view = DCViewport{};

    dc_frame_begin();
    dc_set_clear_color(0xFF2A2D33);
    dc_set_fog_off();
    DCLight sun = {};
    /* From over the viewer's shoulder, whichever way the model is turned */
    float ly = cam.yaw + 0.6f, lp = cam.pitch + 0.5f;
    sun.pos = shz_vec3_init(sinf(ly) * cosf(lp), -sinf(lp), cosf(ly) * cosf(lp));
    sun.sun = true;
    sun.ambient = 0.4f;
    dc_set_light(&sun);
    dc_camera_update(&cam);
    dc_set_camera(&cam);
    if (m) {
        if (m->skeleton) dc_model_animate(m, dc_delta_time());
        dc_draw(m, shz_vec3_init(0, 0, 0));
    }
    dc_frame_end();
    pvr_host_read_front(mv.rgb, 640 * 3);
}

static const char* KB(long b, char* s, size_t n) {
    if (b < 0) snprintf(s, n, "-");
    else snprintf(s, n, "%.1f K", b / 1024.0);
    return s;
}

static bool combo_str(const char* id, std::string& v, const char* const* opts, int n) {
    bool changed = false;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.6f);
    if (ImGui::BeginCombo(id, v.c_str())) {
        for (int i = 0; i < n; i++)
            if (ImGui::Selectable(opts[i], v == opts[i])) { v = opts[i]; changed = true; }
        ImGui::EndCombo();
    }
    return changed;
}

static bool texture_row(size_t i) {
    const Image& im = mv.images[i];
    if ((size_t)im.index >= mv.set.tex.size()) return false;
    TexSet& t = mv.set.tex[im.index];
    bool changed = false;
    char b[64], b2[32];
    ImGui::PushID((int)i);
    ImGui::TableNextRow();

    ImGui::TableNextColumn();
    if (i < mv.thumbs.size() && mv.thumbs[i].t) {
        const Thumb& th = mv.thumbs[i];
        float s = ImGui::GetFontSize() * 3.0f, k = s / (float)std::max(th.w, th.h);
        ImGui::Image((ImTextureID)(intptr_t)th.t, ImVec2(th.w * k, th.h * k), ImVec2(0, 0), ImVec2(th.u, 1));
        if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
            float big = 256.0f / (float)std::max(th.w, th.h);
            if (big < 1) big = 1;
            ImGui::Image((ImTextureID)(intptr_t)th.t, ImVec2(th.w * big, th.h * big), ImVec2(0, 0), ImVec2(th.u, 1));
            ImGui::Text("%d x %d, as converted", th.w, th.h);
            ImGui::EndTooltip();
        }
    } else {
        ImGui::TextDisabled(ICON_IMAGE);
    }

    ImGui::TableNextColumn();
    ImGui::TextUnformatted(im.name.empty() ? ("texture " + std::to_string(im.index)).c_str() : im.name.c_str());
    ImGui::TextDisabled("%d x %d%s%s", im.w, im.h, im.alpha.empty() || im.alpha == "none" ? "" : "  alpha ",
                        im.alpha == "none" ? "" : im.alpha.c_str());
    if (!im.used) { ImGui::TextDisabled("not drawn"); ImGui::PopID(); return false; }

    ImGui::TableNextColumn();
    /* Size: the original, or a power of two under it */
    int biggest = std::max(im.w, im.h);
    snprintf(b, sizeof(b), "%s", t.size ? std::to_string(t.size).c_str() : "original");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
    if (ImGui::BeginCombo("##size", b)) {
        if (ImGui::Selectable("original", t.size == 0)) { t.size = 0; changed = true; }
        for (int s = 1024; s >= 8; s /= 2) {
            if (s >= biggest) continue;
            snprintf(b2, sizeof(b2), "%d", s);
            if (ImGui::Selectable(b2, t.size == s)) { t.size = s; changed = true; }
        }
        ImGui::EndCombo();
    }
    static const char* const formats[] = { "auto", "565", "1555", "4444", "yuv", "pal8", "pal4" };
    changed |= combo_str("##fmt", t.format, formats, 7);
    ui::tooltip("auto: 565 when opaque, 1555 for cutouts, 4444 for soft alpha.\n"
                "yuv: smoother gradients, slower filtering. pal8 / pal4: 256 / 16 colours");

    ImGui::TableNextColumn();
    changed |= ImGui::Checkbox("VQ", &t.vq);
    ui::tooltip("Vector quantised: about an eighth of the memory, a little blur");
    if (t.vq) {
        static const char* const books[] = { "full", "small", "128", "64", "32", "16" };
        changed |= combo_str("##book", t.codebook, books, 6);
        ui::tooltip("The codebook: smaller is less memory and a faster fill, coarser colour");
    }
    bool mip = t.mips < 0 ? im.w == im.h : t.mips == 1;
    if (ImGui::Checkbox("Mips", &mip)) { t.mips = mip ? 1 : 0; changed = true; }
    ui::tooltip("Mipmaps: less shimmer far away, a third more memory (square textures)");

    ImGui::TableNextColumn();
    long now = i < mv.tex_bytes.size() ? mv.tex_bytes[i] : -1;
    long was = i < mv.built_tex_bytes.size() ? mv.built_tex_bytes[i] : -1;
    ImGui::TextUnformatted(KB(now, b, sizeof(b)));
    if (was >= 0 && now >= 0 && was != now)
        ImGui::TextColored(ImColor(now < was ? IM_COL32(120, 200, 120, 255) : ui::WARN), "%s%.1f K",
                           now > was ? "+" : "", (now - was) / 1024.0);
    ImGui::PopID();
    return changed;
}

static void controls() {
    char b[64], b2[64];
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##model", mv.src.empty() ? "Pick a model" : mv.src.c_str())) {
        for (const std::string& s : ed.sources)
            if (ImGui::Selectable(s.c_str(), s == mv.src)) model_open(s);
        ImGui::EndCombo();
    }
    if (mv.src.empty()) {
        ImGui::TextDisabled("A .glb under assets/: pick one above,\nor double-click one in the Project panel.");
        return;
    }

    bool changed = false;
    if (ui::component(ICON_MODEL, "Mesh")) {
        if (mv.info_ok) {
            ImGui::Text("%d meshes%s", mv.meshes, mv.skinned ? ", skinned" : "");
            ImGui::Text("Source       %d triangles, %d vertices", mv.tris, mv.verts);
        }
        ImGui::Text("Converted    %d triangles  %s", mv.btris, KB(mv.built_dms_bytes, b, sizeof(b)));
        ImGui::Text("Preview      %d triangles  %s", mv.ptris, KB(mv.dms_bytes, b2, sizeof(b2)));
        ui::tooltip("Converted is the project's own (pc/); Preview is these settings.\nK is the .dms file: about what it takes of RAM");
        float keep = mv.set.decimate * 100.0f;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##decimate", &keep, 2.0f, 100.0f, "keep %.0f%% of triangles")) {
            mv.set.decimate = roundf(keep) / 100.0f;
            changed = true;
        }
        ui::tooltip("meshoptimizer's simplifier, before strippy makes strips. Tiny meshes are left alone.");
    }

    if (ui::component(ICON_IMAGE, "Textures")) {
        long total = 0, total_was = 0;
        for (long v : mv.tex_bytes) if (v > 0) total += v;
        for (long v : mv.built_tex_bytes) if (v > 0) total_was += v;
        ImGui::Text("VRAM  %s  (converted: %s)", KB(total, b, sizeof(b)), KB(total_was, b2, sizeof(b2)));
        if (mv.images.empty()) ImGui::TextDisabled("No textures");
        else if (ImGui::BeginTable("tex", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
            float em = ImGui::GetFontSize();
            ImGui::TableSetupColumn("##thumb", ImGuiTableColumnFlags_WidthFixed, em * 3.0f);
            ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##format", ImGuiTableColumnFlags_WidthFixed, em * 5.2f);
            ImGui::TableSetupColumn("##vq", ImGuiTableColumnFlags_WidthFixed, em * 5.0f);
            ImGui::TableSetupColumn("##kb", ImGuiTableColumnFlags_WidthFixed, em * 4.2f);
            for (size_t i = 0; i < mv.images.size(); i++) changed |= texture_row(i);
            ImGui::EndTable();
        }
    }

    if (changed) { mv.want = true; mv.changed_at = ImGui::GetTime(); }

    ImGui::Separator();
    bool dirty = !(mv.set == mv.saved);
    ImGui::BeginDisabled(!dirty || ed_job());
    if (ImGui::Button(ICON_SAVE "  Apply")) apply();
    ImGui::EndDisabled();
    ui::tooltip("Write the settings next to the .glb and convert the project's copy");
    ImGui::SameLine();
    ImGui::BeginDisabled(!dirty);
    if (ImGui::Button("Revert")) { mv.set = mv.saved; mv.want = true; mv.changed_at = 0; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Defaults")) { size_t n = mv.set.tex.size(); mv.set = Settings(); mv.set.tex.resize(n); mv.want = true; mv.changed_at = 0; }
    ui::tooltip("strippy's own choices: every triangle, textures as -f auto with VQ");
    if (mv.state == 1) { ImGui::SameLine(); ImGui::TextDisabled("converting..."); }
}

void model_window(void* sdl_renderer) {
    renderer = (SDL_Renderer*)sdl_renderer;
    preview_poll();
    /* Converted a moment after the last change, not on every step of a drag */
    if (mv.want && mv.state != 1 && !mv.src.empty() && ImGui::GetTime() - mv.changed_at > 0.35 &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        convert();

    if (ImGuiWindow* scene = ImGui::FindWindowByName(WIN_SCENE))
        if (scene->DockId) ImGui::SetNextWindowDockID(scene->DockId, ImGuiCond_FirstUseEver);
    bool open = ImGui::Begin(WIN_MODEL);
    if (!open) { ImGui::End(); return; }

    float side = ImGui::GetFontSize() * 30.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float view_w = std::max(64.0f, avail.x - side - ImGui::GetStyle().ItemSpacing.x);

    ImGui::BeginChild("view", ImVec2(view_w, 0));
    DMSModel* m = mv.show_built ? mv.built : mv.model;
    if (!mv.src.empty()) {
        if (mv.spin) mv.yaw += ImGui::GetIO().DeltaTime * 0.6f;
        render(m);
        SDL_Renderer* r = renderer;
        if (!mv.frame && r) mv.frame = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, 640, 480);
        if (mv.frame) SDL_UpdateTexture(mv.frame, nullptr, mv.rgb, 640 * 3);
    }
    ImVec2 va = ImGui::GetContentRegionAvail(), top = ImGui::GetCursorScreenPos();
    float w = va.x, h = va.x * 0.75f;
    if (h > va.y) { h = va.y; w = h / 0.75f; }
    if (mv.frame && !mv.src.empty() && w > 32) {
        ImVec2 at(top.x + (va.x - w) * 0.5f, top.y + (va.y - h) * 0.5f);
        ImGui::SetCursorScreenPos(at);
        ImGui::Image((ImTextureID)(intptr_t)mv.frame, ImVec2(w, h));
        if (ImGui::IsItemHovered()) {
            ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0)) {
                mv.yaw += io.MouseDelta.x * 0.01f;
                mv.pitch = fmaxf(-1.5f, fminf(1.5f, mv.pitch + io.MouseDelta.y * 0.01f));
            }
            if (io.MouseWheel != 0) mv.dist *= powf(0.88f, io.MouseWheel);
        }
        ImGui::SetCursorScreenPos(ImVec2(at.x + 8, at.y + 8));
        if (ImGui::RadioButton("Preview", !mv.show_built)) mv.show_built = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Converted", mv.show_built)) mv.show_built = true;
        ui::tooltip("Converted: the project's own copy, as pc/ has it now");
        ImGui::SameLine();
        ImGui::Checkbox("Spin", &mv.spin);
        ImGui::SameLine();
        if (ImGui::SmallButton("Frame")) mv.dist = 0;
        ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + 10, at.y + h - ImGui::GetFontSize() - 8),
                                            IM_COL32(200, 204, 212, 200), "drag to turn, wheel to zoom");
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("controls", ImVec2(0, 0));
    controls();
    ImGui::EndChild();
    ImGui::End();
}

bool model_busy() { return mv.state == 1 || mv.want; }

void model_shutdown() {
    if (mv.worker.joinable()) mv.worker.join();
    preview_free();
    if (mv.built) dc_model_free(mv.built);
    mv.built = nullptr;
    if (mv.frame) SDL_DestroyTexture(mv.frame);
}
