#include "bf6_core.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, sizeof(error));
    if (!context || !bf6_mount_all(context, 1, error, sizeof(error)))
    {
        std::fprintf(stderr, "open/mount: %s\n", error);
        if (context) bf6_close(context);
        return 1;
    }

    // BFUILoadoutClassType has four selectable soldier classes.  VEHICLES
    // owns the adjacent ConceptClassTabRight route and must not be counted as
    // a fifth record in LoadoutClassItems, even though its separate tab icon
    // is a valid installed asset.
    static const char* classIcons[] = {
        "common/ui/assets/images/classes/t_ui_iconclassassault",
        "common/ui/assets/images/classes/t_ui_iconclassengineer",
        "common/ui/assets/images/classes/t_ui_iconclasssupport",
        "common/ui/assets/images/classes/t_ui_iconclassrecon",
    };
    int decoded = 0;
    for (const char* icon : classIcons)
    {
        const int id = bf6_rime_texture_id(context, icon);
        const bf6_texture* texture = id >= 0
            ? bf6_texture_at(context, id) : nullptr;
        bf6_rime_svg_info svg{};
        const int contours = bf6_rime_svg(
            context, icon, &svg, nullptr, 0, nullptr, 0);
        const bool native = texture && texture->data && texture->data_len > 0 &&
            texture->width > 0 && texture->height > 0;
        const bool vector = contours > 0 && contours == svg.contour_count &&
            svg.point_count > 0 && svg.canvas[0] > 0.f && svg.canvas[1] > 0.f;
        const bool ok = native || vector;
        decoded += ok ? 1 : 0;
        std::printf("icon=%s id=%d native=%d size=%dx%d fmt=%d srgb=%d "
                    "svg=%d/%d/%d canvas=%.0fx%.0f decoded=%d\n",
                    icon, id, native ? 1 : 0,
                    texture ? texture->width : 0,
                    texture ? texture->height : 0,
                    texture ? static_cast<int>(texture->format) : -1,
                    texture ? texture->srgb : -1,
                    svg.shape_count, svg.contour_count, svg.point_count,
                    svg.canvas[0], svg.canvas[1], ok ? 1 : 0);
    }
    const int fake = bf6_rime_texture_id(
        context, "common/ui/assets/images/classes/absent_fake_class_icon");
    const char* vehicleIcon =
        "common/ui/assets/images/classes/t_ui_iconvehicles";
    bf6_rime_svg_info vehicleSvg{};
    const int vehicleContours = bf6_rime_svg(
        context, vehicleIcon, &vehicleSvg, nullptr, 0, nullptr, 0);
    const bool vehicleTabDecoded = vehicleContours > 0 &&
        vehicleContours == vehicleSvg.contour_count &&
        vehicleSvg.point_count > 0 && vehicleSvg.canvas[0] > 0.f &&
        vehicleSvg.canvas[1] > 0.f;
    const char* appearanceIcon =
        "common/ui/assets/images/classes/t_ui_charactersicon";
    bf6_rime_svg_info appearanceSvg{};
    const int appearanceContours = bf6_rime_svg(
        context, appearanceIcon, &appearanceSvg, nullptr, 0, nullptr, 0);
    const bool appearanceDecoded = appearanceContours > 0 &&
        appearanceContours == appearanceSvg.contour_count &&
        appearanceSvg.shape_count == 8 && appearanceSvg.point_count > 0 &&
        appearanceSvg.canvas[0] > 0.f && appearanceSvg.canvas[1] > 0.f;
    std::printf("soldier-class-icons=%d/%zu vehicle-tab-icon=%d "
                "appearance-icon=%d/%d/%d canvas=%.2fx%.2f fake=%d\n",
                decoded, std::size(classIcons),
                vehicleTabDecoded ? 1 : 0,
                appearanceSvg.shape_count, appearanceSvg.contour_count,
                appearanceSvg.point_count, appearanceSvg.canvas[0],
                appearanceSvg.canvas[1], fake);

    static const char* dbds[] = {
        "common/ui/loadout/shared/assets/databindings/loadoutclassitemdbd",
        "common/ui/loadout/shared/assets/databindings/loadoutclassdetailsdbd",
        "common/ui/loadout/shared/assets/databindings/loadoutclasstraitdbd",
    };
    int dbdDecoded = 0;
    for (const char* dbd : dbds)
    {
        char dataName[128]{};
        const int count = bf6_rime_dbd_fields(
            context, dbd, dataName, sizeof(dataName), nullptr, 0);
        std::vector<bf6_rime_dbd_field> fields(
            static_cast<size_t>(count > 0 ? count : 0));
        const int filled = count > 0 ? bf6_rime_dbd_fields(
            context, dbd, dataName, sizeof(dataName), fields.data(), count)
            : count;
        std::printf("dbd=%s data=%s fields=%d/%d\n",
                    dbd, dataName, filled, count);
        if (filled == count && count > 0) ++dbdDecoded;
        for (const bf6_rime_dbd_field& field : fields)
            std::printf("  field=%s signature=%016llX\n", field.name,
                        static_cast<unsigned long long>(field.type_signature));
    }
    const int fakeDbd = bf6_rime_dbd_fields(
        context, "common/ui/loadout/shared/assets/databindings/"
                 "absent_fake_loadoutclassdbd",
        nullptr, 0, nullptr, 0);
    std::printf("dbds=%d/%zu fake-dbd=%d\n", dbdDecoded,
                std::size(dbds), fakeDbd);
    static const char* roles[] = {
        "assault", "engineer", "support", "recon"
    };
    const auto normalizedIcon = [](const char* value) {
        std::string path = value ? value : "";
        if (path.size() > 4 &&
            path.compare(path.size() - 4, 4, ".ebx") == 0)
            path.resize(path.size() - 4);
        return path;
    };
    int upgradeRows = 0, upgradeIcons = 0, upgradeDecoded = 0;
    for (const char* role : roles)
    {
        bf6_field_upgrade_path paths[2]{};
        const int pathCount = bf6_field_upgrade_paths(
            context, role, paths, 2);
        for (int pathIndex = 0; pathIndex < pathCount; ++pathIndex)
        {
            const bf6_field_upgrade_path& path = paths[pathIndex];
            const std::string pathIcon = normalizedIcon(path.icon_asset);
            const int texture = !pathIcon.empty()
                ? bf6_texture_id_by_name(context, pathIcon.c_str()) : -1;
            const int contours = !pathIcon.empty()
                ? bf6_rime_svg(context, pathIcon.c_str(), nullptr,
                               nullptr, 0, nullptr, 0) : -1;
            ++upgradeRows;
            if (path.icon_asset[0]) ++upgradeIcons;
            if (texture >= 0 || contours > 0) ++upgradeDecoded;
            std::printf("upgrade=%s/%d icon=%s texture=%d svg=%d\n",
                        role, pathIndex, path.icon_asset, texture, contours);
            for (int ability = 0; ability < path.ability_count; ++ability)
            {
                const char* icon = path.abilities[ability].icon_asset;
                const std::string abilityIcon = normalizedIcon(icon);
                const int abilityTexture = !abilityIcon.empty()
                    ? bf6_texture_id_by_name(context, abilityIcon.c_str()) : -1;
                const int abilityContours = !abilityIcon.empty()
                    ? bf6_rime_svg(context, abilityIcon.c_str(), nullptr,
                                   nullptr, 0, nullptr, 0) : -1;
                ++upgradeRows;
                if (icon[0]) ++upgradeIcons;
                if (abilityTexture >= 0 || abilityContours > 0)
                    ++upgradeDecoded;
                std::printf("  ability=%d icon=%s texture=%d svg=%d\n",
                            ability, icon, abilityTexture, abilityContours);
            }
        }
    }
    const int fakeUpgrade = bf6_texture_id_by_name(
        context, "common/ui/__control__/fake_field_upgrade_icon");
    std::printf("upgrade-icons=%d/%d rows=%d fake=%d\n",
                upgradeDecoded, upgradeIcons, upgradeRows, fakeUpgrade);
    bf6_close(context);
    return decoded == static_cast<int>(std::size(classIcons)) &&
        vehicleTabDecoded && appearanceDecoded && fake < 0 &&
        dbdDecoded == static_cast<int>(std::size(dbds)) &&
        fakeDbd < 0 && upgradeRows == 40 && upgradeIcons == 26 &&
        upgradeDecoded == upgradeIcons && fakeUpgrade < 0 ? 0 : 1;
}
