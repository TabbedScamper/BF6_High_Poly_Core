/* WHICH check in the RigAsset span join actually fails.
 *
 * bf6_anim_bindings currently reports "rig/DofSet graph unavailable" for
 * animations/kingston/global/rigging/soldier.rig and gives no further detail,
 * because bf6__anim_rig `continue`s past a non-qualifying instance and
 * `return false`s on the first imperfect span. Both are the right shape for a
 * fail-closed reader and the wrong shape for finding out why it closed.
 *
 * This mirrors that function's reads exactly and reports every step instead of
 * abandoning it, so the failing constraint is named rather than guessed.
 *
 * The offsets are not invented here. The research repo's
 * animation-rigasset-owns-dof-key-layout records this exact layout for this
 * exact asset:
 *     +0x28  0x8467D864  import of soldier.ske
 *     +0x38  0x9B5FAFC2  35 direct .ds imports
 *     +0x40  0x4CAF3F79  DeltaTrajectory .ds import      <- NOT in the +0x38 array
 *     +0x48  0xADE23047  2,271 compiled symbolic uint keys
 *     +0x50  0x918B083D  35 offsets into the compiled-key array
 * and notes that the three typed key arrays account for 2,263 of the 2,271
 * keys, leaving eight special/untyped. Both of those facts predict a span
 * arithmetic mismatch at the tail, which is what this measures.
 *
 *   anim_rig_span_probe <game_dir> [rig_ebx]
 */
#include "ebx.h"
#include "source.h"
#include "types.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace bf6;

struct DofRecord { uint32_t component_type, stride, byte_offset; std::string name; };

static bool load_ebx(Source& src, TypeDb& types, const std::string& name, Ebx& out)
{
    std::string error;
    std::string asset = name;
    std::vector<uint8_t> raw = src.get_ebx(asset, error);
    if (raw.empty()) raw = src.get_ebx(asset + ".ebx", error);
    if (raw.empty() && asset.size() > 4 &&
        asset.compare(asset.size() - 4, 4, ".ebx") == 0)
    {
        asset.resize(asset.size() - 4);
        raw = src.get_ebx(asset, error);
    }
    if (raw.empty()) return false;
    out.set_guid_index(&src.partition_index());
    std::string e;
    return out.parse(std::move(raw), e) && out.instance_count() > 0;
}

/* The same 32-byte header / 32-byte record / name-pool read the shipping
 * DofSet loader performs, but reporting the reason instead of bailing. */
