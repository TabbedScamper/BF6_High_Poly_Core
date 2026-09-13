#include "authored_event_dispatch.h"

#include <algorithm>

namespace bf6_ui {
namespace {

bool same_source(const AuthoredEventEdge& edge,
                 const AuthoredEventSource& source)
{
    return edge.partition == source.partition &&
           edge.occurrence == source.occurrence &&
           edge.source_instance == source.instance &&
           edge.source_event == source.event;
}

} // namespace

bool exact_authored_fanout(const AuthoredScreenDocument& document,
                           const AuthoredEventSource& source,
                           AuthoredEventBatch& out,
                           std::string& error)
{
    error.clear();
    out = {};
    out.source = source;
    if (source.partition.empty() || source.instance < 0) {
        error = "authored event source identity is incomplete";
        return false;
    }
    for (const AuthoredEventEdge& edge : document.event_edges())
        if (same_source(edge, source)) out.edges.push_back(edge);
    if (out.edges.empty()) {
        error = "no exact outgoing authored edge for emitted source";
        return false;
    }
    return true;
}

bool exact_focused_activation(const AuthoredScreenDocument& document,
                              const ElementAddress& focused_source,
                              uint32_t emitted_event,
                              AuthoredEventBatch& out,
                              std::string& error)
{
    if (!document.contains(focused_source)) {
        error = "activation source is not an exact element of the document";
        out = {};
        return false;
    }
    return exact_authored_fanout(
        document,
        {focused_source.partition, focused_source.occurrence,
         focused_source.local_instance, emitted_event},
        out, error);
}

std::vector<ProviderHandoff> exact_provider_handoffs(
    const AuthoredEventBatch& batch,
    const std::vector<InterfaceEventOutput>& outputs)
{
    std::vector<ProviderHandoff> result;
    for (const AuthoredEventEdge& edge : batch.edges) {
        const auto found = std::find_if(
            outputs.begin(), outputs.end(),
            [&edge](const InterfaceEventOutput& output) {
                return output.partition == edge.partition &&
                       output.occurrence == edge.occurrence &&
                       output.interface_instance == edge.target_instance &&
                       output.event == edge.target_event;
            });
        if (found != outputs.end()) result.push_back({edge, *found});
    }
    return result;
}

} // namespace bf6_ui
