/* EVERY ENABLED WATER CASCADE, and whether a mesh can represent it.
 *
 *   water_cascades_probe <game_dir> <level>
 *
 * mp_isolated (Tsuru Reef) reads as "boiling" in the Godot preview and correct
 * in Unreal. water_test shows only ONE simulation because it calls the legacy
 * bf6_level_water_sim, which is the first-cascade view the Unreal header
 * already warns about - "Tsuru authors four enabled simulations and dropping
 * three is not a valid approximation". So this prints all of them.
 *
 * The number that decides whether a surface can be drawn without boiling is the
 * ratio between the MESH's vertex spacing and the cascade's shortest wave. A
 * displacement field containing waves shorter than two vertices cannot be
 * sampled without aliasing, and a moving camera turns that spatial aliasing
 * into temporal churn - which is exactly what boiling is. So the ratio is
 * computed here rather than left for someone to work out.
 */
#include "bf6_core.h"

#include <cstdio>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: water_cascades_probe <game> <level>\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    if (bf6_open_level(c, argv[2], nullptr, 0, err, (int)sizeof(err)) != 0)
    { std::printf("open_level: %s\n", err); bf6_close(c); return 1; }

    const int n = bf6_level_water_sims(c, argv[2], nullptr, 0);
    std::printf("%s: bf6_level_water_sims reports %d enabled cascade(s)\n", argv[2], n);
    if (n > 0)
    {
        std::vector<bf6_water_sim_v2> rows((size_t)n);
        const int got = bf6_level_water_sims(c, argv[2], rows.data(), n);
        /* The Godot preview's finest draw-tree tile is 16 m across and carries
         * PATCH_QUADS = 16 quads, so one metre between vertices. */
        const float vertex_spacing_m = 1.0f;
        std::printf("  (preview mesh: 1.0 m between vertices at the finest tile)\n");
        std::printf("  %-3s %-9s %-6s %-9s %-9s %-8s %-9s %s\n",
                    "#", "tile_m", "res", "texel_m", "min_wave_m", "wind", "chop", "verdict");
        for (int i = 0; i < got; ++i)
        {
            const bf6_water_sim_v2& r = rows[(size_t)i];
            const float texel = r.resolution > 0 ? r.tile_dimension / (float)r.resolution : 0.f;
            /* Nyquist: a wave needs at least two vertices per wavelength. */
            const float need = r.min_wavelength * 0.5f;
            const char* verdict = r.min_wavelength <= 0.f ? "no minimum stated"
                : (need >= vertex_spacing_m ? "representable"
                   : "ALIASES - shorter than the mesh can carry");
            /* THE AMPLITUDE MATTERS MOST and was the field nobody printed.
             * A fine cascade is meant to be a small perturbation on the swell;
             * if the renderer ignores its authored amplitude and draws every
             * cascade at full strength, the smallest and fastest one dominates
             * the surface. */
            std::printf("  %-3d %-9.3f %-6d %-9.4f %-9.4f %-8.3f %-9.3f amp=%-9.4f lwr=%-6.3f thick=%-7.3f %s\n",
                        i, r.tile_dimension, r.resolution, texel, r.min_wavelength,
                        r.wind_speed, r.choppiness, r.wave_amplitude,
                        r.large_wave_reduction, r.wave_thickness, verdict);
        }
    }
    bf6_close(c);
    return 0;
}
