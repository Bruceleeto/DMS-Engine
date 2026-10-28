/* Models from elsewhere into the open project: the Library (every .glb in
 * the other examples and games) and files dropped on the window.
 *
 * A .glb is copied to assets/<stem>/ (a new folder if one by that name holds
 * a different file), with any files it names by uri; make converts it to
 * pc/<stem>/<file>.dms in the background, and then it goes into the scene.
 * A .png goes to assets/textures/ and is converted to a .dt. */
#include "editor.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <algorithm>
#include <deque>

namespace {

struct Job {
    std::string target;     /* for make, under the project */
    std::string model;      /* under pc/, to add when made; "" for a texture */
    std::string what;       /* for the log */
    bool placed;
    shz_vec3_t at;
};
std::deque<Job> jobs;

bool exists(const std::string& p) { struct stat st; return !stat(p.c_str(), &st); }
long long mtime(const std::string& p) { struct stat st; return stat(p.c_str(), &st) ? -1 : (long long)st.st_mtime; }

std::string real(const std::string& p) {
    char buf[PATH_MAX];
    return realpath(p.c_str(), buf) ? std::string(buf) : p;
}

bool read_file(const std::string& p, std::string& out) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

bool mkdirs(const std::string& dir) {
    for (size_t i = 1; i <= dir.size(); i++)
        if (i == dir.size() || dir[i] == '/') {
            std::string d = dir.substr(0, i);
            if (mkdir(d.c_str(), 0755) && errno != EEXIST) return false;
        }
    return true;
}

bool write_file(const std::string& p, const std::string& data) {
    size_t s = p.find_last_of('/');
    if (s != std::string::npos && !mkdirs(p.substr(0, s))) return false;
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    return fclose(f) == 0 && ok;
}

/* make takes no spaces and the like in a target */
std::string clean_name(const std::string& n) {
    std::string out = n;
    for (char& c : out)
        if (!isalnum((unsigned char)c) && c != '_' && c != '-' && c != '.') c = '_';
    return out;
}

std::string dir_of(const std::string& p) { size_t s = p.find_last_of('/'); return s == std::string::npos ? "." : p.substr(0, s); }
std::string file_of(const std::string& p) { size_t s = p.find_last_of('/'); return s == std::string::npos ? p : p.substr(s + 1); }
std::string stem_of(const std::string& f) { size_t d = f.find_last_of('.'); return d == std::string::npos ? f : f.substr(0, d); }
std::string ext_of(const std::string& f) {
    size_t d = f.find_last_of('.');
    std::string e = d == std::string::npos ? "" : f.substr(d);
    for (char& c : e) c = (char)tolower((unsigned char)c);
    return e;
}

/* The files a .glb names by uri (images, buffers), from its JSON chunk:
 * plain relative paths only, not data: or anything above the folder */
std::vector<std::string> glb_uris(const std::string& glb) {
    std::vector<std::string> uris;
    if (glb.size() < 20 || glb.compare(0, 4, "glTF") != 0) return uris;
    uint32_t len;
    memcpy(&len, glb.data() + 12, 4);
    if (20 + (size_t)len > glb.size()) return uris;
    std::string json = glb.substr(20, len);
    for (size_t at = 0; (at = json.find("\"uri\"", at)) != std::string::npos;) {
        at += 5;
        size_t q = json.find_first_not_of(" \t\r\n:", at);
        if (q == std::string::npos || json[q] != '"') continue;
        size_t e = json.find('"', q + 1);
        if (e == std::string::npos) break;
        std::string u = json.substr(q + 1, e - q - 1);
        at = e + 1;
        if (u.empty() || u.compare(0, 5, "data:") == 0 || u[0] == '/' || u.find("..") != std::string::npos ||
            u.find("://") != std::string::npos || u.find('\\') != std::string::npos || u.find('%') != std::string::npos)
            continue;
        if (std::find(uris.begin(), uris.end(), u) == uris.end()) uris.push_back(u);
    }
    return uris;
}

/* Where under assets/ <file> goes: <base>/, or <base>_2/... if that holds a
 * different one. same: the very file is there already. */
std::string pick_dir(const std::string& base, const std::string& file, const std::string& data, bool* same) {
    for (int k = 1;; k++) {
        std::string dir = k == 1 ? base : base + "_" + std::to_string(k);
        std::string path = ed.project + "assets/" + dir + "/" + file, have;
        if (!exists(path)) { *same = false; return dir; }
        if (read_file(path, have) && have == data) { *same = true; return dir; }
    }
}

void import_glb(const std::string& src, const shz_vec3_t* at) {
    std::string data;
    if (!read_file(src, data)) { ed_log("Import: cannot read %s", src.c_str()); return; }
    std::string file = clean_name(file_of(src));
    if (ext_of(file) != ".glb") file = stem_of(file) + ".glb";
    std::string stem = stem_of(file);
    bool same;
    std::string dir = pick_dir(stem, file, data, &same);
    std::string adir = ed.project + "assets/" + dir + "/";
    if (!same) {
        if (!write_file(adir + file, data)) { ed_log("Import: cannot write %sassets/%s/%s", ed.project.c_str(), dir.c_str(), file.c_str()); return; }
        int extra = 0;
        /* what it names by uri, and its import settings, if any, beside it */
        for (auto& u : glb_uris(data)) {
            std::string side;
            if (!read_file(dir_of(src) + "/" + u, side)) { ed_log("Import warning: %s names %s, which is not there", file.c_str(), u.c_str()); continue; }
            /* strippy looks for them beside the copy it converts, in pc/ */
            if (write_file(adir + u, side) && write_file(ed.project + "pc/" + dir + "/" + u, side)) extra++;
            else ed_log("Import warning: cannot copy %s", u.c_str());
        }
        std::string settings;
        if (read_file(dir_of(src) + "/" + stem_of(file_of(src)) + ".import.json", settings) &&
            write_file(adir + stem + ".import.json", settings))
            extra++;
        ed_log("Import: %s -> assets/%s/%s%s", src.c_str(), dir.c_str(), file.c_str(),
               extra ? (" (+" + std::to_string(extra) + " files)").c_str() : "");
    } else {
        ed_log("Import: %s is in the project already (assets/%s/)", file.c_str(), dir.c_str());
    }
    Job j{ "pc/" + dir + "/" + stem + ".dms", dir + "/" + stem + ".dms", file, at != nullptr, at ? *at : shz_vec3_init(0, 0, 0) };
    jobs.push_back(j);
    if (ed_job()) ed_log("Import: %s waits for %s", file.c_str(), ed_job());
}

void import_png(const std::string& src) {
    std::string data;
    if (!read_file(src, data)) { ed_log("Import: cannot read %s", src.c_str()); return; }
    std::string file = clean_name(file_of(src));
    std::string stem = stem_of(file);
    file = stem + ".png";
    /* textures/<file>, or textures/<stem>_2.png... if a different one has the name */
    std::string name = stem;
    for (int k = 2;; k++) {
        std::string path = ed.project + "assets/textures/" + name + ".png", have;
        if (!exists(path)) {
            if (!write_file(path, data)) { ed_log("Import: cannot write %s", path.c_str()); return; }
            ed_log("Import: %s -> assets/textures/%s.png", src.c_str(), name.c_str());
            break;
        }
        if (read_file(path, have) && have == data) { ed_log("Import: assets/textures/%s.png is that already", name.c_str()); break; }
        name = stem + "_" + std::to_string(k);
    }
    jobs.push_back(Job{ "pc/textures/" + name + ".dt", "", name + ".png", false, shz_vec3_init(0, 0, 0) });
}

void scan_glbs(const std::string& dir, const std::string& rel, std::vector<std::string>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string name = e->d_name, path = dir + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st)) continue;
        if (S_ISDIR(st.st_mode)) scan_glbs(path, rel + name + "/", out);
        else if (ext_of(name) == ".glb") out.push_back(rel + name);
    }
    closedir(d);
}

}  // namespace

