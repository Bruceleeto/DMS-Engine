#include "vfs.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <lzma.h>
#include <zlib.h>
#include <sys/stat.h>

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? n : 0);
    bool ok = n <= 0 || fread(out.data(), 1, n, f) == (size_t)n;
    fclose(f);
    return ok;
}

static bool is_dir(const std::string& p) {
    struct stat st;
    return !stat(p.c_str(), &st) && S_ISDIR(st.st_mode);
}

static std::vector<std::string> list_dir(const std::string& dir) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.') out.push_back(e->d_name);
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

static bool ends_with(const std::string& s, const char* end) {
    size_t n = strlen(end);
    return s.size() >= n && !s.compare(s.size() - n, n, end);
}

std::string Vfs::norm(std::string p) {
    for (char& c : p) {
        if (c == '\\') c = '/';
        c = (char)tolower((unsigned char)c);
    }
    std::string out;
    for (char c : p) if (!(c == '/' && !out.empty() && out.back() == '/')) out += c;
    while (!out.empty() && out[0] == '/') out.erase(0, 1);
    return out;
}

std::string Vfs::find_gmod() {
    const char* home = getenv("HOME");
    if (!home) return "";
    std::vector<std::string> libs;
    for (const char* rel : { "/.steam/steam", "/.steam/debian-installation", "/.local/share/Steam",
                             "/.var/app/com.valvesoftware.Steam/.local/share/Steam" }) {
        std::string root = std::string(home) + rel;
        libs.push_back(root);
        std::vector<uint8_t> vdf;
        if (!read_file(root + "/steamapps/libraryfolders.vdf", vdf)) continue;
        std::string t(vdf.begin(), vdf.end());
        for (size_t at = t.find("\"path\""); at != std::string::npos; at = t.find("\"path\"", at + 1)) {
            size_t a = t.find('"', at + 6), b = a == std::string::npos ? a : t.find('"', a + 1);
            if (b != std::string::npos) libs.push_back(t.substr(a + 1, b - a - 1));
        }
    }
    for (const std::string& l : libs) {
        std::string g = l + "/steamapps/common/GarrysMod";
        if (is_dir(g + "/garrysmod")) return g;
    }
    return "";
}

bool Vfs::mount_gmod(const std::string& g, bool with_workshop) {
    std::string gm = g + "/garrysmod";
    if (!is_dir(gm)) return false;
    add_dir(gm, "garrysmod");
    for (const std::string& f : list_dir(gm))
        if (ends_with(f, "_dir.vpk")) add_vpk(gm + "/" + f, "garrysmod/" + f.substr(0, f.size() - 8));
    std::string addons = gm + "/addons";
    for (const std::string& f : list_dir(addons))
        if (ends_with(f, ".gma")) add_gma(addons + "/" + f, "addons/" + f);
    for (const std::string& f : list_dir(g + "/sourceengine"))
        if (ends_with(f, "_dir.vpk")) add_vpk(g + "/sourceengine/" + f, f.substr(0, f.size() - 8));
    if (with_workshop) {
        std::string ws = g + "/../../workshop/content/4000";
        for (const std::string& id : list_dir(ws))
            for (const std::string& f : list_dir(ws + "/" + id))
                if (ends_with(f, ".gma") || ends_with(f, ".bin")) add_gma(ws + "/" + id + "/" + f, "workshop/" + id);
    }
    return true;
}

