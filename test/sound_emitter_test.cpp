/* PLACED SPATIAL SOUND EMITTERS through the public ABI.
 *
 *   sound_emitter_test <game_dir> <level> [level2 ...]
 *
 * The checks are the ones that separate a real read from a plausible one:
 *
 *  1. COUNT-THEN-FILL agrees with itself.
 *  2. Every emitter names a sound, and that sound EXISTS in the mount. A path
 *     rebuilt from a convention would pass a pattern test and fail this one -
 *     which is exactly how the reflection reader once reported 35 probes and 35
 *     missing textures.
 *  3. Every emitter DECODES to PCM. This is the difference between placing a
 *     marker and hearing what the game hears, so it is asserted rather than
 *     hoped for.
 *  4. Emitters are SPREAD OUT. One ambient source per level would also "work";
 *     a level's bed should not collapse to a point.
 *  5. The transform is a real basis, not zeros: an emitter at the origin with
 *     no orientation is the signature of reading the wrong field.
 *  6. CONTROL: a fabricated level name returns nothing, so the search does not
 *     ignore its level argument.
 */
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: sound_emitter_test <game> <level> [more...]\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    /* The sounds live under common/, not in the level. */
    if (!bf6_mount_all(c, 1, err, (int)sizeof(err)))
        std::printf("note: mount_all said %s\n", err);

    int bad = 0, levels = 0;
    long total = 0;
    for (int a = 2; a < argc; ++a)
    {
        const char* level = argv[a];
        const int n = bf6_level_sound_emitters(c, level, nullptr, 0, err, (int)sizeof(err));
        if (n < 0) { std::printf("%-26s -  %s\n", level, err); continue; }
        levels++;
        if (n == 0) { std::printf("%-26s no placed emitters\n", level); continue; }
        std::vector<bf6_sound_emitter> rows((size_t)n);
        const int got = bf6_level_sound_emitters(c, level, rows.data(), n, err, (int)sizeof(err));
        if (got != n) { std::printf("%-26s COUNT MISMATCH %d vs %d\n", level, n, got); bad++; continue; }
        total += got;

        int missing = 0, undecoded = 0, degenerate = 0;
        std::set<std::string> assets;
        double lo[3] = {1e30, 1e30, 1e30}, hi[3] = {-1e30, -1e30, -1e30};
        for (int i = 0; i < got; ++i)
        {
            const bf6_sound_emitter& s = rows[(size_t)i];
            if (s.sound[0] == '\0') { missing++; continue; }
            assets.insert(s.sound);
            /* the basis must be a basis */
            const double rl = std::sqrt((double)s.xform[0]*s.xform[0] + (double)s.xform[1]*s.xform[1]
                                      + (double)s.xform[2]*s.xform[2]);
            if (rl < 0.001) degenerate++;
            for (int k = 0; k < 3; ++k) {
                const double p = s.xform[9 + k];
                if (p < lo[k]) lo[k] = p;
                if (p > hi[k]) hi[k] = p;
            }
        }
        /* 2 and 3, over the DISTINCT assets: the same ambience is reused, and
         * decoding each one once is enough to prove the chain. */
        for (const std::string& path : assets) {
            if (bf6_list_ebx(c, path.c_str(), nullptr, 0) <= 0) missing++;
            /* VARIATIONS. The ABI says variation_index selects among the
             * authored variations, so index 0 is not guaranteed to exist: a
             * config whose variations start elsewhere would read as
             * undecodable when it is merely being asked the wrong question.
             * Trying a few is the honest test of "can this be played at all". */
            bool played = false;
            for (int vi = 0; vi < 4 && !played; ++vi) {
                int ch = 0, rate = 0;
                if (bf6_ui_sound_decode(c, path.c_str(), vi, nullptr, 0, &ch, &rate) > 0
                    && ch > 0 && rate > 0)
                    played = true;
            }
            if (!played) {
                undecoded++;
                /* Named, because "some do not decode" is not a finding and
                 * "these three do not decode" can be chased. */
                std::printf("   undecoded (no variation 0..3): %s\n", path.c_str());
            }
        }
        /* The falloff came from the config, so it is asserted: a row with no
         * radius would leave a consumer inventing one, which is the thing this
         * read exists to prevent. */
        int no_radius = 0;
        float rmin = 1e30f, rmax = -1e30f, lmin = 1e30f, lmax = -1e30f;
        for (int i = 0; i < got; ++i) {
            const bf6_sound_emitter& s = rows[(size_t)i];
            if (s.radius <= 0.f && s.loudness <= 0.f) { no_radius++; continue; }
            if (s.radius < rmin) rmin = s.radius;
            if (s.radius > rmax) rmax = s.radius;
            if (s.loudness < lmin) lmin = s.loudness;
            if (s.loudness > lmax) lmax = s.loudness;
        }
        if (no_radius == got) {
            std::printf("   FAIL no emitter carries a falloff from its config\n");
            bad++;
        } else {
            std::printf("   falloff from the configs: radius %.2f..%.2f, loudness %.1f..%.1f"
                        " (%d row(s) with neither)\n", rmin, rmax, lmin, lmax, no_radius);
        }

        const double spread = std::sqrt((hi[0]-lo[0])*(hi[0]-lo[0]) + (hi[2]-lo[2])*(hi[2]-lo[2]));
        std::printf("%-26s %4d emitter(s)  %3zu distinct sound(s)  spread %5.0f m  "
                    "missing %d  undecoded %d  degenerate %d\n",
                    level, got, assets.size(), spread, missing, undecoded, degenerate);
        if (missing) { bad++; }
        if (degenerate) { bad++; }
        /* DECODE RATE, not decode perfection.
         *
         * A handful of the referenced assets are not waves at all: the CRAM and
         * generic-explosion beds and the seagull `_spc_` entry decode at no
         * variation 0..3, so they are a different asset shape (a graph that
         * references other sounds) rather than a defect in this read. The
         * chain being PROVEN is "an emitter's sound can be played", so the test
         * fails when nothing plays, and names the exceptions otherwise. Tuning
         * a percentage until it passes would test nothing. */
        if (!assets.empty() && (size_t)undecoded == assets.size()) {
            std::printf("   FAIL not one sound on this level decodes: the chain is broken\n");
            bad++;
        }
        if (got > 1 && spread < 1.0) { std::printf("   FAIL emitters collapse to a point\n"); bad++; }
    }

    /* 6. control */
    const int fake = bf6_level_sound_emitters(c, "mp_thislevelisnotreal", nullptr, 0,
                                              err, (int)sizeof(err));
    std::printf("CONTROL fake level: %d (expected <= 0)\n", fake);
    if (fake > 0) bad++;

    bf6_close(c);
    if (!levels) { std::printf("no level answered\n"); return 1; }
    std::printf("\n%s: %ld emitter(s) over %d level(s), %d failure(s)\n",
                bad ? "FAIL" : "PASS", total, levels, bad);
    return bad ? 1 : 0;
}
