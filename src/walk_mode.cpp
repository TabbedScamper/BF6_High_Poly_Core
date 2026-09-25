// Rays and walking at eye level (bf6_ray_scene_*, bf6_walk_step). See bf6_core.h.
//
// THE RAYS COME FROM THE DRAWN TRIANGLES, NOT FROM PHYSICS. The Unreal SDK found
// that cooking physics collision for a map's terrain and assets meshes was ~97% of
// the cost of putting the map on screen, and that nothing but its own rays ever
// used it - placement, the click classifier, scatter, the walk. So those rays are
// answered from a uniform grid over the triangles instead, which is one linear
// pass over data the editor already holds. This is that grid (FBF6RayIndex) and
// that walk (TickWalk), moved here so Godot walks the same way without generating
// a single collider.
//
// Everything is metres, Y up.

#include "bf6_core.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

struct V3f { float x, y, z; };
struct V3 { double x, y, z; };

inline V3 sub(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 add(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 mul(const V3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(const V3& a, const V3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double len(const V3& a) { return std::sqrt(dot(a, a)); }
inline V3 norm(const V3& a) { const double l = len(a); return l > 1e-12 ? mul(a, 1.0 / l) : V3{0, 0, 0}; }

// Moller-Trumbore, two sided: terrain is looked at from underneath as often as
// from above.
inline bool ray_tri(const V3& o, const V3& d, const V3f& fa, const V3f& fb, const V3f& fc, double& out_t, V3& out_n)
{
    const V3 a{fa.x, fa.y, fa.z}, b{fb.x, fb.y, fb.z}, c{fc.x, fc.y, fc.z};
    const V3 e1 = sub(b, a), e2 = sub(c, a);
    const V3 p = cross(d, e2);
    const double det = dot(e1, p);
    if (std::fabs(det) < 1e-12) return false;
    const double inv = 1.0 / det;
    const V3 t = sub(o, a);
    const double u = dot(t, p) * inv;
    if (u < -1e-5 || u > 1.0 + 1e-5) return false;
    const V3 q = cross(t, e1);
    const double v = dot(d, q) * inv;
    if (v < -1e-5 || u + v > 1.0 + 1e-5) return false;
    const double dist = dot(e2, q) * inv;
    if (dist < 0.0) return false;
    out_t = dist;
    out_n = norm(cross(e1, e2));
    return true;
}

// One mesh's grid, in the mesh's own space, over the ground plane (x, z).
struct Mesh
{
    std::vector<V3f> v;
    std::vector<int32_t> tri;
    V3f mn{FLT_MAX, FLT_MAX, FLT_MAX}, mx{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    int nx = 0, nz = 0;
    float cx = 1, cz = 1;
    std::vector<int32_t> cell_start, cell_tri, stamp;
    int32_t epoch = 0;

    int32_t tris() const { return int32_t(tri.size() / 3); }
    int cell_x(float x) const { return std::clamp(int((x - mn.x) / cx), 0, nx - 1); }
    int cell_z(float z) const { return std::clamp(int((z - mn.z) / cz), 0, nz - 1); }

    void build()
    {
        const int32_t nt = tris();
        for (const V3f& p : v) {
            mn = {std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)};
            mx = {std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
        }
        const float pad = 0.1f;
        mn = {mn.x - pad, mn.y - pad, mn.z - pad};
        mx = {mx.x + pad, mx.y + pad, mx.z + pad};
        // Cell size follows the geometry, not the triangle count: a cell smaller
        // than one triangle lists that triangle over and over and buys nothing.
        const float span_x = mx.x - mn.x, span_z = mx.z - mn.z;
        double sum = 0;
        int samples = 0;
        const int32_t stride = std::max<int32_t>(1, nt / 4096);
        for (int32_t t = 0; t < nt; t += stride) {
            const V3f& a = v[tri[t * 3]]; const V3f& b = v[tri[t * 3 + 1]]; const V3f& c = v[tri[t * 3 + 2]];
            const float dx = std::max({a.x, b.x, c.x}) - std::min({a.x, b.x, c.x});
            const float dz = std::max({a.z, b.z, c.z}) - std::min({a.z, b.z, c.z});
            sum += std::max(dx, dz);
            ++samples;
        }
        const float avg = samples ? float(sum / samples) : 1.0f;
        const float max_span = std::max(span_x, span_z);
        const float cell = std::clamp(avg * 2.0f, max_span / 1024.0f, std::max(max_span / 8.0f, max_span / 1024.0f));
        nx = std::clamp(int(std::ceil(span_x / std::max(cell, 1e-6f))), 1, 1024);
        nz = std::clamp(int(std::ceil(span_z / std::max(cell, 1e-6f))), 1, 1024);
        cx = std::max(span_x / nx, 1e-6f);
        cz = std::max(span_z / nz, 1e-6f);
        const int nc = nx * nz;
        std::vector<int32_t> count(nc, 0);
        auto range = [&](int32_t t, int& x0, int& x1, int& z0, int& z1) {
            const V3f& a = v[tri[t * 3]]; const V3f& b = v[tri[t * 3 + 1]]; const V3f& c = v[tri[t * 3 + 2]];
            x0 = cell_x(std::min({a.x, b.x, c.x})); x1 = cell_x(std::max({a.x, b.x, c.x}));
            z0 = cell_z(std::min({a.z, b.z, c.z})); z1 = cell_z(std::max({a.z, b.z, c.z}));
        };
        for (int32_t t = 0; t < nt; ++t) {
            int x0, x1, z0, z1; range(t, x0, x1, z0, z1);
            for (int z = z0; z <= z1; ++z) for (int x = x0; x <= x1; ++x) count[z * nx + x]++;
        }
        cell_start.resize(nc + 1);
        int32_t run = 0;
        for (int c = 0; c < nc; ++c) { cell_start[c] = run; run += count[c]; }
        cell_start[nc] = run;
        cell_tri.resize(run);
        std::vector<int32_t> cursor(cell_start.begin(), cell_start.end() - 1);
        for (int32_t t = 0; t < nt; ++t) {
            int x0, x1, z0, z1; range(t, x0, x1, z0, z1);
            for (int z = z0; z <= z1; ++z) for (int x = x0; x <= x1; ++x) cell_tri[cursor[z * nx + x]++] = t;
        }
        stamp.assign(nt, 0);
    }

    // Segment a -> b in this mesh's space. out_u along the segment, out_n local.
    bool trace(const V3& a, const V3& b, double& out_u, V3& out_n)
    {
        if (tri.empty() || nx <= 0) return false;
        V3 dir = sub(b, a);
        const double length = len(dir);
        if (length <= 1e-12) return false;
        dir = mul(dir, 1.0 / length);
        // clip against the whole mesh first: most rays miss it outright
        double t0 = 0, t1 = length;
        const double o[3] = {a.x, a.y, a.z}, d[3] = {dir.x, dir.y, dir.z};
        const double lo[3] = {mn.x, mn.y, mn.z}, hi[3] = {mx.x, mx.y, mx.z};
        for (int axis = 0; axis < 3; ++axis) {
            if (std::fabs(d[axis]) < 1e-12) { if (o[axis] < lo[axis] || o[axis] > hi[axis]) return false; continue; }
            double ta = (lo[axis] - o[axis]) / d[axis], tb = (hi[axis] - o[axis]) / d[axis];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            if (t0 > t1) return false;
        }
        const V3 entry = add(a, mul(dir, t0));
        int x = cell_x(float(entry.x)), z = cell_z(float(entry.z));
        const int step_x = dir.x > 0 ? 1 : (dir.x < 0 ? -1 : 0);
        const int step_z = dir.z > 0 ? 1 : (dir.z < 0 ? -1 : 0);
        double tmax_x = DBL_MAX, tmax_z = DBL_MAX, tdelta_x = DBL_MAX, tdelta_z = DBL_MAX;
        if (step_x) {
            const double edge = mn.x + (x + (step_x > 0 ? 1 : 0)) * double(cx);
            tmax_x = t0 + (edge - entry.x) / dir.x;
            tdelta_x = cx / std::fabs(dir.x);
        }
        if (step_z) {
            const double edge = mn.z + (z + (step_z > 0 ? 1 : 0)) * double(cz);
            tmax_z = t0 + (edge - entry.z) / dir.z;
            tdelta_z = cz / std::fabs(dir.z);
        }
        if (++epoch == INT32_MAX) { std::fill(stamp.begin(), stamp.end(), 0); epoch = 1; }
        double best = t1;
        bool any = false;
        V3 best_n{0, 1, 0};
        for (;;) {
            const int cell = z * nx + x;
            for (int32_t i = cell_start[cell], e = cell_start[cell + 1]; i < e; ++i) {
                const int32_t t = cell_tri[i];
                if (stamp[t] == epoch) continue;
                stamp[t] = epoch;
                double hit; V3 n;
                if (!ray_tri(a, dir, v[tri[t * 3]], v[tri[t * 3 + 1]], v[tri[t * 3 + 2]], hit, n)) continue;
                if (hit >= t0 - 0.01 && hit <= best) { best = hit; best_n = n; any = true; }
            }
            // walking away from a hit already in hand: nothing further can beat it
            const double next_t = std::min(tmax_x, tmax_z);
            if (any && best <= next_t) break;
            if (next_t > t1) break;
            if (tmax_x < tmax_z) { x += step_x; if (x < 0 || x >= nx) break; tmax_x += tdelta_x; }
            else { z += step_z; if (z < 0 || z >= nz) break; tmax_z += tdelta_z; }
        }
        if (!any) return false;
        out_u = best / length;
        out_n = best_n;
        return true;
    }
};

struct Instance
{
    int32_t mesh = -1;
    double m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    double inv[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    V3 lo{0, 0, 0}, hi{0, 0, 0};   // world bounds
};

V3 xform(const double* m, const V3& p)
{
    return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3],
            m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
            m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

bool invert(const double* m, double* out)
{
    const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const double det = a * A + b * B + c * C;
    if (std::fabs(det) < 1e-18) return false;
    const double s = 1.0 / det;
    out[0] = A * s; out[1] = -(b * i - c * h) * s; out[2] = (b * f - c * e) * s;
    out[4] = B * s; out[5] = (a * i - c * g) * s;  out[6] = -(a * f - c * d) * s;
    out[8] = C * s; out[9] = -(a * h - b * g) * s; out[10] = (a * e - b * d) * s;
    const V3 t{m[3], m[7], m[11]};
    out[3] = -(out[0] * t.x + out[1] * t.y + out[2] * t.z);
    out[7] = -(out[4] * t.x + out[5] * t.y + out[6] * t.z);
    out[11] = -(out[8] * t.x + out[9] * t.y + out[10] * t.z);
    return true;
}

// segment against an axis-aligned box: the entry parameter, or false
bool segment_box(const V3& o, const V3& e, const V3& lo, const V3& hi, double& out_u)
{
    const V3 d = sub(e, o);
    double t0 = 0, t1 = 1;
    const double oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z}, l[3] = {lo.x, lo.y, lo.z}, h[3] = {hi.x, hi.y, hi.z};
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(dd[axis]) < 1e-12) { if (oo[axis] < l[axis] || oo[axis] > h[axis]) return false; continue; }
        double ta = (l[axis] - oo[axis]) / dd[axis], tb = (h[axis] - oo[axis]) / dd[axis];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    out_u = t0;
    return true;
}

} // namespace

struct bf6_ray_scene
{
    std::vector<std::unique_ptr<Mesh>> meshes;
    std::vector<Instance> instances;
    std::vector<std::pair<double, int32_t>> candidates;
};

extern "C" {

bf6_ray_scene* bf6_ray_scene_create(void) { return new bf6_ray_scene(); }

void bf6_ray_scene_free(bf6_ray_scene* s) { delete s; }

int32_t bf6_ray_scene_add_mesh(bf6_ray_scene* s, const float* positions, int32_t vertex_count,
                               const int32_t* indices, int32_t index_count)
{
    if (!s || !positions || !indices || vertex_count <= 0 || index_count < 3) return -1;
    auto m = std::make_unique<Mesh>();
    m->v.resize(vertex_count);
    for (int32_t i = 0; i < vertex_count; ++i) m->v[i] = {positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]};
    m->tri.reserve(index_count - index_count % 3);
    for (int32_t i = 0; i + 2 < index_count; i += 3) {
        const int32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a < 0 || b < 0 || c < 0 || a >= vertex_count || b >= vertex_count || c >= vertex_count) continue;
        m->tri.push_back(a); m->tri.push_back(b); m->tri.push_back(c);
    }
    if (m->tri.empty()) return -1;
    m->build();
    s->meshes.push_back(std::move(m));
    return int32_t(s->meshes.size() - 1);
}

int32_t bf6_ray_scene_add_instance(bf6_ray_scene* s, int32_t mesh, const double* transform)
{
    if (!s || !transform || mesh < 0 || mesh >= int32_t(s->meshes.size())) return -1;
    Instance in;
    in.mesh = mesh;
    std::copy(transform, transform + 12, in.m);
    if (!invert(in.m, in.inv)) return -1;
    const Mesh& ms = *s->meshes[mesh];
    in.lo = {DBL_MAX, DBL_MAX, DBL_MAX};
    in.hi = {-DBL_MAX, -DBL_MAX, -DBL_MAX};
    for (int k = 0; k < 8; ++k) {
        const V3 p = xform(in.m, {k & 1 ? ms.mx.x : ms.mn.x, k & 2 ? ms.mx.y : ms.mn.y, k & 4 ? ms.mx.z : ms.mn.z});
        in.lo = {std::min(in.lo.x, p.x), std::min(in.lo.y, p.y), std::min(in.lo.z, p.z)};
        in.hi = {std::max(in.hi.x, p.x), std::max(in.hi.y, p.y), std::max(in.hi.z, p.z)};
    }
    s->instances.push_back(in);
    return int32_t(s->instances.size() - 1);
}

void bf6_ray_scene_clear_instances(bf6_ray_scene* s)
{
    if (s) s->instances.clear();
}

int32_t bf6_ray_scene_counts(bf6_ray_scene* s, int32_t* meshes, int32_t* instances, int64_t* triangles)
{
    if (!s) return -1;
    if (meshes) *meshes = int32_t(s->meshes.size());
    if (instances) *instances = int32_t(s->instances.size());
    if (triangles) {
        int64_t n = 0;
        for (const auto& m : s->meshes) n += m->tris();
        *triangles = n;
    }
    return 0;
}

int32_t bf6_ray_scene_trace(bf6_ray_scene* s, const double* from, const double* to, double* out)
{
    if (!s || !from || !to) return -1;
    const V3 o{from[0], from[1], from[2]}, e{to[0], to[1], to[2]};
    // Bounds first, over every instance, sorted: only the nearest few get their
    // triangles opened, and everything starting past the best hit is skipped.
    s->candidates.clear();
    for (int32_t i = 0; i < int32_t(s->instances.size()); ++i) {
        double u;
        if (segment_box(o, e, s->instances[i].lo, s->instances[i].hi, u)) s->candidates.push_back({u, i});
    }
    std::sort(s->candidates.begin(), s->candidates.end());
    double best_u = 1.0;
    int32_t best_i = -1;
    V3 best_n{0, 1, 0};
    for (const auto& c : s->candidates) {
        if (c.first > best_u) break;
        const Instance& in = s->instances[c.second];
        const V3 la = xform(in.inv, o), lb = xform(in.inv, e);
        double u; V3 ln;
        /* A hit at u == 1 is still on the requested closed segment. */
        if (!s->meshes[in.mesh]->trace(la, lb, u, ln) || u > best_u) continue;
        best_u = u;
        best_i = c.second;
        // a normal goes through the inverse transpose
        best_n = norm({in.inv[0] * ln.x + in.inv[4] * ln.y + in.inv[8] * ln.z,
                       in.inv[1] * ln.x + in.inv[5] * ln.y + in.inv[9] * ln.z,
                       in.inv[2] * ln.x + in.inv[6] * ln.y + in.inv[10] * ln.z});
    }
    if (best_i < 0) return -1;
    if (out) {
        const V3 hit = add(o, mul(sub(e, o), best_u));
        if (dot(best_n, sub(o, hit)) < 0) best_n = mul(best_n, -1.0);   // face the ray
        out[0] = hit.x; out[1] = hit.y; out[2] = hit.z;
        out[3] = best_n.x; out[4] = best_n.y; out[5] = best_n.z;
        out[6] = best_u;
    }
    return best_i;
}

// ------------------------------------------------------------------ walking
//
// The Unreal SDK's TickWalk, in metres, now taking its numbers from
// bf6_walk_tuning instead of a constants block.
//
// WHAT CHANGED AND WHY IT MATTERS: the step height, the jump's horizontal cap
// and the landing penalty are AUTHORED BY THE GAME and were previously the host
// engine's own character defaults. Step height especially - 0.45 against an
// authored 0.32 - meant kerbs the game makes you jump were being walked over.
// The SPEEDS are still the engine's, because BF6 does not author an absolute
// walk or sprint speed anywhere; see bf6_walk_tuning.

/* eye_final: the caller's st->eye is ALREADY the height to stand at, crouch included -
 * a first-person view that follows the game's own animated camera joint, which eases
 * through a stance change over 18-60 frames. The walker's own crouch scale (0.58 of the
 * standing eye) is then not applied on top: applied, it dropped the view on the frame
 * crouch was pressed and then held it 40 % below the game's crouch camera. */
static void walk_step_impl(bf6_walk_state* st, const bf6_walk_input* in,
                           const bf6_walk_tuning* tuning, bf6_walk_ray_fn ray, void* user,
                           bool eye_final)
{
    if (!st || !in || !ray) return;
    bf6_walk_tuning fallback;
    if (!tuning) { bf6_walk_tuning_defaults(&fallback); tuning = &fallback; }
    const bf6_walk_tuning& T = *tuning;
    const double step_up = T.step_height, radius = T.radius;
    const double dt = std::clamp(in->dt, 0.001, 0.1);   // a stall must not fling the walker
    const bool airborne_at_entry = st->grounded == 0;
    const double falling = st->vel_up;   // read before gravity, for the landing penalty
    if (in->jump && st->grounded) {
        st->vel_up = T.jump_speed;
        st->grounded = 0;
        /* A JUMP FROM A STANDSTILL CARRIES NOTHING FORWARD. Below
         * Jump_MinSpeedForHorizontalImpulse the game gives no horizontal
         * impulse, which is why a standing jump in BF6 goes straight up rather
         * than drifting off in whatever direction you were leaning. */
        const double speed = std::sqrt(st->vel[0] * st->vel[0] + st->vel[2] * st->vel[2]);
        if (speed < T.jump_min_speed_for_impulse) { st->vel[0] = 0.0; st->vel[2] = 0.0; }
    }

    V3 pos{st->pos[0], st->pos[1], st->pos[2]};
    // THE HEIGHT ACTUALLY STOOD AT, which is not the standing eye while crouched.
    // Unreal measured every probe from the standing eye, which puts the knee probe
    // and the ground probe 0.72 m lower than the crouched feet: below the floor.
    // Walking crouched uphill then hit the terrain from underneath, and standing
    // up probed from under the ground, found nothing on a terrain sheet (or the
    // underside of a crate), and left the walker at crouch height.
    const double stood = st->eye_last > 0.0 ? st->eye_last : st->eye;
    V3 want{in->wish[0], 0.0, in->wish[2]};
    if (len(want) > 1.0) want = norm(want);
    // Accelerate toward the wanted direction and coast when input stops, so
    // starting, stopping and turning have weight.
    /* THE LANDING PENALTY scales the top speed down after a drop and recovers
     * over the next couple of seconds, so stepping off something and sprinting
     * away instantly is not free. Strength, floor and recovery rate are all
     * authored; the penalty rides in vel[1], which the walker does not
     * otherwise use and which callers already carry across steps. */
    if (!airborne_at_entry) st->vel[1] = 1.0;              /* on the ground, no penalty */
    double penalty = st->vel[1] > 0.0 ? st->vel[1] : 1.0;
    penalty = std::min(1.0, penalty + T.landing_recovery_per_second * dt);

    const double base_top = in->crouch ? T.crouch_speed : (in->run ? T.run_speed : T.walk_speed);
    const double top = base_top * penalty;
    const V3 target = mul(want, top);
    /* AIR CONTROL IS NOT GROUND CONTROL, and in the air it is the authored
     * Jump_StrafeSpeed rather than a share of the walking acceleration. */
    const double accel = st->grounded ? T.accel_ground : T.accel_air;
    V3 vel{st->vel[0], 0.0, st->vel[2]};
    V3 delta = sub(target, vel);
    const double dl = len(delta), max_step = accel * dt;
    vel = dl <= max_step ? target : add(vel, mul(delta, max_step / dl));
    V3 move = mul(vel, dt);

    // ---- horizontal: slide along whatever we run into ----
    const double skin = 0.08;   // a small gap, or probes start inside the wall
    double hit[3], nrm[3];
    for (int pass = 0; pass < 4 && len(move) > 1e-6; ++pass) {
        const double length = len(move);
        const V3 dir = mul(move, 1.0 / length);
        bool blocked = false;
        V3 normal{0, 1, 0};
        // knees, chest and head: a doorway must pass, a railing must not
        for (const double h : {-stood + step_up + 0.10, -stood * 0.45, -0.10}) {
            const V3 from = add(pos, {0, h, 0});
            const V3 to = add(from, mul(dir, length + radius));
            const double f[3] = {from.x, from.y, from.z}, t[3] = {to.x, to.y, to.z};
            if (!ray(user, f, t, hit, nrm)) continue;
            blocked = true;
            normal = {nrm[0], nrm[1], nrm[2]};
            break;
        }
        if (!blocked) { pos = add(pos, move); break; }
        // keep the part of the move that runs along the surface
        normal.y = 0;
        if (len(normal) < 1e-6) break;
        normal = norm(normal);
        move = sub(move, mul(normal, dot(move, normal)));
        move = sub(move, mul(dir, std::min(length, skin)));   // never end the frame inside it
    }

    // ---- vertical: gravity, ground, and the slope you are allowed to stand on ----
    const bool was_on_floor = st->grounded != 0;   // snapping is only for walkers
    st->vel_up = std::max(st->vel_up - T.gravity * dt, -30.0);
    pos.y += st->vel_up * dt;

    /* JUMP_HORIZONTALVELOCITYCAP. Airborne speed is capped outright, so a
     * run-up cannot be converted into an arbitrarily long jump. */
    if (!was_on_floor && T.jump_horizontal_cap > 0.0) {
        const double speed = std::sqrt(vel.x * vel.x + vel.z * vel.z);
        if (speed > T.jump_horizontal_cap) {
            const double k = T.jump_horizontal_cap / speed;
            vel.x *= k;
            vel.z *= k;
        }
    }

    const double eye = in->crouch && !eye_final ? st->eye * 0.58 : st->eye;
    // from the feet as they are now, so standing up searches from above the floor
    const double probe[3] = {pos.x, pos.y - std::min(eye, stood) + step_up, pos.z};
    st->eye_last = eye;
    const double down[3] = {pos.x, probe[1] - 1000.0, pos.z};
    if (ray(user, probe, down, hit, nrm)) {
        const double stand = hit[1] + eye;
        const bool walkable = nrm[1] >= 0.71;   // about 45 degrees
        // Snap down only when already on the floor and the ground dropped by less
        // than a step - stairs and ramps. Applied while airborne it cut jumps short.
        /* A LOWER EYE ON THE FLOOR IS THE BODY CROUCHING, NOT A FALL. With eye_final the
         * caller's eye rides the graph's CameraJoint, already on its authored curve; left
         * to this test the 0.58 m stand-to-crouch drop exceeded a step, the walker went
         * airborne and fell to the new height over 8 frames under gravity while the arms,
         * anchored on the joint, were already there - the rifle leapt 580 mm up the
         * screen on the first crouch frame (native/render_walk.gd crouch). */
        const bool crouched_down = eye_final && was_on_floor && eye < stood - 1e-6;
        const bool snap = was_on_floor && st->vel_up <= 0.0 && ((pos.y - stand) <= step_up || crouched_down);
        if (walkable && (pos.y <= stand || snap)) {
            pos.y = stand;
            /* TOUCHDOWN. A hard landing costs speed, scaled by how fast the
             * fall was and floored by Jump_LandingPenalty_MinScaleClamp so it
             * never brings the soldier to a stop. Only a real fall counts; a
             * snap down a stair has no drop speed to speak of. */
            if (!was_on_floor && falling < 0.0) {
                const double hurt = (-falling) / T.landing_penalty_strength;
                penalty = std::max((double)T.landing_penalty_floor, 1.0 - hurt);
            }
            st->vel_up = 0.0;
            st->grounded = 1;
        } else if (pos.y > stand + 0.02) {
            st->grounded = 0;
        } else if (!walkable && pos.y <= stand) {
            // too steep to stand on: sit on it but slide down the face
            pos.y = stand;
            st->grounded = 0;
            const V3 slide = norm({nrm[0], 0.0, nrm[2]});
            pos = add(pos, mul(slide, 2.0 * dt));
            st->vel_up = 0.0;
        }
    }
    st->pos[0] = pos.x; st->pos[1] = pos.y; st->pos[2] = pos.z;
    st->vel[0] = vel.x; st->vel[2] = vel.z;
    /* vel[1] carries the landing penalty between steps. It was documented as
     * ignored and written as zero; it is now the one piece of walker state
     * that had nowhere else to live without changing the struct, which callers
     * compile against. */
    st->vel[1] = penalty;
}

void bf6_walk_step_tuned(bf6_walk_state* st, const bf6_walk_input* in,
                         const bf6_walk_tuning* tuning, bf6_walk_ray_fn ray, void* user)
{
    walk_step_impl(st, in, tuning, ray, user, false);
}

void bf6_walk_step(bf6_walk_state* st, const bf6_walk_input* in, bf6_walk_ray_fn ray, void* user)
{
    bf6_walk_step_tuned(st, in, nullptr, ray, user);
}

static int scene_ray(void* user, const double* from, const double* to, double* hit, double* normal)
{
    double out[7];
    if (bf6_ray_scene_trace(static_cast<bf6_ray_scene*>(user), from, to, out) < 0) return 0;
    hit[0] = out[0]; hit[1] = out[1]; hit[2] = out[2];
    normal[0] = out[3]; normal[1] = out[4]; normal[2] = out[5];
    return 1;
}

void bf6_walk_step_scene(bf6_walk_state* st, const bf6_walk_input* in, bf6_ray_scene* s)
{
    if (s) bf6_walk_step(st, in, scene_ray, s);
}

void bf6_walk_step_scene_tuned_eye(bf6_walk_state* st, const bf6_walk_input* in,
                                   const bf6_walk_tuning* tuning, bf6_ray_scene* s, int eye_final)
{
    if (s) walk_step_impl(st, in, tuning, scene_ray, s, eye_final != 0);
}

void bf6_walk_step_scene_tuned(bf6_walk_state* st, const bf6_walk_input* in,
                               const bf6_walk_tuning* tuning, bf6_ray_scene* s)
{
    if (s) bf6_walk_step_tuned(st, in, tuning, scene_ray, s);
}

} // extern "C"
