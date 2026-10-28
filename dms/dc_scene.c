#include "dc_scene.h"
#include "dc_draw.h"
#include "dc_engine.h"
#include "pvrtex.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * JSON: parsed into a flat array of nodes. Objects and arrays point at their
 * first child, every node at its next sibling; strings are unescaped in place
 * in a copy of the text.
 * ================================================================ */

enum { J_NULL, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ };

typedef struct {
    uint8_t     type;
    const char* key;        /* in an object: the member's name */
    const char* str;
    double      num;
    int         child, next;    /* -1: none */
} JNode;

typedef struct {
    char*  p;
    JNode* nodes;
    int    count, cap;
    bool   bad;
} JParser;

static int j_node(JParser* j, int type) {
    if (j->count == j->cap) {
        int cap = j->cap ? j->cap * 2 : 64;
        JNode* n = realloc(j->nodes, cap * sizeof(JNode));
        if (!n) { j->bad = true; return -1; }
        j->nodes = n;
        j->cap = cap;
    }
    JNode* n = &j->nodes[j->count];
    memset(n, 0, sizeof(*n));
    n->type = type;
    n->child = n->next = -1;
    return j->count++;
}

static void j_space(JParser* j) {
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r') j->p++;
}

/* A string at j->p (past the quote), unescaped where it is. \u gives '?'
 * past ASCII: names and paths are ASCII. */
static char* j_string(JParser* j) {
    char *start = j->p, *out = j->p;
    while (*j->p && *j->p != '"') {
        char c = *j->p++;
        if (c == '\\') {
            c = *j->p++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                unsigned v = 0;
                for (int i = 0; i < 4 && *j->p; i++) {
                    char h = *j->p++;
                    v = v * 16 + (h >= 'a' ? h - 'a' + 10 : h >= 'A' ? h - 'A' + 10 : h - '0');
                }
                c = v < 128 ? (char)v : '?';
                break;
            }
            case 0: j->bad = true; return start;
            default: break;     /* \" \\ \/ */
            }
        }
        *out++ = c;
    }
    if (*j->p != '"') { j->bad = true; return start; }
    j->p++;
    *out = 0;
    return start;
}

static int j_value(JParser* j, int depth) {
    j_space(j);
    if (depth > 32) { j->bad = true; return -1; }
    char c = *j->p;
    if (c == '{' || c == '[') {
        bool obj = c == '{';
        int n = j_node(j, obj ? J_OBJ : J_ARR), last = -1;
        if (n < 0) return -1;
        j->p++;
        j_space(j);
        if (*j->p == (obj ? '}' : ']')) { j->p++; return n; }
        for (;;) {
            const char* key = NULL;
            if (obj) {
                j_space(j);
                if (*j->p != '"') { j->bad = true; return -1; }
                j->p++;
                key = j_string(j);
                j_space(j);
                if (*j->p != ':') { j->bad = true; return -1; }
                j->p++;
            }
            int v = j_value(j, depth + 1);
            if (v < 0 || j->bad) { j->bad = true; return -1; }
            j->nodes[v].key = key;
            if (last < 0) j->nodes[n].child = v;
            else j->nodes[last].next = v;
            last = v;
            j_space(j);
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p == (obj ? '}' : ']')) { j->p++; return n; }
            j->bad = true;
            return -1;
        }
    }
    if (c == '"') {
        int n = j_node(j, J_STR);
        if (n < 0) return -1;
        j->p++;
        j->nodes[n].str = j_string(j);
        return n;
    }
    if (!strncmp(j->p, "true", 4))  { j->p += 4; return j_node(j, J_TRUE); }
    if (!strncmp(j->p, "false", 5)) { j->p += 5; return j_node(j, J_FALSE); }
    if (!strncmp(j->p, "null", 4))  { j->p += 4; return j_node(j, J_NULL); }
    char* end;
    double v = strtod(j->p, &end);
    if (end == j->p) { j->bad = true; return -1; }
    j->p = end;
    int n = j_node(j, J_NUM);
    if (n >= 0) j->nodes[n].num = v;
    return n;
}

