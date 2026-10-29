#include "mdl.h"
#include "material.h"
#include <cmath>
#include <cstring>

template <class T> static T rd(const std::vector<uint8_t>& b, size_t off) {
    T v{};
    if (off + sizeof(T) <= b.size()) memcpy(&v, &b[off], sizeof(T));
    return v;
}
static int ri(const std::vector<uint8_t>& b, size_t off) { return rd<int32_t>(b, off); }
static float rf(const std::vector<uint8_t>& b, size_t off) { return rd<float>(b, off); }
static Vec3 rv(const std::vector<uint8_t>& b, size_t off) { return { rf(b, off), rf(b, off + 4), rf(b, off + 8) }; }
static std::string rs(const std::vector<uint8_t>& b, size_t off) {
    std::string s;
    while (off < b.size() && b[off]) s += (char)b[off++];
    return s;
}

/* studiohdr_t (v44-49) */
enum {
    H_NAME = 12, H_FLAGS = 152, H_NUMBONES = 156, H_BONEINDEX = 160,
    H_NUMLOCALANIM = 180, H_LOCALANIMINDEX = 184,
    H_NUMTEXTURES = 204, H_TEXTUREINDEX = 208, H_NUMCDTEXTURES = 212, H_CDTEXTUREINDEX = 216,
    H_NUMSKINREF = 220, H_NUMSKINFAMILIES = 224, H_SKININDEX = 228,
    H_NUMBODYPARTS = 232, H_BODYPARTINDEX = 236,
    H_NUMINCLUDEMODELS = 336, H_INCLUDEMODELINDEX = 340,
    H_ANIMBLOCKNAME = 348, H_NUMANIMBLOCKS = 352, H_ANIMBLOCKINDEX = 356,
};
enum { BONE_SIZE = 216, TEX_SIZE = 64, BODYPART_SIZE = 16, MODEL_SIZE = 148, MESH_SIZE = 116, ANIMDESC_SIZE = 100 };

static bool header(const Vfs& vfs, const std::string& path, Mdl& out, std::string* why) {
    out.path = Vfs::norm(path);
    if (!vfs.read(out.path, out.raw)) { if (why) *why = "not found: " + out.path; return false; }
    const auto& b = out.raw;
    if (b.size() < 408 || memcmp(b.data(), "IDST", 4)) { if (why) *why = "not a Source model"; return false; }
    out.version = ri(b, 4);
    if (out.version < 44 || out.version > 49) { if (why) *why = "model version " + std::to_string(out.version); return false; }
    out.name = rs(b, H_NAME);

    int nb = ri(b, H_NUMBONES), bi = ri(b, H_BONEINDEX);
    for (int i = 0; i < nb; i++) {
        size_t o = (size_t)bi + (size_t)i * BONE_SIZE;
        MdlBone bone;
        bone.name = rs(b, o + ri(b, o));
        bone.parent = ri(b, o + 4);
        bone.pos = rv(b, o + 32);
        bone.quat = { rf(b, o + 44), rf(b, o + 48), rf(b, o + 52), rf(b, o + 56) };
        bone.rot = rv(b, o + 60);
        bone.posscale = rv(b, o + 72);
        bone.rotscale = rv(b, o + 84);
        for (int k = 0; k < 12; k++) bone.pose_to_bone[k] = rf(b, o + 96 + 4 * k);
        out.bones.push_back(bone);
    }

    int ni = ri(b, H_NUMINCLUDEMODELS), ii = ri(b, H_INCLUDEMODELINDEX);
    for (int i = 0; i < ni; i++) {
        size_t o = (size_t)ii + (size_t)i * 8;
        out.includes.push_back(Vfs::norm(rs(b, o + ri(b, o + 4))));
    }

    /* Animation data kept in the .ani beside it */
    if (ri(b, H_NUMANIMBLOCKS) > 0 && ri(b, H_ANIMBLOCKNAME)) {
        std::string ani = Vfs::norm(rs(b, ri(b, H_ANIMBLOCKNAME)));
        if (!vfs.read(ani, out.ani)) {
            std::string beside = out.path.substr(0, out.path.size() - 4) + ".ani";
            vfs.read(beside, out.ani);
        }
    }
    return true;
}

