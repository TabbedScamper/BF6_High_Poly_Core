#include "bf6_core.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: rime_markup_svg_live_test <game-dir>\n");
        return 2;
    }
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, sizeof(error));
    if (!context || !bf6_mount_all(context, 1, error, sizeof(error)))
    {
        std::fprintf(stderr, "open/mount failed: %s\n", error);
        if (context) bf6_close(context);
        return 1;
    }
    const int count = bf6_rime_markup_svgs(context, nullptr, 0);
    std::vector<bf6_rime_markup_svg> rows(count > 0 ? (size_t)count : 0u);
    const int filled = count > 0
        ? bf6_rime_markup_svgs(context, rows.data(), count) : count;
    std::map<std::string, std::string> actual;
    for (const auto& row : rows) actual.emplace(row.name, row.image_asset);
    std::printf("markup rows count=%d filled=%d unique=%zu\n",
                count, filled, actual.size());
    for (const auto& row : actual)
        std::printf("  %s -> %s\n", row.first.c_str(), row.second.c_str());

    const std::map<std::string, std::string> expected = {
        {"SpeechToText", "common/ui/assets/images/hud/voip/t_ui_icon_voip_on"},
        {"CustomWeaponDrop", "common/ui/hud/worldicons/assets/images/f2p/t_ui_granite_customweapondrop_icn"},
        {"CombatVehicleTrailerLocked", "common/ui/hud/worldicons/assets/images/f2p/t_ui_iwi_vehiclecratelocked_icn"},
        {"bot", "common/ui/assets/images/misc/t_ui_player_bot_icn"},
        {"WeaponsCache", "common/ui/hud/worldicons/assets/images/gamemodes/t_ui_menu_weaponscache"},
        {"JetsDeployIcon", "common/ui/hud/worldicons/assets/images/t_ui_wi_vehicle_airjetfighter"},
        {"bomb", "common/ui/hud/worldicons/assets/images/f2p/t_ui_iwi_demolitioncharge_lg_icn"},
    };
    int failures = 0;
    auto check = [&](bool condition, const char* label) {
        std::printf("%-62s %s\n", label, condition ? "ok" : "FAIL");
        if (!condition) ++failures;
    };
    check(count == 16 && filled == count && actual.size() == 16,
          "installed typed SVG list returns all 16 unique rows");
    bool expected_ok = true;
    for (const auto& row : expected)
    {
        const auto found = actual.find(row.first);
        expected_ok = expected_ok && found != actual.end() &&
                      found->second == row.second;
    }
    check(expected_ok, "all seven localized img operands join exact authored assets");
    check(actual.count("__bf6_inline_control_missing__") == 0,
          "fabricated operand control has no authored mapping");
    check(actual.count("customweapondrop") == 0,
          "case-mutated operand control does not gain a guessed mapping");

    int decoded = 0;
    for (const auto& row : expected)
    {
        bf6_rime_svg_info info{};
        const int status = bf6_rime_svg(context, row.second.c_str(), &info,
                         nullptr, 0, nullptr, 0) >= 0 &&
            info.canvas[0] > 0.f && info.canvas[1] > 0.f &&
            info.contour_count > 0 && info.point_count > 0;
        std::printf("  decode %-28s shapes=%d contours=%d points=%d canvas=%.0fx%.0f\n",
                    row.first.c_str(), info.shape_count, info.contour_count,
                    info.point_count, info.canvas[0], info.canvas[1]);
        if (status)
            ++decoded;
    }
    check(decoded == (int)expected.size(),
          "all seven joined assets decode through installed SVG resources");
    bf6_close(context);
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