static const JNode* j_get(const JParser* j, int obj, const char* key) {
    if (obj < 0 || j->nodes[obj].type != J_OBJ) return NULL;
    for (int c = j->nodes[obj].child; c >= 0; c = j->nodes[c].next)
        if (!strcmp(j->nodes[c].key, key)) return &j->nodes[c];
    return NULL;
}

static int j_index(const JParser* j, const JNode* n) { return n ? (int)(n - j->nodes) : -1; }

static float j_num(const JParser* j, int obj, const char* key, float def) {
    const JNode* n = j_get(j, obj, key);
    return n && n->type == J_NUM ? (float)n->num : def;
}

static bool j_bool(const JParser* j, int obj, const char* key, bool def) {
    const JNode* n = j_get(j, obj, key);
    return n && (n->type == J_TRUE || n->type == J_FALSE) ? n->type == J_TRUE : def;
}

static void j_str(const JParser* j, int obj, const char* key, char* out, size_t size) {
    const JNode* n = j_get(j, obj, key);
    if (n && n->type == J_STR) snprintf(out, size, "%s", n->str);
}

static shz_vec3_t j_vec3(const JParser* j, int obj, const char* key, shz_vec3_t def) {
    const JNode* n = j_get(j, obj, key);
    if (!n || n->type != J_ARR) return def;
    float v[3] = { def.x, def.y, def.z };
    int i = 0;
    for (int c = n->child; c >= 0 && i < 3; c = j->nodes[c].next, i++)
        if (j->nodes[c].type == J_NUM) v[i] = (float)j->nodes[c].num;
    return shz_vec3_init(v[0], v[1], v[2]);
}

/* "#rrggbb" */
static uint32_t j_colour(const JParser* j, int obj, const char* key, uint32_t def) {
    const JNode* n = j_get(j, obj, key);
    if (!n || n->type != J_STR || n->str[0] != '#' || strlen(n->str) != 7) return def;
    return (uint32_t)strtoul(n->str + 1, NULL, 16);
}

/* ================================================================
 * Loading
 * ================================================================ */

DCScene* dc_scene_new(const char* root) {
    DCScene* s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    snprintf(s->root, sizeof(s->root), "%s", root ? root : "");
    s->cam_pos = shz_vec3_init(0.0f, 2.0f, -6.0f);
    s->cam_fov = 60.0f;
    s->fog_near = 50.0f;
    s->fog_far = 400.0f;
    s->fog_rgb = 0x808080;
    return s;
}

static DCSceneObject* add_object(DCScene* s) {
    if (s->object_count == s->object_cap) {
        int cap = s->object_cap ? s->object_cap * 2 : 16;
        DCSceneObject* o = realloc(s->objects, cap * sizeof(*o));
        if (!o) return NULL;
        s->objects = o;
        s->object_cap = cap;
    }
    DCSceneObject* o = &s->objects[s->object_count++];
    memset(o, 0, sizeof(*o));
    o->scale = shz_vec3_init(1.0f, 1.0f, 1.0f);
    o->tint = 0xFFFFFF;
    return o;
}