bool mdl_load_skeleton(const Vfs& vfs, const std::string& path, Mdl& out, std::string* why) {
    return header(vfs, path, out, why);
}

bool mdl_load(const Vfs& vfs, const std::string& path, Mdl& out, std::string* why, const MdlPick& pick) {
    if (!header(vfs, path, out, why)) return false;
    const auto& b = out.raw;
    auto fail = [&](const std::string& w) { if (why) *why = w; return false; };

    /* Materials: each texture name tried in each $cdmaterials folder */
    std::vector<std::string> dirs;
    int ncd = ri(b, H_NUMCDTEXTURES), cdi = ri(b, H_CDTEXTUREINDEX);
    for (int i = 0; i < ncd; i++) dirs.push_back(Vfs::norm(rs(b, ri(b, cdi + 4 * i))));
    int nt = ri(b, H_NUMTEXTURES), ti = ri(b, H_TEXTUREINDEX);
    std::vector<std::string> textures;
    for (int i = 0; i < nt; i++) {
        size_t o = (size_t)ti + (size_t)i * TEX_SIZE;
        std::string name = Vfs::norm(rs(b, o + ri(b, o)));
        std::string found = name;
        for (const std::string& d : dirs) {
            std::string p = "materials/" + d + (d.empty() || d.back() == '/' ? "" : "/") + name + ".vmt";
            p = Vfs::norm(p);
            if (vfs.exists(p)) { found = p; break; }
        }
        textures.push_back(found);
    }
    /* The skin: which texture each mesh's material number means */
    int nref = ri(b, H_NUMSKINREF), si = ri(b, H_SKININDEX);
    out.skins = std::max(1, ri(b, H_NUMSKINFAMILIES));
    int family = pick.skin >= 0 && pick.skin < out.skins ? pick.skin : 0;
    std::vector<int> skin(nref);
    for (int i = 0; i < nref; i++) skin[i] = rd<int16_t>(b, si + 2 * ((size_t)family * nref + i));
    out.materials = textures;

    /* The .vvd: its LOD 0 vertices, through the fixups */
    std::string base = out.path.substr(0, out.path.size() - 4);
    std::vector<uint8_t> vvd, vtx;
    if (!vfs.read(base + ".vvd", vvd) || vvd.size() < 64 || memcmp(vvd.data(), "IDSV", 4)) return fail("no .vvd");
    if (!vfs.read(base + ".dx90.vtx", vtx) && !vfs.read(base + ".vtx", vtx) && !vfs.read(base + ".dx80.vtx", vtx))
        return fail("no .vtx");
    int nfix = ri(vvd, 48), fixi = ri(vvd, 52), vdata = ri(vvd, 56);
    int nlod0 = ri(vvd, 16);
    std::vector<int> order;     /* LOD 0's vertex i is the file's order[i] */
    if (nfix) {
        for (int i = 0; i < nfix; i++) {
            size_t o = (size_t)fixi + (size_t)i * 12;
            if (ri(vvd, o) < 0) continue;
            int src = ri(vvd, o + 4), n = ri(vvd, o + 8);
            for (int k = 0; k < n; k++) order.push_back(src + k);
        }
    } else {
        for (int i = 0; i < nlod0; i++) order.push_back(i);
    }
    std::vector<MdlVertex> all(order.size());
    for (size_t i = 0; i < order.size(); i++) {
        size_t o = (size_t)vdata + (size_t)order[i] * 48;
        MdlVertex& v = all[i];
        for (int k = 0; k < 3; k++) { v.weight[k] = rf(vvd, o + 4 * k); v.bone[k] = vvd[o + 12 + k]; }
        v.bones = std::max(1, (int)vvd[o + 15]);
        v.pos = rv(vvd, o + 16);
        v.normal = rv(vvd, o + 28);
        v.u = rf(vvd, o + 40);
        v.v = rf(vvd, o + 44);
    }

    /* The .vtx's LOD 0 triangles, by body part, model, mesh, strip group */
    bool v49 = out.version >= 49;
    size_t sg_size = v49 ? 33 : 25, strip_size = v49 ? 35 : 27;
    int nbp = ri(b, H_NUMBODYPARTS), bpi = ri(b, H_BODYPARTINDEX);
    int vtx_nbp = ri(vtx, 28), vtx_bpo = ri(vtx, 32);
    if (vtx_nbp != nbp) return fail(".vtx does not match the .mdl");
    std::vector<int> remap(all.size(), -1);     /* vvd vertex -> out.vertices */
    for (int bp = 0; bp < nbp; bp++) {
        size_t mbp = (size_t)bpi + (size_t)bp * BODYPART_SIZE;
        size_t xbp = (size_t)vtx_bpo + (size_t)bp * 8;
        int nmodels = ri(b, mbp + 4);
        MdlBodyPart part;
        part.name = rs(b, mbp + ri(b, mbp));
        for (int k = 0; k < nmodels; k++) part.models.push_back(rs(b, mbp + ri(b, mbp + 12) + (size_t)k * MODEL_SIZE));
        out.bodyparts.push_back(part);
        if (nmodels < 1) continue;
        int m = bp < (int)pick.body.size() && pick.body[bp] >= 0 && pick.body[bp] < nmodels ? pick.body[bp] : 0;
        size_t mm = mbp + ri(b, mbp + 12) + (size_t)m * MODEL_SIZE;
        size_t xm = xbp + ri(vtx, xbp + 4) + (size_t)m * 8;
        int model_vert0 = ri(b, mm + 84) / 48;
        size_t xlod = xm + ri(vtx, xm + 4);     /* LOD 0 */
        int nmesh = ri(b, mm + 72);
        if (ri(vtx, xlod) != nmesh) return fail(".vtx meshes do not match");
        for (int me = 0; me < nmesh; me++) {
            size_t mme = mm + ri(b, mm + 76) + (size_t)me * MESH_SIZE;
            size_t xme = xlod + ri(vtx, xlod + 4) + (size_t)me * 9;
            int mat = ri(b, mme);
            int vert0 = model_vert0 + ri(b, mme + 12);
            MdlMesh mesh;
            mesh.material = mat >= 0 && mat < (int)skin.size() ? skin[mat] : mat;
            int nsg = ri(vtx, xme);
            for (int g = 0; g < nsg; g++) {
                size_t sg = xme + ri(vtx, xme + 4) + (size_t)g * sg_size;
                int nv = ri(vtx, sg), vo = ri(vtx, sg + 4), nidx = ri(vtx, sg + 8), io = ri(vtx, sg + 12);
                int nstrip = ri(vtx, sg + 16), so = ri(vtx, sg + 20);
                /* Counts that run past the end of the file are a misread (or
                 * a broken file): a stop, not billions of triangles */
                if (nv < 0 || nidx < 0 || nstrip < 0 ||
                    sg + vo + (size_t)nv * 9 > vtx.size() || sg + io + (size_t)nidx * 2 > vtx.size() ||
                    sg + so + (size_t)nstrip * strip_size > vtx.size())
                    return fail(".vtx strip group out of range");
                std::vector<int> local(nv);
                for (int k = 0; k < nv; k++) {
                    int orig = rd<uint16_t>(vtx, sg + vo + (size_t)k * 9 + 4);
                    int vi = vert0 + orig;
                    if (vi < 0 || vi >= (int)all.size()) return fail("vertex out of range");
                    if (remap[vi] < 0) { remap[vi] = (int)out.vertices.size(); out.vertices.push_back(all[vi]); }
                    local[k] = remap[vi];
                }
                auto idx = [&](int k) { return (int)rd<uint16_t>(vtx, sg + io + (size_t)k * 2); };
                auto tri = [&](int a, int bb, int c) {
                    if (a >= nv || bb >= nv || c >= nv) return;
                    /* Source's front faces wind the other way to glTF's */
                    mesh.indices.push_back(local[a]);
                    mesh.indices.push_back(local[c]);
                    mesh.indices.push_back(local[bb]);
                };
                if (!nstrip) {
                    for (int k = 0; k + 2 < nidx; k += 3) tri(idx(k), idx(k + 1), idx(k + 2));
                }
                for (int s = 0; s < nstrip; s++) {
                    size_t st = sg + so + (size_t)s * strip_size;
                    int sn = ri(vtx, st), sio = ri(vtx, st + 4);
                    if (sn < 0 || sio < 0 || (size_t)sio + sn > (size_t)nidx) return fail(".vtx strip out of range");
                    uint8_t flags = vtx[st + 18];
                    if (flags & 2) {
                        for (int k = 0; k + 2 < sn; k++) {
                            int a = idx(sio + k), c1 = idx(sio + k + 1), c2 = idx(sio + k + 2);
                            if (k & 1) tri(c1, a, c2); else tri(a, c1, c2);
                        }
                    } else {
                        for (int k = 0; k + 2 < sn; k += 3) tri(idx(sio + k), idx(sio + k + 1), idx(sio + k + 2));
                    }
                }
            }
            if (!mesh.indices.empty()) out.meshes.push_back(mesh);
        }
    }
    if (out.meshes.empty()) return fail("no triangles");
    return true;
}

