// bf6_terrain_mesh_*: the shared adaptive terrain mesh.
//   terrain_mesh_test [<game_dir> <level>]
// Synthetic checks always run; with an install, the level's own heightfield.
//   * watertight: every edge is used by two triangles, except the map rim
//   * one winding throughout: the default is the Unreal plugin's order,
//     flip_winding the Godot plugin's (the order its old grid used)
//   * refinement follows the error ladder: a flat field stays at the base
//     spacing, a bumpy one refines
//   * height_at agrees with the vertices it lands on
#include "bf6_core.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>
#include <vector>

static int failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

struct Summary {
    long long tris = 0, verts = 0, open_edges = 0, rim_edges = 0, bad_edges = 0;
    long long up = 0, down = 0, flat = 0;  // flat: vertical slivers where a coarse cell meets finer edge points
    double height_err = 0.0;
};

static Summary walk(bf6_terrain_mesh* m, const bf6_terrain_mesh_info& info, double wmin[3], double wmax[3],
                    bool check_edges)
{
    Summary s;
    std::map<std::tuple<long long, long long, long long, long long>, int> edges;
    const int tiles = info.tiles_per_side * info.tiles_per_side;
    const double sx = (wmax[0] - wmin[0]) / (info.native_size - 1);
    const double sz = (wmax[2] - wmin[2]) / (info.native_size - 1);
    auto key = [&](const float* p) {
        return std::make_pair((long long)std::llround((p[0] - wmin[0]) / sx),
                              (long long)std::llround((p[2] - wmin[2]) / sz));
    };
    for (int t = 0; t < tiles; ++t) {
        bf6_terrain_tile tile{};
        if (!bf6_terrain_mesh_tile(m, t, &tile)) continue;
        s.verts += tile.vertex_count;
        s.tris += tile.index_count / 3;
        for (int i = 0; i + 2 < tile.index_count; i += 3) {
            const float* a = tile.positions + 3 * tile.indices[i];
            const float* b = tile.positions + 3 * tile.indices[i + 1];
            const float* c = tile.positions + 3 * tile.indices[i + 2];
            // Positive for the Unreal order, e.g. (0,0) (0,1) (1,1) in (x, z);
            // negative for Godot's (0,0) (1,0) (0,1).
            const double ny = (double)(b[2] - a[2]) * (c[0] - a[0]) - (double)(b[0] - a[0]) * (c[2] - a[2]);
            if (ny > 0) s.up++; else if (ny < 0) s.down++; else s.flat++;
            if (check_edges) {
                const float* v[3] = {a, b, c};
                for (int e = 0; e < 3; ++e) {
                    auto p = key(v[e]), q = key(v[(e + 1) % 3]);
                    if (q < p) std::swap(p, q);
                    edges[{p.first, p.second, q.first, q.second}]++;
                }
            }
        }
        for (int v = 0; v < tile.vertex_count; v += 97) {
            const float* p = tile.positions + 3 * v;
            s.height_err = std::max(s.height_err, std::fabs(bf6_terrain_mesh_height_at(m, p[0], p[2]) - p[1]));
        }
        bf6_terrain_mesh_release_tile(m, t);
    }
    const long long last = info.native_size - 1;
    for (const auto& kv : edges) {
        if (kv.second == 2) continue;
        const auto& k = kv.first;
        const bool rim = (std::get<0>(k) == std::get<2>(k) && (std::get<0>(k) == 0 || std::get<0>(k) == last)) ||
                         (std::get<1>(k) == std::get<3>(k) && (std::get<1>(k) == 0 || std::get<1>(k) == last));
        if (kv.second == 1 && rim) s.rim_edges++;
        else if (kv.second == 1) s.open_edges++;
        else s.bad_edges++;
    }
    return s;
}

