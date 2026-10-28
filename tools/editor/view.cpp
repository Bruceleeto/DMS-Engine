/* The scene view: the engine's 640x480 frame, with the grid, icons, the
 * selection and the gizmo drawn over it by ImGui (none of it is in the
 * frame itself). The camera flies as Unity's does: hold the right button to
 * look, with WASD/QE to move; middle drag pans, the wheel dollies, Alt+left
 * drag orbits, F frames the selection. */
#include "editor.h"
#include "imgui.h"
#include "ImGuizmo.h"
#include "ui.h"
#include <math.h>
#include <float.h>

static const float NEAR = 0.1f;
static const float DEG = 3.14159265f / 180.0f;

/* The engine's camera, as dc_camera_update builds it: right, up and forward.
 * Yaw 0 looks along +z, positive pitch looks down. */
struct Basis { shz_vec3_t r, u, f; };

static Basis basis(const DCCamera& c) {
    float sy = sinf(c.yaw), cy = cosf(c.yaw), sp = sinf(c.pitch), cp = cosf(c.pitch);
    return { shz_vec3_init(-cy, 0.0f, sy), shz_vec3_init(sy * sp, cp, cy * sp), shz_vec3_init(sy * cp, -sp, cy * cp) };
}

static shz_vec3_t add(shz_vec3_t a, shz_vec3_t b) { return shz_vec3_init(a.x + b.x, a.y + b.y, a.z + b.z); }
static shz_vec3_t sub(shz_vec3_t a, shz_vec3_t b) { return shz_vec3_init(a.x - b.x, a.y - b.y, a.z - b.z); }
static shz_vec3_t mul(shz_vec3_t a, float s) { return shz_vec3_init(a.x * s, a.y * s, a.z * s); }
static float dot(shz_vec3_t a, shz_vec3_t b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float len(shz_vec3_t a) { return sqrtf(dot(a, a)); }

static float tan_half(const DCCamera& c) { return tanf((c.fov > 1.0f ? c.fov : 60.0f) * 0.5f * DEG); }

/* Camera space: x right, y up, z ahead */
static shz_vec3_t to_cam(const DCCamera& c, const Basis& b, shz_vec3_t p) {
    shz_vec3_t d = sub(p, c.pos);
    return shz_vec3_init(dot(d, b.r), dot(d, b.u), dot(d, b.f));
}

static void cam_to_frame(const DCCamera& c, shz_vec3_t v, float* x, float* y) {
    float ty = tan_half(c), tx = ty * (640.0f / 480.0f);
    *x = 320.0f + 320.0f * (v.x / v.z) / tx;
    *y = 240.0f - 240.0f * (v.y / v.z) / ty;
}

bool view_project(const DCCamera& c, shz_vec3_t p, float* x, float* y) {
    shz_vec3_t v = to_cam(c, basis(c), p);
    if (v.z < NEAR) return false;
    cam_to_frame(c, v, x, y);
    return true;
}

void view_matrices(const DCCamera& c, float view[16], float proj[16]) {
    Basis b = basis(c);
    /* OpenGL looks down -z, so the third row is minus forward */
    float v[16] = { b.r.x, b.u.x, -b.f.x, 0,
                    b.r.y, b.u.y, -b.f.y, 0,
                    b.r.z, b.u.z, -b.f.z, 0,
                    -dot(b.r, c.pos), -dot(b.u, c.pos), dot(b.f, c.pos), 1 };
    memcpy(view, v, sizeof(v));
    float ty = tan_half(c), tx = ty * (640.0f / 480.0f), n = NEAR, f = 2000.0f;
    float p[16] = { 1 / tx, 0, 0, 0,
                    0, 1 / ty, 0, 0,
                    0, 0, -(f + n) / (f - n), -1,
                    0, 0, -2 * f * n / (f - n), 0 };
    memcpy(proj, p, sizeof(p));
}

/* The ray through a point of the frame */
static shz_vec3_t ray_dir(const DCCamera& c, float fx, float fy) {
    Basis b = basis(c);
    float ty = tan_half(c), tx = ty * (640.0f / 480.0f);
    float x = (fx - 320.0f) / 320.0f * tx, y = -(fy - 240.0f) / 240.0f * ty;
    shz_vec3_t d = add(add(mul(b.r, x), mul(b.u, y)), b.f);
    return mul(d, 1.0f / len(d));
}

/* ================================================================
 * Drawing over the frame
 * ================================================================ */

static ImVec2 g_origin;     /* where the frame's top left is on screen */
static float g_scale;       /* screen pixels per frame pixel */

static ImVec2 screen(float fx, float fy) { return ImVec2(g_origin.x + fx * g_scale, g_origin.y + fy * g_scale); }

/* A world line, cut where it goes behind the camera */
static void line3(ImDrawList* dl, const DCCamera& c, shz_vec3_t a, shz_vec3_t b, ImU32 col, float thick = 1.0f) {
    Basis bs = basis(c);
    shz_vec3_t va = to_cam(c, bs, a), vb = to_cam(c, bs, b);
    if (va.z < NEAR && vb.z < NEAR) return;
    if (va.z < NEAR) va = add(va, mul(sub(vb, va), (NEAR - va.z) / (vb.z - va.z)));
    if (vb.z < NEAR) vb = add(vb, mul(sub(va, vb), (NEAR - vb.z) / (va.z - vb.z)));
    float x0, y0, x1, y1;
    cam_to_frame(c, va, &x0, &y0);
    cam_to_frame(c, vb, &x1, &y1);
    dl->AddLine(screen(x0, y0), screen(x1, y1), col, thick);
}

static void draw_grid(ImDrawList* dl, const DCCamera& c) {
    float h = fabsf(c.pos.y);
    float step = h > 60.0f ? 10.0f : h > 12.0f ? 2.0f : 1.0f;
    int n = 40;
    float cx = roundf(c.pos.x / step) * step, cz = roundf(c.pos.z / step) * step;
    for (int i = -n; i <= n; i++) {
        float x = cx + i * step, z = cz + i * step;
        bool major = fmodf(fabsf(x), step * 10.0f) < 0.001f;
        ImU32 col = x == 0.0f ? IM_COL32(200, 70, 70, 150) : major ? IM_COL32(255, 255, 255, 55) : IM_COL32(255, 255, 255, 25);
        line3(dl, c, shz_vec3_init(x, 0, cz - n * step), shz_vec3_init(x, 0, cz + n * step), col);
        major = fmodf(fabsf(z), step * 10.0f) < 0.001f;
        col = z == 0.0f ? IM_COL32(70, 110, 220, 150) : major ? IM_COL32(255, 255, 255, 55) : IM_COL32(255, 255, 255, 25);
        line3(dl, c, shz_vec3_init(cx - n * step, 0, z), shz_vec3_init(cx + n * step, 0, z), col);
    }
}

/* A sun's icon sits up where its light comes from (pos is the way it shines) */
static shz_vec3_t light_icon_pos(const DCSceneLight& l) {
    if (!l.light.sun) return l.light.pos;
    shz_vec3_t d = l.light.pos;
    float n = len(d);
    return n > 0.0f ? mul(d, -12.0f / n) : shz_vec3_init(0, 12, 0);
}

static ImU32 light_colour(const DCLight& l, int alpha) {
    return IM_COL32((int)(l.r * 255), (int)(l.g * 255), (int)(l.b * 255), alpha);
}

static void draw_icons(ImDrawList* dl, const DCCamera& c) {
    for (int i = 0; i < ed.scene->light_count; i++) {
        const DCSceneLight& l = ed.scene->lights[i];
        shz_vec3_t p = light_icon_pos(l);
        float x, y;
        if (!view_project(c, p, &x, &y)) continue;
        bool sel = ed.sel.is(SelKind::Light, i);
        ImVec2 s = screen(x, y);
        ImU32 col = sel ? IM_COL32(255, 170, 40, 255) : IM_COL32(240, 220, 120, 230);
        dl->AddCircleFilled(s, 7.0f, light_colour(l.light, 255));
        dl->AddCircle(s, 7.0f, col, 0, 2.0f);
        if (l.light.sun) {
            for (int k = 0; k < 8; k++) {
                float a = k * 3.14159265f / 4.0f;
                dl->AddLine(ImVec2(s.x + cosf(a) * 10, s.y + sinf(a) * 10), ImVec2(s.x + cosf(a) * 14, s.y + sinf(a) * 14), col, 1.5f);
            }
            line3(dl, c, p, mul(p, 0.6f), IM_COL32(240, 220, 120, 120));
        } else if (sel) {
            /* its reach, round the three axes */
            float r = l.light.range > 0.0f ? l.light.range : 500.0f;
            for (int axis = 0; axis < 3; axis++)
                for (int k = 0; k < 48; k++) {
                    float a0 = k * 2 * 3.14159265f / 48, a1 = (k + 1) * 2 * 3.14159265f / 48;
                    shz_vec3_t p0, p1;
                    if (axis == 0) { p0 = shz_vec3_init(cosf(a0) * r, sinf(a0) * r, 0); p1 = shz_vec3_init(cosf(a1) * r, sinf(a1) * r, 0); }
                    else if (axis == 1) { p0 = shz_vec3_init(cosf(a0) * r, 0, sinf(a0) * r); p1 = shz_vec3_init(cosf(a1) * r, 0, sinf(a1) * r); }
                    else { p0 = shz_vec3_init(0, cosf(a0) * r, sinf(a0) * r); p1 = shz_vec3_init(0, cosf(a1) * r, sinf(a1) * r); }
                    line3(dl, c, add(p, p0), add(p, p1), IM_COL32(240, 220, 120, 90));
                }
        }
        dl->AddText(ImVec2(s.x + 10, s.y - 7), IM_COL32(230, 230, 230, 200), l.name);
    }
    /* where the game's camera starts */
    DCCamera start;
    dc_scene_camera(ed.scene, &start);
    Basis b = basis(start);
    float d = 1.0f, ty = tan_half(start) * d, tx = ty * 4.0f / 3.0f;
    shz_vec3_t o = start.pos, f = add(o, mul(b.f, d));
    shz_vec3_t k[4] = { add(add(f, mul(b.r, tx)), mul(b.u, ty)), add(add(f, mul(b.r, -tx)), mul(b.u, ty)),
                        add(add(f, mul(b.r, -tx)), mul(b.u, -ty)), add(add(f, mul(b.r, tx)), mul(b.u, -ty)) };
    ImU32 cc = IM_COL32(150, 200, 255, 160);
    for (int i = 0; i < 4; i++) { line3(dl, c, o, k[i], cc); line3(dl, c, k[i], k[(i + 1) % 4], cc); }
}

/* An object's own bounds: its model's, or a marker's unit cube (its scale
 * makes it the size it is) */
static bool object_bounds(const DCSceneObject& o, DCBounds* bd) {
    if (dc_scene_is_marker(&o)) {
        bd->min = shz_vec3_init(-0.5f, -0.5f, -0.5f);
        bd->max = shz_vec3_init(0.5f, 0.5f, 0.5f);
        return true;
    }
    if (!o.m) return false;
    *bd = dc_model_bounds(o.m, nullptr);
    return bd->min.x <= bd->max.x;
}

/* An object's box: its bounds, turned, scaled and placed */
static bool object_box(const DCSceneObject& o, shz_vec3_t corner[8]) {
    DCBounds bd;
    if (!object_bounds(o, &bd)) return false;
    float r[9];
    dc_scene_object_rot(&o, r);
    for (int i = 0; i < 8; i++) {
        float x = (i & 1 ? bd.max.x : bd.min.x) * o.scale.x;
        float y = (i & 2 ? bd.max.y : bd.min.y) * o.scale.y;
        float z = (i & 4 ? bd.max.z : bd.min.z) * o.scale.z;
        corner[i] = shz_vec3_init(o.pos.x + r[0] * x + r[3] * y + r[6] * z,
                                  o.pos.y + r[1] * x + r[4] * y + r[7] * z,
                                  o.pos.z + r[2] * x + r[5] * y + r[8] * z);
    }
    return true;
}

static const int box_edges[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };

static void draw_selection(ImDrawList* dl, const DCCamera& c) {
    if (ed.sel.kind != SelKind::Object) return;
    shz_vec3_t k[8];
    if (!object_box(ed.scene->objects[ed.sel.index], k)) return;
    for (auto& l : box_edges) line3(dl, c, k[l[0]], k[l[1]], IM_COL32(255, 160, 30, 230), 1.5f);
}

/* Markers: a box each, its name over it when the mouse is near or it is chosen */
static void draw_markers(ImDrawList* dl, const DCCamera& c) {
    ImVec2 mouse = ImGui::GetMousePos();
    for (int i = 0; i < ed.scene->object_count; i++) {
        const DCSceneObject& o = ed.scene->objects[i];
        if (!dc_scene_is_marker(&o) || o.hidden) continue;
        shz_vec3_t k[8];
        object_box(o, k);
        bool sel = ed.sel.is(SelKind::Object, i);
        if (!sel) for (auto& l : box_edges) line3(dl, c, k[l[0]], k[l[1]], IM_COL32(120, 220, 200, 150));
        float x, y;
        if (!view_project(c, o.pos, &x, &y)) continue;
        ImVec2 s = screen(x, y);
        dl->AddCircleFilled(s, 3.0f, IM_COL32(120, 220, 200, 230));
        float dx = mouse.x - s.x, dy = mouse.y - s.y;
        if (sel || dx * dx + dy * dy < 40.0f * 40.0f)
            dl->AddText(ImVec2(s.x + 6, s.y - 16), IM_COL32(200, 245, 235, 230), o.name);
    }
}

/* ================================================================
 * Picking
 * ================================================================ */

static bool ray_box(shz_vec3_t o, shz_vec3_t d, const DCSceneObject& ob, float* t_out) {
    DCBounds bd;
    if (!object_bounds(ob, &bd)) return false;
    float r[9];
    dc_scene_object_rot(&ob, r);
    /* into the object's own space: undo the place, turn and scale */
    shz_vec3_t p = sub(o, ob.pos);
    float lo[3] = { (r[0] * p.x + r[1] * p.y + r[2] * p.z) / ob.scale.x,
                    (r[3] * p.x + r[4] * p.y + r[5] * p.z) / ob.scale.y,
                    (r[6] * p.x + r[7] * p.y + r[8] * p.z) / ob.scale.z };
    float ld[3] = { (r[0] * d.x + r[1] * d.y + r[2] * d.z) / ob.scale.x,
                    (r[3] * d.x + r[4] * d.y + r[5] * d.z) / ob.scale.y,
                    (r[6] * d.x + r[7] * d.y + r[8] * d.z) / ob.scale.z };
    float mn[3] = { bd.min.x, bd.min.y, bd.min.z }, mx[3] = { bd.max.x, bd.max.y, bd.max.z };
    float t0 = 0.0f, t1 = FLT_MAX;
    for (int i = 0; i < 3; i++) {
        if (fabsf(ld[i]) < 1e-9f) {
            if (lo[i] < mn[i] || lo[i] > mx[i]) return false;
            continue;
        }
        float a = (mn[i] - lo[i]) / ld[i], b = (mx[i] - lo[i]) / ld[i];
        if (a > b) { float t = a; a = b; b = t; }
        t0 = fmaxf(t0, a);
        t1 = fminf(t1, b);
        if (t0 > t1) return false;
    }
    *t_out = t0;    /* in the scaled space's units; the same ray, so it orders hits */
    return true;
}

static void pick(const DCCamera& c, float fx, float fy) {
    /* lights first: their icons are small and drawn on top */
    for (int i = 0; i < ed.scene->light_count; i++) {
        float x, y;
        if (view_project(c, light_icon_pos(ed.scene->lights[i]), &x, &y) &&
            (x - fx) * (x - fx) + (y - fy) * (y - fy) < 100.0f / (g_scale * g_scale) + 50.0f) {
            ed.sel = { SelKind::Light, i };
            return;
        }
    }
    shz_vec3_t d = ray_dir(c, fx, fy);
    float best = FLT_MAX;
    Selection hit;
    for (int i = 0; i < ed.scene->object_count; i++) {
        const DCSceneObject& o = ed.scene->objects[i];
        float t;
        if (o.hidden || !ray_box(c.pos, d, o, &t)) continue;
        /* back to world distance, to compare objects of different scales */
        shz_vec3_t k[8];
        float dist = t;
        if (object_box(o, k)) {
            shz_vec3_t mid = mul(add(k[0], k[7]), 0.5f);
            dist = len(sub(mid, c.pos));
            /* a box the camera is in loses to anything in front */
            if (t == 0.0f) dist += 1e6f;
        }
        /* a marker is small and inside a level's box: it wins */
        if (dc_scene_is_marker(&o)) dist -= 1e7f;
        if (dist < best) { best = dist; hit = { SelKind::Object, i }; }
    }
    ed.sel = hit;
}

/* Where a ray meets the ground, or a way ahead if it doesn't */
static shz_vec3_t ground_point(const DCCamera& c, float fx, float fy) {
    shz_vec3_t d = ray_dir(c, fx, fy);
    if (d.y < -0.01f && c.pos.y > 0.0f) {
        float t = -c.pos.y / d.y;
        if (t < 200.0f) return add(c.pos, mul(d, t));
    }
    return add(c.pos, mul(d, 8.0f));
}

bool view_ground_at(float x, float y, shz_vec3_t* out) {
    if (g_scale <= 0.0f) return false;
    float fx = (x - g_origin.x) / g_scale, fy = (y - g_origin.y) / g_scale;
    if (fx < 0 || fy < 0 || fx >= 640 || fy >= 480) return false;
    *out = ground_point(ed.cam, fx, fy);
    return true;
}

/* ================================================================
 * Gizmo
 * ================================================================ */

static void object_matrix(const DCSceneObject& o, float m[16]) {
    float r[9];
    dc_scene_object_rot(&o, r);
    for (int c = 0; c < 3; c++) {
        float s = c == 0 ? o.scale.x : c == 1 ? o.scale.y : o.scale.z;
        m[c * 4 + 0] = r[c * 3 + 0] * s;
        m[c * 4 + 1] = r[c * 3 + 1] * s;
        m[c * 4 + 2] = r[c * 3 + 2] * s;
        m[c * 4 + 3] = 0.0f;
    }
    m[12] = o.pos.x; m[13] = o.pos.y; m[14] = o.pos.z; m[15] = 1.0f;
}

/* Back from a matrix: Ry * Rx * Rz, the turn dc_scene uses */
static void matrix_to_object(const float m[16], DCSceneObject& o, int op) {
    o.pos = shz_vec3_init(m[12], m[13], m[14]);
    float s[3], r[9];
    for (int c = 0; c < 3; c++) {
        s[c] = sqrtf(m[c * 4] * m[c * 4] + m[c * 4 + 1] * m[c * 4 + 1] + m[c * 4 + 2] * m[c * 4 + 2]);
        for (int k = 0; k < 3; k++) r[c * 3 + k] = s[c] > 0.0f ? m[c * 4 + k] / s[c] : 0.0f;
    }
    if (op == 2) o.scale = shz_vec3_init(s[0], s[1], s[2]);
    if (op == 1) {
        float x, y, z;
        if (fabsf(r[7]) < 0.9999f) {
            x = asinf(-r[7]);
            y = atan2f(r[6], r[8]);
            z = atan2f(r[1], r[4]);
        } else {
            x = r[7] < 0 ? 1.5707963f : -1.5707963f;
            y = atan2f(-r[2], r[0]);
            z = 0.0f;
        }
        o.rot = shz_vec3_init(x / DEG, y / DEG, z / DEG);
    }
}

static void gizmo(const DCCamera& c, ImVec2 pos, ImVec2 size) {
    if (ed.sel.kind == SelKind::None) return;
    float view[16], proj[16], m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    view_matrices(c, view, proj);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(pos.x, pos.y, size.x, size.y);

    int op = ed.gizmo_op;
    if (ed.sel.kind == SelKind::Object) object_matrix(ed.scene->objects[ed.sel.index], m);
    else {
        op = 0;     /* a light only moves */
        shz_vec3_t p = light_icon_pos(ed.scene->lights[ed.sel.index]);
        m[12] = p.x; m[13] = p.y; m[14] = p.z;
    }
    ImGuizmo::OPERATION ops[3] = { ImGuizmo::TRANSLATE, ImGuizmo::ROTATE, ImGuizmo::SCALE };
    float snap[3] = { ed.snap_move, ed.snap_move, ed.snap_move };
    if (op == 1) snap[0] = snap[1] = snap[2] = ed.snap_rot;
    if (op == 2) snap[0] = snap[1] = snap[2] = ed.snap_scale;

    static bool using_before;
    bool changed = ImGuizmo::Manipulate(view, proj, ops[op], ed.gizmo_local || op == 2 ? ImGuizmo::LOCAL : ImGuizmo::WORLD,
                                        m, nullptr, ed.snap ? snap : nullptr);
    bool using_now = ImGuizmo::IsUsing();
    if (using_now && !using_before) ed_checkpoint();
    using_before = using_now;
    if (!changed) return;

    if (ed.sel.kind == SelKind::Object) {
        matrix_to_object(m, ed.scene->objects[ed.sel.index], op);
    } else {
        DCSceneLight& l = ed.scene->lights[ed.sel.index];
        shz_vec3_t p = shz_vec3_init(m[12], m[13], m[14]);
        if (l.light.sun) {
            float n = len(p);
            if (n > 0.0f) l.light.pos = mul(p, -1.0f / n);
        } else {
            l.light.pos = p;
        }
    }
    ed_changed();
}

/* ================================================================
 * Camera
 * ================================================================ */

void view_focus_selection() {
    shz_vec3_t target;
    float radius = 2.0f;
    if (ed.sel.kind == SelKind::Object) {
        const DCSceneObject& o = ed.scene->objects[ed.sel.index];
        shz_vec3_t k[8];
        if (object_box(o, k)) {
            target = mul(add(k[0], k[7]), 0.5f);
            radius = fmaxf(len(sub(k[7], k[0])) * 0.5f, 0.5f);
        } else {
            target = o.pos;
        }
    } else if (ed.sel.kind == SelKind::Light) {
        target = light_icon_pos(ed.scene->lights[ed.sel.index]);
    } else {
        return;
    }
    float d = radius / sinf(tan_half(ed.cam)) + NEAR;
    ed.cam.pos = sub(target, mul(basis(ed.cam).f, d));
}

static void fly(bool hovered) {
    ImGuiIO& io = ImGui::GetIO();
    static bool looking, panning, orbiting;
    static shz_vec3_t pivot;
    static float pivot_dist;
    Basis b = basis(ed.cam);

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) looking = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) looking = false;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) panning = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) panning = false;
    if (hovered && io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        orbiting = true;
        pivot_dist = 8.0f;
        if (ed.sel.kind == SelKind::Object) pivot_dist = fmaxf(len(sub(ed.scene->objects[ed.sel.index].pos, ed.cam.pos)), 1.0f);
        pivot = add(ed.cam.pos, mul(b.f, pivot_dist));
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) orbiting = false;

    float dt = io.DeltaTime;
    if (looking || orbiting) {
        ed.cam.yaw -= io.MouseDelta.x * 0.005f;
        ed.cam.pitch += io.MouseDelta.y * 0.005f;
        ed.cam.pitch = fmaxf(-1.55f, fminf(1.55f, ed.cam.pitch));
        if (orbiting) ed.cam.pos = sub(pivot, mul(basis(ed.cam).f, pivot_dist));
    }
    if (looking) {
        float speed = ed.move_speed * (io.KeyShift ? 3.0f : 1.0f) * dt;
        shz_vec3_t m = shz_vec3_init(0, 0, 0);
        if (ImGui::IsKeyDown(ImGuiKey_W)) m = add(m, b.f);
        if (ImGui::IsKeyDown(ImGuiKey_S)) m = sub(m, b.f);
        if (ImGui::IsKeyDown(ImGuiKey_D)) m = add(m, b.r);
        if (ImGui::IsKeyDown(ImGuiKey_A)) m = sub(m, b.r);
        if (ImGui::IsKeyDown(ImGuiKey_E)) m.y += 1.0f;
        if (ImGui::IsKeyDown(ImGuiKey_Q)) m.y -= 1.0f;
        ed.cam.pos = add(ed.cam.pos, mul(m, speed));
        if (io.MouseWheel != 0.0f) ed.move_speed = fmaxf(0.5f, fminf(500.0f, ed.move_speed * powf(1.2f, io.MouseWheel)));
    } else if (hovered && io.MouseWheel != 0.0f) {
        ed.cam.pos = add(ed.cam.pos, mul(b.f, io.MouseWheel * ed.move_speed * 0.15f));
    }
    if (panning) {
        float k = ed.move_speed * 0.004f;
        ed.cam.pos = add(ed.cam.pos, add(mul(b.r, -io.MouseDelta.x * k), mul(b.u, io.MouseDelta.y * k)));
    }
    if (hovered && !looking && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) ed.gizmo_op = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) ed.gizmo_op = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) ed.gizmo_op = 2;
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) view_focus_selection();
    }
}

