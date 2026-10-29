/* The Garry's Mod tab: the models and maps of Garry's Mod (its addons and
 * workshop too) and of the other Source games installed beside it (Half-Life 2
 * and its episodes, Counter-Strike: Source, Team Fortress 2, Portal), as the games
 * have them, untouched, and the
 * choices for bringing one into the project: its skin, its body parts, its
 * animations, how big its textures may be. Nothing is converted until asked:
 * Bring in writes a .glb under assets/gmod/, which the Model tab then sets
 * up for the Dreamcast. tools/gmod/gmod2glb does all the reading. */
#include "editor.h"
#include "ui.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <sys/stat.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <math.h>
#include <set>
#include <string.h>
#include <thread>
#include <unistd.h>

static std::string tool() { return ed.root + "tools/gmod/gmod2glb"; }

static std::string q(const std::string& s) {
    std::string o = "'";
    for (char c : s) o += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return o + "'";
}

/* cmd's output, and what it said on stderr (a file each: the threads run at once) */
static bool run(const std::string& cmd, std::string* out, std::string* err = nullptr) {
    static std::atomic<int> calls{ 0 };
    std::string dir = ed.root + "build/editor/gmod/";
    std::string ef = dir + "err" + std::to_string(++calls) + ".txt";
    mkdir((ed.root + "build/editor").c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    std::string e = err ? " 2>" + q(ef) : " 2>/dev/null";
    /* At most 3 GB each: a converter gone wrong fails, not the machine */
    FILE* p = popen(("ulimit -v 3000000; " + cmd + e).c_str(), "r");
    if (!p) return false;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) if (out) out->append(buf, n);
    bool ok = pclose(p) == 0;
    if (err) {
        FILE* f = fopen(ef.c_str(), "r");
        if (f) { while ((n = fread(buf, 1, sizeof(buf), f)) > 0) err->append(buf, n); fclose(f); }
        unlink(ef.c_str());
    }
    return ok;
}

static std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> v;
    size_t at = 0;
    for (;;) {
        size_t t = line.find('\t', at);
        v.push_back(line.substr(at, t == std::string::npos ? std::string::npos : t - at));
        if (t == std::string::npos) return v;
        at = t + 1;
    }
}

static std::string base_name(const std::string& p) {
    std::string b = p.substr(p.find_last_of('/') + 1);
    bool ext = b.size() > 4 && (!b.compare(b.size() - 4, 4, ".mdl") || !b.compare(b.size() - 4, 4, ".bsp"));
    return ext ? b.substr(0, b.size() - 4) : b;
}

/* ================================================================
 * What gmod2glb says
 * ================================================================ */

/* This file's own types, out of the other files' way (panels.cpp has an
 * Entry: two of one name share their inline code, whichever is the size) */
namespace {

/* Which game a file is from, by where it was found */
enum Game { ALL, GMOD, HL2, CSS, TF2, PORTAL, L4D, ADDONS, GAMES };
static const char* const GAME_NAMES[GAMES] = { "Every game", "Garry's Mod", "Half-Life 2", "Counter-Strike: Source",
                                               "Team Fortress 2", "Portal", "Left 4 Dead", "Addons" };
static Game game_of(const std::string& from) {
    auto starts = [&](const char* p) { return !from.compare(0, strlen(p), p); };
    if (starts("workshop/") || starts("addons/")) return ADDONS;
    if (starts("hl2") || starts("content_hl2")) return HL2;
    if (starts("cstrike") || starts("content_cstrike")) return CSS;
    if (starts("tf/") || from == "tf") return TF2;
    if (starts("portal")) return PORTAL;
    if (starts("l4d")) return L4D;
    return GMOD;
}

struct Entry { std::string path, from; Game game; bool map; };
struct Node { std::map<std::string, Node> dirs; std::vector<int> files; };

enum { ALPHATEST = 1, TRANSLUCENT = 2, TWO_SIDED = 4, ADDITIVE = 8 };

struct Mat {
    std::string name, vmt, texture, note;
    int w = 0, h = 0;                       /* in the game */
    int pw = 0, ph = 0;                     /* in the preview */
    uint8_t flags = 0;
    std::vector<uint8_t> rgba;              /* the preview's, until it is a texture */
    SDL_Texture* tex = nullptr;             /* the Textures list's */
    unsigned gl = 0;                        /* the view's */
};

/* A material's triangles: idx until they are on the card, then where */
struct PMesh { int mat; std::vector<uint32_t> idx; uint32_t first = 0, count = 0; bool placed = false; };

/* A map's prop: its meshes, drawn at each place (pos[3], rot[9] by columns) */
struct Part { uint32_t first, count; std::vector<float> places; };

/* One model (or map) as gmod2glb --info reads it */
struct Loaded {
    std::string path, from, error;
    bool map = false;
    int unsimplified = 0;                               /* a map's triangles before simplifying */
    std::vector<std::pair<std::string, int>> props;     /* a map's: each model, how many */
    int bones = 0, verts = 0, tris = 0, skins = 1;
    std::vector<std::pair<std::string, std::vector<std::string>>> body;
    int prop_tris = 0, props_missing = 0;               /* a map's props', as placed */
    std::vector<Mat> mats;                  /* the first `listed` named; then the props' */
    int listed = 0;
    std::vector<std::string> anims;
    std::set<std::string> anim_files;
    std::vector<uint8_t> vdata;             /* float pos[3], nrm[3], uv[2], u8 rgba: the file's */
    std::vector<PMesh> meshes;
    std::vector<Part> parts;
    unsigned vbo = 0, ibo = 0;
    float centre[3] = { 0, 0, 0 }, radius = 1;
};

}  // namespace

