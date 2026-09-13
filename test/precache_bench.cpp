// Where does precache time go? Times the existing core readers against a real
// installation, read-only, and writes nothing into the game folder.
//
//   precache_bench <game_dir> <level> [level...] [--meshes N] [--contexts K]
//
// Stages timed: open (mount + type schema + lift), level open (index + walk),
// placements, unique mesh reads (with materials), texture payload reads,
// terrain. --contexts K reopens the installation K times to price per-context
// parallelism (contexts are not thread-safe), then reads meshes on K threads.
#include "bf6_core.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using clk = std::chrono::steady_clock;
static double secs(clk::time_point a, clk::time_point b) { return std::chrono::duration<double>(b - a).count(); }

struct StageClock {
    std::mutex m;
    clk::time_point origin = clk::now();
    std::map<std::string, std::pair<double, double>> spans; // first, last seen
    std::vector<std::string> order;
};

static int on_progress(void* user, const char* stage, int, int)
{
    auto* s = (StageClock*)user;
    const double t = secs(s->origin, clk::now());
    std::lock_guard<std::mutex> lock(s->m);
    const std::string key = stage ? stage : "?";
    auto it = s->spans.find(key);
    if (it == s->spans.end()) { s->spans[key] = { t, t }; s->order.push_back(key); }
    else it->second.second = t;
    return 1;
}

struct MeshJob { std::string res, bundle, variation; };

// Order-sensitive FNV-1a over everything read, to prove optimisations change no bytes.
static void fold(unsigned long long& h, const unsigned char* p, size_t n)
{
    if (!p) return;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
}

struct ReadStats {
    unsigned long long digest = 1469598103934665603ull;
    int meshes = 0, failed = 0, sections = 0, textures = 0;
    long long vertices = 0, texture_bytes = 0;
    double mesh_seconds = 0, texture_seconds = 0;
};

