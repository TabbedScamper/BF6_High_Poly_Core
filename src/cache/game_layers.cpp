#include "game_layers.h"

#include "thread_pool.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace bf6::cache {

namespace {

using clk = std::chrono::steady_clock;
double since(clk::time_point t) { return std::chrono::duration<double>(clk::now() - t).count(); }

// Little-endian append-only record writer.
struct Out {
    std::string b;
    void pod(const void* p, std::size_t n) { b.append((const char*)p, n); }
    void u32(std::uint32_t v) { pod(&v, 4); }
    void i32(std::int32_t v) { pod(&v, 4); }
    void u64(std::uint64_t v) { pod(&v, 8); }
    void str(const char* s) { const std::uint32_t n = s ? (std::uint32_t)std::strlen(s) : 0; u32(n); if (n) pod(s, n); }
    void str(const std::string& s) { u32((std::uint32_t)s.size()); pod(s.data(), s.size()); }
    void arr(const void* p, std::size_t count, std::size_t elem)
    {
        u32(p ? (std::uint32_t)count : 0);
        if (p && count) pod(p, count * elem);
    }
};

std::string hex16(std::uint64_t v)
{
    static const char* d = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i) { s[(size_t)i] = d[v & 15]; v >>= 4; }
    return s;
}

std::string mesh_res_for(const char* placement)
{
    std::string s = placement ? placement : "";
    if (s.size() >= 4 && s.compare(s.size() - 4, 4, ".ebx") == 0) s.resize(s.size() - 4);
    return s + "_mesh";
}

// Mesh record: every vertex stream and material the core returns, with texture
// references stored by resource name (texture ids are per context).
bool serialise_mesh(bf6_ctx* ctx, const bf6_mesh* m, std::string& out, std::vector<std::string>& textures)
{
    Out o;
    o.u32(0x48534D42); // "BMSH"
    o.u32(kMeshesVersion);
    o.i32(m->mesh_type); o.i32(m->bone_count); o.i32(m->lod_count);
    o.pod(m->aabb_min, sizeof m->aabb_min); o.pod(m->aabb_max, sizeof m->aabb_max);
    o.u32((std::uint32_t)m->section_count);
    for (int i = 0; i < m->section_count; ++i) {
        const bf6_section& s = m->sections[i];
        const std::size_t vc = (std::size_t)std::max(0, s.vertex_count);
        o.i32(s.vertex_count); o.i32(s.index_count); o.i32(s.material);
        o.i32(s.bone_list_count); o.i32(s.skin_influences); o.i32(s.is_decal); o.u64(s.state_key);
        o.arr(s.positions, vc * 3, sizeof(float));
        o.arr(s.normals, vc * 3, sizeof(float));
        o.arr(s.tangents, vc * 4, sizeof(float));
        o.arr(s.uv0, vc * 2, sizeof(float));
        o.arr(s.uv1, vc * 2, sizeof(float));
        o.arr(s.colors, vc, sizeof(std::uint32_t));
        o.arr(s.indices, (std::size_t)std::max(0, s.index_count), sizeof(std::uint32_t));
        o.arr(s.bones, vc, sizeof(std::uint16_t));
        o.arr(s.bone_list, (std::size_t)std::max(0, s.bone_list_count), sizeof(std::uint16_t));
        o.arr(s.skin_bones, vc * (std::size_t)std::max(0, s.skin_influences), sizeof(std::uint16_t));
        o.arr(s.skin_weights, vc * (std::size_t)std::max(0, s.skin_influences), sizeof(float));
    }
    auto tex_name = [&](int id) -> std::string {
        const char* n = id >= 0 ? bf6_texture_name_at(ctx, id) : nullptr;
        if (!n || !*n) return {};
        textures.emplace_back(n);
        return n;
    };
    o.u32((std::uint32_t)m->material_count);
    for (int k = 0; k < m->material_count; ++k) {
        bf6_material_desc md = m->materials[k];
        const bf6_tex_binding* tb = md.textures;
        const int tbn = md.texture_count;
        const bf6_shader_tex_binding* sb = md.shader_textures;
        const int sbn = md.shader_texture_count;
        md.textures = nullptr; md.shader_textures = nullptr;
        o.pod(&md, sizeof md);
        o.u32((std::uint32_t)std::max(0, tbn));
        for (int b = 0; b < tbn; ++b) { o.i32((std::int32_t)tb[b].slot); o.str(tex_name(tb[b].texture)); }
        o.u32((std::uint32_t)std::max(0, sbn));
        for (int b = 0; b < sbn; ++b) { o.u32(sb[b].name32); o.str(tex_name(sb[b].texture)); }
    }
    out.swap(o.b);
    return true;
}

}  // namespace

