/* mesh_read_test - exercise the PUBLIC ABI: bf6_open + bf6_read_mesh. */
#include <cstdio>
#include <cfloat>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <vector>
#include "bf6_core.h"
#include "terraincomposite.h"
int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: mesh_read_test <game> <res> [placing_bundle] [variation]\n");
        return 2;
    }
    char err[256] = {0};
    bf6_ctx* ctx = bf6_open(argv[1], err, sizeof(err));
    if (!ctx) { std::printf("open: %s\n", err); return 1; }
    // Mesh metadata can be shared while its LOD chunk lives in a level/update
    // package. Mount all for this diagnostic so NULL means decode, not scope.
    if (!bf6_mount_all(ctx, 1, err, sizeof(err)))
    { std::printf("mount: %s\n", err); bf6_close(ctx); return 1; }
    // A level placement's shader state key is bundle-scoped.  Keep the old
    // resource-only read as the explicit control, and allow the exact placing
    // bundle to exercise the same public path used by the Unreal add-on.
    const bool armory_index = std::getenv("BF6_MESH_ARMORY_INDEX") != nullptr;
    bf6_mesh* m = argc > 3
        ? (armory_index
            ? bf6_read_armory_mesh_scoped(
                ctx, argv[2], 0, argv[3], argc > 4 ? argv[4] : nullptr)
            : bf6_read_mesh_scoped(
                ctx, argv[2], 0, argv[3], argc > 4 ? argv[4] : nullptr))
        : (armory_index
            ? bf6_read_armory_mesh_scoped(ctx, argv[2], 0, nullptr, nullptr)
            : bf6_read_mesh(ctx, argv[2], 0));
    if (!m) { std::printf("read_mesh returned null\n"); bf6_close(ctx); return 1; }
    std::printf("MESH type=%d bones=%d lods=%d sections=%d aabb=[%.2f %.2f %.2f]..[%.2f %.2f %.2f]\n",
        m->mesh_type, m->bone_count, m->lod_count, m->section_count, m->aabb_min[0], m->aabb_min[1], m->aabb_min[2],
        m->aabb_max[0], m->aabb_max[1], m->aabb_max[2]);
    for (int i = 0; i < m->section_count; i++) {
        const bf6_section& s = m->sections[i];
        float pLo[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
        float pHi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (int v=0; v<s.vertex_count; v++) for (int k=0; k<3; k++) {
            if (s.positions[v*3+k] < pLo[k]) pLo[k] = s.positions[v*3+k];
            if (s.positions[v*3+k] > pHi[k]) pHi[k] = s.positions[v*3+k];
        }
        std::printf("  S%d statekey=%016llx verts=%d tris=%d normals=%s uv0=%s uv1=%s decal=%d receiver=%d translucent=%d alpha_test=%d alpha_albedo=%d two_sided=%d rgba=%.3f/%.3f/%.3f/%.3f", i,
            (unsigned long long)s.state_key, s.vertex_count, s.index_count / 3,
            s.normals ? "yes" : "no", s.uv0 ? "yes" : "no",
            s.uv1 ? "yes" : "no", s.is_decal,
            m->materials[i].terrain_decal_receiver,
            m->materials[i].translucent, m->materials[i].alpha_test,
            m->materials[i].alpha_from_albedo,
            m->materials[i].two_sided,
            m->materials[i].base_color[0], m->materials[i].base_color[1],
            m->materials[i].base_color[2], m->materials[i].base_color[3]);
        const bf6_material_desc& md = m->materials[i];
        std::printf(" textures=");
        for (int b = 0; b < md.texture_count; b++) {
            const int ti = md.textures[b].texture;
            std::printf("%s%d:%s", b ? "," : "", (int)md.textures[b].slot,
                        ti >= 0 ? bf6_texture_name_at(ctx, ti) : "<none>");
        }
        std::printf(" shader_textures=");
        for (int b = 0; b < md.shader_texture_count; b++) {
            const bf6_shader_tex_binding& tb = md.shader_textures[b];
            std::printf("%s%08x:%s", b ? "," : "", tb.name32,
                        tb.texture >= 0 ? bf6_texture_name_at(ctx, tb.texture) : "<none>");
        }
        std::printf(" aabb=[%.2f %.2f %.2f]..[%.2f %.2f %.2f]\n",
                    pLo[0],pLo[1],pLo[2],pHi[0],pHi[1],pHi[2]);
        if (std::getenv("BF6_MESH_SHADER_STATS"))
        for (int b = 0; b < md.shader_texture_count; ++b) {
            const bf6_shader_tex_binding& sb = md.shader_textures[b];
            const bf6_texture* t = sb.texture >= 0 ? bf6_texture_at(ctx, sb.texture) : nullptr;
            if (!t) continue;
            int dxgi = 0;
            if (t->format == BF6_FMT_BC1) dxgi = t->srgb ? 72 : 71;
            else if (t->format == BF6_FMT_BC3) dxgi = t->srgb ? 78 : 77;
            else if (t->format == BF6_FMT_BC4) dxgi = 80;
            else if (t->format == BF6_FMT_BC5) dxgi = 83;
            else if (t->format == BF6_FMT_BC7) dxgi = t->srgb ? 99 : 98;
            std::vector<uint8_t> rgba; std::string decodeError;
            if (!dxgi || !bf6::bcn_to_rgba8(t->data, (size_t)t->data_len,
                    t->width, t->height, dxgi, rgba, decodeError)) continue;
            unsigned mn = 255, mx = 0; uint64_t sum = 0, high = 0;
            for (size_t p = 0; p < rgba.size(); p += 4) {
                mn = (std::min)(mn, (unsigned)rgba[p]);
                mx = (std::max)(mx, (unsigned)rgba[p]);
                sum += rgba[p]; high += rgba[p] >= 128;
            }
            const size_t pixels = rgba.size() / 4;
            auto sample = [&](int x, int y) { return rgba[((size_t)y * t->width + x) * 4]; };
            std::printf("      shader %08x %dx%d R=%u..%u mean=%.1f high=%.1f%% samples=%u/%u/%u/%u/%u\n",
                sb.name32, t->width, t->height, mn, mx,
                pixels ? (double)sum / pixels : 0.0,
                pixels ? 100.0 * (double)high / pixels : 0.0,
                sample(t->width/4,t->height/4), sample(3*t->width/4,t->height/4),
                sample(t->width/2,t->height/2), sample(t->width/4,3*t->height/4),
                sample(3*t->width/4,3*t->height/4));
        }
        for (int channel = 0; channel < 2; channel++) {
            const float* uv = channel ? s.uv1 : s.uv0;
            if (!uv) continue;
            float loU=FLT_MAX, loV=FLT_MAX, hiU=-FLT_MAX, hiV=-FLT_MAX;
            for (int v=0; v<s.vertex_count; v++) {
                if (uv[v*2] < loU) loU=uv[v*2]; if (uv[v*2] > hiU) hiU=uv[v*2];
                if (uv[v*2+1] < loV) loV=uv[v*2+1]; if (uv[v*2+1] > hiV) hiV=uv[v*2+1];
            }
            std::printf("      uv%d [%.4f %.4f]..[%.4f %.4f]\n", channel, loU, loV, hiU, hiV);
        }
    }
    bf6_free(ctx, m);
    bf6_close(ctx);
    return 0;
}
