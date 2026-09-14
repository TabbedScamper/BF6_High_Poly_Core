// bf6_ray_scene and bf6_walk_step against geometry built here, no game install.
//
//   walk_mode_test
//
// The ray grid is checked against a brute-force loop over every triangle, for
// meshes under rotation, scale and mirroring. The walk is checked on a small
// course: a drop, a wall, a slide along it, a low and a high step, a jump,
// crouching, and a ramp too steep to stand on.

#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

static int failures = 0, checks = 0;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { ++failures; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

struct Geo { std::vector<float> v; std::vector<int32_t> i; };

// An axis-aligned box as 12 triangles.
static Geo box(float x0, float y0, float z0, float x1, float y1, float z1)
{
    Geo g;
    for (int k = 0; k < 8; ++k) { g.v.push_back(k & 1 ? x1 : x0); g.v.push_back(k & 2 ? y1 : y0); g.v.push_back(k & 4 ? z1 : z0); }
    const int f[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    for (auto& q : f) { g.i.insert(g.i.end(), {q[0], q[1], q[2], q[0], q[2], q[3]}); }
    return g;
}

static const double IDENT[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

static int32_t add(bf6_ray_scene* s, const Geo& g, const double* m = IDENT)
{
    const int32_t mesh = bf6_ray_scene_add_mesh(s, g.v.data(), int32_t(g.v.size() / 3), g.i.data(), int32_t(g.i.size()));
    return bf6_ray_scene_add_instance(s, mesh, m);
}

// brute force over world triangles
static bool brute(const std::vector<std::vector<double>>& tris, const double* o, const double* e, double& best_u)
{
    bool any = false;
    best_u = 1.0;
    const double d[3] = {e[0] - o[0], e[1] - o[1], e[2] - o[2]};
    for (const auto& t : tris) {
        const double e1[3] = {t[3] - t[0], t[4] - t[1], t[5] - t[2]}, e2[3] = {t[6] - t[0], t[7] - t[1], t[8] - t[2]};
        const double p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
        const double det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
        if (std::fabs(det) < 1e-14) continue;
        const double inv = 1.0 / det;
        const double tv[3] = {o[0] - t[0], o[1] - t[1], o[2] - t[2]};
        const double u = (tv[0] * p[0] + tv[1] * p[1] + tv[2] * p[2]) * inv;
        if (u < 0 || u > 1) continue;
        const double q[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0]};
        const double v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
        if (v < 0 || u + v > 1) continue;
        const double s = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
        if (s < 0 || s > best_u) continue;
        best_u = s;
        any = true;
    }
    return any;
}

static void grid_matches_brute_force()
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> uf(-20.0f, 20.0f);
    // a bumpy terrain-like sheet plus scattered boxes, under three transforms
    Geo sheet;
    const int n = 60;
    for (int z = 0; z <= n; ++z) for (int x = 0; x <= n; ++x) {
        sheet.v.push_back(-30.0f + x); sheet.v.push_back(std::sin(x * 0.3f) * std::cos(z * 0.2f) * 2.0f); sheet.v.push_back(-30.0f + z);
    }
    for (int z = 0; z < n; ++z) for (int x = 0; x < n; ++x) {
        const int a = z * (n + 1) + x, b = a + 1, c = a + n + 1, d = c + 1;
        sheet.i.insert(sheet.i.end(), {a, c, b, b, c, d});
    }
    const double c30 = std::cos(0.52), s30 = std::sin(0.52);
    const double transforms[3][12] = {
        {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0},
        {c30 * 1.5, 0, s30 * 1.5, 5, 0, 0.7, 0, -1, -s30 * 1.5, 0, c30 * 1.5, 3},
        {-1, 0, 0, 2, 0, 1, 0, 0, 0, 0, 1, -4},   // mirrored
    };
    for (int k = 0; k < 3; ++k) {
        bf6_ray_scene* s = bf6_ray_scene_create();
        std::vector<Geo> parts{sheet};
        for (int b = 0; b < 12; ++b) {
            const float x = uf(rng), z = uf(rng);
            parts.push_back(box(x, 0.0f, z, x + 1.5f, 3.0f, z + 1.0f));
        }
        std::vector<std::vector<double>> world;
        for (const Geo& g : parts) {
            add(s, g, transforms[k]);
            const double* m = transforms[k];
            for (size_t t = 0; t + 2 < g.i.size(); t += 3) {
                std::vector<double> tri;
                for (int c = 0; c < 3; ++c) {
                    const float* p = &g.v[g.i[t + c] * 3];
                    tri.push_back(m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3]);
                    tri.push_back(m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7]);
                    tri.push_back(m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]);
                }
                world.push_back(tri);
            }
        }
        int agree = 0, hits = 0, rays = 3000;
        double worst = 0;
        for (int r = 0; r < rays; ++r) {
            const double o[3] = {uf(rng) * 1.5, uf(rng) * 0.5 + 8.0, uf(rng) * 1.5};
            const double e[3] = {uf(rng) * 1.5, uf(rng) * 0.5 - 8.0, uf(rng) * 1.5};
            double out[7], bu;
            const bool g = bf6_ray_scene_trace(s, o, e, out) >= 0;
            const bool b = brute(world, o, e, bu);
            if (g == b && (!g || std::fabs(out[6] - bu) < 1e-5)) ++agree;
            if (g && b) worst = std::max(worst, std::fabs(out[6] - bu));
            hits += b;
        }
        CHECK(agree == rays, "transform %d: grid agrees with brute force on %d of %d rays (%d hit, worst %.2e)", k, agree, rays, hits, worst);
        CHECK(hits > rays / 2, "transform %d: the course is mostly hit (%d)", k, hits);
        bf6_ray_scene_free(s);
    }
}

