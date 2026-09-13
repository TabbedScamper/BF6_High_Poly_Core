#pragma once

#include "bf6_core.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bf6_rime_runtime {

enum class ValueKind { Null, Bool, Int, UInt, Real, String };

struct Value {
    ValueKind kind = ValueKind::Null;
    bool boolean = false;
    int64_t integer = 0;
    uint64_t unsigned_integer = 0;
    double real = 0.0;
    std::string string;

    static Value from_bool(bool value);
    static Value from_int(int64_t value);
    static Value from_uint(uint64_t value);
    static Value from_real(double value);
    static Value from_string(std::string value);
    bool operator==(const Value& other) const;
    bool operator!=(const Value& other) const { return !(*this == other); }
};

struct OccurrenceStep {
    std::string owner_partition;
    int32_t reference_instance = -1;
    bool operator==(const OccurrenceStep& other) const;
    bool operator<(const OccurrenceStep& other) const;
};
using OccurrencePath = std::vector<OccurrenceStep>;

/* Portable identity of one property slot. Instance ids are local to a
 * partition, so the occurrence path is mandatory whenever a widget or logic
 * prefab is instantiated more than once. */
struct Address {
    std::string partition;
    OccurrencePath occurrence;
    int32_t instance = -1;
    uint32_t field = 0;

    bool operator==(const Address& other) const;
    bool operator<(const Address& other) const;
};

struct EventAddress {
    std::string partition;
    OccurrencePath occurrence;
    int32_t instance = -1;
    uint32_t event = 0;
};

struct Commit {
    Address address;
    Value value;
};

struct LayoutCommit {
    Address address;
    bf6_rime_axis layout{};
    int32_t timeline_instance = -1;
};

struct EventDelivery {
    EventAddress source;
    EventAddress target;
    int32_t realm = 0;
};

struct CompileReport {
    int tree_rows = 0;
    int graph_occurrences = 0;
    int logic_prefab_occurrences = 0;
    int property_edges = 0;
    int event_edges = 0;
    int interface_fields = 0;
    int operator_nodes = 0;
    int float_interpolator_nodes = 0;
    int linear_interpolator_nodes = 0;
    int eased_interpolator_nodes = 0; /* authored Quad/Cubic */
    int declined_interpolator_nodes = 0;
    int cycle_references_declined = 0;
    int depth_references_declined = 0;
    int unreadable_partitions = 0;
    int ambiguous_interface_pins_declined = 0;
    int parent_to_child_pins = 0;
    int child_to_parent_pins = 0;
    int idle_input_behavior_nodes = 0;
    int idle_input_behavior_outputs = 0;
    int input_behavior_gate_control_matches = 0;
    int compare_bool_nodes = 0;
    int declined_compare_bool_nodes = 0;
    int timeline_nodes = 0;
    int layout_track_nodes = 0;
    int targeted_layout_track_nodes = 0;
    int declined_layout_track_nodes = 0;
};

struct TickReport {
    int iterations = 0;
    int propagated_edges = 0;
    int executed_operators = 0;
    int ambiguous_slots = 0;
    int unresolved_operator_nodes = 0;
    int initialized_float_interpolators = 0;
    int advanced_float_interpolators = 0;
    int completed_float_interpolators = 0;
    int emitted_compare_bool_events = 0;
    int delivered_compare_bool_events = 0;
    int started_timelines = 0;
    int active_timelines = 0;
    int completed_timelines = 0;
    int sampled_layout_tracks = 0;
    bool converged = false;
};

/* Engine-neutral, current-install Rime property/event runtime.
 *
 * compile() walks the live screen tree and recursively mounts every visual
 * and logic-prefab occurrence. No exported table or screen-specific rule is
 * accepted. set() supplies provider state at an exact authored slot; tick()
 * rebuilds the transaction from shipped defaults and provider state, executes
 * every currently proven operator, and returns renderer-facing commits.
 */
class Runtime {
public:
    explicit Runtime(bf6_ctx* context);
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    bool compile(const std::string& root_partition, int max_depth,
                 std::string& error);
    /* Duplicate a compiled, read-free runtime occurrence graph.  Used for
     * repeated authored cells that share one installed template but require
     * independent focus/timeline state. */
    std::unique_ptr<Runtime> clone() const;
    void clear();

    bool set(const Address& address, Value value, std::string& error);
    int set_public(const std::string& partition,
                   const OccurrencePath& occurrence, uint32_t field,
                   Value value, std::string& error);
    bool erase(const Address& address);

    TickReport tick(double delta_seconds);
    std::optional<Value> get(const Address& address) const;
    std::vector<Address> public_slots(uint32_t field = 0) const;
    const std::vector<Commit>& commits() const { return commits_; }
    const std::vector<EventDelivery>& emitted_events() const {
        return emitted_events_;
    }
    const std::vector<LayoutCommit>& layout_commits() const {
        return layout_commits_;
    }

    std::vector<EventDelivery> emit(const EventAddress& source) const;
    const CompileReport& compile_report() const { return compile_report_; }
    const std::string& root_partition() const { return root_partition_; }
    double time_seconds() const { return time_seconds_; }

    static uint32_t hash(const char* name);

private:
    struct Impl;
    bf6_ctx* context_ = nullptr;
    std::string root_partition_;
    CompileReport compile_report_{};
    double time_seconds_ = 0.0;
    std::map<Address, Value> provider_values_;
    std::map<Address, Value> resolved_values_;
    std::vector<Commit> commits_;
    std::vector<EventDelivery> emitted_events_;
    std::vector<LayoutCommit> layout_commits_;
    std::unique_ptr<Impl> impl_;
};

} // namespace bf6_rime_runtime