static bool read_preview(const std::string& file, Loaded& L) {
    FILE* f = fopen(file.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> d;
    uint8_t buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) d.insert(d.end(), buf, buf + n);
    fclose(f);
    size_t p = 0;
    auto need = [&](size_t k) { return p + k <= d.size(); };
    auto u32 = [&]() { uint32_t v = 0; if (need(4)) memcpy(&v, &d[p], 4); p += 4; return v; };
    if (!need(8) || memcmp(d.data(), "GMPV", 4) || d[4] != 3) return false;
    p = 8;
    L.listed = (int)L.mats.size();
    uint32_t nm = u32();
    for (uint32_t i = 0; i < nm && need(5); i++) {
        uint16_t w, h;
        memcpy(&w, &d[p], 2);
        memcpy(&h, &d[p + 2], 2);
        uint8_t flags = d[p + 4];
        p += 5;
        size_t bytes = (size_t)w * h * 4;
        if (!need(bytes)) return false;
        if (i >= L.mats.size()) L.mats.push_back(Mat());
        Mat& m = L.mats[i];
        m.flags = flags;
        m.pw = w;
        m.ph = h;
        m.rgba.assign(d.begin() + p, d.begin() + p + bytes);
        p += bytes;
    }
    uint32_t nv = u32();
    if (!need((size_t)nv * 36)) return false;
    L.vdata.assign(d.begin() + p, d.begin() + p + (size_t)nv * 36);
    p += (size_t)nv * 36;
    uint32_t nmesh = u32();
    for (uint32_t i = 0; i < nmesh && need(8); i++) {
        PMesh m;
        m.mat = (int)u32();
        if (m.mat < 0 || m.mat >= (int)L.mats.size()) m.mat = -1;
        uint32_t ni = u32();
        if (!need((size_t)ni * 4)) return false;
        m.idx.resize(ni);
        memcpy(m.idx.data(), &d[p], (size_t)ni * 4);
        p += (size_t)ni * 4;
        for (uint32_t& x : m.idx) if (x >= nv) x = 0;
        L.meshes.push_back(std::move(m));
    }
    uint32_t nparts = u32();
    for (uint32_t i = 0; i < nparts && need(12); i++) {
        Part pt;
        pt.first = u32();
        pt.count = u32();
        uint32_t n = u32();
        if (!need((size_t)n * 48) || pt.first + pt.count > L.meshes.size()) return false;
        pt.places.resize((size_t)n * 12);
        memcpy(pt.places.data(), &d[p], (size_t)n * 48);
        p += (size_t)n * 48;
        for (uint32_t k = 0; k < pt.count; k++) L.meshes[pt.first + k].placed = true;
        L.parts.push_back(std::move(pt));
    }
    /* Its size: the world's (or the model's), the props aside */
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    bool any = false;
    for (const PMesh& m : L.meshes) {
        if (m.placed) continue;
        for (uint32_t x : m.idx) {
            float v[3];
            memcpy(v, &L.vdata[(size_t)x * 36], 12);
            for (int k = 0; k < 3; k++) { mn[k] = std::min(mn[k], v[k]); mx[k] = std::max(mx[k], v[k]); }
            any = true;
        }
    }
    if (any) {
        float r = 0;
        for (int k = 0; k < 3; k++) {
            L.centre[k] = (mn[k] + mx[k]) * 0.5f;
            r += (mx[k] - mn[k]) * (mx[k] - mn[k]);
        }
        L.radius = std::max(0.01f, sqrtf(r) * 0.5f);
    }
    return true;
}

static void load(const std::string& path, int skin, const std::vector<int>& body, float simplify, Loaded& L) {
    std::string dir = ed.root + "build/editor/gmod/";
    L.map = path.size() > 4 && !path.compare(path.size() - 4, 4, ".bsp");
    /* A map has a couple of hundred textures */
    std::string cmd = q(tool()) + " --info " + q(path) + " --preview " + q(dir + "preview.bin") + (L.map ? " --tex 128" : " --tex 512");
    if (skin) cmd += " --skin " + std::to_string(skin);
    if (L.map && simplify > 0) cmd += " --simplify " + std::to_string(simplify);
    if (!body.empty()) {
        std::string b;
        for (size_t i = 0; i < body.size(); i++) b += (i ? "," : "") + std::to_string(body[i]);
        cmd += " --body " + b;
    }
    std::string out, err;
    bool ok = run(cmd, &out, &err);
    L.path = path;
    size_t at = 0;
    while (at < out.size()) {
        size_t nl = out.find('\n', at);
        if (nl == std::string::npos) nl = out.size();
        std::vector<std::string> f = fields(out.substr(at, nl - at));
        at = nl + 1;
        const std::string& k = f[0];
        if ((k == "model" || k == "map") && f.size() > 2) L.from = f[2];
        else if (k == "simplified" && f.size() > 1) L.unsimplified = atoi(f[1].c_str());
        else if (k == "prop" && f.size() > 2) L.props.push_back({ f[1], atoi(f[2].c_str()) });
        else if (k == "proptris" && f.size() > 2) {
            L.prop_tris = atoi(f[1].c_str());
            L.props_missing = atoi(f[2].c_str());
        }
        else if (k == "counts" && f.size() > 4) {
            L.bones = atoi(f[1].c_str());
            L.verts = atoi(f[2].c_str());
            L.tris = atoi(f[3].c_str());
            L.skins = std::max(1, atoi(f[4].c_str()));
        } else if (k == "body" && f.size() > 1) {
            L.body.push_back({ f[1], std::vector<std::string>(f.begin() + 2, f.end()) });
        } else if (k == "mat" && f.size() > 6) {
            Mat m;
            m.name = f[1];
            m.vmt = f[2];
            m.texture = f[3];
            m.w = atoi(f[4].c_str());
            m.h = atoi(f[5].c_str());
            m.note = f[6];
            L.mats.push_back(m);
        } else if (k == "anim" && f.size() > 2) {
            L.anims.push_back(f[1]);
            L.anim_files.insert(f[2]);
        }
    }
    if (!ok) {
        L.error = err.empty() ? "gmod2glb failed" : err;
        while (!L.error.empty() && L.error.back() == '\n') L.error.pop_back();
        return;
    }
    if (!read_preview(dir + "preview.bin", L)) L.error = "cannot read the preview";
}

/* ================================================================
 * State
 * ================================================================ */

static struct {
    /* The list of models */
    std::atomic<int> list_state{ 0 };       /* 0 not read, 1 reading, 2 read, 3 failed */
    std::thread list_worker;
    std::vector<Entry> entries;
    std::string list_error;
    int show = ALL;                         /* Game: which game's files are listed */
    int kind = 0, kind_was = -1;            /* 0 models, 1 maps */
    int simplify = 2;                       /* a map's: SIMPLIFY[] */
    char filter[128] = "";
    std::string filter_was = "\x01";
    int show_was = -1;
    std::vector<int> matches;
    Node tree;