// the course: floor y=0 over x -10..30, z -10..10
struct Course {
    bf6_ray_scene* s = bf6_ray_scene_create();
    Course()
    {
        add(s, box(-10, -1, -10, 30, 0, 10));          // floor
        add(s, box(5, 0, -10, 6, 4, 10));              // wall across x=5..6
        add(s, box(-8, 0, 2, -6, 0.3f, 4));            // low step 0.3 m
        add(s, box(-8, 0, -4, -6, 1.0f, -2));          // high step 1.0 m
    }
    ~Course() { bf6_ray_scene_free(s); }
};

static bf6_walk_state stand_at(double x, double y, double z)
{
    bf6_walk_state st{};
    st.pos[0] = x; st.pos[1] = y; st.pos[2] = z;
    st.eye = 1.72;
    return st;
}

static void run(bf6_ray_scene* s, bf6_walk_state& st, double wx, double wz, double seconds, int run_key = 0, int crouch = 0)
{
    bf6_walk_input in{};
    in.wish[0] = wx; in.wish[2] = wz; in.run = run_key; in.crouch = crouch; in.dt = 1.0 / 60.0;
    for (int f = 0; f < int(seconds * 60); ++f) bf6_walk_step_scene(&st, &in, s);
}

static void walk_course()
{
    Course c;
    // a drop from 5 m lands on the floor at eye height
    bf6_walk_state st = stand_at(0, 5, 0);
    run(c.s, st, 0, 0, 0.3);
    CHECK(st.pos[1] < 5.0 && !st.grounded, "falling: still in the air after 0.3 s (y %.3f)", st.pos[1]);
    run(c.s, st, 0, 0, 2.0);
    CHECK(st.grounded && std::fabs(st.pos[1] - 1.72) < 1e-6, "landing: grounded at eye height (y %.4f, grounded %d)", st.pos[1], st.grounded);

    // walking at 2.6 m/s reaches top speed
    run(c.s, st, 0, 1, 1.0);
    CHECK(std::fabs(std::hypot(st.vel[0], st.vel[2]) - 2.6) < 1e-6, "walking speed 2.6 (%.3f)", std::hypot(st.vel[0], st.vel[2]));
    run(c.s, st, 0, 0, 1.0);
    CHECK(std::hypot(st.vel[0], st.vel[2]) < 1e-6, "coasts to a stop");
    st = stand_at(0, 1.72, 0);
    st.grounded = 1;
    run(c.s, st, 0, 1, 0.5, 1);
    CHECK(std::fabs(std::hypot(st.vel[0], st.vel[2]) - 6.0) < 1e-6, "running speed 6.0 (%.3f)", std::hypot(st.vel[0], st.vel[2]));

    // into the wall at x=5: never through it
    st = stand_at(0, 1.72, 0);
    st.grounded = 1;
    run(c.s, st, 1, 0, 6.0);
    CHECK(st.pos[0] < 5.0 && st.pos[0] > 4.0, "the wall stops the walker in front of it (x %.3f)", st.pos[0]);
    // sliding along it: diagonal input keeps the along-wall part
    const double z0 = st.pos[2];
    run(c.s, st, 0.7071, 0.7071, 2.0);
    CHECK(st.pos[0] < 5.0 && st.pos[2] - z0 > 1.0, "slides along the wall (x %.3f, moved z %.3f)", st.pos[0], st.pos[2] - z0);

    // the low step is climbed, the high one blocks
    st = stand_at(-3, 1.72, 3);
    st.grounded = 1;
    run(c.s, st, -1, 0, 2.0);
    CHECK(std::fabs(st.pos[1] - (0.3 + 1.72)) < 1e-6 || st.pos[0] < -8.0, "climbs a 0.3 m step (y %.3f at x %.3f)", st.pos[1], st.pos[0]);
    st = stand_at(-3, 1.72, -3);
    st.grounded = 1;
    run(c.s, st, -1, 0, 2.0);
    CHECK(st.pos[0] > -6.0 && std::fabs(st.pos[1] - 1.72) < 1e-6, "a 1 m step blocks (x %.3f, y %.3f)", st.pos[0], st.pos[1]);

    // a jump rises and lands again
    st = stand_at(0, 1.72, 0);
    st.grounded = 1;
    bf6_walk_input jump{};
    jump.jump = 1; jump.dt = 1.0 / 60.0;
    bf6_walk_step_scene(&st, &jump, c.s);
    double peak = st.pos[1];
    jump.jump = 0;
    for (int f = 0; f < 120; ++f) { bf6_walk_step_scene(&st, &jump, c.s); peak = std::max(peak, st.pos[1]); }
    CHECK(peak > 1.72 + 0.8 && peak < 1.72 + 1.0 && st.grounded && std::fabs(st.pos[1] - 1.72) < 1e-6,
          "jump peaks near 0.9 m and lands (peak +%.3f)", peak - 1.72);

    // crouching lowers the eye to 58%
    run(c.s, st, 0, 0, 1.0, 0, 1);   // the eye settles under gravity, as in Unreal
    CHECK(std::fabs(st.pos[1] - 1.72 * 0.58) < 1e-6, "crouched eye at %.4f", st.pos[1]);
    run(c.s, st, 0, 0, 0.2, 0, 0);
    CHECK(std::fabs(st.pos[1] - 1.72) < 1e-6 && st.grounded, "stands back up on a closed box (y %.4f)", st.pos[1]);
}