static bool read_dof_set(Source& src, TypeDb& types, const std::string& path,
                         std::vector<DofRecord>& records, std::string& why)
{
    Ebx ebx(types);
    if (!load_ebx(src, types, path, ebx)) { why = "asset unreadable"; return false; }
    for (size_t instance = 0; instance < ebx.instance_count(); ++instance)
    {
        const int64_t base = ebx.payload() + ebx.instance_offset(instance);
        std::vector<uint8_t> bytes;
        if (!ebx.opaque_array_at(base + 0x28, 8, bytes) || bytes.size() < 32)
            continue;
        uint32_t header[8] = {};
        std::memcpy(header, bytes.data(), sizeof(header));
        const uint64_t count = header[1] == 0xFFFFFFFFu ? 0 : (uint64_t)header[1] + 1;
        const uint64_t records_end = 32 + count * 32;
        if (records_end > bytes.size() || header[4] != records_end ||
            (uint64_t)header[4] + header[5] > bytes.size()) continue;
        records.assign((size_t)count, DofRecord{});
        for (size_t i = 0; i < records.size(); ++i)
        {
            const uint8_t* source = bytes.data() + 32 + i * 32;
            uint32_t w[4] = {};
            std::memcpy(w, source, sizeof(w));
            if (!w[1] || w[2] % w[1] || w[3] >= header[5])
            { why = "record " + std::to_string(i) + " failed the stride/offset guard"; return false; }
            records[i].component_type = w[0];
            records[i].stride = w[1];
            records[i].byte_offset = w[2];
            const char* nm = (const char*)bytes.data() + header[4] + w[3];
            const size_t avail = (size_t)header[5] - w[3];
            records[i].name.assign(nm, strnlen(nm, avail));
        }
        return true;
    }
    why = "no instance carried a well-formed slot table";
    return false;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    { std::fprintf(stderr, "usage: anim_rig_span_probe <game_dir> [rig_ebx]\n"); return 2; }
    const std::string rig = argc > 2 ? argv[2]
        : "animations/kingston/global/rigging/soldier.rig";

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
        if (!types.open(cand, te)) continue;
        if (types.looks_encrypted()) continue;
        typed = true; break;
    }
    if (!typed) { std::fprintf(stderr, "no readable type schema\n"); return 1; }

    Ebx ebx(types);
    if (!load_ebx(src, types, rig, ebx))
    { std::fprintf(stderr, "rig unreadable: %s\n", rig.c_str()); return 1; }
    std::printf("rig %s : %zu instances\n\n", rig.c_str(), ebx.instance_count());

    int qualifying = 0;
    for (size_t instance = 0; instance < ebx.instance_count(); ++instance)
    {
        const int64_t base = ebx.payload() + ebx.instance_offset(instance);
        std::vector<uint8_t> dof_bytes, key_bytes, offset_bytes;
        Ebx::OpaqueArrayDescriptor dof_desc;
        const bool a = ebx.opaque_array_at(base + 0x38, 8, dof_bytes, &dof_desc);
        const bool b = ebx.opaque_array_at(base + 0x48, 4, key_bytes);
        const bool c2 = ebx.opaque_array_at(base + 0x50, 4, offset_bytes);
        if (!a && !b && !c2) continue;   /* not a rig-root instance at all */

        std::vector<uint32_t> keys(key_bytes.size() / 4), offsets(offset_bytes.size() / 4);
        if (!keys.empty()) std::memcpy(keys.data(), key_bytes.data(), keys.size() * 4);
        if (!offsets.empty()) std::memcpy(offsets.data(), offset_bytes.data(), offsets.size() * 4);

        std::printf("instance %zu: dof_array=%s(count=%llu bytes=%zu) keys=%s(%zu) offsets=%s(%zu)\n",
                    instance, a ? "ok" : "MISSING",
                    (unsigned long long)dof_desc.count, dof_bytes.size(),
                    b ? "ok" : "MISSING", keys.size(),
                    c2 ? "ok" : "MISSING", offsets.size());
        if (!a || !b || !c2 || keys.empty()) continue;

        if (offsets.size() != dof_desc.count)
            std::printf("   CHECK FAIL offsets.size()=%zu != dof count=%llu\n",
                        offsets.size(), (unsigned long long)dof_desc.count);
        if (dof_bytes.size() != offsets.size() * 8)
            std::printf("   CHECK FAIL dof_bytes=%zu != offsets*8=%zu\n",
                        dof_bytes.size(), offsets.size() * 8);
        bool monotonic = !offsets.empty() && offsets[0] == 0;
        for (size_t i = 0; i < offsets.size(); ++i)
            monotonic = monotonic && offsets[i] < keys.size() &&
                        (!i || offsets[i] > offsets[i - 1]);
        std::printf("   monotonic=%s  offsets[0]=%u  last=%u  keys=%zu\n",
                    monotonic ? "yes" : "NO",
                    offsets.empty() ? 0u : offsets[0],
                    offsets.empty() ? 0u : offsets.back(), keys.size());
        if (offsets.size() != dof_desc.count ||
            dof_bytes.size() != offsets.size() * 8 || !monotonic) continue;
        qualifying++;

        /* THE SPAN JOIN, reported per set instead of abandoned on the first
         * mismatch, so a single bad tail cannot hide 34 good sets. */
        int exact = 0, mismatched = 0, unreadable = 0;
        size_t covered = 0;
        for (size_t i = 0; i < offsets.size(); ++i)
        {
            std::string partition, imported;
            const int64_t ptr = ebx.payload() + dof_desc.offset + (int64_t)i * 8;
            if (!ebx.import_ref_at(ptr, partition, imported) || imported.empty())
            { std::printf("   [%2zu] IMPORT UNRESOLVED\n", i); unreadable++; continue; }

            std::vector<DofRecord> recs;
            std::string why;
            const size_t end = i + 1 < offsets.size() ? offsets[i + 1] : keys.size();
            const size_t span = end - offsets[i];
            if (!read_dof_set(src, types, imported, recs, why))
            {
                std::printf("   [%2zu] %-58s UNREADABLE (%s) span=%zu\n",
                            i, imported.c_str(), why.c_str(), span);
                unreadable++; continue;
            }
            covered += recs.size();
            if (recs.size() == span) exact++;
            else
            {
                mismatched++;
                std::printf("   [%2zu] %-58s records=%zu span=%zu  DIFF %+d\n",
                            i, imported.c_str(), recs.size(), span,
                            (int)recs.size() - (int)span);
            }
        }
        std::printf("\n   spans exact=%d mismatched=%d unreadable=%d of %zu\n",
                    exact, mismatched, unreadable, offsets.size());
        std::printf("   dof records total=%zu   compiled keys=%zu   keys not covered=%d\n",
                    covered, keys.size(), (int)keys.size() - (int)covered);
        std::printf("\n   VERDICT: the shipping reader returns false unless "
                    "mismatched=0 and unreadable=0.\n");
    }
    if (!qualifying)
        std::printf("\nNo instance passed the structural checks, so the span join "
                    "was never reached.\n");
    return 0;
}