/* ================================================================
 * Animations
 * ================================================================ */

std::vector<std::string> mdl_anim_names(const Mdl& m, std::vector<bool>* delta) {
    std::vector<std::string> out;
    const auto& b = m.raw;
    int n = ri(b, H_NUMLOCALANIM), ai = ri(b, H_LOCALANIMINDEX);
    for (int i = 0; i < n; i++) {
        size_t o = (size_t)ai + (size_t)i * ANIMDESC_SIZE;
        out.push_back(rs(b, o + ri(b, o + 4)));
        if (delta) delta->push_back(ri(b, o + 12) & 0x0004);
    }
    return out;
}

/* Source's run-length packed values: the value at frame k of a channel */
static float rle_value(const uint8_t* p, const uint8_t* end, int k) {
    for (;;) {
        if (p + 2 > end) return 0;
        uint8_t valid = p[0], total = p[1];
        if (!total) return 0;
        if (total > k) {
            int at = valid > k ? k + 1 : valid;
            int16_t v;
            memcpy(&v, p + 2 * at, 2);
            return v;
        }
        k -= total;
        p += 2 * (valid + 1);
    }
}

static float half(uint16_t h) {
    int s = h >> 15, e = (h >> 10) & 31, m = h & 1023;
    float f = e == 0 ? ldexpf((float)m, -24) : e == 31 ? INFINITY : ldexpf(1.0f + m / 1024.0f, e - 15);
    return s ? -f : f;
}