/* ================================================================ */

/* A row of small buttons over the view's top left */
static void overlay(ImVec2 at) {
    ImGui::SetCursorScreenPos(ImVec2(at.x + 8, at.y + 8));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float h = ImGui::GetFrameHeight(), n = 5;
    dl->AddRectFilled(ImVec2(at.x + 4, at.y + 4), ImVec2(at.x + 12 + n * (h + 2), at.y + 12 + h), IM_COL32(16, 17, 20, 200), 7);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 0));
    if (ui::icon_button("grid", ICON_GRID, "Grid", ed.show_grid)) ed.show_grid = !ed.show_grid;
    ImGui::SameLine();
    if (ui::icon_button("icons", ICON_LIGHT, "Lights and the start camera", ed.show_icons)) ed.show_icons = !ed.show_icons;
    ImGui::SameLine();
    if (ui::icon_button("goto", ICON_CAMERA, "Look through the start camera")) dc_scene_camera(ed.scene, &ed.cam);
    ImGui::SameLine();
    if (ui::icon_button("setcam", ICON_LOCAL, "Make this view the start camera")) {
        ed_checkpoint();
        ed.scene->cam_pos = ed.cam.pos;
        ed.scene->cam_yaw = ed.cam.yaw / DEG;
        ed.scene->cam_pitch = ed.cam.pitch / DEG;
        ed.scene->cam_fov = ed.cam.fov;
        ed_changed();
    }
    ImGui::SameLine();
    if (ui::icon_button("focus", ICON_INSPECT, "Frame the selection (F)")) view_focus_selection();
    ImGui::PopStyleVar();
}

