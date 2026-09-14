/* The water draw tree: which square tiles of the shared 16x16-quad patch to
 * draw this frame. Moved from the Unreal plugin (BuildWaterDrawTree) so every
 * engine tiles the sea the same way; see bf6_water_draw_tree in bf6_core.h. */
#include "bf6_core.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <vector>

namespace {

constexpr int    kMaxDepth = 12;
constexpr double kRootWidthM = 65536.0;
constexpr double kProjectorFarM = 5000.0;

struct Node {
    double cx, cy, half;
    uint64_t code;
    int level;
    double priority;
};

/* Largest projected error first; equal errors in code order, so the result
 * does not depend on the heap implementation. */
struct WorseFirst {
    bool operator()(const Node& a, const Node& b) const
    {
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.code > b.code;
    }
};

double distance_to_tile(const bf6_water_view& v, double minx, double miny,
                        double maxx, double maxy, double height)
{
    const double dx = std::max({minx - v.camera[0], 0.0, v.camera[0] - maxx});
    const double dy = std::max({miny - v.camera[1], 0.0, v.camera[1] - maxy});
    const double dz = std::fabs(v.camera[2] - height);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double half_fov(const bf6_water_view& v)
{
    const double deg = std::clamp((double)v.horizontal_fov_degrees, 10.0, 170.0);
    return deg * 0.5 * 3.14159265358979323846 / 180.0;
}

bool in_horizontal_view(const bf6_water_view& v, double cx, double cy, double half)
{
    double fx = v.forward[0], fy = v.forward[1];
    const double len = std::sqrt(fx * fx + fy * fy);
    if (!(len > 1e-8)) return true;
    fx /= len; fy /= len;
    const double tx = cx - v.camera[0], ty = cy - v.camera[1];
    const double along = tx * fx + ty * fy;
    const double side = std::fabs(fx * ty - fy * tx);
    const double radius = half * std::sqrt(2.0);
    if (along + radius < 0.0) return false;
    const double hf = half_fov(v);
    const double cos_half = std::max(std::cos(hf), 1e-3);
    return side <= std::max(along, 0.0) * std::tan(hf) + radius / cos_half;
}

} // namespace

extern "C" int bf6_water_draw_tree(const bf6_water_view* view, const double* bounds,
                                   double height_m, bf6_water_tile* out, int out_max,
                                   bf6_water_tree_stats* stats_out)
{
    bf6_water_tree_stats st{};
    if (!view || !bounds) { if (stats_out) *stats_out = st; return 0; }
    const bf6_water_view& v = *view;
    const int quads = std::max(v.patch_quads > 0 ? v.patch_quads : 16, 1);
    const int cap = v.tile_cap > 0 ? v.tile_cap : 12288;
    const int off_depth = std::clamp(v.off_view_max_depth >= 0 ? v.off_view_max_depth : 6, 0, kMaxDepth);
    const double tolerance = 1.0 / quads;
    const double bminx = bounds[0], bminy = bounds[1], bmaxx = bounds[2], bmaxy = bounds[3];
    const double rootx = (bminx + bmaxx) * 0.5, rooty = (bminy + bmaxy) * 0.5;
    const double root_half = kRootWidthM * 0.5;
    const int width_px = std::max(v.viewport_width, 1);
    const double world_per_pixel = 2.0 * std::tan(half_fov(v)) / width_px;

    auto priority_of = [&](double cx, double cy, double half) {
        const double d = std::max(distance_to_tile(v, cx - half, cy - half, cx + half, cy + half, height_m), 1.0);
        const double error = half * 2.0 * tolerance / d;
        return in_horizontal_view(v, cx, cy, half) ? error : error * 0.1;
    };

    std::priority_queue<Node, std::vector<Node>, WorseFirst> pending;
    for (uint64_t q = 0; q < 4; ++q) {
        const double h = root_half * 0.5;
        const double cx = rootx + ((q & 1) ? h : -h), cy = rooty + ((q & 2) ? h : -h);
        pending.push({cx, cy, h, 4 | q, 1, priority_of(cx, cy, h)});
    }

    int emitted = 0;
    st.min_level = kMaxDepth + 1;
    st.min_spacing_m = 1e300;
    while (!pending.empty()) {
        const Node n = pending.top();
        pending.pop();
        const double minx = n.cx - n.half, miny = n.cy - n.half;
        const double maxx = n.cx + n.half, maxy = n.cy + n.half;
        /* FBox2D::Intersect: touching edges count as intersecting. */
        if (minx > bmaxx || bminx > maxx || miny > bmaxy || bminy > maxy) {
            ++st.rejected_outside_bounds;
            continue;
        }
        const bool in_view = in_horizontal_view(v, n.cx, n.cy, n.half);
        const double d = distance_to_tile(v, minx, miny, maxx, maxy, height_m);
        if (d > kProjectorFarM) ++st.beyond_projector;
        const double size = n.half * 2.0;
        const double spacing = size * tolerance;
        const bool small_enough = spacing < world_per_pixel * std::max(d, 0.001);
        const int max_here = in_view ? kMaxDepth : off_depth;
        const bool wants = n.level < max_here && !small_enough;
        const bool afford = emitted + (int)pending.size() + 3 <= cap;
        if (wants && !afford) ++st.refused_for_budget;
        if (wants && afford) {
            const double ch = n.half * 0.5;
            for (uint64_t q = 0; q < 4; ++q) {
                const double cx = n.cx + ((q & 1) ? ch : -ch), cy = n.cy + ((q & 2) ? ch : -ch);
                pending.push({cx, cy, ch, n.code * 4 | q, n.level + 1, priority_of(cx, cy, ch)});
            }
            continue;
        }
        if (emitted >= cap) st.hit_tile_cap = 1;
        if (in_view) ++st.emitted_in_view; else ++st.emitted_coarse;
        if (out && emitted < out_max) {
            bf6_water_tile& t = out[emitted];
            t.center[0] = n.cx;
            t.center[1] = n.cy;
            t.width_m = size;
            t.level = n.level;
            t.in_view = in_view ? 1 : 0;
        }
        ++emitted;
        st.min_level = std::min(st.min_level, n.level);
        st.max_level = std::max(st.max_level, n.level);
        st.min_spacing_m = std::min(st.min_spacing_m, spacing);
        st.max_spacing_m = std::max(st.max_spacing_m, spacing);
        st.max_tile_m = std::max(st.max_tile_m, size);
    }
    if (st.min_level > kMaxDepth) st.min_level = 0;
    if (st.min_spacing_m == 1e300) st.min_spacing_m = 0.0;
    if (stats_out) *stats_out = st;
    return emitted;
}
