/* Live material-scope control for package skins. No exported table is read. */
#include "bf6_core.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

static std::set<int> texture_ids(const bf6_mesh* mesh)
{
    std::set<int> ids;
    if (!mesh) return ids;
    for (int m = 0; m < mesh->material_count; ++m)
        for (int t = 0; t < mesh->materials[m].texture_count; ++t)
            if (mesh->materials[m].textures[t].texture >= 0)
                ids.insert(mesh->materials[m].textures[t].texture);
    return ids;
}

static unsigned long long material_signature(const bf6_mesh* mesh)
{
    unsigned long long h = 1469598103934665603ull;
    auto add = [&](const void* ptr, size_t size) {
        const unsigned char* p = static_cast<const unsigned char*>(ptr);
        for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    };
    if (!mesh) return h;
    add(&mesh->material_count, sizeof(mesh->material_count));
    for (int m = 0; m < mesh->material_count; ++m) {
        const bf6_material_desc& md = mesh->materials[m];
        add(md.base_color, sizeof(md.base_color));
        add(md.emissive, sizeof(md.emissive));
        add(&md.roughness, sizeof(md.roughness));
        add(&md.metallic, sizeof(md.metallic));
        for (int t = 0; t < md.texture_count; ++t) {
            add(&md.textures[t].slot, sizeof(md.textures[t].slot));
            add(&md.textures[t].texture, sizeof(md.textures[t].texture));
        }
    }
    return h;
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::printf("usage: weapon_skin_material_test <game_dir>\n");
        return 2;
    }
    char err[512]{};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c || !bf6_mount_all(c, 0, err, (int)sizeof(err))) {
        std::printf("mount failed: %s\n", err);
        return 1;
    }
    const char* md = "common/hardware/weapons/carbine/m4a1/md_m4a1";
    const char* real[] = { "wse0053", "wse0023", "wser0014", "wsl0021",
                           "wsr0075", "wsr0096", "wsrr0014" };
    int resolved = 0;
    for (const char* skin : real) {
        char bundle[256]{};
        const int r = bf6_weapon_skin_material_bundle(
            c, md, skin, bundle, (int)sizeof(bundle));
        std::printf("real\t%s\t%d\tscope=%d\t%s\n", skin, r,
                    r == 1 ? bf6_material_scope_exists(c, bundle) : 0, bundle);
        if (r == 1) ++resolved;
    }
    char fakeBundle[256]{};
    const int fake = bf6_weapon_skin_material_bundle(
        c, md, "wse9999_codex_control", fakeBundle, (int)sizeof(fakeBundle));
    std::printf("resolved=%d/7 fake=%d value='%s' (negative control)\n",
                resolved, fake, fakeBundle);

    // Ask both exact scopes for every part in the WSE0053 gameplay package.
    // The comparison is against the live dpf material, never an exported
    // expected table. A usable SPO sidecar must change at least one binding;
    // silently falling back to the DPF produces zero differences.
    const char* equipment =
        "common/hardware/weapons/carbine/m4a1/equipment_m4a1";
    const int configCount = bf6_weapon_package_configs(c, equipment, nullptr, 0);
    std::vector<bf6_weapon_package_config> configs(
        configCount > 0 ? (size_t)configCount : 0);
    if (configCount > 0)
        bf6_weapon_package_configs(c, equipment, configs.data(), configCount);
    const bf6_weapon_package_config* selected = nullptr;
    for (const auto& config : configs)
        if (std::string(config.skin) == "wse0053") selected = &config;
    int compared = 0, changed = 0, spoTextured = 0;
    if (selected) {
        std::vector<bf6_weapon_fit> fits((size_t)selected->fit_count);
        for (int i = 0; i < selected->fit_count; ++i) {
            fits[(size_t)i].slot = selected->fits[i].slot;
            fits[(size_t)i].attachment = selected->fits[i].attachment;
        }
        std::vector<bf6_weapon_part> parts(128);
        const int partCount = bf6_weapon_package_parts(
            c, md, fits.data(), (int)fits.size(), selected->skin,
            parts.data(), (int)parts.size());
        char spo[256]{};
        if (bf6_weapon_skin_material_bundle(c, md, selected->skin,
                                            spo, (int)sizeof(spo)) == 1) {
            for (int i = 0; i < partCount && i < (int)parts.size(); ++i) {
                bf6_mesh* base = bf6_read_armory_mesh_scoped(
                    c, parts[(size_t)i].mesh, 0, parts[(size_t)i].bundle, nullptr);
                bf6_mesh* skinMesh = bf6_read_armory_mesh_layered(
                    c, parts[(size_t)i].mesh, 0, parts[(size_t)i].bundle,
                    spo, nullptr);
                const std::set<int> a = texture_ids(base);
                const std::set<int> b = texture_ids(skinMesh);
                if (!b.empty()) ++spoTextured;
                if (a != b || material_signature(base) != material_signature(skinMesh))
                    ++changed;
                ++compared;
                if (base) bf6_free(c, base);
                if (skinMesh) bf6_free(c, skinMesh);
            }
        }
    }
    std::printf("wse0053 scope: compared=%d textured=%d changed=%d\n",
                compared, spoTextured, changed);

    // WSEr0014 is the positive control for package-specific alternate meshes.
    // Discover every mesh and bundle through the package read path, then ask
    // the mounted asset table which live OV actually resolves. No expected
    // texture name or package bundle is staged beside this executable.
    const bf6_weapon_package_config* selected14 = nullptr;
    for (const auto& config : configs)
        if (std::string(config.skin) == "wser0014") selected14 = &config;
    int variationParts = 0, variedTextures = 0, fakeVariationHits = 0;
    if (selected14) {
        std::vector<bf6_weapon_fit> fits((size_t)selected14->fit_count);
        for (int i = 0; i < selected14->fit_count; ++i) {
            fits[(size_t)i].slot = selected14->fits[i].slot;
            fits[(size_t)i].attachment = selected14->fits[i].attachment;
        }
        std::vector<bf6_weapon_part> parts(128);
        const int partCount = bf6_weapon_package_parts(
            c, md, fits.data(), (int)fits.size(), selected14->skin,
            parts.data(), (int)parts.size());
        for (int i = 0; i < partCount && i < (int)parts.size(); ++i) {
            char variation[512]{}, fakeVariation[512]{};
            const int vr = bf6_weapon_part_variation(
                c, parts[(size_t)i].mesh, parts[(size_t)i].bundle,
                selected14->skin, variation, (int)sizeof(variation));
            const int fr = bf6_weapon_part_variation(
                c, parts[(size_t)i].mesh, parts[(size_t)i].bundle,
                "wser9999_codex_control", fakeVariation,
                (int)sizeof(fakeVariation));
            if (fr != 0 || fakeVariation[0]) ++fakeVariationHits;
            int textures = 0;
            if (vr == 1) {
                ++variationParts;
                bf6_mesh* varied = bf6_read_armory_mesh_scoped(
                    c, parts[(size_t)i].mesh, 0, parts[(size_t)i].bundle,
                    variation);
                textures = (int)texture_ids(varied).size();
                variedTextures += textures;
                if (varied) bf6_free(c, varied);
            }
            std::printf("wser0014 part: vr=%d textures=%d mesh=%s bundle=%s ov=%s\n",
                        vr, textures, parts[(size_t)i].mesh,
                        parts[(size_t)i].bundle, variation);
        }
    }
    std::printf("wser0014 total: variation-parts=%d textures=%d; fake-hits=%d (negative control)\n",
                variationParts, variedTextures, fakeVariationHits);
    bf6_close(c);
    return resolved == 7 && fake == 0 && fakeBundle[0] == '\0' &&
           compared > 0 && variationParts > 0 && variedTextures > 0 &&
           fakeVariationHits == 0 ? 0 : 1;
}
