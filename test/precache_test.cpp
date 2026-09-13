// Self-contained tests for the up-front cache infrastructure (no game install).
//   precache_test <scratch folder>
#include "bf6_core.h"
#include "cache/cache_store.h"
#include "cache/pack.h"
#include "cache/progress.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace bf6::cache;

static int g_checks = 0, g_failures = 0;
static void check(bool ok, const char* label)
{
    ++g_checks;
    if (!ok) { ++g_failures; std::printf("FAIL %s\n", label); }
    else std::printf("PASS %s\n", label);
}

static bf6_precache_progress progress_of(bf6_precache* c)
{
    bf6_precache_progress p{};
    p.struct_size = sizeof(p);
    bf6_precache_progress_get(c, &p);
    return p;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: precache_test <scratch folder>\n"); return 2; }
    const fs::path scratch = fs::absolute(fs::u8path(argv[1]));
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);
    std::string err;

    // ---- pack files
    {
        PackWriter w;
        check(w.begin(scratch / "packs", "sample", err), "pack begins");
        check(w.add("a", std::string("alpha"), err) && w.add("b", std::string(100000, 'x'), err), "records append");
        check(!w.add("a", std::string("dup"), err), "duplicate record names are refused");
        std::string back;
        check(w.read_back("b", back, err) && back.size() == 100000, "unfinished records read back");
        std::uint64_t bytes = 0; Digest d;
        check(w.finish(bytes, d, err) && bytes == 100005, "pack publishes with its payload size");
        PackReader r;
        check(r.open(scratch / "packs", "sample", err) && r.size() == 2, "reader loads the index");
        std::string out;
        check(r.read("a", out, err) && out == "alpha", "record round-trips");
        // Corrupt one payload byte: the digest must catch it.
        { std::fstream f(scratch / "packs" / "sample.pack", std::ios::in | std::ios::out | std::ios::binary); f.seekp(7); f.put('!'); }
        PackReader r2;
        check(r2.open(scratch / "packs", "sample", err) && !r2.read("b", out, err), "a corrupted record is rejected");
        // Corrupt the index: the reader must refuse the pack.
        { std::fstream f(scratch / "packs" / "sample.idx", std::ios::in | std::ios::out | std::ios::binary); f.seekp(14); f.put('\x7f'); }
        PackReader r3;
        check(!r3.open(scratch / "packs", "sample", err), "a corrupted index is refused");
        PackWriter abandoned;
        abandoned.begin(scratch / "packs", "never", err);
        abandoned.add("x", std::string("y"), err);
        abandoned.abandon();
        check(!fs::exists(scratch / "packs" / "never.pack") && !fs::exists(scratch / "packs" / "never.pack.writing"), "an abandoned pack leaves nothing");
    }

    // ---- content store
    {
        ContentStore s;
        check(s.open(scratch / "store", err), "content store opens");
        bool added = false;
        check(s.put("k1", "payload-one", added, err) && added, "new content is stored");
        check(s.put("k1", "payload-one", added, err) && !added, "identical key is stored once");
        std::string out;
        check(s.get("k1", out, err) && out == "payload-one", "unflushed content reads back");
        check(s.flush(err), "store flushes");
        ContentStore reopened;
        check(reopened.open(scratch / "store", err) && reopened.contains("k1") && reopened.get("k1", out, err) && out == "payload-one", "content survives reopening");
    }

    // ---- progress
    {
        Progress p;
        p.reset({ "m1", "m2" }, { 1.0, 3.0 }, { {"a", 1.0}, {"b", 3.0} }, 0.0);
        p.layer("m2", "b", 0.5);
        const double first = p.snapshot().overall;
        check(first > 0.25 && first < 0.30, "overall is weighted by map cost and layer weight");
        p.layer("m2", "b", 0.2);
        check(p.snapshot().overall >= first, "progress never moves backwards");
        p.map_done("m1", true);
        const auto maps = p.maps();
        check(maps.size() == 2 && maps[0].level == "m1" && maps[0].progress == 1.0, "per-map rows keep build order and completion");
    }

    // ---- cache key and invalidation rules
    check(cache_key("install-A") == cache_key("install-A") && cache_key("install-A") != cache_key("install-B"), "key follows the installation");
    check(cache_key("install-A", kRecipe) != cache_key("install-A", kRecipe + 1), "a recipe bump changes the key");
    check(cache_key("install-A", kRecipe, kFormat) != cache_key("install-A", kRecipe, kFormat + 1), "a format bump changes the key");
    check(!Store::valid_level_name("../x") && !Store::valid_level_name("MP_Upper") && Store::valid_level_name("mp_dumbo"), "level names are validated");

    // ---- C API end to end with the self-test layers
    const std::string root = (scratch / "root").u8string();
    char e[256] = {};
    bf6_precache* c = bf6_precache_open_identity("identity-one", root.c_str(), e, sizeof e);
    check(c != nullptr, "precache opens for an identity");
    const char* levels[] = { "mp_one", "mp_two", "mp_three" };
    check(bf6_precache_build_start(c, levels, 3, 0) == -2, "a cache opened by identity refuses a game build (no installation)");
    check(bf6_precache_ready(c) == 0, "nothing is ready before a build");

    check(bf6_precache_build_start(c, levels, 3, BF6_PRECACHE_BUILD_SELFTEST) == 0, "self-test build starts");
    check(bf6_precache_build_start(c, levels, 3, BF6_PRECACHE_BUILD_SELFTEST) == -3, "a second start while running is refused");
    double last = 0.0; bool monotonic = true, saw_item = false;
    for (int i = 0; i < 20000; ++i) {
        const auto p = progress_of(c);
        if (p.overall + 1e-12 < last) monotonic = false;
        last = p.overall;
        if (p.current_item[0]) saw_item = true;
        if (p.state == BF6_PRECACHE_DONE || p.state == BF6_PRECACHE_FAILED) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const int final_state = bf6_precache_build_wait(c);
    const auto done = progress_of(c);
    check(final_state == BF6_PRECACHE_DONE && done.overall == 1.0 && done.maps_done == 3, "self-test build completes all maps");
    check(monotonic && saw_item, "polled progress is monotonic and names the current item");
    check(bf6_precache_ready(c) == 1 && bf6_precache_map_ready(c, "mp_two") == 1, "cache reports ready after the build");
    std::vector<bf6_precache_map_progress> rows(8);
    check(bf6_precache_map_progress_get(c, rows.data(), 8) == 3 && rows[1].state == BF6_PRECACHE_MAP_DONE && rows[1].layer_count == bf6_precache_layer_count(c), "per-map progress rows and layer names line up");

    // Cancel then resume: completed maps are kept and skipped.
    const fs::path cache_dir = fs::u8path(root) / "bf6hp-cache" / ("v" + std::to_string(kFormat)) / bf6_precache_key(c);
    fs::remove(cache_dir / "maps" / "mp_three" / "complete.json");
    fs::remove(cache_dir / "install-complete.json");
    check(bf6_precache_ready(c) == 0, "removing one map's completion makes the cache not ready");
    const auto kept = fs::last_write_time(cache_dir / "maps" / "mp_one" / "complete.json");
    check(bf6_precache_build_start(c, levels, 3, BF6_PRECACHE_BUILD_SELFTEST) == 0, "resume build starts");
    bf6_precache_build_wait(c);
    check(bf6_precache_ready(c) == 1 && fs::last_write_time(cache_dir / "maps" / "mp_one" / "complete.json") == kept, "resume rebuilds only the missing map");

    // A completion record for another key, or copied from another map, is not trusted.
    {
        const fs::path two = cache_dir / "maps" / "mp_two" / "complete.json";
        std::string original;
        read_file(two, original);
        std::string foreign_key = original;
        const std::string key = bf6_precache_key(c);
        foreign_key.replace(foreign_key.find(key), key.size(), std::string(key.size(), '0'));
        { std::ofstream(two, std::ios::binary | std::ios::trunc) << foreign_key; }
        check(bf6_precache_map_ready(c, "mp_two") == 0, "a completion record naming another cache key is rejected");
        std::string one;
        read_file(cache_dir / "maps" / "mp_one" / "complete.json", one);
        { std::ofstream(two, std::ios::binary | std::ios::trunc) << one; }
        check(bf6_precache_map_ready(c, "mp_two") == 0, "a completion record copied from another map is rejected");
        std::string layerless = original;
        layerless.replace(layerless.find("\"props\""), 7, "\"propz\"");
        { std::ofstream(two, std::ios::binary | std::ios::trunc) << layerless; }
        check(bf6_precache_map_ready(c, "mp_two") == 0, "a completion record missing a required layer is rejected");
        { std::ofstream(two, std::ios::binary | std::ios::trunc) << original; }
        check(bf6_precache_map_ready(c, "mp_two") == 1, "control: the original completion record is accepted");
    }

    bf6_precache_build_start(c, levels, 3, BF6_PRECACHE_BUILD_SELFTEST);
    bf6_precache_build_cancel(c);
    bf6_precache_build_wait(c);
    check(progress_of(c).state != BF6_PRECACHE_FAILED, "cancel ends a build without failing it");
    bf6_precache_close(c);

    // A different installation gets its own root; sweeping removes only our old roots.
    const fs::path foreign = fs::u8path(root) / "bf6hp-cache" / ("v" + std::to_string(kFormat)) / "not-ours";
    fs::create_directories(foreign);
    { std::ofstream(foreign / "keep.txt") << "user data"; }
    bf6_precache* c2 = bf6_precache_open_identity("identity-two", root.c_str(), e, sizeof e);
    check(c2 && std::string(bf6_precache_key(c2)) != cache_dir.filename().u8string(), "another installation gets another key");
    check(bf6_precache_ready(c2) == 0, "the new key starts empty");
    const int removed = bf6_precache_sweep_stale(c2, e, sizeof e);
    check(removed == 1 && !fs::exists(cache_dir) && fs::exists(foreign / "keep.txt"), "sweep removes the old cache and keeps folders it did not create");
    bf6_precache_close(c2);

    std::printf("precache_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