int Vfs::mount_games(const std::string& common) {
    /* Each game's folder, its label (the first word is which game, for lists) */
    static const char* const GAMES[][2] = {
        { "Half-Life 2/hl2", "hl2" },           { "Half-Life 2/episodic", "hl2/episodic" },
        { "Half-Life 2/ep2", "hl2/ep2" },        { "Half-Life 2/lostcoast", "hl2/lostcoast" },
        { "Counter-Strike Source/cstrike", "cstrike" }, { "Team Fortress 2/tf", "tf" },
        { "Portal/portal", "portal" },
        { "left 4 dead/left4dead", "l4d" },     { "left 4 dead/left4dead_dlc3", "l4d/dlc3" },
    };
    int n = 0;
    for (auto& g : GAMES) {
        std::string dir = common + "/" + g[0];
        if (!is_dir(dir)) continue;
        add_dir(dir, g[1]);
        for (const std::string& f : list_dir(dir))
            if (ends_with(f, "_dir.vpk")) add_vpk(dir + "/" + f, std::string(g[1]) + "/" + f.substr(0, f.size() - 8));
        n++;
    }
    return n;
}

static void walk(Vfs& v, int src, const std::string& root, const std::string& rel) {
    for (const std::string& f : list_dir(root + "/" + rel)) {
        std::string r = rel.empty() ? f : rel + "/" + f;
        std::string full = root + "/" + r;
        if (is_dir(full)) {
            /* Only what holds assets: not lua, saves, caches */
            if (rel.empty() && f != "models" && f != "materials" && f != "maps") continue;
            walk(v, src, root, r);
            continue;
        }
        struct stat st;
        if (stat(full.c_str(), &st)) continue;
        std::string key = Vfs::norm(r);
        if (v.files.count(key)) continue;
        VfsEntry e{};
        e.source = src;
        e.offset = 0;
        e.size = (uint64_t)st.st_size;
        v.files[key] = e;
    }
}

bool Vfs::add_dir(const std::string& dir, const std::string& label) {
    if (!is_dir(dir)) return false;
    sources.push_back({ VfsSource::DIR, dir, label, {} });
    walk(*this, (int)sources.size() - 1, dir, "");
    return true;
}

bool Vfs::add_vpk(const std::string& path, const std::string& label) {
    std::vector<uint8_t> d;
    if (!read_file(path, d) || d.size() < 12) return false;
    uint32_t sig, ver, tree;
    memcpy(&sig, &d[0], 4);
    memcpy(&ver, &d[4], 4);
    memcpy(&tree, &d[8], 4);
    if (sig != 0x55aa1234 || (ver != 1 && ver != 2)) return false;
    size_t hdr = ver == 2 ? 28 : 12, p = hdr, end = hdr + tree;
    if (end > d.size()) return false;
    sources.push_back({ VfsSource::VPK, path, label, {} });
    int src = (int)sources.size() - 1;
    auto str = [&]() {
        std::string s;
        while (p < end && d[p]) s += (char)d[p++];
        p++;
        return s;
    };
    for (;;) {
        std::string ext = str();
        if (ext.empty() || p >= end) break;
        for (;;) {
            std::string dir = str();
            if (dir.empty()) break;
            for (;;) {
                std::string name = str();
                if (name.empty()) break;
                if (p + 18 > end) return true;
                uint16_t pre, arc;
                uint32_t off, len;
                memcpy(&pre, &d[p + 4], 2);
                memcpy(&arc, &d[p + 6], 2);
                memcpy(&off, &d[p + 8], 4);
                memcpy(&len, &d[p + 12], 4);
                p += 18;
                VfsEntry e{};
                e.source = src;
                e.archive = arc;
                e.offset = arc == 0x7fff ? end + off : off;
                e.size = len;
                e.preload.assign(d.begin() + p, d.begin() + p + pre);
                p += pre;
                std::string key = norm((dir == " " ? "" : dir + "/") + name + (ext == " " ? "" : "." + ext));
                if (!files.count(key)) files[key] = std::move(e);
            }
        }
    }
    return true;
}

