#include "bf6_core.h"
#include "rime.h"
#include "rime_state_bridge.h"

#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace {

std::string path_key(const rime_state::OccurrencePath& path)
{
    std::string result;
    for (const auto& step : path)
    {
        result += step.owner_partition;
        result += '#';
        result += std::to_string(step.reference_instance);
        result += '/';
    }
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: rime_state_bridge_live_test <game-dir>\n");
        return 2;
    }
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, static_cast<int>(sizeof(error)));
    if (!context)
    {
        std::fprintf(stderr, "open: %s\n", error);
        return 1;
    }
    if (!bf6_mount_frontend(context, error, static_cast<int>(sizeof(error))))
    {
        std::fprintf(stderr, "mount: %s\n", error);
        bf6_close(context);
        return 1;
    }

    const char* root = "common/ui/weapons/screens/menuweaponscreen";
    bf6_rime_tree_stats stats{};
    const int count = bf6_rime_tree(context, root, 6, nullptr, 0, &stats);
    std::vector<bf6_rime_node> rows(static_cast<size_t>(count > 0 ? count : 0));
    const int got = count > 0
        ? bf6_rime_tree(context, root, 6, rows.data(), count, &stats) : count;
    rime::Screen screen;
    std::string adapter_error;
    const bool adapted = got == count &&
        rime::from_live(rows.data(), got, screen, adapter_error);
    const int compiled = adapted
        ? rime::load_interface_text_graphs(context, screen) : 0;
    const int fake_tree = bf6_rime_tree(
        context, "common/ui/__control__/not_a_real_screen", 6,
        nullptr, 0, nullptr);
    bf6_close(context);
    if (!adapted || compiled <= 0 || stats.unknown_types != 0)
    {
        std::fprintf(stderr,
            "tree failed count=%d got=%d compiled=%d unknown=%d error=%s\n",
            count, got, compiled, stats.unknown_types, adapter_error.c_str());
        return 1;
    }

    using LocalKey = std::pair<std::string, int>;
    std::map<LocalKey, std::vector<size_t>> groups;
    std::vector<rime_state::OccurrencePath> paths(screen.elements.size());
    int info_view_elements = 0;
    int info_view_local_three = 0;
    const std::string info_view =
        "common/ui/metacore/metacustomization/views/"
        "metacustomization_infoview";
    for (size_t index = 0; index < screen.elements.size(); ++index)
    {
        if (!rime_state::occurrence_path(screen, index, paths[index])) continue;
        const rime::Element& element = screen.elements[index];
        if (element.partition == info_view && paths[index].size() == 1 &&
            paths[index][0].owner_partition == root &&
            paths[index][0].reference_instance == 8)
        {
            ++info_view_elements;
            if (element.instance == 3) ++info_view_local_three;
        }
        if (paths[index].empty() || element.instance < 0) continue;
        groups[{element.partition, element.instance}].push_back(index);
    }

    size_t first = screen.elements.size(), second = screen.elements.size();
    for (const auto& entry : groups)
    {
        const auto& candidates = entry.second;
        for (size_t left = 0; left < candidates.size(); ++left)
            for (size_t right = left + 1; right < candidates.size(); ++right)
                if (path_key(paths[candidates[left]]) !=
                    path_key(paths[candidates[right]]))
                {
                    first = candidates[left];
                    second = candidates[right];
                    break;
                }
        if (first != screen.elements.size()) break;
    }
    if (first == screen.elements.size())
    {
        std::fprintf(stderr, "no repeated current-install occurrence found\n");
        return 1;
    }

    const rime::Element& first_element = screen.elements[first];
    rime_state::StateSlot first_slot;
    first_slot.partition = first_element.partition;
    first_slot.occurrence = paths[first];
    first_slot.local_instance = first_element.instance;
    first_slot.field = rime::property_hash("Alpha");
    first_slot.value = rime_state::Value::from_real(0.25);
    rime_state::StateSlot second_slot = first_slot;
    second_slot.occurrence = paths[second];
    second_slot.value = rime_state::Value::from_real(0.75);

    const rime_state::ApplyReport exact =
        rime_state::apply_state(screen, {first_slot, second_slot});
    const bool exact_values = exact.applied_slots == 2 &&
        screen.elements[first].alpha == 0.25f &&
        screen.elements[second].alpha == 0.75f;

    rime::Screen fake_screen = screen;
    rime_state::StateSlot fake_slot = first_slot;
    fake_slot.occurrence.back().reference_instance ^= 0x40000000;
    const rime_state::ApplyReport fake =
        rime_state::apply_state(fake_screen, {fake_slot});
    const bool fake_rejected = fake.applied_slots == 0 &&
        fake.missing_occurrences == 1 &&
        fake_screen.elements[first].alpha == 0.25f;

    std::printf(
        "rows=%d compiled=%d repeated=%s:%d path_a=%s path_b=%s "
        "exact=%d fake=%d info_view=%d local_three=%d fake_tree=%d unknown=%d\n",
        count, compiled, first_element.partition.c_str(), first_element.instance,
        path_key(paths[first]).c_str(), path_key(paths[second]).c_str(),
        exact.applied_slots, fake.applied_slots, info_view_elements,
        info_view_local_three, fake_tree, stats.unknown_types);
    return exact_values && fake_rejected && info_view_elements > 0 &&
        info_view_local_three == 1 && fake_tree < 0 ? 0 : 1;
}
