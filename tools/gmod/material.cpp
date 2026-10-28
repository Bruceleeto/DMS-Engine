#include "material.h"
#include <cmath>
#include <cstring>
#include <map>

/* ================================================================
 * VTF
 * ================================================================ */

enum {
    F_RGBA8888 = 0, F_ABGR8888, F_RGB888, F_BGR888, F_RGB565, F_I8, F_IA88, F_P8, F_A8,
    F_RGB888_BLUESCREEN, F_BGR888_BLUESCREEN, F_ARGB8888, F_BGRA8888, F_DXT1, F_DXT3, F_DXT5,
    F_BGRX8888, F_BGR565, F_BGRX5551, F_BGRA4444, F_DXT1_ONEBITALPHA, F_BGRA5551, F_UV88,
    F_UVWQ8888, F_RGBA16161616F, F_RGBA16161616, F_UVLX8888,
};

static size_t mip_bytes(int fmt, int w, int h) {
    int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
    switch (fmt) {
    case F_DXT1: case F_DXT1_ONEBITALPHA: return (size_t)bw * bh * 8;
    case F_DXT3: case F_DXT5: return (size_t)bw * bh * 16;
    case F_RGBA8888: case F_ABGR8888: case F_ARGB8888: case F_BGRA8888: case F_BGRX8888:
    case F_UVWQ8888: case F_UVLX8888: return (size_t)w * h * 4;
    case F_RGB888: case F_BGR888: case F_RGB888_BLUESCREEN: case F_BGR888_BLUESCREEN: return (size_t)w * h * 3;
    case F_RGB565: case F_BGR565: case F_IA88: case F_BGRX5551: case F_BGRA4444: case F_BGRA5551: case F_UV88:
        return (size_t)w * h * 2;
    case F_I8: case F_P8: case F_A8: return (size_t)w * h;
    case F_RGBA16161616F: case F_RGBA16161616: return (size_t)w * h * 8;
    }
    return 0;
}

