/* gmod2glb: Garry's Mod models and maps (and the HL2, CS:S, TF2 and Portal installed beside it, and
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

#include "bsp.h"
#include "glb.h"
#include <sys/stat.h>

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
    float simplify = 0;     /* a map: bsp_simplify's error, 0 none */
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
 * The preview file, little-endian: "GMPV" u32 3; u32 materials, each u16 w,
 * u16 h, u8 flags (1 alphatest, 2 translucent, 4 two-sided, 8 additive),
 * w*h RGBA; u32 vertices, each float pos[3], normal[3], uv[2] (glTF's axes,
 * metres), u8 rgba (a map's light; white for a model); u32 meshes, each u32
 * material, u32 indices, the indices; u32 parts (a map's props), each u32
 * first mesh, u32 meshes, u32 places, each float pos[3], rot[9] (by columns):
 * those meshes drawn again at each place. Meshes in no part are drawn as
 * they are
 *
 * A map (.bsp) is the same, with "map" for "model" and a line a prop:
 *   prop <model> <how many>                      spawn <x> <y> <z>
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
    std::vector<uint8_t> out = { 'G', 'M', 'P', 'V', 3, 0, 0, 0 };
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
        u32(0xffffffff);
    }
    u32((uint32_t)m.meshes.size());
    for (const MdlMesh& me : m.meshes) {
        u32((uint32_t)slot[me.material]);
        u32((uint32_t)me.indices.size());
        for (uint32_t i : me.indices) u32(i);
    }
    u32(0);     /* no parts placed again */

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

/* ================================================================
 * Maps
 * ================================================================ */

static std::string vmt_base(const std::string& vmt) {
    std::string b = vmt.substr(vmt.find_last_of('/') + 1);
    return b.size() > 4 && !b.compare(b.size() - 4, 4, ".vmt") ? b.substr(0, b.size() - 4) : b;
}

static uint8_t mat_flags(const Material& m) {
    return (m.alphatest ? 1 : 0) | (m.translucent ? 2 : 0) | (m.nocull ? 4 : 0) | (m.additive ? 8 : 0);
}

static bool map_info(Vfs& vfs, const std::string& path, const Options& opt, const std::string& preview) {
    Bsp b;
    std::string why;
    if (!bsp_load(vfs, path, b, &why)) { fprintf(stderr, "%s: %s\n", path.c_str(), why.c_str()); return false; }
    size_t unsimplified = opt.simplify > 0 ? bsp_simplify(b, opt.simplify) : 0;
    const VfsEntry* e = vfs.find(path);
    size_t world_verts = 0;
    for (const BspGroup& g : b.groups) world_verts += g.pos.size() / 3;
    printf("map\t%s\t%s\n", b.path.c_str(), e ? vfs.sources[e->source].label.c_str() : "");
    printf("counts\t0\t%zu\t%zu\t1\n", world_verts, b.tris());
    if (unsimplified) printf("simplified\t%zu\n", unsimplified);

    /* The preview's pieces, put together at the end */
    std::vector<uint8_t> mats, verts, meshes, parts;
    uint32_t nmats = 0, nverts = 0, nmeshes = 0, nparts = 0;
    auto put32 = [](std::vector<uint8_t>& v, uint32_t x) { v.insert(v.end(), (uint8_t*)&x, (uint8_t*)&x + 4); };
    auto putf = [](std::vector<uint8_t>& v, float x) { v.insert(v.end(), (uint8_t*)&x, (uint8_t*)&x + 4); };
    std::map<std::string, uint32_t> mat_of;
    /* A material once, however many use it; printed for the world's only */
    auto material = [&](const std::string& vmt, bool print) {
        auto it = mat_of.find(vmt);
        if (it != mat_of.end()) return it->second;
        Material mat;
        Image im;
        std::string err;
        int fw = 0, fh = 0;
        bool ok = texture_of(vfs, vmt, opt.tex, mat, im, err, &fw, &fh);
        /* Water and glass have no texture of their own: a see-through tint */
        if (!ok && (mat.shader == "water" || mat.shader == "refract")) {
            bool water = mat.shader == "water";
            im.w = im.h = 1;
            im.rgba = water ? std::vector<uint8_t>{ 64, 97, 115, 153 } : std::vector<uint8_t>{ 191, 217, 230, 64 };
            mat.translucent = true;
            ok = true;
            err.clear();
        }
        if (print)
            printf("mat\t%s\t%s\t%s\t%d\t%d\t%s\n", vmt_base(vmt).c_str(), vmt.c_str(), mat.basetexture.c_str(), fw, fh,
                   ok ? "" : err.c_str());
        uint16_t w = ok ? (uint16_t)im.w : 0, h = ok ? (uint16_t)im.h : 0;
        mats.insert(mats.end(), (uint8_t*)&w, (uint8_t*)&w + 2);
        mats.insert(mats.end(), (uint8_t*)&h, (uint8_t*)&h + 2);
        mats.push_back(mat_flags(mat));
        if (ok) mats.insert(mats.end(), im.rgba.begin(), im.rgba.end());
        mat_of[vmt] = nmats;
        return nmats++;
    };

    /* The world, where it is */
    for (const BspGroup& g : b.groups) {
        uint32_t mi = material(g.material, true);
        for (size_t i = 0; i < g.pos.size() / 3; i++) {
            for (int k = 0; k < 3; k++) putf(verts, g.pos[i * 3 + k]);
            for (int k = 0; k < 3; k++) putf(verts, g.nrm[i * 3 + k]);
            putf(verts, g.uv[i * 2]);
            putf(verts, g.uv[i * 2 + 1]);
            verts.insert(verts.end(), &g.rgba[i * 4], &g.rgba[i * 4] + 4);
        }
        put32(meshes, mi);
        put32(meshes, (uint32_t)g.idx.size());
        for (uint32_t i : g.idx) put32(meshes, nverts + i);
        nverts += (uint32_t)(g.pos.size() / 3);
        nmeshes++;
    }

    /* Each prop's model once (as its skin has it), drawn where each stands */
    std::map<std::string, std::vector<const BspProp*>> kinds;
    for (const BspProp& p : b.props) kinds[p.model + "#" + std::to_string(p.skin)].push_back(&p);
    size_t prop_tris = 0;
    int missing = 0;
    for (auto& kv : kinds) {
        const BspProp& first = *kv.second[0];
        Mdl m;
        MdlPick pick;
        pick.skin = first.skin;
        if (!mdl_load(vfs, first.model, m, nullptr, pick)) { missing++; continue; }
        uint32_t first_mesh = nmeshes;
        for (const MdlVertex& v : m.vertices) {
            float p[3], n[3];
            to_gl(v.pos, p, INCH);
            to_gl(v.normal, n, 1.0f);
            for (float x : p) putf(verts, x);
            for (float x : n) putf(verts, x);
            putf(verts, v.u);
            putf(verts, v.v);
            put32(verts, 0xffffffff);
        }
        size_t tris = 0;
        for (const MdlMesh& me : m.meshes) {
            uint32_t mi = material(me.material < (int)m.materials.size() ? m.materials[me.material] : "", false);
            put32(meshes, mi);
            put32(meshes, (uint32_t)me.indices.size());
            for (uint32_t i : me.indices) put32(meshes, nverts + i);
            nmeshes++;
            tris += me.indices.size() / 3;
        }
        nverts += (uint32_t)m.vertices.size();
        put32(parts, first_mesh);
        put32(parts, nmeshes - first_mesh);
        put32(parts, (uint32_t)kv.second.size());
        for (const BspProp* p : kv.second) {
            for (float x : p->pos) putf(parts, x);
            for (float x : p->rot) putf(parts, x);
        }
        nparts++;
        prop_tris += tris * kv.second.size();
    }

    std::map<std::string, int> props;
    for (const BspProp& p : b.props) props[p.model]++;
    for (auto& kv : props) printf("prop\t%s\t%d\n", kv.first.c_str(), kv.second);
    printf("proptris\t%zu\t%d\n", prop_tris, missing);
    if (b.has_spawn) printf("spawn\t%g\t%g\t%g\n", b.spawn[0], b.spawn[1], b.spawn[2]);
    printf("skybox\t%d\t%d\n", b.sky_faces, b.sky_props);
    if (!preview.empty()) {
        std::vector<uint8_t> out = { 'G', 'M', 'P', 'V', 3, 0, 0, 0 };
        put32(out, nmats);
        out.insert(out.end(), mats.begin(), mats.end());
        put32(out, nverts);
        out.insert(out.end(), verts.begin(), verts.end());
        put32(out, nmeshes);
        out.insert(out.end(), meshes.begin(), meshes.end());
        put32(out, nparts);
        out.insert(out.end(), parts.begin(), parts.end());
        FILE* f = fopen(preview.c_str(), "wb");
        if (!f || fwrite(out.data(), 1, out.size(), f) != out.size()) { if (f) fclose(f); fprintf(stderr, "cannot write %s\n", preview.c_str()); return false; }
        fclose(f);
    }
    return true;
}

/* The world: a primitive a material, its light in COLOR_0 */
static bool write_world(const Vfs& vfs, const Bsp& b, const std::string& out, int tex) {
    Glb g;
    std::vector<std::string> materials, textures, images, prims;
    for (size_t gi = 0; gi < b.groups.size(); gi++) {
        const BspGroup& gr = b.groups[gi];
        Material mat;
        Image im;
        std::string err;
        std::string json = "{\"name\":\"" + esc(vmt_base(gr.material)) + "\"";
        if (texture_of(vfs, gr.material, tex, mat, im, err)) {
            bool alpha = mat.alphatest || mat.translucent;
            if (!alpha) for (size_t i = 3; i < im.rgba.size(); i += 4) im.rgba[i] = 255;
            std::vector<uint8_t> p = png(im, alpha);
            int v = g.view(p.data(), p.size());
            images.push_back("{\"bufferView\":" + std::to_string(v) + ",\"mimeType\":\"image/png\",\"name\":\"" +
                             esc(mat.basetexture.substr(mat.basetexture.find_last_of('/') + 1)) + "\"}");
            textures.push_back("{\"source\":" + std::to_string(images.size() - 1) + "}");
            json += ",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":" + std::to_string(textures.size() - 1) +
                    "},\"metallicFactor\":0,\"roughnessFactor\":1}";
            if (mat.translucent || mat.additive) json += ",\"alphaMode\":\"BLEND\"";
            else if (mat.alphatest) json += ",\"alphaMode\":\"MASK\"";
        } else if (mat.shader == "water") {
            json += ",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.25,0.38,0.45,0.6],\"metallicFactor\":0,\"roughnessFactor\":1}"
                    ",\"alphaMode\":\"BLEND\"";
        } else if (mat.shader == "refract") {     /* glass that bends what is behind it: faint */
            json += ",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.75,0.85,0.9,0.25],\"metallicFactor\":0,\"roughnessFactor\":1}"
                    ",\"alphaMode\":\"BLEND\"";
        } else {
            fprintf(stderr, "  %s: %s\n", gr.material.c_str(), err.c_str());
            json += ",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.6,0.6,0.6,1],\"metallicFactor\":0,\"roughnessFactor\":1}";
        }
        if (mat.nocull) json += ",\"doubleSided\":true";
        materials.push_back(json + "}");

        size_t nv = gr.pos.size() / 3;
        float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
        for (size_t i = 0; i < nv; i++)
            for (int k = 0; k < 3; k++) { mn[k] = std::min(mn[k], gr.pos[i * 3 + k]); mx[k] = std::max(mx[k], gr.pos[i * 3 + k]); }
        std::vector<float> col(nv * 4);
        for (size_t i = 0; i < nv * 4; i++) col[i] = gr.rgba[i] / 255.0f;
        std::string attrs = "\"POSITION\":" + std::to_string(g.accessor(gr.pos.data(), nv, 5126, "VEC3", 1, mn, mx, 3)) +
                            ",\"NORMAL\":" + std::to_string(g.accessor(gr.nrm.data(), nv, 5126, "VEC3", 1)) +
                            ",\"TEXCOORD_0\":" + std::to_string(g.accessor(gr.uv.data(), nv, 5126, "VEC2", 1)) +
                            ",\"COLOR_0\":" + std::to_string(g.accessor(col.data(), nv, 5126, "VEC4", 1));
        int ai = g.accessor(gr.idx.data(), gr.idx.size(), 5125, "SCALAR", 2);
        prims.push_back("{\"attributes\":{" + attrs + "},\"indices\":" + std::to_string(ai) + ",\"material\":" +
                        std::to_string(gi) + "}");
    }
    std::string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"DMS gmod2glb\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}]";
    json += ",\"nodes\":[{\"name\":\"" + esc(b.name) + "\",\"mesh\":0}]";
    json += ",\"meshes\":[{\"name\":\"" + esc(b.name) + "\",\"primitives\":[" + join(prims) + "]}]";
    json += ",\"materials\":[" + join(materials) + "]";
    if (!textures.empty()) json += ",\"textures\":[" + join(textures) + "],\"images\":[" + join(images) + "]";
    json += ",\"accessors\":[" + join(g.accessors) + "],\"bufferViews\":[" + join(g.views) + "]";
    json += ",\"buffers\":[{\"byteLength\":" + std::to_string((g.bin.size() + 3) & ~(size_t)3) + "}]}";
    return write_glb(out, json, g.bin);
}

static bool exists(const std::string& p) { FILE* f = fopen(p.c_str(), "rb"); if (f) fclose(f); return f != nullptr; }

static void mkdirs(const std::string& dir) {
    for (size_t at = 1; at <= dir.size(); at++)
        if (at == dir.size() || dir[at] == '/') mkdir(dir.substr(0, at).c_str(), 0755);
}

/* A map into a project: its world, each prop it uses (once, shared by every
 * map), and a scene with them all where the map has them */
static bool bring_map(Vfs& vfs, const std::string& path, const std::string& assets_in, std::string name, const Options& opt,
                      bool props,
                      const std::string& player, float player_scale) {
    Bsp b;
    std::string why;
    if (!bsp_load(vfs, path, b, &why)) { fprintf(stderr, "%s: %s\n", path.c_str(), why.c_str()); return false; }
    size_t unsimplified = opt.simplify > 0 ? bsp_simplify(b, opt.simplify) : 0;
    if (name.empty()) name = b.name;
    std::string assets = assets_in;
    if (!assets.empty() && assets.back() != '/') assets += '/';
    std::string world_rel = "gmod/maps/" + name + "/" + name;
    mkdirs(assets + "gmod/maps/" + name);
    if (!write_world(vfs, b, assets + world_rel + ".glb", opt.tex)) { fprintf(stderr, "cannot write the world\n"); return false; }
    printf("%s: %zu triangles, %zu materials -> %s.glb\n", b.path.c_str(), b.tris(), b.groups.size(), world_rel.c_str());
    if (unsimplified) printf("simplified from %zu triangles\n", unsimplified);
    if (b.sky_faces || b.sky_props)
        printf("the 3D skybox left out: %d faces, %d props\n", b.sky_faces, b.sky_props);

    struct Obj { std::string name, model; float pos[3], rot[3]; };
    std::vector<Obj> objs;
    std::map<std::string, std::string> done;     /* model and skin -> its .dms under assets */
    std::map<std::string, int> names;
    int made = 0, failed = 0;
    if (props)
        for (const BspProp& p : b.props) {
            std::string key = p.model + "#" + std::to_string(p.skin);
            auto it = done.find(key);
            if (it == done.end()) {
                std::string m = p.model;
                if (!m.compare(0, 7, "models/")) m = m.substr(7);
                if (m.size() > 4) m.resize(m.size() - 4);
                std::string rel = "gmod/" + m + (p.skin ? "_skin" + std::to_string(p.skin) : "");
                std::string glb = assets + rel + ".glb";
                bool ok = exists(glb);
                if (!ok) {
                    mkdirs(glb.substr(0, glb.find_last_of('/')));
                    Options po = opt;
                    po.pick = MdlPick();
                    po.pick.skin = p.skin;
                    po.anims.clear();
                    ok = convert(vfs, p.model, glb, po);
                    if (ok) made++; else failed++;
                }
                it = done.emplace(key, ok ? rel + ".dms" : "").first;
            }
            if (it->second.empty()) continue;
            Obj o;
            std::string base = p.model.substr(p.model.find_last_of('/') + 1);
            base = base.substr(0, base.size() - 4);
            o.name = base + "_" + std::to_string(++names[base]);
            o.model = it->second;
            memcpy(o.pos, p.pos, sizeof(o.pos));
            bsp_rot_degrees(p.rot, o.rot);
            objs.push_back(o);
        }

    /* The scene: the world (World, which the examples stand on), the props,
     * the camera where a player starts */
    mkdirs(assets + "scenes");
    std::string scene = assets + "scenes/" + name + ".json";
    FILE* f = fopen(scene.c_str(), "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", scene.c_str()); return false; }
    float cam[3] = { b.spawn[0], b.spawn[1] + 1.6f, b.spawn[2] };
    fprintf(f, "{\n  \"dms_scene\": 1,\n  \"background\": \"#8db4d8\",\n");
    fprintf(f, "  \"camera\": { \"pos\": [%g, %g, %g], \"yaw\": %g, \"pitch\": 0, \"fov\": 60 },\n", cam[0], cam[1], cam[2],
            b.spawn_yaw);
    fprintf(f, "  \"lights\": [],\n  \"objects\": [\n");
    fprintf(f, "    { \"name\": \"World\", \"model\": \"%s.dms\",\n      \"pos\": [0, 0, 0], \"rot\": [0, 0, 0], \"scale\": [1, 1, 1],\n"
               "      \"tint\": \"#ffffff\", \"add\": false, \"visible\": true }",
            esc(world_rel).c_str());
    /* Player: where the map starts a player (on the ground below it), as the
     * examples spawn there. The project's character, or a marker */
    fprintf(f, ",\n    { \"name\": \"Player\", \"model\": \"%s\",\n      \"pos\": [%g, %g, %g], \"rot\": [0, %g, 0], "
               "\"scale\": [%g, %g, %g],\n      \"tint\": \"#ffffff\", \"add\": false, \"visible\": true }",
            esc(player).c_str(), b.spawn[0], b.spawn[1], b.spawn[2], b.spawn_yaw, player_scale, player_scale, player_scale);
    for (const Obj& o : objs)
        fprintf(f, ",\n    { \"name\": \"%s\", \"model\": \"%s\",\n      \"pos\": [%g, %g, %g], \"rot\": [%g, %g, %g], "
                   "\"scale\": [1, 1, 1],\n      \"tint\": \"#ffffff\", \"add\": false, \"visible\": true }",
                esc(o.name).c_str(), esc(o.model).c_str(), o.pos[0], o.pos[1], o.pos[2], o.rot[0], o.rot[1], o.rot[2]);
    fprintf(f, "\n  ]\n}\n");
    fclose(f);
    printf("%zu props placed (%zu kinds, %d converted now, %d failed) -> scenes/%s.json\n", objs.size(), done.size(), made,
           failed, name.c_str());
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
            "       %s [--gmod dir] --info model.mdl|maps/x.bsp [--preview out.bin] [--tex 512]\n"
            "       %s [--gmod dir] maps/x.bsp --assets <project>/assets [--name x] [--tex 128] [--no-props]\n"
            "           [--simplify 0.02]   (fewer triangles: how far shape (metres) and light (0..1) may move)\n"
            "           [--player char/model.dms --player-scale 1]   (the scene's Player, at the map's start)\n"
            "  and --skin N, --body a,b,c (a choice per body part) for a model's other looks\n",
            self, self, self, self, self);
}

