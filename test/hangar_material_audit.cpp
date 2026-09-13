/* Live audit for the front-end soldier studio's nearby hangar materials.
 *
 * This intentionally mirrors WorkerLoadNearbyHangar's data selection without
 * creating a renderer: the exact presentation-soldier placement establishes
 * the radius centre, then each admitted content_hangar mesh is resolved in its
 * placement scope.  Rows expose whether the 256-mesh viewer cap or an absent
 * base-colour binding is responsible for a visible approximation.
 */
#include "bf6_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
struct Candidate {
    float distance = 0.f;
    const bf6_instance* placement = nullptr;
};

bool admitted(const bf6_instance& p)
{
    if (!p.res_name || !p.source) return false;
    const std::string resource = p.res_name;
    if (resource.find("common/environment/") == std::string::npos &&
        resource.find("common/lighting/") == std::string::npos)
        return false;
    return resource.find("/decals/") == std::string::npos &&
           resource.find("/de_") == std::string::npos;
}

std::string mesh_name(const char* raw)
{
    std::string result = raw ? raw : "";
    if (result.size() > 4 && result.compare(result.size() - 4, 4, ".ebx") == 0)
        result.resize(result.size() - 4);
    if (result.size() < 5 || result.compare(result.size() - 5, 5, "_mesh") != 0)
        result += "_mesh";
    return result;
}

