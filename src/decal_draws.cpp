/* Road paint and ground decals as both engines draw them: the Unreal plugin's
 * BuildRoads and RoadMaterialFor, moved here so the Godot plugin classifies,
 * styles, orders and drapes every record identically. See
 * bf6_level_decal_draws in bf6_core.h. */
#include "bf6_core.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

struct bf6_decal_draws {
    std::vector<bf6_decal_draw> draws;
    std::vector<std::vector<float>> verts;
    bf6_decal_draw_stats stats{};
};

namespace {

bool has(const std::string& lower, const char* needle)
{
    return lower.find(needle) != std::string::npos;
}

bool is_marking(const std::string& n)
{
    return has(n, "airfield_text") || has(n, "airstrip") || has(n, "roadmark") ||
           has(n, "road_text") || has(n, "arrow") || has(n, "crosswalk");
}

bool is_wear(const std::string& n)
{
    return has(n, "wear") || has(n, "stain") || has(n, "scraped") || has(n, "crack");
}

template <class F>
void parallel_for(int count, F&& fn)
{
    unsigned hw = std::thread::hardware_concurrency();
    const int workers = std::clamp<int>(hw ? (int)hw : 4, 1, std::max(count, 1));
    std::atomic<int> next{0};
    auto run = [&]() {
        for (int i = next++; i < count; i = next++) fn(i);
    };
    std::vector<std::thread> pool;
    for (int w = 1; w < std::min(workers, 64); ++w) pool.emplace_back(run);
    run();
    for (auto& t : pool) t.join();
}

struct Result {
    std::vector<float> verts;
    int tris = 0;
    int painted = 0, colourless = 0, elevated_candidate = 0, pool_hole_tris = 0;
    int receiver_verts = 0, elevated = 0, band_only_rejected = 0;
};

} // namespace

