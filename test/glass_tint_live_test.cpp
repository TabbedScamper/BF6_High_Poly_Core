/* The authored glass transmission palette reaches a consumer through the
 * public ABI, on the real install.
 *
 * Guards the whole read path, not the decode in isolation: mount -> mesh ->
 * material desc -> glass_tint_palette. It fails if the payload stops being
 * 124 bytes, if the eight-entry stride changes, or if the field is dropped.
 *
 * The negative is in scope by construction. The SAME loop over the SAME meshes
 * reports the records that carry no palette; if that count were zero the test
 * fails, because a probe that finds the constant everywhere has not proved it
 * can tell the difference.
 *
 *   glass_tint_live_test <game_dir>
 */
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/* The neutral - clear glass. Slightly cyan, because glass passes blue better
 * than red. This is the fill the engine puts in unused palette slots, and the
 * value bf6_core reports when the constant is absent. */
static const float kNeutral[3] = { 0.79138f, 0.85638f, 0.89001f };

static bool near3(const float* a, const float* b, float eps = 0.0005f)
{
    for (int i = 0; i < 3; i++) if (std::fabs(a[i] - b[i]) > eps) return false;
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: glass_tint_live_test <game_dir>\n");
        return 2;
    }
    char err[512] = { 0 };
    bf6_ctx* ctx = bf6_open(argv[1], err, (int)sizeof(err));
    if (!ctx) { std::fprintf(stderr, "open: %s\n", err); return 1; }

    /* Vehicles whose glass is authored with a NON-neutral tint, plus one that
     * is authored neutral, so both branches are exercised on real data. */
    struct Case { const char* mesh; const char* what; };
    const Case cases[] = {
        { "common/hardware/vehicles/motorcycle/dirtbike01/art/"
          "ob_veh_motorcycle_dirtbike01_base_mesh",            "red lens expected" },
        { "common/hardware/vehicles/tank/aav7a1/art/"
          "ob_veh_tank_aav7a1_base_mesh",                      "vision block" },
        { "common/hardware/gadgets/misc/vehiclekeycard/art/"
          "ob_gad_misc_vehiclekeycard_base_1p_mesh",           "all-neutral expected" },
    };

    int meshes_read = 0, sections = 0;
    int with_palette = 0, without_palette = 0;
    int non_neutral = 0, bad_alpha = 0, above_one = 0;

    for (const Case& c : cases)
    {
        bf6_mesh* m = bf6_read_mesh(ctx, c.mesh, 0);
        if (!m) { std::printf("SKIP  %s (not in this install)\n", c.mesh); continue; }
        meshes_read++;
        std::printf("\n%s\n  (%s)\n", c.mesh, c.what);

        for (int i = 0; i < m->section_count; i++)
        {
            const bf6_section& s = m->sections[i];
            if (s.material < 0 || s.material >= m->material_count) continue;
            const bf6_material_desc& md = m->materials[s.material];
            sections++;

            if (md.glass_tint_count == 0) { without_palette++; continue; }
            with_palette++;

            if (md.glass_tint_count != 8)
            {
                std::printf("  FAIL section %d: glass_tint_count=%d, expected 8\n",
                            i, md.glass_tint_count);
                return 1;
            }
            /* Entry 0 must be the head of the palette, not a separate read. */
            for (int k = 0; k < 4; k++)
                if (md.glass_tint[k] != md.glass_tint_palette[k])
                {
                    std::printf("  FAIL section %d: glass_tint[%d] does not match "
                                "palette entry 0\n", i, k);
                    return 1;
                }
            for (int e = 0; e < 8; e++)
            {
                if (md.glass_tint_palette[e * 4 + 3] != 1.f) bad_alpha++;
                if (!near3(&md.glass_tint_palette[e * 4], kNeutral)) non_neutral++;
                for (int k = 0; k < 3; k++)
                    if (md.glass_tint_palette[e * 4 + k] > 1.f) above_one++;
            }
            std::printf("  section %2d translucent=%d entry0=(%.5f %.5f %.5f)\n",
                        i, md.translucent,
                        md.glass_tint[0], md.glass_tint[1], md.glass_tint[2]);
        }
        bf6_free(ctx, m);
    }

    std::printf("\n==== result ====\n");
    std::printf("meshes_read=%d sections=%d\n", meshes_read, sections);
    std::printf("with_palette=%d without_palette=%d non_neutral_entries=%d\n",
                with_palette, without_palette, non_neutral);
    std::printf("entries_above_one=%d (allowed: an emissive lens authors >1)\n",
                above_one);

    int rc = 0;
    if (meshes_read == 0)
    { std::printf("FAIL: no case mesh present in this install\n"); rc = 1; }
    if (with_palette == 0)
    { std::printf("FAIL: no material reported a palette\n"); rc = 1; }
    /* The in-scope control: the same loop must also find records WITHOUT one. */
    if (without_palette == 0)
    { std::printf("FAIL: every record reported a palette, so the negative is "
                  "not in scope and a present-everywhere bug would pass\n"); rc = 1; }
    if (non_neutral == 0)
    { std::printf("FAIL: every entry read as the neutral - the payload is "
                  "probably not being read at all\n"); rc = 1; }
    if (bad_alpha)
    { std::printf("FAIL: %d entries had w != 1\n", bad_alpha); rc = 1; }
    std::printf(rc ? "FAIL\n" : "PASS\n");
    bf6_close(ctx);
    return rc;
}
