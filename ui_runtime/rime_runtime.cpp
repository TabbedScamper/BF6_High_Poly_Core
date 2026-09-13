#include "rime_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

namespace bf6_rime_runtime {
namespace {

std::string normalized(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    for (char& ch : value)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);
    if (value.size() > 4 && value.compare(value.size() - 4, 4, ".ebx") == 0)
        value.resize(value.size() - 4);
    return value;
}

Value public_value(const bf6_rime_interface_field& row)
{
    switch (row.value_kind) {
    case BF6_RIME_VALUE_BOOL: return Value::from_bool(row.bool_value != 0);
    case BF6_RIME_VALUE_INT: return Value::from_int(row.int_value);
    case BF6_RIME_VALUE_UINT: return Value::from_uint(row.uint_value);
    case BF6_RIME_VALUE_REAL: return Value::from_real(row.real_value);
    case BF6_RIME_VALUE_STRING: return Value::from_string(row.string_value);
    default: return {};
    }
}

bool as_bool(const Value& value, bool& out)
{
    switch (value.kind) {
    case ValueKind::Bool: out = value.boolean; return true;
    case ValueKind::Int: out = value.integer != 0; return true;
    case ValueKind::UInt: out = value.unsigned_integer != 0; return true;
    case ValueKind::Real: out = value.real != 0.0; return true;
    default: return false;
    }
}

bool as_real(const Value& value, double& out)
{
    switch (value.kind) {
    case ValueKind::Bool: out = value.boolean ? 1.0 : 0.0; return true;
    case ValueKind::Int: out = static_cast<double>(value.integer); return true;
    case ValueKind::UInt: out = static_cast<double>(value.unsigned_integer); return true;
    case ValueKind::Real: out = value.real; return std::isfinite(out);
    default: return false;
    }
}

struct ScopeKey {
    std::string partition;
    OccurrencePath occurrence;
    bool operator<(const ScopeKey& other) const {
        if (partition != other.partition) return partition < other.partition;
        return occurrence < other.occurrence;
    }
};

struct Edge { Address source, target; int32_t mode = 0; };
struct EventEdge { EventAddress source, target; int32_t mode = 0; };

struct CompareBoolNode {
    Address node;
    uint32_t input_field = 0;
    uint32_t on_true_event = 0;
    uint32_t on_false_event = 0;
    bool authored_value = false;
    bool always_send = false;
    bool trigger_on_property_change = false;
    bool trigger_on_start = false;
};

struct CompareBoolState {
    bool initialized = false;
    bool value = false;
};

struct LayoutTrackTemplate {
    bf6_rime_layout_track track{};
    std::vector<bf6_rime_layout_key> keys;
};

struct TimelineNode {
    Address node;
    bf6_rime_timeline timeline{};
};

struct LayoutTrackNode {
    Address timeline;
    Address target;
    std::vector<bf6_rime_layout_key> keys;
};

struct TimelineState {
    bool active = false;
    double time = 0.0;
};

struct Operator {
    enum class Kind { ConditionalFloat, ConditionalProperty, And, Or, Not,
                      PropertyDefault, FloatInterpolator,
                      InputBehaviorIdle };
    Kind kind = Kind::And;
    Address node;
    float true_value = 0.f;
    float false_value = 0.f;
    bool authored_condition = false;
    uint32_t true_field = 0;
    uint32_t false_field = 0;
    uint32_t out_field = 0;
    uint32_t input_field = 0;
    float default_value = 0.f;
    float duration = 0.f;
    float velocity = 0.f;
    int32_t use_velocity = 0;
    int32_t interpolation_type = 0;
    int32_t interpolation_mode = 0;
    int32_t dynamic_duration = 0;
    int32_t use_real_time_clock = 0;
    int32_t force_frame_correct_output = 0;
};

struct InterpolatorState {
    bool initialized = false;
    bool has_target = false;
    double start = 0.0;
    double target = 0.0;
    double output = 0.0;
    double elapsed = 0.0;
};

struct Scope {
    ScopeKey key;
    std::vector<int32_t> interfaces;
    std::vector<bf6_rime_interface_field> interface_fields;
    std::vector<bf6_rime_connection> local_edges;
};

struct PartitionTemplate {
    std::vector<int32_t> interfaces;
    std::vector<bf6_rime_interface_field> interface_fields;
    std::vector<bf6_rime_connection> edges;
    std::vector<bf6_rime_conditional_float> conditional_floats;
    std::vector<bf6_rime_float_interpolator> float_interpolators;
    std::vector<bf6_rime_conditional_property> conditional_properties;
    std::vector<bf6_rime_logic_operation> logic;
    std::vector<bf6_rime_logic_reference> references;
    std::vector<bf6_rime_event_connection> events;
    std::vector<bf6_rime_compare_bool> compare_bools;
    std::vector<bf6_rime_timeline> timelines;
    std::vector<LayoutTrackTemplate> layout_tracks;
};

template <typename T, typename F>
bool read_rows(F&& function, const std::string& partition,
               std::vector<T>& out)
{
    const int count = function(partition.c_str(), static_cast<T*>(nullptr), 0);
    if (count < 0) return false;
    out.resize(static_cast<size_t>(count));
    if (!count) return true;
    return function(partition.c_str(), out.data(), count) == count;
}

OccurrencePath row_occurrence(const std::vector<bf6_rime_node>& rows,
                              int row_index)
{
    std::vector<int> ancestors;
    int cursor = rows[static_cast<size_t>(row_index)].parent;
    std::set<int> seen;
    while (cursor >= 0 && cursor < static_cast<int>(rows.size()) &&
           seen.insert(cursor).second) {
        if (rows[static_cast<size_t>(cursor)].reference[0])
            ancestors.push_back(cursor);
        cursor = rows[static_cast<size_t>(cursor)].parent;
    }
    OccurrencePath result;
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        const bf6_rime_node& ref = rows[static_cast<size_t>(*it)];
        result.push_back({normalized(ref.partition), ref.instance});
    }
    return result;
}

} // namespace