extern "C" bf6_decal_draws* bf6_level_decal_draws(bf6_ctx* ctx, const char* level,
                                                  const bf6_terrain_mesh* ground,
                                                  const bf6_decal_drape_options* options)
{
    if (!ctx || !level || !ground) return nullptr;
    const int total = bf6_level_decals(ctx, level, nullptr, 0);
    auto* out = new bf6_decal_draws();
    if (total <= 0) return out;
    std::vector<bf6_decal> raw((size_t)total);
    const int got = bf6_level_decals(ctx, level, raw.data(), total);
    /* The Unreal plugin's list: rows with vertices, in reader order. Its index
     * is the draw index the sort key and the names use. */
    std::vector<bf6_decal> rows;
    std::vector<int> row_record;
    for (int i = 0; i < got; ++i) {
        if (!raw[(size_t)i].verts || raw[(size_t)i].vertex_count < 3) continue;
        rows.push_back(raw[(size_t)i]);
        row_record.push_back(i);
    }
    const double lift = options && options->lift_m > 0.f ? options->lift_m : 0.06;
    const bool uv_swapped = options && options->uv_swapped;
    const auto sample = options ? options->sample : nullptr;
    const auto touches_pool = options ? options->touches_pool : nullptr;
    void* user = options ? options->user : nullptr;

    std::vector<Result> results(rows.size());
    parallel_for((int)rows.size(), [&](int di) {
        Result& rs = results[(size_t)di];
        const bf6_decal& d = rows[(size_t)di];
        const int nv = d.vertex_count;
        if (nv < 3 || nv % 3 != 0) return;
        /* A record with no colour sheet and no colour constant is a puddle or
         * a modulator: it needs a material that writes only normal and
         * roughness, so it is not drawn. */
        if (d.albedo < 0) {
            if (d.has_tint || d.has_tint2) rs.painted++;
            else { rs.colourless++; return; }
        }
        double ground_hi = -1e30;
        for (int v = 0; v < nv; ++v)
            ground_hi = std::max(ground_hi, bf6_terrain_mesh_height_at(ground, d.verts[v * 8], d.verts[v * 8 + 1]));
        const bool elevated = (double)d.aabb_min[1] - ground_hi > 2.0;
        if (elevated) rs.elevated_candidate++;
        bool used_receiver = false;

        auto point = [&](const float* a, const float* b, const float* c, double u, double w, double* p) {
            for (int k = 0; k < 8; ++k) p[k] = a[k] * (1.0 - u - w) + b[k] * u + c[k] * w;
        };
        auto emit = [&](const double* pa, const double* pb, const double* pc) {
            const double cx = (pa[0] + pb[0] + pc[0]) / 3.0;
            const double cz = (pa[1] + pb[1] + pc[1]) / 3.0;
            if (sample) {
                double ry = 0.0;
                int hole = 0;
                const int at_centre = sample(user, cx, cz, d.aabb_min[1], d.aabb_max[1], &ry, &hole);
                if (!at_centre && hole) { rs.pool_hole_tris++; return; }
            }
            for (const double* p : {pa, pb, pc}) {
                const double gx = p[0], gz = p[1];
                double gy = 0.0;
                int hole = 0;
                if (sample && sample(user, gx, gz, d.aabb_min[1], d.aabb_max[1], &gy, &hole)) {
                    rs.receiver_verts++;
                    used_receiver = true;
                } else {
                    gy = bf6_terrain_mesh_height_at(ground, gx, gz);
                }
                float u = (float)p[2], w = (float)p[3];
                if (d.planar) {
                    const float t0 = std::fabs(d.tiling0) > 1e-3f ? d.tiling0 : 10.f;
                    const float t1 = std::fabs(d.tiling1) > 1e-3f ? d.tiling1 : t0;
                    u /= t1;
                    w /= t0;
                }
                const float uv[2] = {uv_swapped ? w : u, uv_swapped ? u : w};
                const float v9[9] = {(float)gx, (float)(gy + lift), (float)gz, uv[0], uv[1],
                                     (float)p[4], (float)p[5], (float)p[6], (float)p[7]};
                rs.verts.insert(rs.verts.end(), v9, v9 + 9);
            }
            rs.tris++;
        };
        /* Long triangles are split at 4 m (1 m where they touch a pool) and
         * every generated point is projected, so a single 128 m triangle
         * follows the ground instead of bridging it. */
        for (int t = 0; t + 2 < nv; t += 3) {
            const float* a = d.verts + t * 8;
            const float* b = d.verts + (t + 1) * 8;
            const float* c = d.verts + (t + 2) * 8;
            auto len = [](const float* p, const float* q) {
                const double dx = (double)p[0] - q[0], dz = (double)p[1] - q[1];
                return std::sqrt(dx * dx + dz * dz);
            };
            const double longest = std::max({len(a, b), len(b, c), len(c, a)});
            const double minx = std::min({(double)a[0], (double)b[0], (double)c[0]});
            const double maxx = std::max({(double)a[0], (double)b[0], (double)c[0]});
            const double minz = std::min({(double)a[1], (double)b[1], (double)c[1]});
            const double maxz = std::max({(double)a[1], (double)b[1], (double)c[1]});
            const bool pool = touches_pool && touches_pool(user, minx, minz, maxx, maxz);
            const double spacing = pool ? 1.0 : 4.0;
            const int n = std::clamp((int)std::ceil(longest / spacing), 1, pool ? 128 : 32);
            for (int iu = 0; iu < n; ++iu)
                for (int iw = 0; iw < n - iu; ++iw) {
                    double p00[8], p10[8], p01[8], p11[8];
                    point(a, b, c, (double)iu / n, (double)iw / n, p00);
                    point(a, b, c, (double)(iu + 1) / n, (double)iw / n, p10);
                    point(a, b, c, (double)iu / n, (double)(iw + 1) / n, p01);
                    emit(p00, p10, p01);
                    if (iu + iw < n - 1) {
                        point(a, b, c, (double)(iu + 1) / n, (double)(iw + 1) / n, p11);
                        emit(p10, p11, p01);
                    }
                }
        }
        if (elevated) {
            if (used_receiver) rs.elevated++;
            else rs.band_only_rejected++;
        }
    });

    bf6_decal_draw_stats& st = out->stats;
    st.records = (int32_t)rows.size();
    for (size_t di = 0; di < rows.size(); ++di) {
        const Result& rs = results[di];
        st.painted += rs.painted;
        st.colourless += rs.colourless;
        st.elevated_candidates += rs.elevated_candidate;
        st.pool_hole_tris += rs.pool_hole_tris;
        st.receiver_verts += rs.receiver_verts;
        st.elevated += rs.elevated;
        st.band_only_rejected += rs.band_only_rejected;
        if (rs.tris == 0) continue;
        const bf6_decal& d = rows[di];

        std::string opacity_name;
        if (d.opacity >= 0) {
            const char* nm = bf6_texture_name_at(ctx, d.opacity);
            opacity_name = nm ? nm : "";
            for (char& ch : opacity_name) ch = (char)std::tolower((unsigned char)ch);
        }
        bf6_decal_draw draw{};
        draw.record = row_record[di];
        draw.draw_index = (int32_t)di;
        draw.albedo = d.albedo;
        draw.normal = d.normal;
        draw.opacity = d.opacity;
        draw.marking = is_marking(opacity_name) ? 1 : 0;
        draw.wear = is_wear(opacity_name) ? 1 : 0;
        draw.mip_bias = draw.marking ? -4.f : 0.f;

        /* BaseColorTint. With a sheet bound the colour multiplies it and is
         * clamped to 1 (unclamped, decks saturate white); with no sheet it is
         * the colour itself. Outline and track-wear masks with a white first
         * endpoint take the second. */
        draw.tint[0] = draw.tint[1] = draw.tint[2] = 1.f;
        if (d.has_tint || d.has_tint2) {
            float c[3];
            for (int k = 0; k < 3; ++k) c[k] = d.has_tint ? d.tint[k] : d.tint2[k];
            if (d.albedo < 0 && d.has_tint2 && (has(opacity_name, "outline") || has(opacity_name, "trackwear")) &&
                (!d.has_tint || (d.tint[0] > 0.99f && d.tint[1] > 0.99f && d.tint[2] > 0.99f))) {
                for (int k = 0; k < 3; ++k) c[k] = d.tint2[k];
                st.second_colour++;
            }
            if (d.albedo >= 0) {
                bool clamped = false;
                for (int k = 0; k < 3; ++k) {
                    if (c[k] > 1.f) { c[k] = 1.f; }
                    if (c[k] < d.tint[k]) clamped = true;
                }
                if (clamped) st.tint_clamped++;
            }
            for (int k = 0; k < 3; ++k) draw.tint[k] = c[k];
        }

        /* Coverage scale: calibrations against matched frames, not game data. */
        float scale = 1.f;
        if (d.albedo >= 0 && d.opacity < 0) { scale = 0.18f; st.base_surface_blended++; }
        if (d.albedo < 0 && d.has_tint && d.tint[0] <= 0.01f && d.tint[1] <= 0.01f && d.tint[2] <= 0.01f &&
            has(opacity_name, "stain"))
            scale = 0.12f;
        if (has(opacity_name, "singleline")) { scale = 0.25f; st.surface_blended++; }
        if (draw.wear) { scale = has(opacity_name, "stain") ? 0.12f : 0.18f; st.wear_softened++; }
        draw.opacity_scale = scale;

        draw.mask_select[0] = 1.f;
        if (d.mask_channel >= 0 && d.mask_channel <= 3) {
            draw.mask_select[0] = 0.f;
            draw.mask_select[d.mask_channel] = 1.f;
        }

        /* Passes fill, detail, wear, marking; record order inside each. */
        draw.band = d.planar ? 0 : 2000;
        if (draw.wear) draw.band = 4000;
        if (draw.marking) { draw.band = 6000; st.markings++; }
        draw.sort_key = draw.band + (int32_t)di;

        out->verts.push_back(rs.verts);
        draw.vertex_count = (int32_t)(rs.verts.size() / 9);
        st.drawn++;
        st.triangles += rs.tris;
        out->draws.push_back(draw);
    }
    for (size_t i = 0; i < out->draws.size(); ++i) out->draws[i].verts = out->verts[i].data();
    return out;
}

extern "C" int bf6_decal_draws_count(const bf6_decal_draws* d)
{
    return d ? (int)d->draws.size() : 0;
}

extern "C" int bf6_decal_draws_get(const bf6_decal_draws* d, int index, bf6_decal_draw* out)
{
    if (!d || !out || index < 0 || index >= (int)d->draws.size()) return 0;
    *out = d->draws[(size_t)index];
    return 1;
}

extern "C" int bf6_decal_draws_stats(const bf6_decal_draws* d, bf6_decal_draw_stats* out)
{
    if (!d || !out) return 0;
    *out = d->stats;
    return 1;
}

extern "C" void bf6_decal_draws_free(bf6_decal_draws* d)
{
    delete d;
}
