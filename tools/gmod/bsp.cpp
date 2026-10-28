#include "bsp.h"
#include "../converter/meshoptimizer/src/meshoptimizer.h"
#include <algorithm>
#include <cmath>
#include <cstring>

static const float INCH = 0.0254f;
static const float PI = 3.14159265f;

template <class T> static T rd(const std::vector<uint8_t>& b, size_t off) {
    T v{};
    if (off + sizeof(T) <= b.size()) memcpy(&v, &b[off], sizeof(T));
    return v;
}
static int ri(const std::vector<uint8_t>& b, size_t off) { return rd<int32_t>(b, off); }
static float rf(const std::vector<uint8_t>& b, size_t off) { return rd<float>(b, off); }

struct V3 { float x, y, z; };
static V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3 operator*(V3 a, float k) { return { a.x * k, a.y * k, a.z * k }; }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static V3 norm(V3 a) { float l = sqrtf(dot(a, a)); return l > 0 ? a * (1.0f / l) : a; }
static V3 rv(const std::vector<uint8_t>& b, size_t off) { return { rf(b, off), rf(b, off + 4), rf(b, off + 8) }; }

/* Source (Z up, X forward) to glTF (Y up, Z forward) */
static void gl(V3 s, float* o, float k) { o[0] = s.y * k; o[1] = s.z * k; o[2] = s.x * k; }

enum {
    L_ENTITIES = 0, L_PLANES = 1, L_TEXDATA = 2, L_VERTEXES = 3, L_NODES = 5, L_TEXINFO = 6, L_FACES = 7, L_LIGHTING = 8,
    L_LEAFS = 10, L_EDGES = 12, L_SURFEDGES = 13, L_MODELS = 14, L_DISPINFO = 26, L_DISP_VERTS = 33, L_GAME = 35,
    L_PAKFILE = 40, L_TEXSTRDATA = 43, L_TEXSTRTABLE = 44, L_LIGHTING_HDR = 53, L_FACES_HDR = 58,
};
enum {
    SURF_SKY2D = 0x2, SURF_SKY = 0x4, SURF_TRIGGER = 0x40, SURF_NODRAW = 0x80, SURF_HINT = 0x100, SURF_SKIP = 0x200,
    SURF_BUMPLIGHT = 0x800,
};

size_t Bsp::tris() const {
    size_t n = 0;
    for (const BspGroup& g : groups) n += g.idx.size() / 3;
    return n;
}

void bsp_angles(const float ang[3], float r[9]) {
    float sp = sinf(ang[0] * PI / 180), cp = cosf(ang[0] * PI / 180);
    float sy = sinf(ang[1] * PI / 180), cy = cosf(ang[1] * PI / 180);
    float sr = sinf(ang[2] * PI / 180), cr = cosf(ang[2] * PI / 180);
    /* Source's AngleMatrix: m[row][col], its columns forward, left, up */
    float m[3][3] = {
        { cp * cy, sr * sp * cy - cr * sy, cr * sp * cy + sr * sy },
        { cp * sy, sr * sp * sy + cr * cy, cr * sp * sy - sr * cy },
        { -sp, sr * cp, cr * cp },
    };
    /* In glTF's axes: gl axis i is Source axis (1, 2, 0)[i] */
    static const int P[3] = { 1, 2, 0 };
    for (int c = 0; c < 3; c++)
        for (int row = 0; row < 3; row++) r[c * 3 + row] = m[P[row]][P[c]];
}

void bsp_rot_degrees(const float r[9], float out[3]) {
    float x, y, z;
    if (fabsf(r[7]) < 0.9999f) {
        x = asinf(-r[7]);
        y = atan2f(r[6], r[8]);
        z = atan2f(r[1], r[4]);
    } else {
        x = r[7] < 0.0f ? PI / 2 : -PI / 2;
        y = atan2f(-r[2], r[0]);
        z = 0.0f;
    }
    out[0] = x * 180 / PI;
    out[1] = y * 180 / PI;
    out[2] = z * 180 / PI;
}

