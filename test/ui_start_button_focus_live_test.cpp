#include "bf6_core.h"
#include "rime.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct LineState {
    std::string partition;
    int instance = -1;
    float start = 0.f;
    float end = 0.f;
    float glow = 0.f;
    bool visible = false;
};

std::vector<LineState> lines(const rime::Screen& screen)
{
    std::vector<LineState> result;
    for (const rime::Element& element : screen.elements)
        if (element.kind == rime::Kind::Line &&
            element.partition.find("cl_border_focus") != std::string::npos)
            result.push_back({element.partition, element.instance,
                              element.line_start_progress,
                              element.line_end_progress,
                              element.line_glow_size, element.visible});
    return result;
}

int differences(const std::vector<LineState>& left,
                const std::vector<LineState>& right)
{
    if (left.size() != right.size()) return -1;
    int changed = 0;
    for (size_t index = 0; index < left.size(); ++index)
    {
        const LineState& a = left[index];
        const LineState& b = right[index];
        if (a.partition != b.partition || a.instance != b.instance) return -1;
        changed += std::fabs(a.start - b.start) > 1e-6f ||
                   std::fabs(a.end - b.end) > 1e-6f ||
                   std::fabs(a.glow - b.glow) > 1e-6f ||
                   a.visible != b.visible;
    }
    return changed;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: ui_start_button_focus_live_test <game>\n");
        return 2;
    }
    constexpr const char* kRoot =
        "game/glacierflow/flow_mainmenu/ui/screens/bootflowstartscreen";
    constexpr const char* kInteractable =
        "common/ui/componentlibrary/shared/interaction/cb_interactable_advanced";
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, sizeof(error));
    if (!context || !bf6_mount_frontend(context, error, sizeof(error)))
    {
        std::fprintf(stderr, "mount: %s\n", error);
        if (context) bf6_close(context);
        return 1;
    }
    bf6_rime_tree_stats stats{};
    const int count = bf6_rime_tree(context, kRoot, 6, nullptr, 0, &stats);
    std::vector<bf6_rime_node> rows(static_cast<size_t>(count > 0 ? count : 0));
    const int got = count > 0
        ? bf6_rime_tree(context, kRoot, 6, rows.data(), count, &stats) : count;
    rime::Screen base;
    std::string adapter_error;
    if (got != count || !rime::from_live(rows.data(), got, base, adapter_error))
    {
        std::fprintf(stderr, "tree: %s\n", adapter_error.c_str());
        bf6_close(context);
        return 1;
    }
    rime::load_shapes(context, base);
    const int graphs = rime::load_interface_text_graphs(context, base);
    const int defaults = rime::apply_interface_defaults(base);
    const int fake_tree = bf6_rime_tree(
        context, "game/glacierflow/flow_mainmenu/ui/screens/__fake_start__",
        6, nullptr, 0, nullptr);
    bf6_close(context);

    const std::vector<LineState> before = lines(base);
    int interactable_graphs = 0;
    int focus_sources = 0;
    for (const rime::Screen::InterfaceTextGraph& graph :
         base.interface_text_graphs)
        if (graph.partition == kInteractable)
        {
            ++interactable_graphs;
            for (const bf6_rime_connection& connection : graph.connections)
                if (connection.source_field == rime::property_hash("IsFocused"))
                {
                    ++focus_sources;
                    std::printf("focus source=%d target=%d target-field=0x%08X\n",
                                connection.source, connection.target,
                                connection.target_field);
                }
            std::printf("interactable scope=%d interfaces=%zu connections=%zu\n",
                        graph.scope, graph.interfaces.size(),
                        graph.connections.size());
            if (graph.scope >= 0 &&
                static_cast<size_t>(graph.scope) < base.elements.size())
            {
                const rime::Element& owner =
                    base.elements[static_cast<size_t>(graph.scope)];
                std::printf("scope owner part=%s instance=%d name=%s parent=%d\n",
                            owner.partition.c_str(), owner.instance,
                            owner.name.c_str(), owner.parent);
            }
        }
    rime::Screen exact = base;
    int exact_ambiguous = 0;
    const uint32_t focus = rime::property_hash("IsFocused");
    const int exact_applied = rime::set_provider_bool(
        exact, kInteractable, focus, true, &exact_ambiguous);
    const std::vector<LineState> after = lines(exact);
    int bridged_focus_inputs = 0;
    for (const rime::Screen::InterfaceTextGraph& graph : exact.interface_text_graphs)
        if (graph.partition.find("cl_border_focus") != std::string::npos ||
            graph.partition.find("cb_buttonbase") != std::string::npos ||
            graph.partition.find("cl_labelbutton") != std::string::npos)
        {
            for (const rime::Screen::InterfaceTextGraph::Input& input : graph.inputs)
                if (input.field == focus)
                {
                    ++bridged_focus_inputs;
                    std::printf("focus input part=%s scope=%d source=%d kind=%d value=%d\n",
                                graph.partition.c_str(), graph.scope,
                                input.source_instance, input.kind,
                                input.boolean ? 1 : 0);
                }
        }
    int focus_element_changes = 0;
    for (size_t i = 0; i < base.elements.size() && i < exact.elements.size(); ++i)
    {
        const rime::Element& a = base.elements[i];
        const rime::Element& b = exact.elements[i];
        if (a.partition.find("cl_border_focus") == std::string::npos) continue;
        if (a.visible != b.visible || std::fabs(a.alpha - b.alpha) > 1e-6f)
        {
            ++focus_element_changes;
            std::printf("focus element %s kind=%d v:%d->%d alpha:%.3f->%.3f\n",
                        a.name.c_str(), static_cast<int>(a.kind),
                        a.visible ? 1 : 0, b.visible ? 1 : 0,
                        a.alpha, b.alpha);
        }
    }
    rime::Screen control = base;
    int control_ambiguous = 0;
    const int control_applied = rime::set_provider_bool(
        control, kInteractable, focus ^ 1u, true, &control_ambiguous);
    const int exact_changes = differences(before, after);
    const int control_changes = differences(before, lines(control));
    std::printf(
        "rows=%d graphs=%d defaults=%d lines=%zu interactable-graphs=%d "
        "focus-sources=%d bridged-focus-inputs=%d focus-element-changes=%d exact-applied=%d "
        "exact-line-changes=%d exact-ambiguous=%d xor-applied=%d "
        "xor-line-changes=%d xor-ambiguous=%d fake-tree=%d unknown=%d\n",
        got, graphs, defaults, before.size(), interactable_graphs,
        focus_sources, bridged_focus_inputs, focus_element_changes,
        exact_applied, exact_changes,
        exact_ambiguous, control_applied, control_changes, control_ambiguous,
        fake_tree, stats.unknown_types);

    return got > 0 && graphs > 0 && !before.empty() &&
        exact_applied > 0 && bridged_focus_inputs >= 2 &&
        exact_changes == 0 && exact_ambiguous == 0 &&
        control_applied == 0 && control_changes == 0 &&
        control_ambiguous == 0 && fake_tree < 0 && stats.unknown_types == 0
        ? 0 : 1;
}
