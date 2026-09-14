// The mount snapshot against a direct mount, field by field.
//
//   mount_snapshot_test <game_dir> <level> [--keep]
//
// Mounts the level three ways in one process: directly (snapshots off), through
// the snapshot with an empty snapshot folder (a miss, which writes it), and
// through the snapshot again (a hit, which maps it). Every res, ebx, loose chunk
// and bundle chunk entry, every bundle, depot and per-type count, and a sample
// of payload reads must match the direct mount. The snapshot folder is a fresh
// temporary directory, removed afterwards unless --keep is given.
#include "source.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using clk = std::chrono::steady_clock;
static double secs(clk::time_point a) { return std::chrono::duration<double>(clk::now() - a).count(); }

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; if (failures <= 20) { std::printf("FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } } while (0)

static bool same(const bf6::CasLoc& a, const bf6::CasLoc& b)
{
    return a.chunk_id == b.chunk_id && a.cas_ix == b.cas_ix && a.off == b.off && a.size == b.size;
}

struct Opened { bf6::Source src; double shared = 0, level = 0; bf6::MountSnapshotStats shared_stats, level_stats; };

static bool open_level(Opened& o, const std::string& game, const std::string& level, int snapshots)
{
    std::string err;
    o.src.set_mount_snapshots(snapshots != 0);
    if (!o.src.open(game, err)) { std::printf("open: %s\n", err.c_str()); return false; }
    // Exactly bf6_open's shared mount, then bf6_open_level's.
    auto t0 = clk::now();
    std::vector<std::string> tocs;
    std::error_code ec;
    for (const auto& f : std::filesystem::directory_iterator(game + "/Data/Win32", ec))
        if (f.path().extension() == ".toc") tocs.push_back(f.path().string());
    size_t mounted = 0;
    o.src.mount_tocs(tocs, "shared", mounted, err);
    o.shared = secs(t0);
    o.shared_stats = o.src.mount_snapshot_stats();
    t0 = clk::now();
    if (!o.src.mount_level(level, false, err)) { std::printf("mount_level: %s\n", err.c_str()); return false; }
    o.level = secs(t0);
    o.level_stats = o.src.mount_snapshot_stats();
    return true;
}

static void compare(const char* label, bf6::Source& want, bf6::Source& got)
{
    const int before = failures;
    CHECK(want.res().size() == got.res().size(), "%s res count %zu vs %zu", label, want.res().size(), got.res().size());
    CHECK(want.ebx().size() == got.ebx().size(), "%s ebx count %zu vs %zu", label, want.ebx().size(), got.ebx().size());
    CHECK(want.loose_chunks().size() == got.loose_chunks().size(), "%s loose chunk count", label);
    CHECK(want.bundle_chunks().size() == got.bundle_chunks().size(), "%s bundle chunk count", label);

    size_t n = 0;
    for (const auto& kv : want.res()) {
        const bf6::ResEntry* e = got.res().lookup(kv.first);
        CHECK(e, "%s res missing %s", label, kv.first.c_str());
        if (!e) continue;
        CHECK(same(e->loc, kv.second.loc) && e->dsize == kv.second.dsize && e->type == kv.second.type && e->rid == kv.second.rid,
              "%s res differs %s", label, kv.first.c_str());
        CHECK(want.bundle_of(kv.first) == got.bundle_of(kv.first), "%s res bundle differs %s", label, kv.first.c_str());
        ++n;
    }
    size_t iterated = 0;
    for (const auto& kv : got.res()) { (void)kv; ++iterated; }
    CHECK(iterated == want.res().size(), "%s res iteration %zu", label, iterated);
    for (const auto& kv : want.ebx()) {
        const bf6::EbxEntry* e = got.ebx().lookup(kv.first);
        CHECK(e && same(e->loc, kv.second.loc) && e->dsize == kv.second.dsize, "%s ebx differs %s", label, kv.first.c_str());
        CHECK(want.bundle_of_ebx(kv.first) == got.bundle_of_ebx(kv.first), "%s ebx bundle differs %s", label, kv.first.c_str());
        ++n;
    }
    for (const auto& kv : want.loose_chunks()) {
        const bf6::CasLoc* e = got.loose_chunks().lookup(kv.first);
        CHECK(e && same(*e, kv.second), "%s loose chunk differs %s", label, kv.first.c_str());
        ++n;
    }
    for (const auto& kv : want.bundle_chunks()) {
        const bf6::CasLoc* e = got.bundle_chunks().lookup(kv.first);
        CHECK(e && same(*e, kv.second), "%s bundle chunk differs %s", label, kv.first.c_str());
        ++n;
    }
    CHECK(want.depots_by_bundle() == got.depots_by_bundle(), "%s depots differ", label);
    CHECK(want.res_entries_by_type() == got.res_entries_by_type(), "%s per-type counts differ", label);
    CHECK(want.res_entries_total() == got.res_entries_total(), "%s listing total differs", label);
    CHECK(!got.res().lookup("no/such/resource/at/all"), "%s a missing name was found", label);

    // Payload reads through both, a spread sample.
    size_t step = std::max<size_t>(1, want.res().size() / 300), i = 0, reads = 0;
    for (const auto& kv : want.res()) {
        if (i++ % step) continue;
        std::string e1, e2;
        CHECK(want.get_res(kv.first, e1) == got.get_res(kv.first, e2), "%s payload differs %s", label, kv.first.c_str());
        ++reads;
    }
    std::printf("%s: %zu entries compared, %zu payloads read, %s\n", label, n, reads,
                failures == before ? "identical" : "DIFFERENT");
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: mount_snapshot_test <game_dir> <level> [--keep]\n"); return 2; }
    const std::string game = argv[1], level = argv[2];
    const bool keep = argc > 3 && std::strcmp(argv[3], "--keep") == 0;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
        ("bf6-mount-snapshot-test-" + std::to_string(clk::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
#ifdef _WIN32
    _putenv_s("BF6_ARMORY_INDEX_CACHE_DIR", dir.string().c_str());
#else
    setenv("BF6_ARMORY_INDEX_CACHE_DIR", dir.string().c_str(), 1);
#endif

    {
    Opened direct, miss, hit;
    if (!open_level(direct, game, level, 0)) return 1;
    std::printf("direct mount   shared %.3f s + level %.3f s (res %zu, ebx %zu)\n",
                direct.shared, direct.level, direct.src.res().size(), direct.src.ebx().size());
    if (!open_level(miss, game, level, 1)) return 1;
    std::printf("snapshot miss  shared %.3f s (saved %d, write %.3f s) + level %.3f s (saved %d, write %.3f s)\n",
                miss.shared, miss.shared_stats.saved, miss.shared_stats.save_seconds,
                miss.level, miss.level_stats.saved, miss.level_stats.save_seconds);
    if (!open_level(hit, game, level, 1)) return 1;
    std::printf("snapshot hit   shared %.3f s (loaded %d) + level %.3f s (loaded %d)  [%s]\n",
                hit.shared, hit.shared_stats.loaded, hit.level, hit.level_stats.loaded,
                hit.level_stats.note.c_str());
    CHECK(miss.shared_stats.saved && miss.level_stats.saved, "the miss did not write both layers (%s / %s)",
          miss.shared_stats.note.c_str(), miss.level_stats.note.c_str());
    CHECK(hit.shared_stats.loaded && hit.level_stats.loaded, "the hit did not load both layers (%s / %s)",
          hit.shared_stats.note.c_str(), hit.level_stats.note.c_str());

    compare("after writing", direct.src, miss.src);
    compare("mapped", direct.src, hit.src);

    const std::string other = level == "mp_isolated" ? "mp_dumbo" : "mp_isolated";
    auto folder_mb = [&]() {
        uint64_t bytes = 0;
        for (const auto& e : std::filesystem::directory_iterator(dir / "mount")) bytes += e.file_size();
        return bytes / 1048576.0;
    };
    const double one_level = folder_mb();

    // Mounting more after a mapped load still follows first-mount-wins.
    {
        std::string err;
        const std::vector<std::string> extra = hit.src.find_tocs(other, false);
        size_t m1 = 0, m2 = 0;
        direct.src.mount_tocs(extra, "extra", m1, err);
        hit.src.mount_tocs(extra, "extra", m2, err);
        CHECK(m1 == m2, "extra mount count %zu vs %zu", m1, m2);
        compare("mapped, then another level", direct.src, hit.src);
    }

    // A second level in its own context reuses the shared layers and stores
    // only its own.
    Opened second;
    if (!open_level(second, game, other, 1)) return 1;
    CHECK(second.shared_stats.loaded, "the second level did not reuse the shared layer (%s)", second.shared_stats.note.c_str());
    std::printf("snapshot files: %.1f MB for %s, %.1f MB with %s added\n", one_level, level.c_str(),
                folder_mb(), other.c_str());
    }
    // THE SWEEP. Plant two copies of the first layer under other first-layer
    // names: one intact (a chain that started from other archives and is still
    // valid) and one whose recorded TOC stamp no longer matches the disk (what a
    // game patch leaves). Remove the real first layer so the next open writes a
    // new one and sweeps.
    {
        std::string first_layer;
        for (const auto& e : std::filesystem::directory_iterator(dir / "mount")) {
            const std::string fn = e.path().filename().string();
            // <root16>-<base16>-<base16><32 more>.bf6m: the first layer's key starts with its base
            if (e.path().extension() == ".bf6m" && fn.size() > 34 && fn.compare(17, 16, fn, 34, 16) == 0)
                first_layer = e.path().string();
        }
        CHECK(!first_layer.empty(), "no first layer file found");
        if (!first_layer.empty()) {
            const std::filesystem::path src(first_layer);
            const std::string fn = src.filename().string();
            const std::string root = fn.substr(0, 16);
            const std::filesystem::path valid = dir / "mount" / (root + "-1111111111111111-1111111111111111" + fn.substr(50));
            const std::filesystem::path stale = dir / "mount" / (root + "-2222222222222222-2222222222222222" + fn.substr(50));
            std::filesystem::copy_file(src, valid);
            std::filesystem::copy_file(src, stale);
            {
                // Change one digit of the last stamp's modification time.
                std::fstream f(stale, std::ios::in | std::ios::out | std::ios::binary);
                std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                size_t at = bytes.size();
                while (at > 0 && !(bytes[at - 1] >= '0' && bytes[at - 1] <= '9')) --at;
                CHECK(at > 0, "no stamp digits in the planted copy");
                if (at > 0) {
                    bytes[at - 1] = bytes[at - 1] == '9' ? '8' : (char)(bytes[at - 1] + 1);
                    f.seekp(0);
                    f.write(bytes.data(), (std::streamsize)bytes.size());
                }
            }
            std::filesystem::remove(src);
            Opened again;
            if (!open_level(again, game, level, 1)) return 1;
            CHECK(again.shared_stats.saved, "reopening did not write a new first layer (%s)", again.shared_stats.note.c_str());
            CHECK(std::filesystem::exists(valid), "the sweep removed a chain whose archives are unchanged");
            CHECK(!std::filesystem::exists(stale), "the sweep kept a chain whose archives changed");
            std::printf("sweep: valid chain %s, stale chain %s\n",
                        std::filesystem::exists(valid) ? "kept" : "REMOVED",
                        std::filesystem::exists(stale) ? "KEPT" : "removed");
        }
    }

    std::error_code ec;
    if (!keep) std::filesystem::remove_all(dir, ec);
    else std::printf("kept %s\n", dir.string().c_str());
    std::printf("%s (%d failures)\n", failures ? "MOUNT SNAPSHOT TEST FAILED" : "MOUNT SNAPSHOT TEST PASSED", failures);
    return failures ? 1 : 0;
}
