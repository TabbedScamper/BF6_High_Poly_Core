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
#include <cstdlib>
#include <stdio.h>
#include <cstring>
#include <functional>
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

// Everything a consumer can read from a mesh, with textures by resource name.
static unsigned long long mesh_digest(const bf6_mesh* m,
                                      const std::function<const char*(int)>& texture_name,
                                      const std::function<int(int, bf6_surface_desc*)>& surface)
{
    unsigned long long h = 1469598103934665603ull;
    auto fold_i = [&](int32_t v) { unsigned char b[4]; std::memcpy(b, &v, 4); for (unsigned char x : b) h = (h ^ x) * 1099511628211ull; };
    auto fold_p = [&](const void* p, size_t n) { const auto* c = (const unsigned char*)p; if (!c) { fold_i(-7); return; } for (size_t i = 0; i < n; ++i) h = (h ^ c[i]) * 1099511628211ull; };
    auto fold_s = [&](const char* s) { fold_p(s ? s : "", s ? std::strlen(s) : 0); fold_i(0x5eed); };
    fold_i(m->section_count); fold_i(m->material_count); fold_i(m->mesh_type); fold_i(m->bone_count); fold_i(m->lod_count);
    fold_p(m->aabb_min, sizeof m->aabb_min); fold_p(m->aabb_max, sizeof m->aabb_max);
    for (int i = 0; i < m->section_count; ++i) {
        const bf6_section& s = m->sections[i];
        const size_t vc = (size_t)std::max(0, s.vertex_count);
        fold_i(s.vertex_count); fold_i(s.index_count); fold_i(s.material); fold_i(s.bone_list_count);
        fold_i(s.skin_influences); fold_i(s.is_decal); fold_p(&s.state_key, 8);
        fold_p(s.positions, vc * 12); fold_p(s.normals, vc * 12); fold_p(s.tangents, vc * 16);
        fold_p(s.uv0, vc * 8); fold_p(s.uv1, vc * 8); fold_p(s.colors, vc * 4);
        fold_p(s.indices, (size_t)std::max(0, s.index_count) * 4); fold_p(s.bones, vc * 2);
        fold_p(s.bone_list, (size_t)std::max(0, s.bone_list_count) * 2);
        fold_p(s.skin_bones, vc * (size_t)std::max(0, s.skin_influences) * 2);
        fold_p(s.skin_weights, vc * (size_t)std::max(0, s.skin_influences) * 4);
        bf6_surface_desc sd{}; sd.struct_size = sizeof sd;
        if (surface(i, &sd)) {
            fold_i(sd.profile); fold_i(sd.complete); fold_p(sd.material, sizeof sd.material);
            for (int k = 0; k < 4; ++k) fold_s(texture_name(sd.textures[k]));
            for (int k = 0; k < 5; ++k) fold_p(sd.uv[k], vc * 8);
            fold_p(sd.color0, vc * 16); fold_p(sd.submaterial, vc);
        } else fold_i(-1);
    }
    for (int k = 0; k < m->material_count; ++k) {
        bf6_material_desc md = m->materials[k];
        const bf6_tex_binding* tb = md.textures; const int tn = md.texture_count;
        const bf6_shader_tex_binding* sb = md.shader_textures; const int sn = md.shader_texture_count;
        md.textures = nullptr; md.shader_textures = nullptr;
        fold_p(&md, sizeof md);
        for (int b = 0; b < tn; ++b) { fold_i((int32_t)tb[b].slot); fold_s(texture_name(tb[b].texture)); }
        for (int b = 0; b < sn; ++b) { fold_i((int32_t)sb[b].name32); fold_s(texture_name(sb[b].texture)); }
    }
    return h;
}

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
    if (const char* limit = std::getenv("BF6_BENCH_MAXSTDIO")) std::printf("maxstdio set: %d\n", _setmaxstdio(std::atoi(limit)));
    else std::printf("maxstdio: %d (default)\n", _getmaxstdio());
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

        // Mesh references: live read vs reference rebuilt from the game files.
        if (n > 0) {
            std::vector<std::string> records(n);
            std::vector<unsigned long long> live(n, 0), fromref(n, 0);
            long long record_bytes = 0;
            size_t live_ok = 0, records_ok = 0;
            auto t0 = clk::now();
            for (size_t i = 0; i < n; ++i) {
                const MeshJob& job = jobs[i];
                const char* bundle = job.bundle.empty() ? nullptr : job.bundle.c_str();
                const char* variation = job.variation.empty() ? nullptr : job.variation.c_str();
                if (bf6_mesh* m = bf6_read_mesh_scoped(ctx, job.res.c_str(), 0, bundle, variation)) {
                    live[i] = mesh_digest(m, [&](int id) { return id >= 0 ? bf6_texture_name_at(ctx, id) : nullptr; },
                                          [&](int s, bf6_surface_desc* out) { return bf6_mesh_surface(ctx, m, s, out); });
                    bf6_free(ctx, m);
                    ++live_ok;
                }
                uint8_t* blob = nullptr;
                const int64_t len = bf6_mesh_reference(ctx, job.res.c_str(), 0, bundle, variation, &blob);
                if (len > 0 && blob) { records[i].assign((const char*)blob, (size_t)len); record_bytes += len; ++records_ok; }
                bf6_blob_free(blob);
            }
            const double record_s = secs(t0, clk::now());
            std::atomic<size_t> next{ 0 };
            std::atomic<long long> vertices{ 0 };
            t0 = clk::now();
            std::vector<std::thread> pool;
            for (int k = 0; k < tex_threads; ++k)
                pool.emplace_back([&] {
                    for (size_t i; (i = next++) < n;) {
                        if (records[i].empty()) continue;
                        bf6_mesh* m = bf6_mesh_decode_reference(game.c_str(), (const uint8_t*)records[i].data(), (int64_t)records[i].size());
                        if (!m) continue;
                        fromref[i] = mesh_digest(m, [&](int id) { return bf6_mesh_reference_texture_name(m, id); },
                                                 [&](int s, bf6_surface_desc* out) { return bf6_mesh_reference_surface(m, s, out); });
                        for (int s = 0; s < m->section_count; ++s) vertices += m->sections[s].vertex_count;
                        bf6_mesh_reference_free(m);
                    }
                });
            for (auto& th : pool) th.join();
            const double decode_s = secs(t0, clk::now());
            size_t same = 0, shown = 0;
            for (size_t i = 0; i < n; ++i) {
                if (live[i] == fromref[i]) { ++same; continue; }
                if (shown++ < 5) std::printf("  mesh mismatch %s (record %zu bytes, live %s)\n", jobs[i].res.c_str(), records[i].size(), live[i] ? "read" : "failed");
            }
            std::printf("mesh references: %zu records (%zu live reads) in %.2f s serial incl. live reads, %.1f MB (%.0f bytes each); "
                        "decode on %d threads %.2f s (%.0f meshes/s, %lld vertices); identical: %zu / %zu\n",
                        records_ok, live_ok, record_s, record_bytes / 1048576.0, records_ok ? double(record_bytes) / records_ok : 0.0,
                        tex_threads, decode_s, decode_s > 0 ? records_ok / decode_s : 0.0, vertices.load(), same, n);
        }

        // Thread-safe texture decode on this one context: 1 thread, then many.
        if (!seen.empty()) {
            std::vector<std::string> names;
            for (int id : seen) { const char* nm = bf6_texture_name_at(ctx, id); if (nm && *nm) names.push_back(nm); }
            std::sort(names.begin(), names.end());

            // References: record where each texture lives, then decode from the record
            // with no context. Digests must equal the live decode of the same textures.
            {
                std::vector<std::string> records(names.size());
                std::atomic<size_t> next{ 0 };
                std::atomic<long long> record_bytes{ 0 };
                auto t0 = clk::now();
                std::vector<std::thread> pool;
                for (int k = 0; k < tex_threads; ++k)
                    pool.emplace_back([&] {
                        for (size_t i; (i = next++) < names.size();) {
                            const int64_t need = bf6_texture_reference(ctx, names[i].c_str(), nullptr, 0);
                            if (need <= 0) continue;
                            records[i].resize((size_t)need);
                            bf6_texture_reference(ctx, names[i].c_str(), (uint8_t*)records[i].data(), need);
                            record_bytes += need;
                        }
                    });
                for (auto& th : pool) th.join();
                const double make_s = secs(t0, clk::now());
                size_t made = 0;
                for (const auto& r : records) if (!r.empty()) ++made;
                std::printf("texture references: %zu of %zu recorded in %.2f s, %.1f KB total (%.0f bytes each)\n",
                            made, names.size(), make_s, record_bytes.load() / 1024.0,
                            made ? double(record_bytes.load()) / made : 0.0);
                for (int cap : { 0, max_dim }) {
                    if (cap < 0) continue;
                    std::vector<unsigned long long> live(names.size(), 0), fromref(names.size(), 0);
                    std::atomic<size_t> i1{ 0 }, i2{ 0 };
                    std::vector<std::thread> p1, p2;
                    for (int k = 0; k < tex_threads; ++k)
                        p1.emplace_back([&] {
                            for (size_t i; (i = i1++) < names.size();) {
                                unsigned long long h = 1469598103934665603ull;
                                if (bf6_texture* tx = bf6_texture_decode_res(ctx, names[i].c_str(), cap)) {
                                    const int32_t dims[4] = { tx->width, tx->height, tx->mip_count, (int32_t)tx->format };
                                    fold(h, (const unsigned char*)dims, sizeof dims); fold(h, tx->data, (size_t)tx->data_len);
                                    bf6_texture_decode_free(tx);
                                }
                                live[i] = h;
                            }
                        });
                    for (auto& th : p1) th.join();
                    t0 = clk::now();
                    std::atomic<long long> bytes{ 0 };
                    for (int k = 0; k < tex_threads; ++k)
                        p2.emplace_back([&] {
                            for (size_t i; (i = i2++) < names.size();) {
                                unsigned long long h = 1469598103934665603ull;
                                if (!records[i].empty())
                                    if (bf6_texture* tx = bf6_texture_decode_reference(game.c_str(), (const uint8_t*)records[i].data(), (int64_t)records[i].size(), cap)) {
                                        const int32_t dims[4] = { tx->width, tx->height, tx->mip_count, (int32_t)tx->format };
                                        fold(h, (const unsigned char*)dims, sizeof dims); fold(h, tx->data, (size_t)tx->data_len);
                                        bytes += tx->data_len;
                                        bf6_texture_decode_free(tx);
                                    }
                                fromref[i] = h;
                            }
                        });
                    for (auto& th : p2) th.join();
                    const double ref_s = secs(t0, clk::now());
                    // Determinism check: repeat both parallel passes and count self-mismatches.
                    {
                        auto pass = [&](bool reference) {
                            std::vector<unsigned long long> out(names.size(), 0);
                            std::atomic<size_t> idx{ 0 };
                            std::vector<std::thread> ps;
                            for (int k = 0; k < tex_threads; ++k)
                                ps.emplace_back([&] {
                                    for (size_t i; (i = idx++) < names.size();) {
                                        unsigned long long h = 1469598103934665603ull;
                                        bf6_texture* tx = reference
                                            ? (records[i].empty() ? nullptr : bf6_texture_decode_reference(game.c_str(), (const uint8_t*)records[i].data(), (int64_t)records[i].size(), cap))
                                            : bf6_texture_decode_res(ctx, names[i].c_str(), cap);
                                        if (tx) {
                                            const int32_t dims[4] = { tx->width, tx->height, tx->mip_count, (int32_t)tx->format };
                                            fold(h, (const unsigned char*)dims, sizeof dims); fold(h, tx->data, (size_t)tx->data_len);
                                            bf6_texture_decode_free(tx);
                                        }
                                        out[i] = h;
                                    }
                                });
                            for (auto& th : ps) th.join();
                            return out;
                        };
                        const auto live2 = pass(false);
                        const auto ref2 = pass(true);
                        size_t lv = 0, rf = 0;
                        for (size_t i = 0; i < names.size(); ++i) { if (live[i] != live2[i]) ++lv; if (fromref[i] != ref2[i]) ++rf; }
                        std::printf("determinism (max_dim %d): live pass vs repeat differs on %zu, reference pass vs repeat differs on %zu\n", cap, lv, rf);
                    }
                    size_t same = 0, shown = 0;
                    const unsigned long long empty_hash = 1469598103934665603ull;
                    for (size_t i = 0; i < names.size(); ++i) {
                        if (live[i] == fromref[i]) { ++same; continue; }
                        if (shown++ < 6) {
                            bf6_texture* a = bf6_texture_decode_res(ctx, names[i].c_str(), cap);
                            bf6_texture* b = records[i].empty() ? nullptr : bf6_texture_decode_reference(game.c_str(), (const uint8_t*)records[i].data(), (int64_t)records[i].size(), cap);
                            std::printf("  mismatch %s: live %s %dx%d mips %d %d bytes | ref %s %dx%d mips %d %d bytes | record %zu bytes | repeat-live-equal %d\n",
                                names[i].c_str(), a ? "ok" : (live[i] == empty_hash ? "FAILED" : "failed-now"), a ? a->width : 0, a ? a->height : 0, a ? a->mip_count : 0, a ? a->data_len : 0,
                                b ? "ok" : "FAILED", b ? b->width : 0, b ? b->height : 0, b ? b->mip_count : 0, b ? b->data_len : 0,
                                records[i].size(), (int)(a && b && a->data_len == b->data_len && std::memcmp(a->data, b->data, (size_t)a->data_len) == 0));
                            if (a) bf6_texture_decode_free(a);
                            if (b) bf6_texture_decode_free(b);
                        }
                    }
                    std::printf("reference decode on %d threads, max_dim %d: %.2f s (%.0f MB/s); identical to live decode: %zu / %zu\n",
                                tex_threads, cap, ref_s, ref_s > 0 ? bytes.load() / 1048576.0 / ref_s : 0.0, same, names.size());
                }
            }
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
        {
            uint8_t* rec = nullptr;
            const int64_t len = bf6_terrain_reference(ctx, level.c_str(), 0, &rec);
            const auto td = clk::now();
            bf6_terrain* fromref = len > 0 ? bf6_terrain_decode_reference(game.c_str(), rec, len) : nullptr;
            const double decode_s = secs(td, clk::now());
            const bool same = terr && fromref && terr->width == fromref->width && terr->height == fromref->height &&
                std::memcmp(terr->heights, fromref->heights, (size_t)terr->width * terr->height * 2) == 0 &&
                std::memcmp(terr->world_min, fromref->world_min, sizeof terr->world_min) == 0 &&
                std::memcmp(terr->world_max, fromref->world_max, sizeof terr->world_max) == 0 &&
                terr->height_scale == fromref->height_scale;
            std::printf("terrain reference: %lld bytes; decode %.2f s; identical heights and bounds: %s\n",
                        (long long)len, decode_s, same ? "yes" : "NO");
            if (fromref) bf6_terrain_reference_free(fromref);
            bf6_blob_free(rec);
        }
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
