// ============================================================================
// CORE on the GPU (BT3_PVR_RENDER=gpu, the default; =soft keeps the tiles)
//
// The tile lists are walked as render_tile() walks them, every polygon taken
// once (a polygon is in the list of each tile it touches: the same parameter
// address), in the order of its parameters, which is the order the TA was
// given them. They are drawn in one call, each triangle as its bounding box,
// and the fragment shader is CORE's pixel loop: the edge test with the
// top-left rule, Z, the texture fetched from PVR memory as TextureSampler
// does, tsp_shade_pixel, fog, the colour clamp, then (in primitive order,
// ARB_fragment_shader_interlock) the depth test, punch-through and the
// blend into the primary and secondary accumulation buffers, kept as images.
// The primary buffer is read back and written to the frame buffer by
// flush_tile_to_framebuffer, as a tile is.
//
// Auto-sort keeps each pixel's translucent fragments, the 8 nearest sorted by
// depth as they come (one farther than all 8 is blended at once), and a
// second pass blends them back to front, as Flycast's OIT does.
//
// What it does not do goes back to the tiles for that render: modifier volumes, a polygon in a tile set that is not a rectangle (user
// clip "outside"), tiles that keep their buffers (z_clear) or appear twice.
// ============================================================================

#include "core.h"
#include "vram.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <string>

