#include "bf6_core.h"
#include "rime.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 2 && argc != 3) return 2;
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, sizeof(error));
    if (!context || !bf6_mount_all(context, 1, error, sizeof(error)))
    {
        std::fprintf(stderr, "open/mount: %s\n", error);
        if (context) bf6_close(context);
        return 1;
    }

    const char* defaultRoutes[] = {
        "common/ui/loadout/menu/screens/menuloadoutclassselectscreen",
        "common/ui/loadout/menu/screens/menuloadoutoverviewscreen",
        "common/ui/loadout/menu/screens/menuloadoutscreen",
        "common/ui/loadout/menu/screens/menuclassdetailsscreen",
        "common/ui/loadout/menu/screens/menuloadoutfieldupgradesscreen",
        "common/ui/loadout/menu/screens/menuloadoutinfoscreen",
        "common/ui/loadout/menu/screens/menuloadoutcharactericonscreen",
        "common/ui/soldier/shared/screens/menucharacterselect_view",
    };
    std::vector<const char*> routes;
    if (argc == 3)
        routes.push_back(argv[2]);
    else
        routes.assign(std::begin(defaultRoutes), std::end(defaultRoutes));
    if (argc == 3)
    {
        char dataName[256]{};
        const int fieldCount = bf6_rime_dbd_fields(
            context, argv[2], dataName, static_cast<int>(sizeof(dataName)),
            nullptr, 0);
        if (fieldCount > 0)
        {
            std::vector<bf6_rime_dbd_field> fields(
                static_cast<size_t>(fieldCount));
            const int filled = bf6_rime_dbd_fields(
                context, argv[2], dataName,
                static_cast<int>(sizeof(dataName)), fields.data(), fieldCount);
            std::printf("DBD %s data=%s fields=%d/%d\n",
                        argv[2], dataName, filled, fieldCount);
            for (const bf6_rime_dbd_field& field : fields)
                std::printf("  %s sig=0x%016llx\n", field.name,
                            static_cast<unsigned long long>(
                                field.type_signature));
            bf6_close(context);
            return filled == fieldCount ? 0 : 1;
        }
    }
    bool passed = true;
    static const char* roles[] = {"assault", "engineer", "support", "recon"};
    for (const char* role : roles)
    {
        bf6_field_upgrade_path paths[2]{};
        const int pathCount = bf6_field_upgrade_paths(context, role, paths, 2);
        std::printf("===== field-upgrade %s paths=%d =====\n", role, pathCount);
        for (int path = 0; path < pathCount; ++path)
        {
            std::printf("PATH[%d] id=%s name=%s desc=%s\n", path,
                        paths[path].id, paths[path].name,
                        paths[path].description);
            for (int ability = 0; ability < paths[path].ability_count; ++ability)
                std::printf("  ABILITY[%d] id=%s name=%s desc=%s\n", ability,
                            paths[path].abilities[ability].id,
                            paths[path].abilities[ability].name,
                            paths[path].abilities[ability].description);
        }
    }
    for (const char* route : routes)
    {
        bf6_rime_tree_stats stats{};
        const int count = bf6_rime_tree(
            context, route, 6, nullptr, 0, &stats);
        std::vector<bf6_rime_node> nodes(
            static_cast<size_t>((std::max)(count, 0)));
        const int filled = count > 0 ? bf6_rime_tree(
            context, route, 6, nodes.data(), count, &stats) : count;
        rime::Screen screen;
        std::string rimeError;
        const bool converted = filled == count && count > 0 &&
            rime::from_live(nodes.data(), count, screen, rimeError);
        if (converted) rime::solve(screen, 1920.f, 1080.f);
        std::printf("===== %s rows=%d/%d converted=%d gaps=%d/%d/%d =====\n",
                    route, filled, count, converted ? 1 : 0,
                    stats.unknown_types, stats.unresolved_refs,
                    stats.ambiguous_refs);
        if (!converted)
        {
            std::printf("error=%s\n", rimeError.c_str());
            passed = false;
            continue;
        }
        for (size_t index = 0; index < screen.elements.size(); ++index)
        {
            const rime::Element& element = screen.elements[index];
            const bool interesting =
                std::string(route).find("menuloadoutclassselectscreen") !=
                    std::string::npos ||
                std::string(route).find("metacustomization_griditemcell") !=
                    std::string::npos ||
                std::string(route) ==
                    "common/ui/loadout/menu/screens/menuloadoutscreen" ||
                element.depth <= 4 ||
                !element.item_template.empty() ||
                !element.text.empty() ||
                element.name.find("List") != std::string::npos ||
                element.name.find("Tile") != std::string::npos ||
                element.name.find("Class") != std::string::npos ||
                element.name.find("Weapon") != std::string::npos ||
                element.name.find("Gadget") != std::string::npos;
            if (!interesting) continue;
            std::printf(
                "[%zu] p=%d d=%d solved=%d visible=%d alpha=%.3f "
                "box=%.1f,%.1f..%.1f,%.1f name=%s type=%s "
                "template=%s list(spacing=%.1f orientation=%d size=%d "
                "flow=%d space=%d preserve=%d) "
                "grid(orientation=%d segments=%d col=%.1f row=%.1f "
                "gap=%.1f/%.1f distribution=%d mode=%d flow=%d/%d fit=%d) "
                "text=%s ref=%s part=%s\n",
                index, element.parent, element.depth,
                element.solved ? 1 : 0, element.visible ? 1 : 0,
                element.alpha, element.x0, element.y0,
                element.x1, element.y1, element.name.c_str(),
                element.type_name.c_str(), element.item_template.c_str(),
                element.item_spacing, element.stack_orientation,
                element.data_list_size_distribution,
                element.data_list_flow_direction,
                element.data_list_space_distribution,
                element.data_list_preserve_fit_content,
                element.grid_orientation,
                element.grid_static_segment_item_count,
                element.grid_column_size, element.grid_row_size,
                element.grid_column_spacing, element.grid_row_spacing,
                element.grid_segment_distribution,
                element.grid_segment_count_mode,
                element.grid_column_flow_direction,
                element.grid_row_flow_direction,
                element.grid_item_fit_content,
                element.text.c_str(), element.references_widget.c_str(),
                element.partition.c_str());
        }
        if (std::string(route) ==
                "common/ui/loadout/menu/screens/menuclassdetailsscreen")
        {
            static const uint32_t classDetailSids[] = {
                0x12AC25E1u, 0xAB0E0EA3u, 0x6DF0DD4Du,
                0x622A0B3Au, 0x5D1D3DEAu, 0x62722892u,
                0x1424E3C5u, 0x1424FA3Du, 0xA7047F19u,
                0x9A36A272u,
            };
            std::printf("===== class-details localized constants =====\n");
            for (uint32_t sid : classDetailSids)
            {
                const char* localized = bf6_localized_string(context, sid);
                std::printf("SID 0x%08X = %s\n", sid,
                            localized && *localized ? localized :
                            "<unresolved>");
            }
        }
    }
    if (argc == 3 && std::string(argv[2]) ==
            "common/ui/loadout/menu/screens/menuloadoutscreen")
    {
        const int assetCount = bf6_list_ebx(
            context, "databindings", nullptr, 0);
        std::vector<bf6_asset> assets(
            static_cast<size_t>((std::max)(assetCount, 0)));
        if (assetCount > 0)
            bf6_list_ebx(context, "databindings", assets.data(), assetCount);
        std::printf("===== loadout DBD candidates =====\n");
        for (const bf6_asset& asset : assets)
        {
            if (!asset.name || !std::strstr(asset.name, "loadout")) continue;
            char dataName[256]{};
            const int fieldCount = bf6_rime_dbd_fields(
                context, asset.name, dataName,
                static_cast<int>(sizeof(dataName)), nullptr, 0);
            if (fieldCount <= 0) continue;
            std::vector<bf6_rime_dbd_field> fields(
                static_cast<size_t>(fieldCount));
            const int filled = bf6_rime_dbd_fields(
                context, asset.name, dataName,
                static_cast<int>(sizeof(dataName)), fields.data(), fieldCount);
            std::printf("DBD %s data=%s fields=%d/%d\n",
                        asset.name, dataName, filled, fieldCount);
            for (const bf6_rime_dbd_field& field : fields)
                std::printf("  %s sig=0x%016llx\n", field.name,
                            static_cast<unsigned long long>(
                                field.type_signature));
        }
    }
    if (argc == 3 && std::string(argv[2]) ==
            "common/ui/vehicles/shared/screens/menuvehicle_loadoutscreen")
    {
        const int assetCount = bf6_list_ebx(context, "vehicle", nullptr, 0);
        std::vector<bf6_asset> assets(
            static_cast<size_t>((std::max)(assetCount, 0)));
        if (assetCount > 0)
            bf6_list_ebx(context, "vehicle", assets.data(), assetCount);
        std::printf("===== vehicle screen candidates =====\n");
        for (const bf6_asset& asset : assets)
            if (asset.name && std::strstr(asset.name, "/screens/") &&
                (std::strstr(asset.name, "vehicle") ||
                 std::strstr(asset.name, "Vehicle")))
                std::printf("SCREEN %s\n", asset.name);
        std::printf("===== vehicle DBD candidates =====\n");
        for (const bf6_asset& asset : assets)
        {
            if (!asset.name || !std::strstr(asset.name, "databinding"))
                continue;
            char dataName[256]{};
            const int fieldCount = bf6_rime_dbd_fields(
                context, asset.name, dataName,
                static_cast<int>(sizeof(dataName)), nullptr, 0);
            if (fieldCount <= 0) continue;
            std::vector<bf6_rime_dbd_field> fields(
                static_cast<size_t>(fieldCount));
            const int filled = bf6_rime_dbd_fields(
                context, asset.name, dataName,
                static_cast<int>(sizeof(dataName)), fields.data(), fieldCount);
            std::printf("DBD %s data=%s fields=%d/%d\n",
                        asset.name, dataName, filled, fieldCount);
            for (const bf6_rime_dbd_field& field : fields)
                std::printf("  %s sig=0x%016llx\n", field.name,
                            static_cast<unsigned long long>(
                                field.type_signature));
        }
    }
    bf6_close(context);
    return passed ? 0 : 1;
}
