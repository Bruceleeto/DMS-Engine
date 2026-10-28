/* What gmod2glb writes with: Source's space turned into glTF's, PNG, and
 * a .glb put together by hand */
#pragma once
#include "material.h"
#include "mdl.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <zlib.h>

static const float INCH = 0.0254f;

/* Source (Z up, X forward) to glTF (Y up, Z forward), as a rotation */
inline void to_gl(const Vec3& s, float* o, float scale) { o[0] = s.y * scale; o[1] = s.z * scale; o[2] = s.x * scale; }
inline void to_gl(const Quat& q, float* o) { o[0] = q.y; o[1] = q.z; o[2] = q.x; o[3] = q.w; }
static const int PERM[3] = { 1, 2, 0 };    /* gl axis i is Source axis PERM[i] */

/* ================================================================
 * PNG (zlib)
 * ================================================================ */

inline void be32(std::vector<uint8_t>& o, uint32_t v) { for (int s = 24; s >= 0; s -= 8) o.push_back((uint8_t)(v >> s)); }

inline void chunk(std::vector<uint8_t>& o, const char* type, const std::vector<uint8_t>& data) {
    be32(o, (uint32_t)data.size());
    size_t at = o.size();
    o.insert(o.end(), type, type + 4);
    o.insert(o.end(), data.begin(), data.end());
    be32(o, (uint32_t)crc32(0, &o[at], (uInt)(o.size() - at)));
}

inline std::vector<uint8_t> png(const Image& im, bool alpha) {
    int ch = alpha ? 4 : 3;
    std::vector<uint8_t> raw;
    raw.reserve((size_t)im.h * (im.w * ch + 1));
    for (int y = 0; y < im.h; y++) {
        raw.push_back(0);
        for (int x = 0; x < im.w; x++) {
            const uint8_t* p = &im.rgba[((size_t)y * im.w + x) * 4];
            raw.insert(raw.end(), p, p + ch);
        }
    }
    uLongf n = compressBound((uLong)raw.size());
    std::vector<uint8_t> z(n);
    compress2(z.data(), &n, raw.data(), (uLong)raw.size(), 6);
    z.resize(n);
    std::vector<uint8_t> o = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' }, ihdr;
    be32(ihdr, im.w);
    be32(ihdr, im.h);
    ihdr.insert(ihdr.end(), { 8, (uint8_t)(alpha ? 6 : 2), 0, 0, 0 });
    chunk(o, "IHDR", ihdr);
    chunk(o, "IDAT", z);
    chunk(o, "IEND", {});
    return o;
}

/* ================================================================
 * GLB
 * ================================================================ */

struct Glb {
    std::vector<uint8_t> bin;
    std::vector<std::string> views, accessors;

    int view(const void* data, size_t n, int target = 0) {
        while (bin.size() % 4) bin.push_back(0);
        size_t at = bin.size();
        bin.insert(bin.end(), (const uint8_t*)data, (const uint8_t*)data + n);
        char b[128];
        snprintf(b, sizeof(b), "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu%s}", at, n,
                 target == 1 ? ",\"target\":34962" : target == 2 ? ",\"target\":34963" : "");
        views.push_back(b);
        return (int)views.size() - 1;
    }
    /* comp: 5126 float, 5125 uint, 5123 ushort, 5121 ubyte */
    int accessor(const void* data, size_t count, int comp, const char* type, int target = 0,
                 const float* mn = nullptr, const float* mx = nullptr, int ncomp = 0) {
        size_t csize = comp == 5126 || comp == 5125 ? 4 : comp == 5123 ? 2 : 1;
        int per = !strcmp(type, "SCALAR") ? 1 : !strcmp(type, "VEC2") ? 2 : !strcmp(type, "VEC3") ? 3 : !strcmp(type, "VEC4") ? 4 : 16;
        int v = view(data, count * per * csize, target);
        std::string s = "{\"bufferView\":" + std::to_string(v) + ",\"componentType\":" + std::to_string(comp) +
                        ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"";
        if (mn && mx) {
            s += ",\"min\":[";
            for (int i = 0; i < ncomp; i++) s += (i ? "," : "") + std::to_string(mn[i]);
            s += "],\"max\":[";
            for (int i = 0; i < ncomp; i++) s += (i ? "," : "") + std::to_string(mx[i]);
            s += "]";
        }
        accessors.push_back(s + "}");
        return (int)accessors.size() - 1;
    }
};

inline std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o;
}

inline std::string join(const std::vector<std::string>& v) {
    std::string o;
    for (size_t i = 0; i < v.size(); i++) o += (i ? "," : "") + v[i];
    return o;
}

inline bool write_glb(const std::string& path, const std::string& json_in, std::vector<uint8_t> bin) {
    std::string json = json_in;
    while (json.size() % 4) json += ' ';
    while (bin.size() % 4) bin.push_back(0);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    uint32_t h[3] = { 0x46546C67, 2, (uint32_t)(12 + 8 + json.size() + (bin.empty() ? 0 : 8 + bin.size())) };
    fwrite(h, 4, 3, f);
    uint32_t jc[2] = { (uint32_t)json.size(), 0x4E4F534A };
    fwrite(jc, 4, 2, f);
    fwrite(json.data(), 1, json.size(), f);
    if (!bin.empty()) {
        uint32_t bc[2] = { (uint32_t)bin.size(), 0x004E4942 };
        fwrite(bc, 4, 2, f);
        fwrite(bin.data(), 1, bin.size(), f);
    }
    return fclose(f) == 0;
}