DCScene* dc_scene_parse(const char* json, const char* root) {
    JParser j = { 0 };
    char* text = strdup(json);
    if (!text) return NULL;
    j.p = text;
    int top = j_value(&j, 0);
    if (j.bad || top < 0 || j.nodes[top].type != J_OBJ) {
        printf("dc_scene: not a JSON object (near \"%.20s\")\n", j.p);
        free(j.nodes); free(text);
        return NULL;
    }

    DCScene* s = dc_scene_new(root);
    if (!s) { free(j.nodes); free(text); return NULL; }
    s->background = j_colour(&j, top, "background", 0x000000);

    int fog = j_index(&j, j_get(&j, top, "fog"));
    if (fog >= 0 && j.nodes[fog].type == J_OBJ) {
        s->fog = true;
        s->fog_rgb = j_colour(&j, fog, "color", s->fog_rgb);
        s->fog_near = j_num(&j, fog, "near", s->fog_near);
        s->fog_far = j_num(&j, fog, "far", s->fog_far);
    }

    int cam = j_index(&j, j_get(&j, top, "camera"));
    s->cam_pos = j_vec3(&j, cam, "pos", s->cam_pos);
    s->cam_yaw = j_num(&j, cam, "yaw", 0.0f);
    s->cam_pitch = j_num(&j, cam, "pitch", 0.0f);
    s->cam_fov = j_num(&j, cam, "fov", 60.0f);

    const JNode* lights = j_get(&j, top, "lights");
    if (lights && lights->type == J_ARR)
        for (int c = lights->child; c >= 0 && s->light_count < DC_MAX_LIGHTS; c = j.nodes[c].next) {
            DCSceneLight* l = &s->lights[s->light_count++];
            snprintf(l->name, sizeof(l->name), "Light %d", s->light_count);
            j_str(&j, c, "name", l->name, sizeof(l->name));
            l->light.pos = j_vec3(&j, c, "pos", shz_vec3_init(0.0f, 10.0f, 0.0f));
            l->light.sun = j_bool(&j, c, "sun", false);
            uint32_t rgb = j_colour(&j, c, "color", 0xFFFFFF);
            l->light.r = (float)((rgb >> 16) & 255) / 255.0f;
            l->light.g = (float)((rgb >> 8) & 255) / 255.0f;
            l->light.b = (float)(rgb & 255) / 255.0f;
            l->light.range = j_num(&j, c, "range", 500.0f);
            l->light.ambient = j_num(&j, c, "ambient", 0.25f);
            l->light.bands = (int)j_num(&j, c, "bands", 0.0f);
        }

    const JNode* objects = j_get(&j, top, "objects");
    if (objects && objects->type == J_ARR)
        for (int c = objects->child; c >= 0; c = j.nodes[c].next) {
            DCSceneObject* o = add_object(s);
            if (!o) break;
            snprintf(o->name, sizeof(o->name), "Object %d", s->object_count);
            j_str(&j, c, "name", o->name, sizeof(o->name));
            j_str(&j, c, "model", o->model, sizeof(o->model));
            j_str(&j, c, "anims", o->anims, sizeof(o->anims));
            o->pos = j_vec3(&j, c, "pos", o->pos);
            o->rot = j_vec3(&j, c, "rot", o->rot);
            o->scale = j_vec3(&j, c, "scale", o->scale);
            o->tint = j_colour(&j, c, "tint", 0xFFFFFF);
            o->add = j_bool(&j, c, "add", false);
            o->hidden = !j_bool(&j, c, "visible", true);
        }

    free(j.nodes);
    free(text);
    dc_scene_load_models(s);
    return s;
}

DCScene* dc_scene_play(const char* root, const char* scene) {
    const char* open = getenv("DMS_SCENE");
    char path[256], start[128] = "";
    if (!open || !*open) {
        /* The editor's: the scene it had open when it sent the game off */
        snprintf(path, sizeof(path), "%s%s", root, DC_START_SCENE);
        file_t fd = dc_asset_open(path, O_RDONLY);
        if (fd >= 0) {
            ssize_t n = fs_read(fd, start, sizeof(start) - 1);
            fs_close(fd);
            start[n > 0 ? n : 0] = 0;
            start[strcspn(start, "\r\n")] = 0;
            open = start;
        }
    }
    if (open && *open) {
        snprintf(path, sizeof(path), "%s%s", root, open);
        DCScene* s = dc_scene_load(path, root);
        if (s) return s;
    }
    snprintf(path, sizeof(path), "%s%s", root, scene);
    return dc_scene_load(path, root);
}

DMSModel* dc_scene_model(DCScene* scene, const char* name) {
    DCSceneObject* o = scene ? dc_scene_find(scene, name) : NULL;
    return o ? o->m : NULL;
}