static Quat euler_quat(Vec3 a) {
    float sr = sinf(a.x * 0.5f), cr = cosf(a.x * 0.5f);
    float sp = sinf(a.y * 0.5f), cp = cosf(a.y * 0.5f);
    float sy = sinf(a.z * 0.5f), cy = cosf(a.z * 0.5f);
    Quat q;
    q.x = sr * cp * cy - cr * sp * sy;
    q.y = cr * sp * cy + sr * cp * sy;
    q.z = cr * cp * sy - sr * sp * cy;
    q.w = cr * cp * cy + sr * sp * sy;
    return q;
}

static Quat quat48(const uint8_t* d) {
    uint16_t x, y, zw;
    memcpy(&x, d, 2); memcpy(&y, d + 2, 2); memcpy(&zw, d + 4, 2);
    Quat q;
    q.x = ((int)x - 32768) * (1.0f / 32768.0f);
    q.y = ((int)y - 32768) * (1.0f / 32768.0f);
    q.z = ((int)(zw & 0x7fff) - 16384) * (1.0f / 16384.0f);
    q.w = sqrtf(std::max(0.0f, 1.0f - q.x * q.x - q.y * q.y - q.z * q.z));
    if (zw & 0x8000) q.w = -q.w;
    return q;
}

/* The layout Left 4 Dead brought in: frame by bone instead of bone by frame.
 * A header (where the constants and the frames are, how long a frame is),
 * a flags byte a bone, then what does not change, once, and each frame's
 * values, both bone after bone, a rotation before a position. A bone with
 * neither keeps its rest pose. */
