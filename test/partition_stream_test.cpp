/* partition_stream_test - bounded vs full-read partition index on a real install.
 *
 *   partition_stream_test <game_dir> <level> [--armory] [--rounds N] [--sample N]
 *
 * Mounts <level> (or, with --armory, the front-end mount and its bounded armory
 * index), then builds the partition index with the original full-record read
 * and with the bounded range read, alternating F,B per round so neither route
 * always gets the warm OS file cache. For each build it prints entries, GUIDs,
 * duplicates lost to first-wins, a content hash of the published index, read
 * time, bytes read, blocks decoded/decoded bytes and blocks skipped.
 *
 * Pass requires identical hashes across every build, identical candidate
 * lists (armory), per-record GUID agreement on a deterministic sample, and
 * two controls: GUIDs read from a reference shifted by one byte must never
 * match the true GUID, and a shuffled name/GUID pairing must (nearly) never
 * match. The armory disk cache is disabled for the run so both routes read.
 */
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "cas.h"
#include "source.h"

namespace {

struct Fnv {
    uint64_t h = 1469598103934665603ull;
    void add(const std::string& s) { for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; } h ^= 0xFF; h *= 1099511628211ull; }
};

uint64_t index_hash(const std::map<std::string, std::string>& m) {
    Fnv f; for (const auto& kv : m) { f.add(kv.first); f.add(kv.second); } return f.h;
}
uint64_t candidates_hash(const std::map<std::string, std::vector<std::string>>& m) {
    Fnv f; for (const auto& kv : m) { f.add(kv.first); for (const auto& p : kv.second) f.add(p); } return f.h;
}

void print_stats(const char* label, const bf6::PartitionIndexStats& s, size_t entries, uint64_t hash) {
    std::printf("%-8s %s entries=%zu partitions=%llu guids=%llu dup=%llu hash=%016llx "
                "read=%.3fs file_reads=%llu file_bytes=%llu decoded_blocks=%llu decoded_bytes=%llu "
                "skipped_blocks=%llu reuses=%llu failures=%llu mapped_reads=%llu mapped_bytes=%llu mapped_faults=%llu%s%s\n",
        label, s.full_read ? "FULL   " : "BOUNDED", entries,
        (unsigned long long)s.partitions, (unsigned long long)s.guids, (unsigned long long)s.duplicates,
        (unsigned long long)hash, s.read_seconds,
        (unsigned long long)s.io.file_reads, (unsigned long long)s.io.file_bytes,
        (unsigned long long)s.io.blocks_decoded, (unsigned long long)s.io.decoded_bytes,
        (unsigned long long)s.io.blocks_skipped, (unsigned long long)s.io.block_reuses,
        (unsigned long long)s.io.failures,
        (unsigned long long)s.io.mapped_reads, (unsigned long long)s.io.mapped_bytes,
        (unsigned long long)s.io.mapped_faults,
        s.from_cache ? " FROM-CACHE(invalid A/B)" : "", s.cancelled ? " CANCELLED" : "");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: partition_stream_test <game_dir> <level> [--armory] [--rounds N] [--sample N]\n");
        return 2;
    }
    const std::string game = argv[1], level = argv[2];
    bool armory = false;
    int rounds = 1;
    size_t sample = 4000;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--armory")) armory = true;
        else if (!std::strcmp(argv[i], "--rounds") && i + 1 < argc) rounds = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--sample") && i + 1 < argc) sample = (size_t)std::max(1, std::atoi(argv[++i]));
    }
#ifdef _MSC_VER
    _putenv_s("BF6_DISABLE_ARMORY_INDEX_CACHE", "1");
    _putenv_s("BF6_PARTITION_INDEX_FULL_READ", "");
#else
    setenv("BF6_DISABLE_ARMORY_INDEX_CACHE", "1", 1);
    unsetenv("BF6_PARTITION_INDEX_FULL_READ");
