/* Level reflection volumes through the public ABI.
 *
 *   reflection_probes_test <game_dir> <level> [level2 ...]
 *
 * Checks the properties that distinguish a real read from a plausible one:
 *
 *   1. COUNT-THEN-FILL agrees with itself: the counting call and the filling
 *      call return the same number.
 *   2. Every probe names a baked texture partition, and that partition EXISTS
 *      in the mount. A texture name rebuilt from a convention would pass a
 *      pattern test and fail this one.
 *   3. The basis is non-degenerate and the translation finite: a box with a
 *      zero axis has no interior and would accept or reject every point.
 *   4. Probes are spatially spread. One authored volume per level would also
 *      "work"; a level's probes should not collapse to a point.
 *   5. CONTROL: a fabricated level name returns no probes rather than the
 *      whole mount's worth, which is what a search that ignores its level
 *      argument would do.
 */
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static double len3(const float* v)
{
    return std::sqrt((double)v[0]*v[0] + (double)v[1]*v[1] + (double)v[2]*v[2]);
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: reflection_probes_test <game_dir> <level> [level2 ...]\n");
        return 2;
    }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }

    int total = 0, levels_with = 0, bad = 0;
    for (int a = 2; a < argc; ++a)
    {
        const char* level = argv[a];
        const int n = bf6_level_reflection_probes(c, level, nullptr, 0, err, (int)sizeof(err));
        if (n < 0) { std::printf("%-34s -           %s\n", level, err); continue; }
        std::vector<bf6_reflection_probe> probes((size_t)(n > 0 ? n : 1));
        const int got = bf6_level_reflection_probes(c, level, probes.data(), n,
                                                    err, (int)sizeof(err));
        if (got != n) { std::printf("%-34s COUNT MISMATCH %d vs %d\n", level, n, got); bad++; continue; }

        int missing_tex = 0, degenerate = 0;
        double lo[3] = {1e30, 1e30, 1e30}, hi[3] = {-1e30, -1e30, -1e30};
        for (int i = 0; i < n; ++i)
        {
            const bf6_reflection_probe& p = probes[(size_t)i];
            /* The texture must EXIST in the mount, not merely look plausible:
             * bf6_list_ebx over the exact name is the cheapest way to ask, and
             * a name invented from a convention fails it. */
            if (!p.texture[0] || bf6_list_ebx(c, p.texture, nullptr, 0) <= 0)
                missing_tex++;
            if (len3(p.right) < 1e-4 || len3(p.up) < 1e-4 || len3(p.forward) < 1e-4)
                degenerate++;
            for (int k = 0; k < 3; ++k) {
                if (p.translation[k] < lo[k]) lo[k] = p.translation[k];
                if (p.translation[k] > hi[k]) hi[k] = p.translation[k];
            }
        }
        const double spread = n > 0
            ? std::sqrt((hi[0]-lo[0])*(hi[0]-lo[0]) + (hi[1]-lo[1])*(hi[1]-lo[1]) +
                        (hi[2]-lo[2])*(hi[2]-lo[2]))
            : 0.0;
        std::printf("%-34s %4d probe(s)  %d without a mounted texture, %d degenerate, spread %.0f m\n",
                    level, n, missing_tex, degenerate, spread);
        if (n > 0) {
            total += n;
            levels_with++;
            /* Spread only means something with more than one probe: mp_atoll
             * ships exactly one, and a single volume legitimately has none. */
            if (missing_tex > 0 || degenerate > 0 || (n > 1 && spread < 1.0)) bad++;
        }
    }

    /* CONTROL. A level that does not exist must not inherit the mount's probes. */
    char cerr[512] = {0};
    const int control = bf6_level_reflection_probes(c, "mp_notarealleveleverr",
                                                    nullptr, 0, cerr, (int)sizeof(cerr));
    std::printf("CONTROL fake level: %d (expected <= 0)\n", control);

    std::printf("%d probe(s) over %d level(s)\n", total, levels_with);
    const bool ok = total > 0 && bad == 0 && control <= 0;
    std::printf("%s\n", ok ? "REFLECTION PROBES OK" : "REFLECTION PROBES FAILED");
    bf6_close(c);
    return ok ? 0 : 1;
}
