/* Source maps (.bsp, version 19-21): the world's faces and displacements,
 * their lightmaps (as vertex colours), the props placed in them and the
 * entities. Out in glTF's space (Y up, metres). */
#pragma once
#include "vfs.h"
#include <map>
#include <string>
#include <vector>

/* The world's triangles of one material */
struct BspGroup {
    std::string material;           /* materials/....vmt, as named in the map */
    float tex_w = 256, tex_h = 256; /* the size its texture coordinates are for */
    std::vector<float> pos, nrm, uv;
    std::vector<uint8_t> rgba;      /* the lightmap at each vertex */
    std::vector<uint32_t> idx;
};

struct BspProp {
    std::string model;              /* models/....mdl */
    float pos[3];
    float rot[9];                   /* by columns */
    int skin = 0;
    bool from_entity = false;       /* a prop_physics or prop_dynamic, not prop_static */
};

typedef std::map<std::string, std::string> BspEntity;

struct Bsp {
    std::string path, name;
    std::vector<BspGroup> groups;
    std::vector<BspProp> props;
    std::vector<BspEntity> entities;
    std::string sky;                /* worldspawn's skyname */
    bool has_spawn = false;
    float spawn[3] = { 0, 0, 0 }, spawn_yaw = 0;    /* degrees, as the scene's */
    int sky_faces = 0, sky_props = 0;               /* in the 3D skybox's room, left out */
    size_t tris() const;
};

/* Reads the map and mounts its packed-in files (its own materials) in vfs */
/* light: the map's light made this many times brighter (a game that darkens
 * it again outside a torch's beam, say). ambient: no corner darker than
 * this (0..1, on the screen), so a torch lighting it has something to show. */
bool bsp_load(Vfs& vfs, const std::string& path, Bsp& out, std::string* why = nullptr, float light = 1.0f,
              float ambient = 0.0f);

/* Fewer triangles where it doesn't show: the pieces the map was cut into
 * (by its tree, by its lightmaps' size) joined back up, as long as the
 * shape and the light at the vertices stay within error (metres and 0..1
 * light). Triangles before */
size_t bsp_simplify(Bsp& b, float error);

/* Source's (pitch, yaw, roll) as a rotation in glTF's space, by columns */
void bsp_angles(const float ang[3], float rot[9]);
/* The scene's Euler degrees for a rotation by columns (dc_scene_rot_degrees) */
void bsp_rot_degrees(const float r[9], float out[3]);