struct Runtime::Impl {
    std::vector<Scope> scopes;
    std::map<ScopeKey, size_t> scope_index;
    std::map<std::string, PartitionTemplate> templates;
    std::vector<Edge> edges;
    std::vector<EventEdge> event_edges;
    std::vector<Operator> operators;
    std::map<Address, Value> defaults;
    std::map<Address, InterpolatorState> interpolators;
    std::vector<CompareBoolNode> compare_bools;
    std::map<Address, CompareBoolState> compare_bool_states;
    std::vector<TimelineNode> timelines;
    std::vector<LayoutTrackNode> layout_tracks;
    std::map<Address, TimelineState> timeline_states;

    Address address(const Scope& scope, int instance, uint32_t field) const {
        return {scope.key.partition, scope.key.occurrence, instance, field};
    }
};

Runtime::Runtime(bf6_ctx* context) : context_(context) {}

Value Value::from_bool(bool value) { Value v; v.kind = ValueKind::Bool; v.boolean = value; return v; }
Value Value::from_int(int64_t value) { Value v; v.kind = ValueKind::Int; v.integer = value; return v; }
Value Value::from_uint(uint64_t value) { Value v; v.kind = ValueKind::UInt; v.unsigned_integer = value; return v; }
Value Value::from_real(double value) { Value v; v.kind = ValueKind::Real; v.real = value; return v; }
Value Value::from_string(std::string value) { Value v; v.kind = ValueKind::String; v.string = std::move(value); return v; }

bool Value::operator==(const Value& other) const
{
    if (kind != other.kind) return false;
    switch (kind) {
    case ValueKind::Null: return true;
    case ValueKind::Bool: return boolean == other.boolean;
    case ValueKind::Int: return integer == other.integer;
    case ValueKind::UInt: return unsigned_integer == other.unsigned_integer;
    case ValueKind::Real: return real == other.real;
    case ValueKind::String: return string == other.string;
    }
    return false;
}

bool OccurrenceStep::operator==(const OccurrenceStep& other) const
{ return normalized(owner_partition) == normalized(other.owner_partition) &&
         reference_instance == other.reference_instance; }
bool OccurrenceStep::operator<(const OccurrenceStep& other) const
{
    const std::string left = normalized(owner_partition);
    const std::string right = normalized(other.owner_partition);
    return left != right ? left < right : reference_instance < other.reference_instance;
}
bool Address::operator==(const Address& other) const
{ return normalized(partition) == normalized(other.partition) &&
         occurrence == other.occurrence && instance == other.instance && field == other.field; }
bool Address::operator<(const Address& other) const
{
    const std::string left = normalized(partition), right = normalized(other.partition);
    return std::tie(left, occurrence, instance, field) <
           std::tie(right, other.occurrence, other.instance, other.field);
}

uint32_t Runtime::hash(const char* name)
{
    uint32_t value = 5381u;
    if (!name) return value;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p)
        value = value * 33u ^ *p;
    return value;
}

Runtime::~Runtime() = default;

std::unique_ptr<Runtime> Runtime::clone() const
{
    if (!impl_) return nullptr;
    auto result = std::make_unique<Runtime>(context_);
    result->root_partition_ = root_partition_;
    result->compile_report_ = compile_report_;
    result->time_seconds_ = time_seconds_;
    result->provider_values_ = provider_values_;
    result->resolved_values_ = resolved_values_;
    result->commits_ = commits_;
    result->emitted_events_ = emitted_events_;
    result->layout_commits_ = layout_commits_;
    result->impl_ = std::make_unique<Impl>(*impl_);
    return result;
}

void Runtime::clear()
{
    impl_.reset(); root_partition_.clear(); compile_report_ = {};
    provider_values_.clear(); resolved_values_.clear(); commits_.clear();
    emitted_events_.clear(); layout_commits_.clear();
    time_seconds_ = 0.0;
}