// A map's terrain is an open sheet, not a box. Unreal probed from the standing
// eye while crouched - under the sheet - so standing up found no ground at all and
// crouching up a slope hit the terrain from beneath.
static void crouch_on_a_sheet()
{
    bf6_ray_scene* s = bf6_ray_scene_create();
    Geo sheet;
    // flat for x < 0, then a gentle 10 degree rise
    sheet.v = {-20, 0, -10, -20, 0, 10, 0, 0, 10, 0, 0, -10, 20, float(20 * std::tan(0.1745)), 10, 20, float(20 * std::tan(0.1745)), -10};
    sheet.i = {0, 1, 2, 0, 2, 3, 3, 2, 4, 3, 4, 5};
    add(s, sheet);
    bf6_walk_state st = stand_at(-5, 1.72, 0);
    st.grounded = 1;
    run(s, st, 0, 0, 1.0, 0, 1);
    CHECK(std::fabs(st.pos[1] - 1.72 * 0.58) < 1e-6, "crouched on a sheet (y %.4f)", st.pos[1]);
    run(s, st, 0, 0, 0.2, 0, 0);
    CHECK(std::fabs(st.pos[1] - 1.72) < 1e-6 && st.grounded, "stands back up on a sheet (y %.4f)", st.pos[1]);
    run(s, st, 0, 0, 1.0, 0, 1);
    run(s, st, 1, 0, 4.0, 0, 1);
    const double ground = st.pos[0] > 0 ? st.pos[0] * std::tan(0.1745) : 0.0;
    CHECK(st.pos[0] > 0.3 && std::fabs(st.pos[1] - (ground + 1.72 * 0.58)) < 0.05,
          "walks crouched up a gentle slope (x %.3f, eye above ground %.3f)", st.pos[0], st.pos[1] - ground);
    bf6_ray_scene_free(s);
}