namespace pvr {

namespace {

#define PVR_GL_FUNCS(X) \
    X(GetString, const GLubyte *, (GLenum)) \
    X(GetError, GLenum, (void)) \
    X(Viewport, void, (GLint, GLint, GLsizei, GLsizei)) \
    X(Disable, void, (GLenum)) \
    X(DrawArrays, void, (GLenum, GLint, GLsizei)) \
    X(GenTextures, void, (GLsizei, GLuint *)) \
    X(DeleteTextures, void, (GLsizei, const GLuint *)) \
    X(BindTexture, void, (GLenum, GLuint)) \
    X(GetTexImage, void, (GLenum, GLint, GLenum, GLenum, void *)) \
    X(PixelStorei, void, (GLenum, GLint)) \
    X(TexStorage2D, void, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) \
    X(ClearTexImage, void, (GLuint, GLint, GLenum, GLenum, const void *)) \
    X(BindImageTexture, void, (GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum)) \
    X(MemoryBarrier, void, (GLbitfield)) \
    X(Finish, void, (void)) \
    X(CreateShader, GLuint, (GLenum)) \
    X(ShaderSource, void, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(CompileShader, void, (GLuint)) \
    X(GetShaderiv, void, (GLuint, GLenum, GLint *)) \
    X(GetShaderInfoLog, void, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(DeleteShader, void, (GLuint)) \
    X(CreateProgram, GLuint, (void)) \
    X(AttachShader, void, (GLuint, GLuint)) \
    X(LinkProgram, void, (GLuint)) \
    X(GetProgramiv, void, (GLuint, GLenum, GLint *)) \
    X(GetProgramInfoLog, void, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(UseProgram, void, (GLuint)) \
    X(GetUniformLocation, GLint, (GLuint, const GLchar *)) \
    X(Uniform1ui, void, (GLint, GLuint)) \
    X(Uniform1f, void, (GLint, GLfloat)) \
    X(Uniform2f, void, (GLint, GLfloat, GLfloat)) \
    X(GenBuffers, void, (GLsizei, GLuint *)) \
    X(BindBuffer, void, (GLenum, GLuint)) \
    X(BufferData, void, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(BufferSubData, void, (GLenum, GLintptr, GLsizeiptr, const void *)) \
    X(BindBufferBase, void, (GLenum, GLuint, GLuint)) \
    X(GenVertexArrays, void, (GLsizei, GLuint *)) \
    X(BindVertexArray, void, (GLuint)) \
    X(GenFramebuffers, void, (GLsizei, GLuint *)) \
    X(BindFramebuffer, void, (GLenum, GLuint)) \
    X(FramebufferParameteri, void, (GLenum, GLenum, GLint))

#define X(name, ret, args) ret (APIENTRY *gl_##name) args;
struct GlFuncs { PVR_GL_FUNCS(X) };
#undef X

GlFuncs gl;

// One triangle as the fragment shader reads it (std430: 8 vec4)
struct GlTri {
    float x0, y0, x1, y1;        // a
    float x2, y2, inv_area, lod; // b
    float z0, z1, z2, pad0;      // c
    float u0, u1, u2, pad1;      // tu
    float v0, v1, v2, pad2;      // tv
    uint32_t c0, c1, c2, isp;    // base colours, ISP/TSP word
    uint32_t o0, o1, o2, tsp;    // offset colours, TSP word
    uint32_t tcw, flags, bb0, bb1; // flags: 0-2 list, 3-5 fill edge 0-2; bb: x | y << 16, inclusive
};
static_assert(sizeof(GlTri) == 128, "GlTri is the shader's struct");

// A polygon (strip triangle, array triangle or quad) of the tile lists
struct GlPrim {
    uint64_t key;      // phase << 48 | parameter address << 20 | index << 16 | copy in the tile
    uint32_t addr;
    uint8_t  list;
    uint8_t  kind;     // 0 strip triangle, 1 triangle array, 2 quad
    uint8_t  skip;
    uint8_t  idx;
    int tx0, ty0, tx1, ty1, tiles;
    uint32_t id;       // the order it was found in, for prim_tiles
};

const char *kVertex = R"(#version 430 core
struct Tri { vec4 a; vec4 b; vec4 c; vec4 tu; vec4 tv; uvec4 col; uvec4 ofs; uvec4 misc; };
layout(std430, binding = 0) readonly buffer TriBuf { Tri tris[]; };
uniform vec2 u_screen;
flat out uint v_tri;
void main() {
    uint t = uint(gl_VertexID) / 6u;
    uint k = uint(gl_VertexID) % 6u;
    uvec4 m = tris[t].misc;
    float x0 = float(m.z & 0xFFFFu), y0 = float(m.z >> 16);
    float x1 = float(m.w & 0xFFFFu) + 1.0, y1 = float(m.w >> 16) + 1.0;
    vec2 p = (k == 0u) ? vec2(x0, y0) : (k == 1u || k == 3u) ? vec2(x1, y0)
           : (k == 4u) ? vec2(x1, y1) : vec2(x0, y1);
    v_tri = t;
    gl_Position = vec4(p / u_screen * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char *kFragment = R"(#version 430 core
#extension GL_ARB_fragment_shader_interlock : require
layout(pixel_interlock_ordered) in;
struct Tri { vec4 a; vec4 b; vec4 c; vec4 tu; vec4 tv; uvec4 col; uvec4 ofs; uvec4 misc; };
layout(std430, binding = 0) readonly buffer TriBuf { Tri tris[]; };
layout(std430, binding = 1) readonly buffer VramBuf { uint vram[]; };
layout(std430, binding = 2) readonly buffer RegBuf { uint pal[1024]; uint fogt[128]; };
layout(binding = 0, r32ui) coherent uniform uimage2D img_prim;
layout(binding = 1, r32ui) coherent uniform uimage2D img_sec;
layout(binding = 2, r32f) coherent uniform image2D img_depth;
layout(binding = 3, r32ui) coherent uniform uimage2D img_count;    // auto-sort layers held
layout(std430, binding = 3) coherent buffer LayerBuf { uvec4 layers[]; };   // z, colour, TSP
uniform uint u_stride;
const uint LAYERS = 8u;
uniform uint u_vram_mask;
uniform uint u_half;       // HALF_OFFSET
uniform uint u_pt_ref;
uniform uint u_pal_fmt;
uniform uint u_text_stride;
uniform uint u_fog_col_ram, u_fog_col_vert, u_fog_density, u_clamp_min, u_clamp_max;
uniform float u_edge_margin;
flat in uint v_tri;

// colours are uvec4(r, g, b, a), packed as ARGB8888
uvec4 unpack(uint c) { return uvec4((c >> 16) & 255u, (c >> 8) & 255u, c & 255u, c >> 24); }
uint pack(uvec4 p) { return (p.a << 24) | (p.r << 16) | (p.g << 8) | p.b; }
uvec4 modulate(uvec4 x, uvec4 y) { return (x * y + 127u) / 255u; }
uvec4 add_sat(uvec4 x, uvec4 y) { return min(x + y, uvec4(255u)); }
uvec4 lerp_p(uvec4 x, uvec4 y, uint t) { return (x * (255u - t) + y * t + 127u) / 255u; }

uint rd8(uint a) { a &= u_vram_mask; return (vram[a >> 2] >> ((a & 3u) * 8u)) & 255u; }
uint rd16(uint a) {
    a &= u_vram_mask;
    if ((a & 1u) == 0u) return (vram[a >> 2] >> ((a & 2u) * 8u)) & 0xFFFFu;
    return rd8(a) | (rd8(a + 1u) << 8);
}
uint rd32(uint a) {
    a &= u_vram_mask;
    if ((a & 3u) == 0u) return vram[a >> 2];
    return rd16(a) | (rd16(a + 2u) << 16);
}

uint ext5(uint v) { return (v << 3) | (v >> 2); }
uint ext6(uint v) { return (v << 2) | (v >> 4); }
uint ext4(uint v) { return (v << 4) | v; }
uvec4 dec1555(uint r) { return uvec4(ext5((r >> 10) & 31u), ext5((r >> 5) & 31u), ext5(r & 31u), (r >> 15) != 0u ? 255u : 0u); }
uvec4 dec565(uint r) { return uvec4(ext5((r >> 11) & 31u), ext6((r >> 5) & 63u), ext5(r & 31u), 255u); }
uvec4 dec4444(uint r) { return uvec4(ext4((r >> 8) & 15u), ext4((r >> 4) & 15u), ext4(r & 15u), ext4((r >> 12) & 15u)); }
uvec4 dec_yuv(uint y, uint u, uint v) {
    float yf = float(y), uf = float(u), vf = float(v);
    const float c = 11.0 / 8.0;
    precise float rf = yf + c * (vf - 128.0);
    precise float gf = yf - 0.25 * c * (uf - 128.0) - 0.5 * c * (vf - 128.0);
    precise float bf = yf + 1.25 * c * (uf - 128.0);
    return uvec4(uint(clamp(rf + 0.5, 0.0, 255.0)), uint(clamp(gf + 0.5, 0.0, 255.0)),
                 uint(clamp(bf + 0.5, 0.0, 255.0)), 255u);
}
uvec4 dec_pal(uint i) {
    uint e = pal[i & 1023u];
    if (u_pal_fmt == 1u) return dec565(e & 0xFFFFu);
    if (u_pal_fmt == 2u) return dec4444(e & 0xFFFFu);
    if (u_pal_fmt == 3u) return unpack(e);
    return dec1555(e & 0xFFFFu);
}
uvec4 pal4(uint i, uint sel) { return dec_pal(((sel & 63u) << 4) | (i & 15u)); }
uvec4 pal8(uint i, uint sel) { return dec_pal((((sel >> 4) & 3u) << 8) | i); }
uvec4 decode16(uint fmt, uint r) {
    if (fmt == 1u) return dec565(r);
    if (fmt == 2u) return dec4444(r);
    if (fmt == 4u) return uvec4((r >> 8) & 255u, r & 255u, 255u, 255u); // bump: S, R
    return dec1555(r);
}

uint spread(uint x) {
    x &= 0x3FFu;
    x = (x | (x << 8u)) & 0x00FF00FFu;
    x = (x | (x << 4u)) & 0x0F0F0F0Fu;
    x = (x | (x << 2u)) & 0x33333333u;
    x = (x | (x << 1u)) & 0x55555555u;
    return x;
}
uint log2i(int s) { uint n = 0u; for (; s > 1; s >>= 1) n++; return n; }
uint twiddle(int u, int v, int su, int sv) {
    if (su == sv) return spread(uint(v)) | (spread(uint(u)) << 1u);
    int mn = min(su, sv), mx = max(su, sv);
    uint mb = log2i(mn);
    uint mask = uint(mn - 1);
    uint a = spread(uint(v) & mask) | (spread(uint(u) & mask) << 1u);
    a &= (1u << (2u * mb)) - 1u;
    uint extra = uint(su > sv ? u : v) >> mb;
    a |= (extra & uint(mx / mn - 1)) << (2u * mb);
    return a;
}
uint entry16(uint a, uint k) { return (rd32(a + (k >> 1) * 4u) >> ((k & 1u) * 16u)) & 0xFFFFu; }

uvec4 fetch_vq(uint tcw, int us, int vs, int ui, int vi) {
    uint base = (tcw & 0x1FFFFFu) << 3;
    uint fmt = (tcw >> 27) & 7u;
    bool palf = fmt == 5u || fmt == 6u;
    bool scan = !palf && ((tcw >> 26) & 1u) != 0u;
    uint sel = (tcw >> 21) & 63u;
    uint cb = base, ix = base + 2048u;
    if (scan) {
        uint pitch = uint(max(1, us / 4));
        uint idx = rd8(ix + uint(vi) * pitch + uint(ui >> 2));
        uint raw = entry16(cb + idx * 8u, uint(ui & 3));
        return fmt == 1u ? dec565(raw) : fmt == 2u ? dec4444(raw) : dec1555(raw);
    }
    if (fmt == 6u) {
        uint bt = twiddle(ui >> 1, vi >> 2, max(1, us >> 1), max(1, vs >> 2));
        uint idx = rd8(ix + bt);
        uint bi = twiddle(ui & 1, vi & 3, 2, 4);
        return pal8(rd8(cb + idx * 8u + bi), sel);
    }
    if (fmt == 5u) {
        uint bt = twiddle(ui >> 2, vi >> 2, max(1, us >> 2), max(1, vs >> 2));
        uint idx = rd8(ix + bt);
        uint ni = twiddle(ui & 3, vi & 3, 4, 4);
        uint b = rd8(cb + idx * 8u + (ni >> 1));
        return pal4((b >> ((ni & 1u) * 4u)) & 15u, sel);
    }
    if ((tcw >> 31) != 0u) {
        const uint off[11] = uint[11](0x0u, 0x1u, 0x2u, 0x6u, 0x16u, 0x56u, 0x156u, 0x556u, 0x1556u, 0x5556u, 0x15556u);
        ix += off[min(log2i(max(us, vs)), 10u)];
    }
    uint bt = twiddle(ui >> 1, vi >> 1, max(1, us / 2), max(1, vs / 2));
    uint idx = rd8(ix + bt);
    uint raw = entry16(cb + idx * 8u, uint((ui & 1) * 2 + (vi & 1)));
    return fmt == 1u ? dec565(raw) : fmt == 2u ? dec4444(raw) : dec1555(raw);
}

uvec4 fetch(uint tcw, int us, int vs, int ui, int vi) {
    if (((tcw >> 30) & 1u) != 0u) return fetch_vq(tcw, us, vs, ui, vi);
    uint base = (tcw & 0x1FFFFFu) << 3;
    uint fmt = (tcw >> 27) & 7u;
    bool palf = fmt == 5u || fmt == 6u;
    bool scan = !palf && ((tcw >> 26) & 1u) != 0u;
    uint sel = (tcw >> 21) & 63u;
    uint ta;
    if (!scan) {
        uint mipo = 0u;
        if ((tcw >> 31) != 0u) {
            const uint off[11] = uint[11](0x3u, 0x4u, 0x8u, 0x18u, 0x58u, 0x158u, 0x558u, 0x1558u, 0x5558u, 0x15558u, 0x55558u);
            mipo = off[min(log2i(max(us, vs)), 10u)];
        }
        if (fmt == 3u) {
            uint d = rd32(base + twiddle(ui >> 1, vi, max(1, us / 2), vs) * 4u);
            return dec_yuv((ui & 1) != 0 ? (d >> 24) : ((d >> 8) & 255u), d & 255u, (d >> 16) & 255u);
        }
        ta = twiddle(ui, vi, us, vs) + mipo;
    } else {
        uint pitch = uint(us);
        if (((tcw >> 25) & 1u) != 0u && u_text_stride != 0u) pitch = u_text_stride * 32u;
        if (fmt == 3u) {
            uint d = rd32(base + (uint(vi) * uint(max(1, us / 2)) + uint(ui >> 1)) * 4u);
            return dec_yuv((ui & 1) != 0 ? (d >> 24) : ((d >> 8) & 255u), d & 255u, (d >> 16) & 255u);
        }
        ta = uint(vi) * pitch + uint(ui);
    }
    if (fmt == 5u) {
        uint b = rd8(base + ta / 2u);
        return pal4((ta & 1u) != 0u ? (b >> 4) & 15u : b & 15u, sel);
    }
    if (fmt == 6u) return pal8(rd8(base + ta), sel);
    return decode16(fmt, rd16(base + ta * 2u));
}

float address(float c, int size, bool flip, bool clampm) {
    float fs = float(size);
    if (clampm) return clamp(c, 0.0, fs - 1.0);
    if (flip) {
        precise float tile = floor(c / fs);
        precise float local = c - tile * fs;
        if (local < 0.0) local += fs;
        if ((int(tile) & 1) != 0) { local = (fs - 1.0) - local; if (local < 0.0) local = 0.0; }
        return local;
    }
    precise float w = c - fs * trunc(c / fs);
    if (w < 0.0) w += fs;
    return w;
}
int addr_coord(int c, int size, bool flip, bool clampm) {
    if (clampm) return clamp(c, 0, size - 1);
    if (flip) {
        int tile = c < 0 ? -1 : (c < size ? 0 : 1);
        int local = c - tile * size;
        if ((tile & 1) != 0) local = (size - 1) - local;
        return local;
    }
    return c & (size - 1);
}
float lerpf(float a, float b, float t) { precise float r = a + (b - a) * t; return r; }

uvec4 sample_tex(uint tsp, uint tcw, float u, float v, float lod, bool texel_half) {
    int us = 8 << ((tsp >> 3) & 7u), vs = 8 << (tsp & 7u);
    uint fmt = (tcw >> 27) & 7u;
    bool palf = fmt == 5u || fmt == 6u;
    bool scan = !palf && ((tcw >> 26) & 1u) != 0u;
    uint filt = (tsp >> 13) & 3u;
    if ((tcw >> 31) != 0u && !scan && lod > 0.0) {
        int lvl = int(lod);
        if (filt == 3u) lvl += 1;
        lvl = min(lvl, int(log2i(max(us, vs))));
        us = max(1, us >> lvl);
        vs = max(1, vs >> lvl);
    }
    bool flu = ((tsp >> 18) & 1u) != 0u, flv = ((tsp >> 17) & 1u) != 0u;
    bool clu = ((tsp >> 16) & 1u) != 0u, clv = ((tsp >> 15) & 1u) != 0u;
    precise float uscaled = u * float(us);
    precise float vscaled = v * float(vs);
    uscaled = address(uscaled, us, flu, clu);
    vscaled = address(vscaled, vs, flv, clv);
    if (filt == 0u) {
        int ui = int(floor(uscaled)), vi = int(floor(vscaled));
        if (u_edge_margin > 0.0) {
            precise float fu = uscaled - float(ui);
            precise float fv = vscaled - float(vi);
            if (fu < u_edge_margin) ui = (ui - 1 + us) % us;
            else if (fu > 1.0 - u_edge_margin) ui = (ui + 1) % us;
            if (fv < u_edge_margin) vi = (vi - 1 + vs) % vs;
            else if (fv > 1.0 - u_edge_margin) vi = (vi + 1) % vs;
        }
        return fetch(tcw, us, vs, clamp(ui, 0, us - 1), clamp(vi, 0, vs - 1));
    }
    float ho = texel_half ? 0.5 : 0.0;
    precise float uf = uscaled - ho;
    precise float vf = vscaled - ho;
    int u0 = int(floor(uf)), v0 = int(floor(vf));
    precise float fu = uf - floor(uf);
    precise float fv = vf - floor(vf);
    int u0c = addr_coord(u0, us, flu, clu), u1c = addr_coord(u0 + 1, us, flu, clu);
    int v0c = addr_coord(v0, vs, flv, clv), v1c = addr_coord(v0 + 1, vs, flv, clv);
    vec4 p00 = vec4(fetch(tcw, us, vs, u0c, v0c)), p10 = vec4(fetch(tcw, us, vs, u1c, v0c));
    vec4 p01 = vec4(fetch(tcw, us, vs, u0c, v1c)), p11 = vec4(fetch(tcw, us, vs, u1c, v1c));
    uvec4 r;
    for (int i = 0; i < 4; i++) {
        float x = lerpf(lerpf(p00[i], p10[i], fu), lerpf(p01[i], p11[i], fu), fv);
        r[i] = uint(clamp(x + 0.5, 0.0, 255.0));
    }
    return r;
}

uint lerp8(uint a, uint b, uint c, float w0, float w1, float w2) {
    if (a == b && b == c) return a;
    precise float v = float(a) * w0 + float(b) * w1 + float(c) * w2 + 0.5;
    return uint(v < 0.0 ? 0.0 : v > 255.0 ? 255.0 : v);
}
uvec4 lerp_col(uint c0, uint c1, uint c2, float w0, float w1, float w2) {
    uvec4 a = unpack(c0), b = unpack(c1), c = unpack(c2);
    return uvec4(lerp8(a.r, b.r, c.r, w0, w1, w2), lerp8(a.g, b.g, c.g, w0, w1, w2),
                 lerp8(a.b, b.b, c.b, w0, w1, w2), lerp8(a.a, b.a, c.a, w0, w1, w2));
}
float edgef(float ax, float ay, float bx, float by, float cx, float cy) {
    precise float r = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    return r;
}

float fog_lookup(float z) {
    uint d = u_fog_density;
    int e = int(d & 0xFFu); if (e > 127) e -= 256;
    float dens = ldexp(1.0 + float((d >> 8) & 0x7Fu) / 128.0, e);
    precise float val = abs(z) * dens;
    val = max(1.0, min(val, 255.9999));
    uint bits = floatBitsToUint(val);
    int ex = int((bits >> 23) & 0xFFu) - 127;
    int idx = clamp(((ex & 7) << 4) | int((bits >> 19) & 0xFu), 0, 127);
    uint en = fogt[idx];
    float ct = float((en >> 8) & 255u), cn = float(en & 255u);
    float fr = float((bits >> 11) & 0xFFu) / 255.0;
    precise float fa = (ct * (1.0 - fr) + cn * fr) / 255.0;
    return clamp(fa, 0.0, 1.0);
}

uvec4 blend_factor(uint ins, uvec4 src, uvec4 dst, bool is_src) {
    uvec4 other = is_src ? dst : src;
    if (ins == 0u) return uvec4(0u);
    if (ins == 1u) return uvec4(255u);
    if (ins == 2u) return other;
    if (ins == 3u) return uvec4(255u) - other;
    if (ins == 4u) return uvec4(src.a);
    if (ins == 5u) return uvec4(255u - src.a);
    if (ins == 6u) return uvec4(dst.a);
    return uvec4(255u - dst.a);
}

// a translucent fragment into the accumulation buffers
void blend_px(ivec2 p, uvec4 shaded, uint tsp) {
    bool ssel = ((tsp >> 25) & 1u) != 0u, dsel = ((tsp >> 24) & 1u) != 0u;
    uvec4 sec = (ssel || dsel) ? unpack(imageLoad(img_sec, p).r) : uvec4(0u);
    uvec4 src = ssel ? sec : shaded;
    uvec4 dst = dsel ? sec : unpack(imageLoad(img_prim, p).r);
    uvec4 sf = blend_factor(tsp >> 29, src, dst, true);
    uvec4 df = blend_factor((tsp >> 26) & 7u, src, dst, false);
    uint res = pack(add_sat(modulate(src, sf), modulate(dst, df)));
    if (dsel) imageStore(img_sec, p, uvec4(res));
    else imageStore(img_prim, p, uvec4(res));
}

bool depth_test(uint mode, float z, float cur) {
    if (mode == 0u) return false;
    if (mode == 1u) return z < cur;
    if (mode == 2u) return z == cur;
    if (mode == 3u) return z <= cur;
    if (mode == 4u) return z > cur;
    if (mode == 5u) return z != cur;
    if (mode == 6u) return z >= cur;
    return true;
}

void main() {
    Tri t = tris[v_tri];
    ivec2 p = ivec2(gl_FragCoord.xy);
    float po = (u_half & 2u) != 0u ? 0.5 : 0.0;
    float px = float(p.x) + po, py = float(p.y) + po;
    float ia = t.b.z;
    precise float w0 = edgef(t.a.z, t.a.w, t.b.x, t.b.y, px, py) * ia;
    precise float w1 = edgef(t.b.x, t.b.y, t.a.x, t.a.y, px, py) * ia;
    precise float w2 = edgef(t.a.x, t.a.y, t.a.z, t.a.w, px, py) * ia;
    uint fl = t.misc.y;
    precise float e0 = w0 + ((fl & 8u) != 0u ? 0.0 : -1.0e-5);
    precise float e1 = w1 + ((fl & 16u) != 0u ? 0.0 : -1.0e-5);
    precise float e2 = w2 + ((fl & 32u) != 0u ? 0.0 : -1.0e-5);
    if (e0 < 0.0 || e1 < 0.0 || e2 < 0.0) discard;

    precise float z;
    if (t.c.x == t.c.y && t.c.y == t.c.z) z = t.c.x;
    else z = t.c.x * w0 + t.c.y * w1 + t.c.z * w2;

    uint list = fl & 7u;
    uint isp = t.col.w, tsp = t.ofs.w, tcw = t.misc.x;
    bool texd = ((isp >> 25) & 1u) != 0u;
    bool offs = ((isp >> 24) & 1u) != 0u;
    bool gour = ((isp >> 23) & 1u) != 0u;
    bool zwd = ((isp >> 26) & 1u) != 0u;

    // ---- tsp_shade_pixel, fog, colour clamp (they read no buffer) ----
    uvec4 col = gour ? lerp_col(t.col.x, t.col.y, t.col.z, w0, w1, w2) : unpack(t.col.z);
    uint offc = (offs && gour) ? pack(lerp_col(t.ofs.x, t.ofs.y, t.ofs.z, w0, w1, w2))
              : (offs ? t.ofs.z : 0u);
    uvec4 off = unpack(offc);
    uint fmt = (tcw >> 27) & 7u;
    if (texd && fmt == 4u) off = uvec4(0u);
    if (((tsp >> 20) & 1u) == 0u) col.a = 255u;
    uvec4 shaded = col;
    if (texd) {
        precise float wz0 = w0 * t.c.x, wz1 = w1 * t.c.y, wz2 = w2 * t.c.z;
        precise float den = wz0 + wz1 + wz2;
        float inv = abs(den) > 1e-12 ? 1.0 / den : 0.0;
        precise float u = (t.tu.x * wz0 + t.tu.y * wz1 + t.tu.z * wz2) * inv;
        precise float v = (t.tv.x * wz0 + t.tv.y * wz1 + t.tv.z * wz2) * inv;
        uvec4 tex = sample_tex(tsp, tcw, u, v, t.b.w, (u_half & 4u) != 0u);
        if (fmt == 4u) {
            const float PI = 3.14159265358979323846;
            float s_rad = float(tex.r) * (PI * 0.5 / 256.0);
            float r_rad = float(tex.g) * (PI * 2.0 / 256.0);
            float q_rad = float(offc & 255u) * (PI * 2.0 / 256.0);
            float k1 = float(offc >> 24) / 255.0, k2 = float((offc >> 16) & 255u) / 255.0;
            float k3 = float((offc >> 8) & 255u) / 255.0;
            float br = clamp(k1 + k2 * sin(s_rad) + k3 * cos(s_rad) * cos(r_rad - q_rad), 0.0, 1.0);
            tex = uvec4(255u, 255u, 255u, uint(br * 255.0 + 0.5));
        } else if (((tsp >> 19) & 1u) != 0u) {
            tex.a = 255u;
        }
        uint sh = (tsp >> 6) & 3u;
        uvec4 o3 = uvec4(off.rgb, 0u);
        if (sh == 0u) {
            shaded = add_sat(tex, o3);
        } else if (sh == 1u) {
            shaded = modulate(col, tex); shaded.a = tex.a; shaded = add_sat(shaded, o3);
        } else if (sh == 2u) {
            uint ta = tex.a;
            shaded = uvec4((tex.rgb * ta + col.rgb * (255u - ta) + 127u) / 255u, col.a);
            shaded = add_sat(shaded, o3);
        } else {
            shaded = modulate(col, tex); shaded.a = (col.a * tex.a + 127u) / 255u;
            shaded = add_sat(shaded, o3);
        }
    }
    uint fog = (tsp >> 22) & 3u;
    if (fog == 0u) {
        uint fa = uint(min(fog_lookup(z) * 255.0, 255.0));
        uvec4 f = lerp_p(shaded, unpack(u_fog_col_ram | 0xFF000000u), fa);
        shaded = uvec4(f.rgb, shaded.a);
    } else if (fog == 1u) {
        uvec4 f = lerp_p(shaded, unpack(u_fog_col_vert | 0xFF000000u), offc >> 24);
        shaded = uvec4(f.rgb, shaded.a);
    } else if (fog == 3u) {
        shaded = uvec4(unpack(u_fog_col_ram).rgb, uint(min(fog_lookup(z) * 255.0, 255.0)));
    }
    if (((tsp >> 21) & 1u) != 0u)
        shaded = clamp(shaded, unpack(u_clamp_min), unpack(u_clamp_max));

    // ---- the ISP depth test and the accumulation buffers, in primitive order ----
    beginInvocationInterlockARB();
    float cur = imageLoad(img_depth, p).r;
    if ((fl & 64u) != 0u) {
        // auto-sort: always GreaterOrEqual, kept to blend later, farthest first;
        // equal depths stay in the order given
        if (z >= cur) {
            uint base = (uint(p.y) * u_stride + uint(p.x)) * LAYERS;
            uint n = imageLoad(img_count, p).r;
            bool keep = true;
            if (n == LAYERS) {
                if (z < uintBitsToFloat(layers[base].x)) {
                    blend_px(p, shaded, tsp);
                    keep = false;
                } else {
                    blend_px(p, unpack(layers[base].y), layers[base].z);
                    for (uint i = 0u; i + 1u < LAYERS; i++) layers[base + i] = layers[base + i + 1u];
                    n = LAYERS - 1u;
                }
            }
            if (keep) {
                uint i = n;
                while (i > 0u && uintBitsToFloat(layers[base + i - 1u].x) > z) {
                    layers[base + i] = layers[base + i - 1u];
                    i--;
                }
                layers[base + i] = uvec4(floatBitsToUint(z), pack(shaded), tsp, 0u);
                imageStore(img_count, p, uvec4(n + 1u));
            }
        }
    }
    bool pass = (fl & 64u) == 0u && depth_test(isp >> 29, z, cur);
    if (list == 4u) {
        if (pass && shaded.a < u_pt_ref) pass = false;
        if (pass && !zwd) imageStore(img_depth, p, vec4(z));
    } else if (pass && !zwd) {
        imageStore(img_depth, p, vec4(z));
    }
    if (pass) {
        if (list != 2u) imageStore(img_prim, p, uvec4(pack(shaded)));
        else blend_px(p, shaded, tsp);
    }
    endInvocationInterlockARB();
}
)";

// The auto-sort pass: one fragment a pixel, its layers blended farthest first
const char *kResolveVertex = R"(#version 430 core
flat out uint v_tri;
void main() {
    uint k = uint(gl_VertexID);
    v_tri = 0u;
    gl_Position = vec4(k == 1u ? 3.0 : -1.0, k == 2u ? 3.0 : -1.0, 0.0, 1.0);
}
)";
const char *kResolveMain = R"(
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    uint n = imageLoad(img_count, p).r;
    uint base = (uint(p.y) * u_stride + uint(p.x)) * LAYERS;
    for (uint i = 0u; i < n; i++) blend_px(p, unpack(layers[base + i].y), layers[base + i].z);
}
)";

struct GlState {
    SDL_Window *win = nullptr;
    SDL_GLContext ctx = nullptr;
    GLuint prog = 0, resolve = 0, vao = 0, fbo = 0;
    GLuint tri_buf = 0, vram_buf = 0, reg_buf = 0, layer_buf = 0;
    GLuint tex[4] = {0, 0, 0, 0};       // primary, secondary, depth, auto-sort layer count
    GLint u_stride = -1, u_stride_resolve = -1;
    int w = 0, h = 0;
    size_t tri_cap = 0;
    std::vector<uint8_t> shadow;   // PVR memory as last uploaded
    bool uploaded = false;
    GLint u_screen, u_vram_mask, u_half, u_pt_ref, u_pal_fmt, u_text_stride;
    GLint u_fog_col_ram, u_fog_col_vert, u_fog_density, u_clamp_min, u_clamp_max, u_edge_margin;
    std::vector<GlTri> tris;
    std::vector<GlPrim> prims;
    std::unordered_map<uint64_t, uint32_t> prim_index;
    std::unordered_map<uint64_t, uint16_t> copies;              // of one tile's list
    std::vector<std::pair<uint32_t, uint32_t>> prim_tiles;     // (polygon, tile) for the ones in no rectangle
    std::vector<uint32_t> readback;
    double bbpix = 0;
};

GlState *G;
int g_mode = -1; // -1 not decided, 0 software, 1 GPU

void report_once(const char *what) {
    static std::vector<const char *> seen;
    for (const char *s : seen)
        if (s == what) return;
    seen.push_back(what);
    std::fprintf(stderr, "pvr: gpu: %s, that render on the software tiles\n", what);
}

GLuint compile(GLenum type, const char *src) {
    GLuint s = gl.gl_CreateShader(type);
    gl.gl_ShaderSource(s, 1, &src, nullptr);
    gl.gl_CompileShader(s);
    GLint ok = 0;
    gl.gl_GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        gl.gl_GetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "pvr: gpu: shader: %s\n", log);
        gl.gl_DeleteShader(s);
        return 0;
    }
    return s;
}

GLuint link(GLuint vs, GLuint fs) {
    GLuint prog = gl.gl_CreateProgram();
    gl.gl_AttachShader(prog, vs);
    gl.gl_AttachShader(prog, fs);
    gl.gl_LinkProgram(prog);
    GLint ok = 0;
    gl.gl_GetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        gl.gl_GetProgramInfoLog(prog, sizeof log, nullptr, log);
        std::fprintf(stderr, "pvr: gpu: link: %s\n", log);
        return 0;
    }
    return prog;
}

bool gl_init() {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    G = new GlState();
    G->win = SDL_CreateWindow("pvr", 16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (G->win)
        G->ctx = SDL_GL_CreateContext(G->win);
    SDL_GL_ResetAttributes();
    if (!G->ctx) {
        std::fprintf(stderr, "pvr: gpu: no OpenGL 4.4 context (%s): the software tiles\n", SDL_GetError());
        return false;
    }
#define X(name, ret, args) \
    gl.gl_##name = reinterpret_cast<ret (APIENTRY *) args>(SDL_GL_GetProcAddress("gl" #name)); \
    if (!gl.gl_##name) { std::fprintf(stderr, "pvr: gpu: no gl" #name ": the software tiles\n"); return false; }
    PVR_GL_FUNCS(X)
#undef X
    GLuint vs = compile(GL_VERTEX_SHADER, kVertex);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFragment);
    // the resolve: the same helpers, its own main, no interlock (a fragment a pixel)
    std::string rsrc = kFragment;
    rsrc.erase(rsrc.find("#extension"), rsrc.find("struct Tri") - rsrc.find("#extension"));
    rsrc.erase(rsrc.find("void main() {"));
    rsrc += kResolveMain;
    GLuint rvs = compile(GL_VERTEX_SHADER, kResolveVertex);
    GLuint rfs = compile(GL_FRAGMENT_SHADER, rsrc.c_str());
    if (!vs || !fs || !rvs || !rfs) {
        std::fprintf(stderr, "pvr: gpu: the shaders do not compile on %s: the software tiles\n",
                     reinterpret_cast<const char *>(gl.gl_GetString(GL_RENDERER)));
        return false;
    }
    G->prog = link(vs, fs);
    G->resolve = link(rvs, rfs);
    if (!G->prog || !G->resolve) return false;
    G->u_stride = gl.gl_GetUniformLocation(G->prog, "u_stride");
    G->u_stride_resolve = gl.gl_GetUniformLocation(G->resolve, "u_stride");
#define U(n) G->n = gl.gl_GetUniformLocation(G->prog, #n);
    U(u_screen) U(u_vram_mask) U(u_half) U(u_pt_ref) U(u_pal_fmt) U(u_text_stride)
    U(u_fog_col_ram) U(u_fog_col_vert) U(u_fog_density) U(u_clamp_min) U(u_clamp_max) U(u_edge_margin)
#undef U
    gl.gl_GenVertexArrays(1, &G->vao);
    gl.gl_GenFramebuffers(1, &G->fbo);
    GLuint b[4];
    gl.gl_GenBuffers(4, b);
    G->tri_buf = b[0];
    G->vram_buf = b[1];
    G->reg_buf = b[2];
    G->layer_buf = b[3];
    std::fprintf(stderr, "pvr: gpu: %s\n", reinterpret_cast<const char *>(gl.gl_GetString(GL_RENDERER)));
    return true;
}

// Uploads the parts of PVR memory that changed since the last render
void upload_vram(const uint8_t *vram, size_t size) {
    gl.gl_BindBuffer(GL_SHADER_STORAGE_BUFFER, G->vram_buf);
    if (!G->uploaded) {
        gl.gl_BufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(size), vram, GL_DYNAMIC_DRAW);
        G->shadow.assign(vram, vram + size);
        G->uploaded = true;
        return;
    }
    const size_t page = 4096;
    for (size_t off = 0; off < size; ) {
        if (std::memcmp(&G->shadow[off], vram + off, page) == 0) {
            off += page;
            continue;
        }
        size_t end = off + page;
        while (end < size && std::memcmp(&G->shadow[end], vram + end, page) != 0)
            end += page;
        gl.gl_BufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(off),
                            static_cast<GLsizeiptr>(end - off), vram + off);
        std::memcpy(&G->shadow[off], vram + off, end - off);
        off = end;
    }
}