DCScene* dc_scene_load(const char* path, const char* root) {
    size_t size;
    char* file = dc_file_read(path, &size);
    if (!file) {
        printf("dc_scene: cannot read %s\n", path);
        return NULL;
    }
    char* text = malloc(size + 1);
    if (!text) { free(file); return NULL; }
    memcpy(text, file, size);
    text[size] = 0;
    free(file);
    DCScene* s = dc_scene_parse(text, root);
    free(text);
    return s;
}

/* The model at path, loaded the first time it is asked for */
static DMSModel* cached_model(DCScene* s, const char* path, const char* who) {
    int k = 0;
    while (k < s->model_count && strcmp(s->models[k].path, path)) k++;
    if (k == s->model_count) {
        if (s->model_count == s->model_cap) {
            int cap = s->model_cap ? s->model_cap * 2 : 8;
            DCSceneModel* m = realloc(s->models, cap * sizeof(*m));
            if (!m) return NULL;
            s->models = m;
            s->model_cap = cap;
        }
        char full[256];
        snprintf(full, sizeof(full), "%s%s", s->root, path);
        DCSceneModel* m = &s->models[s->model_count++];
        snprintf(m->path, sizeof(m->path), "%s", path);
        m->model = strcmp(path, "@cube") ? dc_model_load(full) : dc_model_cube();
        if (!m->model) printf("dc_scene: %s: cannot load %s\n", who, full);
        /* a skinned model has no pose (nor bounds) until it is first animated:
         * posed now at its first frame, so it shows before the game runs it */
        else if (m->model->skeleton) dc_model_animate(m->model, 0.0f);
    }
    return s->models[k].model;
}

/* o's model plays the clips of the one its anims names */
static void share_anims(DCScene* s, DCSceneObject* o) {
    if (!o->m || !o->anims[0] || !o->m->skeleton || o->m->skeleton->animBone) return;
    DMSModel* from = cached_model(s, o->anims, o->name);
    if (from && dc_model_share_anims(o->m, from)) dc_model_animate(o->m, 0.0f);
    else if (from) printf("dc_scene: %s: no clips of %s fit %s\n", o->name, o->anims, o->model);
}

void dc_scene_load_models(DCScene* s) {
    for (int i = 0; i < s->object_count; i++) {
        DCSceneObject* o = &s->objects[i];
        if (!o->m && o->model[0]) o->m = cached_model(s, o->model, o->name);
    }
    for (int i = 0; i < s->object_count; i++) share_anims(s, &s->objects[i]);
}

bool dc_scene_reload_model(DCScene* s, const char* path) {
    int k = 0;
    while (k < s->model_count && strcmp(s->models[k].path, path)) k++;
    if (k == s->model_count || !strcmp(path, "@cube")) return false;
    char full[256];
    snprintf(full, sizeof(full), "%s%s", s->root, path);
    DMSModel* m = dc_model_load(full);
    if (!m) return false;
    if (m->skeleton) dc_model_animate(m, 0.0f);
    DMSModel* old = s->models[k].model;
    s->models[k].model = m;
    for (int i = 0; i < s->object_count; i++) {
        DCSceneObject* o = &s->objects[i];
        if (o->m == old && !strcmp(o->model, path)) { o->m = m; share_anims(s, o); }
        /* Models playing its clips play the new ones */
        else if (o->m && !strcmp(o->anims, path) && o->m->skeleton && o->m->skeleton->animBone)
            dc_model_share_anims(o->m, m);
    }
    if (old) dc_model_free(old);
    return true;
}

void dc_scene_free(DCScene* s) {
    if (!s) return;
    /* Models playing another's clips go before it */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < s->model_count; i++) {
            DMSModel* m = s->models[i].model;
            if (m && (pass || (m->skeleton && m->skeleton->animBone))) {
                dc_model_free(m);
                s->models[i].model = NULL;
            }
        }
    free(s->models);
    free(s->objects);
    free(s);
}

