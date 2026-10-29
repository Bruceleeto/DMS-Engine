#include <stdio.h>
#include <math.h>
#include <string.h>
#include "ragdoll.h"
#include "dms/dc_model.h"

/* The points: a bone each, and how big a ball it is against the world */
static const struct { const char* bone; float radius; } POINTS[RD_POINTS] = {
    { "Pelvis", 0.13f }, { "Spine4", 0.13f }, { "Head1", 0.11f },
    { "L_UpperArm", 0.06f }, { "L_Forearm", 0.05f }, { "L_Hand", 0.05f },
    { "R_UpperArm", 0.06f }, { "R_Forearm", 0.05f }, { "R_Hand", 0.05f },
    { "L_Thigh", 0.08f }, { "L_Calf", 0.07f }, { "L_Foot", 0.06f },
    { "R_Thigh", 0.08f }, { "R_Calf", 0.07f }, { "R_Foot", 0.06f },
};
enum { PELVIS, CHEST, HEAD, LUA, LFA, LH, RUA, RFA, RH, LT, LC, LF, RT, RC, RF };

/* Sticks between points, kept their length. The trunk is braced across so it
 * stays one piece; limbs swing at their joints. */
static const unsigned char STICKS[][2] = {
    { PELVIS, CHEST }, { CHEST, HEAD }, { PELVIS, HEAD }, { CHEST, LUA }, { CHEST, RUA },
    { LUA, RUA }, { PELVIS, LUA }, { PELVIS, RUA }, { HEAD, LUA }, { HEAD, RUA },
    { PELVIS, LT }, { PELVIS, RT }, { LT, RT }, { CHEST, LT }, { CHEST, RT },
    { LUA, LFA }, { LFA, LH }, { RUA, RFA }, { RFA, RH },
    { LT, LC }, { LC, LF }, { RT, RC }, { RC, RF },
};
#define N_STICKS (int)(sizeof(STICKS) / sizeof(STICKS[0]))

/* An elbow or knee bends, but does not fold flat: shoulder to hand and hip to
 * foot are kept at least this much of the limb's length */
static const unsigned char FOLDS[][3] = {
    { LUA, LFA, LH }, { RUA, RFA, RH }, { LT, LC, LF }, { RT, RC, RF },
};
#define FOLD_MIN 0.55f

/* How each part of the body turns: the hips and the chest by two ways at
 * once (up the body, and across it), a limb bone by where it points, after
 * the frame it hangs from */
static const struct { const char* bone; signed char origin, aim, parent; } FRAMES[RD_FRAMES] = {
    { "Pelvis", PELVIS, CHEST, -1 }, { "Spine4", CHEST, HEAD, -1 },
    { "L_UpperArm", LUA, LFA, 1 }, { "L_Forearm", LFA, LH, 2 },
    { "R_UpperArm", RUA, RFA, 1 }, { "R_Forearm", RFA, RH, 4 },
    { "L_Thigh", LT, LC, 0 }, { "L_Calf", LC, LF, 6 },
    { "R_Thigh", RT, RC, 0 }, { "R_Calf", RC, RF, 8 },
};

static int point_bone[RD_POINTS];
static signed char bone_frame[RD_BONES];
static int bone_count;

static int find(const DMSSkeleton* sk, const char* name) {
    char full[64];
    snprintf(full, sizeof(full), "ValveBiped.Bip01_%s", name);
    for (int i = 0; i < sk->boneCount; i++)
        if (!strcmp(sk->bones[i].name, full)) return i;
    return -1;
}

bool rd_init(const DMSModel* m) {
    const DMSSkeleton* sk = m ? m->skeleton : NULL;
    if (!sk || sk->boneCount > RD_BONES) return false;
    for (int i = 0; i < RD_POINTS; i++)
        if ((point_bone[i] = find(sk, POINTS[i].bone)) < 0) return false;
    /* Each bone turns with the frame it starts, or else its parent's */
    bone_count = sk->boneCount;
    for (int b = 0; b < bone_count; b++) {
        bone_frame[b] = -1;
        for (int f = 0; f < RD_FRAMES; f++)
            if (point_bone[FRAMES[f].origin] == b) bone_frame[b] = (signed char)f;
        int p = sk->bones[b].parent;
        if (bone_frame[b] < 0) bone_frame[b] = p >= 0 && p < b ? bone_frame[p] : 0;
    }
    return true;
}

