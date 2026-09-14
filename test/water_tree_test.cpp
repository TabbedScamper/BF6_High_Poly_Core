// bf6_water_draw_tree, no install needed. The controls the Unreal plugin ran
// on its own copy (RunWaterClipmapControl) plus coverage and budget checks.
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

static std::vector<bf6_water_tile> tree(const bf6_water_view& v, const double* b, double h,
                                        bf6_water_tree_stats* st)
{
    const int n = bf6_water_draw_tree(&v, b, h, nullptr, 0, nullptr);
    std::vector<bf6_water_tile> out(size_t(n > 0 ? n : 0));
    const int got = bf6_water_draw_tree(&v, b, h, out.data(), n, st);
    if (got != n) out.clear();
    return out;
}

int main()
{
    const double bounds[4] = {-2500.0, -2500.0, 2500.0, 2500.0};
    const double height = 0.0;
    bf6_water_view a{};
    a.camera[0] = 0.0; a.camera[1] = 0.0; a.camera[2] = 2.0;
    a.forward[0] = 1.0; a.forward[1] = 0.0;
    a.horizontal_fov_degrees = 90.f;
    a.viewport_width = 1920;
    a.off_view_max_depth = -1;
    bf6_water_tree_stats sa{};
    const auto base = tree(a, bounds, height, &sa);
    std::printf("base: %zu tiles, levels %d..%d, spacing %.3f..%.1f m, in view %d, coarse %d, refused %d\n",
                base.size(), sa.min_level, sa.max_level, sa.min_spacing_m, sa.max_spacing_m,
                sa.emitted_in_view, sa.emitted_coarse, sa.refused_for_budget);
    check(base.size() > 1 && sa.min_spacing_m <= 1.0, "tiled finely near the camera");
    check((int)base.size() <= 12288, "within the tile cap");

    // Coverage: tile areas clipped to the bounds sum to the bounds' area, and
    // no two tiles overlap (quadtree leaves), so there is no hole.
    double area = 0.0;
    for (const auto& t : base) {
        const double x0 = std::max(t.center[0] - t.width_m / 2, bounds[0]);
        const double x1 = std::min(t.center[0] + t.width_m / 2, bounds[2]);
        const double y0 = std::max(t.center[1] - t.width_m / 2, bounds[1]);
        const double y1 = std::min(t.center[1] + t.width_m / 2, bounds[3]);
        if (x1 > x0 && y1 > y0) area += (x1 - x0) * (y1 - y0);
    }
    check(std::fabs(area - 5000.0 * 5000.0) < 1.0, "leaves cover the whole surface");

    bf6_water_view b = a;
    b.camera[0] += 500.0;
    const auto moved = tree(b, bounds, height, nullptr);
    bool differ = moved.size() != base.size();
    for (size_t i = 0; !differ && i < base.size(); ++i)
        differ = base[i].center[0] != moved[i].center[0] || base[i].width_m != moved[i].width_m;
    check(!moved.empty() && differ, "moving the camera changes the tree");

    bf6_water_view far = a;
    far.camera[0] += 20000.0;
    const auto far_tiles = tree(far, bounds, height, nullptr);
    check(!far_tiles.empty() && far_tiles.size() < base.size(), "a far camera still sees the surface, coarsely");

    const auto again = tree(a, bounds, height, nullptr);
    bool same = again.size() == base.size();
    for (size_t i = 0; same && i < base.size(); ++i)
        same = base[i].center[0] == again[i].center[0] && base[i].center[1] == again[i].center[1] &&
               base[i].width_m == again[i].width_m;
    check(same, "deterministic");

    bf6_water_view small = a;
    small.tile_cap = 64;
    bf6_water_tree_stats ss{};
    const auto capped = tree(small, bounds, height, &ss);
    double carea = 0.0;
    for (const auto& t : capped) {
        const double x0 = std::max(t.center[0] - t.width_m / 2, bounds[0]);
        const double x1 = std::min(t.center[0] + t.width_m / 2, bounds[2]);
        const double y0 = std::max(t.center[1] - t.width_m / 2, bounds[1]);
        const double y1 = std::min(t.center[1] + t.width_m / 2, bounds[3]);
        if (x1 > x0 && y1 > y0) carea += (x1 - x0) * (y1 - y0);
    }
    check((int)capped.size() <= 64 && ss.refused_for_budget > 0 && std::fabs(carea - 25e6) < 1.0,
          "a small budget costs resolution, never coverage");

    std::printf("water_tree_test: %s (%d)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