int main(int argc, char** argv) {
    std::string gmod, model, out, list, preview, assets, name, player;
    float player_scale = 1.0f;
    bool games = true;
    bool do_list = false, list_anims = false, workshop = true, do_info = false, tex_set = false, props = true;
    Options opt;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--gmod" && i + 1 < argc) gmod = argv[++i];
        else if (a == "--no-workshop") workshop = false;
        else if (a == "--list") { do_list = true; if (i + 1 < argc && argv[i + 1][0] != '-') list = argv[++i]; }
        else if (a == "-o" && i + 1 < argc) out = argv[++i];
        else if (a == "--tex" && i + 1 < argc) { opt.tex = atoi(argv[++i]); tex_set = true; }
        else if (a == "--assets" && i + 1 < argc) assets = argv[++i];
        else if (a == "--name" && i + 1 < argc) name = argv[++i];
        else if (a == "--no-props") props = false;
        else if (a == "--no-games") games = false;
        else if (a == "--simplify" && i + 1 < argc) opt.simplify = (float)atof(argv[++i]);
        else if (a == "--player" && i + 1 < argc) player = argv[++i];
        else if (a == "--player-scale" && i + 1 < argc) player_scale = (float)atof(argv[++i]);
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
    if (games) vfs.mount_games(gmod + "/..");

    if (do_list) {
        std::string l = Vfs::norm(list);
        for (auto& kv : vfs.files)
            if (kv.first.size() > 4 && (!kv.first.compare(kv.first.size() - 4, 4, ".mdl") ||
                                        (!kv.first.compare(kv.first.size() - 4, 4, ".bsp") && !kv.first.compare(0, 5, "maps/"))) &&
                kv.first.find(l) != std::string::npos)
                printf("%s\t%s\n", kv.first.c_str(), vfs.sources[kv.second.source].label.c_str());
        return 0;
    }
    if (model.empty()) { usage(argv[0]); return 2; }
    bool is_map = model.size() > 4 && !model.compare(model.size() - 4, 4, ".bsp");
    if (is_map && !tex_set) opt.tex = 128;     /* a map has a lot of them */
    if (is_map && do_info) return map_info(vfs, model, opt, preview) ? 0 : 1;
    if (is_map) {
        if (assets.empty()) { fprintf(stderr, "a map goes into a project: --assets <project>/assets\n"); return 2; }
        return bring_map(vfs, model, assets, name, opt, props, player, player_scale) ? 0 : 1;
    }
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
