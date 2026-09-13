#include "bf6_core.h"
#include "rime_runtime.h"

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
int fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}
}

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: rime_runtime_install_test <game> [root]\n");
        return 2;
    }
    char native_error[1024]{};
    bf6_ctx* context = bf6_open(argv[1], native_error, sizeof(native_error));
    if (!context || !bf6_mount_frontend(
            context, native_error, sizeof(native_error))) {
        std::fprintf(stderr, "mount: %s\n", native_error);
        if (context) bf6_close(context);
        return 1;
    }

    const char* root = argc == 3 ? argv[2] :
        "game/glacierflow/flow_mainmenu/ui/screens/bootflowstartscreen";
    bf6_rime_tree_stats tree_stats{};
    const int tree_count = bf6_rime_tree(context, root, 8, nullptr, 0, &tree_stats);
    std::vector<bf6_rime_node> tree_rows(
        static_cast<size_t>(tree_count > 0 ? tree_count : 0));
    if (tree_count > 0)
        bf6_rime_tree(context, root, 8, tree_rows.data(), tree_count, &tree_stats);
    std::map<std::string, std::set<int32_t>> input_nodes;
    for (const auto& row : tree_rows)
        if (row.kind == BF6_RIME_INPUT_BEHAVIOR) {
            input_nodes[row.partition].insert(row.instance);
            std::printf("input-behavior partition=%s instance=%d name=%s guid=%s sig=0x%08X\n",
                        row.partition, row.instance, row.name, row.type_guid,
                        row.type_signature);
        }
    for (const auto& group : input_nodes) {
        const int count = bf6_rime_connections(
            context, group.first.c_str(), nullptr, 0);
        std::vector<bf6_rime_connection> edges(
            static_cast<size_t>(count > 0 ? count : 0));
        if (count > 0)
            bf6_rime_connections(context, group.first.c_str(), edges.data(), count);
        for (const auto& edge : edges)
            if (group.second.count(edge.source))
                std::printf("input-edge partition=%s source=%d field=0x%08X target=%d field=0x%08X mode=%d\n",
                            group.first.c_str(), edge.source, edge.source_field,
                            edge.target, edge.target_field, edge.mode);
    }
    const char* debug_partitions[] = {
        "common/ui/componentlibrary/components/buttons/cl_labelbutton",
        "common/ui/componentlibrary/components/buttons/cb_buttonbase",
        "common/ui/componentlibrary/shared/interaction/cb_interactable_advanced",
        "common/ui/componentlibrary/shared/visual/cb_fillvisual",
        "common/ui/componentlibrary/shared/colors/cb_colorlogic",
        "common/ui/componentlibrary/shared/logic/cb_colorinterpolationlogic",
    };
    for (const char* partition : debug_partitions) {
        std::printf("GRAPH %s\n", partition);
        bf6_rime_tree_stats graph_stats{};
        const int graph_count = bf6_rime_tree(context, partition, 1, nullptr, 0,
                                               &graph_stats);
        std::vector<bf6_rime_node> graph_rows(
            static_cast<size_t>(graph_count > 0 ? graph_count : 0));
        if (graph_count > 0)
            bf6_rime_tree(context, partition, 1, graph_rows.data(), graph_count,
                          &graph_stats);
        for (const auto& row : graph_rows)
            if (row.partition == std::string(partition))
                std::printf(" node inst=%d name=%s type=%s guid=%s kind=%d ref=%s color=%s rgb=%06X alpha=%.9g visible=%d\n",
                            row.instance, row.name, row.type_name, row.type_guid,
                            row.kind, row.reference, row.color_name,
                            row.color_rgb, row.alpha, row.visible);
        const int nc = bf6_rime_connections(context, partition, nullptr, 0);
        std::vector<bf6_rime_connection> connections(
            static_cast<size_t>(nc > 0 ? nc : 0));
        if (nc > 0)
            bf6_rime_connections(context, partition, connections.data(), nc);
        for (const auto& edge : connections)
            std::printf(" edge %d:0x%08X -> %d:0x%08X mode=%d\n",
                        edge.source, edge.source_field, edge.target,
                        edge.target_field, edge.mode);
        const int nr = bf6_rime_logic_references(context, partition, nullptr, 0);
        std::vector<bf6_rime_logic_reference> references(
            static_cast<size_t>(nr > 0 ? nr : 0));
        if (nr > 0)
            bf6_rime_logic_references(context, partition, references.data(), nr);
        for (const auto& reference : references)
            std::printf(" logic-ref inst=%d blueprint=%s\n",
                        reference.instance, reference.blueprint);
    }

    bf6_rime_runtime::Runtime runtime(context);
    std::string error;
    if (!runtime.compile(root, 8, error)) {
        std::fprintf(stderr, "compile: %s\n", error.c_str());
        bf6_close(context);
        return 1;
    }
    const auto report = runtime.compile_report();
    std::printf("initial commits=%zu\n", runtime.commits().size());
    for (const auto& commit : runtime.commits())
        if (commit.address.partition.find("button") != std::string::npos ||
            commit.address.partition.find("interactable") != std::string::npos)
        {
            std::printf("  %s occ=%zu inst=%d field=0x%08X kind=%d",
                        commit.address.partition.c_str(),
                        commit.address.occurrence.size(),
                        commit.address.instance, commit.address.field,
                        static_cast<int>(commit.value.kind));
            if (commit.value.kind == bf6_rime_runtime::ValueKind::Bool)
                std::printf(" bool=%d", commit.value.boolean ? 1 : 0);
            else if (commit.value.kind == bf6_rime_runtime::ValueKind::Real)
                std::printf(" real=%.9g", commit.value.real);
            else if (commit.value.kind == bf6_rime_runtime::ValueKind::Int)
                std::printf(" int=%lld", static_cast<long long>(commit.value.integer));
            else if (commit.value.kind == bf6_rime_runtime::ValueKind::UInt)
                std::printf(" uint=%llu", static_cast<unsigned long long>(commit.value.unsigned_integer));
            std::printf("\n");
        }
    if (report.tree_rows <= 0 || report.graph_occurrences <= 1 ||
        report.property_edges <= 0 || report.event_edges <= 0 ||
        report.logic_prefab_occurrences <= 0)
        return fail("live runtime did not compile the nested authored graph");
    if (report.idle_input_behavior_nodes <= 0 ||
        report.idle_input_behavior_outputs <= 0)
        return fail("native idle input behaviors were not compiled");
    if (report.input_behavior_gate_control_matches != 0)
        return fail("one-bit input behavior signature control matched");

    const uint32_t focused = bf6_rime_runtime::Runtime::hash("IsFocused");
    const auto focus_slots = runtime.public_slots(focused);
    if (focus_slots.empty()) return fail("live IsFocused interface was absent");
    std::set<bf6_rime_runtime::OccurrencePath> occurrences;
    for (const auto& address : focus_slots) occurrences.insert(address.occurrence);

    const auto initial = runtime.tick(0.0);
    if (!initial.converged) return fail("default graph transaction did not converge");
    if (!runtime.set(focus_slots.front(),
                     bf6_rime_runtime::Value::from_bool(true), error))
        return fail("exact live provider slot was rejected");
    const auto changed = runtime.tick(1.0 / 60.0);
    if (!changed.converged || runtime.commits().empty())
        return fail("provider transaction produced no converged commits");

    bf6_rime_runtime::Address fake = focus_slots.front();
    fake.occurrence.push_back({"common/ui/__fabricated__", 0x7fffffff});
    if (runtime.set(fake, bf6_rime_runtime::Value::from_bool(true), error))
        return fail("fabricated occurrence control was accepted");
    if (!runtime.public_slots(focused ^ 1u).empty())
        return fail("one-bit field control matched a live public slot");

    std::printf(
        "rows=%d scopes=%d logic=%d property_edges=%d event_edges=%d "
        "operators=%d public_focus=%zu focus_occurrences=%zu "
        "iterations=%d commits=%zu input_idle=%d/%d gate_control=%d "
        "fake=0 xor=0 ambiguous_pins=%d\n",
        report.tree_rows, report.graph_occurrences,
        report.logic_prefab_occurrences, report.property_edges,
        report.event_edges, report.operator_nodes, focus_slots.size(),
        occurrences.size(), changed.iterations, runtime.commits().size(),
        report.idle_input_behavior_nodes,
        report.idle_input_behavior_outputs,
        report.input_behavior_gate_control_matches,
        report.ambiguous_interface_pins_declined);
    bf6_close(context);
    return 0;
}