bool Runtime::compile(const std::string& root_partition, int max_depth,
                      std::string& error)
{
    clear(); error.clear();
    if (!context_ || root_partition.empty()) {
        error = "Rime runtime requires a mounted context and root partition";
        return false;
    }
    root_partition_ = normalized(root_partition);
    max_depth = max_depth <= 0 ? 6 : max_depth;

    bf6_rime_tree_stats tree_stats{};
    const int row_count = bf6_rime_tree(context_, root_partition_.c_str(),
                                         max_depth, nullptr, 0, &tree_stats);
    if (row_count <= 0) {
        error = "root has no readable live Rime tree";
        clear(); return false;
    }
    std::vector<bf6_rime_node> rows(static_cast<size_t>(row_count));
    if (bf6_rime_tree(context_, root_partition_.c_str(), max_depth,
                      rows.data(), row_count, &tree_stats) != row_count) {
        error = "Rime tree changed during count/fill";
        clear(); return false;
    }
    compile_report_.tree_rows = row_count;

    impl_ = std::make_unique<Impl>();
    Impl& p = *impl_;

    /* DiceUIInputBehaviorElementData is the native producer for the three
     * shipped interaction-state outputs (IsFocused, IsHovered and
     * IsInteracting).  The expanded tree has already admitted this kind only
     * through the old-schema (type GUID, signature) gate in libbf6.  Keep a
     * second check here because this runtime assigns executable semantics,
     * and measure a one-bit signature perturbation beside it. */
    constexpr const char* kInputBehaviorGuid =
        "295b0545-d165-e962-3ebe-e871ce63aa88";
    constexpr uint32_t kInputBehaviorSignature = 0x7B914016u;
    std::map<ScopeKey, std::set<int32_t>> idle_input_behaviors;
    for (int index = 0; index < row_count; ++index) {
        const bf6_rime_node& row = rows[static_cast<size_t>(index)];
        if (std::strcmp(row.type_guid, kInputBehaviorGuid) != 0) continue;
        if (row.type_signature == (kInputBehaviorSignature ^ 1u)) {
            ++compile_report_.input_behavior_gate_control_matches;
            continue;
        }
        if (row.kind != BF6_RIME_INPUT_BEHAVIOR ||
            row.type_signature != kInputBehaviorSignature)
            continue;
        idle_input_behaviors[{normalized(row.partition),
                              row_occurrence(rows, index)}].insert(row.instance);
    }

    std::set<ScopeKey> visual;
    visual.insert({root_partition_, {}});
    for (int index = 0; index < row_count; ++index) {
        const bf6_rime_node& row = rows[static_cast<size_t>(index)];
        if (!row.partition[0]) continue;
        visual.insert({normalized(row.partition), row_occurrence(rows, index)});
    }

    std::function<bool(const ScopeKey&, int, const std::vector<std::string>&)> add_scope;
    add_scope = [&](const ScopeKey& key, int depth,
                    const std::vector<std::string>& ancestry) -> bool {
        if (p.scope_index.find(key) != p.scope_index.end()) return true;
        if (depth > max_depth) { ++compile_report_.depth_references_declined; return true; }
        if (std::find(ancestry.begin(), ancestry.end(), key.partition) != ancestry.end()) {
            ++compile_report_.cycle_references_declined; return true;
        }

        auto template_it = p.templates.find(key.partition);
        if (template_it == p.templates.end()) {
            PartitionTemplate value;
            auto bind = [&](auto api, auto& destination) {
                return read_rows<typename std::decay_t<decltype(destination)>::value_type>(
                    [&](const char* part, auto* out, int count) {
                        return api(context_, part, out, count);
                    }, key.partition, destination);
            };
            if (!bind(bf6_rime_connections, value.edges) ||
                !bind(bf6_rime_interface_descriptors, value.interfaces) ||
                !bind(bf6_rime_interface_fields, value.interface_fields) ||
                !bind(bf6_rime_conditional_floats, value.conditional_floats) ||
                !bind(bf6_rime_float_interpolators, value.float_interpolators) ||
                !bind(bf6_rime_conditional_properties, value.conditional_properties) ||
                !bind(bf6_rime_logic_operations, value.logic) ||
                !bind(bf6_rime_logic_references, value.references) ||
                !bind(bf6_rime_event_connections, value.events)) {
                ++compile_report_.unreadable_partitions;
                return false;
            }
            /* Animation APIs intentionally bypass the older-schema binding
             * cache.  Read them once into a generous bounded buffer rather
             * than performing a count parse followed by a fill parse.  Only
             * partitions with CompareBool event producers can activate the
             * timeline semantics implemented below, so unrelated UI graphs
             * do not pay timeline/layout parse costs. */
            auto bounded = [&](auto api, auto& destination) {
                using Row = typename std::decay_t<decltype(destination)>::value_type;
                destination.resize(4096);
                int count = api(context_, key.partition.c_str(),
                                static_cast<Row*>(destination.data()),
                                static_cast<int>(destination.size()));
                if (count < 0) { destination.clear(); return false; }
                if (count > static_cast<int>(destination.size())) {
                    destination.resize(static_cast<size_t>(count));
                    count = api(context_, key.partition.c_str(),
                                destination.data(), count);
                    if (count != static_cast<int>(destination.size())) {
                        destination.clear(); return false;
                    }
                }
                destination.resize(static_cast<size_t>(count));
                return true;
            };
            if (!bounded(bf6_rime_compare_bools, value.compare_bools)) {
                ++compile_report_.unreadable_partitions;
                return false;
            }
            std::vector<bf6_rime_layout_track> layouts;
            if (!value.compare_bools.empty()) {
                if (!bounded(bf6_rime_timelines, value.timelines) ||
                    !bounded(bf6_rime_layout_tracks, layouts)) {
                    ++compile_report_.unreadable_partitions;
                    return false;
                }
            }
            for (const bf6_rime_layout_track& track : layouts) {
                LayoutTrackTemplate layout;
                layout.track = track;
                const int key_count = track.key_count;
                if (key_count <= 0) continue;
                layout.keys.resize(static_cast<size_t>(key_count));
                if (bf6_rime_layout_track_keys(
                        context_, key.partition.c_str(), track.track_instance,
                        layout.keys.data(), key_count) != key_count)
                    continue;
                value.layout_tracks.push_back(std::move(layout));
            }
            template_it = p.templates.emplace(key.partition, std::move(value)).first;
        }
        const PartitionTemplate& graph = template_it->second;
        Scope scope;
        scope.key = key;
        scope.interfaces = graph.interfaces;
        scope.interface_fields = graph.interface_fields;
        scope.local_edges = graph.edges;

        const size_t scope_id = p.scopes.size();
        p.scope_index.emplace(key, scope_id);
        p.scopes.push_back(std::move(scope));
        Scope& added = p.scopes[scope_id];
        ++compile_report_.graph_occurrences;
        compile_report_.property_edges += static_cast<int>(added.local_edges.size());
        compile_report_.event_edges += static_cast<int>(graph.events.size());
        compile_report_.interface_fields += static_cast<int>(added.interface_fields.size());

        for (const auto& field : added.interface_fields) {
            Value value = public_value(field);
            if (value.kind != ValueKind::Null)
                p.defaults[p.address(added, field.interface_instance, field.field_id)] = std::move(value);
        }
        for (const auto& edge : added.local_edges)
            p.edges.push_back({p.address(added, edge.source, edge.source_field),
                               p.address(added, edge.target, edge.target_field), edge.mode});
        for (const auto& edge : graph.events)
            p.event_edges.push_back({
                {key.partition, key.occurrence, edge.source, edge.source_event},
                {key.partition, key.occurrence, edge.target, edge.target_event}, edge.mode});
        for (const auto& node : graph.compare_bools) {
            CompareBoolNode compare;
            compare.node = p.address(added, node.instance, 0);
            compare.input_field = node.input_field;
            compare.on_true_event = node.on_true_event;
            compare.on_false_event = node.on_false_event;
            compare.authored_value = node.authored_value != 0;
            compare.always_send = node.always_send != 0;
            compare.trigger_on_property_change =
                node.trigger_on_property_change != 0;
            compare.trigger_on_start = node.trigger_on_start != 0;
            p.compare_bools.push_back(std::move(compare));
            ++compile_report_.compare_bool_nodes;
            if (node.always_send)
                ++compile_report_.declined_compare_bool_nodes;
        }
        for (const auto& timeline : graph.timelines) {
            TimelineNode node;
            node.node = p.address(added, timeline.timeline_instance, 0);
            node.timeline = timeline;
            p.timelines.push_back(std::move(node));
            ++compile_report_.timeline_nodes;
        }
        for (const LayoutTrackTemplate& layout : graph.layout_tracks) {
            ++compile_report_.layout_track_nodes;
            const bool targeted = layout.track.timeline_instance >= 0 &&
                layout.track.target_instance >= 0 && !layout.keys.empty() &&
                std::all_of(layout.keys.begin(), layout.keys.end(),
                    [](const bf6_rime_layout_key& key) {
                        return key.interpolation_type >= 0 &&
                            key.interpolation_type <= 2 &&
                            key.interpolation_mode >= 0 &&
                            key.interpolation_mode <= 3;
                    });
            if (!targeted) {
                ++compile_report_.declined_layout_track_nodes;
                continue;
            }
            LayoutTrackNode node;
            node.timeline = p.address(
                added, layout.track.timeline_instance, 0);
            node.target = p.address(
                added, layout.track.target_instance,
                layout.track.target_field);
            node.keys = layout.keys;
            p.layout_tracks.push_back(std::move(node));
            ++compile_report_.targeted_layout_track_nodes;
        }
        for (const auto& node : graph.conditional_floats) {
            Operator op; op.kind = Operator::Kind::ConditionalFloat;
            op.node = p.address(added, node.instance, 0);
            op.true_value = node.value_if_true; op.false_value = node.value_if_false;
            op.authored_condition = node.authored_condition != 0;
            p.operators.push_back(std::move(op));
        }
        for (const auto& node : graph.float_interpolators) {
            Operator op; op.kind = Operator::Kind::FloatInterpolator;
            op.node = p.address(added, node.instance, 0);
            op.input_field = node.input_field;
            op.out_field = node.output_field;
            op.default_value = node.default_value;
            op.duration = node.duration;
            op.velocity = node.velocity;
            op.use_velocity = node.use_velocity;
            op.interpolation_type = node.interpolation_type;
            op.interpolation_mode = node.interpolation_mode;
            op.dynamic_duration = node.dynamic_duration;
            op.use_real_time_clock = node.use_real_time_clock;
            op.force_frame_correct_output = node.force_frame_correct_output;
            p.operators.push_back(std::move(op));
            ++compile_report_.float_interpolator_nodes;
            /* Current-executable enums and the mounted Rime corpus establish
             * Linear/Quad/Cubic (0/1/2) and all four ease modes.  The 144
             * front-end nodes author 25 Quad/Cubic curves. Clock-policy
             * variants remain explicit declines: a familiar easing formula
             * is not evidence for velocity/dynamic/real-time integration. */
            const bool supported = node.interpolation_type >= 0 &&
                node.interpolation_type <= 2 && node.interpolation_mode >= 0 &&
                node.interpolation_mode <= 3 && !node.use_velocity &&
                !node.dynamic_duration && !node.use_real_time_clock &&
                !node.force_frame_correct_output &&
                std::isfinite(node.duration) && node.duration >= 0.f;
            if (supported && node.interpolation_type == 0)
                ++compile_report_.linear_interpolator_nodes;
            else if (supported)
                ++compile_report_.eased_interpolator_nodes;
            else
                ++compile_report_.declined_interpolator_nodes;
        }
        for (const auto& node : graph.conditional_properties) {
            Operator op; op.kind = Operator::Kind::ConditionalProperty;
            op.node = p.address(added, node.instance, 0);
            op.true_field = node.value_if_true_property_hash;
            op.false_field = node.value_if_false_property_hash;
            op.out_field = node.out_hash;
            op.authored_condition = node.authored_condition != 0;
            p.operators.push_back(std::move(op));
        }
        for (const auto& node : graph.logic) {
            Operator op;
            if (node.operation == BF6_RIME_LOGIC_AND) op.kind = Operator::Kind::And;
            else if (node.operation == BF6_RIME_LOGIC_OR) op.kind = Operator::Kind::Or;
            else if (node.operation == BF6_RIME_LOGIC_NOT) op.kind = Operator::Kind::Not;
            else op.kind = Operator::Kind::PropertyDefault;
            op.node = p.address(added, node.instance, 0);
            p.operators.push_back(std::move(op));
        }
        const auto behavior_scope = idle_input_behaviors.find(key);
        if (behavior_scope != idle_input_behaviors.end()) {
            for (int32_t instance : behavior_scope->second) {
                Operator op;
                op.kind = Operator::Kind::InputBehaviorIdle;
                op.node = p.address(added, instance, 0);
                p.operators.push_back(std::move(op));
                ++compile_report_.idle_input_behavior_nodes;
                std::set<uint32_t> outputs;
                for (const auto& edge : graph.edges)
                    if (edge.source == instance)
                        outputs.insert(edge.source_field);
                compile_report_.idle_input_behavior_outputs +=
                    static_cast<int>(outputs.size());
            }
        }
        compile_report_.operator_nodes +=
            static_cast<int>(graph.conditional_floats.size() +
                             graph.float_interpolators.size() +
                             graph.conditional_properties.size() +
                             graph.logic.size()) +
            (behavior_scope == idle_input_behaviors.end() ? 0 :
             static_cast<int>(behavior_scope->second.size()));

        std::vector<std::string> child_ancestry = ancestry;
        child_ancestry.push_back(key.partition);
        for (const auto& reference : graph.references) {
            ScopeKey child{normalized(reference.blueprint), key.occurrence};
            child.occurrence.push_back({key.partition, reference.instance});
            if (!add_scope(child, depth + 1, child_ancestry)) continue;
            /* Depth- and cycle-limited references are successful declines:
             * add_scope records them in the compile report but deliberately
             * does not create a scope.  Do not use map::at here; a large
             * route such as Home reaches this boundary at the viewer's
             * production depth of six. */
            const auto child_scope_it = p.scope_index.find(child);
            if (child_scope_it == p.scope_index.end()) continue;
            ++compile_report_.logic_prefab_occurrences;
            const Scope& parent_scope = p.scopes[scope_id];
            const Scope& child_scope = p.scopes[child_scope_it->second];
            for (int32_t interface_instance : child_scope.interfaces) {
                std::set<uint32_t> source_fields, target_fields;
                for (const auto& edge : child_scope.local_edges) {
                    if (edge.source == interface_instance) source_fields.insert(edge.source_field);
                    if (edge.target == interface_instance) target_fields.insert(edge.target_field);
                }
                std::set<uint32_t> ambiguous;
                std::set_intersection(source_fields.begin(), source_fields.end(),
                                      target_fields.begin(), target_fields.end(),
                                      std::inserter(ambiguous, ambiguous.end()));
                compile_report_.ambiguous_interface_pins_declined += static_cast<int>(ambiguous.size());
                for (uint32_t field : source_fields)
                    if (!ambiguous.count(field)) {
                        p.edges.push_back({p.address(parent_scope, reference.instance, field),
                                           p.address(child_scope, interface_instance, field), 0});
                        ++compile_report_.parent_to_child_pins;
                    }
                for (uint32_t field : target_fields)
                    if (!ambiguous.count(field)) {
                        p.edges.push_back({p.address(child_scope, interface_instance, field),
                                           p.address(parent_scope, reference.instance, field), 0});
                        ++compile_report_.child_to_parent_pins;
                    }
            }
        }
        return true;
    };

    for (const ScopeKey& key : visual)
        if (!add_scope(key, static_cast<int>(key.occurrence.size()), {})) {
            error = "one or more live Rime graph partitions could not be read";
            /* Preserve the successfully compiled scopes and report the gap. */
        }
    tick(0.0);
    return !p.scopes.empty();
}