#endif

    bf6::Source src;
    // Every round must be a real build, so not the index stored beside a
    // mount snapshot.
    src.set_mount_snapshots(false);
    std::string err;
    if (!src.open(game, err)) { std::fprintf(stderr, "open: %s\n", err.c_str()); return 1; }
    const bool mounted = armory ? src.mount_frontend(err) : src.mount_level(level, false, err);
    if (!mounted) { std::fprintf(stderr, "mount: %s\n", err.c_str()); return 1; }
    std::printf("mounted %s: ebx=%zu res=%zu\n", armory ? "front-end" : level.c_str(),
                src.ebx_count(), src.res_count());

    int failures = 0;
    uint64_t first_hash = 0, first_cand = 0;
    std::map<std::string, std::string> reference;
    for (int r = 0; r < rounds; ++r) {
        for (int mode = 0; mode < 2; ++mode) {
            const bool full = mode == 0;
            src.reset_partition_indexes();
            src.set_partition_index_full_read(full);
            uint64_t h = 0, ch = 0;
            size_t entries = 0;
            bf6::PartitionIndexStats st;
            if (armory) {
                const auto& m = src.armory_partition_index();
                h = index_hash(m); ch = candidates_hash(src.armory_partition_candidates());
                entries = m.size(); st = src.armory_partition_index_stats();
                if (reference.empty()) reference = m;
            } else {
                const auto& m = src.partition_index();
                h = index_hash(m); entries = m.size(); st = src.partition_index_stats();
                if (reference.empty()) reference = m;
            }
            char label[32]; std::snprintf(label, sizeof(label), "round%d", r);
            print_stats(label, st, entries, h);
            if (st.from_cache || st.cancelled || st.full_read != full) { std::printf("FAIL: build was not a real %s read\n", full ? "full" : "bounded"); ++failures; }
            if (r == 0 && mode == 0) { first_hash = h; first_cand = ch; }
            else if (h != first_hash || ch != first_cand) { std::printf("FAIL: index hash differs from first build\n"); ++failures; }
        }
    }

    // Per-record agreement on a deterministic stride sample, over every mounted
    // partition the index considered (duplicate losers included).
    std::vector<std::pair<std::string, bf6::EbxEntry>> all;
    for (const auto& kv : src.ebx()) all.emplace_back(kv.first, kv.second);
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (armory) {
        // Keep only names the armory index could have published.
        std::map<std::string, int> inidx;
        for (const auto& kv : src.armory_partition_candidates()) for (const auto& p : kv.second) inidx[p] = 1;
        std::vector<std::pair<std::string, bf6::EbxEntry>> keep;
        for (auto& e : all) if (inidx.count(e.first + ".ebx")) keep.push_back(e);
        all.swap(keep);
    }
    const size_t stride = std::max<size_t>(1, all.size() / sample);
    bf6::CasReadStats fs, bs, cs;
    size_t checked = 0, agree = 0, nonempty = 0, shifted_hits = 0, shuffled_hits = 0, shown = 0;
    std::vector<std::string> sampled;
    for (size_t i = 0; i < all.size(); i += stride) {
        const bf6::CasLoc& loc = all[i].second.loc;
        const std::string a = src.partition_guid(loc, true, &fs);
        const std::string b = src.partition_guid(loc, false, &bs);
        ++checked;
        if (a == b) ++agree;
        else if (shown++ < 10) std::printf("MISMATCH %s full=%s bounded=%s\n", all[i].first.c_str(), a.c_str(), b.c_str());
        if (!b.empty()) ++nonempty;
        sampled.push_back(b);
        // CONTROL: the same reference shifted one byte.
        bf6::CasLoc shifted = loc; shifted.off += 1; shifted.size = shifted.size > 0 ? shifted.size - 1 : 0;
        const std::string c = src.partition_guid(shifted, false, &cs);
        if (!c.empty() && c == b) ++shifted_hits;
    }
    // CONTROL: pair each sampled GUID with its neighbour's.
    for (size_t i = 0; i + 1 < sampled.size(); ++i)
        if (!sampled[i].empty() && sampled[i] == sampled[i + 1]) ++shuffled_hits;

    std::printf("sample: checked=%zu agree=%zu nonempty=%zu | full bytes=%llu decoded=%llu | "
                "bounded bytes=%llu decoded=%llu skipped_blocks=%llu\n",
        checked, agree, nonempty,
        (unsigned long long)fs.file_bytes, (unsigned long long)fs.decoded_bytes,
        (unsigned long long)bs.file_bytes, (unsigned long long)bs.decoded_bytes,
        (unsigned long long)bs.blocks_skipped);
    std::printf("controls: shifted-by-one-byte hits=%zu (expect 0), neighbour-pairing hits=%zu of %zu (shared-bytes aliases only)\n",
        shifted_hits, shuffled_hits, sampled.size() ? sampled.size() - 1 : 0);

    if (agree != checked) { std::printf("FAIL: per-record GUID disagreement\n"); ++failures; }
    if (nonempty == 0) { std::printf("FAIL: sample produced no GUIDs\n"); ++failures; }
    if (shifted_hits != 0) { std::printf("FAIL: shifted control matched\n"); ++failures; }
    if (shuffled_hits * 100 > sampled.size()) { std::printf("FAIL: shuffled control matched too often\n"); ++failures; }
    if (reference.empty()) { std::printf("FAIL: empty index\n"); ++failures; }

    std::printf("partition_stream_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
