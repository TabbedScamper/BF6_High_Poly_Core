/* Live regression for a nested front-end material scope.
 *
 * content_hangar owns a child ShaderBlockDepot, but this scissor-lift LOD0
 * state key is deliberately absent there and present in the enclosing
 * flow_mainmenu depot.  A scoped read must therefore retain the child as the
 * first-wins override and continue through its ancestor chain.
 */
#include "bf6_core.h"

#include <cstdio>
#include <cstring>

namespace {
const char* kMesh =
    "common/environment/generic/industrial/props/scissorlift_01/"
    "ind_scissorlift_01_mesh";
const char* kChild =
    "win32/game/glacierflow/flow_mainmenu/content_hangar";
const char* kParent =
    "win32/game/glacierflow/flow_mainmenu/flow_mainmenu";

const char* texture_for(const bf6_ctx* ctx, const bf6_mesh* mesh,
                        bf6_tex_slot slot)
{
    if (!ctx || !mesh) return nullptr;
    for (int m = 0; m < mesh->material_count; ++m)
        for (int b = 0; b < mesh->materials[m].texture_count; ++b)
            if (mesh->materials[m].textures[b].slot == slot)
                return bf6_texture_name_at(
                    const_cast<bf6_ctx*>(ctx),
                    mesh->materials[m].textures[b].texture);
    return nullptr;
}
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: material_parent_scope_test <game_dir>\n");
        return 2;
    }
    char error[512] = {};
    bf6_ctx* ctx = bf6_open(argv[1], error, (int)sizeof(error));
    if (!ctx || !bf6_mount_frontend(ctx, error, (int)sizeof(error))) {
        std::fprintf(stderr, "open/mount: %s\n", error);
        if (ctx) bf6_close(ctx);
        return 1;
    }

    bf6_mesh* through_child =
        bf6_read_mesh_scoped(ctx, kMesh, 0, kChild, nullptr);
    bf6_mesh* direct_parent =
        bf6_read_mesh_scoped(ctx, kMesh, 0, kParent, nullptr);
    const char* child_albedo = texture_for(ctx, through_child, BF6_TEX_ALBEDO);
    const char* parent_albedo = texture_for(ctx, direct_parent, BF6_TEX_ALBEDO);
    const char* child_normal = texture_for(ctx, through_child, BF6_TEX_NORMAL);
    const char* parent_normal = texture_for(ctx, direct_parent, BF6_TEX_NORMAL);

    const bool pass = child_albedo && parent_albedo && child_normal &&
        parent_normal && std::strcmp(child_albedo, parent_albedo) == 0 &&
        std::strcmp(child_normal, parent_normal) == 0;
    std::printf("child albedo=%s normal=%s\n",
                child_albedo ? child_albedo : "<none>",
                child_normal ? child_normal : "<none>");
    std::printf("parent albedo=%s normal=%s\n",
                parent_albedo ? parent_albedo : "<none>",
                parent_normal ? parent_normal : "<none>");

    if (through_child) bf6_free(ctx, through_child);
    if (direct_parent) bf6_free(ctx, direct_parent);
    bf6_close(ctx);
    return pass ? 0 : 1;
}
