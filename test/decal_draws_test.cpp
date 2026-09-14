// bf6_level_decal_draws on an install.
//   decal_draws_test <game_dir> <level>
// Every draw sits on the terrain mesh (no receivers here) at the lift, sort
// keys are unique and banded, every style value is one of the documented
// ones, and a second build is identical.
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

static int failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

static unsigned long long digest(const bf6_decal_draws* d)
{
    unsigned long long h = 1469598103934665603ULL;
    auto mix = [&](const void* p, size_t n) {
        const unsigned char* b = (const unsigned char*)p;
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    };
    for (int i = 0; i < bf6_decal_draws_count(d); ++i) {
        bf6_decal_draw w{};
        bf6_decal_draws_get(d, i, &w);
        mix(&w.sort_key, sizeof(w.sort_key));
        mix(w.tint, sizeof(w.tint));
        mix(&w.opacity_scale, sizeof(w.opacity_scale));
        mix(w.verts, (size_t)w.vertex_count * 9 * sizeof(float));
    }
    return h;
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: decal_draws_test <game_dir> <level>\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, sizeof(err));
    if (!c || bf6_open_level(c, argv[2], nullptr, 0, err, sizeof(err)) != 0) { std::printf("open: %s\n", err); return 1; }
    bf6_terrain* t = bf6_read_terrain(c, argv[2]);
    if (!t) { std::printf("no terrain\n"); return 1; }
    const double wmin[3] = {t->world_min[0], t->world_min[1], t->world_min[2]};
    const double wmax[3] = {t->world_max[0], t->world_max[1], t->world_max[2]};
    bf6_terrain_mesh* m = bf6_terrain_mesh_build(t->heights, t->width, wmin, wmax, t->height_scale, nullptr);
    bf6_decal_draws* d = bf6_level_decal_draws(c, argv[2], m, nullptr);
    bf6_decal_draw_stats s{};
    bf6_decal_draws_stats(d, &s);
    std::printf("%s: %d records, %d drawn, %d triangles, %d painted from a constant, %d colourless skipped\n"
                "  tint clamped %d, second colour %d, markings %d, wear softened %d, base fills blended %d, asphalt ribbons %d\n"
                "  elevated candidates %d\n",
                argv[2], s.records, s.drawn, s.triangles, s.painted, s.colourless, s.tint_clamped, s.second_colour,
                s.markings, s.wear_softened, s.base_surface_blended, s.surface_blended, s.elevated_candidates);
    check(s.drawn > 0 && s.drawn + s.colourless <= s.records, "records drawn");
    std::set<int> keys;
    bool banded = true, styled = true, on_ground = true;
    double worst = 0.0;
    for (int i = 0; i < bf6_decal_draws_count(d); ++i) {
        bf6_decal_draw w{};
        bf6_decal_draws_get(d, i, &w);
        keys.insert(w.sort_key);
        banded = banded && (w.band == 0 || w.band == 2000 || w.band == 4000 || w.band == 6000) &&
                 w.sort_key == w.band + w.draw_index;
        const float sc = w.opacity_scale;
        styled = styled && (sc == 1.f || sc == 0.18f || sc == 0.12f || sc == 0.25f) &&
                 (w.mip_bias == (w.marking ? -4.f : 0.f)) && w.tint[0] <= (w.albedo >= 0 ? 1.f : 1e9f);
        for (int v = 0; v < w.vertex_count; v += 13) {
            const float* p = w.verts + v * 9;
            worst = std::max(worst, std::fabs(p[1] - 0.06 - bf6_terrain_mesh_height_at(m, p[0], p[2])));
        }
        on_ground = on_ground && w.vertex_count % 3 == 0;
    }
    std::printf("  worst distance from ground + lift: %.5f m\n", worst);
    check((int)keys.size() == bf6_decal_draws_count(d), "sort keys unique");
    check(banded, "bands and keys as documented");
    check(styled, "style values as documented");
    check(on_ground && worst < 1e-3, "draped on the terrain mesh at the lift");
    const unsigned long long h1 = digest(d);
    bf6_decal_draws* again = bf6_level_decal_draws(c, argv[2], m, nullptr);
    check(digest(again) == h1, "deterministic");
    bf6_decal_draws_free(again);
    bf6_decal_draws_free(d);
    bf6_terrain_mesh_free(m);
    bf6_free(c, t);
    bf6_close(c);
    std::printf("decal_draws_test: %s (%d)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
