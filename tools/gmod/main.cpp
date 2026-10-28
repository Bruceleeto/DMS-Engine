/* gmod2glb: Garry's Mod models (and the HL2 / CS:S content it mounts, and
 * workshop addons) to .glb, which strippy makes .dms of.
 *
 *   gmod2glb --list [text]             the models whose path has text in it
 *   gmod2glb --anims model.mdl         its animations (its own and $includemodel's)
 *   gmod2glb model.mdl -o out.glb [--tex 256] [--anims name,name | --anims all]
 *   gmod2glb --anims-only model.mdl -o out.glb --anims ...   the skeleton and
 *            animations alone, for models that share them
 *
 * Paths are as the game has them (models/player/kleiner.mdl). The game is
 * found through Steam, or --gmod <GarrysMod folder>. */
#include "material.h"
#include "mdl.h"
#include "vfs.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <set>
#include <zlib.h>

static const float INCH = 0.0254f;

/* Source (Z up, X forward) to glTF (Y up, Z forward), as a rotation */
static void to_gl(const Vec3& s, float* o, float scale) { o[0] = s.y * scale; o[1] = s.z * scale; o[2] = s.x * scale; }
static void to_gl(const Quat& q, float* o) { o[0] = q.y; o[1] = q.z; o[2] = q.x; o[3] = q.w; }
static const int PERM[3] = { 1, 2, 0 };    /* gl axis i is Source axis PERM[i] */

/* ================================================================
 * PNG (zlib)
 * ================================================================ */

static void be32(std::vector<uint8_t>& o, uint32_t v) { for (int s = 24; s >= 0; s -= 8) o.push_back((uint8_t)(v >> s)); }

static void chunk(std::vector<uint8_t>& o, const char* type, const std::vector<uint8_t>& data) {
    be32(o, (uint32_t)data.size());
    size_t at = o.size();
    o.insert(o.end(), type, type + 4);
    o.insert(o.end(), data.begin(), data.end());
    be32(o, (uint32_t)crc32(0, &o[at], (uInt)(o.size() - at)));
}

static std::vector<uint8_t> png(const Image& im, bool alpha) {
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

static std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o;
}

static std::string join(const std::vector<std::string>& v) {
    std::string o;
    for (size_t i = 0; i < v.size(); i++) o += (i ? "," : "") + v[i];
    return o;
}