    /* The model looked at */
    std::string selected;
    int skin = 0;
    std::vector<int> body;
    std::atomic<int> state{ 0 };            /* 0 idle, 1 reading, 2 read */
    bool want = false;
    std::thread worker;
    Loaded next, cur;
    bool have = false;

    /* Its preview */
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* target = nullptr;
    int tw = 0, th = 0;
    float yaw = 0.5f, pitch = 0.15f, dist = 0;
    bool dirty = true;

    /* Bringing it in */
    std::set<std::string> anims;
    char anim_filter[64] = "";
    char name[64] = "";
    int tex = 2;
    std::atomic<int> bring_state{ 0 };      /* 0 idle, 1 working, 2 done, 3 failed */
    std::thread bring_worker;
    std::string bring_log, bring_rel, bring_scene;
    bool bring_map = false, props = true;
} g;

static const int TEX_SIZES[] = { 64, 128, 256, 512, 1024 };
/* How far a map's shape (metres) and light (0..1) may move when simplified */
static const float SIMPLIFY[] = { 0, 0.02f, 0.05f, 0.1f, 0.2f, 0.4f };
static const char* const SIMPLIFY_NAMES[] = { "Off", "Light", "Medium", "Strong", "Stronger", "Most" };

static void list_start() {
    g.list_state = 1;
    g.list_worker = std::thread([] {
        std::string out, err;
        struct stat st;
        if (stat(tool().c_str(), &st)) {
            g.list_error = "tools/gmod/gmod2glb is not built:  make -C tools/gmod";
            g.list_state = 3;
            return;
        }
        if (!run(q(tool()) + " --list", &out, &err)) {
            g.list_error = err.empty() ? "gmod2glb --list failed" : err;
            g.list_state = 3;
            return;
        }
        size_t at = 0;
        while (at < out.size()) {
            size_t nl = out.find('\n', at);
            if (nl == std::string::npos) nl = out.size();
            std::vector<std::string> f = fields(out.substr(at, nl - at));
            at = nl + 1;
            if (f[0].empty()) continue;
            std::string from = f.size() > 1 ? f[1] : "";
            bool map = f[0].size() > 4 && !f[0].compare(f[0].size() - 4, 4, ".bsp");
            g.entries.push_back({ f[0], from, game_of(from), map });
        }
        g.list_state = 2;
    });
}

static void gl_upload(Loaded& L);
static void gl_free(Loaded& L);

static void textures_free(Loaded& L) {
    for (Mat& m : L.mats)
        if (m.tex) { SDL_DestroyTexture(m.tex); m.tex = nullptr; }
}

static void request() {
    if (g.selected.empty()) return;
    if (g.state == 1) { g.want = true; return; }
    if (g.worker.joinable()) g.worker.join();
    g.want = false;
    g.state = 1;
    std::string path = g.selected;
    int skin = g.skin;
    float simplify = SIMPLIFY[g.simplify];
    std::vector<int> body = g.body;
    g.worker = std::thread([path, skin, body, simplify] {
        g.next = Loaded();
        load(path, skin, body, simplify, g.next);
        g.state = 2;
    });
}

static void select(const std::string& path) {
    if (path == g.selected) return;
    g.selected = path;
    g.skin = 0;
    g.body.clear();
    g.anims.clear();
    snprintf(g.name, sizeof(g.name), "%s", base_name(path).c_str());
    g.dist = 0;
    bool map = path.size() > 4 && !path.compare(path.size() - 4, 4, ".bsp");
    /* A map from above, its textures small (it has a couple of hundred) */
    g.pitch = map ? 0.7f : 0.15f;
    g.tex = map ? 1 : 2;
    request();
}

/* A finished read: its textures made, shown */
static void poll() {
    if (g.state != 2) return;
    g.worker.join();
    g.state = 0;
    textures_free(g.cur);
    gl_free(g.cur);
    g.cur = std::move(g.next);
    g.next = Loaded();
    g.have = true;
    g.dirty = true;
    if (!g.cur.error.empty()) ed_log("Garry's Mod: %s: %s", g.cur.path.c_str(), g.cur.error.c_str());
    for (Mat& m : g.cur.mats)
        /* The alpha of an opaque material's texture is a mask for something
         * else (shine, mostly): it is drawn solid */
        if (!(m.flags & (ALPHATEST | TRANSLUCENT | ADDITIVE)))
            for (size_t i = 3; i < m.rgba.size(); i += 4) m.rgba[i] = 255;
    if (g.renderer)
        for (int i = 0; i < g.cur.listed; i++) {
            Mat& m = g.cur.mats[i];
            if (m.rgba.empty() || !m.pw) continue;
            m.tex = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, m.pw, m.ph);
            if (!m.tex) continue;
            SDL_UpdateTexture(m.tex, nullptr, m.rgba.data(), m.pw * 4);
            SDL_SetTextureScaleMode(m.tex, SDL_SCALEMODE_LINEAR);
            SDL_SetTextureBlendMode(m.tex, m.flags & ADDITIVE ? SDL_BLENDMODE_ADD
                                           : m.flags & (ALPHATEST | TRANSLUCENT) ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
        }
    gl_upload(g.cur);
    for (Mat& m : g.cur.mats) std::vector<uint8_t>().swap(m.rgba);
    if (g.want) request();
}

/* ================================================================
 * The preview: drawn by OpenGL in a context of its own (whatever SDL's
 * renderer is), off the screen, then copied to a texture the tab shows. A
 * model lit from beside the camera; a map by its own light, its props lit
 * as models are
 * ================================================================ */

#define GL_1(X) X(Viewport) X(ClearColor) X(Clear) X(Enable) X(Disable) X(DepthMask) X(BlendFunc) X(AlphaFunc) \
    X(CullFace) X(MatrixMode) X(LoadMatrixf) X(LoadIdentity) X(MultMatrixf) X(PushMatrix) X(PopMatrix) X(GenTextures) \
    X(DeleteTextures) X(BindTexture) X(TexImage2D) X(TexParameteri) X(TexEnvi) X(EnableClientState) X(VertexPointer) \
    X(NormalPointer) X(ColorPointer) X(TexCoordPointer) X(DrawElements) X(ReadPixels) X(Lightfv) X(LightModelfv) \
    X(ColorMaterial) X(PixelStorei) X(GetError)