static void read_meshes(bf6_ctx* ctx, const std::vector<MeshJob>& jobs, size_t begin, size_t end,
                        ReadStats& st, std::unordered_set<int>& seen_textures)
{
    for (size_t i = begin; i < end; ++i) {
        const auto t0 = clk::now();
        bf6_mesh* m = bf6_read_mesh_scoped(ctx, jobs[i].res.c_str(), 0,
            jobs[i].bundle.empty() ? nullptr : jobs[i].bundle.c_str(),
            jobs[i].variation.empty() ? nullptr : jobs[i].variation.c_str());
        st.mesh_seconds += secs(t0, clk::now());
        if (!m) { ++st.failed; continue; }
        ++st.meshes;
        for (int s = 0; s < m->section_count; ++s) {
            const bf6_section& sec = m->sections[s];
            fold(st.digest, (const unsigned char*)sec.positions, (size_t)sec.vertex_count * 3 * sizeof(float));
            if (sec.normals) fold(st.digest, (const unsigned char*)sec.normals, (size_t)sec.vertex_count * 3 * sizeof(float));
            if (sec.uv0) fold(st.digest, (const unsigned char*)sec.uv0, (size_t)sec.vertex_count * 2 * sizeof(float));
            fold(st.digest, (const unsigned char*)sec.indices, (size_t)sec.index_count * sizeof(uint32_t));
        }
        st.sections += m->section_count;
        for (int s = 0; s < m->section_count; ++s) st.vertices += m->sections[s].vertex_count;
        std::vector<int> ids;
        for (int k = 0; k < m->material_count; ++k)
            for (int b = 0; b < m->materials[k].texture_count; ++b) {
                const int id = m->materials[k].textures[b].texture;
                if (id >= 0 && seen_textures.insert(id).second) ids.push_back(id);
            }
        const auto t1 = clk::now();
        for (int id : ids) {
            const bf6_texture* tex = bf6_texture_at(ctx, id);
            if (tex && tex->data) {
                ++st.textures; st.texture_bytes += tex->data_len;
                const int32_t dims[4] = { tex->width, tex->height, tex->mip_count, (int32_t)tex->format };
                fold(st.digest, (const unsigned char*)dims, sizeof dims);
                fold(st.digest, tex->data, (size_t)tex->data_len);
            }
        }
        st.texture_seconds += secs(t1, clk::now());
        bf6_free(ctx, m);
    }
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: precache_bench <game_dir> <level> [level...] [--meshes N] [--contexts K]\n"); return 2; }
    const std::string game = argv[1];
    std::vector<std::string> levels;
    int mesh_limit = 400, contexts = 1, tex_threads = 16, max_dim = 2048;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--meshes") && i + 1 < argc) mesh_limit = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--tex-threads") && i + 1 < argc) tex_threads = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--max-dim") && i + 1 < argc) max_dim = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--contexts") && i + 1 < argc) contexts = std::max(1, std::atoi(argv[++i]));
        else levels.push_back(argv[i]);
    }
    char err[1024] = {};
    std::printf("hardware threads: %u\n", std::thread::hardware_concurrency());

    StageClock clock;
    auto t = clk::now();
    bf6_ctx* ctx = bf6_open(game.c_str(), err, sizeof err);
    if (!ctx) { std::printf("open failed: %s\n", err); return 1; }
    bf6_set_progress(ctx, on_progress, &clock);
    std::printf("open: %.2f s (lifted=%d)\n", secs(t, clk::now()), bf6_was_lifted(ctx));

    std::map<std::string, int> seen_in_maps;
    for (const std::string& level : levels) {
        std::printf("\n=== %s\n", level.c_str());
        clock.origin = clk::now();
        { std::lock_guard<std::mutex> lock(clock.m); clock.spans.clear(); clock.order.clear(); }
        t = clk::now();
        if (bf6_prepare_level(ctx, level.c_str(), err, sizeof err) != 0) { std::printf("prepare failed: %s\n", err); continue; }
        const double prep = secs(t, clk::now());
        t = clk::now();
        if (bf6_open_level(ctx, level.c_str(), "", 0, err, sizeof err) != 0) { std::printf("open_level failed: %s\n", err); continue; }
        const double open_level = secs(t, clk::now());
        t = clk::now();
        const int count = bf6_level_instances(ctx, level.c_str(), nullptr, 0);
        std::vector<bf6_instance> inst((size_t)std::max(0, count));
        bf6_level_instances(ctx, level.c_str(), inst.data(), count);
        const double placements = secs(t, clk::now());
        std::printf("prepare (mount+schema): %.2f s | open_level (index+walk): %.2f s | placements: %d in %.3f s\n",
                    prep, open_level, count, placements);
        {
            std::lock_guard<std::mutex> lock(clock.m);
            for (const auto& k : clock.order)
                std::printf("  stage %-40s %7.2f .. %7.2f s\n", k.c_str(), clock.spans[k].first, clock.spans[k].second);
        }

        for (int k = 0; k < 3 && k < count; ++k) {
            bf6_mesh* probe = bf6_read_mesh(ctx, inst[(size_t)k].res_name, 0);
            std::printf("  sample res '%s' bundle '%s' -> %s\n", inst[(size_t)k].res_name ? inst[(size_t)k].res_name : "(null)",
                        inst[(size_t)k].placing_bundle ? inst[(size_t)k].placing_bundle : "", probe ? "read" : "FAILED");
            if (probe) bf6_free(ctx, probe);
        }
        // Unique (mesh, bundle, variation) reads, as a precache would do them.
        std::set<std::string> unique_res;
        std::map<std::string, MeshJob> jobs_by_key;
        for (const auto& in : inst) {
            if (!in.res_name) continue;
            // Placements name the asset partition; its geometry is <stem>_mesh (as both engines read it).
            std::string res = in.res_name;
            if (res.size() >= 4 && res.compare(res.size() - 4, 4, ".ebx") == 0) res.resize(res.size() - 4);
            res += "_mesh";
            unique_res.insert(res);
            const std::string key = res + "|" + (in.placing_bundle ? in.placing_bundle : "") + "|" + (in.variation ? in.variation : "");
            jobs_by_key.emplace(key, MeshJob{ res, in.placing_bundle ? in.placing_bundle : "", in.variation ? in.variation : "" });
        }
        for (const auto& r : unique_res) ++seen_in_maps[r];
        std::vector<MeshJob> jobs;
        for (auto& kv : jobs_by_key) jobs.push_back(kv.second);
        std::printf("unique meshes: %zu | unique mesh+scope+variation reads: %zu (dedupe saves %.1f%% of reads vs per-placement)\n",
                    unique_res.size(), jobs.size(), count > 0 ? 100.0 * (1.0 - double(jobs.size()) / count) : 0.0);
        const size_t n = std::min(jobs.size(), (size_t)mesh_limit);

        ReadStats st;
        std::unordered_set<int> seen;
        t = clk::now();
        read_meshes(ctx, jobs, 0, n, st, seen);
        const double serial = secs(t, clk::now());
        std::printf("serial read of %zu: %.2f s total | mesh+material %.2f s | texture payloads %.2f s | ok %d failed %d | sections %d | verts %lld | textures %d (%.1f MB)\n",
                    n, serial, st.mesh_seconds, st.texture_seconds, st.meshes, st.failed, st.sections, st.vertices,
                    st.textures, st.texture_bytes / 1048576.0);
        std::printf("content digest (meshes + textures read): %016llx\n", st.digest);
        if (n > 0 && jobs.size() > n)
            std::printf("  extrapolated serial time for all %zu reads: %.0f s\n", jobs.size(), serial * jobs.size() / n);

        // Thread-safe texture decode on this one context: 1 thread, then many.
        if (!seen.empty()) {
            std::vector<std::string> names;
            for (int id : seen) { const char* nm = bf6_texture_name_at(ctx, id); if (nm && *nm) names.push_back(nm); }
            for (int threads : { 1, tex_threads }) {
                for (int cap : { 0, max_dim }) {
                    if (cap < 0) continue;
                    std::atomic<size_t> next{ 0 };
                    std::atomic<long long> bytes{ 0 };
                    std::atomic<int> ok{ 0 };
                    const auto t0 = clk::now();
                    std::vector<std::thread> pool;
                    for (int k = 0; k < threads; ++k)
                        pool.emplace_back([&] {
                            for (size_t i; (i = next++) < names.size();) {
                                bf6_texture* tx = bf6_texture_decode_res(ctx, names[i].c_str(), cap);
                                if (tx) { ++ok; bytes += tx->data_len; bf6_texture_decode_free(tx); }
                            }
                        });
                    for (auto& th : pool) th.join();
                    const double s = secs(t0, clk::now());
                    std::printf("texture decode x%zu on %d thread(s), max_dim %d: %.2f s (%.1f tex/s, %.0f MB/s, ok %d, %.1f MB)\n",
                                names.size(), threads, cap, s, s > 0 ? names.size() / s : 0.0,
                                s > 0 ? bytes.load() / 1048576.0 / s : 0.0, ok.load(), bytes.load() / 1048576.0);
                    if (cap == max_dim) break;
                }
            }
        }

        t = clk::now();
        bf6_terrain* terr = bf6_read_terrain(ctx, level.c_str());
        std::printf("terrain: %.2f s (%dx%d)\n", secs(t, clk::now()), terr ? terr->width : 0, terr ? terr->height : 0);
        if (terr) bf6_free(ctx, terr);

        if (contexts > 1 && n > 0) {
            // Price parallelism: extra contexts must mount and open the level again.
            std::vector<bf6_ctx*> extra;
            t = clk::now();
            for (int k = 1; k < contexts; ++k) {
                bf6_ctx* c = bf6_open(game.c_str(), err, sizeof err);
                if (!c || bf6_open_level(c, level.c_str(), "", 0, err, sizeof err) != 0) { std::printf("extra context failed: %s\n", err); if (c) bf6_close(c); break; }
                extra.push_back(c);
            }
            const double extra_open = secs(t, clk::now());
            std::printf("extra contexts: %zu opened in %.2f s (%.2f s each)\n", extra.size(), extra_open,
                        extra.empty() ? 0.0 : extra_open / extra.size());
            std::vector<bf6_ctx*> all = { ctx };
            all.insert(all.end(), extra.begin(), extra.end());
            // Same jobs again on fresh contexts (their caches are cold for these meshes).
            std::vector<ReadStats> per(all.size());
            std::vector<std::unordered_set<int>> seen_per(all.size());
            std::vector<std::thread> threads;
            const size_t chunk = (n + all.size() - 1) / all.size();
            t = clk::now();
            for (size_t k = 0; k < all.size(); ++k) {
                const size_t b = k * chunk, e = std::min(n, b + chunk);
                if (b >= e) break;
                if (k == 0) continue; // context 0 already holds these meshes warm; measure the fresh ones
                threads.emplace_back([&, k, b, e] { read_meshes(all[k], jobs, b, e, per[k], seen_per[k]); });
            }
            for (auto& th : threads) th.join();
            const double par = secs(t, clk::now());
            size_t done = 0;
            for (size_t k = 1; k < per.size(); ++k) done += per[k].meshes + per[k].failed;
            std::printf("parallel read on %zu fresh contexts: %zu reads in %.2f s (%.1f reads/s vs serial %.1f reads/s)\n",
                        extra.size(), done, par, par > 0 ? done / par : 0.0, serial > 0 ? n / serial : 0.0);
            for (bf6_ctx* c : extra) bf6_close(c);
        }
    }
    if (levels.size() > 1) {
        size_t total = 0, shared = 0;
        long long reads_if_per_map = 0;
        for (const auto& kv : seen_in_maps) { ++total; reads_if_per_map += kv.second; if (kv.second > 1) ++shared; }
        std::printf("\ncross-map: %zu distinct meshes over %zu maps; %zu appear in 2+ maps; a shared store reads %zu instead of %lld (%.1f%% fewer)\n",
                    total, levels.size(), shared, total, reads_if_per_map, reads_if_per_map ? 100.0 * (1.0 - double(total) / reads_if_per_map) : 0.0);
    }
    bf6_close(ctx);
    return 0;
}