static void c565(uint16_t c, uint8_t* o) {
    o[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
    o[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
    o[2] = (uint8_t)((c & 31) * 255 / 31);
}

/* One DXT colour block into 4x4 RGBA; dxt1 with its 1-bit alpha */
static void dxt_colour(const uint8_t* b, uint8_t px[16][4], bool dxt1) {
    uint16_t a, c;
    memcpy(&a, b, 2);
    memcpy(&c, b + 2, 2);
    uint8_t pal[4][4];
    c565(a, pal[0]);
    c565(c, pal[1]);
    pal[0][3] = pal[1][3] = pal[2][3] = 255;
    pal[3][3] = 255;
    for (int k = 0; k < 3; k++) {
        if (a > c || !dxt1) {
            pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3);
        } else {
            pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
    }
    if (dxt1 && a <= c) pal[3][3] = 0;
    uint32_t bits;
    memcpy(&bits, b + 4, 4);
    for (int i = 0; i < 16; i++) memcpy(px[i], pal[(bits >> (2 * i)) & 3], 4);
}

bool vtf_decode(const std::vector<uint8_t>& d, int max_side, Image& out, std::string* why) {
    auto fail = [&](const char* w) { if (why) *why = w; return false; };
    if (d.size() < 64 || memcmp(d.data(), "VTF", 4)) return fail("not a VTF");
    uint32_t ver[2], hsize, flags, hifmt, lofmt;
    uint16_t w, h, frames, depth = 1;
    memcpy(ver, &d[4], 8);
    memcpy(&hsize, &d[12], 4);
    memcpy(&w, &d[16], 2);
    memcpy(&h, &d[18], 2);
    memcpy(&flags, &d[20], 4);
    memcpy(&frames, &d[24], 2);
    memcpy(&hifmt, &d[52], 4);
    uint8_t mips = d[56];
    memcpy(&lofmt, &d[57], 4);
    uint8_t low_w = d[61], low_h = d[62];
    if (ver[1] >= 2) memcpy(&depth, &d[63], 2);
    if (!depth) depth = 1;
    if (!frames) frames = 1;
    int faces = (flags & 0x4000) ? (ver[1] < 5 && ver[1] >= 1 ? 7 : 6) : 1;
    if ((flags & 0x4000) && ver[1] < 1) faces = 6;
    size_t data = 0;
    if (ver[1] >= 3) {
        uint32_t nres;
        memcpy(&nres, &d[68], 4);
        for (uint32_t i = 0; i < nres && 80 + i * 8 + 8 <= d.size(); i++) {
            const uint8_t* r = &d[80 + i * 8];
            if (r[0] == 0x30 && r[1] == 0 && r[2] == 0) memcpy(&data, r + 4, 4);
        }
        if (!data) return fail("no image resource");
    } else {
        data = hsize;
        if (lofmt != 0xFFFFFFFFu && low_w && low_h) data += mip_bytes((int)lofmt, low_w, low_h);
    }
    int fmt = (int)hifmt;
    if (!mip_bytes(fmt, 4, 4)) return fail("unknown format");
    if (!mips) mips = 1;
    /* Mips are smallest first; each holds frames x faces x slices */
    int pick = 0;
    while (max_side > 0 && pick + 1 < mips && std::max(w >> pick, h >> pick) > max_side) pick++;
    size_t off = data;
    for (int m = mips - 1; m > pick; m--)
        off += mip_bytes(fmt, std::max(1, w >> m), std::max(1, h >> m)) * frames * faces * std::max(1, depth >> m);
    int mw = std::max(1, w >> pick), mh = std::max(1, h >> pick);
    size_t need = mip_bytes(fmt, mw, mh);
    if (off + need > d.size()) return fail("truncated");
    const uint8_t* s = &d[off];
    out.w = mw;
    out.h = mh;
    out.rgba.assign((size_t)mw * mh * 4, 255);
    uint8_t* o = out.rgba.data();
    if (fmt == F_DXT1 || fmt == F_DXT1_ONEBITALPHA || fmt == F_DXT3 || fmt == F_DXT5) {
        int bs = (fmt == F_DXT3 || fmt == F_DXT5) ? 16 : 8;
        for (int by = 0; by < (mh + 3) / 4; by++)
            for (int bx = 0; bx < (mw + 3) / 4; bx++, s += bs) {
                uint8_t px[16][4];
                dxt_colour(s + (bs == 16 ? 8 : 0), px, bs == 8);
                if (fmt == F_DXT3) {
                    for (int i = 0; i < 16; i++) px[i][3] = (uint8_t)(((s[i / 2] >> (4 * (i & 1))) & 15) * 17);
                } else if (fmt == F_DXT5) {
                    uint8_t a0 = s[0], a1 = s[1], al[8] = { a0, a1 };
                    for (int k = 2; k < 8; k++)
                        al[k] = a0 > a1 ? (uint8_t)(((8 - k) * a0 + (k - 1) * a1) / 7)
                              : k < 6 ? (uint8_t)(((6 - k) * a0 + (k - 1) * a1) / 5) : (k == 6 ? 0 : 255);
                    uint64_t bits = 0;
                    for (int k = 0; k < 6; k++) bits |= (uint64_t)s[2 + k] << (8 * k);
                    for (int i = 0; i < 16; i++) px[i][3] = al[(bits >> (3 * i)) & 7];
                }
                for (int i = 0; i < 16; i++) {
                    int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x < mw && y < mh) memcpy(o + ((size_t)y * mw + x) * 4, px[i], 4);
                }
            }
        return true;
    }
    for (int i = 0; i < mw * mh; i++, o += 4) {
        switch (fmt) {
        case F_RGBA8888: case F_UVWQ8888: case F_UVLX8888: memcpy(o, s, 4); s += 4; break;
        case F_ABGR8888: o[0] = s[3]; o[1] = s[2]; o[2] = s[1]; o[3] = s[0]; s += 4; break;
        case F_ARGB8888: o[0] = s[1]; o[1] = s[2]; o[2] = s[3]; o[3] = s[0]; s += 4; break;
        case F_BGRA8888: o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = s[3]; s += 4; break;
        case F_BGRX8888: o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; s += 4; break;
        case F_RGB888: case F_RGB888_BLUESCREEN: memcpy(o, s, 3); s += 3; break;
        case F_BGR888: case F_BGR888_BLUESCREEN: o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; s += 3; break;
        case F_RGB565: { uint16_t c; memcpy(&c, s, 2); c565(c, o); s += 2; break; }
        case F_BGR565: { uint16_t c; memcpy(&c, s, 2); c565(c, o); std::swap(o[0], o[2]); s += 2; break; }
        case F_BGRA4444: { uint16_t c; memcpy(&c, s, 2);
            o[2] = (c & 15) * 17; o[1] = ((c >> 4) & 15) * 17; o[0] = ((c >> 8) & 15) * 17; o[3] = ((c >> 12) & 15) * 17;
            s += 2; break; }
        case F_BGRX5551: case F_BGRA5551: { uint16_t c; memcpy(&c, s, 2);
            o[2] = (c & 31) * 255 / 31; o[1] = ((c >> 5) & 31) * 255 / 31; o[0] = ((c >> 10) & 31) * 255 / 31;
            o[3] = fmt == F_BGRA5551 && !(c >> 15) ? 0 : 255; s += 2; break; }
        case F_I8: o[0] = o[1] = o[2] = s[0]; s++; break;
        case F_IA88: o[0] = o[1] = o[2] = s[0]; o[3] = s[1]; s += 2; break;
        case F_A8: o[0] = o[1] = o[2] = 255; o[3] = s[0]; s++; break;
        case F_UV88: o[0] = s[0]; o[1] = s[1]; o[2] = 0; s += 2; break;
        case F_RGBA16161616: for (int k = 0; k < 4; k++) { uint16_t v; memcpy(&v, s + 2 * k, 2); o[k] = v >> 8; } s += 8; break;
        case F_RGBA16161616F: for (int k = 0; k < 4; k++) {
            uint16_t hv; memcpy(&hv, s + 2 * k, 2);
            int e = (hv >> 10) & 31, m = hv & 1023;
            float f = e ? ldexpf(1.0f + m / 1024.0f, e - 15) : ldexpf(m / 1024.0f, -14);
            if (k < 3) f = f / (1.0f + f);              /* HDR squashed, roughly */
            o[k] = (uint8_t)std::min(255.0f, f * 255.0f + 0.5f);
            } s += 8; break;
        default: return fail("unsupported format");
        }
    }
    return true;
}

/* ================================================================
 * VMT: KeyValues
 * ================================================================ */

struct KV {
    std::string key, value;
    std::vector<KV> kids;
    const KV* get(const char* k) const {
        for (const KV& c : kids) if (!strcasecmp(c.key.c_str(), k)) return &c;
        return nullptr;
    }
};

static bool token(const char*& p, std::string& out) {
    for (;;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
        break;
    }
    if (!*p) return false;
    out.clear();
    if (*p == '{' || *p == '}') { out = *p++; return true; }
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++) out += *p;
        if (*p) p++;
        return true;
    }
    while (*p && !isspace((unsigned char)*p) && *p != '{' && *p != '}' && *p != '"') out += *p++;
    return true;
}