/* The cube in the corner: shows the way the view looks, and a click on a
 * face looks along that axis */
static void view_cube(ImVec2 at, float w) {
    float view[16], proj[16];
    view_matrices(ed.cam, view, proj);
    float before[16];
    memcpy(before, view, sizeof(view));
    float dist = 8.0f, size = 96.0f;
    ImGuizmo::ViewManipulate(view, dist, ImVec2(at.x + w - size - 8, at.y + 8), ImVec2(size, size), 0x00000000);
    if (!memcmp(before, view, sizeof(view))) return;
    /* forward is minus the view's third row; the eye is where the view puts the origin, turned back */
    shz_vec3_t f = shz_vec3_init(-view[2], -view[6], -view[10]);
    float n = len(f);
    if (n < 1e-6f) return;
    f = mul(f, 1.0f / n);
    shz_vec3_t r = shz_vec3_init(view[0], view[4], view[8]), u = shz_vec3_init(view[1], view[5], view[9]);
    shz_vec3_t t = shz_vec3_init(view[12], view[13], view[14]);
    ed.cam.pos = shz_vec3_init(-(r.x * t.x + u.x * t.y - f.x * t.z), -(r.y * t.x + u.y * t.y - f.y * t.z), -(r.z * t.x + u.z * t.y - f.z * t.z));
    ed.cam.pitch = fmaxf(-1.55f, fminf(1.55f, asinf(-f.y)));
    if (fabsf(f.y) < 0.999f) ed.cam.yaw = atan2f(f.x, f.z);
}

