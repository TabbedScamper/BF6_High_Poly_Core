#include "armory_header_actions.h"
#include "bf6_core.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    const char* game = argc > 1 ? argv[1] :
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Battlefield 6";
    char error[1024]{};
    bf6_ctx* ctx = bf6_open(game, error, (int)sizeof(error));
    if (!ctx) { std::fprintf(stderr, "open: %s\n", error); return 2; }
    if (!bf6_mount_frontend(ctx, error, (int)sizeof(error))) {
        std::fprintf(stderr, "mount: %s\n", error); bf6_close(ctx); return 2;
    }

    std::vector<armory_header::Action> actions;
    armory_header::ReadStats stats{};
    const bool read = armory_header::read(ctx,
        "common/ui/weapons/uiviewmodel/menuweapon_viewmodel", actions, &stats);
    for (const auto& action : actions)
        std::printf("action sid=0x%08X text=%s occurrences=%d\n",
                    action.string_id, action.label.c_str(), action.source_occurrences);

    std::vector<armory_header::Action> fake_actions;
    armory_header::ReadStats fake_stats{};
    const bool fake = armory_header::read(ctx,
        "common/ui/weapons/uiviewmodel/__fabricated_control", fake_actions,
        &fake_stats);
    bf6_close(ctx);

    const char* expected[] = { "EQUIP", "Customize", "Firing Range" };
    bool exact = read && stats.exact_record_tiling && actions.size() == 3 &&
                 stats.label_flow_occurrences == 6 && stats.rejected_non_label == 1;
    for (size_t i = 0; i < actions.size() && i < 3; ++i)
        exact = exact && actions[i].label == expected[i] &&
                actions[i].source_occurrences == 2;
    const bool control = !fake && fake_actions.empty() &&
                         !fake_stats.exact_record_tiling;
    std::printf("armory-header-actions real=%zu/3 flow=%d/6 rejected=%d/1 "
                "fake=%d/1 exact=%d\n",
                actions.size(), stats.label_flow_occurrences,
                stats.rejected_non_label, control ? 0 : 1,
                stats.exact_record_tiling ? 1 : 0);
    return exact && control ? 0 : 1;
}