/* ---- Little 3x3 sums, row by row ---- */

static void mul(const float* a, const float* b, float* out) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            out[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
}

static shz_vec3_t apply(const float* m, shz_vec3_t v) {
    return shz_vec3_init(m[0] * v.x + m[1] * v.y + m[2] * v.z,
                         m[3] * v.x + m[4] * v.y + m[5] * v.z,
                         m[6] * v.x + m[7] * v.y + m[8] * v.z);
}

static shz_vec3_t unit(shz_vec3_t v) {
    float l = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    return l > 1e-6f ? shz_vec3_scale(v, 1.0f / l) : shz_vec3_init(0.0f, 1.0f, 0.0f);
}

static shz_vec3_t cross(shz_vec3_t a, shz_vec3_t b) {
    return shz_vec3_init(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

static float dot(shz_vec3_t a, shz_vec3_t b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float vlen(shz_vec3_t v) { return sqrtf(dot(v, v)); }

/* Columns: up (u), across (s, made square to u), and the third */
static void basis(shz_vec3_t u, shz_vec3_t s, float* b) {
    shz_vec3_t e1 = unit(u);
    shz_vec3_t e2 = unit(shz_vec3_sub(s, shz_vec3_scale(e1, dot(s, e1))));
    shz_vec3_t e3 = cross(e1, e2);
    float m[9] = { e1.x, e2.x, e3.x, e1.y, e2.y, e3.y, e1.z, e2.z, e3.z };
    memcpy(b, m, sizeof(m));
}

/* The least turn that takes unit a to unit b */
static void arc(shz_vec3_t a, shz_vec3_t b, float* r) {
    shz_vec3_t v = cross(a, b);
    float c = dot(a, b);
    if (c < -0.9999f) {                         /* right round: about anything square to a */
        shz_vec3_t o = unit(cross(a, fabsf(a.x) < 0.9f ? shz_vec3_init(1, 0, 0) : shz_vec3_init(0, 1, 0)));
        float m[9] = { 2 * o.x * o.x - 1, 2 * o.x * o.y, 2 * o.x * o.z,
                       2 * o.y * o.x, 2 * o.y * o.y - 1, 2 * o.y * o.z,
                       2 * o.z * o.x, 2 * o.z * o.y, 2 * o.z * o.z - 1 };
        memcpy(r, m, sizeof(m));
        return;
    }
    float k = 1.0f / (1.0f + c);
    float m[9] = { 1 - k * (v.y * v.y + v.z * v.z), -v.z + k * v.x * v.y, v.y + k * v.x * v.z,
                   v.z + k * v.x * v.y, 1 - k * (v.x * v.x + v.z * v.z), -v.x + k * v.y * v.z,
                   -v.y + k * v.x * v.z, v.x + k * v.y * v.z, 1 - k * (v.x * v.x + v.y * v.y) };
    memcpy(r, m, sizeof(m));
}

/* ---- Model space and the world: as dc_draw_ex turns by yaw ---- */

static shz_vec3_t to_world(shz_vec3_t q, shz_vec3_t pos, float yaw) {
    float s = sinf(yaw), c = cosf(yaw);
    return shz_vec3_init(pos.x + q.x * c - q.z * s, pos.y + q.y, pos.z + q.x * s + q.z * c);
}

static shz_vec3_t to_model(shz_vec3_t p, shz_vec3_t pos, float yaw) {
    float s = sinf(yaw), c = cosf(yaw), dx = p.x - pos.x, dz = p.z - pos.z;
    return shz_vec3_init(dx * c + dz * s, p.y - pos.y, -dx * s + dz * c);
}

static shz_vec3_t bone_pos(const DMSModel* m, int b) {
    const shz_mat4x4_t* w = &m->skeleton->bones[b].worldPose;
    return shz_vec3_init(w->elem2D[3][0], w->elem2D[3][1], w->elem2D[3][2]);
}

void rd_track(Ragdoll* r, const DMSModel* m, shz_vec3_t pos, float yaw) {
    for (int i = 0; i < RD_POINTS; i++) {
        shz_vec3_t p = to_world(bone_pos(m, point_bone[i]), pos, yaw);
        r->old[i] = r->tracked ? r->p[i] : p;
        r->p[i] = p;
    }
    r->tracked = true;
}

void rd_start(Ragdoll* r, const DMSModel* m, shz_vec3_t pos, float yaw, shz_vec3_t push) {
    const DMSSkeleton* sk = m->skeleton;
    for (int i = 0; i < RD_POINTS; i++) r->p0[i] = bone_pos(m, point_bone[i]);
    basis(shz_vec3_sub(r->p0[CHEST], r->p0[PELVIS]), shz_vec3_sub(r->p0[RT], r->p0[LT]), r->b0[0]);
    basis(shz_vec3_sub(r->p0[HEAD], r->p0[CHEST]), shz_vec3_sub(r->p0[RUA], r->p0[LUA]), r->b0[1]);
    for (int b = 0; b < bone_count; b++) {
        const shz_mat4x4_t* w = &sk->bones[b].worldPose;
        for (int k = 0; k < 4; k++)
            for (int j = 0; j < 3; j++) r->m0[b][k * 3 + j] = w->elem2D[k][j];
    }
    for (int s = 0; s < N_STICKS; s++)
        r->len[s] = vlen(shz_vec3_sub(r->p0[STICKS[s][0]], r->p0[STICKS[s][1]]));
    /* The knock, a frame's worth, over the upper body */
    shz_vec3_t knock = shz_vec3_scale(push, 1.0f / 60.0f);
    static const int UPPER[] = { CHEST, HEAD, LUA, RUA, PELVIS };
    for (int k = 0; k < 5; k++)
        r->old[UPPER[k]] = shz_vec3_sub(r->old[UPPER[k]], shz_vec3_scale(knock, k == 4 ? 0.5f : 1.0f));
    memcpy(r->rest, r->p, sizeof(r->rest));
    r->calm = 0.0f;
    r->still = false;
}

static void keep(Ragdoll* r, int a, int b, float want, bool at_least) {
    shz_vec3_t d = shz_vec3_sub(r->p[b], r->p[a]);
    float l = vlen(d);
    if (l < 1e-6f || (at_least && l >= want)) return;
    shz_vec3_t fix = shz_vec3_scale(d, 0.5f * (l - want) / l);
    r->p[a] = shz_vec3_add(r->p[a], fix);
    r->p[b] = shz_vec3_sub(r->p[b], fix);
}

/* Out of the world, swept from where it was so a fast one cannot go through
 * a floor; touching, it loses its speed into the surface and most along it */
static void collide(Ragdoll* r, ColWorld* col, int i) {
    float rad = POINTS[i].radius;
    shz_vec3_t d = shz_vec3_sub(r->p[i], r->old[i]);
    float l = vlen(d);
    if (l > rad * 0.5f) {
        ColRayHit h = col_raycast(col, r->old[i], shz_vec3_scale(d, 1.0f / l), l + rad);
        if (h.hit) r->p[i] = shz_vec3_add(r->old[i], shz_vec3_scale(d, fmaxf(h.t - rad, 0.0f) / l));
    }
    shz_vec3_t out = col_move(col, r->old[i], r->p[i], rad);
    shz_vec3_t n = shz_vec3_sub(out, r->p[i]);
    float nl = vlen(n);
    if (nl < 1e-5f && l <= rad * 0.5f) return;
    r->p[i] = out;
    if (nl < 1e-5f) return;
    n = shz_vec3_scale(n, 1.0f / nl);
    shz_vec3_t v = shz_vec3_sub(r->p[i], r->old[i]);
    float vn = dot(v, n);
    if (vn < 0.0f) v = shz_vec3_sub(v, shz_vec3_scale(n, vn));
    r->old[i] = shz_vec3_sub(r->p[i], shz_vec3_scale(v, 0.3f));
}

void rd_step(Ragdoll* r, ColWorld* col, float dt) {
    if (r->still) return;
    for (int i = 0; i < RD_POINTS; i++) {
        shz_vec3_t v = shz_vec3_sub(r->p[i], r->old[i]);
        float l = vlen(v);
        if (l > 0.15f) v = shz_vec3_scale(v, 0.15f / l);     /* 9 m/s at most */
        r->old[i] = r->p[i];
        r->p[i] = shz_vec3_add(r->p[i], shz_vec3_scale(v, 0.99f));
        r->p[i].y -= 9.8f * dt * dt;
    }
    for (int it = 0; it < 6; it++) {
        for (int s = 0; s < N_STICKS; s++) keep(r, STICKS[s][0], STICKS[s][1], r->len[s], false);
        for (int f = 0; f < 4; f++) {
            int a = FOLDS[f][0], b = FOLDS[f][1], c = FOLDS[f][2];
            float limb = vlen(shz_vec3_sub(r->p0[a], r->p0[b])) +
                         vlen(shz_vec3_sub(r->p0[b], r->p0[c]));
            keep(r, a, c, limb * FOLD_MIN, true);
        }
        if (it == 2 || it == 5)
            for (int i = 0; i < RD_POINTS; i++) collide(r, col, i);
    }
    /* None gone 2 cm in half a second (a shiver where it lies is not moving):
     * it has come to rest */
    float most = 0.0f;
    for (int i = 0; i < RD_POINTS; i++) most = fmaxf(most, vlen(shz_vec3_sub(r->p[i], r->rest[i])));
    if (most > 0.02f) {
        memcpy(r->rest, r->p, sizeof(r->rest));
        r->calm = 0.0f;
    } else if ((r->calm += dt) > 0.5f) {
        r->still = true;
    }
}

void rd_pose(const Ragdoll* r, DMSModel* m, shz_vec3_t pos, float yaw) {
    shz_vec3_t q[RD_POINTS];
    for (int i = 0; i < RD_POINTS; i++) q[i] = to_model(r->p[i], pos, yaw);

    /* Each frame's turn from how it was when it went limp */
    float rot[RD_FRAMES][9], b[9], t[9];
    for (int f = 0; f < RD_FRAMES; f++) {
        if (FRAMES[f].parent < 0) {
            if (f == 0) basis(shz_vec3_sub(q[CHEST], q[PELVIS]), shz_vec3_sub(q[RT], q[LT]), b);
            else        basis(shz_vec3_sub(q[HEAD], q[CHEST]), shz_vec3_sub(q[RUA], q[LUA]), b);
            const float* b0 = r->b0[f];
            float b0t[9] = { b0[0], b0[3], b0[6], b0[1], b0[4], b0[7], b0[2], b0[5], b0[8] };
            mul(b, b0t, rot[f]);
        } else {
            const float* rp = rot[FRAMES[f].parent];
            int o = FRAMES[f].origin, a = FRAMES[f].aim;
            shz_vec3_t was = apply(rp, unit(shz_vec3_sub(r->p0[a], r->p0[o])));
            arc(was, unit(shz_vec3_sub(q[a], q[o])), t);
            mul(t, rp, rot[f]);
        }
    }

    /* Each bone turned with its frame, about the frame's joint */
    DMSSkeleton* sk = m->skeleton;
    for (int bi = 0; bi < bone_count; bi++) {
        int f = bone_frame[bi], o = FRAMES[f].origin;
        const float* m0 = r->m0[bi];
        const float* R = rot[f];
        shz_mat4x4_t* w = &sk->bones[bi].worldPose;
        for (int k = 0; k < 3; k++) {
            shz_vec3_t c = apply(R, shz_vec3_init(m0[k * 3], m0[k * 3 + 1], m0[k * 3 + 2]));
            w->elem2D[k][0] = c.x; w->elem2D[k][1] = c.y; w->elem2D[k][2] = c.z; w->elem2D[k][3] = 0.0f;
        }
        shz_vec3_t rel = shz_vec3_sub(shz_vec3_init(m0[9], m0[10], m0[11]), r->p0[o]);
        shz_vec3_t at = shz_vec3_add(apply(R, rel), q[o]);
        w->elem2D[3][0] = at.x; w->elem2D[3][1] = at.y; w->elem2D[3][2] = at.z; w->elem2D[3][3] = 1.0f;
    }
    dc_model_pose_from_bones(m);
}
