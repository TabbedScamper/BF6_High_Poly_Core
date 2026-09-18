/* WHAT KEY JOINS A SPAWN POINT TO ITS LAYERS?
 *
 *   fx_join_probe <game_dir> <level>
 *
 * The Godot overlay knows a spawn point's effect blueprint and its emitter
 * graph; the core knows every LAYER, each carrying its own look. The rewrite
 * puts one emitter in the scene per spawn-point x layer, which only works if
 * the layer rows can be found from what the map walk records. So dump the keys
 * and let the join be measured against the real fx.json rather than assumed:
 * getting this wrong would silently fall back to one generic layer per point,
 * which is exactly the failure the rewrite exists to end.
 *
 * Output is one TSV line per layer so the coverage check can be done against
 * the map-context file the overlay actually reads.
 */
#include "bf6_core.h"

#include <cstdio>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: fx_join_probe <game> <level>\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    if (bf6_open_level(c, argv[2], nullptr, 0, err, (int)sizeof(err)) != 0)
    { std::printf("open_level: %s\n", err); bf6_close(c); return 1; }

    bf6_fx_stats st{};
    const int n = bf6_level_fx(c, argv[2], nullptr, 0, &st, err, (int)sizeof(err));
    if (n <= 0) { std::printf("no layers (%d) %s\n", n, err); bf6_close(c); return 1; }
    std::vector<bf6_fx_layer> rows((size_t)n);
    const int got = bf6_level_fx(c, argv[2], rows.data(), n, &st, err, (int)sizeof(err));

    std::printf("effect\tgraph\tfamily\tlayer\tplacements\tlight\talign\tatlas"
                "\topacity_over_life\trotation_over_life\tsize_over_life"
                "\tbase_size\topacity\tlife\thas_rate\trate\tpmax\tmode\tpreroll\n");
    for (int i = 0; i < got; ++i)
    {
        const bf6_fx_layer& L = rows[(size_t)i];
        bf6_fx_look k{};
        bf6_fx_layer_look(&L, &k);
        /* The over-life fields come back as four floats and the ENCODING is
         * not settled, so print them raw: a curve, a pair of ranges and a
         * min/max look different in the values themselves. */
        std::printf("%s\t%s\t%s\t%d\t%d\t%d\t%d\t%s",
                    L.effect ? L.effect : "", L.graph ? L.graph : "",
                    L.family ? L.family : "", L.layer, L.placements,
                    L.lighting_model, L.alignment, L.atlas ? L.atlas : "");
        const float* cur[3] = {k.opacity_over_life, k.rotation_over_life,
                               k.size_over_life};
        const int bit[3] = {BF6_FXLOOK_OPACITY_OVER_LIFE,
                            BF6_FXLOOK_ROTATION_OVER_LIFE,
                            BF6_FXLOOK_SIZE_OVER_LIFE};
        for (int c = 0; c < 3; ++c)
        {
            if (bf6_fx_look_authored(&k, bit[c]))
                std::printf("\t%g,%g,%g,%g", cur[c][0], cur[c][1], cur[c][2], cur[c][3]);
            else
                std::printf("\t-");
        }
        std::printf("\t%g\t%g\t%g\t%d\t%g\t%d\t%s\t%g\n", k.base_size, k.opacity,
                    L.particle_life, L.has_spawn_rate, L.spawn_rate, L.particle_max,
                    L.spawn_mode ? L.spawn_mode : "", L.preroll_time);
    }
    bf6_close(c);
    return 0;
}
