#pragma once

#include "screen_router.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bf6_ui {

/* One event actually emitted by a decoded Rime node.  This is intentionally
 * more specific than an input concept: current data does not prove a generic
 * ConceptActivate -> event-pin mapping. */
struct AuthoredEventSource {
    std::string partition;
    std::vector<OccurrenceStep> occurrence;
    int32_t instance = -1;
    uint32_t event = 0;
};

struct AuthoredEventBatch {
    AuthoredEventSource source;
    std::vector<AuthoredEventEdge> edges;

    /* EventConnections prove membership and polarity, but not queue order or
     * re-entrancy.  A consumer must not turn vector order into a runtime law. */
    bool ordering_resolved = false;
    bool route_mutation_authorized = false;
};

/* Select every and only byte-exact outgoing edge for one emitted source.
 * This performs one graph hop.  It never recursively treats a target input as
 * a source output; the target node's decoded evaluator must emit that output. */
bool exact_authored_fanout(const AuthoredScreenDocument& document,
                           const AuthoredEventSource& source,
                           AuthoredEventBatch& out,
                           std::string& error);

/* Convenience gate for a focus manager that has proved the focused element
 * is itself the emitting node.  WidgetReference/input-behaviour indirection
 * must be resolved before calling this function. */
bool exact_focused_activation(const AuthoredScreenDocument& document,
                              const ElementAddress& focused_source,
                              uint32_t emitted_event,
                              AuthoredEventBatch& out,
                              std::string& error);

struct InterfaceEventOutput {
    std::string partition;
    std::vector<OccurrenceStep> occurrence;
    int32_t interface_instance = -1;
    uint32_t event = 0;
};

struct ProviderHandoff {
    AuthoredEventEdge edge;
    InterfaceEventOutput output;
};

/* InterfaceDescriptor output ports are an exact provider boundary.  Reaching
 * one reports a handoff only: it does not name or select another screen. */
std::vector<ProviderHandoff> exact_provider_handoffs(
    const AuthoredEventBatch& batch,
    const std::vector<InterfaceEventOutput>& outputs);

} // namespace bf6_ui