void ed_import_file(const std::string& path, const shz_vec3_t* at) {
    std::string ext = ext_of(path);
    if (ext == ".glb") import_glb(path, at);
    else if (ext == ".png") import_png(path);
    else ed_log("Import: %s is not a .glb or .png; nothing done", file_of(path).c_str());
}

void ed_import_poll() {
    if (jobs.empty() || ed_job()) return;
    Job j = jobs.front();
    jobs.pop_front();
    std::string built = ed.project + j.target, source = ed.project + "assets/" + j.target.substr(3);
    if (!j.model.empty()) source = source.substr(0, source.size() - 4) + ".glb";
    else source = source.substr(0, source.size() - 3) + ".png";
    auto done = [j](bool ok) {
        if (!ok) { ed_log("Import of %s failed (see above); its copy stays in assets/", j.what.c_str()); return; }
        if (j.model.empty()) { ed_log("Imported %s: %s", j.what.c_str(), j.target.c_str()); return; }
        if (!ed.scene) return;
        ed_add_model(j.model);
        if (j.placed && ed.sel.kind == SelKind::Object) ed.scene->objects[ed.sel.index].pos = j.at;
        ed_log("Imported %s as %s", j.what.c_str(), j.model.c_str());
    };
    /* converted already, and since the source last changed */
    if (mtime(built) >= mtime(source) && mtime(built) >= 0) { ed_scan_assets(); done(true); return; }
    ed_log("Import: converting %s", j.what.c_str());
    ed_run(j.target, done);
}

std::vector<std::string> ed_library() {
    std::vector<std::string> out;
    std::string mine = real(ed.project);
    if (!mine.empty() && mine.back() == '/') mine.pop_back();
    for (const char* group : { "examples", "games" }) {
        DIR* d = opendir((ed.root + group).c_str());
        if (!d) continue;
        std::vector<std::string> names;
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.') names.push_back(e->d_name);
        closedir(d);
        std::sort(names.begin(), names.end());
        for (auto& n : names) {
            std::string proj = ed.root + group + "/" + n;
            if (real(proj) == mine) continue;
            std::vector<std::string> found;
            scan_glbs(proj + "/assets", std::string(group) + "/" + n + "/assets/", found);
            std::sort(found.begin(), found.end());
            out.insert(out.end(), found.begin(), found.end());
        }
    }
    return out;
}
