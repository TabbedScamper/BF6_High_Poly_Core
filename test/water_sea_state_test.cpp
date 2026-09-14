// The cascades every engine simulates: bf6_level_water_sims_effective.
//   water_sea_state_test <game_dir> <level>
// Checks, on a real install, that the sea state is applied exactly as the
// Unreal plugin applied it (wind for every cascade, amplitude from sea curve
// slot 7, min wavelength authored), and that the isolated route - the one the
// Godot binding uses - returns the same rows as the full route.
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

static std::vector<bf6_water_sim_v2> rows(bf6_ctx* c, const char* level, int isolated,
                                          int effective, bf6_ocean_sea_state* sea)
{
    const int n = effective ? bf6_level_water_sims_effective(c, level, isolated, nullptr, 0, sea)
                  : isolated ? bf6_level_water_sims_isolated(c, level, nullptr, 0)
                             : bf6_level_water_sims(c, level, nullptr, 0);
    std::vector<bf6_water_sim_v2> out(n > 0 ? size_t(n) : 0);
    if (n > 0) {
        if (effective) bf6_level_water_sims_effective(c, level, isolated, out.data(), n, sea);
        else if (isolated) bf6_level_water_sims_isolated(c, level, out.data(), n);
        else bf6_level_water_sims(c, level, out.data(), n);
    }
    return out;
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: water_sea_state_test <game_dir> <level>\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* iso = bf6_open(argv[1], err, sizeof(err));
    if (!iso) { std::printf("open failed: %s\n", err); return 1; }
    bf6_ocean_sea_state iso_sea{};
    const auto iso_raw = rows(iso, argv[2], 1, 0, nullptr);
    const auto iso_eff = rows(iso, argv[2], 1, 1, &iso_sea);
    bf6_close(iso);

    bf6_ctx* full = bf6_open(argv[1], err, sizeof(err));
    if (!full || bf6_open_level(full, argv[2], nullptr, 0, err, sizeof(err)) != 0) {
        std::printf("open_level failed: %s\n", err); return 1;
    }
    bf6_ocean_sea_state full_sea{};
    const auto full_raw = rows(full, argv[2], 0, 0, nullptr);
    const auto full_eff = rows(full, argv[2], 0, 1, &full_sea);
    bf6_close(full);

    std::printf("%s: %zu cascades, sea found %d force %.3f wind %.3f (curve %d of %d)\n",
                argv[2], iso_eff.size(), iso_sea.found, iso_sea.force, iso_sea.wind_mps,
                iso_sea.wind_curve_index, iso_sea.curve_count);
    check(!iso_eff.empty() && iso_eff.size() == iso_raw.size(), "isolated route has cascades");
    check(full_eff.size() == iso_eff.size(), "full route has the same cascade count");
    check(iso_sea.found == full_sea.found && iso_sea.force == full_sea.force &&
          iso_sea.wind_mps == full_sea.wind_mps, "both routes read the same sea state");
    const bool wind = iso_sea.found && iso_sea.wind_curve_index >= 0 && iso_sea.wind_mps > 0.f;
    const bool amp = wind && iso_sea.curve_count > 7 && iso_sea.curve_value[7] > 0.f;
    for (size_t i = 0; i < iso_eff.size(); ++i) {
        const auto& r = iso_raw[i];
        const auto& e = iso_eff[i];
        std::printf("  cascade %zu tile %.1f: wind %.3f -> %.3f, amplitude %.4f -> %.4f, min wavelength %.2f\n",
                    i, e.tile_dimension, r.wind_speed, e.wind_speed, r.wave_amplitude,
                    e.wave_amplitude, e.min_wavelength);
        check(e.wind_speed == (wind ? iso_sea.wind_mps : r.wind_speed), "wind as Unreal applied it");
        check(e.wave_amplitude == (amp ? iso_sea.curve_value[7] : r.wave_amplitude), "amplitude as Unreal applied it");
        check(e.min_wavelength == r.min_wavelength, "min wavelength authored");
        if (i < full_eff.size())
            check(e.wind_speed == full_eff[i].wind_speed && e.wave_amplitude == full_eff[i].wave_amplitude &&
                  e.tile_dimension == full_eff[i].tile_dimension && e.resolution == full_eff[i].resolution,
                  "isolated row equals full row");
    }
    std::printf("water_sea_state_test: %s (%d)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