/* A workshop .bin: an LZMA-alone stream of a .gma */
static bool unlzma(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    lzma_stream s = LZMA_STREAM_INIT;
    if (lzma_alone_decoder(&s, UINT64_MAX) != LZMA_OK) return false;
    s.next_in = in.data();
    s.avail_in = in.size();
    out.clear();
    uint8_t buf[1 << 16];
    lzma_ret r;
    do {
        s.next_out = buf;
        s.avail_out = sizeof(buf);
        r = lzma_code(&s, LZMA_FINISH);
        out.insert(out.end(), buf, buf + (sizeof(buf) - s.avail_out));
    } while (r == LZMA_OK);
    lzma_end(&s);
    return r == LZMA_STREAM_END;
}

/* A raw LZMA stream with its properties and size, as an LZMA-alone stream */
static bool unlzma_raw(const uint8_t* props, const uint8_t* d, size_t n, uint64_t size, std::vector<uint8_t>& out) {
    std::vector<uint8_t> alone(props, props + 5);
    for (int i = 0; i < 8; i++) alone.push_back((uint8_t)(size >> (8 * i)));
    alone.insert(alone.end(), d, d + n);
    return unlzma(alone, out) || out.size() == size;
}

bool valve_unlzma(const uint8_t* d, size_t n, std::vector<uint8_t>& out) {
    if (n < 17 || memcmp(d, "LZMA", 4)) return false;
    uint32_t size, packed;
    memcpy(&size, d + 4, 4);
    memcpy(&packed, d + 8, 4);
    if (17 + (size_t)packed > n) return false;
    return unlzma_raw(d + 12, d + 17, packed, size, out) && out.size() >= size && (out.resize(size), true);
}

static bool inflate_raw(const uint8_t* d, size_t n, uint64_t size, std::vector<uint8_t>& out) {
    out.resize(size);
    z_stream z{};
    if (inflateInit2(&z, -15) != Z_OK) return false;
    z.next_in = (Bytef*)d;
    z.avail_in = (uInt)n;
    z.next_out = out.data();
    z.avail_out = (uInt)size;
    int r = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return r == Z_STREAM_END;
}

struct GmaFile { std::string name; uint64_t offset, size; };

/* A .gma's name and files (their offsets into it) */
static bool parse_gma(const std::vector<uint8_t>& d, std::string& name, std::vector<GmaFile>& list) {
    if (d.size() < 5 || memcmp(d.data(), "GMAD", 4)) return false;
    size_t p = 4;
    uint8_t ver = d[p++];
    p += 16;                                    /* steam id, timestamp */
    auto str = [&]() {
        std::string s;
        while (p < d.size() && d[p]) s += (char)d[p++];
        p++;
        return s;
    };
    if (ver > 1) while (!str().empty()) {}      /* required content */
    name = str();
    str();                                      /* description */
    str();                                      /* author */
    p += 4;                                     /* addon version */
    while (p + 4 <= d.size()) {
        uint32_t n;
        memcpy(&n, &d[p], 4);
        p += 4;
        if (!n) break;
        GmaFile f;
        f.name = str();
        memcpy(&f.size, &d[p], 8);
        p += 12;                                /* size, crc */
        list.push_back(f);
    }
    uint64_t off = p;
    for (GmaFile& f : list) { f.offset = off; off += f.size; }
    return true;
}

/* A .bin's list of files, kept in ~/.cache/dms-gmod so it is unpacked only
 * when something in it is read: named for the file, stale when its size or
 * time changes */
static std::string index_path(const std::string& path) {
    const char* home = getenv("XDG_CACHE_HOME");
    std::string dir = home && *home ? home : std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.cache";
    mkdir(dir.c_str(), 0755);
    dir += "/dms-gmod";
    mkdir(dir.c_str(), 0755);
    uint64_t h = 1469598103934665603ull;        /* FNV-1a */
    for (char c : path) h = (h ^ (uint8_t)c) * 1099511628211ull;
    char b[32];
    snprintf(b, sizeof(b), "/%016llx.idx", (unsigned long long)h);
    return dir + b;
}