static void synthetic(const char* name, int size, double amplitude, int flip, int expect_refined)
{
    std::printf("%s (%d samples, amplitude %.2f m)\n", name, size, amplitude);
    std::vector<uint16_t> h((size_t)size * size);
    for (int z = 0; z < size; ++z)
        for (int x = 0; x < size; ++x) {
            double y = 1000.0;
            if (amplitude > 0 && x > size / 3 && x < size / 2 && z > size / 4)
                y += amplitude * std::sin(x * 0.9) * std::cos(z * 0.7);
            h[(size_t)z * size + x] = (uint16_t)std::lround(y / 0.03);
        }
    double wmin[3] = {-512, 0, -512}, wmax[3] = {512, 2000, 512};
    const float scale = (float)(0.03 * 65536.0);
    bf6_terrain_mesh_options o{};
    o.base_side = 257;
    o.tile_quads = 32;
    o.flip_winding = flip;
    bf6_terrain_mesh* m = bf6_terrain_mesh_build(h.data(), size, wmin, wmax, scale, &o);
    bf6_terrain_mesh_info info{};
    check(m && bf6_terrain_mesh_describe(m, &info), "built");
    if (!m) return;
    std::printf("  step %d, %d tiles a side, adaptive %d, cells by level %lld/%lld/%lld/%lld\n",
                info.native_step, info.tiles_per_side, info.adaptive, info.cells_by_level[0],
                info.cells_by_level[1], info.cells_by_level[2], info.cells_by_level[3]);
    const Summary s = walk(m, info, wmin, wmax, true);
    std::printf("  %lld vertices, %lld triangles, up %lld down %lld, rim edges %lld, open %lld, over-shared %lld, height_at error %.4f m\n",
                s.verts, s.tris, s.up, s.down, s.rim_edges, s.open_edges, s.bad_edges, s.height_err);
    check(s.open_edges == 0 && s.bad_edges == 0, "watertight across cells and tiles");
    check(flip ? (s.up == 0 && s.down > 0) : (s.down == 0 && s.up > 0), "one winding, as requested");
    const long long refined = info.cells_by_level[1] + info.cells_by_level[2] + info.cells_by_level[3];
    check(expect_refined ? refined > 0 : refined == 0, expect_refined ? "bumps refine" : "flat stays at base spacing");
    check(s.height_err < 1e-3, "height_at matches the vertices");
    bf6_terrain_mesh_free(m);
}

int main(int argc, char** argv)
{
    synthetic("flat", 2049, 0.0, 0, 0);
    synthetic("bumpy, flipped", 2049, 3.0, 1, 1);
    if (argc >= 3) {
        char err[512] = {0};
        bf6_ctx* c = bf6_open(argv[1], err, sizeof(err));
        if (!c) { std::printf("open failed: %s\n", err); return 1; }
        if (bf6_open_level(c, argv[2], nullptr, 0, err, sizeof(err)) != 0) { std::printf("open_level: %s\n", err); return 1; }
        bf6_terrain* t = bf6_read_terrain(c, argv[2]);
        if (!t || t->width <= 1) { std::printf("no terrain\n"); return 1; }
        double wmin[3] = {t->world_min[0], t->world_min[1], t->world_min[2]};
        double wmax[3] = {t->world_max[0], t->world_max[1], t->world_max[2]};
        const auto t0 = std::chrono::steady_clock::now();
        bf6_terrain_mesh* m = bf6_terrain_mesh_build(t->heights, t->width, wmin, wmax, t->height_scale, nullptr);
        bf6_terrain_mesh_info info{};
        bf6_terrain_mesh_describe(m, &info);
        const double analyse = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%s: %d native, step %d, %d tiles a side, base %.2f m, finest %.2f m, analyse %.2f s\n",
                    argv[2], info.native_size, info.native_step, info.tiles_per_side, info.base_spacing_m,
                    info.finest_spacing_m, analyse);
        for (int l = 0; l <= info.max_level; ++l)
            std::printf("  %.2fm=%lld\n", info.base_spacing_m / (1 << l), info.cells_by_level[l]);
        const auto t1 = std::chrono::steady_clock::now();
        const Summary s = walk(m, info, wmin, wmax, false);
        std::printf("  %lld vertices, %lld triangles, %.1f s, height_at error %.4f m\n", s.verts, s.tris,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count(), s.height_err);
        check(s.tris > 0 && s.down == 0, "level mesh has the default winding throughout");
        bf6_terrain_mesh_free(m);
        bf6_free(c, t);
        bf6_close(c);
    }
    std::printf("terrain_mesh_test: %s (%d)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