#define GL_X(X) X(GENBUFFERS, GenBuffers) X(DELETEBUFFERS, DeleteBuffers) X(BINDBUFFER, BindBuffer) \
    X(BUFFERDATA, BufferData) X(GENFRAMEBUFFERS, GenFramebuffers) X(DELETEFRAMEBUFFERS, DeleteFramebuffers) \
    X(BINDFRAMEBUFFER, BindFramebuffer) X(FRAMEBUFFERRENDERBUFFER, FramebufferRenderbuffer) \
    X(CHECKFRAMEBUFFERSTATUS, CheckFramebufferStatus) X(GENRENDERBUFFERS, GenRenderbuffers) \
    X(DELETERENDERBUFFERS, DeleteRenderbuffers) X(BINDRENDERBUFFER, BindRenderbuffer) \
    X(RENDERBUFFERSTORAGEMULTISAMPLE, RenderbufferStorageMultisample) X(BLITFRAMEBUFFER, BlitFramebuffer) \
    X(GENERATEMIPMAP, GenerateMipmap)

static struct {
#define F1(n) decltype(&::gl##n) n = nullptr;
#define FX(N, n) PFNGL##N##PROC n = nullptr;
    GL_1(F1) GL_X(FX)
#undef F1
#undef FX
    int state = 0;                  /* 0 not tried, 1 working, 2 none to be had */
    SDL_Window* window = nullptr;
    SDL_GLContext ctx = nullptr;
    SDL_Window* old_window = nullptr;
    SDL_GLContext old_ctx = nullptr;
    unsigned fbo = 0, rb[2] = { 0, 0 }, resolve = 0, resolve_rb = 0, grey = 0;
    int w = 0, h = 0;
    std::vector<uint8_t> pixels;
} gl;

/* Our context current, for a while; false if there is none */
static bool gl_begin() {
    if (gl.state == 2) return false;
    gl.old_window = SDL_GL_GetCurrentWindow();
    gl.old_ctx = SDL_GL_GetCurrentContext();
    if (gl.state == 0) {
        gl.state = 2;
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);
        gl.window = SDL_CreateWindow("preview", 16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        gl.ctx = gl.window ? SDL_GL_CreateContext(gl.window) : nullptr;
        if (!gl.ctx) {
            ed_log("Source preview: no OpenGL (%s)", SDL_GetError());
            if (gl.window) SDL_DestroyWindow(gl.window);
            gl.window = nullptr;
            SDL_GL_MakeCurrent(gl.old_window, gl.old_ctx);
            return false;
        }
        bool all = true;
#define F1(n) all &= (gl.n = (decltype(gl.n))SDL_GL_GetProcAddress("gl" #n)) != nullptr;
#define FX(N, n) all &= (gl.n = (decltype(gl.n))SDL_GL_GetProcAddress("gl" #n)) != nullptr;
        GL_1(F1) GL_X(FX)
#undef F1
#undef FX
        if (!all) {
            ed_log("Source preview: this OpenGL is missing what the preview needs");
            SDL_GL_MakeCurrent(gl.old_window, gl.old_ctx);
            return false;
        }
        gl.state = 1;
        /* What a mesh without a texture is drawn with */
        uint8_t grey[4] = { 180, 180, 180, 255 };
        gl.GenTextures(1, &gl.grey);
        gl.BindTexture(GL_TEXTURE_2D, gl.grey);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, grey);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        return true;
    }
    return SDL_GL_MakeCurrent(gl.window, gl.ctx);
}

/* Whoever had the current context has it back (SDL's renderer, if OpenGL) */
static void gl_end() { SDL_GL_MakeCurrent(gl.old_window, gl.old_ctx); }

/* A read's textures and triangles onto the card */
static void gl_upload(Loaded& L) {
    if (!gl_begin()) return;
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (Mat& m : L.mats) {
        if (m.rgba.empty() || !m.pw) continue;
        gl.GenTextures(1, &m.gl);
        gl.BindTexture(GL_TEXTURE_2D, m.gl);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m.pw, m.ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, m.rgba.data());
        gl.GenerateMipmap(GL_TEXTURE_2D);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
    std::vector<uint32_t> all;
    for (PMesh& m : L.meshes) {
        m.first = (uint32_t)all.size();
        m.count = (uint32_t)m.idx.size();
        all.insert(all.end(), m.idx.begin(), m.idx.end());
        std::vector<uint32_t>().swap(m.idx);
    }
    gl.GenBuffers(1, &L.vbo);
    gl.BindBuffer(GL_ARRAY_BUFFER, L.vbo);
    gl.BufferData(GL_ARRAY_BUFFER, L.vdata.size(), L.vdata.data(), GL_STATIC_DRAW);
    gl.GenBuffers(1, &L.ibo);
    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, L.ibo);
    gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, all.size() * 4, all.data(), GL_STATIC_DRAW);
    std::vector<uint8_t>().swap(L.vdata);
    gl_end();
}

static void gl_free(Loaded& L) {
    if (gl.state != 1 || !gl_begin()) return;
    for (Mat& m : L.mats)
        if (m.gl) { gl.DeleteTextures(1, &m.gl); m.gl = 0; }
    if (L.vbo) gl.DeleteBuffers(1, &L.vbo);
    if (L.ibo) gl.DeleteBuffers(1, &L.ibo);
    L.vbo = L.ibo = 0;
    gl_end();
}

/* Somewhere w x h to draw (4 samples a pixel), and one to read it from */
static bool gl_target(int w, int h) {
    if (gl.fbo && gl.w == w && gl.h == h) return true;
    if (gl.fbo) {
        gl.DeleteFramebuffers(1, &gl.fbo);
        gl.DeleteFramebuffers(1, &gl.resolve);
        gl.DeleteRenderbuffers(2, gl.rb);
        gl.DeleteRenderbuffers(1, &gl.resolve_rb);
    }
    gl.GenFramebuffers(1, &gl.fbo);
    gl.GenRenderbuffers(2, gl.rb);
    gl.BindFramebuffer(GL_FRAMEBUFFER, gl.fbo);
    gl.BindRenderbuffer(GL_RENDERBUFFER, gl.rb[0]);
    gl.RenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA8, w, h);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, gl.rb[0]);
    gl.BindRenderbuffer(GL_RENDERBUFFER, gl.rb[1]);
    gl.RenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_DEPTH_COMPONENT24, w, h);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, gl.rb[1]);
    bool ok = gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    gl.GenFramebuffers(1, &gl.resolve);
    gl.GenRenderbuffers(1, &gl.resolve_rb);
    gl.BindFramebuffer(GL_FRAMEBUFFER, gl.resolve);
    gl.BindRenderbuffer(GL_RENDERBUFFER, gl.resolve_rb);
    gl.RenderbufferStorageMultisample(GL_RENDERBUFFER, 0, GL_RGBA8, w, h);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, gl.resolve_rb);
    ok &= gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    gl.w = w;
    gl.h = h;
    if (!ok) ed_log("Source preview: OpenGL cannot draw off the screen here");
    return ok;
}

