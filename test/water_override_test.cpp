/* THE DIALLED SEA: bf6_level_water_sims_override.
 *
 *   water_override_test <game_dir> [level]
 *
 * Portal Ocean is the one map whose water a script can drive - BeaufortScale,
 * WaveAmplitude, WaterHeight - which makes it the only place a preview can be
 * held against a game showing a sea we chose. This checks the override behaves
 * like the game rather than like a convenient multiplier:
 *
 *  1. NO OVERRIDE IS THE OLD ANSWER. An all-zero override must return exactly
 *     what bf6_level_water_sims_effective returns, field for field. If it does
 *     not, every existing consumer just changed behaviour.
 *  2. THE FORCE RE-EVALUATES THE CURVES. Wind at force 6 must match the
 *     meteorological Beaufort table the curve reproduces (about 12.3 m/s), not
 *     the authored wind scaled by 6/2. A scale factor would pass a "wind went
 *     up" test and be wrong everywhere.
 *  3. IT IS MONOTONIC AND ORDERED across the force domain, because a sea that
 *     goes down as the wind goes up is not a sea.
 *  4. AMPLITUDE FOLLOWS THE PREFAB'S OWN ROUTING. gmpf_water writes
 *     WaveAmplitude into the 192 m cascade only, so a dialled amplitude must
 *     land there and nowhere else - the game cannot produce anything else.
 *  5. HEIGHT IS PASSED THROUGH, not simulated.
 *  6. CONTROL: a force the curves never see (negative) must not produce a
 *     negative wind.
 */
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int fails = 0;
static void ck(const char* what, bool ok)
{
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) fails++;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: water_override_test <game> [level]\n"); return 2; }
    const char* level = (argc > 2) ? argv[2] : "mp_portal_ocean";
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }

    bf6_water_sim_v2 base[8] = {};
    bf6_ocean_sea_state sea0{};
    const int n0 = bf6_level_water_sims_effective(c, level, 1, base, 8, &sea0);
    if (n0 <= 0) { std::printf("%s: no cascades (%d)\n", level, n0); bf6_close(c); return 1; }
    std::printf("%s: %d cascade(s), authored force %.3f wind %.3f\n",
                level, n0, sea0.force, sea0.wind_mps);

    /* 1. an empty override changes nothing */
    bf6_water_override none{};
    bf6_water_sim_v2 same[8] = {};
    bf6_ocean_sea_state sea1{};
    float h = -1.f;
    const int n1 = bf6_level_water_sims_override(c, level, 1, &none, same, 8, &sea1, &h);
    bool identical = (n1 == n0);
    for (int i = 0; identical && i < n0; ++i)
        identical = std::memcmp(&base[i], &same[i], sizeof(bf6_water_sim_v2)) == 0;
    ck("an empty override returns exactly the effective answer", identical);
    ck("an empty override reports the same sea", sea1.found == sea0.found
       && std::fabs(sea1.force - sea0.force) < 1e-6f
       && std::fabs(sea1.wind_mps - sea0.wind_mps) < 1e-6f);

    /* 2 and 3. the force re-evaluates the curve */
    const float forces[] = {0.f, 2.f, 4.f, 6.f, 8.f, 10.f};
    float winds[6] = {0};
    std::printf("  force -> wind, from the level's own ocean curves:\n");
    for (int i = 0; i < 6; ++i) {
        bf6_water_override ov{};
        ov.has_beaufort = 1;
        ov.beaufort = forces[i];
        bf6_water_sim_v2 rows[8] = {};
        bf6_ocean_sea_state s{};
        const int n = bf6_level_water_sims_override(c, level, 1, &ov, rows, 8, &s, nullptr);
        winds[i] = s.wind_mps;
        std::printf("      force %5.1f  wind %7.3f m/s  cascade0 wind %7.3f  (%d row(s))\n",
                    forces[i], s.wind_mps, n > 0 ? rows[0].wind_speed : 0.f, n);
        if (n > 0) ck("the dialled wind reaches the cascades",
                      std::fabs(rows[0].wind_speed - s.wind_mps) < 1e-3f);
    }
    /* The Beaufort table the curve reproduces: force 6 is about 12.3 m/s. A
     * naive scale of the authored 2.45 m/s by 6/2 would give 7.35 and pass any
     * "it went up" check, so the VALUE is asserted, not the direction. */
    ck("force 6 gives the meteorological wind (~12.3 m/s), not a scaled one",
       std::fabs(winds[3] - 12.3f) < 1.5f);
    bool monotonic = true;
    for (int i = 1; i < 6; ++i) if (winds[i] < winds[i - 1] - 1e-3f) monotonic = false;
    ck("wind rises with force across the whole domain", monotonic);

    /* 4. amplitude follows the prefab's routing */
    bf6_water_override amp{};
    amp.has_wave_amplitude = 1;
    amp.wave_amplitude = 3.5f;
    bf6_water_sim_v2 arows[8] = {};
    const int na = bf6_level_water_sims_override(c, level, 1, &amp, arows, 8, nullptr, nullptr);
    int changed = 0, unchanged = 0;
    for (int i = 0; i < na; ++i) {
        if (std::fabs(arows[i].wave_amplitude - 3.5f) < 1e-4f) changed++;
        else if (std::fabs(arows[i].wave_amplitude - base[i].wave_amplitude) < 1e-4f) unchanged++;
    }
    std::printf("  dialled amplitude 3.5: %d cascade(s) took it, %d kept theirs\n", changed, unchanged);
    ck("at least one cascade takes the dialled amplitude", changed >= 1);
    ck("every cascade either takes it or keeps its own", changed + unchanged == na);

    /* 5. height passes through */
    bf6_water_override hh{};
    hh.has_water_height = 1;
    hh.water_height = 123.5f;
    float got = -1.f;
    bf6_level_water_sims_override(c, level, 1, &hh, nullptr, 0, nullptr, &got);
    ck("the dialled water height comes back", std::fabs(got - 123.5f) < 1e-3f);

    /* 6. control */
    bf6_water_override bad{};
    bad.has_beaufort = 1;
    bad.beaufort = -5.f;
    bf6_ocean_sea_state sbad{};
    bf6_level_water_sims_override(c, level, 1, &bad, nullptr, 0, &sbad, nullptr);
    std::printf("  CONTROL force -5 -> wind %.3f\n", sbad.wind_mps);
    ck("a force below the domain does not produce a negative wind", sbad.wind_mps >= 0.f);

    bf6_close(c);
    std::printf("\n%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