static bool write_glb(const std::string& path, const std::string& json_in, std::vector<uint8_t> bin) {
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

/* ================================================================
 * Animations from the model and what it includes, on its bones by name
 * ================================================================ */

/* An animation and the model it is in (kept once, not per animation: the
 * shared ones are tens of MB) */
struct AnimSource { const Mdl* mdl; std::string name; };

static void gather_anims(const Vfs& vfs, const Mdl& m, std::vector<AnimSource>& out, std::set<std::string>& seen,
                         std::list<Mdl>& keep) {
    if (!seen.insert(m.path).second) return;
    std::vector<bool> delta;
    std::vector<std::string> names = mdl_anim_names(m, &delta);
    for (size_t i = 0; i < names.size(); i++)
        if (!delta[i]) out.push_back({ &m, names[i] });
    for (const std::string& inc : m.includes) {
        keep.emplace_back();
        if (mdl_load_skeleton(vfs, inc, keep.back())) gather_anims(vfs, keep.back(), out, seen, keep);
        else keep.pop_back();
    }
}

static bool wanted(const std::string& name, const std::vector<std::string>& want) {
    for (const std::string& w : want) {
        if (w == "all" || w == name) return true;
        /* name* matches a prefix */
        if (!w.empty() && w.back() == '*' && !name.compare(0, w.size() - 1, w, 0, w.size() - 1)) return true;
    }
    return false;
}

/* ================================================================
 * Conversion
 * ================================================================ */

struct Options {
    int tex = 256;
    std::vector<std::string> anims;
    bool anims_only = false;
    MdlPick pick;
};

/* A material's base texture, its biggest level no wider than max; full_w,
 * full_h its size in the game */
static bool texture_of(const Vfs& vfs, const std::string& vmt, int max, Material& mat, Image& im,
                       std::string& err, int* full_w = nullptr, int* full_h = nullptr) {
    if (!vmt_load(vfs, vmt, mat)) { err = "material not found"; return false; }
    if (mat.basetexture.empty()) { err = "no $basetexture"; return false; }
    std::vector<uint8_t> vtf;
    std::string tpath = "materials/" + mat.basetexture + ".vtf";
    if (!vfs.read(tpath, vtf)) { err = tpath + " not found"; return false; }
    if (full_w && vtf.size() > 20) { *full_w = vtf[16] | vtf[17] << 8; *full_h = vtf[18] | vtf[19] << 8; }
    return vtf_decode(vtf, max, im, &err);
}

static bool convert(const Vfs& vfs, const std::string& path, const std::string& out, const Options& opt) {
    Mdl m;
    std::string why;
    bool ok = opt.anims_only ? mdl_load_skeleton(vfs, path, m, &why) : mdl_load(vfs, path, m, &why, opt.pick);
    if (!ok) { fprintf(stderr, "%s: %s\n", path.c_str(), why.c_str()); return false; }

    std::vector<AnimSource> all;
    std::set<std::string> seen;
    std::list<Mdl> keep;
    if (!opt.anims.empty()) gather_anims(vfs, m, all, seen, keep);
    std::vector<const AnimSource*> anims;
    std::set<std::string> taken;
    for (const AnimSource& a : all)
        if (wanted(a.name, opt.anims) && taken.insert(a.name).second) anims.push_back(&a);

    bool skinned = m.bones.size() > 1 || !anims.empty() || opt.anims_only;
    Glb g;
    std::vector<std::string> nodes, meshes, materials, textures, images, skins, anim_json;
    std::vector<int> scene_nodes;
    size_t nb = m.bones.size();

    /* Bones: nodes with their rest pose, children listed */
    if (skinned) {
        std::vector<std::vector<int>> kids(nb);
        for (size_t i = 0; i < nb; i++) if (m.bones[i].parent >= 0) kids[m.bones[i].parent].push_back((int)i);
        for (size_t i = 0; i < nb; i++) {
            float t[3], r[4];
            to_gl(m.bones[i].pos, t, INCH);
            to_gl(m.bones[i].quat, r);
            char b[256];
            snprintf(b, sizeof(b), "\"translation\":[%g,%g,%g],\"rotation\":[%g,%g,%g,%g]", t[0], t[1], t[2], r[0], r[1], r[2], r[3]);
            std::string n = "{\"name\":\"" + esc(m.bones[i].name) + "\"," + b;
            if (!kids[i].empty()) {
                n += ",\"children\":[";
                for (size_t k = 0; k < kids[i].size(); k++) n += (k ? "," : "") + std::to_string(kids[i][k]);
                n += "]";
            }
            nodes.push_back(n + "}");
            if (m.bones[i].parent < 0) scene_nodes.push_back((int)i);
        }
        /* Inverse binds: Source's own pose-to-bone, turned into glTF's space */
        std::vector<float> ibm(nb * 16);
        for (size_t i = 0; i < nb; i++) {
            const float* p = m.bones[i].pose_to_bone;
            float* o = &ibm[i * 16];
            memset(o, 0, 64);
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 3; c++) o[c * 4 + r] = p[PERM[r] * 4 + PERM[c]];
                o[12 + r] = p[PERM[r] * 4 + 3] * INCH;
            }
            o[15] = 1;
        }
        int acc = g.accessor(ibm.data(), nb, 5126, "MAT4");
        std::string joints;
        for (size_t i = 0; i < nb; i++) joints += (i ? "," : "") + std::to_string(i);
        skins.push_back("{\"joints\":[" + joints + "],\"inverseBindMatrices\":" + std::to_string(acc) + "}");
    }

    /* Materials and their textures */
    std::map<int, int> mat_index;
    if (!opt.anims_only)
        for (const MdlMesh& me : m.meshes) {
            if (mat_index.count(me.material)) continue;
            int mi = (int)materials.size();
            mat_index[me.material] = mi;
            std::string vmt = me.material < (int)m.materials.size() ? m.materials[me.material] : "";
            Material mat;
            std::string base = vmt.substr(vmt.find_last_of('/') + 1);
            if (base.size() > 4 && base.substr(base.size() - 4) == ".vmt") base.resize(base.size() - 4);
            std::string json = "{\"name\":\"" + esc(base) + "\"";
            Image im;
            std::string err;
            if (texture_of(vfs, vmt, opt.tex, mat, im, err)) {
                {
                    bool alpha = mat.alphatest || mat.translucent;
                    std::vector<uint8_t> p = png(im, alpha);
                    int v = g.view(p.data(), p.size());
                    images.push_back("{\"bufferView\":" + std::to_string(v) + ",\"mimeType\":\"image/png\",\"name\":\"" +
                                     esc(mat.basetexture.substr(mat.basetexture.find_last_of('/') + 1)) + "\"}");
                    textures.push_back("{\"source\":" + std::to_string(images.size() - 1) + "}");
                    json += ",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":" + std::to_string(textures.size() - 1) +
                            "},\"metallicFactor\":0,\"roughnessFactor\":1}";
                    if (mat.translucent) json += ",\"alphaMode\":\"BLEND\"";
                    else if (mat.alphatest) json += ",\"alphaMode\":\"MASK\"";
                }
            } else {
                fprintf(stderr, "  %s: %s\n", vmt.empty() ? base.c_str() : vmt.c_str(), err.c_str());
            }
            if (mat.nocull) json += ",\"doubleSided\":true";
            materials.push_back(json + "}");
        }

    /* The mesh: a primitive per material, each with only its own vertices */
    if (!opt.anims_only) {
        std::map<int, std::vector<uint32_t>> by_mat;
        for (const MdlMesh& me : m.meshes) {
            auto& v = by_mat[mat_index[me.material]];
            v.insert(v.end(), me.indices.begin(), me.indices.end());
        }
        std::vector<std::string> prims;
        for (auto& kv : by_mat) {
            std::map<uint32_t, uint32_t> remap;
            std::vector<uint32_t> idx;
            std::vector<const MdlVertex*> vs;
            for (uint32_t i : kv.second) {
                auto it = remap.find(i);
                if (it == remap.end()) { it = remap.emplace(i, (uint32_t)vs.size()).first; vs.push_back(&m.vertices[i]); }
                idx.push_back(it->second);
            }
            size_t nv = vs.size();
            std::vector<float> pos(nv * 3), nrm(nv * 3), uv(nv * 2), wts(nv * 4, 0);
            std::vector<uint16_t> jnt(nv * 4, 0);
            float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
            for (size_t i = 0; i < nv; i++) {
                const MdlVertex& v = *vs[i];
                to_gl(v.pos, &pos[i * 3], INCH);
                to_gl(v.normal, &nrm[i * 3], 1.0f);
                uv[i * 2] = v.u;
                uv[i * 2 + 1] = v.v;
                for (int k = 0; k < 3; k++) { mn[k] = std::min(mn[k], pos[i * 3 + k]); mx[k] = std::max(mx[k], pos[i * 3 + k]); }
                float sum = 0;
                for (int k = 0; k < v.bones && k < 3; k++) {
                    jnt[i * 4 + k] = v.bone[k];
                    wts[i * 4 + k] = v.weight[k];
                    sum += v.weight[k];
                }
                if (sum > 0) for (int k = 0; k < 4; k++) wts[i * 4 + k] /= sum;
                else wts[i * 4] = 1;
            }
            std::string attrs = "\"POSITION\":" + std::to_string(g.accessor(pos.data(), nv, 5126, "VEC3", 1, mn, mx, 3)) +
                                ",\"NORMAL\":" + std::to_string(g.accessor(nrm.data(), nv, 5126, "VEC3", 1)) +
                                ",\"TEXCOORD_0\":" + std::to_string(g.accessor(uv.data(), nv, 5126, "VEC2", 1));
            if (skinned)
                attrs += ",\"JOINTS_0\":" + std::to_string(g.accessor(jnt.data(), nv, 5123, "VEC4", 1)) +
                         ",\"WEIGHTS_0\":" + std::to_string(g.accessor(wts.data(), nv, 5126, "VEC4", 1));
            int ai = g.accessor(idx.data(), idx.size(), 5125, "SCALAR", 2);
            prims.push_back("{\"attributes\":{" + attrs + "},\"indices\":" + std::to_string(ai) +
                            ",\"material\":" + std::to_string(kv.first) + "}");
        }
        std::string name = m.path.substr(m.path.find_last_of('/') + 1);
        name = name.substr(0, name.size() - 4);
        meshes.push_back("{\"name\":\"" + esc(name) + "\",\"primitives\":[" + join(prims) + "]}");
        nodes.push_back("{\"name\":\"" + esc(name) + "\",\"mesh\":0" + (skinned ? ",\"skin\":0" : "") + "}");
        scene_nodes.push_back((int)nodes.size() - 1);
    } else {
        /* glTF wants a skin on something: a mesh-less model keeps its skeleton through the skin's joints */
    }

    /* Animations, by bone name onto this model's bones */
    std::map<std::string, int> bone_of;
    for (size_t i = 0; i < nb; i++) bone_of[m.bones[i].name] = (int)i;
    for (const AnimSource* a : anims) {
        MdlAnim an;
        if (!mdl_decode_anim(*a->mdl, a->name, an)) { fprintf(stderr, "  animation %s: cannot decode\n", a->name.c_str()); continue; }
        std::vector<float> times(an.frames);
        for (int f = 0; f < an.frames; f++) times[f] = f / an.fps;
        float tmin = 0, tmax = times.back();
        int at = g.accessor(times.data(), times.size(), 5126, "SCALAR", 0, &tmin, &tmax, 1);
        std::vector<std::string> samplers, channels;
        size_t anb = a->mdl->bones.size();
        for (size_t ab = 0; ab < anb; ab++) {
            auto it = bone_of.find(a->mdl->bones[ab].name);
            if (it == bone_of.end()) continue;
            std::vector<float> t(an.frames * 3), r(an.frames * 4);
            for (int f = 0; f < an.frames; f++) {
                to_gl(an.pos[(size_t)f * anb + ab], &t[f * 3], INCH);
                to_gl(an.rot[(size_t)f * anb + ab], &r[f * 4]);
                /* No flips between frames: the short way round */
                if (f && r[f * 4] * r[f * 4 - 4] + r[f * 4 + 1] * r[f * 4 - 3] + r[f * 4 + 2] * r[f * 4 - 2] + r[f * 4 + 3] * r[f * 4 - 1] < 0)
                    for (int k = 0; k < 4; k++) r[f * 4 + k] = -r[f * 4 + k];
            }
            int ot = g.accessor(t.data(), an.frames, 5126, "VEC3");
            int orr = g.accessor(r.data(), an.frames, 5126, "VEC4");
            int st = (int)samplers.size();
            samplers.push_back("{\"input\":" + std::to_string(at) + ",\"output\":" + std::to_string(ot) + ",\"interpolation\":\"LINEAR\"}");
            samplers.push_back("{\"input\":" + std::to_string(at) + ",\"output\":" + std::to_string(orr) + ",\"interpolation\":\"LINEAR\"}");
            channels.push_back("{\"sampler\":" + std::to_string(st) + ",\"target\":{\"node\":" + std::to_string(it->second) + ",\"path\":\"translation\"}}");
            channels.push_back("{\"sampler\":" + std::to_string(st + 1) + ",\"target\":{\"node\":" + std::to_string(it->second) + ",\"path\":\"rotation\"}}");
        }
        if (channels.empty()) continue;
        anim_json.push_back("{\"name\":\"" + esc(a->name) + "\",\"samplers\":[" + join(samplers) + "],\"channels\":[" + join(channels) + "]}");
        printf("  animation %-32s %4d frames  %.0f fps\n", a->name.c_str(), an.frames, an.fps);
    }

    std::string sn;
    for (size_t i = 0; i < scene_nodes.size(); i++) sn += (i ? "," : "") + std::to_string(scene_nodes[i]);
    std::string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"DMS gmod2glb\"},\"scene\":0,\"scenes\":[{\"nodes\":[" + sn + "]}]";
    json += ",\"nodes\":[" + join(nodes) + "]";
    if (!meshes.empty()) json += ",\"meshes\":[" + join(meshes) + "]";
    if (!materials.empty()) json += ",\"materials\":[" + join(materials) + "]";
    if (!textures.empty()) json += ",\"textures\":[" + join(textures) + "]";
    if (!images.empty()) json += ",\"images\":[" + join(images) + "]";
    if (!skins.empty()) json += ",\"skins\":[" + join(skins) + "]";
    if (!anim_json.empty()) json += ",\"animations\":[" + join(anim_json) + "]";
    json += ",\"accessors\":[" + join(g.accessors) + "],\"bufferViews\":[" + join(g.views) + "]";
    json += ",\"buffers\":[{\"byteLength\":" + std::to_string((g.bin.size() + 3) & ~(size_t)3) + "}]}";
    if (!write_glb(out, json, g.bin)) { fprintf(stderr, "cannot write %s\n", out.c_str()); return false; }

    size_t tris = 0;
    for (const MdlMesh& me : m.meshes) tris += me.indices.size() / 3;
    printf("%s: %zu bones, %zu vertices, %zu triangles, %zu materials, %zu animations -> %s\n", m.path.c_str(), nb,
           m.vertices.size(), tris, materials.size(), anim_json.size(), out.c_str());
    return true;
}

