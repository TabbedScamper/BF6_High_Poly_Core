#include "bf6_core.h"
#include "rime.h"
#include "rime_list_provider.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char* kClassDbd =
    "common/ui/loadout/shared/assets/databindings/loadoutclassitemdbd";
constexpr const char* kCell =
    "common/ui/metacore/metacustomization/widgets/"
    "metacustomization_griditemcell";
constexpr const char* kFooterView =
    "common/ui/metacore/metacustomization/views/"
    "metacustomization_navigationfooterview";
constexpr const char* kCollapseDbd =
    "common/ui/universalmenu/assets/databindings/um_collapsebuttondata";
constexpr const char* kCollapseCell =
    "common/ui/universalmenu/widgets/visual/um_collapsebuttonlistcell";

rime_list::Value boolean_value(bool value)
{
    rime_list::Value result;
    result.kind = rime_list::ValueKind::Bool;
    result.boolean = value;
    return result;
}

rime_list::Value string_value(const char* value)
{
    rime_list::Value result;
    result.kind = rime_list::ValueKind::String;
    result.string = value ? value : "";
    return result;
}

rime_list::Value int_value(int64_t value)
{
    rime_list::Value result;
    result.kind = rime_list::ValueKind::Int;
    result.integer = value;
    return result;
}

rime_list::Value real_value(double value)
{
    rime_list::Value result;
    result.kind = rime_list::ValueKind::Real;
    result.real = value;
    return result;
}