static void render_preview(int w, int h) {
    SDL_Renderer* r = g.renderer;
    if (!r || w < 8 || h < 8) return;
    if (!g.target || g.tw != w || g.th != h) {
        if (g.target) SDL_DestroyTexture(g.target);
        g.target = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        g.tw = w;
        g.th = h;
        g.dirty = true;
    }
    if (!g.target || !g.dirty) return;
    g.dirty = false;
    const Loaded& L = g.cur;
    if (!gl_begin()) return;
    if (!gl_target(w, h)) { gl_end(); return; }

    gl.BindFramebuffer(GL_FRAMEBUFFER, gl.fbo);
    gl.Viewport(0, 0, w, h);
    gl.ClearColor(0x26 / 255.0f, 0x29 / 255.0f, 0x2f / 255.0f, 1);
    gl.DepthMask(GL_TRUE);
    gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (g.dist <= 0) g.dist = L.radius * (L.map ? 1.5f : 2.6f);
    float cp = cosf(g.pitch), sp = sinf(g.pitch), cy = cosf(g.yaw), sy = sinf(g.yaw);
    float eye[3] = { L.centre[0] + g.dist * cp * sy, L.centre[1] + g.dist * sp, L.centre[2] + g.dist * cp * cy };
    float fw[3] = { -cp * sy, -sp, -cp * cy };
    float rt[3] = { -fw[2], 0, fw[0] };         /* forward x up */
    float rl = sqrtf(rt[0] * rt[0] + rt[2] * rt[2]);
    rt[0] /= rl; rt[2] /= rl;
    float up[3] = { rt[1] * fw[2] - rt[2] * fw[1], rt[2] * fw[0] - rt[0] * fw[2], rt[0] * fw[1] - rt[1] * fw[0] };

    /* 40 degrees top to bottom; near and far from how far out and how big */
    float zn = std::max(0.01f, std::min(g.dist, L.radius) * 0.002f), zf = g.dist + L.radius * 4;
    float f = 1.0f / tanf(20.0f * 3.14159265f / 180.0f), aspect = (float)w / h;
    float proj[16] = { f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (zf + zn) / (zn - zf), -1, 0, 0, 2 * zf * zn / (zn - zf), 0 };
    float view[16] = { rt[0], up[0], -fw[0], 0, rt[1], up[1], -fw[1], 0, rt[2], up[2], -fw[2], 0, 0, 0, 0, 1 };
    for (int k = 0; k < 3; k++) view[12 + k] = -(view[k] * eye[0] + view[4 + k] * eye[1] + view[8 + k] * eye[2]);
    gl.MatrixMode(GL_PROJECTION);
    gl.LoadMatrixf(proj);
    gl.MatrixMode(GL_MODELVIEW);
    gl.LoadIdentity();

    /* The light, from up and to the left of the camera, set where the camera is */
    float lt[4] = { -0.4f, 0.6f, 1.0f, 0 }, amb[4] = { 0.42f, 0.42f, 0.42f, 1 }, dif[4] = { 0.58f, 0.58f, 0.58f, 1 };
    float none[4] = { 0, 0, 0, 1 };
    gl.Lightfv(GL_LIGHT0, GL_POSITION, lt);
    gl.Lightfv(GL_LIGHT0, GL_DIFFUSE, dif);
    gl.Lightfv(GL_LIGHT0, GL_AMBIENT, none);
    gl.LightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
    gl.Enable(GL_LIGHT0);
    gl.ColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    gl.Enable(GL_COLOR_MATERIAL);
    gl.Enable(GL_NORMALIZE);
    gl.LoadMatrixf(view);

    gl.Enable(GL_DEPTH_TEST);
    gl.Enable(GL_TEXTURE_2D);
    gl.TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    gl.CullFace(GL_BACK);
    gl.AlphaFunc(GL_GREATER, 0.5f);
    gl.BindBuffer(GL_ARRAY_BUFFER, L.vbo);
    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, L.ibo);
    gl.EnableClientState(GL_VERTEX_ARRAY);
    gl.EnableClientState(GL_NORMAL_ARRAY);
    gl.EnableClientState(GL_TEXTURE_COORD_ARRAY);
    gl.EnableClientState(GL_COLOR_ARRAY);
    gl.VertexPointer(3, GL_FLOAT, 36, (void*)0);
    gl.NormalPointer(GL_FLOAT, 36, (void*)12);
    gl.TexCoordPointer(2, GL_FLOAT, 36, (void*)24);
    gl.ColorPointer(4, GL_UNSIGNED_BYTE, 36, (void*)32);

    /* One mesh as its material has it; the see-through ones on the second pass */
    auto mesh = [&](const PMesh& m, bool see_pass) {
        const Mat* mat = m.mat >= 0 ? &L.mats[m.mat] : nullptr;
        uint8_t fl = mat ? mat->flags : 0;
        bool see = fl & (TRANSLUCENT | ADDITIVE);
        if (see != see_pass || !m.count) return;
        gl.BindTexture(GL_TEXTURE_2D, mat && mat->gl ? mat->gl : gl.grey);
        if (fl & TWO_SIDED) gl.Disable(GL_CULL_FACE); else gl.Enable(GL_CULL_FACE);
        if (fl & ALPHATEST) gl.Enable(GL_ALPHA_TEST); else gl.Disable(GL_ALPHA_TEST);
        if (see) {
            gl.Enable(GL_BLEND);
            gl.BlendFunc(GL_SRC_ALPHA, fl & ADDITIVE ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
            gl.DepthMask(GL_FALSE);
        } else {
            gl.Disable(GL_BLEND);
            gl.DepthMask(GL_TRUE);
        }
        gl.DrawElements(GL_TRIANGLES, m.count, GL_UNSIGNED_INT, (void*)(uintptr_t)(m.first * 4));
    };
    for (int pass = 0; pass < 2; pass++) {
        /* The world (a map's lit by itself) or the model */
        if (L.map) gl.Disable(GL_LIGHTING); else gl.Enable(GL_LIGHTING);
        for (const PMesh& m : L.meshes)
            if (!m.placed) mesh(m, pass == 1);
        /* The props, each where it stands */
        gl.Enable(GL_LIGHTING);
        for (const Part& pt : L.parts)
            for (size_t i = 0; i + 12 <= pt.places.size(); i += 12) {
                const float* at = &pt.places[i];
                float m4[16] = { at[3], at[4], at[5], 0, at[6], at[7], at[8], 0, at[9], at[10], at[11], 0,
                                 at[0], at[1], at[2], 1 };
                gl.PushMatrix();
                gl.MultMatrixf(m4);
                for (uint32_t k = 0; k < pt.count; k++) mesh(L.meshes[pt.first + k], pass == 1);
                gl.PopMatrix();
            }
    }
    gl.DepthMask(GL_TRUE);

    /* Down to a pixel each, and read back (bottom row first: shown flipped) */
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, gl.fbo);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, gl.resolve);
    gl.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    gl.BindFramebuffer(GL_FRAMEBUFFER, gl.resolve);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl.pixels.resize((size_t)w * h * 4);
    gl.ReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, gl.pixels.data());
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.BindBuffer(GL_ARRAY_BUFFER, 0);
    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    gl_end();
    SDL_UpdateTexture(g.target, nullptr, gl.pixels.data(), w * 4);
}