/* ================================================================
 * Use
 * ================================================================ */

void dc_scene_apply(const DCScene* s) {
    dc_set_clear_color(0xFF000000u | s->background);
    if (s->fog) dc_set_fog(s->fog_rgb, s->fog_near, s->fog_far);
    else dc_set_fog_off();
    if (s->light_count) {
        DCLight lights[DC_MAX_LIGHTS];
        for (int i = 0; i < s->light_count; i++) lights[i] = s->lights[i].light;
        dc_set_lights(lights, s->light_count);
    } else {
        dc_set_light(NULL);
    }
}

void dc_scene_camera(const DCScene* s, DCCamera* cam) {
    dc_camera_init(cam);
    cam->pos = s->cam_pos;
    cam->yaw = SHZ_DEG_TO_RAD(s->cam_yaw);
    cam->pitch = SHZ_DEG_TO_RAD(s->cam_pitch);
    cam->fov = s->cam_fov > 1.0f ? s->cam_fov : 60.0f;
}

void dc_scene_object_rot(const DCSceneObject* o, float r[9]) {
    float sx = sinf(SHZ_DEG_TO_RAD(o->rot.x)), cx = cosf(SHZ_DEG_TO_RAD(o->rot.x));
    float sy = sinf(SHZ_DEG_TO_RAD(o->rot.y)), cy = cosf(SHZ_DEG_TO_RAD(o->rot.y));
    float sz = sinf(SHZ_DEG_TO_RAD(o->rot.z)), cz = cosf(SHZ_DEG_TO_RAD(o->rot.z));
    /* Ry * Rx * Rz, by columns */
    r[0] = cy * cz + sy * sx * sz;  r[1] = cx * sz;  r[2] = -sy * cz + cy * sx * sz;
    r[3] = -cy * sz + sy * sx * cz; r[4] = cx * cz;  r[5] = sy * sz + cy * sx * cz;
    r[6] = sy * cx;                 r[7] = -sx;      r[8] = cy * cx;
}

void dc_scene_draw(const DCScene* s) {
    for (int i = 0; i < s->object_count; i++) {
        const DCSceneObject* o = &s->objects[i];
        if (o->hidden || !o->m) continue;
        float rot[9];
        DCDrawOpts d = { .pos = o->pos, .add = o->add };
        if ((o->tint & 0xFFFFFF) != 0xFFFFFF) d.tint = o->tint ? o->tint : 0x010101;
        /* the same on every axis is a plain scale, lit exactly; a stretch is lit close */
        if (o->scale.x == o->scale.y && o->scale.y == o->scale.z)
            d.scale = fmaxf(o->scale.x, 0.001f);
        else
            d.stretch = shz_vec3_init(fmaxf(o->scale.x, 0.001f), fmaxf(o->scale.y, 0.001f), fmaxf(o->scale.z, 0.001f));
        if (o->m->skeleton) {
            d.yaw = -SHZ_DEG_TO_RAD(o->rot.y);
        } else if (o->rot.x != 0.0f || o->rot.y != 0.0f || o->rot.z != 0.0f) {
            dc_scene_object_rot(o, rot);
            d.rot = rot;
        }
        dc_draw_ex(o->m, &d);
    }
}

shz_vec3_t dc_scene_rot_degrees(const float r[9]) {
    float x, y, z;
    if (fabsf(r[7]) < 0.9999f) {
        x = asinf(-r[7]);
        y = atan2f(r[6], r[8]);
        z = atan2f(r[1], r[4]);
    } else {                            /* straight up or down: y and z are one turn */
        x = r[7] < 0.0f ? 1.5707963f : -1.5707963f;
        y = atan2f(-r[2], r[0]);
        z = 0.0f;
    }
    return shz_vec3_init(SHZ_RAD_TO_DEG(x), SHZ_RAD_TO_DEG(y), SHZ_RAD_TO_DEG(z));
}