static bool load_index(const std::string& idx, const std::string& stamp, std::string& name, std::vector<GmaFile>& list) {
    FILE* f = fopen(idx.c_str(), "r");
    if (!f) return false;
    char line[4096];
    bool ok = fgets(line, sizeof(line), f) && stamp + "\n" == line && fgets(line, sizeof(line), f);
    if (ok) {
        name = line;
        name.pop_back();
        while (fgets(line, sizeof(line), f)) {
            unsigned long long off, size;
            int n = 0;
            if (sscanf(line, "%llu %llu %n", &off, &size, &n) != 2) { ok = false; break; }
            std::string p = line + n;
            if (!p.empty() && p.back() == '\n') p.pop_back();
            list.push_back({ p, off, size });
        }
    }
    fclose(f);
    return ok;
}

static void save_index(const std::string& idx, const std::string& stamp, const std::string& name, const std::vector<GmaFile>& list) {
    std::string tmp = idx + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return;
    fprintf(f, "%s\n%s\n", stamp.c_str(), name.c_str());
    for (const GmaFile& g : list) fprintf(f, "%llu %llu %s\n", (unsigned long long)g.offset, (unsigned long long)g.size, g.name.c_str());
    if (fclose(f) == 0) rename(tmp.c_str(), idx.c_str());
}

bool Vfs::add_gma(const std::string& path, const std::string& label) {
    bool packed = ends_with(path, ".bin");
    std::string name;
    std::vector<GmaFile> list;
    if (packed) {
        struct stat st;
        if (stat(path.c_str(), &st)) return false;
        std::string stamp = "dmsgma1 " + std::to_string((long long)st.st_size) + " " + std::to_string((long long)st.st_mtime);
        std::string idx = index_path(path);
        if (!load_index(idx, stamp, name, list)) {
            name.clear();
            list.clear();
            std::vector<uint8_t> d, raw;
            if (!read_file(path, d) || !unlzma(d, raw) || !parse_gma(raw, name, list)) return false;
            save_index(idx, stamp, name, list);
        }
    } else {
        /* Only its list of files is read, the start of it: a .gma's files are
         * read where they lie */
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        std::vector<uint8_t> d;
        bool ok = false;
        for (size_t want = 64 << 10; !ok; want *= 4) {
            d.resize(want);
            fseek(f, 0, SEEK_SET);
            d.resize(fread(d.data(), 1, want, f));
            list.clear();
            ok = parse_gma(d, name, list);
            /* the list ends before what was read: whole */
            if (ok && d.size() == want && (list.empty() ? d.size() : list.front().offset) >= d.size()) ok = false;
            if (d.size() < want) break;
        }
        fclose(f);
        if (!ok) return false;
    }
    VfsSource s{ VfsSource::GMA, path, label + (name.empty() ? "" : " (" + name + ")"), {} };
    s.packed = packed;
    sources.push_back(s);
    int src = (int)sources.size() - 1;
    for (const GmaFile& f : list) {
        std::string key = norm(f.name);
        if (!files.count(key)) {
            VfsEntry e{};
            e.source = src;
            e.offset = f.offset;
            e.size = f.size;
            files[key] = e;
        }
    }
    return true;
}

