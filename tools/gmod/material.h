/* Source's materials (.vmt, KeyValues text) and textures (.vtf) */
#pragma once
#include "vfs.h"
#include <cstdint>
#include <string>
#include <vector>

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;      /* w * h * 4, top row first */
};

/* The largest mip of a .vtf no bigger than max_side (0: the largest), frame
 * and face 0, as RGBA. False for a format it does not know. */
bool vtf_decode(const std::vector<uint8_t>& vtf, int max_side, Image& out, std::string* why = nullptr);

struct Material {
    std::string shader;             /* lower case: vertexlitgeneric, unlitgeneric, eyes... */
    std::string basetexture;        /* under materials/, no extension */
    bool alphatest = false, translucent = false, additive = false, nocull = false, selfillum = false;
};

/* A .vmt under materials/ (path with or without it, no extension); patch
 * materials are followed to what they include. */
bool vmt_load(const Vfs& vfs, const std::string& path, Material& out);