bool Runtime::set(const Address& input, Value value, std::string& error)
{
    error.clear();
    const Impl* p = impl_.get();
    if (!p) { error = "Rime runtime is not compiled"; return false; }
    Address address = input; address.partition = normalized(address.partition);
    if (p->scope_index.find({address.partition, address.occurrence}) == p->scope_index.end()) {
        error = "provider address does not name a compiled occurrence";
        return false;
    }
    if (address.instance < 0 || value.kind == ValueKind::Null) {
        error = "provider address/value is invalid"; return false;
    }
    provider_values_[std::move(address)] = std::move(value);
    return true;
}

int Runtime::set_public(const std::string& partition,
                        const OccurrencePath& occurrence, uint32_t field,
                        Value value, std::string& error)
{
    error.clear(); const Impl* p = impl_.get();
    if (!p) { error = "Rime runtime is not compiled"; return -1; }
    const auto found = p->scope_index.find({normalized(partition), occurrence});
    if (found == p->scope_index.end()) { error = "public input occurrence is not compiled"; return -1; }
    const Scope& scope = p->scopes[found->second];
    std::set<int32_t> targets;
    for (const auto& row : scope.interface_fields) {
        if (row.field_id != field) continue;
        bool sources_graph = false, receives_graph = false;
        for (const auto& edge : scope.local_edges) {
            sources_graph |= edge.source == row.interface_instance &&
                             edge.source_field == field;
            receives_graph |= edge.target == row.interface_instance &&
                              edge.target_field == field;
        }
        /* An InterfaceDescriptor source is an input into its child graph.
         * Target-only fields are graph outputs; accepting host writes there
         * would reverse the proven connection polarity. Bidirectional pins
         * stay unresolved for the same reason the prefab linker declines
         * them. */
        if (sources_graph && !receives_graph)
            targets.insert(row.interface_instance);
    }
    if (targets.empty()) { error = "public field is absent from the live interface"; return 0; }
    for (int32_t instance : targets)
        provider_values_[p->address(scope, instance, field)] = value;
    return static_cast<int>(targets.size());
}