static void parse_kids(const char*& p, KV& into) {
    std::string t;
    while (token(p, t)) {
        if (t == "}") return;
        KV kv;
        kv.key = t;
        if (!token(p, t)) return;
        if (t == "{") parse_kids(p, kv);
        else kv.value = t;
        into.kids.push_back(kv);
    }
}

static bool kv_parse(const std::string& text, KV& root) {
    const char* p = text.c_str();
    std::string t;
    if (!token(p, root.key)) return false;
    if (!token(p, t) || t != "{") return false;
    parse_kids(p, root);
    return true;
}

static bool flag(const KV& m, const char* k) {
    const KV* v = m.get(k);
    return v && v->value != "0" && v->value != "";
}

bool vmt_load(const Vfs& vfs, const std::string& path_in, Material& out) {
    std::string path = Vfs::norm(path_in);
    if (path.compare(0, 10, "materials/")) path = "materials/" + path;
    if (path.size() < 4 || path.compare(path.size() - 4, 4, ".vmt")) path += ".vmt";
    std::vector<uint8_t> raw;
    if (!vfs.read(path, raw)) return false;
    KV root;
    if (!kv_parse(std::string(raw.begin(), raw.end()), root)) return false;
    std::string shader = Vfs::norm(root.key);
    if (shader == "patch") {
        const KV* inc = root.get("include");
        Material base;
        if (inc && vmt_load(vfs, inc->value, base)) out = base;
        for (const char* section : { "insert", "replace" })
            if (const KV* s = root.get(section)) {
                if (const KV* b = s->get("$basetexture")) out.basetexture = Vfs::norm(b->value);
                if (s->get("$alphatest")) out.alphatest = flag(*s, "$alphatest");
                if (s->get("$translucent")) out.translucent = flag(*s, "$translucent");
            }
        return !out.shader.empty();
    }
    out.shader = shader;
    const KV* b = root.get("$basetexture");
    if (!b && (shader == "eyes" || shader == "eyerefract")) b = root.get("$iris");
    if (b) out.basetexture = Vfs::norm(b->value);
    out.alphatest = flag(root, "$alphatest");
    out.translucent = flag(root, "$translucent");
    out.additive = flag(root, "$additive");
    out.nocull = flag(root, "$nocull");
    out.selfillum = flag(root, "$selfillum");
    return true;
}