/* The entities lump: { "key" "value" ... } ... */
static std::vector<BspEntity> parse_entities(const char* p, const char* end) {
    std::vector<BspEntity> out;
    auto str = [&](std::string& s) {
        while (p < end && *p != '"' && *p != '{' && *p != '}') p++;
        if (p >= end || *p != '"') return false;
        p++;
        s.clear();
        while (p < end && *p != '"') s += *p++;
        if (p < end) p++;
        return true;
    };
    while (p < end) {
        while (p < end && *p != '{') p++;
        if (p >= end) break;
        p++;
        BspEntity e;
        for (;;) {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            if (p >= end || *p == '}') { p++; break; }
            std::string k, v;
            if (!str(k) || !str(v)) break;
            for (char& c : k) c = (char)tolower((unsigned char)c);
            e[k] = v;
        }
        out.push_back(e);
    }
    return out;
}

static V3 parse_v3(const std::string& s) {
    V3 v{ 0, 0, 0 };
    sscanf(s.c_str(), "%f %f %f", &v.x, &v.y, &v.z);
    return v;
}

namespace {
struct Reader {
    std::vector<uint8_t> f;
    size_t ofs[64], len[64];
    size_t at(int lump) const { return ofs[lump]; }
    size_t count(int lump, size_t size) const { return len[lump] / size; }
};
}

/* ColorRGBExp32 at byte offset o, to a 0..255 screen colour */
static void luxel(const std::vector<uint8_t>& f, size_t o, float out[3]) {
    int e = (int8_t)(o + 3 < f.size() ? f[o + 3] : 0);
    float k = ldexpf(1.0f, e) / 255.0f;
    for (int c = 0; c < 3; c++) {
        /* Overbright: the game's lightmaps are half what they light */
        float lin = (o + c < f.size() ? f[o + c] : 0) * k * 2.0f;
        /* Linear light to the screen's gamma, as the game shows it */
        out[c] += std::min(255.0f, powf(std::max(0.0f, lin), 1.0f / 2.2f) * 255.0f);
    }
}

