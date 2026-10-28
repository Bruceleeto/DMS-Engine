/* Source models: .mdl (skeleton, materials, animations), .vvd (vertices and
 * their bone weights), .dx90.vtx (triangles), .ani (animation data kept out
 * of the .mdl). All in Source's own space: Z up, X forward, inches. */
#pragma once
#include "vfs.h"
#include <cstdint>
#include <string>
#include <vector>

struct Quat { float x = 0, y = 0, z = 0, w = 1; };
struct Vec3 { float x = 0, y = 0, z = 0; };

struct MdlBone {
    std::string name;
    int parent = -1;
    Vec3 pos;               /* rest pose, relative to the parent */
    Quat quat;
    Vec3 rot;               /* the same rotation as Euler radians (what animations add to) */
    Vec3 posscale, rotscale;
    float pose_to_bone[12]; /* 3x4: model space to the bone's (the inverse bind) */
};

struct MdlVertex {
    Vec3 pos, normal;
    float u = 0, v = 0;
    uint8_t bone[3] = { 0, 0, 0 };
    float weight[3] = { 1, 0, 0 };
    int bones = 1;
};

struct MdlMesh {
    int material = 0;               /* into Mdl::materials */
    std::vector<uint32_t> indices;  /* triangles, into Mdl::vertices */
};

struct MdlAnim {
    std::string name;
    float fps = 30;
    int frames = 1;
    /* frames x bones, local to the parent */
    std::vector<Vec3> pos;
    std::vector<Quat> rot;
};

struct MdlBodyPart {
    std::string name;
    std::vector<std::string> models;        /* its choices ("" for a blank one) */
};

/* Which of a model's looks: a skin family, a choice per body part */
struct MdlPick {
    int skin = 0;
    std::vector<int> body;                  /* per body part; missing ones are 0 */
};

struct Mdl {
    std::string path, name;
    int version = 0;
    std::vector<MdlBone> bones;
    std::vector<std::string> materials;     /* full .vmt paths under materials/ (found), or the bare name */
    std::vector<MdlVertex> vertices;
    std::vector<MdlMesh> meshes;
    std::vector<std::string> includes;      /* $includemodel: other .mdl files with animations */
    std::vector<MdlBodyPart> bodyparts;
    int skins = 1;                          /* skin families */
    std::vector<uint8_t> raw, ani;          /* the .mdl, the .ani: kept for its animations */
};

/* The model as pick chooses (by default skin 0, each body part's first) */
bool mdl_load(const Vfs& vfs, const std::string& path, Mdl& out, std::string* why = nullptr, const MdlPick& pick = {});
/* Only the skeleton and the animations' names (for an animation-only .mdl) */
bool mdl_load_skeleton(const Vfs& vfs, const std::string& path, Mdl& out, std::string* why = nullptr);

/* Its animations' names, and which are deltas (gestures added on top) */
std::vector<std::string> mdl_anim_names(const Mdl& m, std::vector<bool>* delta = nullptr);
/* Decodes one animation by name into local poses of m's own bones */
bool mdl_decode_anim(const Mdl& m, const std::string& name, MdlAnim& out);
