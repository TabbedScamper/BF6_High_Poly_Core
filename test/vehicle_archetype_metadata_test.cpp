#include "bf6_core.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, (int)sizeof(error));
    if (!context)
    {
        std::fprintf(stderr, "open failed: %s\n", error);
        return 1;
    }
    if (!bf6_mount_all(context, 0, error, (int)sizeof(error)))
    {
        std::fprintf(stderr, "mount failed: %s\n", error);
        bf6_close(context);
        return 1;
    }

    const int count = bf6_vehicle_archetypes(context, nullptr, 0);
    std::vector<bf6_vehicle_archetype> rows(
        count > 0 ? (size_t)count : 0u);
    const int filled = count > 0
        ? bf6_vehicle_archetypes(context, rows.data(), count) : count;

    static const std::array<const char*, 13> expectedIds{{
        "MBT", "IFV", "TacticalLightTransport", "APC", "AA",
        "Helicopter", "ScoutHeli", "TransportHelicopter", "AttackPlane",
        "FighterPlane", "PatrolBoat", "FleetDefenseFighter",
        "MultiroleFighter"
    }};
    std::set<int> iconIndices;
    bool exact = count == (int)expectedIds.size() && filled == count;
    for (int i = 0; i < filled && i < (int)rows.size(); ++i)
    {
        const auto& row = rows[(size_t)i];
        exact = exact && std::strcmp(row.id, expectedIds[(size_t)i]) == 0 &&
            row.order == i + 1 && row.name[0] && row.abbreviated_name[0] &&
            row.description[0] && row.icon_asset[0] && row.icon_atlas[0] &&
            row.icon_index >= 0 && row.icon_index < count &&
            row.variant_count > 0;
        iconIndices.insert(row.icon_index);
    }
    exact = exact && iconIndices.size() == expectedIds.size();

    const int presetCount = bf6_vehicle_loadout_presets(context, nullptr, 0);
    std::vector<bf6_vehicle_loadout_preset> presets(
        presetCount > 0 ? (size_t)presetCount : 0u);
    const int presetsFilled = presetCount > 0
        ? bf6_vehicle_loadout_presets(
            context, presets.data(), presetCount) : presetCount;
    std::array<const char*, 8> presetGroups{{
        "AA", "AH", "APC", "IFV", "MBT", "SH", "PB", "MF"
    }};
    int presetOrdinal = 0;
    exact = exact && presetCount == 24 && presetsFilled == presetCount;
    for (const char* group : presetGroups)
        for (int index = 1; index <= 3; ++index, ++presetOrdinal)
        {
            const auto& preset = presets[(size_t)presetOrdinal];
            exact = exact && std::strcmp(preset.archetype, group) == 0 &&
                preset.preset_index == index && preset.name[0] &&
                preset.description[0];
        }

    const int fake = bf6_list_ebx(context,
        "__bf6_control__/uivehiclearchetypemetadata", nullptr, 0);
    const int mutated = bf6_list_ebx(context,
        "uivehiclearchetypemetadata_control", nullptr, 0);
    exact = exact && fake == 0 && mutated == 0;

    std::printf("vehicle-archetype rows=%d/%zu icon-permutation=%zu "
                "presets=%d/24 fake=%d mutated=%d %s\n",
                filled, expectedIds.size(), iconIndices.size(), presetsFilled,
                fake, mutated,
                exact ? "PASS" : "FAIL");
    bf6_close(context);
    return exact ? 0 : 1;
}