static void steep_ramp()
{
    // a 60 degree face: standing on it slides down the slope
    bf6_ray_scene* s = bf6_ray_scene_create();
    add(s, box(-10, -1, -10, 30, 0, 10));
    const float h = 10.0f;
    Geo ramp;
    ramp.v = {0, 0, -5, 0, 0, 5, float(h / std::tan(1.047)), h, 5, float(h / std::tan(1.047)), h, -5};
    ramp.i = {0, 1, 2, 0, 2, 3};
    add(s, ramp);
    bf6_walk_state st = stand_at(4.0, 9.0, 0);   // feet just above the face (6.93 m there)
    run(s, st, 0, 0, 2.0);
    CHECK(st.pos[0] < 4.0, "slides down a slope too steep to stand on (x %.3f)", st.pos[0]);
    bf6_ray_scene_free(s);
}

static void api_edges()
{
    bf6_ray_scene* s = bf6_ray_scene_create();
    const float v[] = {0, 0, 0};
    const int32_t i[] = {0, 0};
    CHECK(bf6_ray_scene_add_mesh(s, v, 1, i, 2) == -1, "no triangles is refused");
    CHECK(bf6_ray_scene_add_instance(s, 3, IDENT) == -1, "an unknown mesh is refused");
    const double singular[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    Geo b = box(0, 0, 0, 1, 1, 1);
    const int32_t m = bf6_ray_scene_add_mesh(s, b.v.data(), 8, b.i.data(), int32_t(b.i.size()));
    CHECK(bf6_ray_scene_add_instance(s, m, singular) == -1, "a collapsed transform is refused");
    CHECK(bf6_ray_scene_add_instance(s, m, IDENT) == 0, "an instance is added");
    int32_t meshes = 0, instances = 0; int64_t tris = 0;
    bf6_ray_scene_counts(s, &meshes, &instances, &tris);
    CHECK(meshes == 1 && instances == 1 && tris == 12, "counts %d %d %lld", meshes, instances, (long long)tris);
    const double o[3] = {0.5, 5, 0.5}, e[3] = {0.5, -5, 0.5};
    double out[7];
    CHECK(bf6_ray_scene_trace(s, o, e, out) == 0 && std::fabs(out[1] - 1.0) < 1e-6 && out[4] > 0.99, "hits the top face facing up");
    const double o2[3] = {0.5, -5, 0.5}, e2[3] = {0.5, 5, 0.5};
    CHECK(bf6_ray_scene_trace(s, o2, e2, out) == 0 && std::fabs(out[1]) < 1e-6 && out[4] < -0.99, "from below, the bottom face facing down");
    bf6_ray_scene_clear_instances(s);
    CHECK(bf6_ray_scene_trace(s, o, e, out) == -1, "cleared instances hit nothing");
    bf6_ray_scene_free(s);
}

int main()
{
    grid_matches_brute_force();
    walk_course();
    steep_ramp();
    crouch_on_a_sheet();
    api_edges();
    std::printf("walk_mode_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