bool Runtime::erase(const Address& input)
{
    Address address = input; address.partition = normalized(address.partition);
    return provider_values_.erase(address) != 0;
}

TickReport Runtime::tick(double delta_seconds)
{
    TickReport report; Impl* p = impl_.get();
    if (!p) return report;
    const double tick_delta = std::isfinite(delta_seconds) && delta_seconds > 0.0
        ? delta_seconds : 0.0;
    time_seconds_ += tick_delta;
    const uint32_t condition = hash("Condition"), output = hash("Output");
    const uint32_t in_value = 0x62da2189u, default_value = 0x7b256865u;
    emitted_events_.clear();
    layout_commits_.clear();
    auto slot = [](const Address& base, uint32_t field) {
        Address result = base; result.field = field; return result;
    };

    auto ease_weight = [](int type, int mode, double t) {
        t = std::max(0.0, std::min(1.0, t));
        if (type == 0) return t;
        auto power = [type](double value) {
            return type == 1 ? value * value : value * value * value;
        };
        if (mode == 0) return power(t);                         // EaseIn
        if (mode == 1) return 1.0 - power(1.0 - t);            // EaseOut
        if (mode == 2) return t < 0.5
            ? 0.5 * power(t * 2.0)
            : 1.0 - 0.5 * power((1.0 - t) * 2.0);             // EaseInOut
        return t < 0.5
            ? 0.5 * (1.0 - power(1.0 - t * 2.0))
            : 0.5 + 0.5 * power((t - 0.5) * 2.0);             // EaseOutIn
    };

    /* Stateful nodes sample the preceding converged property transaction
     * exactly once per frame.  Advancing them inside the combinational
     * fixpoint lets an upstream fallback and its later resolved value reset
     * the same interpolator repeatedly in one tick.  The one-transaction
     * boundary also matches the public set -> tick contract: a provider
     * transaction resolves first, then the next clock step consumes it. */
    for (const Operator& op : p->operators) {
        if (op.kind != Operator::Kind::FloatInterpolator) continue;
        InterpolatorState& state = p->interpolators[op.node];
        if (!state.initialized) {
            state.initialized = true;
            state.start = state.target = state.output = op.default_value;
            state.elapsed = 0.0;
            ++report.initialized_float_interpolators;
        }
        const bool supported = op.interpolation_type >= 0 &&
            op.interpolation_type <= 2 && op.interpolation_mode >= 0 &&
            op.interpolation_mode <= 3 && !op.use_velocity &&
            !op.dynamic_duration &&
            !op.use_real_time_clock && !op.force_frame_correct_output &&
            std::isfinite(op.duration) && op.duration >= 0.f;
        const auto source = resolved_values_.find(slot(op.node, op.input_field));
        double target = 0.0;
        if (!supported || source == resolved_values_.end() ||
            !as_real(source->second, target))
            continue;
        if (!state.has_target || state.target != target) {
            state.start = state.output;
            state.target = target;
            state.elapsed = 0.0;
            state.has_target = true;
        }
        const bool was_complete = state.output == state.target;
        state.elapsed += tick_delta;
        const double duration = static_cast<double>(op.duration);
        const double linear_weight = duration <= 0.0 ? 1.0 :
            std::min(1.0, state.elapsed / duration);
        const double weight = ease_weight(
            op.interpolation_type, op.interpolation_mode, linear_weight);
        state.output = state.start + (state.target - state.start) * weight;
        if (!was_complete) {
            ++report.advanced_float_interpolators;
            if (state.output == state.target)
                ++report.completed_float_interpolators;
        }
    }

    struct Cell { Value value; int priority = -1; bool ambiguous = false; };
    std::map<Address, Cell> cells;
    auto write = [&](const Address& key, const Value& value, int priority) {
        Cell& cell = cells[key];
        if (priority > cell.priority) {
            const bool changed = cell.priority != priority || cell.ambiguous || cell.value != value;
            cell.value = value; cell.priority = priority; cell.ambiguous = false; return changed;
        }
        if (priority == cell.priority && cell.value != value && !cell.ambiguous) {
            cell.ambiguous = true; return true;
        }
        return false;
    };
    for (const auto& row : p->defaults) write(row.first, row.second, 0);
    for (const auto& row : provider_values_) write(row.first, row.second, 3);

    std::map<Address, std::set<uint32_t>> incoming, outgoing;
    for (const Edge& edge : p->edges) {
        incoming[{edge.target.partition, edge.target.occurrence, edge.target.instance, 0}].insert(edge.target.field);
        outgoing[{edge.source.partition, edge.source.occurrence, edge.source.instance, 0}].insert(edge.source.field);
    }
    const int limit = static_cast<int>(p->edges.size() + p->operators.size()) + 2;
    std::set<size_t> executed_nodes;
    for (int pass = 0; pass < limit; ++pass) {
        bool changed = false;
        for (const Edge& edge : p->edges) {
            const auto source = cells.find(edge.source);
            if (source == cells.end() || source->second.ambiguous) continue;
            if (write(edge.target, source->second.value, source->second.priority)) {
                changed = true; ++report.propagated_edges;
            }
        }
        for (size_t operator_index = 0; operator_index < p->operators.size(); ++operator_index) {
            const Operator& op = p->operators[operator_index];
            bool executed = false;
            if (op.kind == Operator::Kind::ConditionalFloat ||
                op.kind == Operator::Kind::ConditionalProperty) {
                bool selected = op.authored_condition;
                const auto dynamic = cells.find(slot(op.node, condition));
                if (dynamic != cells.end()) {
                    if (dynamic->second.ambiguous || !as_bool(dynamic->second.value, selected)) continue;
                }
                if (op.kind == Operator::Kind::ConditionalFloat) {
                    changed |= write(slot(op.node, output),
                                     Value::from_real(selected ? op.true_value : op.false_value), 2);
                    executed = true;
                } else {
                    const auto source = cells.find(slot(op.node, selected ? op.true_field : op.false_field));
                    if (source != cells.end() && !source->second.ambiguous) {
                        changed |= write(slot(op.node, op.out_field), source->second.value, 2);
                        executed = true;
                    }
                }
            } else if (op.kind == Operator::Kind::FloatInterpolator) {
                InterpolatorState& state = p->interpolators[op.node];
                changed |= write(slot(op.node, op.out_field),
                                 Value::from_real(state.output), 2);
                executed = true;
            } else if (op.kind == Operator::Kind::InputBehaviorIdle) {
                /* This transaction is the pre-input state: the native
                 * behavior has received no focus, pointer hover, or press.
                 * Its exact output pin identities come from the installed
                 * graph, so no screen-specific hash table participates. */
                const auto fields = outgoing.find(op.node);
                if (fields != outgoing.end()) {
                    for (uint32_t field : fields->second)
                        changed |= write(slot(op.node, field),
                                         Value::from_bool(false), 2);
                    executed = !fields->second.empty();
                }
            } else if (op.kind == Operator::Kind::PropertyDefault) {
                const auto primary = cells.find(slot(op.node, in_value));
                const auto fallback = cells.find(slot(op.node, default_value));
                const Cell* selected = primary != cells.end() && !primary->second.ambiguous
                    ? &primary->second
                    : fallback != cells.end() && !fallback->second.ambiguous ? &fallback->second : nullptr;
                if (selected) {
                    for (uint32_t field : outgoing[op.node])
                        changed |= write(slot(op.node, field), selected->value, 2);
                    executed = true;
                }
            } else {
                std::vector<bool> values; bool known = true;
                for (uint32_t field : incoming[op.node]) {
                    const auto found = cells.find(slot(op.node, field)); bool value = false;
                    if (found == cells.end() || found->second.ambiguous ||
                        !as_bool(found->second.value, value)) { known = false; break; }
                    values.push_back(value);
                }
                if (known && !values.empty()) {
                    bool result = values.front();
                    if (op.kind == Operator::Kind::And)
                        result = std::all_of(values.begin(), values.end(), [](bool v) { return v; });
                    else if (op.kind == Operator::Kind::Or)
                        result = std::any_of(values.begin(), values.end(), [](bool v) { return v; });
                    else if (op.kind == Operator::Kind::Not && values.size() == 1)
                        result = !values.front();
                    else if (op.kind == Operator::Kind::Not) known = false;
                    if (known) {
                        for (uint32_t field : outgoing[op.node])
                            changed |= write(slot(op.node, field), Value::from_bool(result), 2);
                        executed = true;
                    }
                }
            }
            if (executed) executed_nodes.insert(operator_index);
        }
        report.iterations = pass + 1;
        if (!changed) { report.converged = true; break; }
    }
    report.executed_operators = static_cast<int>(executed_nodes.size());
    std::map<Address, Value> next;
    report.ambiguous_slots = 0;
    for (const auto& row : cells) {
        if (row.second.ambiguous) { ++report.ambiguous_slots; continue; }
        next.emplace(row.first, row.second.value);
    }
    std::set<Address> operator_instances;
    for (const Operator& op : p->operators) operator_instances.insert(op.node);
    for (const Address& node : operator_instances) {
        bool has_output = false;
        for (uint32_t field : outgoing[node])
            has_output |= next.find(slot(node, field)) != next.end();
        report.unresolved_operator_nodes += has_output ? 0 : 1;
    }
    commits_.clear();
    for (const auto& row : next) {
        const auto old = resolved_values_.find(row.first);
        if (old == resolved_values_.end() || old->second != row.second)
            commits_.push_back({row.first, row.second});
    }
    resolved_values_ = std::move(next);

    /* CompareBool is the proven bridge from the converged property graph to
     * authored event routing.  AlwaysSend depends on native evaluation/input
     * transaction timing that this runtime does not yet expose, so those
     * records are counted and declined instead of being fired every frame. */
    for (const CompareBoolNode& node : p->compare_bools) {
        if (node.always_send) continue;
        bool value = node.authored_value;
        const auto dynamic = resolved_values_.find(slot(node.node, node.input_field));
        if (dynamic != resolved_values_.end() &&
            !as_bool(dynamic->second, value))
            continue;
        CompareBoolState& state = p->compare_bool_states[node.node];
        const bool fire = !state.initialized
            ? node.trigger_on_start
            : node.trigger_on_property_change && state.value != value;
        state.initialized = true;
        state.value = value;
        if (!fire) continue;
        ++report.emitted_compare_bool_events;
        EventAddress source{node.node.partition, node.node.occurrence,
                            node.node.instance,
                            value ? node.on_true_event : node.on_false_event};
        for (const EventEdge& edge : p->event_edges)
            if (edge.source.partition == source.partition &&
                edge.source.occurrence == source.occurrence &&
                edge.source.instance == source.instance &&
                edge.source.event == source.event)
            {
                emitted_events_.push_back({edge.source, edge.target, edge.mode});
                ++report.delivered_compare_bool_events;
            }
    }

    std::set<Address> started;
    for (const EventDelivery& delivery : emitted_events_)
        for (const TimelineNode& node : p->timelines)
            if (node.node.partition == delivery.target.partition &&
                node.node.occurrence == delivery.target.occurrence &&
                node.node.instance == delivery.target.instance)
            {
                TimelineState& state = p->timeline_states[node.node];
                state.active = true;
                state.time = node.timeline.reset_time_on_started
                    ? node.timeline.start_time : node.timeline.init_time;
                started.insert(node.node);
                ++report.started_timelines;
            }

    auto sample_layout = [&](const std::vector<bf6_rime_layout_key>& keys,
                             double time, bf6_rime_axis& out) {
        if (keys.empty()) return false;
        if (time <= keys.front().time) { out = keys.front().layout; return true; }
        if (time >= keys.back().time) { out = keys.back().layout; return true; }
        size_t upper = 1;
        while (upper < keys.size() && time > keys[upper].time) ++upper;
        if (upper >= keys.size()) { out = keys.back().layout; return true; }
        const bf6_rime_layout_key& left = keys[upper - 1];
        const bf6_rime_layout_key& right = keys[upper];
        const double span = static_cast<double>(right.time - left.time);
        if (!(span > 0.0)) { out = right.layout; return true; }
        const double linear = (time - left.time) / span;
        const double weight = ease_weight(
            left.interpolation_type, left.interpolation_mode, linear);
        auto mix = [weight](float a, float b) {
            return static_cast<float>(a + (b - a) * weight);
        };
        out.anchor_start = mix(left.layout.anchor_start, right.layout.anchor_start);
        out.anchor_end = mix(left.layout.anchor_end, right.layout.anchor_end);
        out.offset_start = mix(left.layout.offset_start, right.layout.offset_start);
        out.offset_end = mix(left.layout.offset_end, right.layout.offset_end);
        out.pivot = mix(left.layout.pivot, right.layout.pivot);
        out.weight = mix(left.layout.weight, right.layout.weight);
        return true;
    };

    for (const TimelineNode& node : p->timelines) {
        TimelineState& state = p->timeline_states[node.node];
        if (!state.active) continue;
        if (!started.count(node.node)) state.time += tick_delta;
        ++report.active_timelines;
        for (const LayoutTrackNode& track : p->layout_tracks) {
            if (!(track.timeline == node.node)) continue;
            bf6_rime_axis layout{};
            if (sample_layout(track.keys, state.time, layout)) {
                layout_commits_.push_back(
                    {track.target, layout, node.node.instance});
                ++report.sampled_layout_tracks;
            }
        }
        if (node.timeline.end_rule == 0 &&
            state.time >= node.timeline.end_time) {
            state.active = false;
            ++report.completed_timelines;
        }
    }
    return report;
}