void view_window() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool open = ImGui::Begin(WIN_SCENE);
    ImGui::PopStyleVar();
    g_scale = 0.0f;     /* not over the view, until it is drawn */
    if (!open) { ImGui::End(); return; }

    /* The frame, as big as fits at 4:3, in the middle */
    ImVec2 avail = ImGui::GetContentRegionAvail(), top = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(top, ImVec2(top.x + avail.x, top.y + avail.y), IM_COL32(12, 12, 14, 255));
    float w = avail.x, h = avail.x * 0.75f;
    if (h > avail.y) { h = avail.y; w = h / 0.75f; }
    if (w < 64 || h < 48) { ImGui::End(); return; }
    ImVec2 at(top.x + (avail.x - w) * 0.5f, top.y + (avail.y - h) * 0.5f);
    ImGui::SetCursorScreenPos(at);
    ImGui::Image((ImTextureID)(intptr_t)ed.frame_texture, ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    g_origin = at;
    g_scale = w / 640.0f;

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("DMS_MODEL")) {
            ImVec2 mp = ImGui::GetMousePos();
            std::string model((const char*)p->Data, p->DataSize);
            ed_add_model(model);
            if (ed.sel.kind == SelKind::Object)
                ed.scene->objects[ed.sel.index].pos = ground_point(ed.cam, (mp.x - at.x) / g_scale, (mp.y - at.y) / g_scale);
        }
        /* from the Library: imported, then put here */
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("DMS_LIBRARY")) {
            ImVec2 mp = ImGui::GetMousePos();
            shz_vec3_t pos = ground_point(ed.cam, (mp.x - at.x) / g_scale, (mp.y - at.y) / g_scale);
            ed_import_file(ed.root + std::string((const char*)p->Data, p->DataSize), &pos);
        }
        ImGui::EndDragDropTarget();
    }

    fly(hovered);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(at, ImVec2(at.x + w, at.y + h), true);
    if (ed.show_grid) draw_grid(dl, ed.cam);
    draw_selection(dl, ed.cam);
    if (ed.show_icons) { draw_icons(dl, ed.cam); draw_markers(dl, ed.cam); }
    dl->PopClipRect();

    gizmo(ed.cam, at, ImVec2(w, h));
    view_cube(at, w);
    overlay(at);
    bool on_overlay = ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive();
    ImVec2 mp = ImGui::GetMousePos();
    bool on_cube = mp.x > at.x + w - 112 && mp.y < at.y + 112;

    /* A click that wasn't on the gizmo, a button or a camera move picks */
    ImGuiIO& io = ImGui::GetIO();
    if (hovered && !on_overlay && !on_cube && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !io.KeyAlt &&
        !ImGuizmo::IsOver() && !ImGuizmo::IsUsing() &&
        fabsf(ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).x) < 3.0f &&
        fabsf(ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).y) < 3.0f)
        pick(ed.cam, (mp.x - at.x) / g_scale, (mp.y - at.y) / g_scale);

    char info[128];
    snprintf(info, sizeof(info), "640 x 480 PVR    %.1f  %.1f  %.1f    yaw %.0f  pitch %.0f    speed %.1f",
             ed.cam.pos.x, ed.cam.pos.y, ed.cam.pos.z, ed.cam.yaw / DEG, ed.cam.pitch / DEG, ed.move_speed);
    ImVec2 ts = ImGui::CalcTextSize(info);
    dl->AddRectFilled(ImVec2(at.x + 6, at.y + h - ts.y - 14), ImVec2(at.x + 18 + ts.x, at.y + h - 6), IM_COL32(16, 17, 20, 190), 6);
    dl->AddText(ImVec2(at.x + 12, at.y + h - ts.y - 10), IM_COL32(200, 204, 212, 220), info);
    ImGui::End();
}
