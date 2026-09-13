#include "bf6_core.h"
#include "rime.h"
#include "rime_state_process.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 4 && argc != 5)
    {
        std::fprintf(stderr,
            "usage: rime_state_process_live_test <game> <python> <capture-tool>\n");
        return 2;
    }
    const char* route = "common/ui/weapons/screens/menuweaponscreen";
    if (argc == 5 && std::string(argv[4]) == "--pipe-only")
    {
        rime::Screen screen;
        screen.partition = route;
        rime_state::LiveCaptureOptions options;
        options.game_directory = argv[1];
        options.python_executable = argv[2];
        options.capture_tool = argv[3];
        options.route = route;
        options.max_depth = 6;
        rime_state::LiveCaptureReport report;
        std::string capture_error;
        const bool captured = rime_state::capture_and_apply(
            screen, options, report, capture_error);
        std::printf(
            "pipe-only captured=%d exit=%lu wire=%d controls=%zu "
            "provider_resolved=%d provider_remaining=%d error=%s\n",
            captured ? 1 : 0, report.exit_code, report.wire.declared_slots,
            report.wire.controls.size(),
            report.wire.resolved_consumed_external_provider_slots,
            report.wire.remaining_consumed_external_provider_slots,
            capture_error.c_str());
        return captured && report.exit_code == 0 &&
            report.wire.declared_slots > 0 &&
            report.wire.provider_metadata_present &&
            report.wire.resolved_consumed_external_provider_slots == 1 &&
            report.wire.remaining_consumed_external_provider_slots == 10 &&
            report.wire.controls.count("passed") &&
            report.wire.controls.at("passed") == 1 ? 0 : 1;
    }
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, static_cast<int>(sizeof(error)));
    if (!context || !bf6_mount_frontend(context, error, static_cast<int>(sizeof(error))))
    {
        std::fprintf(stderr, "mount failed: %s\n", error);
        if (context) bf6_close(context);
        return 1;
    }
    bf6_rime_tree_stats stats{};
    constexpr int kDepth = 6;
    const int count = bf6_rime_tree(context, route, kDepth, nullptr, 0, &stats);
    std::vector<bf6_rime_node> rows(static_cast<size_t>(count > 0 ? count : 0));
    const int got = count > 0
        ? bf6_rime_tree(context, route, kDepth, rows.data(), count, &stats) : count;
    rime::Screen screen;
    std::string adapter_error;
    const bool adapted = got == count &&
        rime::from_live(rows.data(), got, screen, adapter_error);
    if (adapted) rime::load_interface_text_graphs(context, screen);
    if (!adapted)
    {
        std::fprintf(stderr, "tree failed: %s\n", adapter_error.c_str());
        bf6_close(context);
        return 1;
    }
    const char* referenced =
        "common/ui/metacore/metacustomization/views/"
        "metacustomization_infoview";
    int referenced_occurrence_elements = 0;
    int referenced_local_three = 0;
    for (size_t index = 0; index < screen.elements.size(); ++index)
    {
        const rime::Element& element = screen.elements[index];
        if (element.partition != referenced) continue;
        rime_state::OccurrencePath path;
        if (!rime_state::occurrence_path(screen, index, path) ||
            path.size() != 1 || path[0].owner_partition != route ||
            path[0].reference_instance != 8)
            continue;
        ++referenced_occurrence_elements;
        if (element.instance == 3) ++referenced_local_three;
    }
    bf6_rime_tree_stats referenced_stats{};
    const int referenced_count = bf6_rime_tree(
        context, referenced, kDepth, nullptr, 0, &referenced_stats);
    const int fake_tree = bf6_rime_tree(
        context, "common/ui/__control__/not_a_real_screen", kDepth,
        nullptr, 0, nullptr);
    if (argc == 5 && std::string(argv[4]) == "--tree-only")
    {
        for (size_t index = 0; index < screen.elements.size(); ++index)
        {
            const rime::Element& element = screen.elements[index];
            if (!(element.partition == route &&
                  !element.references_widget.empty()) && element.instance != 8 &&
                element.partition.find("metacustomization_infoview") ==
                    std::string::npos)
                continue;
            rime_state::OccurrencePath path;
            const bool valid = rime_state::occurrence_path(screen, index, path);
            std::printf("element=%zu kind=%d partition=%s instance=%d ref=%s path-valid=%d",
                        index, static_cast<int>(element.kind),
                        element.partition.c_str(), element.instance,
                        element.references_widget.c_str(), valid ? 1 : 0);
            for (const auto& step : path)
                std::printf(";%s@%d", step.owner_partition.c_str(),
                            step.reference_instance);
            std::printf("\n");
        }
        std::printf("referenced-root=%s tree=%d occurrence-elements=%d "
                    "local-three=%d unresolved=%d depth_limited=%d fake=%d\n",
                    referenced, referenced_count,
                    referenced_occurrence_elements, referenced_local_three,
                    referenced_stats.unresolved_refs,
                    referenced_stats.depth_limited, fake_tree);
        bf6_close(context);
        return 0;
    }
    bf6_close(context);

    rime_state::LiveCaptureOptions options;
    options.game_directory = argv[1];
    options.python_executable = argv[2];
    options.capture_tool = argv[3];
    options.route = route;
    options.max_depth = kDepth;
    rime_state::LiveCaptureReport report;
    std::string capture_error;
    const bool captured = rime_state::capture_and_apply(
        screen, options, report, capture_error);
    std::printf(
        "tree=%d depth_limited=%d captured=%d exit=%lu wire=%d omitted=%d applied=%d missing_occ=%d "
        "missing_inst=%d non_renderer=%d unsupported=%d types=%d controls=%zu "
        "requirements=%d external_requirements=%d consumed_external=%d "
        "provider_resolved=%d provider_remaining=%d "
        "referenced_elements=%d referenced_local_three=%d fake_tree=%d error=%s\n",
        got, stats.depth_limited, captured ? 1 : 0, report.exit_code,
        report.wire.declared_slots,
        report.wire.omitted_non_scalar_slots, report.applied.applied_slots,
        report.applied.missing_occurrences, report.applied.missing_instances,
        report.applied.non_renderer_targets,
        report.applied.unsupported_fields, report.applied.type_mismatches,
        report.wire.controls.size(), report.wire.host_requirement_slots,
        report.wire.external_host_requirement_slots,
        report.wire.consumed_external_host_requirement_slots,
        report.wire.resolved_consumed_external_provider_slots,
        report.wire.remaining_consumed_external_provider_slots,
        referenced_occurrence_elements,
        referenced_local_three, fake_tree, capture_error.c_str());
    const bool verbose = argc == 5 && std::string(argv[4]) == "--verbose";
    if (verbose)
        for (const auto& entry : report.applied.unsupported_field_counts)
            std::printf("unsupported-field=0x%08x count=%d\n",
                        entry.first, entry.second);
    for (const auto& entry : report.applied.missing_occurrence_field_counts)
        std::printf("missing-occurrence-field=0x%08x count=%d\n",
                    entry.first, entry.second);
    for (const auto& entry : report.applied.missing_instance_field_counts)
        std::printf("missing-instance-field=0x%08x count=%d\n",
                    entry.first, entry.second);
    for (const auto& slot : report.applied.missing_occurrence_examples)
    {
        std::printf("missing-example=%s", slot.partition.c_str());
        for (const auto& step : slot.occurrence)
            std::printf(";%s@%d", step.owner_partition.c_str(),
                        step.reference_instance);
        std::printf(" instance=%d field=0x%08x\n",
                    slot.local_instance, slot.field);
    }
    return captured && report.exit_code == 0 &&
        report.wire.declared_slots > 0 &&
        report.wire.requirement_metadata_present &&
        report.wire.provider_metadata_present &&
        report.wire.resolved_consumed_external_provider_slots == 1 &&
        report.wire.remaining_consumed_external_provider_slots == 10 &&
        report.applied.missing_occurrences == 0 &&
        report.applied.missing_instances == 0 &&
        referenced_occurrence_elements > 0 && referenced_local_three == 1 &&
        referenced_count > 1 && fake_tree < 0 &&
        report.wire.controls.count("passed") &&
        report.wire.controls.at("passed") == 1 ? 0 : 1;
}