static float host_scale(const DCSceneObject* host) {
    return host->scale.x != 0.0f ? host->scale.x : 1.0f;
}

int dc_scene_import_entities(DCScene* s, const DCSceneObject* host) {
    if (!host->m || !host->m->entity_count) return 0;
    const DMSModel* m = host->m;
    shz_vec3_t at = host->pos;
    float k = host_scale(host);
    int n = 0;
    for (uint32_t i = 0; i < m->entity_count; i++) {
        const DMSEntity* e = &m->entities[i];
        DCSceneObject* o = add_object(s);
        if (!o) break;
        host = NULL;                    /* add_object may have moved the objects */
        snprintf(o->name, sizeof(o->name), "%s", e->name);
        o->model[0] = 0;
        o->pos = shz_vec3_init(at.x + e->pos.x * k, at.y + e->pos.y * k, at.z + e->pos.z * k);
        o->rot = dc_scene_rot_degrees(e->rot);
        o->scale = shz_vec3_init(e->scale.x * k, e->scale.y * k, e->scale.z * k);
        n++;
    }
    return n;
}

int dc_scene_entities(const DCScene* s, DCSceneObject* host) {
    if (!host->m) return 0;
    int n = 0;
    for (int i = 0; i < s->object_count; i++) n += dc_scene_is_marker(&s->objects[i]);
    if (!n) return 0;
    DMSEntity* es = calloc(n, sizeof(DMSEntity));
    if (!es) return 0;
    float k = host_scale(host);
    DMSEntity* e = es;
    for (int i = 0; i < s->object_count; i++) {
        const DCSceneObject* o = &s->objects[i];
        if (!dc_scene_is_marker(o)) continue;
        snprintf(e->name, sizeof(e->name), "%s", o->name);
        e->pos = shz_vec3_init((o->pos.x - host->pos.x) / k, (o->pos.y - host->pos.y) / k, (o->pos.z - host->pos.z) / k);
        dc_scene_object_rot(o, e->rot);
        e->scale = shz_vec3_init(o->scale.x / k, o->scale.y / k, o->scale.z / k);
        e++;
    }
    free(host->m->entities);            /* the model frees what it has */
    host->m->entities = es;
    host->m->entity_count = n;
    return n;
}

DCSceneObject* dc_scene_find(DCScene* s, const char* name) {
    for (int i = 0; i < s->object_count; i++)
        if (!strcmp(s->objects[i].name, name)) return &s->objects[i];
    return NULL;
}

DCSceneObject* dc_scene_add(DCScene* s, const char* name, const char* model) {
    DCSceneObject* o = add_object(s);
    if (!o) return NULL;
    snprintf(o->name, sizeof(o->name), "%s", name);
    snprintf(o->model, sizeof(o->model), "%s", model ? model : "");
    dc_scene_load_models(s);
    return o;
}

void dc_scene_remove(DCScene* s, int index) {
    if (index < 0 || index >= s->object_count) return;
    memmove(&s->objects[index], &s->objects[index + 1],
            (s->object_count - index - 1) * sizeof(DCSceneObject));
    s->object_count--;
}

/* ================================================================
 * Writing: one field a line, numbers rounded so diffs stay small.
 * ================================================================ */

typedef struct { char* buf; size_t len, cap; } Out;