bool bsp_load(Vfs& vfs, const std::string& path, Bsp& out, std::string* why) {
    auto fail = [&](const std::string& w) { if (why) *why = w; return false; };
    Reader R;
    out.path = Vfs::norm(path);
    out.name = out.path.substr(out.path.find_last_of('/') + 1);
    if (out.name.size() > 4) out.name.resize(out.name.size() - 4);
    if (!vfs.read(out.path, R.f)) return fail("not found: " + out.path);
    auto& f = R.f;
    if (f.size() < 1036 || memcmp(f.data(), "VBSP", 4)) return fail("not a Source map");
    int version = ri(f, 4);
    if (version < 19 || version > 21) return fail("map version " + std::to_string(version));
    for (int i = 0; i < 64; i++) {
        R.ofs[i] = (size_t)ri(f, 8 + i * 16);
        R.len[i] = (size_t)ri(f, 8 + i * 16 + 4);
        if (R.ofs[i] + R.len[i] > f.size()) R.len[i] = 0;
    }
    /* Lumps may be LZMA'd: those unpacked onto the end of the file, where
     * the reads below find them */
    for (int i = 0; i < 64; i++) {
        if (i == L_GAME || R.len[i] < 17 || memcmp(&f[R.ofs[i]], "LZMA", 4)) continue;
        std::vector<uint8_t> raw;
        if (!valve_unlzma(&f[R.ofs[i]], R.len[i], raw)) {
            fprintf(stderr, "%s: lump %d not unpacked\n", out.path.c_str(), i);
            R.len[i] = 0;
            continue;
        }
        R.ofs[i] = f.size();
        R.len[i] = raw.size();
        f.insert(f.end(), raw.begin(), raw.end());
    }
    /* Some maps have only the HDR faces and light */
    int faces_lump = R.len[L_FACES] ? L_FACES : L_FACES_HDR;
    int light_lump = R.len[L_LIGHTING] ? L_LIGHTING : L_LIGHTING_HDR;

    /* Its own files (materials, mostly), over the game's */
    if (R.len[L_PAKFILE]) {
        std::vector<uint8_t> zip(f.begin() + R.ofs[L_PAKFILE], f.begin() + R.ofs[L_PAKFILE] + R.len[L_PAKFILE]);
        vfs.add_zip(std::move(zip), out.path + " (packed)");
    }

    out.entities = parse_entities((const char*)&f[R.ofs[L_ENTITIES]], (const char*)&f[R.ofs[L_ENTITIES]] + R.len[L_ENTITIES]);

    /* Texture names */
    size_t ntexdata = R.count(L_TEXDATA, 32);
    std::vector<std::string> tex_name(ntexdata);
    std::vector<float> tex_w(ntexdata, 256), tex_h(ntexdata, 256);
    for (size_t i = 0; i < ntexdata; i++) {
        size_t o = R.at(L_TEXDATA) + i * 32;
        int sid = ri(f, o + 12);
        tex_w[i] = (float)std::max(1, ri(f, o + 16));
        tex_h[i] = (float)std::max(1, ri(f, o + 20));
        if (sid >= 0 && (size_t)sid < R.count(L_TEXSTRTABLE, 4)) {
            size_t so = R.at(L_TEXSTRDATA) + (size_t)ri(f, R.at(L_TEXSTRTABLE) + sid * 4);
            std::string n;
            while (so < f.size() && f[so]) n += (char)f[so++];
            tex_name[i] = Vfs::norm(n);
        }
    }

    std::map<std::string, int> group_of;
    auto group = [&](int td) -> BspGroup& {
        std::string mat = "materials/" + tex_name[td] + ".vmt";
        auto it = group_of.find(mat);
        if (it == group_of.end()) {
            it = group_of.emplace(mat, (int)out.groups.size()).first;
            BspGroup g;
            g.material = mat;
            g.tex_w = tex_w[td];
            g.tex_h = tex_h[td];
            out.groups.push_back(g);
        }
        return out.groups[it->second];
    };

    auto vertex = [&](int i) { return rv(f, R.at(L_VERTEXES) + (size_t)i * 12); };
    size_t nfaces = R.count(faces_lump, 56), ntexinfo = R.count(L_TEXINFO, 72);

    /* The area (a sealed part of the map) a point is in: down the tree to its leaf */
    size_t leaf_size = version == 19 ? 56 : 32;
    auto area_at = [&](V3 p) {
        int node = 0;
        for (int guard = 0; node >= 0 && guard < 4096; guard++) {
            if ((size_t)node >= R.count(L_NODES, 32)) return -1;
            size_t no = R.at(L_NODES) + (size_t)node * 32;
            size_t po = R.at(L_PLANES) + (size_t)ri(f, no) * 20;
            float d = dot(rv(f, po), p) - rf(f, po + 12);
            node = ri(f, no + (d >= 0 ? 4 : 8));
        }
        int leaf = -1 - node;
        if (node >= 0 || (size_t)leaf >= R.count(L_LEAFS, leaf_size)) return -1;
        return rd<uint16_t>(f, R.at(L_LEAFS) + (size_t)leaf * leaf_size + 6) & 0x1ff;
    };
    /* The 3D skybox: a room of its own, a sixteenth the size, that the game
     * draws behind everything. Left out (its props too) */
    int sky_area = -1;
    for (const BspEntity& e : out.entities) {
        auto c = e.find("classname"), org = e.find("origin");
        if (c != e.end() && c->second == "sky_camera" && org != e.end()) sky_area = area_at(parse_v3(org->second));
    }
    if (sky_area == 0) sky_area = -1;     /* outside the map: not a room */

    /* Every corner on each plane: a face's edge with another's corner along
     * it (a T-junction) gets that corner too, so faces side by side share
     * their edges and can be joined when simplified */
    std::map<int, std::vector<V3>> plane_corners;
    for (size_t fi = 0; fi < nfaces; fi++) {
        size_t fo = R.at(faces_lump) + fi * 56;
        if (rd<int16_t>(f, fo + 12) >= 0) continue;
        int first = ri(f, fo + 4), nedges = rd<int16_t>(f, fo + 8);
        std::vector<V3>& list = plane_corners[rd<uint16_t>(f, fo)];
        for (int k = 0; k < nedges; k++) {
            int se = ri(f, R.at(L_SURFEDGES) + (size_t)(first + k) * 4);
            list.push_back(vertex(rd<uint16_t>(f, R.at(L_EDGES) + (size_t)std::abs(se) * 4 + (se >= 0 ? 0 : 2))));
        }
    }
    for (auto& kv : plane_corners) {
        auto less = [](const V3& a, const V3& b) { return a.x != b.x ? a.x < b.x : a.y != b.y ? a.y < b.y : a.z < b.z; };
        auto same = [](const V3& a, const V3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
        std::sort(kv.second.begin(), kv.second.end(), less);
        kv.second.erase(std::unique(kv.second.begin(), kv.second.end(), same), kv.second.end());
    }

    /* A face's triangles into its material's group */
    auto add_face = [&](int fi, V3 offset) {
        size_t fo = R.at(faces_lump) + (size_t)fi * 56;
        int ti = rd<int16_t>(f, fo + 10);
        if (ti < 0 || (size_t)ti >= ntexinfo) return;
        size_t to = R.at(L_TEXINFO) + (size_t)ti * 72;
        int flags = ri(f, to + 64), td = ri(f, to + 68);
        if (flags & (SURF_SKY2D | SURF_SKY | SURF_TRIGGER | SURF_NODRAW | SURF_HINT | SURF_SKIP)) return;
        if (td < 0 || (size_t)td >= ntexdata) return;
        if (!tex_name[td].compare(0, 6, "tools/")) return;
        float tv[2][4], lv[2][4];
        for (int k = 0; k < 8; k++) { tv[k / 4][k % 4] = rf(f, to + 4 * k); lv[k / 4][k % 4] = rf(f, to + 32 + 4 * k); }

        /* The plane it is on faces the way it does: a face on the back of a
         * node's plane has the plane's flipped twin (odd numbered) already */
        int plane = rd<uint16_t>(f, fo);
        V3 pn = rv(f, R.at(L_PLANES) + (size_t)plane * 20);
        int first = ri(f, fo + 4), nedges = rd<int16_t>(f, fo + 8), disp = rd<int16_t>(f, fo + 12);
        int lightofs = ri(f, fo + 20);
        int mins[2] = { ri(f, fo + 28), ri(f, fo + 32) }, size[2] = { ri(f, fo + 36), ri(f, fo + 40) };
        bool lit = lightofs >= 0 && f[fo + 16] != 255 && R.len[light_lump];

        std::vector<V3> poly;
        for (int k = 0; k < nedges; k++) {
            int se = ri(f, R.at(L_SURFEDGES) + (size_t)(first + k) * 4);
            size_t eo = R.at(L_EDGES) + (size_t)std::abs(se) * 4;
            poly.push_back(vertex(rd<uint16_t>(f, eo + (se >= 0 ? 0 : 2))));
        }
        if (poly.size() < 3) return;
        if (sky_area >= 0) {
            /* Just in front of its middle, where it is seen from */
            V3 mid{ 0, 0, 0 };
            for (const V3& p : poly) mid = mid + p;
            if (area_at(mid * (1.0f / poly.size()) + offset + pn) == sky_area) { out.sky_faces++; return; }
        }

        /* The lightmap where a point (on the undisplaced face) falls */
        auto light = [&](V3 p, uint8_t* rgba) {
            rgba[3] = 255;
            if (!lit) { rgba[0] = rgba[1] = rgba[2] = 200; return; }
            float u = p.x * lv[0][0] + p.y * lv[0][1] + p.z * lv[0][2] + lv[0][3] - mins[0];
            float v = p.x * lv[1][0] + p.y * lv[1][1] + p.z * lv[1][2] + lv[1][3] - mins[1];
            u = std::min(std::max(u, 0.0f), (float)size[0]);
            v = std::min(std::max(v, 0.0f), (float)size[1]);
            int u0 = std::min((int)u, size[0]), v0 = std::min((int)v, size[1]);
            int u1 = std::min(u0 + 1, size[0]), v1 = std::min(v0 + 1, size[1]);
            float fu = u - u0, fv = v - v0;
            int w = size[0] + 1;
            float c[3] = { 0, 0, 0 }, s[4][3] = {};
            int us[4] = { u0, u1, u0, u1 }, vs[4] = { v0, v0, v1, v1 };
            float wt[4] = { (1 - fu) * (1 - fv), fu * (1 - fv), (1 - fu) * fv, fu * fv };
            for (int k = 0; k < 4; k++) {
                luxel(f, R.at(light_lump) + (size_t)lightofs + ((size_t)vs[k] * w + us[k]) * 4, s[k]);
                for (int ch = 0; ch < 3; ch++) c[ch] += s[k][ch] * wt[k];
            }
            for (int ch = 0; ch < 3; ch++) rgba[ch] = (uint8_t)std::min(255.0f, c[ch]);
        };

        BspGroup& g = group(td);
        uint32_t base = (uint32_t)(g.pos.size() / 3);
        auto emit = [&](V3 p, V3 flat, V3 n) {
            float o[3];
            gl(p + offset, o, INCH);
            g.pos.insert(g.pos.end(), o, o + 3);
            gl(n, o, 1.0f);
            g.nrm.insert(g.nrm.end(), o, o + 3);
            g.uv.push_back((flat.x * tv[0][0] + flat.y * tv[0][1] + flat.z * tv[0][2] + tv[0][3]) / g.tex_w);
            g.uv.push_back((flat.x * tv[1][0] + flat.y * tv[1][1] + flat.z * tv[1][2] + tv[1][3]) / g.tex_h);
            uint8_t c[4];
            light(flat, c);
            g.rgba.insert(g.rgba.end(), c, c + 4);
        };
        /* Counter-clockwise about the face's normal, as glTF has front faces */
        auto tri = [&](uint32_t a, uint32_t b, uint32_t c, V3 pa, V3 pb, V3 pc) {
            if (dot(cross(pb - pa, pc - pa), pn) < 0) std::swap(b, c);
            g.idx.insert(g.idx.end(), { base + a, base + b, base + c });
        };

        if (disp >= 0 && (size_t)disp < R.count(L_DISPINFO, 176) && poly.size() == 4) {
            size_t d = R.at(L_DISPINFO) + (size_t)disp * 176;
            V3 start = rv(f, d);
            int vstart = ri(f, d + 12), power = ri(f, d + 20);
            int n = (1 << power) + 1;
            int s0 = 0;
            float best = 1e30f;
            for (int k = 0; k < 4; k++) {
                V3 dd = poly[k] - start;
                if (dot(dd, dd) < best) { best = dot(dd, dd); s0 = k; }
            }
            V3 c[4];
            for (int k = 0; k < 4; k++) c[k] = poly[(s0 + k) % 4];
            std::vector<V3> flat(n * n), pos(n * n), nrm(n * n, V3{ 0, 0, 0 });
            for (int y = 0; y < n; y++) {
                float fy = (float)y / (n - 1);
                V3 a = c[0] + (c[1] - c[0]) * fy, b = c[3] + (c[2] - c[3]) * fy;
                for (int x = 0; x < n; x++) {
                    float fx = (float)x / (n - 1);
                    V3 p = a + (b - a) * fx;
                    size_t dv = R.at(L_DISP_VERTS) + (size_t)(vstart + y * n + x) * 20;
                    flat[y * n + x] = p;
                    pos[y * n + x] = p + rv(f, dv) * rf(f, dv + 12);
                }
            }
            /* Two triangles a cell, the diagonal alternating, as the game cuts them */
            std::vector<uint32_t> cells;
            for (int y = 0; y + 1 < n; y++)
                for (int x = 0; x + 1 < n; x++) {
                    uint32_t i0 = y * n + x, i1 = y * n + x + 1, i2 = (y + 1) * n + x, i3 = (y + 1) * n + x + 1;
                    if ((x + y) & 1) cells.insert(cells.end(), { i0, i2, i1, i1, i2, i3 });
                    else cells.insert(cells.end(), { i0, i2, i3, i0, i3, i1 });
                }
            /* Which way round faces the face's own normal: the same for every cell */
            bool flip = false;
            V3 sum{ 0, 0, 0 };
            for (size_t k = 0; k < cells.size(); k += 3)
                sum = sum + cross(flat[cells[k + 1]] - flat[cells[k]], flat[cells[k + 2]] - flat[cells[k]]);
            if (dot(sum, pn) < 0) flip = true;
            for (size_t k = 0; k < cells.size(); k += 3) {
                if (flip) std::swap(cells[k + 1], cells[k + 2]);
                V3 tn = cross(pos[cells[k + 1]] - pos[cells[k]], pos[cells[k + 2]] - pos[cells[k]]);
                for (int j = 0; j < 3; j++) nrm[cells[k + j]] = nrm[cells[k + j]] + tn;
            }
            for (int k = 0; k < n * n; k++) emit(pos[k], flat[k], norm(nrm[k]));
            for (uint32_t i : cells) g.idx.push_back(base + i);
            return;
        }
        /* Corners of the faces beside it that lie along its edges */
        std::vector<V3> ring;
        const std::vector<V3>& corners = plane_corners[plane];
        for (size_t k = 0; k < poly.size(); k++) {
            V3 a = poly[k], e = poly[(k + 1) % poly.size()] - a;
            float len2 = dot(e, e);
            ring.push_back(a);
            if (len2 < 1e-6f) continue;
            std::vector<std::pair<float, V3>> on;
            for (const V3& c : corners) {
                float t = dot(c - a, e) / len2;
                if (t <= 1e-4f || t >= 1 - 1e-4f) continue;
                V3 off = c - (a + e * t);
                if (dot(off, off) < 1e-4f) on.push_back({ t, c });
            }
            std::sort(on.begin(), on.end(), [](const std::pair<float, V3>& x, const std::pair<float, V3>& y) { return x.first < y.first; });
            for (auto& o : on) ring.push_back(o.second);
        }
        if (ring.size() == poly.size()) {
            for (const V3& p : poly) emit(p, p, pn);
            for (size_t k = 1; k + 1 < poly.size(); k++) tri(0, (uint32_t)k, (uint32_t)k + 1, poly[0], poly[k], poly[k + 1]);
            return;
        }
        /* Corners along its edges: fanned from its middle, so none of the
         * triangles is a sliver along an edge */
        V3 mid{ 0, 0, 0 };
        for (const V3& p : poly) mid = mid + p;
        mid = mid * (1.0f / poly.size());
        for (const V3& p : ring) emit(p, p, pn);
        emit(mid, mid, pn);
        uint32_t m = (uint32_t)ring.size();
        for (size_t k = 0; k < ring.size(); k++) {
            uint32_t n = (uint32_t)((k + 1) % ring.size());
            tri(m, (uint32_t)k, n, mid, ring[k], ring[n]);
        }
    };

    /* The world, then the brush entities where they stand (doors, walls, glass) */
    size_t nmodels = R.count(L_MODELS, 48);
    auto add_model = [&](int mi, V3 offset) {
        if (mi < 0 || (size_t)mi >= nmodels) return;
        size_t mo = R.at(L_MODELS) + (size_t)mi * 48;
        int ff = ri(f, mo + 40), nf = ri(f, mo + 44);
        for (int k = 0; k < nf; k++)
            if ((size_t)(ff + k) < nfaces) add_face(ff + k, offset);
    };
    add_model(0, { 0, 0, 0 });
    for (const BspEntity& e : out.entities) {
        auto cls = e.find("classname"), model = e.find("model");
        if (cls == e.end() || model == e.end() || model->second.empty() || model->second[0] != '*') continue;
        const std::string& c = cls->second;
        if (!c.compare(0, 8, "trigger_") || c == "func_areaportal" || c == "func_areaportalwindow" || c == "func_occluder" ||
            !c.compare(0, 9, "func_clip") || c == "func_viscluster" || c == "func_buyzone" || c == "func_bomb_target")
            continue;
        auto org = e.find("origin");
        add_model(atoi(model->second.c_str() + 1), org == e.end() ? V3{ 0, 0, 0 } : parse_v3(org->second));
    }
    /* Groups with nothing in them (all their faces left out) go */
    out.groups.erase(std::remove_if(out.groups.begin(), out.groups.end(), [](const BspGroup& g) { return g.idx.empty(); }),
                     out.groups.end());

    /* Static props, in the game lump */
    if (R.len[L_GAME] >= 4) {
        size_t go = R.at(L_GAME);
        int n = ri(f, go);
        for (int i = 0; i < n; i++) {
            size_t e = go + 4 + (size_t)i * 16;
            uint32_t id = (uint32_t)ri(f, e);
            int gflags = rd<uint16_t>(f, e + 4), gver = rd<uint16_t>(f, e + 6);
            size_t gofs = (size_t)ri(f, e + 8), glen = (size_t)ri(f, e + 12);
            if (id != 0x73707270 /* sprp */ || !glen || gofs + glen > f.size()) continue;
            /* Packed: its own LZMA, unpacked onto the end of the file */
            if (gflags & 1) {
                std::vector<uint8_t> raw;
                if (!valve_unlzma(&f[gofs], glen, raw)) {
                    fprintf(stderr, "%s: its static props are not unpacked: left out\n", out.path.c_str());
                    break;
                }
                gofs = f.size();
                glen = raw.size();
                f.insert(f.end(), raw.begin(), raw.end());
            }
            size_t p = gofs, end = gofs + glen;
            int ndict = ri(f, p);
            p += 4;
            std::vector<std::string> dict;
            for (int k = 0; k < ndict; k++, p += 128) {
                std::string s;
                for (size_t c = p; c < p + 128 && c < f.size() && f[c]; c++) s += (char)f[c];
                dict.push_back(Vfs::norm(s));
            }
            int nleaf = ri(f, p);
            p += 4 + (size_t)nleaf * 2;
            int nprop = ri(f, p);
            p += 4;
            if (nprop <= 0) break;
            size_t each = (end - p) / nprop;
            if (each < 36) { fprintf(stderr, "static props version %d: not read\n", gver); break; }
            for (int k = 0; k < nprop; k++, p += each) {
                int type = rd<uint16_t>(f, p + 24);
                if (type >= (int)dict.size()) continue;
                BspProp pr;
                pr.model = dict[type];
                gl(rv(f, p), pr.pos, INCH);
                V3 a = rv(f, p + 12);
                float ang[3] = { a.x, a.y, a.z };
                bsp_angles(ang, pr.rot);
                pr.skin = ri(f, p + 32);
                if (sky_area >= 0 && area_at(rv(f, p)) == sky_area) { out.sky_props++; continue; }
                out.props.push_back(pr);
            }
        }
    }
    /* And the props the map places as entities */
    for (const BspEntity& e : out.entities) {
        auto cls = e.find("classname"), model = e.find("model");
        if (cls == e.end() || model == e.end()) continue;
        if (cls->second.compare(0, 12, "prop_physics") && cls->second.compare(0, 12, "prop_dynamic") &&
            cls->second != "prop_ragdoll")
            continue;
        BspProp pr;
        pr.model = Vfs::norm(model->second);
        pr.from_entity = true;
        auto org = e.find("origin"), ang = e.find("angles"), skin = e.find("skin");
        gl(org == e.end() ? V3{ 0, 0, 0 } : parse_v3(org->second), pr.pos, INCH);
        V3 a = ang == e.end() ? V3{ 0, 0, 0 } : parse_v3(ang->second);
        float an[3] = { a.x, a.y, a.z };
        bsp_angles(an, pr.rot);
        pr.skin = skin == e.end() ? 0 : atoi(skin->second.c_str());
        if (sky_area >= 0 && org != e.end() && area_at(parse_v3(org->second)) == sky_area) { out.sky_props++; continue; }
        out.props.push_back(pr);
    }

    /* Where a player starts, and the sky */
    for (const char* cls : { "info_player_start", "info_player_deathmatch", "info_player_combine", "info_player_terrorist" }) {
        for (const BspEntity& e : out.entities) {
            auto c = e.find("classname");
            if (c == e.end() || c->second != cls) continue;
            auto org = e.find("origin"), ang = e.find("angles");
            gl(org == e.end() ? V3{ 0, 0, 0 } : parse_v3(org->second), out.spawn, INCH);
            /* Source's yaw turns from +X (glTF's +Z) towards +Y (glTF's +X) */
            out.spawn_yaw = ang == e.end() ? 0 : parse_v3(ang->second).y;
            out.has_spawn = true;
            break;
        }
        if (out.has_spawn) break;
    }
    for (const BspEntity& e : out.entities) {
        auto c = e.find("classname"), s = e.find("skyname");
        if (c != e.end() && c->second == "worldspawn" && s != e.end()) out.sky = s->second;
    }
    if (out.groups.empty()) return fail("no faces drawn");
    if (!out.has_spawn) {
        /* No start: over the middle of the map (the examples drop to the ground) */
        float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
        for (const BspGroup& g : out.groups)
            for (size_t i = 0; i < g.pos.size(); i++) {
                mn[i % 3] = std::min(mn[i % 3], g.pos[i]);
                mx[i % 3] = std::max(mx[i % 3], g.pos[i]);
            }
        for (int k = 0; k < 3; k++) out.spawn[k] = (mn[k] + mx[k]) * 0.5f;
        out.spawn[1] = mx[1];
    }
    return true;
}

size_t bsp_simplify(Bsp& b, float error) {
    size_t before = b.tris();
    for (BspGroup& g : b.groups) {
        /* One vertex for each place, normal and texture coordinate; its
         * light the mean of the faces' there (each face has its own lightmap) */
        size_t nv = g.pos.size() / 3;
        std::map<std::vector<float>, uint32_t> at;
        std::vector<uint32_t> remap(nv);
        std::vector<float> pos, nrm, uv, col;
        std::vector<int> count;
        for (size_t i = 0; i < nv; i++) {
            /* Rounded: neighbouring faces' texture coordinates differ in the last bits */
            auto r = [](float x, float k) { return roundf(x * k) / k; };
            std::vector<float> key = { g.pos[i * 3], g.pos[i * 3 + 1], g.pos[i * 3 + 2], r(g.nrm[i * 3], 1e3f),
                                       r(g.nrm[i * 3 + 1], 1e3f), r(g.nrm[i * 3 + 2], 1e3f), r(g.uv[i * 2], 1e4f),
                                       r(g.uv[i * 2 + 1], 1e4f) };
            auto it = at.find(key);
            if (it == at.end()) {
                it = at.emplace(key, (uint32_t)count.size()).first;
                pos.insert(pos.end(), &key[0], &key[3]);
                nrm.insert(nrm.end(), &key[3], &key[6]);
                uv.insert(uv.end(), &key[6], &key[8]);
                col.insert(col.end(), { 0, 0, 0 });
                count.push_back(0);
            }
            uint32_t v = it->second;
            remap[i] = v;
            for (int k = 0; k < 3; k++) col[v * 3 + k] += g.rgba[i * 4 + k] / 255.0f;
            count[v]++;
        }
        for (size_t v = 0; v < count.size(); v++)
            for (int k = 0; k < 3; k++) col[v * 3 + k] /= count[v];
        std::vector<uint32_t> idx;
        for (size_t t = 0; t + 2 < g.idx.size(); t += 3) {
            uint32_t a = remap[g.idx[t]], c1 = remap[g.idx[t + 1]], c2 = remap[g.idx[t + 2]];
            if (a != c1 && a != c2 && c1 != c2) idx.insert(idx.end(), { a, c1, c2 });
        }
        size_t wn = count.size();
        if (error > 0 && !idx.empty()) {
            std::vector<uint32_t> out(idx.size());
            static const float weights[3] = { 1, 1, 1 };
            /* Edges are not locked: a vertex slides along a straight edge (which
             * leaves the edge where it is), not off it */
            unsigned opts = meshopt_SimplifyErrorAbsolute;
            size_t n = meshopt_simplifyWithAttributes(out.data(), idx.data(), idx.size(), pos.data(), wn, 12, col.data(), 12,
                                                      weights, 3, nullptr, 0, error,
                                                      opts, nullptr);
            out.resize(n);
            idx.swap(out);
        }
        /* Only the vertices still used, in the order they are */
        std::vector<int64_t> keep(wn, -1);
        BspGroup s;
        s.material = g.material;
        s.tex_w = g.tex_w;
        s.tex_h = g.tex_h;
        for (uint32_t& i : idx) {
            if (keep[i] < 0) {
                keep[i] = (int64_t)(s.pos.size() / 3);
                s.pos.insert(s.pos.end(), &pos[i * 3], &pos[i * 3] + 3);
                s.nrm.insert(s.nrm.end(), &nrm[i * 3], &nrm[i * 3] + 3);
                s.uv.insert(s.uv.end(), &uv[i * 2], &uv[i * 2] + 2);
                for (int k = 0; k < 3; k++) s.rgba.push_back((uint8_t)std::min(255.0f, col[i * 3 + k] * 255.0f + 0.5f));
                s.rgba.push_back(255);
            }
            i = (uint32_t)keep[i];
        }
        s.idx.swap(idx);
        g = std::move(s);
    }
    b.groups.erase(std::remove_if(b.groups.begin(), b.groups.end(), [](const BspGroup& g) { return g.idx.empty(); }),
                   b.groups.end());
    return before;
}