/* ================================================================
 * Bringing it in: gmod2glb into assets/gmod/<name>/, then the Model tab
 * ================================================================ */

static std::string bring_rel() {
    return std::string(g.cur.map ? "gmod/maps/" : "gmod/") + g.name + "/" + g.name + ".glb";
}

static void bring_in() {
    if (g.bring_state == 1 || !g.have || g.name[0] == 0) return;
    if (g.bring_worker.joinable()) g.bring_worker.join();
    std::string rel = bring_rel(), glb = ed.project + "assets/" + rel;
    g.bring_map = g.cur.map;
    if (g.cur.map) {
        /* Its world, its props (each once, under assets/gmod/), a scene of them */
        std::string cmd = q(tool()) + " " + q(g.cur.path) + " --assets " + q(ed.project + "assets") + " --name " + q(g.name) +
                          " --tex " + std::to_string(TEX_SIZES[g.tex]);
        if (!g.props) cmd += " --no-props";
        if (SIMPLIFY[g.simplify] > 0) cmd += " --simplify " + std::to_string(SIMPLIFY[g.simplify]);
        /* The open scene's Player (the project's character) goes where the map starts one */
        if (DCSceneObject* p = ed.scene ? dc_scene_find(ed.scene, "Player") : nullptr)
            if (p->model[0]) cmd += std::string(" --player ") + q(p->model) + " --player-scale " + std::to_string(p->scale.x);
        g.bring_rel = rel;
        g.bring_scene = std::string("scenes/") + g.name + ".json";
        g.bring_state = 1;
        g.bring_worker = std::thread([cmd] {
            std::string out, err;
            bool ok = run(cmd, &out, &err);
            g.bring_log = out + err;
            g.bring_state = ok ? 2 : 3;
        });
        return;
    }
    std::string cmd = "mkdir -p " + q(glb.substr(0, glb.find_last_of('/'))) + " && " + q(tool()) + " " + q(g.cur.path) +
                      " -o " + q(glb) + " --tex " + std::to_string(TEX_SIZES[g.tex]);
    if (g.skin) cmd += " --skin " + std::to_string(g.skin);
    if (!g.body.empty()) {
        std::string b;
        for (size_t i = 0; i < g.body.size(); i++) b += (i ? "," : "") + std::to_string(g.body[i]);
        cmd += " --body " + b;
    }
    if (!g.anims.empty()) {
        std::string a;
        for (const std::string& n : g.anims) a += (a.empty() ? "" : ",") + n;
        cmd += " --anims " + q(a);
    }
    g.bring_rel = rel;
    g.bring_state = 1;
    g.bring_worker = std::thread([cmd] {
        std::string out, err;
        bool ok = run(cmd, &out, &err);
        g.bring_log = out + err;
        g.bring_state = ok ? 2 : 3;
    });
}

static void bring_poll() {
    int s = g.bring_state;
    if (s < 2) return;
    g.bring_worker.join();
    g.bring_state = 0;
    std::string log = g.bring_log;
    while (!log.empty() && log.back() == '\n') log.pop_back();
    if (s == 3) {
        ed_log("Garry's Mod: could not bring in %s", g.bring_rel.c_str());
        ed_log("%s", log.c_str());
        return;
    }
    if (g.bring_map) {
        ed_log("Garry's Mod: %s", log.c_str());
        ed_scan_assets();
        /* Made into .dms, then its scene opened */
        std::string scene = g.bring_scene;
        ed_run("assets", [scene](bool ok) {
            ed_scan_assets();
            if (ok) ed_switch_scene(scene);
        });
        return;
    }
    ed_log("Garry's Mod: brought in assets/%s", g.bring_rel.c_str());
    ed_scan_assets();
    model_open(g.bring_rel);
    ed_run("assets");
}

/* ================================================================
 * The tab
 * ================================================================ */

static void build_tree() {
    g.tree = Node();
    for (int i = 0; i < (int)g.entries.size(); i++) {
        const Entry& e = g.entries[i];
        if ((g.show != ALL && e.game != g.show) || e.map != (g.kind == 1)) continue;
        Node* n = &g.tree;
        size_t at = 0;
        for (;;) {
            size_t s = e.path.find('/', at);
            if (s == std::string::npos) break;
            n = &n->dirs[e.path.substr(at, s - at)];
            at = s + 1;
        }
        n->files.push_back(i);
    }
}

static void entry_row(int i, bool full) {
    const Entry& e = g.entries[i];
    std::string label = (full ? e.path : base_name(e.path)) + "##" + e.path;
    if (ImGui::Selectable(label.c_str(), e.path == g.selected)) select(e.path);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", e.path.c_str(), e.from.c_str());
}

