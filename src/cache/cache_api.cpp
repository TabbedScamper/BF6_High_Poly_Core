// C API for the up-front cache (bf6_precache_*). See docs/PRECACHE.md.
#include "bf6_core.h"
#include "../cas.h"
#include "../oodle.h"

#include "cache_store.h"
#include "game_layers.h"
#include "pack.h"
#include "progress.h"
#include "thread_pool.h"
#include "../../include/bf6_cache_identity.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <stdio.h>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <set>
#include <cctype>
#include <unordered_map>
#include <mutex>
#include <sstream>

namespace {

using namespace bf6::cache;

void put_text(char* out, int out_len, const std::string& text)
{
    if (!out || out_len <= 0) return;
    std::snprintf(out, (std::size_t)out_len, "%s", text.c_str());
}

// One unit of cached output per map. Producers write into the map's pack and may
// place shared payloads in the content store.
struct LayerContext {
    const std::string& level;
    PackWriter& pack;
    ContentStore& shared;
    std::function<void(double, const std::string&)> report;
    const std::atomic<bool>& cancel;
};

struct Layer {
    std::string id;
    std::uint32_t version = 1;
    double weight = 1.0;
    std::function<bool(LayerContext&, std::string&)> produce;
};

// Phase 1 has no game layers yet. The self-test layer exercises every piece of
// the infrastructure with synthetic data so engines can wire their UI against a
// real build, and so tests can prove resume, cancel and invalidation.
std::vector<Layer> self_test_layers()
{
    std::vector<Layer> layers;
    auto synthetic = [](const std::string& id, int records, int shared_every) {
        return [id, records, shared_every](LayerContext& ctx, std::string& err) {
            for (int i = 0; i < records; ++i) {
                if (ctx.cancel.load()) { err = "cancelled"; return false; }
                std::string payload(1024 + (i % 7) * 97, (char)('a' + (i % 26)));
                payload += ctx.level + "/" + id + "/" + std::to_string(i);
                if (!ctx.pack.add(id + "/" + std::to_string(i), payload, err)) return false;
                if (shared_every > 0 && i % shared_every == 0) {
                    bool added = false;
                    const std::string key = "selftest/" + id + "/" + std::to_string(i / shared_every % 5);
                    if (!ctx.shared.put(key, std::string(4096, (char)('A' + i % 26)), added, err)) return false;
                }
                ctx.report(double(i + 1) / records, id + ": " + std::to_string(i + 1) + " / " + std::to_string(records));
            }
            return true;
        };
    };
    layers.push_back({ "placements", 1, 1.0, synthetic("placements", 200, 0) });
    layers.push_back({ "terrain", 1, 3.0, synthetic("terrain", 120, 10) });
    layers.push_back({ "props", 1, 6.0, synthetic("props", 400, 4) });
    return layers;
}

}  // namespace

std::string lower(std::string s)
{
    for (char& ch : s) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
    return s;
}

struct bf6_precache {
    Store store;
    std::string game_dir;                      // empty when opened by identity only
    bool game = false;                         // building real game layers
    GameBuildOptions game_options;
    std::map<std::string, std::string> read_name;   // store name -> name the reader expects
    ContentStore shared;
    Progress progress;
    std::vector<Layer> layers;
    std::vector<std::string> levels;
    std::atomic<bool> cancel{ false };
    std::atomic<bool> running{ false };
    std::thread worker;
    // Reader side. One persistent pool: its threads keep their archive handles
    // open between batches (cas.cpp caches handles per thread).
    std::mutex pool_mutex;
    std::unique_ptr<ThreadPool> read_pool;
    ThreadPool& readers()
    {
        std::lock_guard<std::mutex> lock(pool_mutex);
        if (!read_pool) {
#ifdef _WIN32
            if (_getmaxstdio() < 8192) _setmaxstdio(8192);   // each reader thread holds archive handles
#endif
            read_pool = std::make_unique<ThreadPool>(0);
        }
        return *read_pool;
    }
    std::mutex batch_mutex;   // one batch on the pool at a time (wait() is pool-wide)
    // Mesh resource -> shared record key, for one level at a time.
    std::mutex index_mutex;
    std::string index_level;
    std::unordered_map<std::string, std::string> mesh_index;

