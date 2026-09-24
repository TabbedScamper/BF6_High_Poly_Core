/* TWO-BONE ARM IK TO A HAND TARGET, on a skeleton given as parent indices.
 *
 * The same solve as loadout.cpp's solve_arm_ik, which is ported from the research UI
 * viewer's SolveSoldierArmIk (impl/bf6_ui_tool/source/viewer/bf6viewer.cpp) - the
 * oracle that poses the class stances correctly. Kept here in its own namespace so the
 * first-person runtime can run it on its own pose without reaching into loadout.cpp's
 * anonymous namespace.
 *
 * What is the GAME'S: the target. The weapon carries Wep_IK_LeftHand / Wep_IK_RightHand
 * markers, and with the upper body's hand IK disabled the authored clips put each hand
 * on its marker to within 0.7 mm and 0.1 degrees (core/test/hand_ik_probe). What is the
 * viewer's reconstruction rather than the engine's solver: the elbow. It stays on the
 * side it was already bending to, which is where the pose underneath put it.
 *
 * Matrices are 3x4 row-vector transforms (rows right, up, forward, translation), a then
 * b = mul(a, b), model = local * parent_model. */
#ifndef BF6_ARM_IK_H
#define BF6_ARM_IK_H

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace bf6ik {

struct M { float m[12]; };

inline M mul(const M& a, const M& b)
{
    M r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 3; ++j) {
            double s = i == 3 ? b.m[9 + j] : 0.0;
            for (int k = 0; k < 3; ++k) s += (double)a.m[i * 3 + k] * b.m[k * 3 + j];
            r.m[i * 3 + j] = (float)s;
        }
    return r;
}

inline bool inverse(const M& a, M& out)
{
    const double m00 = a.m[0], m01 = a.m[1], m02 = a.m[2], m10 = a.m[3], m11 = a.m[4], m12 = a.m[5],
                 m20 = a.m[6], m21 = a.m[7], m22 = a.m[8];
    const double det = m00 * (m11 * m22 - m12 * m21) - m01 * (m10 * m22 - m12 * m20) + m02 * (m10 * m21 - m11 * m20);
    if (std::fabs(det) < 1e-20) return false;
    const double inv[9] = {
        (m11 * m22 - m12 * m21) / det, -(m01 * m22 - m02 * m21) / det, (m01 * m12 - m02 * m11) / det,
        -(m10 * m22 - m12 * m20) / det, (m00 * m22 - m02 * m20) / det, -(m00 * m12 - m02 * m10) / det,
        (m10 * m21 - m11 * m20) / det, -(m00 * m21 - m01 * m20) / det, (m00 * m11 - m01 * m10) / det};
    for (int k = 0; k < 9; ++k) out.m[k] = (float)inv[k];
    for (int j = 0; j < 3; ++j)
        out.m[9 + j] = (float)-(a.m[9] * inv[0 + j] + a.m[10] * inv[3 + j] + a.m[11] * inv[6 + j]);
    return true;
}

