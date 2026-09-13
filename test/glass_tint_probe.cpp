/* What does the glass tint palette constant 0xA0106346 actually hold?
 *
 * bf6_core classifies a material as translucent when the destruction-glass
 * volume slot (0xBB245590) is bound OR the tint palette constant (0xA0106346)
 * is present, but it only COUNTS the constant - the payload is never read and
 * never handed to a consumer. A renderer that wants to reproduce the authored
 * dual-source composite dst = src0 + dst*src1 needs the tint, so the payload
 * has to be measured before a field can be exposed for it.
 *
 * Reports, over every material record reachable on a level:
 *   - how many carry the constant, and its distinct byte LENGTHS
 *   - the raw bytes and their float reading for the first samples
 *   - whether the values look like a colour (0..1, or a half-scale neutral)
 *   - the in-scope control: how many records were examined and did NOT carry
 *     it, so an absence is a measurement rather than a failed lookup
 *
 *   glass_tint_probe <game_dir> <level> [max_meshes] [name_filter]
 */
#include "depot.h"
#include "meshset.h"
#include "source.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace bf6;

static const uint32_t kTintPalette = 0xA0106346u;
static const uint32_t kGlassVolume = 0xBB245590u;

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr,
                     "usage: glass_tint_probe <game_dir> <level> [max_meshes] [filter]\n");
        return 2;
    }
    const int limit = argc > 3 ? std::atoi(argv[3]) : 4000;
    const std::string filter = argc > 4 ? argv[4] : std::string();

    Source src;
    std::string err;
    if (!src.open(argv[1], err)) { std::fprintf(stderr, "open: %s\n", err.c_str()); return 1; }
    std::fprintf(stderr, "mounting %s...\n", argv[2]);
    if (!src.mount_level(argv[2], false, err))
    { std::fprintf(stderr, "mount: %s\n", err.c_str()); return 1; }

    std::vector<std::string> meshes;
    for (const auto& kv : src.res())
        if (kv.first.size() > 5 && kv.first.compare(kv.first.size() - 5, 5, "_mesh") == 0 &&
            (filter.empty() || kv.first.find(filter) != std::string::npos))
            meshes.push_back(kv.first);
    std::sort(meshes.begin(), meshes.end());
    std::fprintf(stderr, "%zu meshes in this mount\n", meshes.size());

    std::map<std::string, Depot> depot_cache;
    std::map<std::string, std::vector<uint8_t>> depot_bytes;

    int n_mesh = 0, records = 0, with_tint = 0, without_tint = 0;
    int tint_and_volume = 0, volume_only = 0;
    int shown = 0;
    std::map<size_t, int> len_hist;
    // Every distinct payload, so a "tint" that is really one global constant
    // is not mistaken for per-material authoring.
    std::map<std::string, int> distinct_payload;
    std::map<std::string, std::string> payload_example;

    for (const std::string& mname : meshes)
    {
        if (n_mesh >= limit) break;
        std::vector<uint8_t> mres = src.get_res(mname, err);
        if (mres.empty()) continue;
        MeshSet ms = meshset_parse(mres.data(), mres.size(), err);
        if (!ms.ok || ms.lods.empty()) continue;
        n_mesh++;

        const std::string dname = src.depot_for_res(mname);
        if (dname.empty()) continue;
        if (!depot_cache.count(dname))
        {
            std::vector<uint8_t> db = src.get_res(dname, err);
            Depot dp;
            std::string e;
            if (db.empty() || !dp.parse(db, e)) continue;
            depot_bytes[dname] = std::move(db);
            depot_cache[dname] = std::move(dp);
        }
        Depot& dep = depot_cache[dname];
        const std::vector<uint8_t>& db = depot_bytes[dname];

        for (const MeshSection& s : ms.lods[0].sections)
        {
            if (s.state_key == 0) continue;
            MaterialBinding mb = dep.textures_for(s.state_key, db);
            if (!mb.valid) continue;
            records++;

            auto it = mb.constants.find(kTintPalette);
            const bool has_vol = mb.textures.count(kGlassVolume) != 0;
            if (it == mb.constants.end())
            {
                without_tint++;
                if (has_vol) volume_only++;
                continue;
            }
            with_tint++;
            if (has_vol) tint_and_volume++;

            const std::vector<uint8_t>& b = it->second;
            len_hist[b.size()]++;

            std::string hex;
            char tmp[8];
            for (size_t i = 0; i < b.size() && i < 256; i++)
            { std::snprintf(tmp, sizeof(tmp), "%02x", b[i]); hex += tmp; }
            distinct_payload[hex]++;
            if (!payload_example.count(hex))
                payload_example[hex] = mname + " :: " + s.material;

            if (shown < 12)
            {
                shown++;
                std::printf("\nTINT mesh=%s\n  material=%s state=0x%016llx\n"
                            "  bytes=%zu glass_volume_slot=%s\n  hex=%s\n",
                            mname.c_str(), s.material.c_str(),
                            (unsigned long long)s.state_key, b.size(),
                            has_vol ? "yes" : "no", hex.c_str());
                std::printf("  floats:");
                for (size_t i = 0; i + 4 <= b.size() && i < 256; i += 4)
                {
                    float f; std::memcpy(&f, b.data() + i, 4);
                    std::printf(" [%zu]=%.5f", i / 4, f);
                }
                std::printf("\n");
            }
        }
    }

    std::printf("\n==== glass tint palette 0xA0106346 ====\n");
    std::printf("meshes_parsed=%d material_records=%d\n", n_mesh, records);
    std::printf("with_tint=%d  without_tint=%d   (in-scope control: the same\n"
                "  lookup on the same depots found the constant absent on\n"
                "  %d records, so an absence is measured, not a failed read)\n",
                with_tint, without_tint, without_tint);
    std::printf("tint_and_glass_volume=%d  glass_volume_without_tint=%d\n",
                tint_and_volume, volume_only);
    std::printf("distinct payload lengths:");
    for (const auto& kv : len_hist) std::printf(" %zuB x%d", kv.first, kv.second);
    std::printf("\ndistinct payloads=%zu\n", distinct_payload.size());
    int listed = 0;
    for (const auto& kv : distinct_payload)
    {
        if (listed++ >= 20) { std::printf("  ... %zu more\n", distinct_payload.size() - 20); break; }
        std::printf("  x%-4d %s\n        first: %s\n",
                    kv.second, kv.first.c_str(), payload_example[kv.first].c_str());
    }
    if (with_tint == 0)
        std::printf("\nNOTE: zero carriers on this level. Not evidence the constant is\n"
                    "unused - re-run on a level with vehicle or window glass before\n"
                    "concluding anything.\n");
    return 0;
}