void size_images(int w, int h) {
    if (w <= G->w && h <= G->h) return;
    w = std::max(w, G->w);
    h = std::max(h, G->h);
    if (G->tex[0]) gl.gl_DeleteTextures(4, G->tex);
    gl.gl_GenTextures(4, G->tex);
    for (int i = 0; i < 4; i++) {
        gl.gl_BindTexture(GL_TEXTURE_2D, G->tex[i]);
        gl.gl_TexStorage2D(GL_TEXTURE_2D, 1, i == 2 ? GL_R32F : GL_R32UI, w, h);
    }
    G->w = w;
    G->h = h;
    G->readback.resize(static_cast<size_t>(w) * h);
    gl.gl_BindBuffer(GL_SHADER_STORAGE_BUFFER, G->layer_buf);
    gl.gl_BufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(w) * h * 8 * 16, nullptr, GL_DYNAMIC_DRAW);
}

float bits_f(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }

} // namespace

// ----------------------------------------------------------------------------
// The polygons of one render, then the triangles the shader takes
// ----------------------------------------------------------------------------

bool CORE::render_gl(const std::vector<RegionArrayEntry> &tiles) {
    if (g_mode < 0) {
        const char *e = std::getenv("BT3_PVR_RENDER");
        g_mode = (e && std::strcmp(e, "soft") == 0) ? 0 : 1;
        if (g_mode) {
            SDL_Window *pw = SDL_GL_GetCurrentWindow();
            SDL_GLContext pc = SDL_GL_GetCurrentContext();
            if (!gl_init()) g_mode = 0;
            if (pc) SDL_GL_MakeCurrent(pw, pc);
        }
    }
    if (!g_mode) return false;

    // ---- the tiles: all cleared, flushed or not, pre-sort, no modifier volumes ----
    int max_tx = 0, max_ty = 0;
    for (const RegionArrayEntry &r : tiles) {
        if (r.z_clear) { report_once("a tile keeps its buffers (z_clear)"); return false; }
        max_tx = std::max(max_tx, static_cast<int>(r.tile_x));
        max_ty = std::max(max_ty, static_cast<int>(r.tile_y));
    }
    const int gw = max_tx + 1, gh = max_ty + 1;
    std::vector<uint8_t> seen(static_cast<size_t>(gw) * gh, 0);
    for (const RegionArrayEntry &r : tiles) {
        uint8_t &s = seen[r.tile_y * gw + r.tile_x];
        if (s) { report_once("a tile in the region array twice"); return false; }
        s = 1;
    }

    static const bool timing = std::getenv("PVR_GL_TIME") != nullptr;
    static double tsum[6];
    static int tn;
    uint64_t t0 = SDL_GetTicksNS();
    const uint32_t param_base = regs_->read(RegisterName::PARAM_BASE);
    G->prims.clear();
    G->prim_index.clear();
    G->prim_tiles.clear();

    // A block the tile's list holds twice is drawn twice there (after a parameter overflow every
    // later polygon is the one block the TA stopped at)
    auto add = [&](int phase, uint8_t list, uint32_t addr, uint8_t kind, uint8_t skip, uint8_t idx, int tx, int ty) {
        uint64_t key = (static_cast<uint64_t>(phase) << 48) | (static_cast<uint64_t>(addr & 0xFFFFFFu) << 20) |
                       (static_cast<uint64_t>(idx) << 16);
        uint16_t &copy = G->copies[key];
        key |= copy;
        if (copy < 0xFFFF) copy++;
        auto it = G->prim_index.find(key);
        if (it == G->prim_index.end()) {
            uint32_t id = static_cast<uint32_t>(G->prims.size());
            G->prim_index.emplace(key, id);
            G->prims.push_back(GlPrim{key, addr, list, kind, skip, idx, tx, ty, tx, ty, 1, id});
            G->prim_tiles.emplace_back(id, static_cast<uint32_t>(ty << 8 | tx));
            return;
        }
        G->prim_tiles.emplace_back(it->second, static_cast<uint32_t>(ty << 8 | tx));
        GlPrim &p = G->prims[it->second];
        p.tx0 = std::min(p.tx0, tx); p.ty0 = std::min(p.ty0, ty);
        p.tx1 = std::max(p.tx1, tx); p.ty1 = std::max(p.ty1, ty);
        p.tiles++;
    };

    // process_list's walk, collecting instead of drawing
    auto walk = [&](const RegionArrayEntry &r, ListType lt, int phase) -> bool {
        const RegionListPointer &lp = r.list_pointers[static_cast<int>(lt)];
        if (lp.empty) return true;
        uint32_t addr = lp.byte_address();
        unsigned links = 0, objects = 0;
        for (;;) {
            uint32_t word = vram_read32(addr);
            if (word == 0) break;
            ObjectPointer op = ObjectPointer::parse(word);
            if (op.type == ObjectPointer::Type::PointerBlockLink) {
                if (op.end_of_list) break;
                uint32_t next = op.next_pointer * 4;
                if (next == addr || ++links > 4096) { report_once("an object list that loops"); return false; }
                addr = next;
                continue;
            }
            if (lt == ListType::OpaqueModifierVolume) { report_once("modifier volumes"); return false; }
            const uint32_t pa = param_base + op.start_address * 4;
            const uint8_t l = static_cast<uint8_t>(lt);
            if (op.type == ObjectPointer::Type::TriangleStrip) {
                for (int i = 0; i < 6; i++)
                    if (op.mask & (1 << (5 - i))) add(phase, l, pa, 0, op.skip, i, r.tile_x, r.tile_y);
            } else if (op.type == ObjectPointer::Type::TriangleArray) {
                uint32_t stride = 12u + 3u * static_cast<uint32_t>(op.skip + 3) * 4u;
                for (int i = 0; i <= op.num_primitives; i++)
                    add(phase, l, pa + i * stride, 1, op.skip, 0, r.tile_x, r.tile_y);
            } else if (op.type == ObjectPointer::Type::QuadArray) {
                for (int i = 0; i <= op.num_primitives; i++) add(phase, l, pa, 2, op.skip, i, r.tile_x, r.tile_y);
            }
            addr += 4;
            if (++objects > 65536) { report_once("over 65536 objects in a tile"); return false; }
        }
        return true;
    };

    bool auto_sort = false;     // the translucent list is sorted here (all tiles alike)
    for (const RegionArrayEntry &r : tiles) {
        G->copies.clear();
        if (!walk(r, ListType::Opaque, 0) || !walk(r, ListType::PunchThrough, 1) ||
            !walk(r, ListType::OpaqueModifierVolume, 0))
            return false;
        if (!r.list_pointers[static_cast<int>(ListType::Translucent)].empty && !r.pre_sort) auto_sort = true;
        if (!walk(r, ListType::Translucent, 2)) return false;
    }
    std::sort(G->prims.begin(), G->prims.end(), [](const GlPrim &a, const GlPrim &b) { return a.key < b.key; });
    // a polygon whose tiles are no rectangle (user clip outside, the copies after an overflow) is drawn tile by tile
    bool any_split = false;
    for (const GlPrim &p : G->prims)
        any_split |= (p.tx1 - p.tx0 + 1) * (p.ty1 - p.ty0 + 1) != p.tiles;
    if (any_split)
        std::sort(G->prim_tiles.begin(), G->prim_tiles.end());

    uint64_t t1 = SDL_GetTicksNS();
    // ---- the triangles, as rasterize_triangle sets them up ----
    const float cull_val = bits_f(regs_->read(RegisterName::FPU_CULL_VAL));
    static const bool no_mip = std::getenv("PVR_NOMIP") != nullptr;
    G->tris.clear();

    auto tri = [&](ListType lt, const ISPTSPInstructionWord &isp, uint32_t isp_w, uint32_t tsp_w, uint32_t tcw_w,
                   const TSPInstructionWord &tsp, const TextureControlWord &tcw,
                   const Vertex &v0, const Vertex &v1, const Vertex &v2, int tx0, int ty0, int tx1, int ty1) {
        float min_x = std::min({v0.x, v1.x, v2.x}), max_x = std::max({v0.x, v1.x, v2.x});
        float min_y = std::min({v0.y, v1.y, v2.y}), max_y = std::max({v0.y, v1.y, v2.y});
        if (!(min_x == min_x) || !(max_x == max_x) || !(min_y == min_y) || !(max_y == max_y)) return;
        auto fl = [](float f) { return static_cast<int>(std::floor(std::max(-1.0e6f, std::min(f, 1.0e6f)))); };
        int ix0 = std::max(tx0 * 32, fl(min_x)), ix1 = std::min(tx1 * 32 + 31, fl(max_x));
        int iy0 = std::max(ty0 * 32, fl(min_y)), iy1 = std::min(ty1 * 32 + 31, fl(max_y));
        if (ix0 > ix1 || iy0 > iy1) return;
        float area = edge_function(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y);
        if (std::abs(area) < 1e-6f) return;
        const float aa = std::fabs(area);
        switch (isp.culling_mode) {
        case CullingMode::NoCulling: break;
        case CullingMode::CullIfSmall: if (aa < cull_val) return; break;
        case CullingMode::CullIfNegative: if (area < 0.0f || aa < cull_val) return; break;
        case CullingMode::CullIfPositive: if (area > 0.0f || aa < cull_val) return; break;
        }
        auto fill = [area](float ax, float ay, float bx, float by) {
            float dx = bx - ax, dy = by - ay;
            if (area < 0.0f) { dx = -dx; dy = -dy; }
            if (dy == 0.0f && dx < 0.0f) return true;
            return dy < 0.0f;
        };
        GlTri t;
        t.x0 = v0.x; t.y0 = v0.y; t.x1 = v1.x; t.y1 = v1.y; t.x2 = v2.x; t.y2 = v2.y;
        t.inv_area = 1.0f / area;
        t.lod = (isp.texture && tcw.mip_mapped && !tcw.scan_order && !no_mip) ? compute_mip_lod(tsp, tcw, v0, v1, v2) : 0.0f;
        t.z0 = v0.z; t.z1 = v1.z; t.z2 = v2.z; t.pad0 = 0;
        t.u0 = v0.u; t.u1 = v1.u; t.u2 = v2.u; t.pad1 = 0;
        t.v0 = v0.v; t.v1 = v1.v; t.v2 = v2.v; t.pad2 = 0;
        t.c0 = v0.base_color; t.c1 = v1.base_color; t.c2 = v2.base_color; t.isp = isp_w;
        t.o0 = v0.offset_color; t.o1 = v1.offset_color; t.o2 = v2.offset_color; t.tsp = tsp_w;
        t.tcw = tcw_w;
        t.flags = static_cast<uint32_t>(lt) | (auto_sort && lt == ListType::Translucent ? 64u : 0u) | (fill(v1.x, v1.y, v2.x, v2.y) ? 8u : 0u) |
                  (fill(v2.x, v2.y, v0.x, v0.y) ? 16u : 0u) | (fill(v0.x, v0.y, v1.x, v1.y) ? 32u : 0u);
        t.bb0 = static_cast<uint32_t>(ix0) | (static_cast<uint32_t>(iy0) << 16);
        t.bb1 = static_cast<uint32_t>(ix1) | (static_cast<uint32_t>(iy1) << 16);
        G->tris.push_back(t);
        G->bbpix += static_cast<double>(ix1 - ix0 + 1) * (iy1 - iy0 + 1);
    };

    // the background, as draw_background has it, over every tile
    {
        uint32_t tag_offset = regs_->isp_backgnd_tag_offset();
        uint32_t skip = regs_->isp_backgnd_skip();
        uint32_t pa = param_base + regs_->isp_backgnd_tag_address() * 4;
        float bg_depth = bits_f(regs_->read(RegisterName::ISP_BACKGND_D));
        ISPTSPInstructionWord isp;
        TSPInstructionWord tsp;
        TextureControlWord tcw;
        // read_isp_tsp_params reads the vertices from the block's start: move it so they start at tag_offset
        std::vector<Vertex> v;
        read_isp_tsp_params(pa, static_cast<int>(skip), static_cast<int>(tag_offset) + 3, isp, tsp, tcw, v);
        Vertex b[3] = {v[tag_offset], v[tag_offset + 1], v[tag_offset + 2]};
        for (Vertex &x : b) x.z = bg_depth;
        Vertex d = b[0];
        d.x = b[1].x + b[2].x - b[0].x;
        d.y = b[1].y + b[2].y - b[0].y;
        d.u = b[1].u + b[2].u - b[0].u;
        d.v = b[1].v + b[2].v - b[0].v;
        auto ext = [](uint32_t a, uint32_t bb, uint32_t c) {
            uint32_t out = 0;
            for (int s = 0; s < 32; s += 8) {
                int o = static_cast<int>((bb >> s) & 255) + static_cast<int>((c >> s) & 255) - static_cast<int>((a >> s) & 255);
                out |= static_cast<uint32_t>(std::max(0, std::min(255, o))) << s;
            }
            return out;
        };
        d.base_color = ext(b[0].base_color, b[1].base_color, b[2].base_color);
        d.offset_color = ext(b[0].offset_color, b[1].offset_color, b[2].offset_color);
        uint32_t isp_w = vram_read32(pa);
        isp_w = (isp_w & ~(0xE0000000u | (1u << 26))) | (7u << 29); // Always, Z written
        ISPTSPInstructionWord bisp = ISPTSPInstructionWord::parse(isp_w);
        uint32_t tsp_w = vram_read32(pa + 4), tcw_w = isp.texture ? vram_read32(pa + 8) : 0u;
        tri(ListType::Opaque, bisp, isp_w, tsp_w, tcw_w, tsp, tcw, b[0], b[1], d, 0, 0, max_tx, max_ty);
        tri(ListType::Opaque, bisp, isp_w, tsp_w, tcw_w, tsp, tcw, b[0], d, b[2], 0, 0, max_tx, max_ty);
    }

    {
        std::vector<Vertex> v;
        ISPTSPInstructionWord isp;
        TSPInstructionWord tsp;
        TextureControlWord tcw;
        uint32_t isp_w = 0, tsp_w = 0, tcw_w = 0, have_addr = 0xFFFFFFFFu;
        int have_n = 0;
        for (size_t i = 0; i < G->prims.size(); i++) {
            const GlPrim &p = G->prims[i];
            ListType lt = static_cast<ListType>(p.list);
            int need = p.kind == 0 ? p.idx + 3 : p.kind == 1 ? 3 : (p.idx + 1) * 4;
            if (p.addr != have_addr || need > have_n) {
                // the most vertices the polygons at this address take
                int n = need;
                for (size_t j = i + 1; j < G->prims.size() && G->prims[j].addr == p.addr && G->prims[j].list == p.list; j++) {
                    const GlPrim &q = G->prims[j];
                    n = std::max(n, q.kind == 0 ? q.idx + 3 : q.kind == 1 ? 3 : (q.idx + 1) * 4);
                }
                read_isp_tsp_params(p.addr, p.skip, n, isp, tsp, tcw, v);
                isp_w = vram_read32(p.addr);
                tsp_w = vram_read32(p.addr + 4);
                tcw_w = isp.texture ? vram_read32(p.addr + 8) : 0u;
                have_addr = p.addr;
                have_n = n;
            }
            auto emit = [&](int rx0, int ry0, int rx1, int ry1) {
                if (p.kind == 0) {
                    int k = p.idx;
                    if ((k & 1) == 0) tri(lt, isp, isp_w, tsp_w, tcw_w, tsp, tcw, v[k], v[k + 1], v[k + 2], rx0, ry0, rx1, ry1);
                    else tri(lt, isp, isp_w, tsp_w, tcw_w, tsp, tcw, v[k + 1], v[k], v[k + 2], rx0, ry0, rx1, ry1);
                } else if (p.kind == 1) {
                    tri(lt, isp, isp_w, tsp_w, tcw_w, tsp, tcw, v[0], v[1], v[2], rx0, ry0, rx1, ry1);
                } else {
                    const Vertex &a = v[p.idx * 4], &b = v[p.idx * 4 + 1], &c = v[p.idx * 4 + 2];
                    Vertex d = v[p.idx * 4 + 3];
                    float e1x = b.x - a.x, e1y = b.y - a.y, e2x = c.x - a.x, e2y = c.y - a.y;
                    float det = e1x * e2y - e1y * e2x;
                    if (std::abs(det) > 1e-12f) {
                        float dx = d.x - a.x, dy = d.y - a.y;
                        float qa = (dx * e2y - dy * e2x) / det;
                        float qb = (e1x * dy - e1y * dx) / det;
                        d.z = a.z + qa * (b.z - a.z) + qb * (c.z - a.z);
                    }
                    tri(lt, isp, isp_w, tsp_w, tcw_w, tsp, tcw, a, b, c, rx0, ry0, rx1, ry1);
                    tri(lt, isp, isp_w, tsp_w, tcw_w, tsp, tcw, a, c, d, rx0, ry0, rx1, ry1);
                }

            };
            if ((p.tx1 - p.tx0 + 1) * (p.ty1 - p.ty0 + 1) == p.tiles) {
                emit(p.tx0, p.ty0, p.tx1, p.ty1);
            } else {
                auto r = std::lower_bound(G->prim_tiles.begin(), G->prim_tiles.end(), std::make_pair(p.id, 0u));
                for (; r != G->prim_tiles.end() && r->first == p.id; ++r) {
                    int tx = static_cast<int>(r->second & 0xFF), ty = static_cast<int>(r->second >> 8);
                    emit(tx, ty, tx, ty);
                }
            }
        }
    }

    // ---- the draw ----
    SDL_Window *prev_win = SDL_GL_GetCurrentWindow();
    SDL_GLContext prev_ctx = SDL_GL_GetCurrentContext();
    SDL_GL_MakeCurrent(G->win, G->ctx);

    const int w = gw * 32, h = gh * 32;
    size_images(w, h);
    uint64_t t2 = SDL_GetTicksNS();
    upload_vram(vram_, vram_size_);
    uint64_t t3 = SDL_GetTicksNS();

    uint32_t regs[1024 + 128];
    for (int i = 0; i < 1024; i++) regs[i] = regs_->palette_ram(i);
    for (int i = 0; i < 128; i++) regs[1024 + i] = regs_->fog_table(i);
    gl.gl_BindBuffer(GL_SHADER_STORAGE_BUFFER, G->reg_buf);
    gl.gl_BufferData(GL_SHADER_STORAGE_BUFFER, sizeof regs, regs, GL_DYNAMIC_DRAW);
    gl.gl_BindBuffer(GL_SHADER_STORAGE_BUFFER, G->tri_buf);
    gl.gl_BufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(std::max<size_t>(1, G->tris.size()) * sizeof(GlTri)),
                     G->tris.empty() ? nullptr : G->tris.data(), GL_STREAM_DRAW);

    const uint32_t zero = 0;
    for (int i = 0; i < 4; i++)
        gl.gl_ClearTexImage(G->tex[i], 0, i == 2 ? GL_RED : GL_RED_INTEGER, i == 2 ? GL_FLOAT : GL_UNSIGNED_INT, &zero);

    gl.gl_BindFramebuffer(GL_FRAMEBUFFER, G->fbo);
    gl.gl_FramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_WIDTH, G->w);
    gl.gl_FramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_HEIGHT, G->h);
    gl.gl_Viewport(0, 0, G->w, G->h);
    gl.gl_Disable(GL_DEPTH_TEST);
    gl.gl_Disable(GL_CULL_FACE);
    gl.gl_Disable(GL_BLEND);
    gl.gl_Disable(GL_SCISSOR_TEST);
    gl.gl_UseProgram(G->prog);
    gl.gl_BindVertexArray(G->vao);
    gl.gl_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, G->tri_buf);
    gl.gl_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, G->vram_buf);
    gl.gl_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, G->reg_buf);
    gl.gl_BindImageTexture(0, G->tex[0], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    gl.gl_BindImageTexture(1, G->tex[1], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    gl.gl_BindImageTexture(2, G->tex[2], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
    gl.gl_BindImageTexture(3, G->tex[3], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    gl.gl_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, G->layer_buf);
    gl.gl_Uniform1ui(G->u_stride, static_cast<GLuint>(G->w));

    static float edge = -1.0f;
    if (edge < 0.0f) {
        const char *e = std::getenv("PVR_EDGE");
        edge = e ? static_cast<float>(std::atof(e)) : 1.0f / 64;
    }
    gl.gl_Uniform2f(G->u_screen, static_cast<float>(G->w), static_cast<float>(G->h));
    gl.gl_Uniform1ui(G->u_vram_mask, static_cast<GLuint>(vram_size_ - 1));
    gl.gl_Uniform1ui(G->u_half, regs_->read(RegisterName::HALF_OFFSET));
    gl.gl_Uniform1ui(G->u_pt_ref, regs_->read(RegisterName::PT_ALPHA_REF) & 0xFF);
    gl.gl_Uniform1ui(G->u_pal_fmt, regs_->palette_format());
    gl.gl_Uniform1ui(G->u_text_stride, regs_->text_stride());
    gl.gl_Uniform1ui(G->u_fog_col_ram, regs_->read(RegisterName::FOG_COL_RAM));
    gl.gl_Uniform1ui(G->u_fog_col_vert, regs_->read(RegisterName::FOG_COL_VERT));
    gl.gl_Uniform1ui(G->u_fog_density, regs_->read(RegisterName::FOG_DENSITY));
    gl.gl_Uniform1ui(G->u_clamp_min, regs_->read(RegisterName::FOG_CLAMP_MIN));
    gl.gl_Uniform1ui(G->u_clamp_max, regs_->read(RegisterName::FOG_CLAMP_MAX));
    gl.gl_Uniform1f(G->u_edge_margin, edge);

    if (!G->tris.empty())
        gl.gl_DrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(G->tris.size() * 6));
    if (auto_sort) {
        gl.gl_MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
        gl.gl_UseProgram(G->resolve);
        gl.gl_Uniform1ui(G->u_stride_resolve, static_cast<GLuint>(G->w));
        gl.gl_DrawArrays(GL_TRIANGLES, 0, 3);
    }

    if (timing) gl.gl_Finish();
    uint64_t t4 = SDL_GetTicksNS();
    gl.gl_MemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    gl.gl_BindTexture(GL_TEXTURE_2D, G->tex[0]);
    gl.gl_PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl.gl_GetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, G->readback.data());

    uint64_t t5 = SDL_GetTicksNS();
    GLenum err = gl.gl_GetError();
    if (prev_ctx) SDL_GL_MakeCurrent(prev_win, prev_ctx);
    if (err != GL_NO_ERROR) {
        static int n;
        if (n++ < 5) std::fprintf(stderr, "pvr: gpu: GL error 0x%x\n", err);
    }

    // ---- the accumulation buffer to the frame buffer, tile by tile ----
    static TileContext ctx;
    for (const RegionArrayEntry &r : tiles) {
        if (r.flush_accumulate) continue;
        for (int ly = 0; ly < static_cast<int>(TILE_SIZE); ly++) {
            const uint32_t *row = &G->readback[static_cast<size_t>(r.tile_y * 32 + ly) * G->w + r.tile_x * 32];
            for (int lx = 0; lx < static_cast<int>(TILE_SIZE); lx++)
                ctx.buf.primary[ly][lx] = Pixel::from_packed(row[lx]);
        }
        flush_tile_to_framebuffer(ctx, r.tile_x, r.tile_y);
    }
    if (timing) {
        uint64_t t6 = SDL_GetTicksNS();
        tsum[0] += (t1 - t0) / 1e6; tsum[1] += (t2 - t1) / 1e6; tsum[2] += (t3 - t2) / 1e6;
        tsum[3] += (t4 - t3) / 1e6; tsum[4] += (t5 - t4) / 1e6; tsum[5] += (t6 - t5) / 1e6;
        if (++tn == 100) {
            std::fprintf(stderr, "pvr: gpu ms/render: lists %.2f tris %.2f upload %.2f submit %.2f draw+read %.2f flush %.2f (%zu tris, %.0f bbox pixels)\n",
                         tsum[0] / tn, tsum[1] / tn, tsum[2] / tn, tsum[3] / tn, tsum[4] / tn, tsum[5] / tn, G->tris.size(), G->bbpix / tn);
            G->bbpix = 0;
            tn = 0;
            std::memset(tsum, 0, sizeof tsum);
        }
    }
    return true;
}

} // namespace pvr