static void tree_node(const Node& n, int depth = 0) {
    for (auto& kv : n.dirs) {
        if (depth == 0) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (ImGui::TreeNodeEx(kv.first.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
            tree_node(kv.second, depth + 1);
            ImGui::TreePop();
        }
    }
    for (int i : n.files) {
        ImGui::Indent(ImGui::GetStyle().IndentSpacing * 0.5f);
        entry_row(i, false);
        ImGui::Unindent(ImGui::GetStyle().IndentSpacing * 0.5f);
    }
}

static void browser() {
    if (g.list_state == 0) list_start();
    if (g.list_state == 1) { ImGui::TextDisabled("Reading Garry's Mod..."); return; }
    if (g.list_state == 3) {
        if (g.list_worker.joinable()) g.list_worker.join();
        ImGui::TextColored(ImColor(ui::WARN), "%s", g.list_error.c_str());
        if (ImGui::Button("Try again")) { g.list_state = 0; g.entries.clear(); }
        return;
    }
    if (g.list_worker.joinable()) g.list_worker.join();

    const char* kinds[] = { "Models", "Maps" };
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.5f);
    ImGui::Combo("##kind", &g.kind, kinds, 2);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 7.5f);
    ImGui::InputTextWithHint("##gf", ICON_SEARCH "  Search", g.filter, sizeof(g.filter));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##show", &g.show, GAME_NAMES, GAMES);
    if (g.show != g.show_was || g.kind != g.kind_was) {
        build_tree();
        g.filter_was = "\x01";
        g.show_was = g.show;
        g.kind_was = g.kind;
    }
    if (g.filter_was != g.filter) {
        g.filter_was = g.filter;
        g.matches.clear();
        std::string f = g.filter;
        for (char& c : f) c = (char)tolower((unsigned char)c);
        if (!f.empty())
            for (int i = 0; i < (int)g.entries.size(); i++) {
                const Entry& e = g.entries[i];
                if ((g.show != ALL && e.game != g.show) || e.map != (g.kind == 1)) continue;
                if (e.path.find(f) != std::string::npos) g.matches.push_back(i);
            }
    }
    ImGui::BeginChild("list", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (g.filter[0]) {
        ImGuiListClipper clip;
        clip.Begin((int)g.matches.size());
        while (clip.Step())
            for (int k = clip.DisplayStart; k < clip.DisplayEnd; k++) entry_row(g.matches[k], true);
        if (g.matches.empty()) ImGui::TextDisabled(g.kind ? "No map matches" : "No model matches");
    } else {
        tree_node(g.tree);
    }
    ImGui::EndChild();
}

static void preview_view() {
    ImVec2 avail = ImGui::GetContentRegionAvail(), top = ImGui::GetCursorScreenPos();
    if (!g.have) {
        ImGui::TextDisabled(g.state == 1 ? "Reading..." : "Pick a model to see it as the game has it");
        return;
    }
    int w = (int)avail.x, h = (int)avail.y;
    render_preview(w, h);
    if (!g.target) return;
    ImGui::Image((ImTextureID)(intptr_t)g.target, ImVec2((float)w, (float)h), ImVec2(0, 1), ImVec2(1, 0));
    if (ImGui::IsItemHovered()) {
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0)) {
            g.yaw -= io.MouseDelta.x * 0.01f;
            g.pitch = fmaxf(-1.5f, fminf(1.5f, g.pitch + io.MouseDelta.y * 0.01f));
            g.dirty = true;
        }
        if (io.MouseWheel != 0) { g.dist *= powf(0.88f, io.MouseWheel); g.dirty = true; }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    char b[96];
    if (g.cur.prop_tris)
        snprintf(b, sizeof(b), "As in the game: %d triangles, %d more in props%s", g.cur.tris, g.cur.prop_tris,
                 g.state == 1 ? "   reading..." : "");
    else
        snprintf(b, sizeof(b), "As in the game: %d triangles%s", g.cur.tris, g.state == 1 ? "   reading..." : "");
    dl->AddText(ImVec2(top.x + 8, top.y + 6), IM_COL32(200, 204, 212, 220), b);
    dl->AddText(ImVec2(top.x + 8, top.y + h - ImGui::GetFontSize() - 6), IM_COL32(200, 204, 212, 160), "drag to turn, wheel to zoom");
}

static const char* const PLAYER_BASICS[] = { "@idle_all_01", "a_WalkN", "a_RunN", "@cidle_all", "a_CwalkN", "@jump_passive" };