    bool index_for(const std::string& level, std::string& err)
    {
        if (index_level == level) return true;
        mesh_index.clear();
        index_level.clear();
        PackReader pr;
        std::string rows;
        if (!pr.open(store.map_dir(level), "placements", err) || !pr.read("rows", rows, err)) return false;
        const auto* d = (const std::uint8_t*)rows.data();
        const std::size_t n = rows.size();
        std::size_t at = 0;
        bool ok = true;
        auto u32 = [&]() -> std::uint32_t { std::uint32_t v = 0; if (at + 4 > n) { ok = false; return 0; } std::memcpy(&v, d + at, 4); at += 4; return v; };
        auto str = [&]() -> std::string { const std::uint32_t len = u32(); if (!ok || at + len > n) { ok = false; return {}; } std::string s((const char*)d + at, len); at += len; return s; };
        if (u32() != 0x4C504642 || u32() != kPlacementsVersion) { err = "placements record format"; return false; }
        const std::uint32_t count = u32();
        for (std::uint32_t i = 0; ok && i < count; ++i) {
            std::string key = str();
            std::string res = str();
            if (at + 48 > n) { ok = false; break; }
            at += 48;   // xform
            str(); str(); str();   // bundle, variation, source
            if (res.size() >= 4 && res.compare(res.size() - 4, 4, ".ebx") == 0) res.resize(res.size() - 4);
            mesh_index.emplace(lower(res + "_mesh"), std::move(key));
        }
        if (!ok) { mesh_index.clear(); err = "placements record truncated"; return false; }
        index_level = level;
        return true;
    }

    bool mesh_record(const std::string& level, const std::string& mesh_res, std::string& out)
    {
        std::string key, err;
        {
            std::lock_guard<std::mutex> lock(index_mutex);
            if (!index_for(level, err)) return false;
            const auto it = mesh_index.find(lower(mesh_res));
            if (it == mesh_index.end()) return false;
            key = it->second;
        }
        return shared.get(key, out, err);
    }

    ~bf6_precache()
    {
        cancel = true;
        if (worker.joinable()) worker.join();
    }

    void use_game_layers()
    {
        game = true;
        layers = {
            { "placements", kPlacementsVersion, 1.0, nullptr },
            { "meshes", kMeshesVersion, 4.0, nullptr },
            { "textures", kTexturesVersion, 6.0, nullptr },
            { "terrain", kTerrainVersion, 1.0, nullptr },
        };
    }

    std::map<std::string, std::uint32_t> required() const
    {
        std::map<std::string, std::uint32_t> r;
        if (game) {
            r["placements"] = kPlacementsVersion; r["meshes"] = kMeshesVersion;
            r["textures"] = kTexturesVersion; r["terrain"] = kTerrainVersion;
            return r;
        }
        for (const Layer& l : layers) r[l.id] = l.version;
        return r;
    }

    bool build_map(const std::string& level, std::string& err)
    {
        PackWriter pack;
        std::map<std::string, LayerRecord> records;
        const fs::path dir = store.map_dir(level);
        for (const Layer& layer : layers) {
            if (cancel.load()) { err = "cancelled"; return false; }
            if (!pack.begin(dir, layer.id, err)) return false;
            LayerContext ctx{ level, pack, shared,
                [this, &level, &layer](double f, const std::string& item) { progress.layer(level, layer.id, f, item); },
                cancel };
            progress.layer(level, layer.id, 0.0, layer.id);
            if (!layer.produce(ctx, err)) { pack.abandon(); return false; }
            LayerRecord rec;
            Digest d;
            rec.version = layer.version;
            if (!pack.finish(rec.bytes, d, err)) return false;
            rec.digest = d.hex();
            records[layer.id] = rec;
            progress.layer(level, layer.id, 1.0);
        }
        if (!shared.flush(err)) return false;
        return store.write_map_complete(level, records, err);
    }

