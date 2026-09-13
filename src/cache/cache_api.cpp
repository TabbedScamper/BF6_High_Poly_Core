// C API for the up-front cache (bf6_precache_*). See docs/PRECACHE.md.
#include "bf6_core.h"

#include "cache_store.h"
#include "pack.h"
#include "progress.h"
#include "thread_pool.h"
#include "../../include/bf6_cache_identity.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>

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

struct bf6_precache {
    Store store;
    ContentStore shared;
    Progress progress;
    std::vector<Layer> layers;
    std::vector<std::string> levels;
    std::atomic<bool> cancel{ false };
    std::atomic<bool> running{ false };
    std::thread worker;

    ~bf6_precache()
    {
        cancel = true;
        if (worker.joinable()) worker.join();
    }

    std::map<std::string, std::uint32_t> required() const
    {
        std::map<std::string, std::uint32_t> r;
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

    void run()
    {
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
    return bf6_precache_open_identity(snap.identity.c_str(), cache_root, err, err_len);
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

int bf6_precache_map_ready(bf6_precache* cache, const char* level)
{
    if (!cache || !level || cache->layers.empty()) return 0;
    return cache->store.map_complete(level, cache->required()) ? 1 : 0;
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
    for (int i = 0; i < level_count; ++i) {
        if (!levels[i] || !Store::valid_level_name(levels[i])) return -1;
        list.emplace_back(levels[i]);
    }
    cache->layers = (flags & BF6_PRECACHE_BUILD_SELFTEST) ? self_test_layers() : std::vector<Layer>{};
    if (cache->layers.empty()) return -2;   // no game layers exist yet (phase 1)
    cache->levels = list;
    std::vector<LayerSpec> specs;
    for (const Layer& l : cache->layers) specs.push_back({ l.id, l.weight });
    cache->progress.reset(list, std::vector<double>(list.size(), 1.0), specs, 0.05 * list.size());
    cache->cancel = false;
    cache->running = true;
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