std::string mesh_key(const std::string& res, const std::string& bundle, const std::string& variation)
{
    // Readable prefix for diagnostics, hashed suffix for uniqueness.
    const Digest d = digest_of((res + "\x1f" + bundle + "\x1f" + variation).data(), res.size() + bundle.size() + variation.size() + 2);
    return "mesh/" + hex16(d.a) + hex16(d.b);
}

std::string texture_key(const std::string& res, int max_dim)
{
    return "tex/" + std::to_string(max_dim) + "/" + res;
}

bool build_game_level(bf6_ctx* ctx, const std::string& level, const fs::path& map_dir,
                      ContentStore& shared, const GameBuildOptions& options,
                      const Report& report, const std::atomic<bool>& cancel,
                      GameLayerResult& r, std::string& err)
{
    char e[1024] = {};
    auto t = clk::now();
    report("placements", 0.0, "opening " + level);
    if (bf6_open_level(ctx, level.c_str(), "", 0, e, sizeof e) != 0) { err = std::string("open level: ") + e; return false; }
    const int count = bf6_level_instances(ctx, level.c_str(), nullptr, 0);
    std::vector<bf6_instance> inst((std::size_t)std::max(0, count));
    if (count > 0) bf6_level_instances(ctx, level.c_str(), inst.data(), count);
    r.seconds_open = since(t);
    r.placements = count;

    // ---- placements: one row per placement, meshes referenced by shared key
    struct Job { std::string res, bundle, variation, key; };
    std::vector<Job> jobs;
    std::unordered_map<std::string, std::size_t> job_of;
    {
        Out o;
        o.u32(0x4C504642); // "BFPL"
        o.u32(kPlacementsVersion);
        o.u32((std::uint32_t)inst.size());
        for (const bf6_instance& in : inst) {
            const std::string res = mesh_res_for(in.res_name);
            const std::string bundle = in.placing_bundle ? in.placing_bundle : "";
            const std::string variation = in.variation ? in.variation : "";
            const std::string key = mesh_key(res, bundle, variation);
            if (!job_of.count(key)) { job_of.emplace(key, jobs.size()); jobs.push_back({ res, bundle, variation, key }); }
            o.str(key);
            o.str(in.res_name);
            o.pod(in.xform, sizeof in.xform);
            o.str(bundle);
            o.str(variation);
            o.str(in.source);
        }
        PackWriter pw;
        Digest d;
        auto tw = clk::now();
        if (!pw.begin(map_dir, "placements", err) || !pw.add("rows", o.b, err) || !pw.finish(r.placement_bytes, d, err)) return false;
        r.seconds_write += since(tw);
        r.placements_digest = d.hex();
    }
    report("placements", 1.0, std::to_string(count) + " placements");

    // ---- meshes: serial reads (material resolution uses the context), each
    // unique (mesh, bundle, variation) stored once per install.
    t = clk::now();
    std::set<std::string> texture_names;
    std::size_t done = 0;
    for (const Job& job : jobs) {
        if (cancel.load()) { err = "cancelled"; return false; }
        ++done;
        if (shared.contains(job.key)) continue;
        ++r.mesh_reads;
        bf6_mesh* m = bf6_read_mesh_scoped(ctx, job.res.c_str(), 0,
            job.bundle.empty() ? nullptr : job.bundle.c_str(),
            job.variation.empty() ? nullptr : job.variation.c_str());
        if (!m) { ++r.meshes_failed; continue; }
        std::string blob;
        std::vector<std::string> textures;
        serialise_mesh(ctx, m, blob, textures);
        bf6_free(ctx, m);
        for (auto& name : textures) texture_names.insert(std::move(name));
        bool added = false;
        auto tw = clk::now();
        if (!shared.put(job.key, blob, added, err)) return false;
        r.seconds_write += since(tw);
        if (added) { ++r.meshes_new; r.mesh_bytes += blob.size(); }
        if ((done & 255) == 0)
            report("meshes", double(done) / jobs.size(), "meshes: " + std::to_string(done) + " / " + std::to_string(jobs.size()));
    }
    r.seconds_meshes = since(t);
    report("meshes", 1.0, std::to_string(r.meshes_new) + " new meshes");

    // ---- textures: decoded on many threads against this one context
    t = clk::now();
    std::vector<std::string> pending;
    for (const std::string& name : texture_names)
        if (!shared.contains(texture_key(name, options.texture_max_dim))) pending.push_back(name);
    {
        std::atomic<std::size_t> finished{ 0 };
        std::atomic<bool> failed{ false };
        std::mutex err_mutex;
        std::string first_err;
        std::atomic<std::uint64_t> bytes{ 0 };
        std::atomic<int> added_count{ 0 }, bad{ 0 };
        std::atomic<double> write_seconds{ 0 };
        ThreadPool pool(options.threads);
        for (const std::string& name : pending) {
            pool.submit([&, name] {
                if (cancel.load() || failed.load()) return;
                bf6_texture* tx = bf6_texture_decode_res(ctx, name.c_str(), options.texture_max_dim);
                if (!tx) { ++bad; ++finished; return; }
                Out o;
                o.u32(0x58455442); // "BTEX"
                o.u32(kTexturesVersion);
                o.i32(tx->width); o.i32(tx->height); o.i32(tx->mip_count);
                o.i32((std::int32_t)tx->format); o.i32(tx->srgb);
                o.u32((std::uint32_t)tx->data_len);
                o.pod(tx->data, (std::size_t)tx->data_len);
                bf6_texture_decode_free(tx);
                bool added = false;
                std::string e2;
                auto tw = clk::now();
                if (!shared.put(texture_key(name, options.texture_max_dim), o.b, added, e2)) {
                    failed = true;
                    std::lock_guard<std::mutex> lock(err_mutex);
                    if (first_err.empty()) first_err = e2;
                }
                double w = since(tw), cur = write_seconds.load();
                while (!write_seconds.compare_exchange_weak(cur, cur + w)) {}
                if (added) { ++added_count; bytes += o.b.size(); }
                const std::size_t f = ++finished;
                if ((f & 63) == 0)
                    report("textures", double(f) / std::max<std::size_t>(1, pending.size()),
                           "textures: " + std::to_string(f) + " / " + std::to_string(pending.size()));
            });
        }
        pool.wait();
        if (failed) { err = first_err; return false; }
        if (cancel.load()) { err = "cancelled"; return false; }
        r.textures_new = added_count;
        r.textures_failed = bad;
        r.texture_bytes = bytes;
        r.seconds_write += write_seconds.load();
    }
    r.seconds_textures = since(t);
    report("textures", 1.0, std::to_string(r.textures_new) + " new textures");

    // ---- terrain heightfield
    t = clk::now();
    {
        Out o;
        o.u32(0x52524554); // "TERR"
        o.u32(kTerrainVersion);
        bf6_terrain* terr = bf6_read_terrain(ctx, level.c_str());
        o.i32(terr ? 1 : 0);
        if (terr) {
            o.i32(terr->width); o.i32(terr->height);
            o.pod(terr->world_min, sizeof terr->world_min); o.pod(terr->world_max, sizeof terr->world_max);
            o.pod(&terr->height_scale, sizeof terr->height_scale);
            const char* splat = bf6_texture_name_at(ctx, terr->splat_texture);
            const char* color = bf6_texture_name_at(ctx, terr->color_texture);
            o.str(terr->splat_texture >= 0 ? splat : "");
            o.str(terr->color_texture >= 0 ? color : "");
            o.arr(terr->heights, (std::size_t)terr->width * (std::size_t)terr->height, sizeof(std::uint16_t));
            bf6_free(ctx, terr);
        }
        PackWriter pw;
        Digest d;
        auto tw = clk::now();
        if (!pw.begin(map_dir, "terrain", err) || !pw.add("heightfield", o.b, err) || !pw.finish(r.terrain_bytes, d, err)) return false;
        r.seconds_write += since(tw);
        r.terrain_digest = d.hex();
    }
    r.seconds_terrain = since(t);
    report("terrain", 1.0, "terrain");
    return true;
}

}  // namespace bf6::cache
