/* WHERE DO REFLECTION VOLUMES ACTUALLY LIVE?
 *
 *   reflection_scan_test <game_dir> <level> [max_partitions]
 *
 * bf6_level_reflection_probes finds 35 volumes on mp_isolated and ZERO on
 * mp_subsurface and mp_dumbo. Either those levels ship no probes, or the
 * reader's search is too narrow: it looks only at partitions whose name
 * contains "lighting" and that sit inside the level's own directory.
 *
 * Guessing which would be wrong either way, so this scans EVERY mounted
 * partition for the BakedTexture field and reports which partitions carry it.
 * The answer is the set of name shapes the reader has to cover.
 *
 * The same fake-field control the reader uses runs here: a neighbouring hash
 * that does not exist must resolve nowhere, or "found it everywhere" means the
 * import read is matching anything rather than that probes are everywhere.
 */
#include "ebx.h"
#include "source.h"
#include "types.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace bf6;

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: reflection_scan_test <game> <level> [max]\n"); return 2; }
    const std::string level = argv[2];
    const int maxn = (argc > 3) ? std::atoi(argv[3]) : 100000;

    Source src;
    std::string err;
    if (!src.open(argv[1], err)) { std::printf("open: %s\n", err.c_str()); return 1; }
    if (!src.mount_level(level, false, err)) { std::printf("mount: %s\n", err.c_str()); return 1; }

    TypeDb types;
    bool got = false;
    for (const std::string& cand : TypeDb::exe_candidates(src.game_dir()))
        if (types.open(cand, err) && !types.looks_encrypted()) { got = true; break; }
    if (!got) { std::printf("types: %s\n", err.c_str()); return 1; }

    const uint32_t kBaked = 0xa8285286u;
    const uint32_t kFake  = 0xa8285287u;

    std::printf("mount carries %zu EBX partition(s); scanning up to %d\n",
                src.ebx().size(), maxn);
    int scanned = 0, with = 0, controls = 0, total_probes = 0;
    int in_level_dir = 0, name_has_lighting = 0;
    std::map<std::string, int> by_partition;
    std::map<std::string, double> far_by_partition;

    for (const auto& kv : src.ebx()) {
        if (scanned >= maxn) break;
        const std::string& name = kv.first;
        std::string e;
        std::vector<uint8_t> raw = src.get_ebx(name, e);
        if (raw.empty()) continue;
        Ebx ebx(types);
        if (!ebx.parse(std::move(raw), e)) continue;
        ebx.set_guid_index(&src.partition_index());
        scanned++;

        int here = 0;
        for (size_t i = 0; i < ebx.instance_count(); ++i) {
            std::string guid, path;
            if (ebx.import_ref(i, kBaked, guid, path)) here++;
            std::string g2, p2;
            if (ebx.import_ref(i, kFake, g2, p2)) controls++;
        }
        if (here <= 0) continue;
        /* WORLD OR PREFAB-LOCAL? A volume authored inside a prefab carries a
         * transform in the prefab's own space, and placing it as though it were
         * world space would scatter probes around the origin. The magnitude of
         * the translations tells the two apart without guessing: a level's own
         * volumes sit hundreds of metres out, a prefab's sit near its origin. */
        {
            const uint32_t kTr = 0xd6351edeu, kTrans = 0xbc4b07b4u;
            const uint32_t kX = 0x3901db14u, kY = 0x42fc0f5eu, kZ = 0x32a99b9cu;
            double far_m = 0.0;
            for (size_t i = 0; i < ebx.instance_count(); ++i) {
                std::string g, p;
                if (!ebx.import_ref(i, kBaked, g, p)) continue;
                const EbxValue v = ebx.read_instance(i);
                const EbxValue* tr = v.field(kTr);
                const EbxValue* t = tr ? tr->field(kTrans) : nullptr;
                if (!t) continue;
                auto num = [](const EbxValue* f) -> double {
                    if (!f) return 0.0;
                    if (f->kind == EbxValue::Kind::Real) return f->f;
                    if (f->kind == EbxValue::Kind::Int) return (double)f->i;
                    if (f->kind == EbxValue::Kind::Uint) return (double)f->u;
                    return 0.0;
                };
                const double x = num(t->field(kX)), y = num(t->field(kY)), z = num(t->field(kZ));
                const double d = std::sqrt(x*x + y*y + z*z);
                if (d > far_m) far_m = d;
            }
            far_by_partition[name] = far_m;
        }
        with++;
        total_probes += here;
        by_partition[name] = here;
        if (Source::level_dir_end(name, level) != std::string::npos) in_level_dir++;
        if (name.find("lighting") != std::string::npos) name_has_lighting++;
    }

    std::printf("%s: scanned %d partition(s), %d carry reflection volumes, %d volume(s) total\n",
                level.c_str(), scanned, with, total_probes);
    std::printf("  of those partitions: %d inside the level directory, %d with 'lighting' in the name\n",
                in_level_dir, name_has_lighting);
    std::printf("  CONTROL fabricated field resolved %d time(s) (expected 0)\n", controls);
    /* The reader's current filter is (in level dir) AND (name has lighting).
     * What it MISSES is what this is for. */
    int missed = 0;
    std::printf("\ncarrying partitions (marked MISSED where the current filter skips them):\n");
    for (const auto& kv : by_partition) {
        const bool lvl = Source::level_dir_end(kv.first, level) != std::string::npos;
        const bool lit = kv.first.find("lighting") != std::string::npos;
        const bool kept = lvl && lit;
        if (!kept) missed++;
        std::printf("  %-5s %4d  farthest %8.1f m  %s\n", kept ? "keep" : "MISS",
                    kv.second, far_by_partition[kv.first], kv.first.c_str());
    }
    std::printf("\n%d of %d carrying partition(s) are missed by the current filter\n",
                missed, with);
    if (controls > 0) { std::printf("CONTROL FAILED\n"); return 1; }
    return 0;
}