static void details() {
    if (!g.have) return;
    const Loaded& L = g.cur;
    ImGui::TextUnformatted(base_name(L.path).c_str());
    ImGui::TextDisabled("%s", L.path.c_str());
    ImGui::TextDisabled("from %s", L.from.c_str());
    if (!L.error.empty()) { ImGui::TextColored(ImColor(ui::ERROR), "%s", L.error.c_str()); }
    if (L.map) {
        ImGui::Text("%d triangles, %d vertices, %d materials", L.tris, L.verts, L.listed);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        if (ImGui::SliderInt("Simplify", &g.simplify, 0, 5, SIMPLIFY_NAMES[g.simplify])) request();
        ui::tooltip("Joins the pieces the map was cut into where it doesn't show: flat, evenly lit surfaces\n"
                    "become a few big triangles; shadows and edges keep theirs. The preview shows the result");
        if (L.unsimplified) {
            ImGui::SameLine();
            ImGui::TextDisabled("%d%% of %d", (int)(100.0f * L.tris / L.unsimplified + 0.5f), L.unsimplified);
        }
        int placed = 0;
        for (auto& p : L.props) placed += p.second;
        if (ui::component(ICON_BOXES, "Props")) {
            ImGui::Checkbox("Bring the props in too", &g.props);
            ui::tooltip("Each model once, under assets/gmod/, then placed in the scene where the map has it");
            ImGui::TextDisabled("%d placed, %zu kinds", placed, L.props.size());
            ImGui::BeginChild("props", ImVec2(0, ImGui::GetFontSize() * 8), ImGuiChildFlags_Borders);
            for (auto& p : L.props) {
                ImGui::Text("%3d  %s", p.second, p.first.c_str());
                if (ImGui::IsItemClicked()) gmod_open(p.first);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to look at it");
            }
            ImGui::EndChild();
        }
    } else {
        ImGui::Text("%d triangles, %d vertices, %d bones", L.tris, L.verts, L.bones);
    }

    /* Its looks */
    bool changed = false;
    if (L.skins > 1) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        changed |= ImGui::SliderInt("Skin", &g.skin, 0, L.skins - 1);
    }
    for (size_t i = 0; i < L.body.size(); i++) {
        const auto& part = L.body[i];
        if (part.second.size() < 2) continue;
        if (g.body.size() <= i) g.body.resize(i + 1, 0);
        int& c = g.body[i];
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        if (ImGui::BeginCombo(part.first.c_str(), part.second[std::min((size_t)c, part.second.size() - 1)].c_str())) {
            for (int k = 0; k < (int)part.second.size(); k++)
                if (ImGui::Selectable(part.second[k].c_str(), k == c)) { c = k; changed = true; }
            ImGui::EndCombo();
        }
    }
    if (changed) request();

    if (ui::component(ICON_IMAGE, "Textures", !L.map)) {
        if (ImGui::BeginTable("tex", 3, ImGuiTableFlags_SizingStretchProp)) {
            for (int mi = 0; mi < L.listed; mi++) {
                const Mat& m = L.mats[mi];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (m.tex) {
                    float s = ImGui::GetFontSize() * 2.2f;
                    ImGui::Image((ImTextureID)(intptr_t)m.tex, ImVec2(s * (m.ph ? (float)m.pw / m.ph : 1), s));
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::Image((ImTextureID)(intptr_t)m.tex, ImVec2((float)m.pw, (float)m.ph));
                        ImGui::EndTooltip();
                    }
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(m.name.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", m.vmt.c_str(), m.texture.c_str());
                ImGui::TableNextColumn();
                if (m.w) ImGui::TextDisabled("%d x %d %s", m.w, m.h, m.note.c_str());
                else ImGui::TextColored(ImColor(ui::WARN), "%s", m.note.c_str());
            }
            ImGui::EndTable();
        }
    }

    if (!L.anims.empty() && ui::component(ICON_PLAY, "Animations", false)) {
        std::string files;
        for (const std::string& f : L.anim_files) files += (files.empty() ? "" : ", ") + base_name(f);
        ImGui::TextDisabled("%zu, from %s. %zu chosen", L.anims.size(), files.c_str(), g.anims.size());
        if (ImGui::SmallButton("None")) g.anims.clear();
        ImGui::SameLine();
        if (ImGui::SmallButton("Player basics"))
            for (const char* n : PLAYER_BASICS)
                if (std::find(L.anims.begin(), L.anims.end(), n) != L.anims.end()) g.anims.insert(n);
        ui::tooltip("Idle, walk, run, crouch, crouch walk, jump");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##af", ICON_SEARCH "  Search", g.anim_filter, sizeof(g.anim_filter));
        std::string f = g.anim_filter;
        for (char& c : f) c = (char)tolower((unsigned char)c);
        std::vector<int> shown;
        for (int i = 0; i < (int)L.anims.size(); i++) {
            std::string n = L.anims[i];
            for (char& c : n) c = (char)tolower((unsigned char)c);
            if (f.empty() || n.find(f) != std::string::npos) shown.push_back(i);
        }
        ImGui::BeginChild("anims", ImVec2(0, ImGui::GetFontSize() * 10), ImGuiChildFlags_Borders);
        ImGuiListClipper clip;
        clip.Begin((int)shown.size());
        while (clip.Step())
            for (int k = clip.DisplayStart; k < clip.DisplayEnd; k++) {
                const std::string& n = L.anims[shown[k]];
                bool on = g.anims.count(n);
                if (ImGui::Checkbox(n.c_str(), &on)) { if (on) g.anims.insert(n); else g.anims.erase(n); }
            }
        ImGui::EndChild();
        ImGui::TextWrapped("Player models share one set: bring the animations in with one of them, then have the others "
                           "play its clips (Clips from, in the Inspector).");
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    ImGui::InputText("Name", g.name, sizeof(g.name), ImGuiInputTextFlags_CharsNoBlank);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
    const char* sizes[] = { "64", "128", "256", "512", "1024" };
    ImGui::Combo("Textures at most", &g.tex, sizes, 5);
    struct stat st;
    bool exists = g.name[0] && !stat((ed.project + "assets/" + bring_rel()).c_str(), &st);
    ImGui::BeginDisabled(g.bring_state == 1 || !g.name[0] || !L.error.empty());
    if (ImGui::Button(g.bring_state == 1 ? "Bringing in..." : exists ? ICON_PLUS "  Bring in again (replace)" : ICON_PLUS "  Bring in"))
        bring_in();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("to assets/%s", bring_rel().c_str());
    if (L.map) ImGui::TextDisabled("and a scene, scenes/%s.json, that it opens", g.name);
}

void gmod_window(void* sdl_renderer) {
    g.renderer = (SDL_Renderer*)sdl_renderer;
    poll();
    bring_poll();
    /* With the Console when the saved layout is older than this tab */
    if (ImGuiWindow* con = ImGui::FindWindowByName(WIN_CONSOLE))
        if (con->DockId) ImGui::SetNextWindowDockID(con->DockId, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(WIN_GMOD)) { ImGui::End(); return; }
    float fs = ImGui::GetFontSize();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float list_w = std::max(fs * 14, avail.x * 0.28f);
    float side_w = std::max(fs * 22, avail.x * 0.34f);
    float view_w = std::max(fs * 8, avail.x - list_w - side_w - 2 * ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginChild("browser", ImVec2(list_w, 0));
    browser();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("preview", ImVec2(view_w, 0));
    preview_view();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("details", ImVec2(0, 0));
    details();
    ImGui::EndChild();
    ImGui::End();
}

bool gmod_busy() { return g.list_state == 1 || g.state == 1 || g.want || g.bring_state == 1; }

void gmod_open(const std::string& mdl) {
    g.kind = mdl.size() > 4 && !mdl.compare(mdl.size() - 4, 4, ".bsp");
    select(mdl);
    if (ImGuiWindow* w = ImGui::FindWindowByName(WIN_GMOD)) ImGui::FocusWindow(w);
}

void gmod_shutdown() {
    for (std::thread* t : { &g.list_worker, &g.worker, &g.bring_worker })
        if (t->joinable()) t->join();
    textures_free(g.cur);
    gl_free(g.cur);
    if (g.target) SDL_DestroyTexture(g.target);
    if (gl.ctx) SDL_GL_DestroyContext(gl.ctx);
    if (gl.window) SDL_DestroyWindow(gl.window);
}
