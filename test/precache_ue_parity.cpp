// Parity for the Unreal adapter's cache path, over one level:
//  - every placement scope (mesh, placing bundle, variation): the scoped record
//    decoded with bf6_mesh_decode_reference against bf6_read_mesh_scoped, with
//    texture bindings compared by NAME (record name table vs context names);
//  - every texture those bindings name: bf6_texture_decode_reference(max_dim)
//    against bf6_texture_at_max_dim on the context.
//   precache_ue_parity <game_dir> <cache_root> <level> [max_dim]
#include "bf6_core.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {
std::string mesh_res_for(const char* placement)
{
    std::string s = placement ? placement : "";
    if (s.size() >= 4 && s.compare(s.size() - 4, 4, ".ebx") == 0) s.resize(s.size() - 4);
    return s + "_mesh";
}

bool same_bytes(const void* a, const void* b, size_t n) { return (!a && !b) || (a && b && std::memcmp(a, b, n) == 0); }

// Section geometry, material values and binding names.
std::string compare_mesh(bf6_ctx* ctx, const bf6_mesh* live, const bf6_mesh* ref)
{
    if (live->section_count != ref->section_count) return "section count";
    if (live->material_count != ref->material_count) return "material count";
    for (int i = 0; i < live->section_count; ++i) {
        const bf6_section& a = live->sections[i];
        const bf6_section& b = ref->sections[i];
        if (a.vertex_count != b.vertex_count || a.index_count != b.index_count || a.material != b.material) return "section shape";
        const size_t v = (size_t)a.vertex_count;
        if (!same_bytes(a.positions, b.positions, v * 12)) return "positions";
        if (!same_bytes(a.normals, b.normals, v * 12)) return "normals";
        if (!same_bytes(a.uv0, b.uv0, v * 8)) return "uv0";
        if (!same_bytes(a.colors, b.colors, v * 4)) return "colors";
        if (!same_bytes(a.indices, b.indices, (size_t)a.index_count * 4)) return "indices";
        if (a.is_decal != b.is_decal || a.state_key != b.state_key) return "section flags";
    }
    for (int m = 0; m < live->material_count; ++m) {
        const bf6_material_desc& a = live->materials[m];
        const bf6_material_desc& b = ref->materials[m];
        if (a.texture_count != b.texture_count || a.shader_texture_count != b.shader_texture_count) return "binding counts";
        if (a.alpha_test != b.alpha_test || a.translucent != b.translucent || a.alpha_from_albedo != b.alpha_from_albedo
            || a.normal_is_nsm != b.normal_is_nsm || a.terrain_decal_receiver != b.terrain_decal_receiver
            || std::memcmp(a.base_color, b.base_color, sizeof a.base_color) != 0 || a.roughness != b.roughness)
            return "material values";
        for (int k = 0; k < a.texture_count; ++k) {
            if (a.textures[k].slot != b.textures[k].slot) return "binding slot";
            const char* an = a.textures[k].texture >= 0 ? bf6_texture_name_at(ctx, a.textures[k].texture) : "";
            const char* bn = b.textures[k].texture >= 0 ? bf6_mesh_reference_texture_name(ref, b.textures[k].texture) : "";
            if (std::strcmp(an ? an : "", bn ? bn : "") != 0) return "binding name";
        }
        for (int k = 0; k < a.shader_texture_count; ++k) {
            if (a.shader_textures[k].name32 != b.shader_textures[k].name32) return "shader binding id";
            const char* an = a.shader_textures[k].texture >= 0 ? bf6_texture_name_at(ctx, a.shader_textures[k].texture) : "";
            const char* bn = b.shader_textures[k].texture >= 0 ? bf6_mesh_reference_texture_name(ref, b.shader_textures[k].texture) : "";
            if (std::strcmp(an ? an : "", bn ? bn : "") != 0) return "shader binding name";
        }
    }
    return "";
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 4) { std::fprintf(stderr, "usage: precache_ue_parity <game_dir> <cache_root> <level> [max_dim]\n"); return 2; }
    const int max_dim = argc > 4 ? std::atoi(argv[4]) : 1024;
    char err[1024] = {};
    bf6_precache* cache = bf6_precache_open(argv[1], argv[2], err, sizeof err);
    if (!cache || !bf6_precache_map_ready(cache, argv[3])) { std::fprintf(stderr, "no cache for level: %s\n", err); return 1; }
    bf6_ctx* ctx = bf6_open(argv[1], err, sizeof err);
    if (!ctx) { std::fprintf(stderr, "open: %s\n", err); return 1; }
    if (bf6_open_level(ctx, argv[3], "", 0, err, sizeof err) != 0) { std::fprintf(stderr, "open level: %s\n", err); return 1; }
    const int count = bf6_level_instances(ctx, argv[3], nullptr, 0);
    std::vector<bf6_instance> inst((size_t)(count > 0 ? count : 0));
    if (count > 0) bf6_level_instances(ctx, argv[3], inst.data(), count);
    std::set<std::tuple<std::string, std::string, std::string>> scopes;
    for (const bf6_instance& in : inst)
        scopes.emplace(mesh_res_for(in.res_name), in.placing_bundle ? in.placing_bundle : "", in.variation ? in.variation : "");

    using clk = std::chrono::steady_clock;
    int checked = 0, no_record = 0, live_failed = 0, ref_failed = 0, mismatches = 0, printed = 0;
    std::set<std::string> texture_names;
    for (const auto& s : scopes) {
        const std::string& res = std::get<0>(s);
        const std::string& bundle = std::get<1>(s);
        const std::string& variation = std::get<2>(s);
        uint8_t* rec = nullptr;
        const int64_t len = bf6_precache_mesh_record_scoped(cache, res.c_str(), bundle.c_str(), variation.c_str(), &rec);
        bf6_mesh* live = bf6_read_mesh_scoped(ctx, res.c_str(), 0, bundle.empty() ? nullptr : bundle.c_str(),
                                              variation.empty() ? nullptr : variation.c_str());
        if (len <= 0) {
            if (live) ++no_record;   // the live read succeeds where the cache has nothing
            bf6_blob_free(rec);
            if (live) bf6_free(ctx, live);
            continue;
        }
        bf6_mesh* ref = bf6_mesh_decode_reference(argv[1], rec, len);
        bf6_blob_free(rec);
        ++checked;
        if (!live) { ++live_failed; if (ref) bf6_mesh_reference_free(ref); continue; }
        if (!ref) { ++ref_failed; bf6_free(ctx, live); continue; }
        const std::string bad = compare_mesh(ctx, live, ref);
        if (!bad.empty()) {
            ++mismatches;
            if (printed++ < 15) std::printf("MISMATCH %s | %s | %s: %s\n", res.c_str(), bundle.c_str(), variation.c_str(), bad.c_str());
        }
        for (int m = 0; m < ref->material_count; ++m) {
            for (int k = 0; k < ref->materials[m].texture_count; ++k)
                if (ref->materials[m].textures[k].texture >= 0)
                    texture_names.insert(bf6_mesh_reference_texture_name(ref, ref->materials[m].textures[k].texture));
            for (int k = 0; k < ref->materials[m].shader_texture_count; ++k)
                if (ref->materials[m].shader_textures[k].texture >= 0)
                    texture_names.insert(bf6_mesh_reference_texture_name(ref, ref->materials[m].shader_textures[k].texture));
        }
        bf6_mesh_reference_free(ref);
        bf6_free(ctx, live);
    }
    std::printf("mesh scopes %zu: compared %d, no record %d, live failed %d, record failed %d, mismatches %d\n",
                scopes.size(), checked, no_record, live_failed, ref_failed, mismatches);

    int tex_checked = 0, tex_no_record = 0, tex_mismatch = 0, tex_both_null = 0;
    double live_s = 0, ref_s = 0;
    for (const std::string& name : texture_names) {
        const int id = bf6_texture_id_by_name(ctx, name.c_str());
        uint8_t* rec = nullptr;
        const int64_t len = bf6_precache_texture_record(cache, name.c_str(), &rec);
        if (id < 0 || len <= 0) { ++tex_no_record; bf6_blob_free(rec); continue; }
        auto t0 = clk::now();
        const bf6_texture* live = bf6_texture_at_max_dim(ctx, id, max_dim);
        auto t1 = clk::now();
        bf6_texture* ref = bf6_texture_decode_reference(argv[1], rec, len, max_dim);
        auto t2 = clk::now();
        live_s += std::chrono::duration<double>(t1 - t0).count();
        ref_s += std::chrono::duration<double>(t2 - t1).count();
        bf6_blob_free(rec);
        ++tex_checked;
        const bool live_ok = live && live->data && live->data_len > 0;
        const bool ref_ok = ref && ref->data && ref->data_len > 0;
        bool same = live_ok == ref_ok;
        if (live_ok && ref_ok)
            same = live->width == ref->width && live->height == ref->height && live->format == ref->format
                && live->mip_count == ref->mip_count && live->srgb == ref->srgb && live->data_len == ref->data_len
                && std::memcmp(live->data, ref->data, (size_t)live->data_len) == 0;
        if (!live_ok && !ref_ok) ++tex_both_null;
        if (!same) {
            ++tex_mismatch;
            if (printed++ < 30) std::printf("TEXTURE MISMATCH %s\n", name.c_str());
        }
        if (ref) bf6_texture_decode_free(ref);
        if (live) bf6_release_texture_payload(ctx, id, max_dim);
    }
    std::printf("textures %zu: compared %d (both unreadable %d), no record %d, mismatches %d; live %.2f s, record %.2f s\n",
                texture_names.size(), tex_checked, tex_both_null, tex_no_record, tex_mismatch, live_s, ref_s);
    const bool ok = checked > 0 && mismatches == 0 && ref_failed == 0 && tex_checked > 0 && tex_mismatch == 0;
    std::printf("RESULT %s\n", ok ? "PASS" : "FAIL");
    bf6_close(ctx);
    bf6_precache_close(cache);
    return ok ? 0 : 1;
}