/* ================================================================
 * The model as the game has it, for the editor's browser: what it is
 * (lines on stdout) and, with a path, its triangles and textures as they
 * are (no skinning, no animation: its reference pose)
 *
 *   model <path> <where from>        counts <bones> <vertices> <triangles> <skins>
 *   body <part> <choice>...          mat <name> <vmt> <texture> <w> <h> <flags>
 *   anim <name> <in .mdl>            include <.mdl>
 *
 * The preview file, little-endian: "GMPV" u32 1; u32 materials, each u16 w,
 * u16 h, u8 flags (1 alphatest, 2 translucent, 4 two-sided, 8 additive),
 * w*h RGBA; u32 vertices, each float pos[3], normal[3], uv[2] (glTF's axes,
 * metres); u32 meshes, each u32 material, u32 indices, the indices
 * ================================================================ */

static bool info(const Vfs& vfs, const std::string& path, const Options& opt, const std::string& preview) {
    Mdl m;
    std::string why;
    if (!mdl_load(vfs, path, m, &why, opt.pick)) { fprintf(stderr, "%s: %s\n", path.c_str(), why.c_str()); return false; }
    const VfsEntry* e = vfs.find(path);
    size_t tris = 0;
    for (const MdlMesh& me : m.meshes) tris += me.indices.size() / 3;
    printf("model\t%s\t%s\n", m.path.c_str(), e ? vfs.sources[e->source].label.c_str() : "");
    printf("counts\t%zu\t%zu\t%zu\t%d\n", m.bones.size(), m.vertices.size(), tris, m.skins);
    for (const MdlBodyPart& bp : m.bodyparts) {
        printf("body\t%s", bp.name.c_str());
        for (const std::string& c : bp.models) printf("\t%s", c.empty() ? "(none)" : c.c_str());
        printf("\n");
    }

    std::vector<int> used;      /* materials the meshes use, in order */
    std::map<int, int> slot;
    for (const MdlMesh& me : m.meshes)
        if (!slot.count(me.material)) { slot[me.material] = (int)used.size(); used.push_back(me.material); }
    std::vector<uint8_t> out = { 'G', 'M', 'P', 'V', 1, 0, 0, 0 };
    auto u32 = [&](uint32_t v) { out.insert(out.end(), (uint8_t*)&v, (uint8_t*)&v + 4); };
    auto f32 = [&](float v) { out.insert(out.end(), (uint8_t*)&v, (uint8_t*)&v + 4); };
    u32((uint32_t)used.size());
    for (int mi : used) {
        std::string vmt = mi < (int)m.materials.size() ? m.materials[mi] : "";
        std::string base = vmt.substr(vmt.find_last_of('/') + 1);
        if (base.size() > 4 && !base.compare(base.size() - 4, 4, ".vmt")) base.resize(base.size() - 4);
        Material mat;
        Image im;
        std::string err;
        int fw = 0, fh = 0;
        bool ok = texture_of(vfs, vmt, opt.tex, mat, im, err, &fw, &fh);
        uint8_t flags = (mat.alphatest ? 1 : 0) | (mat.translucent ? 2 : 0) | (mat.nocull ? 4 : 0) | (mat.additive ? 8 : 0);
        std::string fl = std::string(mat.alphatest ? "alphatest " : "") + (mat.translucent ? "translucent " : "") +
                         (mat.nocull ? "two-sided " : "") + (mat.additive ? "additive " : "");
        if (!ok) fl = err;
        printf("mat\t%s\t%s\t%s\t%d\t%d\t%s\n", base.c_str(), vmt.c_str(), mat.basetexture.c_str(), fw, fh, fl.c_str());
        uint16_t w = ok ? (uint16_t)im.w : 0, h = ok ? (uint16_t)im.h : 0;
        out.insert(out.end(), (uint8_t*)&w, (uint8_t*)&w + 2);
        out.insert(out.end(), (uint8_t*)&h, (uint8_t*)&h + 2);
        out.push_back(flags);
        if (ok) out.insert(out.end(), im.rgba.begin(), im.rgba.end());
    }
    u32((uint32_t)m.vertices.size());
    for (const MdlVertex& v : m.vertices) {
        float p[3], n[3];
        to_gl(v.pos, p, INCH);
        to_gl(v.normal, n, 1.0f);
        for (float x : p) f32(x);
        for (float x : n) f32(x);
        f32(v.u);
        f32(v.v);
    }
    u32((uint32_t)m.meshes.size());
    for (const MdlMesh& me : m.meshes) {
        u32((uint32_t)slot[me.material]);
        u32((uint32_t)me.indices.size());
        for (uint32_t i : me.indices) u32(i);
    }

    std::vector<AnimSource> all;
    std::set<std::string> seen, taken;
    std::list<Mdl> keep;
    gather_anims(vfs, m, all, seen, keep);
    for (const AnimSource& a : all)
        if (taken.insert(a.name).second) printf("anim\t%s\t%s\n", a.name.c_str(), a.mdl->path.c_str());
    for (const std::string& inc : m.includes) printf("include\t%s\n", inc.c_str());

    if (!preview.empty()) {
        FILE* f = fopen(preview.c_str(), "wb");
        if (!f || fwrite(out.data(), 1, out.size(), f) != out.size()) { if (f) fclose(f); fprintf(stderr, "cannot write %s\n", preview.c_str()); return false; }
        fclose(f);
    }
    return true;
}