std::optional<Value> Runtime::get(const Address& input) const
{
    Address address = input; address.partition = normalized(address.partition);
    const auto found = resolved_values_.find(address);
    return found == resolved_values_.end() ? std::nullopt : std::optional<Value>(found->second);
}

std::vector<Address> Runtime::public_slots(uint32_t field) const
{
    std::vector<Address> result;
    const Impl* p = impl_.get();
    if (!p) return result;
    for (const Scope& scope : p->scopes)
        for (const bf6_rime_interface_field& row : scope.interface_fields) {
            bool sources_graph = false, receives_graph = false;
            for (const auto& edge : scope.local_edges) {
                sources_graph |= edge.source == row.interface_instance &&
                                 edge.source_field == row.field_id;
                receives_graph |= edge.target == row.interface_instance &&
                                  edge.target_field == row.field_id;
            }
            if ((!field || row.field_id == field) &&
                sources_graph && !receives_graph)
                result.push_back(p->address(
                    scope, row.interface_instance, row.field_id));
        }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<EventDelivery> Runtime::emit(const EventAddress& input) const
{
    std::vector<EventDelivery> result; const Impl* p = impl_.get();
    if (!p) return result;
    EventAddress source = input; source.partition = normalized(source.partition);
    for (const EventEdge& edge : p->event_edges)
        if (edge.source.partition == source.partition &&
            edge.source.occurrence == source.occurrence &&
            edge.source.instance == source.instance && edge.source.event == source.event)
            result.push_back({edge.source, edge.target, edge.mode});
    return result;
}

} // namespace bf6_rime_runtime