static void out(Out* o, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void out(Out* o, const char* fmt, ...) {
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(o->buf ? o->buf + o->len : NULL, o->buf ? o->cap - o->len : 0, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if (o->buf && o->len + n < o->cap) { o->len += n; return; }
        size_t cap = o->cap ? o->cap * 2 : 4096;
        while (cap < o->len + n + 1) cap *= 2;
        char* b = realloc(o->buf, cap);
        if (!b) return;
        o->buf = b;
        o->cap = cap;
    }
}

/* Up to 4 places, no trailing zeros, no "-0" */
static const char* num(char* b, float v) {
    float r = roundf(v * 10000.0f) / 10000.0f;
    if (r == 0.0f) r = 0.0f;
    snprintf(b, 24, "%.4f", r);
    char* e = b + strlen(b) - 1;
    while (*e == '0') *e-- = 0;
    if (*e == '.') *e = 0;
    return b;
}

static void out_str(Out* o, const char* s) {
    out(o, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') out(o, "\\%c", *s);
        else if ((unsigned char)*s < 32) out(o, "\\u%04x", *s);
        else out(o, "%c", *s);
    }
    out(o, "\"");
}

static void out_vec3(Out* o, shz_vec3_t v) {
    char a[24], b[24], c[24];
    out(o, "[%s, %s, %s]", num(a, v.x), num(b, v.y), num(c, v.z));
}

static uint32_t rgb_of(float r, float g, float b) {
    int R = (int)(r * 255.0f + 0.5f), G = (int)(g * 255.0f + 0.5f), B = (int)(b * 255.0f + 0.5f);
    R = R < 0 ? 0 : R > 255 ? 255 : R;
    G = G < 0 ? 0 : G > 255 ? 255 : G;
    B = B < 0 ? 0 : B > 255 ? 255 : B;
    return (uint32_t)(R << 16 | G << 8 | B);
}

char* dc_scene_write(const DCScene* s) {
    Out o = { 0 };
    char a[24], b[24], c[24];
    out(&o, "{\n  \"dms_scene\": 1,\n");
    out(&o, "  \"background\": \"#%06x\",\n", (unsigned)(s->background & 0xFFFFFF));
    if (s->fog)
        out(&o, "  \"fog\": { \"color\": \"#%06x\", \"near\": %s, \"far\": %s },\n",
            (unsigned)(s->fog_rgb & 0xFFFFFF), num(a, s->fog_near), num(b, s->fog_far));
    out(&o, "  \"camera\": { \"pos\": ");
    out_vec3(&o, s->cam_pos);
    out(&o, ", \"yaw\": %s, \"pitch\": %s, \"fov\": %s },\n",
        num(a, s->cam_yaw), num(b, s->cam_pitch), num(c, s->cam_fov));

    out(&o, "  \"lights\": [");
    for (int i = 0; i < s->light_count; i++) {
        const DCSceneLight* l = &s->lights[i];
        out(&o, "%s\n    { \"name\": ", i ? "," : "");
        out_str(&o, l->name);
        out(&o, ", \"pos\": ");
        out_vec3(&o, l->light.pos);
        out(&o, ", \"sun\": %s, \"color\": \"#%06x\", \"range\": %s, \"ambient\": %s, \"bands\": %d }",
            l->light.sun ? "true" : "false", (unsigned)rgb_of(l->light.r, l->light.g, l->light.b),
            num(a, l->light.range), num(b, l->light.ambient), l->light.bands);
    }
    out(&o, "%s],\n", s->light_count ? "\n  " : "");

    out(&o, "  \"objects\": [");
    for (int i = 0; i < s->object_count; i++) {
        const DCSceneObject* ob = &s->objects[i];
        out(&o, "%s\n    { \"name\": ", i ? "," : "");
        out_str(&o, ob->name);
        out(&o, ", \"model\": ");
        out_str(&o, ob->model);
        if (ob->anims[0]) {
            out(&o, ", \"anims\": ");
            out_str(&o, ob->anims);
        }
        out(&o, ",\n      \"pos\": ");
        out_vec3(&o, ob->pos);
        out(&o, ", \"rot\": ");
        out_vec3(&o, ob->rot);
        out(&o, ", \"scale\": ");
        out_vec3(&o, ob->scale);
        out(&o, ",\n      \"tint\": \"#%06x\", \"add\": %s, \"visible\": %s }",
            (unsigned)(ob->tint & 0xFFFFFF), ob->add ? "true" : "false", ob->hidden ? "false" : "true");
    }
    out(&o, "%s]\n}\n", s->object_count ? "\n  " : "");
    return o.buf;
}
