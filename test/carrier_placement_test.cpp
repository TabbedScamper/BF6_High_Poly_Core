/* DOES THE LEVEL WALK REACH THE CARRIER?
 *
 *   carrier_placement_test <game_dir> <level> [substring]
 *
 * A Portal experience can stand on the carrier, but the carrier does not appear
 * in either editor. The carrier's geometry lives in prefabs named
 * `pf_carrier_interior_01_<faction>_carrierstrike`, and the name says why that
 * might be: they belong to the CARRIER STRIKE mode, not to the level's base
 * content. If the level walk never enters them, nothing downstream can place
 * them however many toggles are switched on.
 *
 * This asks the walk directly: which partitions did it enter, did any of them
 * carry the substring, and how many mesh rows came out of them. It also lists
 * every partition the mount holds matching the substring, so "the walk missed
 * it" is told apart from "the level does not have one".
 */
#include "ebx.h"
#include "source.h"
#include "types.h"
#include "walk.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>

using namespace bf6;

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: carrier_placement_test <game> <level> [substring]\n"); return 2; }
    const std::string level = argv[2];
    const std::string want = (argc > 3) ? argv[3] : "carrier";

    Source src;
    std::string err;
    if (!src.open(argv[1], err)) { std::printf("open: %s\n", err.c_str()); return 1; }
    if (!src.mount_level(level, false, err)) { std::printf("mount: %s\n", err.c_str()); return 1; }

    TypeDb types;
    bool got = false;
    for (const std::string& cand : TypeDb::exe_candidates(src.game_dir()))
        if (types.open(cand, err) && !types.looks_encrypted()) { got = true; break; }
    if (!got) { std::printf("types: %s\n", err.c_str()); return 1; }

    /* 1. WHAT THE MOUNT HOLDS. If the level ships no such partition the walk
     * cannot be blamed for missing it. */
    std::vector<std::string> in_mount;
    for (const auto& kv : src.ebx())
        if (kv.first.find(want) != std::string::npos) in_mount.push_back(kv.first);
    std::sort(in_mount.begin(), in_mount.end());
    std::printf("partitions in the mount matching \"%s\": %zu\n", want.c_str(), in_mount.size());
    for (size_t i = 0; i < in_mount.size() && i < 14; ++i)
        std::printf("    %s\n", in_mount[i].c_str());

    /* 2. WHAT THE WALK ENTERED. */
    Walk walk(src, types);
    walk.build_catalog();
    walk.set_record_entries(true);
    if (!walk.run(level, err)) { std::printf("walk: %s\n", err.c_str()); return 1; }
    std::printf("\nwalk: %zu row(s), %llu partition(s) entered, %llu missing, %llu unresolved\n",
                walk.rows().size(), (unsigned long long)walk.n_partitions,
                (unsigned long long)walk.n_missing, (unsigned long long)walk.n_unresolved);

    std::set<std::string> entered;
    for (const auto& e : walk.partition_entries()) entered.insert(e.ref);
    size_t entered_matching = 0;
    for (const std::string& p : entered)
        if (p.find(want) != std::string::npos) entered_matching++;
    std::printf("  partitions entered matching \"%s\": %zu\n", want.c_str(), entered_matching);
    for (const std::string& p : entered)
        if (p.find(want) != std::string::npos) std::printf("    entered %s\n", p.c_str());

    /* 3. WHAT CAME OUT. A partition can be entered and still place nothing. */
    struct Box {
        size_t n = 0;
        double sx = 0, sy = 0, sz = 0;
        float lo[3] = {1e30f, 1e30f, 1e30f};
        float hi[3] = {-1e30f, -1e30f, -1e30f};
    };
    std::map<std::string, Box> rows_by_src;
    for (const WalkRow& r : walk.rows()) {
        if (r.src.find(want) == std::string::npos) continue;
        Box& b = rows_by_src[r.src];
        b.n++;
        const Vec3& t = r.xf.m[3];
        b.sx += t.x; b.sy += t.y; b.sz += t.z;
        const float p[3] = {t.x, t.y, t.z};
        for (int k = 0; k < 3; ++k) {
            if (p[k] < b.lo[k]) b.lo[k] = p[k];
            if (p[k] > b.hi[k]) b.hi[k] = p[k];
        }
    }
    /* BY LAYER, which is what the editors gate on. `src` is the prefab a row
     * came out of; `scope` is the layer that pulled the prefab in, and that is
     * the thing a visibility switch keys on. Grouping by prefab answers "what
     * is the carrier made of" and cannot answer "why is it hidden". */
    std::map<std::string, Box> rows_by_scope;
    for (const WalkRow& r : walk.rows()) {
        if (r.src.find(want) == std::string::npos) continue;
        Box& b = rows_by_scope[r.scope];
        b.n++;
        const Vec3& t = r.xf.m[3];
        b.sx += t.x; b.sy += t.y; b.sz += t.z;
        const float p[3] = {t.x, t.y, t.z};
        for (int k = 0; k < 3; ++k) {
            if (p[k] < b.lo[k]) b.lo[k] = p[k];
            if (p[k] > b.hi[k]) b.hi[k] = p[k];
        }
    }
    std::printf("\n  the same rows BY LAYER (what a visibility switch keys on):\n");
    std::printf("    %6s  %-28s %s\n", "rows", "centre", "layer scope");
    for (const auto& kv : rows_by_scope) {
        const Box& b = kv.second;
        std::printf("    %6zu  %8.0f %8.0f %8.0f  %s\n", b.n,
                    b.sx / b.n, b.sy / b.n, b.sz / b.n,
                    kv.first.empty() ? "(always on)" : kv.first.c_str());
    }

    size_t total_rows = 0;
    for (const auto& kv : rows_by_src) total_rows += kv.second.n;
    /* WHERE each one sits, not just how many. Two layouts of the same ship can
     * be the same hull redressed (same centre) or two different ships on the
     * map (centres far apart), and the counts alone cannot tell those apart. */
    std::printf("  mesh rows whose source matches: %zu\n", total_rows);
    std::printf("    %6s  %-58s %s\n", "rows", "centre (game x,y,z metres)", "extent");
    for (const auto& kv : rows_by_src) {
        const Box& b = kv.second;
        std::printf("    %6zu  %8.0f %8.0f %8.0f  %6.0f x %6.0f x %6.0f  %s\n",
                    b.n, b.sx / b.n, b.sy / b.n, b.sz / b.n,
                    b.hi[0] - b.lo[0], b.hi[1] - b.lo[1], b.hi[2] - b.lo[2],
                    kv.first.c_str());
    }

    /* EVERY MATCHING PLACEMENT, so an external check can compare these against
     * something else - the markers a mode authors, for instance. Position plus
     * the RIGHT axis, because two layouts of the same ship can sit in the same
     * place and differ only in heading. */
    if (const char* dump = std::getenv("BF6_CARRIER_DUMP")) {
        FILE* f = std::fopen(dump, "w");
        if (f) {
            std::fprintf(f, "scope\tsrc\tx\ty\tz\trx\try\trz\n");
            for (const WalkRow& r : walk.rows()) {
                if (r.src.find(want) == std::string::npos) continue;
                std::fprintf(f, "%s\t%s\t%.2f\t%.2f\t%.2f\t%.4f\t%.4f\t%.4f\n",
                             r.scope.c_str(), r.src.c_str(),
                             r.xf.m[3].x, r.xf.m[3].y, r.xf.m[3].z,
                             r.xf.m[0].x, r.xf.m[0].y, r.xf.m[0].z);
            }
            std::fclose(f);
            std::printf("\nwrote placements to %s\n", dump);
        }
    }

    if (!in_mount.empty() && entered_matching == 0)
        std::printf("\nTHE LEVEL SHIPS IT AND THE WALK NEVER ENTERS IT.\n"
                    "Nothing downstream can place it, whatever the editor toggles say.\n");
    else if (total_rows == 0 && entered_matching > 0)
        std::printf("\nEntered but placed nothing: the geometry is gated below the entry.\n");
    else if (total_rows > 0)
        std::printf("\nThe walk does place it, so the gap is downstream of the walk.\n");
    return 0;
}