bool Vfs::add_zip(std::vector<uint8_t> z, const std::string& label) {
    auto u16 = [&](size_t o) { return o + 2 <= z.size() ? (uint32_t)(z[o] | z[o + 1] << 8) : 0u; };
    auto u32 = [&](size_t o) { return o + 4 <= z.size() ? (uint32_t)(u16(o) | u16(o + 2) << 16) : 0u; };
    /* The end of the central directory, the last thing in it */
    if (z.size() < 22) return false;
    size_t end = z.size() - 22;
    while (end > 0 && u32(end) != 0x06054b50) end--;
    if (u32(end) != 0x06054b50) return false;
    int count = (int)u16(end + 10);
    size_t at = u32(end + 16);
    sources.push_back({ VfsSource::ZIP, "", label, {} });
    int src = (int)sources.size() - 1;
    int skipped = 0;
    for (int i = 0; i < count && u32(at) == 0x02014b50; i++) {
        uint32_t method = u16(at + 10), csize = u32(at + 20), size = u32(at + 24);
        uint32_t nlen = u16(at + 28), xlen = u16(at + 30), clen = u16(at + 32), local = u32(at + 42);
        std::string name(z.begin() + at + 46, z.begin() + std::min(z.size(), (size_t)(at + 46 + nlen)));
        at += 46 + nlen + xlen + clen;
        if (method != 0 && method != 8 && method != 14) { skipped++; continue; }
        size_t data = local + 30 + u16(local + 26) + u16(local + 28);
        if (data + csize > z.size()) continue;
        VfsEntry e{};
        e.source = src;
        e.offset = data;
        e.size = size;
        e.method = (uint16_t)method;
        e.csize = csize;
        files[norm(name)] = e;
    }
    if (skipped) fprintf(stderr, "%s: %d files packed in a way not read, left out\n", label.c_str(), skipped);
    sources[src].blob.swap(z);
    return true;
}

const VfsEntry* Vfs::find(const std::string& path) const {
    auto it = files.find(norm(path));
    return it == files.end() ? nullptr : &it->second;
}

bool Vfs::exists(const std::string& path) const { return find(path) != nullptr; }

bool Vfs::read(const std::string& path, std::vector<uint8_t>& out) const {
    const VfsEntry* e = find(path);
    if (!e) return false;
    const VfsSource& s = sources[e->source];
    out = e->preload;
    if (!e->size) return true;
    if (s.packed && s.blob.empty()) {
        std::vector<uint8_t> d;
        if (!read_file(s.path, d) || !unlzma(d, s.blob)) return false;
    }
    if (s.kind == VfsSource::ZIP && e->method) {
        if (e->offset + e->csize > s.blob.size()) return false;
        const uint8_t* d = s.blob.data() + e->offset;
        /* Zip's LZMA: u16 version, u16 properties' size, the properties, the stream */
        if (e->method == 14)
            return e->csize > 9 && unlzma_raw(d + 4, d + 9, e->csize - 9, e->size, out) && out.size() >= e->size &&
                   (out.resize(e->size), true);
        return inflate_raw(d, e->csize, e->size, out);
    }
    if (s.packed || s.kind == VfsSource::ZIP) {
        if (e->offset + e->size > s.blob.size()) return false;
        out.insert(out.end(), s.blob.begin() + e->offset, s.blob.begin() + e->offset + e->size);
        return true;
    }
    std::string file = s.path;
    if (s.kind == VfsSource::DIR) {
        /* The real name may not be lower case: find it a piece at a time */
        std::string rel = norm(path), cur = s.path;
        size_t at = 0;
        while (at <= rel.size()) {
            size_t slash = rel.find('/', at);
            std::string piece = rel.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
            std::string match;
            for (const std::string& f : list_dir(cur)) {
                std::string l = f;
                for (char& c : l) c = (char)tolower((unsigned char)c);
                if (l == piece) { match = f; break; }
            }
            if (match.empty()) return false;
            cur += "/" + match;
            if (slash == std::string::npos) break;
            at = slash + 1;
        }
        file = cur;
    } else if (s.kind == VfsSource::VPK && e->archive != 0x7fff) {
        char num[16];
        snprintf(num, sizeof(num), "_%03u.vpk", e->archive);
        file = s.path.substr(0, s.path.size() - 8) + num;
    }
    FILE* f = fopen(file.c_str(), "rb");
    if (!f) return false;
    size_t at = out.size();
    out.resize(at + e->size);
    fseeko(f, (off_t)e->offset, SEEK_SET);
    bool ok = fread(out.data() + at, 1, e->size, f) == e->size;
    fclose(f);
    return ok;
}