bool load_cell(bf6_ctx* context, const char* partition,
               float width, float height, rime::Screen& cell)
{
    bf6_rime_tree_stats stats{};
    const int count = bf6_rime_tree(
        context, partition, 6, nullptr, 0, &stats);
    if (count <= 0) return false;
    std::vector<bf6_rime_node> rows(static_cast<size_t>(count));
    if (bf6_rime_tree(
            context, partition, 6, rows.data(), count, &stats) != count)
        return false;
    std::string error;
    if (!rime::from_live(rows.data(), count, cell, error)) return false;
    if (rime::load_interface_text_graphs(context, cell) <= 0) return false;
    rime::solve(cell, width, height);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr,
            "usage: loadout_class_cell_route_test <game-dir>\n");
        return 2;
    }
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, sizeof(error));
    if (!context || !bf6_mount_all(context, 1, error, sizeof(error)))
    {
        std::fprintf(stderr, "open/mount: %s\n", error);
        if (context) bf6_close(context);
        return 1;
    }

    rime_list::DbdContract contract{};
    rime::Screen template_cell{};
    const bool contract_ok = rime_list::load_contract(
        context, contract, kClassDbd);
    const bool cell_ok = load_cell(context, kCell, 152.f, 152.f,
                                   template_cell);
    std::printf("class-cell inputs contract=%d fields=%zu cell=%d rows=%zu\n",
        contract_ok ? 1 : 0, contract.fields.size(), cell_ok ? 1 : 0,
        template_cell.elements.size());

    const struct Probe {
        const char* name;
        rime_list::Value value;
    } probes[] = {
        {"Title", string_value("ASSAULT")},
        {"Description", string_value("FRONTLINE")},
        {"IsDisabled", boolean_value(false)},
        {"IsSelected", boolean_value(true)},
        {"IsFocused", boolean_value(true)},
    };

    bool passed = contract_ok && cell_ok;
    int routed = 0;
    for (const Probe& probe : probes)
    {
        const rime_list::DbdField* field =
            rime_list::contract_field(contract, probe.name);
        rime_list::Record record{};
        const bool supplied = field &&
            record.put(contract, field->property_id, probe.value);
        const rime_list::RouteReport real = supplied
            ? rime_list::route_contract(context, kClassDbd, kFooterView, kCell,
                                        record)
            : rime_list::RouteReport{};
        const rime_list::RouteReport shuffled = supplied
            ? rime_list::route_contract_shuffled_control(
                  context, kClassDbd, kFooterView, kCell, record)
            : rime_list::RouteReport{};
        rime::Screen bound = template_cell;
        const rime_list::BindReport binding = supplied
            ? rime_list::bind_record(bound, kCell, contract, record)
            : rime_list::BindReport{};
        std::printf(
            "%s supplied=%d route=%d shuffled=%d bind=%d/%d "
            "unrouted=%d ambiguous=%d unsupported=%d\n",
            probe.name, supplied ? 1 : 0, real.connected,
            shuffled.connected, binding.applied_fields,
            binding.applied_targets, binding.unrouted, binding.ambiguous,
            binding.unsupported_values);
        const bool exact = supplied && real.connected > 0 &&
            shuffled.connected == 0 && binding.applied_fields == 1 &&
            binding.applied_targets > 0 && binding.unrouted == 0 &&
            binding.ambiguous == 0 && binding.unsupported_values == 0;
        routed += exact ? 1 : 0;
    }

    // Direct LoadoutClassItemData -> GridItemCell is the negative boundary.
    // The installed um_loadoutnavigationlistgenerator instead constructs one
    // UM_CollapseButtonData record per class item before publishing the
    // NavigationFooter collection.  Exercise that exact positive terminal.
    rime_list::DbdContract grid_contract{};
    if (rime_list::load_item_contract(context, grid_contract))
    {
        rime_list::Record grid_record{};
        auto put_bool = [&](const char* name, bool value) {
            const rime_list::DbdField* field =
                rime_list::contract_field(grid_contract, name);
            return field && grid_record.put(
                grid_contract, field->property_id, boolean_value(value));
        };
        const rime_list::DbdField* header =
            rime_list::contract_field(grid_contract, "Header");
        if (header) grid_record.put(grid_contract, header->property_id,
                                    string_value("ASSAULT"));
        put_bool("IsHeaderVisible", true);
        put_bool("IsTextBackingVisible", true);
        put_bool("IsIconVisible", true);
        put_bool("IsTextureVisible", false);
        put_bool("IsTextureSourceVisible", false);
        put_bool("IsStateVisible", false);
        put_bool("IsTopTitleVisible", false);
        put_bool("IsSvgBaseImageVisible", false);
        put_bool("IsCategoryVisible", false);
        put_bool("IsEnabled", true);
        rime::Screen adapted = template_cell;
        rime_list::BindReport report = rime_list::bind_record(
            adapted, kCell, grid_contract, grid_record);
        rime::solve(adapted, 158.f, 158.f);
        std::printf("adapted-grid bind=%d/%d terminals:\n",
                    report.applied_fields, report.applied_targets);
        for (size_t i = 0; i < adapted.elements.size(); ++i)
        {
            const rime::Element& element = adapted.elements[i];
            if (element.kind != rime::Kind::Svg &&
                element.kind != rime::Kind::Texture &&
                element.kind != rime::Kind::Label &&
                element.kind != rime::Kind::Blur &&
                element.kind != rime::Kind::Border)
                continue;
            std::printf(
                "  row=%zu kind=%d parent=%d visible=%d solved=%d "
                "box=%.1f,%.1f..%.1f,%.1f name=%s part=%s image=%s text=%s\n",
                i, (int)element.kind, element.parent,
                element.visible ? 1 : 0, element.solved ? 1 : 0,
                element.x0, element.y0, element.x1, element.y1,
                element.name.c_str(), element.partition.c_str(),
                element.image_asset.c_str(), element.text.c_str());
        }
    }


    rime_list::DbdContract collapse_contract{};
    rime::Screen collapse_cell{};
    const bool collapse_contract_ok = rime_list::load_contract(
        context, collapse_contract, kCollapseDbd);
    const bool collapse_cell_ok = load_cell(
        context, kCollapseCell, 830.f, 164.f, collapse_cell);
    const Probe collapse_probes[] = {
        {"Label", string_value("ASSAULT")},
        {"IsDisabled", boolean_value(false)},
        {"IsSelected", boolean_value(true)},
        {"SvgSize", int_value(40)},
        {"IsVisible", boolean_value(true)},
        {"IsInteractionBlocked", boolean_value(false)},
        {"IsLabelHidden", boolean_value(false)},
        {"IsIconlHidden", boolean_value(false)},
        {"IsUnselectedLabelVisible", boolean_value(false)},
        {"IsProficiencyVisible", boolean_value(false)},
        {"IsFitWidthToContent", boolean_value(true)},
        {"StaticButtonWidth", real_value(0.0)},
        {"ButtonType", int_value(64)},
    };
    int collapse_routed = 0;
    bool disabled_uses_authored_enabled_default = false;
    for (const Probe& probe : collapse_probes)
    {
        const rime_list::DbdField* field =
            rime_list::contract_field(collapse_contract, probe.name);
        rime_list::Record record{};
        const bool supplied = field && record.put(
            collapse_contract, field->property_id, probe.value);
        rime::Screen real_cell = collapse_cell;
        const rime_list::BindReport real = supplied
            ? rime_list::bind_record(real_cell, kCollapseCell,
                                     collapse_contract, record)
            : rime_list::BindReport{};
        rime::Screen control_cell = collapse_cell;
        const rime_list::BindReport control = supplied
            ? rime_list::bind_record_shuffled_control(
                  control_cell, kCollapseCell, collapse_contract, record)
            : rime_list::BindReport{};
        const bool exact = supplied && real.applied_fields == 1 &&
            real.applied_targets > 0 && !real.unrouted && !real.ambiguous &&
            !real.unsupported_values && control.applied_fields == 0 &&
            control.applied_targets == 0;
        collapse_routed += exact ? 1 : 0;
        if (std::strcmp(probe.name, "IsDisabled") == 0)
            disabled_uses_authored_enabled_default = supplied &&
                !real.applied_fields && real.unrouted == 1 &&
                !real.ambiguous && !real.unsupported_values &&
                !control.applied_fields && !control.applied_targets;
        std::printf("collapse %s property=%08X provider=%08X supplied=%d bind=%d/%d control=%d/%d "
                    "unrouted=%d ambiguous=%d unsupported=%d\n",
                    probe.name, field ? field->property_id : 0,
                    field ? field->provider_id : 0, supplied ? 1 : 0,
                    real.applied_fields, real.applied_targets,
                    control.applied_fields, control.applied_targets,
                    real.unrouted, real.ambiguous, real.unsupported_values);
    }
    std::printf("collapse exact=%d/%zu contract=%d cell=%d\n",
                collapse_routed, std::size(collapse_probes),
                collapse_contract_ok ? 1 : 0, collapse_cell_ok ? 1 : 0);

    // Exercise the exact three-input label-visibility expression as one
    // provider transaction.  Per-field probes cannot distinguish the
    // interface's authored fallback from the computed output because the
    // expression deliberately waits until every wired producer is known.
    auto exact_visibility_program = [](const std::vector<bf6_rime_math_instruction>& rows,
                                       bool perturb) {
        const int code[] = {3, 60, 3, 60, 3, 11, 10, 11, 68};
        const int p1[] = {
            (int)0x900D2293u, 0, (int)0x9EEEFD25u, 1,
            (int)0x93D12C76u, 2, 2, 0, 1};
        const int p2[] = {0, 0, 0, 0, 0, 3, 1, 1, 0};
        const int result[] = {0, 0, 1, 2, 3, 2, 1, 0, 0};
        std::vector<const bf6_rime_math_instruction*> exact;
        for (const bf6_rime_math_instruction& row : rows)
            if (row.instance == 8) exact.push_back(&row);
        if (exact.size() != std::size(code)) return false;
        for (size_t i = 0; i < exact.size(); ++i)
        {
            const int wanted_code = code[i] + (perturb && i == 0 ? 1 : 0);
            if (exact[i]->instruction_index != (int)i ||
                exact[i]->code != wanted_code || exact[i]->param1 != p1[i] ||
                exact[i]->param2 != p2[i] || exact[i]->result != result[i])
                return false;
        }
        return true;
    };
    bool exact_visibility_guard = false;
    bool perturbed_visibility_control = false;
    for (const rime::Screen::InterfaceTextGraph& graph :
         collapse_cell.interface_text_graphs)
    {
        if (graph.partition != kCollapseCell) continue;
        std::printf("collapse graph scope=%d wires=%zu logic=%zu math=%zu "
                    "mathop=%zu defaults=%zu\n",
                    graph.scope, graph.connections.size(),
                    graph.logic_operations.size(),
                    graph.math_instructions.size(),
                    graph.math_operations.size(), graph.defaults.size());
        for (const bf6_rime_logic_operation& operation :
             graph.logic_operations)
            std::printf("  logic instance=%d operation=%d\n",
                        operation.instance, operation.operation);
        for (const bf6_rime_math_instruction& instruction :
             graph.math_instructions)
            std::printf("  math instance=%d ip=%d code=%d p1=%d p2=%d r=%d\n",
                        instruction.instance, instruction.instruction_index,
                        instruction.code, instruction.param1,
                        instruction.param2, instruction.result);
        exact_visibility_guard |= exact_visibility_program(
            graph.math_instructions, false);
        perturbed_visibility_control |= exact_visibility_program(
            graph.math_instructions, true);
    }
    std::printf("collapse terminal visibility guard=%d perturbed=%d\n",
                exact_visibility_guard ? 1 : 0,
                perturbed_visibility_control ? 1 : 0);
    for (bool selected : {false, true})
    for (bool show_unselected : {false, true})
    for (bool hidden : {false, true})
    {
        rime_list::Record state_record{};
        auto put_state = [&](const char* name, bool value) {
            const rime_list::DbdField* field =
                rime_list::contract_field(collapse_contract, name);
            return field && state_record.put(
                collapse_contract, field->property_id,
                boolean_value(value));
        };
        put_state("IsSelected", selected);
        put_state("IsUnselectedLabelVisible", show_unselected);
        put_state("IsLabelHidden", hidden);
        rime::Screen state_cell = collapse_cell;
        const rime_list::BindReport state_bind =
            rime_list::bind_record_transaction(
                state_cell, kCollapseCell, collapse_contract, state_record);
        rime::Screen shuffled_cell = collapse_cell;
        const rime_list::BindReport shuffled_bind =
            rime_list::bind_record_transaction_shuffled_control(
                shuffled_cell, kCollapseCell, collapse_contract,
                state_record);
        std::printf("state selected=%d unselected-visible=%d hidden=%d "
                    "bind=%d/%d control=%d/%d\n",
                    selected ? 1 : 0, show_unselected ? 1 : 0,
                    hidden ? 1 : 0, state_bind.applied_fields,
                    state_bind.applied_targets, shuffled_bind.applied_fields,
                    shuffled_bind.applied_targets);
        for (const rime::Element& element : state_cell.elements)
        {
            if (element.name != "Label" && element.name != "Svg" &&
                element.name != "FocusFill" && element.name != "BGFill" &&
                element.name != "Underscore")
                continue;
            std::printf("  %-10s visible=%d alpha=%.4f rgb=%06X "
                        "source=%d unresolved(v/a/c)=%d/%d/%d "
                        "size=%.1fx%.1f\n",
                        element.name.c_str(), element.visible ? 1 : 0,
                        element.alpha, element.color_rgb,
                        element.color_source,
                        element.runtime_visibility_unresolved ? 1 : 0,
                        element.runtime_alpha_unresolved ? 1 : 0,
                        element.runtime_color_unresolved ? 1 : 0,
                        element.width, element.height);
        }
    }
    bool have_fe_text = false;
    bool have_fe_text_invert = false;
    const int palette_count = bf6_rime_palette(context, nullptr, 0);
    if (palette_count > 0)
    {
        std::vector<bf6_rime_color> palette(
            static_cast<size_t>(palette_count));
        if (bf6_rime_palette(context, palette.data(), palette_count) ==
            palette_count)
            for (const bf6_rime_color& color : palette)
                if (std::strcmp(color.id,
                        "259c7d88-01e8-4e73-8788-a35b3bc33450") == 0 ||
                    std::strcmp(color.id,
                        "28bcf8c9-65d5-49f3-b2f6-18efe7e45f58") == 0 ||
                    std::strcmp(color.id,
                        "60d25b34-015c-4280-b5de-0ed5079af438") == 0 ||
                    std::strcmp(color.id,
                        "b75f7c5f-5d0a-4b9b-9dea-f654d908aea0") == 0 ||
                    std::strcmp(color.id,
                        "067c49c7-9e08-4a69-88d8-ba20e887dc54") == 0)
                {
                    std::printf("collapse palette id=%s name=%s rgb=%06X\n",
                                color.id, color.name, color.rgb);
                    have_fe_text |= std::strcmp(color.name, "FE-Text") == 0 &&
                        color.rgb == 0xBFCAD1u;
                    have_fe_text_invert |=
                        std::strcmp(color.name, "FE-TextInvert") == 0 &&
                        color.rgb == 0x000000u;
                }
    }
    passed = passed && collapse_contract_ok && collapse_cell_ok &&
        collapse_routed == static_cast<int>(std::size(collapse_probes)) - 1 &&
        disabled_uses_authored_enabled_default && exact_visibility_guard &&
        !perturbed_visibility_control && have_fe_text &&
        have_fe_text_invert;

    const rime_list::RouteReport fake = rime_list::route_contract(
        context,
        "common/ui/loadout/shared/assets/databindings/"
        "codex_fake_loadoutclassitemdbd",
        kFooterView, kCell, rime_list::Record{});
    std::printf("class-cell exact=%d/%zu fake=%d\n", routed,
                std::size(probes), fake.connected);
    const char* soldier_localization_probes[] = {
        "ASSAULT", "ENGINEER", "SUPPORT", "RECON",
        "FRONTLINE", "ANTI-VEHICLE", "SQUAD SUSTAIN",
        "RECONNAISSANCE"
    };
    for (const char* value : soldier_localization_probes)
    {
        const int count = bf6_localized_string_ids_by_text(
            context, value, nullptr, 0);
        std::printf("localized %s=%d\n", value, count);
    }
    // This string exists, but belongs to the adjacent vehicle tab rather than
    // LoadoutClassItems.  Keep it visibly separate in the regression output.
    const int vehicle_tab_localized = bf6_localized_string_ids_by_text(
        context, "VEHICLES", nullptr, 0);
    std::printf("localized vehicle-tab VEHICLES=%d\n",
                vehicle_tab_localized);
    const int fake_localized = bf6_localized_string_ids_by_text(
        context, "__BF6_CLASS_CELL_CONTROL_MISSING__", nullptr, 0);
    std::printf("localized fake=%d\n", fake_localized);
    passed = passed && fake.connected == 0;
    bf6_close(context);
    return passed ? 0 : 1;
}
