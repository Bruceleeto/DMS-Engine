/* Garry's Mod's files as the game sees them: its loose folders, its .vpk
 * packs (and the HL2 / CS:S content it mounts) and workshop addons (.gma, or
 * the older LZMA'd .bin), by lower-case path. A file in an earlier source
 * hides the same path in a later one, as in the game. */
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct VfsEntry {
    int source;             /* index into Vfs::sources */
    uint16_t archive;       /* vpk: which _NNN.vpk, 0x7fff the _dir itself */
    uint64_t offset, size;
    std::vector<uint8_t> preload;   /* vpk: bytes kept in the directory */
    uint16_t method = 0;            /* zip: 8 deflate, 14 LZMA (packed as csize bytes) */
    uint64_t csize = 0;
};

/* Valve's LZMA (a map's lumps): "LZMA", u32 size, u32 packed size, 5 bytes
 * of properties, the stream. False if it isn't that */
bool valve_unlzma(const uint8_t* d, size_t n, std::vector<uint8_t>& out);

struct VfsSource {
    enum Kind { DIR, VPK, GMA, ZIP } kind;
    std::string path;       /* the folder, the _dir.vpk, or the .gma (.bin) */
    std::string label;      /* where it came from, for lists */
    mutable std::vector<uint8_t> blob;  /* a .bin addon, unpacked when first read */
    bool packed = false;                /* a .bin: LZMA'd */
};

class Vfs {
public:
    std::vector<VfsSource> sources;
    std::map<std::string, VfsEntry> files;   /* lower-case, / separated */

    /* Garry's Mod's own install: finds it through Steam's library folders */
    static std::string find_gmod();
    /* Everything the game mounts, from the GarrysMod folder; workshop addons
     * too when with_workshop */
    bool mount_gmod(const std::string& gmod, bool with_workshop = true);
    /* The other Source games installed beside it (Steam's common/): Half-Life 2
     * and its episodes, Counter-Strike: Source, Team Fortress 2. Under what
     * is already mounted; how many */
    int mount_games(const std::string& steam_common);

    bool add_dir(const std::string& dir, const std::string& label);
    bool add_vpk(const std::string& dir_vpk, const std::string& label);
    bool add_gma(const std::string& path, const std::string& label);
    /* A .zip in memory (a map's pakfile: its stored files); its files hide
     * the same paths already mounted, as a map's own do in the game */
    bool add_zip(std::vector<uint8_t> zip, const std::string& label);

    bool exists(const std::string& path) const;
    bool read(const std::string& path, std::vector<uint8_t>& out) const;
    const VfsEntry* find(const std::string& path) const;

    static std::string norm(std::string path);
};