    void run_game()
    {
        progress.state(Progress::State::Running);
#ifdef _WIN32
        // Every decode thread keeps its own handle to each game archive it reads
        // (see cas.cpp); the C runtime default of 512 stdio files runs out.
        if (_getmaxstdio() < 8192) _setmaxstdio(8192);
#endif
        const auto req = required();
        bool failed = false;
        std::string first_error;
        // Opened on the first map that needs building: a complete cache never mounts the game.
        bf6_ctx* ctx = nullptr;
        for (const std::string& level : levels) {
            if (cancel.load()) break;
            if (store.map_complete(level, req)) { progress.map_done(level, true); continue; }
            if (!ctx) {
                char e[1024] = {};
                ctx = bf6_open(game_dir.c_str(), e, sizeof e);
                if (!ctx) { failed = true; first_error = std::string("cannot open the installation: ") + e; break; }
            }
            GameLayerResult r;
            std::string err;
            auto report = [this, &level](const char* layer, double f, const std::string& item) {
                progress.layer(level, layer, f, item);
            };
            const auto t0 = std::chrono::steady_clock::now();
            bool ok = build_game_level(ctx, read_name[level], store.map_dir(level), shared, game_options,
                                       report, cancel, r, err);
            if (ok) ok = shared.flush(err);
            const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (ok) {
                std::map<std::string, LayerRecord> records;
                records["placements"] = { kPlacementsVersion, r.placement_bytes, r.placements_digest };
                records["meshes"] = { kMeshesVersion, r.mesh_bytes, "" };
                records["textures"] = { kTexturesVersion, r.texture_bytes, "" };
                records["terrain"] = { kTerrainVersion, r.terrain_bytes, r.terrain_digest };
                ok = store.write_map_complete(level, records, err);
            }
            std::ostringstream stats;
            stats << "{\n  \"level\": \"" << level << "\", \"ok\": " << (ok ? "true" : "false")
                  << ",\n  \"seconds_total\": " << total << ", \"seconds_open\": " << r.seconds_open
                  << ", \"seconds_meshes\": " << r.seconds_meshes << ", \"seconds_textures\": " << r.seconds_textures
                  << ", \"seconds_terrain\": " << r.seconds_terrain << ", \"seconds_write_incl_wait\": " << r.seconds_write
                  << ",\n  \"placements\": " << r.placements << ", \"mesh_reads\": " << r.mesh_reads
                  << ", \"meshes_new\": " << r.meshes_new << ", \"meshes_failed\": " << r.meshes_failed
                  << ", \"textures_new\": " << r.textures_new << ", \"textures_failed\": " << r.textures_failed
                  << ",\n  \"mesh_bytes\": " << r.mesh_bytes << ", \"texture_bytes\": " << r.texture_bytes
                  << ", \"placement_bytes\": " << r.placement_bytes << ", \"terrain_bytes\": " << r.terrain_bytes
                  << ", \"texture_max_dim\": " << game_options.texture_max_dim
                  << ",\n  \"sections\": " << r.sections << ", \"sections_with_colours\": " << r.sections_with_colours
                  << ", \"colour_bytes\": " << r.colour_bytes << ", \"geometry_bytes\": " << r.geometry_bytes
                  << ", \"material_bytes\": " << r.material_bytes << "\n}\n";
            std::string ignore;
            atomic_write(store.map_dir(level) / "stats.json", stats.str(), ignore);
            progress.map_done(level, ok);
            if (!ok) { failed = true; if (first_error.empty()) first_error = level + ": " + err; }
        }
        if (ctx) bf6_close(ctx);
        std::string err;
        if (!shared.flush(err)) { failed = true; if (first_error.empty()) first_error = err; }
        progress.shared(1.0);
        if (cancel.load()) progress.state(Progress::State::Idle, "cancelled; completed maps are kept");
        else if (failed) progress.state(Progress::State::Failed, first_error);
        else if (!store.write_install_complete(levels, err)) progress.state(Progress::State::Failed, err);
        else progress.state(Progress::State::Done);
        running = false;
    }

