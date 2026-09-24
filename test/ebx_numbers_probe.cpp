/* Every number in an asset, with the field-hash path that reaches it.
 *
 * Written to find a weapon's muzzle velocity, which the field-name tables do not
 * name: BFBulletEntityData's largest number is 150, so the speed is either on the
 * weapon's firing data (where Frostbite has historically kept Shot.InitialSpeed as
 * a Vec3) or scaled. Printing every number in a plausible range, with where it sits,
 * turns "not named" into a short list to check against a second weapon.
 *
 *   ebx_numbers_probe <game_dir> <asset> [<asset> ...]
 *   BF6_NUM_LO / BF6_NUM_HI   the range printed (default 100 .. 2000)
 *   BF6_NUM_ALL=1             print every number, not just the range
 */
#include "ebx.h"
#include "source.h"
#include "types.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace bf6;

static double g_lo = 100.0, g_hi = 2000.0;
static bool g_all = false;

static bool number(const EbxValue& v, double& out)
{
    switch (v.kind)
    {
    case EbxValue::Kind::Real: out = v.f; return true;
    case EbxValue::Kind::Int:  out = (double)v.i; return true;
    case EbxValue::Kind::Uint: out = (double)v.u; return true;
    default: return false;
    }
}

static void walk(const EbxValue& v, const std::string& path, int depth)
{
    if (depth > 12) return;
    double x = 0.0;
    if (number(v, x))
    {
        if (g_all || (std::fabs(x) >= g_lo && std::fabs(x) <= g_hi))
            std::printf("  %-60s %g\n", path.c_str(), x);
        return;
    }
    if (v.kind == EbxValue::Kind::Struct)
    {
        /* A three- or four-float struct is printed whole when any lane is in range,
         * because an initial speed has historically been a Vec3 (0, 0, speed). */
        if (v.fields.size() >= 3 && v.fields.size() <= 4)
        {
            double a[4] = {0, 0, 0, 0};
            bool all = true, hit = false;
            for (size_t k = 0; k < v.fields.size(); ++k)
            {
                if (!number(v.fields[k].second, a[k])) { all = false; break; }
                if (std::fabs(a[k]) >= g_lo && std::fabs(a[k]) <= g_hi) hit = true;
            }
            if (all)
            {
                if (hit || g_all)
                    std::printf("  %-60s (%g, %g, %g%s)\n", path.c_str(), a[0], a[1], a[2],
                                v.fields.size() == 4 ? ", ..." : "");
                return;
            }
        }
        for (const auto& f : v.fields)
        {
            char h[16];
            std::snprintf(h, sizeof h, "%08x", f.first);
            walk(f.second, path + "." + h, depth + 1);
        }
        return;
    }
    if (v.kind == EbxValue::Kind::Array)
    {
        for (size_t k = 0; k < v.items.size() && k < 64; ++k)
            walk(v.items[k], path + "[" + std::to_string(k) + "]", depth + 1);
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: ebx_numbers_probe <game_dir> <asset> [<asset> ...]\n");
        return 2;
    }
    if (const char* s = std::getenv("BF6_NUM_LO")) g_lo = std::atof(s);
    if (const char* s = std::getenv("BF6_NUM_HI")) g_hi = std::atof(s);
    g_all = std::getenv("BF6_NUM_ALL") != nullptr;

    Source src;
    std::string err;
    if (!src.open(argv[1], err)) { std::fprintf(stderr, "open: %s\n", err.c_str()); return 1; }
    if (!src.mount_level(std::string(), false, err))
    { std::fprintf(stderr, "mount: %s\n", err.c_str()); return 1; }
    TypeDb types;
    bool typed = false;
    for (const std::string& cand : TypeDb::exe_candidates(src.game_dir()))
    {
        std::string te;
        if (!types.open(cand, te) || types.looks_encrypted()) continue;
        typed = true;
        break;
    }
    if (!typed) { std::fprintf(stderr, "no readable type schema\n"); return 1; }

    for (int a = 2; a < argc; ++a)
    {
        const std::string asset = argv[a];
        std::vector<uint8_t> raw = src.get_ebx(asset + ".ebx", err);
        if (raw.empty()) raw = src.get_ebx(asset, err);
        if (raw.empty()) { std::printf("MISSING %s: %s\n", asset.c_str(), err.c_str()); continue; }
        Ebx ebx(types);
        ebx.set_guid_index(&src.armory_partition_index());
        std::string e;
        if (!ebx.parse(std::move(raw), e)) { std::printf("PARSE FAILED %s: %s\n", asset.c_str(), e.c_str()); continue; }
        std::printf("== %s  (%zu instances)\n", asset.c_str(), ebx.instance_count());
        for (size_t i = 0; i < ebx.instance_count(); ++i)
        {
            const EbxValue v = ebx.read_instance(i);
            char g[64];
            std::snprintf(g, sizeof g, "#%zu", i);
            walk(v, g, 0);
        }
    }
    return 0;
}
