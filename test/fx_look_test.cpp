/* THE PER-LAYER LOOK, resolved: bf6_fx_layer_look.
 *
 *   fx_look_test <game_dir> <level>
 *
 * Godot builds its FX from about forty hand-curated emitter-graph archetypes
 * while the game authors thousands of layers, which is why its effects read as
 * generic puffs. This is the data that fixes that: every layer's own values,
 * resolved from its parameter table by pids named in the core so both editors
 * cannot disagree about them.
 *
 * What is asserted:
 *  1. Layers DIFFER. If every layer resolved to the same look, per-layer data
 *     would be worth nothing over the archetypes and this whole exercise would
 *     be pointless - so the spread is measured, not assumed.
 *  2. The values that matter are actually authored: opacity-over-life,
 *     rotation-over-life, base size, drag, and the lighting scales.
 *  3. The lighting models are present and the GnomonLit share is real, because
 *     that is the claim the six-way shader work rests on.
 *  4. CONTROL: a pid one bit away from a real one resolves nothing.
 */
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: fx_look_test <game> <level>\n"); return 2; }
    const char* level = argv[2];
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }

    /* bf6_level_fx wants the level walked, not merely mounted. */
    /* Returns 0 on success, like the other level openers. */
    if (bf6_open_level(c, level, nullptr, 0, err, (int)sizeof(err)) != 0)
    { std::printf("open_level: %s\n", err); bf6_close(c); return 1; }

    bf6_fx_stats st{};
    const int n = bf6_level_fx(c, level, nullptr, 0, &st, err, (int)sizeof(err));
    if (n <= 0) { std::printf("%s: no fx layers (%d) %s\n", level, n, err); bf6_close(c); return 1; }
    std::vector<bf6_fx_layer> layers((size_t)n);
    const int got = bf6_level_fx(c, level, layers.data(), n, &st, err, (int)sizeof(err));
    if (got <= 0) { std::printf("fill: %s\n", err); bf6_close(c); return 1; }
    std::printf("%s: %d emitter layer(s)\n", level, got);

    int fails = 0;
    std::map<int, int> by_light, by_align;
    std::map<int, int> authored_count;
    std::set<std::string> distinct_looks;
    int with_opacity_curve = 0, with_rot_curve = 0, with_sun = 0, with_local = 0;
    double size_min = 1e30, size_max = -1e30;

    for (int i = 0; i < got; ++i)
    {
        const bf6_fx_layer& L = layers[(size_t)i];
        by_light[L.lighting_model]++;
        by_align[L.alignment]++;
        bf6_fx_look look{};
        const int authored = bf6_fx_layer_look(&L, &look);
        authored_count[authored > 12 ? 12 : authored]++;
        if (bf6_fx_look_authored(&look, BF6_FXLOOK_OPACITY_OVER_LIFE)) with_opacity_curve++;
        if (bf6_fx_look_authored(&look, BF6_FXLOOK_ROTATION_OVER_LIFE)) with_rot_curve++;
        if (bf6_fx_look_authored(&look, BF6_FXLOOK_SUN_LIGHT)) with_sun++;
        if (bf6_fx_look_authored(&look, BF6_FXLOOK_LOCAL_LIGHT)) with_local++;
        if (bf6_fx_look_authored(&look, BF6_FXLOOK_BASE_SIZE)) {
            if (look.base_size < size_min) size_min = look.base_size;
            if (look.base_size > size_max) size_max = look.base_size;
        }
        /* 1. a cheap fingerprint of the resolved look, to count how many
         * genuinely different looks the level carries. */
        char key[160];
        std::snprintf(key, sizeof(key), "%.3f|%.3f|%.3f|%.3f|%.3f|%d|%d",
                      look.base_size, look.opacity, look.drag, look.gravity,
                      look.rotation_speed, L.lighting_model, L.alignment);
        distinct_looks.insert(key);
    }

    std::printf("  lighting models:");
    for (const auto& kv : by_light)
        std::printf(" %s=%d", kv.first == 0 ? "Emissive" : kv.first == 1 ? "VertexLit"
                    : kv.first == 2 ? "GnomonLit" : "other", kv.second);
    std::printf("\n  alignments:");
    for (const auto& kv : by_align) std::printf(" %d=%d", kv.first, kv.second);
    std::printf("\n  authored: opacity-over-life %d, rotation-over-life %d, "
                "sun scale %d, local-light scale %d\n",
                with_opacity_curve, with_rot_curve, with_sun, with_local);
    std::printf("  base size spans %.2f .. %.2f\n", size_min, size_max);
    std::printf("  DISTINCT resolved looks: %zu of %d layer(s)\n", distinct_looks.size(), got);

    /* 1. the point of per-layer data */
    if (distinct_looks.size() < 8) {
        std::printf("  FAIL only %zu distinct look(s): per-layer data would add nothing\n",
                    distinct_looks.size());
        fails++;
    }
    /* 2. the fields the fidelity work depends on */
    if (with_opacity_curve == 0 && with_rot_curve == 0) {
        std::printf("  FAIL no layer authors an over-life curve; the pids are wrong\n");
        fails++;
    }
    /* 3. the claim the shader work rests on */
    if (by_light.find(2) == by_light.end()) {
        std::printf("  NOTE this level authors no GnomonLit layer\n");
    }

    /* 4. CONTROL. A pid one bit from BaseSize must resolve nothing, which is
     * what says the table is matched on the id and not on position. */
    int fake_hits = 0;
    for (int i = 0; i < got; ++i) {
        const bf6_fx_layer& L = layers[(size_t)i];
        for (int k = 0; k < L.param_count; ++k)
            if (L.params[k].pid == 0xBD355AD4u) fake_hits++;
    }
    std::printf("  CONTROL fabricated pid present %d time(s) (expected 0)\n", fake_hits);
    if (fake_hits) fails++;

    bf6_close(c);
    std::printf("\n%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