static std::vector<std::string> split(const std::string& l) {
    std::vector<std::string> v;
    for (size_t at = 0; at <= l.size();) {
        size_t c = l.find(',', at);
        if (c == std::string::npos) c = l.size();
        if (c > at) v.push_back(l.substr(at, c - at));
        at = c + 1;
    }
    return v;
}

static void usage(const char* self) {
    fprintf(stderr,
            "usage: %s [--gmod dir] [--no-workshop] --list [text]\n"
            "       %s [--gmod dir] --anims model.mdl\n"
            "       %s [--gmod dir] model.mdl -o out.glb [--tex 256] [--anims name,prefix*,all] [--anims-only]\n"
            "       %s [--gmod dir] --info model.mdl [--preview out.bin] [--tex 512]\n"
            "  and --skin N, --body a,b,c (a choice per body part) for a model's other looks\n",
            self, self, self, self);
}

int main(int argc, char** argv) {
    std::string gmod, model, out, list, preview;
    bool do_list = false, list_anims = false, workshop = true, do_info = false;
    Options opt;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--gmod" && i + 1 < argc) gmod = argv[++i];
        else if (a == "--no-workshop") workshop = false;
        else if (a == "--list") { do_list = true; if (i + 1 < argc && argv[i + 1][0] != '-') list = argv[++i]; }
        else if (a == "-o" && i + 1 < argc) out = argv[++i];
        else if (a == "--tex" && i + 1 < argc) opt.tex = atoi(argv[++i]);
        else if (a == "--anims-only") opt.anims_only = true;
        else if (a == "--info") do_info = true;
        else if (a == "--preview" && i + 1 < argc) preview = argv[++i];
        else if (a == "--skin" && i + 1 < argc) opt.pick.skin = atoi(argv[++i]);
        else if (a == "--body" && i + 1 < argc) for (const std::string& b : split(argv[++i])) opt.pick.body.push_back(atoi(b.c_str()));
        else if (a == "--anims") {
            if (i + 1 < argc && argv[i + 1][0] != '-' && !strstr(argv[i + 1], ".mdl")) {
                for (const std::string& n : split(argv[++i])) opt.anims.push_back(n);
            } else {
                list_anims = true;
            }
        }
        else if (a[0] != '-') model = a;
        else { usage(argv[0]); return 2; }
    }
    if (gmod.empty()) gmod = Vfs::find_gmod();
    if (gmod.empty()) { fprintf(stderr, "Garry's Mod not found: --gmod <GarrysMod folder>\n"); return 1; }
    Vfs vfs;
    if (!vfs.mount_gmod(gmod, workshop)) { fprintf(stderr, "Not a Garry's Mod folder: %s\n", gmod.c_str()); return 1; }

    if (do_list) {
        std::string l = Vfs::norm(list);
        for (auto& kv : vfs.files)
            if (kv.first.size() > 4 && !kv.first.compare(kv.first.size() - 4, 4, ".mdl") && kv.first.find(l) != std::string::npos)
                printf("%s\t%s\n", kv.first.c_str(), vfs.sources[kv.second.source].label.c_str());
        return 0;
    }
    if (model.empty()) { usage(argv[0]); return 2; }
    if (do_info) return info(vfs, model, opt, preview) ? 0 : 1;
    if (list_anims && out.empty()) {
        Mdl m;
        std::string why;
        if (!mdl_load_skeleton(vfs, model, m, &why)) { fprintf(stderr, "%s\n", why.c_str()); return 1; }
        std::vector<AnimSource> all;
        std::set<std::string> seen;
        std::list<Mdl> keep;
        gather_anims(vfs, m, all, seen, keep);
        for (const AnimSource& a : all) {
            MdlAnim an;
            bool ok = mdl_decode_anim(*a.mdl, a.name, an);
            printf("%-40s %5d frames %5.0f fps  %s%s\n", a.name.c_str(), an.frames, an.fps, a.mdl->path.c_str(), ok ? "" : "  (cannot decode)");
        }
        return 0;
    }
    if (out.empty()) { usage(argv[0]); return 2; }
    return convert(vfs, model, out, opt) ? 0 : 1;
}
