/* EVERY PARAMETER ON EVERY LAYER, raw.
 *
 *   fx_param_dump <game_dir> <level>
 *
 * For finding the pid behind a value we know exists but have not named. The
 * layer's `particle_life` comes from the emitter's spawn config and is the
 * TEMPLATE's number - 546 of 665 layers on MP_Subsurface report the same 5.0 -
 * while the authored per-layer lifetime plainly varies. That override is in the
 * parameter table under a pid, and this is how to find which.
 *
 * TSV: one row per (layer, parameter), so the analysis can be done against the
 * named property table rather than by guessing at hashes.
 */
#include "bf6_core.h"

#include <cstdio>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: fx_param_dump <game> <level>\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    if (bf6_open_level(c, argv[2], nullptr, 0, err, (int)sizeof(err)) != 0)
    { std::printf("open_level: %s\n", err); bf6_close(c); return 1; }

    bf6_fx_stats st{};
    const int n = bf6_level_fx(c, argv[2], nullptr, 0, &st, err, (int)sizeof(err));
    if (n <= 0) { std::printf("no layers\n"); bf6_close(c); return 1; }
    std::vector<bf6_fx_layer> rows((size_t)n);
    const int got = bf6_level_fx(c, argv[2], rows.data(), n, &st, err, (int)sizeof(err));

    std::printf("effect\tlayer\tgraph\tparticle_life\tpid\ttype\tv0\tv1\tv2\tv3\tint\n");
    for (int i = 0; i < got; ++i)
    {
        const bf6_fx_layer& L = rows[(size_t)i];
        for (int k = 0; k < L.param_count; ++k)
        {
            const bf6_fx_param& p = L.params[k];
            std::printf("%s\t%d\t%s\t%g\t0x%08X\t%d\t%g\t%g\t%g\t%g\t%d\n",
                        L.effect ? L.effect : "", L.layer,
                        L.graph ? L.graph : "", L.particle_life,
                        p.pid, p.type, p.v[0], p.v[1], p.v[2], p.v[3], p.ivalue);
        }
    }
    bf6_close(c);
    return 0;
}