    void run()
    {
        if (game) { run_game(); return; }
        progress.state(Progress::State::Running);
        std::string err;
        const auto req = required();
        // Maps run concurrently; the pool size bounds memory as well as CPU.
        const std::size_t threads = std::max<std::size_t>(1, std::min<std::size_t>(
            levels.size(), std::max<unsigned>(1, std::thread::hardware_concurrency() / 2)));
        std::atomic<bool> failed{ false };
        std::string first_error;
        std::mutex error_mutex;
        {
            ThreadPool pool(threads);
            for (const std::string& level : levels) {
                if (store.map_complete(level, req)) { progress.map_done(level, true); continue; }
                pool.submit([&, level] {
                    std::string e;
                    const bool ok = !cancel.load() && build_map(level, e);
                    progress.map_done(level, ok);
                    if (!ok) {
                        failed = true;
                        std::lock_guard<std::mutex> lock(error_mutex);
                        if (first_error.empty()) first_error = level + ": " + e;
                    }
                });
            }
            pool.wait();
        }
        if (!shared.flush(err)) { failed = true; if (first_error.empty()) first_error = err; }
        progress.shared(1.0);
        if (cancel.load()) progress.state(Progress::State::Idle, "cancelled; completed maps are kept");
        else if (failed) progress.state(Progress::State::Failed, first_error);
        else if (!store.write_install_complete(levels, err)) progress.state(Progress::State::Failed, err);
        else progress.state(Progress::State::Done);
        running = false;
    }
};