static void decode_frame_anim(const Mdl& m, const uint8_t* p, const uint8_t* end, int frame,
                              Vec3* pos, Quat* rot) {
    enum { CONST_POS = 1, CONST_ROT = 2, ANIM_POS = 4, ANIM_ROT = 8, FULLANIM_POS = 16 };
    if (p + 24 > end) return;
    int constants, frames, length;
    memcpy(&constants, p, 4); memcpy(&frames, p + 4, 4); memcpy(&length, p + 8, 4);
    size_t nb = m.bones.size();
    const uint8_t* bf = p + 24;
    const uint8_t* c = p + constants;
    const uint8_t* fd = p + frames + (size_t)frame * length;
    for (size_t i = 0; i < nb; i++) {
        uint8_t f = bf[i];
        if (f & ANIM_ROT) { if (fd + 6 <= end) rot[i] = quat48(fd); fd += 6; }
        else if (f & CONST_ROT) { if (c + 6 <= end) rot[i] = quat48(c); c += 6; }
        if (f & ANIM_POS) {
            uint16_t h[3];
            if (fd + 6 <= end) { memcpy(h, fd, 6); pos[i] = { half(h[0]), half(h[1]), half(h[2]) }; }
            fd += 6;
        } else if (f & CONST_POS) {
            uint16_t h[3];
            if (c + 6 <= end) { memcpy(h, c, 6); pos[i] = { half(h[0]), half(h[1]), half(h[2]) }; }
            c += 6;
        } else if (f & FULLANIM_POS) {
            if (fd + 12 <= end) memcpy(&pos[i], fd, 12);
            fd += 12;
        }
    }
}

