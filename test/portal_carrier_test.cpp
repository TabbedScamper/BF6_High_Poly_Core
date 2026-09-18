/* WHICH CARRIER DOES PORTAL'S CUSTOM MODE STAND ON?
 *
 *   portal_carrier_test <game_dir> <level>
 *
 * The symptom: in Custom Portal the deck is outlined by combat volumes and
 * other markers, but the ship itself is invisible. So the MODE knows about the
 * carrier while the GEOMETRY does not get shown, which means the markers and
 * the hull are reached through differently named layers and our gate ties a
 * layer to its own name.
 *
 * This measures it instead of arguing about it: take every gameplay marker the
 * customportal mode owns, take the carrier hull placements from the walk, and
 * ask whether the markers sit on a hull. If they do, the layer that holds that
 * hull is the layer Custom Portal needs shown, whatever it happens to be called.
 *
 * Control: the same distance test against the OTHER hull. A marker set that is
 * near both is not evidence for either.
 */
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

struct Box {
    double cx = 0, cy = 0, cz = 0;
    size_t n = 0;
    void add(float x, float y, float z) { cx += x; cy += y; cz += z; n++; }
    void centre(double* o) const { o[0] = cx / n; o[1] = cy / n; o[2] = cz / n; }
};

double dist2d(const double* a, const double* b)
{
    const double dx = a[0] - b[0], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dz * dz);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: portal_carrier_test <game> <level>\n"); return 2; }
    const char* level = argv[2];
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }

    const int n = bf6_level_gamemodes(c, level, nullptr, 0, nullptr, err, (int)sizeof(err));
    if (n <= 0) { std::printf("gamemodes: %s\n", err); return 1; }
    std::vector<bf6_gm_entity> ents((size_t)n);
    bf6_gm_stats st{};
    const int got = bf6_level_gamemodes(c, level, ents.data(), n, &st, err, (int)sizeof(err));
    if (got <= 0) { std::printf("gamemodes fill: %s\n", err); return 1; }
    std::printf("%s: %d gameplay entit(ies)\n", level, got);

    /* Every mode's markers, and which LAYER each mode's markers come from. The
     * layer is the thing our gate keys on, so it is what has to be reported. */
    std::map<std::string, Box> by_mode;
    std::map<std::string, std::map<std::string, size_t>> layers_by_mode;
    for (int i = 0; i < got; ++i) {
        const bf6_gm_entity& e = ents[(size_t)i];
        const std::string mode = e.mode ? e.mode : "";
        by_mode[mode].add(e.xform[9], e.xform[10], e.xform[11]);
        layers_by_mode[mode][e.layer ? e.layer : ""]++;
    }

    /* The two NATO hull layouts, measured earlier from the walk. Stated here as
     * data rather than re-derived, and printed so a reader can check them. */
    const double nato_carrierstrike[3] = {-623, 119, 1153};
    const double nato_plain[3]         = {-147, 119, 715};

    std::printf("\n%-22s %7s  %-28s  %9s %9s\n", "mode", "markers",
                "centre (x,y,z)", "to CS hull", "to plain");
    for (const auto& kv : by_mode) {
        double ctr[3];
        kv.second.centre(ctr);
        std::printf("%-22s %7zu  %8.0f %8.0f %8.0f  %9.0f %9.0f\n",
                    kv.first.c_str(), kv.second.n, ctr[0], ctr[1], ctr[2],
                    dist2d(ctr, nato_carrierstrike), dist2d(ctr, nato_plain));
    }

    std::printf("\nlayers each mode's markers were reached from:\n");
    for (const auto& kv : layers_by_mode) {
        std::printf("  %s\n", kv.first.c_str());
        for (const auto& l : kv.second)
            std::printf("      %6zu  %s\n", l.second, l.first.c_str());
    }

    /* WHICH MODES STAND ON EACH HULL.
     *
     * A mode's CENTROID is useless for this: markers spread over a whole map
     * average out to the middle of it, which is why the table above puts every
     * mode roughly a kilometre from both ships. The honest measure is how many
     * of a mode's markers fall within a hull's own footprint. 150 m covers the
     * Carrier Strike hull's 208 x 69 m plan with room for the deck furniture. */
    struct Hull { const char* name; const double* c; };
    const double pax_carrierstrike[3] = {-1346, 120, -565};
    const double pax_plain[3]         = {-1340, 119, -682};
    const Hull hulls[] = {
        {"NATO Carrier Strike", nato_carrierstrike},
        {"NATO plain",          nato_plain},
        {"PAX Carrier Strike",  pax_carrierstrike},
        {"PAX plain",           pax_plain},
    };
    std::printf("\nmarkers standing ON each hull (within 150 m of its centre):\n");
    for (const Hull& h : hulls) {
        std::map<std::string, size_t> hits;
        std::map<std::string, size_t> layers;
        for (int i = 0; i < got; ++i) {
            const bf6_gm_entity& e = ents[(size_t)i];
            const double p[3] = {e.xform[9], e.xform[10], e.xform[11]};
            if (dist2d(p, h.c) >= 150.0) continue;
            hits[e.mode ? e.mode : ""]++;
            layers[e.layer ? e.layer : ""]++;
        }
        size_t total = 0;
        for (const auto& kv : hits) total += kv.second;
        std::printf("  %-22s %4zu marker(s)", h.name, total);
        if (hits.empty()) { std::printf("  - nothing stands here\n"); continue; }
        std::printf("  modes:");
        for (const auto& kv : hits) std::printf(" %s(%zu)", kv.first.c_str(), kv.second);
        std::printf("\n");
        for (const auto& kv : layers)
            std::printf("        %5zu  %s\n", kv.second, kv.first.c_str());
    }

    bf6_close(c);
    return 0;
}
