#include "bf6_core.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

bool read_grid(bf6_ctx* ctx, const char* partition, bf6_rime_node& out)
{
    bf6_rime_tree_stats stats{};
    const int count = bf6_rime_tree(ctx, partition, 1, nullptr, 0, &stats);
    if (count <= 0) return false;
    std::vector<bf6_rime_node> nodes(static_cast<size_t>(count));
    if (bf6_rime_tree(ctx, partition, 1, nodes.data(), count, &stats) != count)
        return false;
    const auto it = std::find_if(nodes.begin(), nodes.end(), [](const auto& n) {
        return std::strcmp(n.name, "DiceUIUniformGridList") == 0;
    });
    if (it == nodes.end()) return false;
    out = *it;
    return true;
}

bool candidate_is(const bf6_rime_node& grid, int index, const char* expected)
{
    return index >= 0 && index < grid.item_template_count && index < 8 &&
           std::strcmp(grid.item_templates[index], expected) == 0;
}

bool contains(const bf6_rime_node& grid, const char* value)
{
    for (int index = 0; index < grid.item_template_count && index < 8; ++index)
        if (std::strcmp(grid.item_templates[index], value) == 0) return true;
    return false;
}

void print_grid(const char* label, const bf6_rime_node& grid)
{
    std::printf("grid\t%s\tcount=%d\tfirst=%s\n", label,
                grid.item_template_count, grid.item_template);
    for (int index = 0; index < grid.item_template_count && index < 8; ++index)
        std::printf("candidate\t%s\t%d\t%s\n", label, index,
                    grid.item_templates[index]);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr,
                     "usage: home_template_route_proof_test <game-dir>\n");
        return 2;
    }

    char error[512]{};
    bf6_ctx* ctx = bf6_open(argv[1], error, static_cast<int>(sizeof(error)));
    if (!ctx) {
        std::fprintf(stderr, "open failed: %s\n", error);
        return 2;
    }
    if (!bf6_mount_frontend(ctx, error, static_cast<int>(sizeof(error)))) {
        std::fprintf(stderr, "mount failed: %s\n", error);
        bf6_close(ctx);
        return 2;
    }

    /* Current-install raw EFIX ownership oracle (2026-09-05):
     *
     * gamecardrowcell inst[8] +0x158 / 0xBF9C282E = [import 15],
     * where import 15 is gamecardpagecell.  The PagePips list, not this grid,
     * owns import 0 gamecardpagepipcell.
     *
     * gamecardpagecell inst[2] +0x158 / 0xBF9C282E =
     * [import 1, import 0, import 6, import 5], respectively gamecardcell,
     * storecardcell, deeplinkcardcell and challengelinkcardcell.
     *
     * This is deliberately a red regression gate until bf6_rime_tree stops
     * merging ItemTemplate arrays from sibling list instances. */
    bf6_rime_node row{};
    bf6_rime_node page{};
    const bool have_row = read_grid(
        ctx, "common/ui/home/widgets/gamecardrowcell", row);
    const bool have_page = read_grid(
        ctx, "common/ui/home/widgets/gamecardpagecell", page);
    if (have_row) print_grid("row", row);
    if (have_page) print_grid("page", page);

    const bool row_exact = have_row && row.item_template_count == 1 &&
        candidate_is(row, 0, "common/ui/home/widgets/gamecardpagecell");
    const bool page_exact = have_page && page.item_template_count == 4 &&
        candidate_is(page, 0, "common/ui/home/widgets/gamecardcell") &&
        candidate_is(page, 1, "common/ui/store/widgets/storecardcell") &&
        candidate_is(page, 2, "common/ui/menu/widgets/deeplinkcardcell") &&
        candidate_is(page, 3, "common/ui/menu/widgets/challengelinkcardcell");

    const bool row_wrong_list = have_row &&
        contains(row, "common/ui/home/widgets/gamecardpagepipcell");
    const bool row_shuffled = have_row &&
        contains(row, "common/ui/home/widgets/gamecardcell");
    const bool page_wrong_list = have_page &&
        (contains(page, "common/ui/home/widgets/gamecardpagepipcell") ||
         contains(page, "common/ui/home/widgets/gamecardpagecell"));
    std::printf("score\texact-row=%d\texact-page=%d\t"
                "wrong-row-list=%d\tshuffled-row=%d\twrong-page-list=%d\n",
                row_exact, page_exact, row_wrong_list, row_shuffled,
                page_wrong_list);

    bf6_rime_tree_stats fake_stats{};
    const int fake = bf6_rime_tree(
        ctx, "common/ui/home/widgets/__control_missing", 1, nullptr, 0,
        &fake_stats);
    std::printf("control\tfake-partition\tcount=%d\n", fake);

    bf6_close(ctx);
    return row_exact && page_exact && !row_wrong_list && !row_shuffled &&
                   !page_wrong_list && fake < 0
               ? 0
               : 1;
}