uint16_t read_u16(const uint8_t* bytes, size_t offset)
{
    uint16_t value = 0;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

int32_t read_i32(const uint8_t* bytes, size_t offset)
{
    int32_t value = 0;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

uint32_t read_u32(const uint8_t* bytes, size_t offset)
{
    uint32_t value = 0;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

float determinant3x3(const float* m)
{
    return m[0] * (m[4] * m[8] - m[5] * m[7]) -
           m[1] * (m[3] * m[8] - m[5] * m[6]) +
           m[2] * (m[3] * m[7] - m[4] * m[6]);
}
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: hangar_material_audit <game_dir>\n");
        return 2;
    }
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, (int)sizeof(error));
    if (!context || !bf6_mount_frontend(context, error, (int)sizeof(error))) {
        std::fprintf(stderr, "open/mount: %s\n", error);
        if (context) bf6_close(context);
        return 1;
    }

    static const char* kCustomization =
        "game/glacierflow/flow_mainmenu/content_customization";
    static const char* kHangar =
        "game/glacierflow/flow_mainmenu/content_hangar";
    static const char* kSoldier =
        "common/characters/presentationsoldier/presentationsoldier.ebx";

    const int actorCount = bf6_asset_instances(
        context, kCustomization, nullptr, 0, error, (int)sizeof(error));
    std::vector<bf6_instance> actors((size_t)(std::max)(actorCount, 0));
    if (actorCount <= 0 || bf6_asset_instances(
            context, kCustomization, actors.data(), actorCount,
            error, (int)sizeof(error)) != actorCount) {
        std::fprintf(stderr, "customization walk: %s\n", error);
        bf6_close(context);
        return 1;
    }
    const bf6_instance* actor = nullptr;
    for (const bf6_instance& row : actors)
        if (row.source && std::strcmp(row.source, kSoldier) == 0) {
            actor = &row;
            break;
        }
    if (!actor) {
        std::fprintf(stderr, "presentation-soldier placement absent\n");
        bf6_close(context);
        return 1;
    }
    std::printf("ACTOR origin=(%.6f %.6f %.6f)\n",
        actor->xform[9], actor->xform[10], actor->xform[11]);

    static const char* kReflectionTextures[] = {
        "a847bfb7-db01-4a62-a279-f3bba63c578a-tex",
        "f84912ce-67f6-415a-b431-4f53ef20a810-tex",
        "f3e26174-8ef3-4682-b131-891753a1ccf1-tex",
        "9523b79e-504b-4d0d-854e-b70b50214a7e-tex",
        "31131877-d634-4246-9b3a-b5fb204892ec-tex",
        "e35b9646-90c5-4937-b6b9-e39333160a27-tex",
        "b2d35748-4fe8-4921-8127-34343398b47c-tex",
    };
    static const char* kReflectionRoot =
        "game/glacierflow/flow_mainmenu/reflectionvolumetexture/"
        "content_hangar_reflectionvolumes/";
    for (const char* leaf : kReflectionTextures) {
        const std::string resource = std::string(kReflectionRoot) + leaf;
        const uint8_t* raw = nullptr;
        const int64_t size = bf6_read_raw(
            context, BF6_RAW_RES, resource.c_str(), &raw);
        if (size < 120 || !raw) {
            std::printf("REFLECTION %s unreadable bytes=%lld\n", leaf,
                (long long)size);
            continue;
        }
        const int mipCount = raw[30];
        uint64_t faceChainBytes = 0;
        for (int mip = 0; mip < (std::min)(mipCount, 15); ++mip)
            faceChainBytes += read_u32(raw, 56 + (size_t)mip * 4);
        std::printf("REFLECTION %s format=%d size=%ux%u slices=%u mips=%d "
                    "streamflag=0x%02x per_face_chain=%llu\n",
            leaf, read_i32(raw, 12), read_u16(raw, 22), read_u16(raw, 24),
            read_u16(raw, 28), mipCount, raw[21],
            (unsigned long long)faceChainBytes);
        const int textureId = bf6_texture_id_by_name(context, resource.c_str());
        const bf6_texture* texture = textureId >= 0
            ? bf6_texture_at(context, textureId) : nullptr;
        std::printf("REFLECTION_DECODE %s id=%d ok=%d width=%d height=%d "
                    "mips=%d format=%d bytes=%d srgb=%d\n",
            leaf, textureId, texture ? 1 : 0,
            texture ? texture->width : 0, texture ? texture->height : 0,
            texture ? texture->mip_count : 0,
            texture ? (int)texture->format : -1,
            texture ? texture->data_len : 0,
            texture ? texture->srgb : 0);
    }

    const int placementCount = bf6_asset_instances(
        context, kHangar, nullptr, 0, error, (int)sizeof(error));
    std::vector<bf6_instance> placements(
        (size_t)(std::max)(placementCount, 0));
    if (placementCount <= 0 || bf6_asset_instances(
            context, kHangar, placements.data(), placementCount,
            error, (int)sizeof(error)) != placementCount) {
        std::fprintf(stderr, "hangar walk: %s\n", error);
        bf6_close(context);
        return 1;
    }

    std::vector<Candidate> candidates;
    for (const bf6_instance& row : placements) {
        if (!admitted(row)) continue;
        const float dx = row.xform[9] - actor->xform[9];
        const float dy = row.xform[10] - actor->xform[10];
        const float dz = row.xform[11] - actor->xform[11];
        const float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 <= 30.f * 30.f)
            candidates.push_back({std::sqrt(d2), &row});
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) {
            return a.distance < b.distance;
        });

    int decoded = 0, capped = 0, allMaterials = 0, albedoMaterials = 0;
    int missingRows = 0, omittedMissingRows = 0;
    long long windingWithNormal = 0, windingAgainstNormal = 0;
    int positivePlacements = 0, negativePlacements = 0, singularPlacements = 0;
    for (size_t index = 0; index < candidates.size(); ++index) {
        const bf6_instance& row = *candidates[index].placement;
        const float placementDeterminant = determinant3x3(row.xform);
        if (placementDeterminant > 1e-8f) ++positivePlacements;
        else if (placementDeterminant < -1e-8f) ++negativePlacements;
        else ++singularPlacements;
        const std::string mesh = mesh_name(row.res_name);
        bf6_mesh* value = bf6_read_mesh_scoped(
            context, mesh.c_str(), 0, row.placing_bundle, row.variation);
        if (!value) continue;
        ++decoded;
        if (index < 256) ++capped;
        for (int section = 0; section < value->section_count; ++section) {
            const bf6_section& s = value->sections[section];
            if (!s.positions || !s.normals || !s.indices) continue;
            for (int triangle = 0; triangle + 2 < s.index_count;
                 triangle += 3) {
                const uint32_t i0 = s.indices[triangle + 0];
                const uint32_t i1 = s.indices[triangle + 1];
                const uint32_t i2 = s.indices[triangle + 2];
                if (i0 >= (uint32_t)s.vertex_count ||
                    i1 >= (uint32_t)s.vertex_count ||
                    i2 >= (uint32_t)s.vertex_count)
                    continue;
                const float* a = s.positions + i0 * 3;
                const float* b = s.positions + i1 * 3;
                const float* c = s.positions + i2 * 3;
                const float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
                const float ac[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                const float cross[3] = {
                    ab[1] * ac[2] - ab[2] * ac[1],
                    ab[2] * ac[0] - ab[0] * ac[2],
                    ab[0] * ac[1] - ab[1] * ac[0]};
                const float* n0 = s.normals + i0 * 3;
                const float* n1 = s.normals + i1 * 3;
                const float* n2 = s.normals + i2 * 3;
                const float normal[3] = {
                    n0[0] + n1[0] + n2[0],
                    n0[1] + n1[1] + n2[1],
                    n0[2] + n1[2] + n2[2]};
                const float dot = cross[0] * normal[0] +
                                  cross[1] * normal[1] +
                                  cross[2] * normal[2];
                if (dot > 1e-10f) ++windingWithNormal;
                else if (dot < -1e-10f) ++windingAgainstNormal;
            }
        }
        int albedo = 0;
        for (int material = 0; material < value->material_count; ++material) {
            ++allMaterials;
            bool hasAlbedo = false;
            for (int binding = 0;
                 binding < value->materials[material].texture_count; ++binding)
                hasAlbedo |= value->materials[material].textures[binding].slot ==
                             BF6_TEX_ALBEDO;
            if (hasAlbedo) {
                ++albedo;
                ++albedoMaterials;
            }
        }
        if (albedo != value->material_count) {
            ++missingRows;
            if (index >= 256) ++omittedMissingRows;
            std::printf("%s index=%zu distance=%.3f materials=%d albedo=%d "
                        "scope=%s mesh=%s\n",
                index < 256 ? "LOADED" : "OMITTED", index,
                candidates[index].distance, value->material_count, albedo,
                row.placing_bundle ? row.placing_bundle : "<none>",
                mesh.c_str());
        }
        bf6_free(context, value);
    }
    const float capDistance = candidates.size() > 258
        ? candidates[258].distance
        : (candidates.empty() ? 0.f : candidates.back().distance);
    const float farDistance = candidates.empty() ? 0.f : candidates.back().distance;
    std::printf("SUMMARY placements=%d candidates=%zu decoded=%d "
                "viewer_first_256=%d materials=%d albedo=%d "
                "missing_rows=%d omitted_missing_rows=%d "
                "cap_distance=%.3f far_distance=%.3f\n",
        placementCount, candidates.size(), decoded, capped, allMaterials,
        albedoMaterials, missingRows, omittedMissingRows, capDistance,
        farDistance);
    std::printf("WINDING cross-dot-normal positive=%lld negative=%lld "
                "positive_placements=%d negative_placements=%d singular=%d\n",
        windingWithNormal, windingAgainstNormal, positivePlacements,
        negativePlacements, singularPlacements);
    bf6_close(context);
    return candidates.empty() || decoded == 0 ||
        (windingWithNormal == 0 && windingAgainstNormal == 0) ? 1 : 0;
}