/* Turn a model-space basis so `from` points along `to`, keeping its position. */
inline bool rotate_basis(M& m, const float from_in[3], const float to_in[3])
{
    auto len = [](const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
    float from[3] = { from_in[0], from_in[1], from_in[2] }, to[3] = { to_in[0], to_in[1], to_in[2] };
    const float lf = len(from), lt = len(to);
    if (lf < 1e-12f || lt < 1e-12f) return false;
    for (int k = 0; k < 3; ++k) { from[k] /= lf; to[k] /= lt; }
    float dot = from[0] * to[0] + from[1] * to[1] + from[2] * to[2];
    if (!std::isfinite(dot)) return false;
    dot = std::max(-1.f, std::min(dot, 1.f));
    float axis[3] = { from[1] * to[2] - from[2] * to[1], from[2] * to[0] - from[0] * to[2], from[0] * to[1] - from[1] * to[0] };
    const float al = len(axis);
    if (al < 1e-6f) {
        if (dot > 0.999999f) return true;
        const float fb[3] = { std::fabs(from[0]) < 0.8f ? 1.f : 0.f, std::fabs(from[0]) < 0.8f ? 0.f : 1.f, 0.f };
        axis[0] = from[1] * fb[2] - from[2] * fb[1];
        axis[1] = from[2] * fb[0] - from[0] * fb[2];
        axis[2] = from[0] * fb[1] - from[1] * fb[0];
        const float l2 = len(axis);
        if (l2 < 1e-12f) return false;
        for (int k = 0; k < 3; ++k) axis[k] /= l2;
    } else {
        for (int k = 0; k < 3; ++k) axis[k] /= al;
    }
    const float ang = std::acos(dot), cs = std::cos(ang), sn = std::sin(ang), t = 1.f - cs;
    const float x = axis[0], y = axis[1], z = axis[2];
    M r{};
    r.m[0] = t * x * x + cs;     r.m[1] = t * x * y + sn * z; r.m[2] = t * x * z - sn * y;
    r.m[3] = t * x * y - sn * z; r.m[4] = t * y * y + cs;     r.m[5] = t * y * z + sn * x;
    r.m[6] = t * x * z + sn * y; r.m[7] = t * y * z - sn * x; r.m[8] = t * z * z + cs;
    const float px = m.m[9], py = m.m[10], pz = m.m[11];
    m = mul(m, r);
    m.m[9] = px; m.m[10] = py; m.m[11] = pz;
    return true;
}

/* Recompose every bone after `from` (topological order) from its local and its parent. */
inline void recompose_after(const std::vector<int>& parent, int from, const std::vector<M>& local, std::vector<M>& model)
{
    for (size_t b = (size_t)from + 1; b < parent.size(); ++b) {
        const int p = parent[b];
        if (p >= 0 && p < (int)b) model[b] = mul(local[b], model[(size_t)p]);
    }
}

/* arm -> fore -> hand to `target` (position and orientation). `model` is updated and
 * the three bones' locals are re-derived from it; everything else is left alone. */
inline bool solve_arm(const std::vector<int>& parent, int arm, int fore, int hand, const M& target,
                      std::vector<M>& local, std::vector<M>& model)
{
    if (arm < 0 || fore < 0 || hand < 0 || parent[(size_t)fore] != arm || parent[(size_t)hand] != fore) return false;
    auto P = [&](int b, float* o) { o[0] = model[(size_t)b].m[9]; o[1] = model[(size_t)b].m[10]; o[2] = model[(size_t)b].m[11]; };
    auto sub = [](const float* a, const float* b, float* o) { for (int k = 0; k < 3; ++k) o[k] = a[k] - b[k]; };
    auto len = [](const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
    auto cross = [](const float* a, const float* b, float* o) {
        o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; };
    float sh[3], el[3], wr[3], rq[3] = { target.m[9], target.m[10], target.m[11] };
    P(arm, sh); P(fore, el); P(hand, wr);
    float u[3], l[3], reach[3];
    sub(el, sh, u); sub(wr, el, l); sub(rq, sh, reach);
    const float ul = len(u), ll = len(l), rl = len(reach);
    if (ul < 1e-5f || ll < 1e-5f || rl < 1e-5f) return false;
    const float dir[3] = { reach[0] / rl, reach[1] / rl, reach[2] / rl };
    const float minr = std::fabs(ul - ll) + 1e-5f, maxr = ul + ll - 1e-5f;
    const float sr = std::max(minr, std::min(rl, maxr));
    const float st[3] = { sh[0] + dir[0] * sr, sh[1] + dir[1] * sr, sh[2] + dir[2] * sr };
    float nrm[3];
    cross(u, l, nrm);
    if (len(nrm) < 1e-5f) { const float up[3] = { 0, 1, 0 }; cross(dir, up, nrm); }
    if (len(nrm) < 1e-5f) { const float xa[3] = { 1, 0, 0 }; cross(dir, xa, nrm); }
    { const float nl = len(nrm); for (int k = 0; k < 3; ++k) nrm[k] /= nl; }
    float bend[3];
    cross(nrm, dir, bend);
    { const float bl = len(bend); if (bl < 1e-12f) return false; for (int k = 0; k < 3; ++k) bend[k] /= bl; }
    const float along = (ul * ul + sr * sr - ll * ll) / (2.f * sr);
    const float perp = std::sqrt(std::max(0.f, ul * ul - along * along));
    float ea[3], eb[3];
    for (int k = 0; k < 3; ++k) { ea[k] = sh[k] + dir[k] * along + bend[k] * perp; eb[k] = sh[k] + dir[k] * along - bend[k] * perp; }
    auto d2 = [](const float* a, const float* b) { float s2 = 0; for (int k = 0; k < 3; ++k) s2 += (a[k] - b[k]) * (a[k] - b[k]); return s2; };
    const float* se = d2(ea, el) <= d2(eb, el) ? ea : eb;
    const std::vector<M> keep = model;
    float from[3], to[3];
    sub(el, sh, from); sub(se, sh, to);
    if (!rotate_basis(model[(size_t)arm], from, to)) { model = keep; return false; }
    recompose_after(parent, arm, local, model);
    float me[3], mw[3];
    P(fore, me); P(hand, mw);
    sub(mw, me, from); sub(st, me, to);
    if (!rotate_basis(model[(size_t)fore], from, to)) { model = keep; return false; }
    recompose_after(parent, fore, local, model);
    model[(size_t)hand] = target;
    if (rl > maxr || rl < minr) { model[(size_t)hand].m[9] = st[0]; model[(size_t)hand].m[10] = st[1]; model[(size_t)hand].m[11] = st[2]; }
    /* the three bones' locals from the solved model; their descendants keep their
     * locals and are recomposed, so fingers follow the hand */
    for (int b : { arm, fore, hand }) {
        M inv;
        const int p = parent[(size_t)b];
        if (p >= 0 && inverse(model[(size_t)p], inv)) local[(size_t)b] = mul(model[(size_t)b], inv);
    }
    recompose_after(parent, hand, local, model);
    return true;
}

} // namespace bf6ik

#endif
