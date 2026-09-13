#include "bf6_core.h"
#include "rime.h"
#include "rime_list_provider.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

bool load_screen(bf6_ctx* context, const char* partition,
                 rime::Screen& screen)
{
    bf6_rime_tree_stats stats{};
    const int count = bf6_rime_tree(
        context, partition, 8, nullptr, 0, &stats);
    if (count <= 0)
    {
        std::fprintf(stderr, "tree count=%d\n", count);
        return false;
    }
    std::vector<bf6_rime_node> rows((size_t)count);
    const int got = bf6_rime_tree(
        context, partition, 8, rows.data(), count, &stats);
    if (got != count)
        return false;
    std::string error;
    const bool adapted = rime::from_live(rows.data(), count, screen, error);
    const int compiled = adapted
        ? rime::load_interface_text_graphs(context, screen) : 0;
    if (!adapted || compiled <= 0)
        std::fprintf(stderr,
            "screen count=%d got=%d adapted=%d compiled=%d unknown=%d "
            "error=%s\n", count, got, adapted ? 1 : 0, compiled,
            stats.unknown_types, error.c_str());
    return adapted && compiled > 0;
}

int matching_label_count(const rime::Screen& screen, const char* text)
{
    int count = 0;
    for (const rime::Element& element : screen.elements)
        if (element.kind == rime::Kind::Label && element.name == "EnumLabel" &&
            element.text == text)
            ++count;
    return count;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr,
            "usage: rime_array_element_live_test <game-dir>\n");
        return 2;
    }
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, (int)sizeof(error));
    if (!context ||
        !bf6_mount_frontend(context, error, (int)sizeof(error)))
    {
        std::fprintf(stderr, "open/mount: %s\n", error);
        if (context) bf6_close(context);
        return 1;
    }

    constexpr const char* kCell =
        "common/ui/options/ui/widgets/buttons/options_enumbuttoncell";
    constexpr const char* kEnum =
        "common/ui/options/ui/widgets/options_enum";
    constexpr const char* kDbd =
        "common/ui/options/ui/assets/databindings/"
        "uioptionsenumdatabinding";

    rime::Screen screen;
    rime_list::DbdContract contract;
    const bool screen_loaded = load_screen(context, kCell, screen);
    const bool contract_loaded =
        rime_list::load_contract(context, contract, kDbd);
    const bool loaded = screen_loaded && contract_loaded;
    const rime_list::DbdField* strings =
        loaded ? rime_list::contract_field(contract, "EnumStrings") : nullptr;
    const int operators = bf6_rime_array_elements(context, kEnum, nullptr, 0);
    const int fake_operators = bf6_rime_array_elements(
        context, "common/ui/__control__/not_a_real_enum", nullptr, 0);
    bf6_close(context);
    if (!loaded || !strings)
    {
        std::fprintf(stderr,
            "live enum cell or DBD unavailable screen=%d contract=%d "
            "strings=%d operators=%d fake=%d\n",
            screen_loaded ? 1 : 0, contract_loaded ? 1 : 0,
            strings ? 1 : 0, operators, fake_operators);
        return 1;
    }

    const std::vector<std::string> choices{"ZERO", "ONE", "TWO"};
    int value_ambiguous = 0;
    int array_ambiguous = 0;
    const uint32_t value_pin = rime::property_hash("Value");
    const int value_targets = rime::set_interface_int_field(
        screen, kEnum, value_pin, 1, &value_ambiguous);
    const int array_targets = rime::set_interface_string_array_field(
        screen, kEnum, strings->property_id, choices, &array_ambiguous);
    const int selected = matching_label_count(screen, "ONE");
    rime::Screen control = screen;
    int array_control_ambiguous = 0;
    int value_control_ambiguous = 0;
    const int array_control_targets = rime::set_interface_string_array_field(
        control, kEnum, strings->property_id ^ 1u, choices,
        &array_control_ambiguous);
    const int value_control_targets = rime::set_interface_int_field(
        control, kEnum, value_pin ^ 1u, 1, &value_control_ambiguous);

    std::printf(
        "operators=%d fake=%d value-targets=%d array-targets=%d "
        "selected=%d ambiguous=%d/%d control=%d/%d/%d/%d\n",
        operators, fake_operators, value_targets, array_targets, selected,
        value_ambiguous, array_ambiguous, array_control_targets,
        value_control_targets, array_control_ambiguous,
        value_control_ambiguous);
    return operators == 1 && fake_operators < 0 && value_targets >= 0 &&
        array_targets > 0 && selected == 1 && !value_ambiguous &&
        !array_ambiguous && !array_control_targets &&
        !value_control_targets && !array_control_ambiguous &&
        !value_control_ambiguous ? 0 : 1;
}