bool mdl_decode_anim(const Mdl& m, const std::string& name, MdlAnim& out) {
    const auto& b = m.raw;
    int n = ri(b, H_NUMLOCALANIM), ai = ri(b, H_LOCALANIMINDEX);
    size_t desc = 0;
    bool found = false;
    for (int i = 0; i < n && !found; i++) {
        size_t o = (size_t)ai + (size_t)i * ANIMDESC_SIZE;
        if (rs(b, o + ri(b, o + 4)) == name) { desc = o; found = true; }
    }
    if (!found) return false;
    out.name = name;
    out.fps = rf(b, desc + 8);
    if (out.fps <= 0) out.fps = 30;
    int flags = ri(b, desc + 12);
    out.frames = std::max(1, ri(b, desc + 16));
    int block = ri(b, desc + 52), index = ri(b, desc + 56);
    int secindex = ri(b, desc + 80), secframes = ri(b, desc + 84);
    size_t nb = m.bones.size();
    out.pos.assign((size_t)out.frames * nb, Vec3());
    out.rot.assign((size_t)out.frames * nb, Quat());
    int nblocks = ri(b, H_NUMANIMBLOCKS), blocks = ri(b, H_ANIMBLOCKINDEX);

    for (int f = 0; f < out.frames; f++) {
        Vec3* pos = &out.pos[(size_t)f * nb];
        Quat* rot = &out.rot[(size_t)f * nb];
        for (size_t i = 0; i < nb; i++) { pos[i] = m.bones[i].pos; rot[i] = m.bones[i].quat; }
        if (flags & 0x0020) continue;           /* STUDIO_ALLZEROS: the rest pose */

        int blk = block, idx = index, local = f;
        size_t rel = desc;
        if (secframes > 0) {
            int sec = f / secframes;
            local = f - sec * secframes;
            blk = ri(b, desc + secindex + (size_t)sec * 8);
            idx = ri(b, desc + secindex + (size_t)sec * 8 + 4);
        }
        const uint8_t *p, *end;
        if (blk == 0) {
            p = b.data() + rel + idx;
            end = b.data() + b.size();
        } else {
            if (blk >= nblocks || m.ani.empty()) return false;
            int start = ri(b, blocks + (size_t)blk * 8);
            p = m.ani.data() + start + idx;
            end = m.ani.data() + m.ani.size();
        }
        if (flags & 0x0040) {                   /* STUDIO_FRAMEANIM (Left 4 Dead on) */
            decode_frame_anim(m, p, end, local, pos, rot);
            continue;
        }
        while (p + 4 <= end) {
            uint8_t bone = p[0], af = p[1];
            int16_t next;
            memcpy(&next, p + 2, 2);
            if (bone < nb) {
                const MdlBone& bn = m.bones[bone];
                const uint8_t* data = p + 4;
                bool delta = af & 0x10;
                if (af & 0x02) {                    /* RAWROT: Quaternion48 */
                    uint16_t x, y, zw;
                    memcpy(&x, data, 2); memcpy(&y, data + 2, 2); memcpy(&zw, data + 4, 2);
                    Quat q;
                    q.x = ((int)x - 32768) * (1.0f / 32768.0f);
                    q.y = ((int)y - 32768) * (1.0f / 32768.0f);
                    q.z = ((int)(zw & 0x7fff) - 16384) * (1.0f / 16384.0f);
                    q.w = sqrtf(std::max(0.0f, 1.0f - q.x * q.x - q.y * q.y - q.z * q.z));
                    if (zw & 0x8000) q.w = -q.w;
                    rot[bone] = q;
                } else if (af & 0x20) {             /* RAWROT2: Quaternion64 */
                    uint64_t v;
                    memcpy(&v, data, 8);
                    Quat q;
                    q.x = ((int)(v & 0x1fffff) - 1048576) * (1.0f / 1048576.5f);
                    q.y = ((int)((v >> 21) & 0x1fffff) - 1048576) * (1.0f / 1048576.5f);
                    q.z = ((int)((v >> 42) & 0x1fffff) - 1048576) * (1.0f / 1048576.5f);
                    q.w = sqrtf(std::max(0.0f, 1.0f - q.x * q.x - q.y * q.y - q.z * q.z));
                    if (v >> 63) q.w = -q.w;
                    rot[bone] = q;
                } else if (af & 0x08) {             /* ANIMROT: packed Euler channels */
                    Vec3 e;
                    float* ev = &e.x;
                    for (int k = 0; k < 3; k++) {
                        int16_t off;
                        memcpy(&off, data + 2 * k, 2);
                        ev[k] = off ? rle_value(data + off, end, local) * (&bn.rotscale.x)[k] : 0;
                        if (!delta) ev[k] += (&bn.rot.x)[k];
                    }
                    rot[bone] = euler_quat(e);
                }
                if (af & 0x01) {                    /* RAWPOS: Vector48 */
                    const uint8_t* pp = data + ((af & 0x02) ? 6 : 0) + ((af & 0x20) ? 8 : 0);
                    uint16_t h[3];
                    memcpy(h, pp, 6);
                    pos[bone] = { half(h[0]), half(h[1]), half(h[2]) };
                } else if (af & 0x04) {             /* ANIMPOS */
                    const uint8_t* vp = data + ((af & 0x08) ? 6 : 0);
                    float* pv = &pos[bone].x;
                    for (int k = 0; k < 3; k++) {
                        int16_t off;
                        memcpy(&off, vp + 2 * k, 2);
                        pv[k] = off ? rle_value(vp + off, end, local) * (&bn.posscale.x)[k] : 0;
                        if (!delta) pv[k] += (&bn.pos.x)[k];
                    }
                }
            }
            if (!next) break;
            p += next;
        }
    }
    return true;
}