extern "C" {

bf6_precache* bf6_precache_open_identity(const char* install_identity, const char* cache_root,
                                   char* err, int err_len)
{
    if (!install_identity || !cache_root) { put_text(err, err_len, "missing argument"); return nullptr; }
    auto cache = std::make_unique<bf6_precache>();
    std::string e;
    if (!cache->store.open(fs::u8path(cache_root), install_identity, e) ||
        !cache->shared.open(cache->store.shared_dir(), e)) {
        put_text(err, err_len, e);
        return nullptr;
    }
    return cache.release();
}

bf6_precache* bf6_precache_open(const char* game_dir, const char* cache_root, char* err, int err_len)
{
    if (!game_dir || !cache_root) { put_text(err, err_len, "missing argument"); return nullptr; }
    const auto snap = ::bf6_cache::inspect(fs::u8path(game_dir));
    if (!snap.ok) { put_text(err, err_len, "cannot identify installation: " + snap.error); return nullptr; }
    bf6_precache* cache = bf6_precache_open_identity(snap.identity.c_str(), cache_root, err, err_len);
    if (cache) {
        cache->game_dir = game_dir;
        cache->use_game_layers();   // map readiness is answerable before any build
    }
    return cache;
}

void bf6_precache_close(bf6_precache* cache) { delete cache; }

const char* bf6_precache_key(bf6_precache* cache) { return cache ? cache->store.key().c_str() : ""; }

int bf6_precache_sweep_stale(bf6_precache* cache, char* err, int err_len)
{
    if (!cache) return -1;
    std::string e;
    const int removed = cache->store.sweep_stale(e);
    if (!e.empty()) put_text(err, err_len, e);
    return removed;
}

namespace {
int64_t store_record(bf6_precache* cache, const std::string& key, uint8_t** out)
{
    std::string rec, err;
    if (!cache->shared.get(key, rec, err)) return 0;
    auto* b = (uint8_t*)std::malloc(rec.size() ? rec.size() : 1);
    if (!b) return -1;
    std::memcpy(b, rec.data(), rec.size());
    *out = b;
    return (int64_t)rec.size();
}
}  // namespace

int64_t bf6_precache_mesh_record_scoped(bf6_precache* cache, const char* mesh_res, const char* placing_bundle,
                                        const char* variation, uint8_t** out)
{
    if (!out) return -1;
    *out = nullptr;
    if (!cache || !mesh_res || !*mesh_res) return -1;
    try {
        return store_record(cache, mesh_key(mesh_res, placing_bundle ? placing_bundle : "", variation ? variation : ""), out);
    } catch (...) {
        return -1;
    }
}

int64_t bf6_precache_texture_record(bf6_precache* cache, const char* texture_res, uint8_t** out)
{
    if (!out) return -1;
    *out = nullptr;
    if (!cache || !texture_res || !*texture_res) return -1;
    try {
        return store_record(cache, texture_key(texture_res), out);
    } catch (...) {
        return -1;
    }
}

int64_t bf6_precache_mesh_record(bf6_precache* cache, const char* level, const char* mesh_res, uint8_t** out)
{
    if (!out) return -1;
    *out = nullptr;
    if (!cache || !level || !mesh_res) return -1;
    try {
        std::string rec;
        if (!cache->mesh_record(lower(level), mesh_res, rec)) return 0;
        auto* b = (uint8_t*)std::malloc(rec.size() ? rec.size() : 1);
        if (!b) return -1;
        std::memcpy(b, rec.data(), rec.size());
        *out = b;
        return (int64_t)rec.size();
    } catch (...) {
        return -1;
    }
}

int64_t bf6_precache_mesh_surfaces(bf6_precache* cache, const char* level, const char* mesh_names,
                                   int lod, int threads, uint8_t** out)
{
    if (!out) return -1;
    *out = nullptr;
    if (!cache || !level || !mesh_names || cache->game_dir.empty()) return -1;
    try {
        std::vector<std::string> names;
        {
            const std::string all = mesh_names;
            std::size_t start = 0;
            while (start <= all.size()) {
                std::size_t end = all.find('\n', start);
                if (end == std::string::npos) end = all.size();
                names.push_back(all.substr(start, end - start));
                start = end + 1;
            }
            if (!all.empty() && all.back() == '\n') names.pop_back();
            if (all.empty()) names.clear();
        }
        const std::string lvl = lower(level);
        std::vector<std::string> blobs(names.size());
        std::vector<int64_t> lens(names.size(), 0);
        {
            (void)threads;
            std::lock_guard<std::mutex> batch(cache->batch_mutex);
            ThreadPool& pool = cache->readers();
            for (std::size_t i = 0; i < names.size(); ++i) {
                pool.submit([&, i] {
                    std::string rec;
                    if (names[i].empty() || !cache->mesh_record(lvl, names[i], rec)) return;
                    uint8_t* b = nullptr;
                    const int64_t len = bf6_meshset_surfaces_reference(cache->game_dir.c_str(),
                        (const uint8_t*)rec.data(), (int64_t)rec.size(), lod, 0,
                        nullptr, 0, nullptr, nullptr, 0, &b);
                    lens[i] = len;
                    if (len > 0 && b) blobs[i].assign((const char*)b, (std::size_t)len);
                    bf6_blob_free(b);
                });
            }
            pool.wait();
        }
        std::string o;
        auto put = [&](const void* p, std::size_t n) { o.append((const char*)p, n); };
        const std::uint32_t magic = 0x42534D42, count = (std::uint32_t)names.size();   // "BMSB"
        put(&magic, 4);
        put(&count, 4);
        for (std::size_t i = 0; i < names.size(); ++i) {
            const int64_t len = lens[i] > 0 ? (int64_t)blobs[i].size() : lens[i];
            put(&len, 8);
            if (len > 0) {
                o += blobs[i];
                while (o.size() % 4) o.push_back('\0');
            }
        }
        auto* b = (uint8_t*)std::malloc(o.size());
        if (!b) return -1;
        std::memcpy(b, o.data(), o.size());
        *out = b;
        return (int64_t)o.size();
    } catch (...) {
        return -1;
    }
}

namespace {

std::vector<std::string> split_lines(const char* s)
{
    std::vector<std::string> out;
    const std::string all = s ? s : "";
    std::size_t start = 0;
    while (start < all.size()) {
        std::size_t end = all.find('\n', start);
        if (end == std::string::npos) end = all.size();
        if (end > start) out.push_back(all.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

// The texture name table of a mesh reference record (see bf6_mesh_reference).
void reference_texture_names(const std::string& rec, std::vector<std::string>& names)
{
    std::size_t at = 0;
    auto u32 = [&](std::uint32_t& v) { if (at + 4 > rec.size()) return false; std::memcpy(&v, rec.data() + at, 4); at += 4; return true; };
    auto skip_str = [&]() { std::uint32_t n = 0; if (!u32(n) || at + n > rec.size()) return false; at += n; return true; };
    auto str = [&](std::string& s) { std::uint32_t n = 0; if (!u32(n) || at + n > rec.size()) return false; s.assign(rec.data() + at, n); at += n; return true; };
    std::uint32_t v = 0;
    if (!u32(v) || !u32(v) || !skip_str() || !u32(v) || !u32(v) || !u32(v) || !u32(v)) return;
    std::string guid;
    if (!str(guid)) return;
    if (!guid.empty() && (!skip_str() || !u32(v) || !u32(v))) return;
    std::uint32_t hidden = 0;
    if (!u32(hidden) || at + (std::size_t)hidden * 2 > rec.size()) return;
    at += (std::size_t)hidden * 2;
    std::uint32_t count = 0;
    if (!u32(count)) return;
    for (std::uint32_t k = 0; k < count; ++k) {
        std::string name;
        if (!str(name)) return;
        names.push_back(std::move(name));
    }
}

std::string hex_of(const std::uint8_t* p, std::size_t n, bool reversed)
{
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t b = reversed ? p[n - 1 - i] : p[i];
        s.push_back(digits[b >> 4]);
        s.push_back(digits[b & 15]);
    }
    return s;
}

uint8_t* copy_out(const std::string& o, int64_t& len)
{
    auto* b = (uint8_t*)std::malloc(o.size() ? o.size() : 1);
    if (!b) return nullptr;
    std::memcpy(b, o.data(), o.size());
    len = (int64_t)o.size();
    return b;
}

}  // namespace

int64_t bf6_precache_mesh_texture_names(bf6_precache* cache, const char* level, const char* mesh_names, uint8_t** out)
{
    if (!out) return -1;
    *out = nullptr;
    if (!cache || !level || !mesh_names) return -1;
    try {
        const std::string lvl = lower(level);
        std::vector<std::string> order;
        std::set<std::string> seen;
        for (const std::string& mesh : split_lines(mesh_names)) {
            std::string rec;
            if (!cache->mesh_record(lvl, mesh, rec)) continue;
            std::vector<std::string> names;
            reference_texture_names(rec, names);
            for (std::string& n : names)
                if (!n.empty() && seen.insert(n).second) order.push_back(std::move(n));
        }
        std::string o;
        for (const std::string& n : order) { o += n; o.push_back('\n'); }
        int64_t len = 0;
        *out = copy_out(o, len);
        return *out ? len : -1;
    } catch (...) {
        return -1;
    }
}

int64_t bf6_precache_texture_chunks(bf6_precache* cache, const char* texture_names, int max_dim, int threads, uint8_t** out)
{
    if (!out) return -1;
    *out = nullptr;
    if (!cache || !texture_names || cache->game_dir.empty() || !bf6::oodle_open(cache->game_dir.c_str())) return -1;
    try {
        const std::vector<std::string> names = split_lines(texture_names);
        struct Entry { std::string res, guid; std::vector<std::uint8_t> chunk; bool ok = false; };
        std::vector<Entry> entries(names.size());
        {
            (void)threads;
            std::lock_guard<std::mutex> batch(cache->batch_mutex);
            ThreadPool& pool = cache->readers();
            for (std::size_t i = 0; i < names.size(); ++i) {
                pool.submit([&, i] {
                    std::string rec, err;
                    if (!cache->shared.get("texref/" + names[i], rec, err)) return;
                    const auto* d = (const std::uint8_t*)rec.data();
                    const std::size_t n = rec.size();
                    std::size_t at = 0;
                    auto u32 = [&](std::uint32_t& v) { if (at + 4 > n) return false; std::memcpy(&v, d + at, 4); at += 4; return true; };
                    auto str = [&](std::string& s) { std::uint32_t len = 0; if (!u32(len) || at + len > n) return false; s.assign((const char*)d + at, len); at += len; return true; };
                    std::uint32_t magic = 0, version = 0, res_len = 0, count = 0;
                    if (!u32(magic) || !u32(version) || !u32(res_len) || at + res_len > n) return;
                    Entry& e = entries[i];
                    e.res.assign((const char*)d + at, res_len);
                    at += res_len;
                    if (!u32(count)) return;
                    struct Loc { std::string guid, path; std::uint32_t off = 0, size = 0; };
                    std::vector<Loc> locs;
                    for (std::uint32_t k = 0; k < count; ++k) {
                        Loc l;
                        if (!str(l.guid) || !str(l.path) || !u32(l.off) || !u32(l.size)) return;
                        locs.push_back(std::move(l));
                    }
                    // bf6_texture.gd: header, which_chunk, and the cap swap to the embedded chunk.
                    const auto* h = (const std::uint8_t*)e.res.data();
                    const std::size_t hn = e.res.size();
                    if (hn < 56) return;
                    const int w = h[22] | (h[23] << 8), ht = h[24] | (h[25] << 8);
                    const bool has_streamed = hn >= 180;
                    bool streamed_first = has_streamed && (h[21] & 0x10) != 0;
                    if (max_dim > 0 && streamed_first && (w > max_dim || ht > max_dim)) streamed_first = false;
                    const std::string order[2] = {
                        streamed_first ? hex_of(h + 164, 16, false) : hex_of(h + 40, 16, false),
                        streamed_first ? hex_of(h + 40, 16, false) : (has_streamed ? hex_of(h + 164, 16, false) : std::string()) };
                    for (const std::string& raw : order) {
                        if (raw.empty() || raw == std::string(32, '0')) continue;
                        std::string rev;
                        for (std::size_t k = 0; k < 32; k += 2) rev = raw.substr(k, 2) + rev;
                        for (const Loc& l : locs) {
                            std::string g = l.guid;
                            for (char& ch : g) ch = (char)std::tolower((unsigned char)ch);
                            if (g != raw && g != rev) continue;
                            std::string path = l.path;
                            const bool absolute = path.size() > 1 && (path[1] == ':' || path[0] == '/' || path[0] == '\\');
                            if (!absolute) {
                                std::string root = cache->game_dir;
                                if (!root.empty() && root.back() != '/' && root.back() != '\\') root += '/';
                                path = root + path;
                            }
                            std::string e2;
                            std::vector<std::uint8_t> bytes = bf6::cas_read(path, l.off, l.size, false, e2);
                            if (bytes.empty()) continue;
                            e.guid = raw;
                            e.chunk = std::move(bytes);
                            e.ok = true;
                            return;
                        }
                    }
                });
            }
            pool.wait();
        }
        // One allocation, written in place: these records carry hundreds of megabytes
        // and every intermediate copy was a single-threaded pass over all of it.
        auto padded = [](std::size_t n) { return 4 + ((n + 3) & ~std::size_t(3)); };
        std::size_t total = 8;
        for (std::size_t i = 0; i < names.size(); ++i) {
            const Entry& e = entries[i];
            total += padded(names[i].size()) + padded(e.res.size()) + padded(e.ok ? e.guid.size() : 0) + padded(e.ok ? e.chunk.size() : 0);
        }
        auto* blob = (uint8_t*)std::malloc(total);
        if (!blob) return -1;
        std::size_t at = 8;
        auto put = [&](const void* p, std::size_t n) {
            const std::uint32_t len32 = (std::uint32_t)n;
            std::memcpy(blob + at, &len32, 4);
            if (n) std::memcpy(blob + at + 4, p, n);
            const std::size_t step = padded(n);
            if (step > 4 + n) std::memset(blob + at + 4 + n, 0, step - 4 - n);
            at += step;
        };
        const std::uint32_t head[2] = { 0x42585442, (std::uint32_t)names.size() };   // "BTXB"
        std::memcpy(blob, head, 8);
        for (std::size_t i = 0; i < names.size(); ++i) {
            Entry& e = entries[i];
            put(names[i].data(), names[i].size());
            put(e.res.data(), e.res.size());
            put(e.guid.data(), e.ok ? e.guid.size() : 0);
            put(e.chunk.data(), e.ok ? e.chunk.size() : 0);
            std::vector<std::uint8_t>().swap(e.chunk);
        }
        *out = blob;
        return (int64_t)total;
    } catch (...) {
        return -1;
    }
}

int bf6_precache_map_ready(bf6_precache* cache, const char* level)
{
    if (!cache || !level || cache->layers.empty()) return 0;
    return cache->store.map_complete(lower(level), cache->required()) ? 1 : 0;
}

int bf6_precache_ready(bf6_precache* cache)
{
    if (!cache || cache->layers.empty() || cache->levels.empty() || cache->running.load()) return 0;
    if (!cache->store.install_complete(cache->levels)) return 0;
    for (const std::string& level : cache->levels)
        if (!cache->store.map_complete(level, cache->required())) return 0;
    return 1;
}

int bf6_precache_build_start(bf6_precache* cache, const char* const* levels, int level_count, int flags)
{
    if (!cache || !levels || level_count <= 0) return -1;
    if (cache->running.load()) return -3;
    if (cache->worker.joinable()) cache->worker.join();
    std::vector<std::string> list;
    std::map<std::string, std::string> names;
    for (int i = 0; i < level_count; ++i) {
        if (!levels[i]) return -1;
        const std::string key = lower(levels[i]);
        if (!Store::valid_level_name(key)) return -1;
        if (!names.count(key)) list.push_back(key);
        names[key] = levels[i];
    }
    cache->read_name = names;
    if (flags & BF6_PRECACHE_BUILD_SELFTEST) {
        cache->game = false;
        cache->layers = self_test_layers();
    } else {
        if (cache->game_dir.empty()) return -2;   // opened by identity: no installation to read
        cache->use_game_layers();
        cache->game_options.texture_max_dim = (flags & BF6_PRECACHE_BUILD_TEXTURES_2048) ? 2048 : 0;
    }
    cache->levels = list;
    std::vector<LayerSpec> specs;
    for (const Layer& l : cache->layers) specs.push_back({ l.id, l.weight });
    cache->progress.reset(list, std::vector<double>(list.size(), 1.0), specs, 0.05 * list.size());
    cache->cancel = false;
    cache->running = true;
    cache->progress.state(Progress::State::Running);   // visible before the worker starts
    cache->worker = std::thread([cache] { cache->run(); });
    return 0;
}

void bf6_precache_build_cancel(bf6_precache* cache)
{
    if (!cache) return;
    cache->cancel = true;
    if (cache->running.load()) cache->progress.state(Progress::State::Cancelling);
}

int bf6_precache_build_wait(bf6_precache* cache)
{
    if (!cache) return -1;
    if (cache->worker.joinable()) cache->worker.join();
    return (int)cache->progress.snapshot().state;
}

int bf6_precache_progress_get(bf6_precache* cache, bf6_precache_progress* out)
{
    if (!cache || !out || out->struct_size != (int32_t)sizeof(bf6_precache_progress)) return -1;
    const auto s = cache->progress.snapshot();
    out->state = (int32_t)s.state;
    out->overall = s.overall;
    out->shared = s.shared;
    out->map_count = s.map_count;
    out->maps_done = s.maps_done;
    out->bytes_written = (int64_t)cache->shared.bytes_written();
    out->seconds_since_update = s.seconds_since_update;
    put_text(out->current_map, sizeof(out->current_map), s.current_map);
    put_text(out->current_layer, sizeof(out->current_layer), s.current_layer);
    put_text(out->current_item, sizeof(out->current_item), s.current_item);
    put_text(out->error, sizeof(out->error), s.error);
    return 0;
}

int bf6_precache_layer_count(bf6_precache* cache)
{
    return cache ? (int)cache->progress.layer_specs().size() : -1;
}

int bf6_precache_layer_name(bf6_precache* cache, int index, char* out, int out_len)
{
    if (!cache) return -1;
    const auto specs = cache->progress.layer_specs();
    if (index < 0 || index >= (int)specs.size()) return -1;
    put_text(out, out_len, specs[(size_t)index].id);
    return 0;
}

int bf6_precache_map_progress_get(bf6_precache* cache, bf6_precache_map_progress* out, int out_max)
{
    if (!cache || out_max < 0) return -1;
    const auto maps = cache->progress.maps();
    if (out)
        for (int i = 0; i < out_max && i < (int)maps.size(); ++i) {
            bf6_precache_map_progress& row = out[i];
            std::memset(&row, 0, sizeof(row));
            put_text(row.level, sizeof(row.level), maps[(size_t)i].level);
            row.state = (int32_t)maps[(size_t)i].state;
            row.progress = maps[(size_t)i].progress;
            row.layer_count = (int32_t)std::min<std::size_t>(maps[(size_t)i].layers.size(), BF6_PRECACHE_MAX_LAYERS);
            for (int k = 0; k < row.layer_count; ++k) row.layer_progress[k] = maps[(size_t)i].layers[(size_t)k];
        }
    return (int)maps.size();
}

}  // extern "C"
