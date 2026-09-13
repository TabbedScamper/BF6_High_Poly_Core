#include "install_screen_source.h"

#include "rime_state_bridge.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <type_traits>
#include <utility>

namespace bf6_ui {
namespace {

std::vector<OccurrenceStep> convert_occurrence(
    const rime_state::OccurrencePath& input)
{
    std::vector<OccurrenceStep> result;
    result.reserve(input.size());
    for (const rime_state::OccurrenceStep& step : input)
        result.push_back({step.owner_partition, step.reference_instance});
    return result;
}

std::vector<OccurrenceStep> convert_layout_occurrence(
    const bf6_rime_runtime::OccurrencePath& input)
{
    std::vector<OccurrenceStep> result;
    result.reserve(input.size());
    for (const bf6_rime_runtime::OccurrenceStep& step : input)
        result.push_back({step.owner_partition, step.reference_instance});
    return result;
}

rime_state::OccurrencePath convert_runtime_occurrence(
    const bf6_rime_runtime::OccurrencePath& input)
{
    rime_state::OccurrencePath result;
    result.reserve(input.size());
    for (const bf6_rime_runtime::OccurrenceStep& step : input)
        result.push_back({step.owner_partition, step.reference_instance});
    return result;
}

rime_state::Value convert_runtime_value(
    const bf6_rime_runtime::Value& value)
{
    using Kind = bf6_rime_runtime::ValueKind;
    switch (value.kind) {
    case Kind::Bool: return rime_state::Value::from_bool(value.boolean);
    case Kind::Int: return rime_state::Value::from_int(value.integer);
    case Kind::UInt: return rime_state::Value::from_uint(value.unsigned_integer);
    case Kind::Real: return rime_state::Value::from_real(value.real);
    case Kind::String: return rime_state::Value::from_string(value.string);
    default: return {};
    }
}

rime_state::ApplyReport apply_runtime_commits(
    rime::Screen& screen,
    const std::vector<bf6_rime_runtime::Commit>& commits)
{
    std::vector<rime_state::StateSlot> slots;
    slots.reserve(commits.size());
    for (const bf6_rime_runtime::Commit& commit : commits) {
        if (commit.value.kind == bf6_rime_runtime::ValueKind::Null) continue;
        rime_state::StateSlot slot;
        slot.partition = commit.address.partition;
        slot.occurrence = convert_runtime_occurrence(commit.address.occurrence);
        slot.local_instance = commit.address.instance;
        slot.field = commit.address.field;
        slot.value = convert_runtime_value(commit.value);
        slots.push_back(std::move(slot));
    }
    return rime_state::apply_state(screen, slots);
}

int apply_runtime_layout_commits(
    rime::Screen& screen,
    const std::vector<bf6_rime_runtime::LayoutCommit>& commits)
{
    int applied = 0;
    for (const bf6_rime_runtime::LayoutCommit& commit : commits) {
        std::vector<size_t> matches;
        for (size_t index = 0; index < screen.elements.size(); ++index) {
            rime_state::OccurrencePath path;
            if (!rime_state::occurrence_path(screen, index, path)) continue;
            const rime::Element& element = screen.elements[index];
            if (element.partition == commit.address.partition &&
                element.instance == commit.address.instance &&
                convert_occurrence(path) ==
                    convert_layout_occurrence(commit.address.occurrence))
                matches.push_back(index);
        }
        if (matches.size() != 1) continue;
        rime::Axis axis;
        axis.anchor_start = commit.layout.anchor_start;
        axis.anchor_end = commit.layout.anchor_end;
        axis.offset_start = commit.layout.offset_start;
        axis.offset_end = commit.layout.offset_end;
        axis.pivot = commit.layout.pivot;
        axis.weight = commit.layout.weight;
        axis.present = true;
        if (commit.address.field == 0x36BBCDA1u)
            screen.elements[matches[0]].h = axis;
        else if (commit.address.field == 0x8A1C220Du)
            screen.elements[matches[0]].v = axis;
        else
            continue;
        ++applied;
    }
    return applied;
}

bool screen_path(const char* value)
{
    return value && std::string_view(value).find("/screens/") !=
        std::string_view::npos;
}

struct GraphOccurrence {
    std::string partition;
    std::vector<OccurrenceStep> occurrence;

    bool operator<(const GraphOccurrence& other) const {
        if (partition != other.partition) return partition < other.partition;
        return occurrence < other.occurrence;
    }
};

} // namespace

bool DirectInstallScreenDocument::contains(
    const ElementAddress& address) const
{
    return std::find(elements_.begin(), elements_.end(), address) !=
        elements_.end();
}

bool DirectInstallScreenDocument::tick_and_apply(
    double delta_seconds, DirectScreenTickReport& report, std::string& error)
{
    report = {};
    error.clear();
    if (!runtime_)
    {
        error = "direct-install document has no compiled Rime runtime";
        return false;
    }
    if (!std::isfinite(delta_seconds) || delta_seconds < 0.0)
    {
        error = "Rime tick delta must be finite and non-negative";
        return false;
    }
    report.runtime = runtime_->tick(delta_seconds);
    report.applied = apply_runtime_commits(screen_, runtime_->commits());
    report.applied_layouts = apply_runtime_layout_commits(
        screen_, runtime_->layout_commits());
    rime::solve(screen_, 1920.f, 1080.f);
    return report.runtime.converged;
}

DirectInstallScreenSource::~DirectInstallScreenSource()
{
    if (context_ && owns_context_) bf6_close(context_);
}

std::unique_ptr<DirectInstallScreenSource> DirectInstallScreenSource::open(
    const std::string& game_directory, MountMode mode, std::string& error)
{
    error.clear();
    if (game_directory.empty())
    {
        error = "a current Steam install directory is required";
        return nullptr;
    }
    if (bf6_abi_version() != BF6_ABI_VERSION)
    {
        error = "libbf6 ABI does not match the compiled header";
        return nullptr;
    }
    char native_error[1024]{};
    bf6_ctx* context = bf6_open(
        game_directory.c_str(), native_error,
        static_cast<int>(sizeof(native_error)));
    if (!context)
    {
        error = native_error[0] ? native_error : "bf6_open failed";
        return nullptr;
    }
    if (bf6_was_lifted(context))
    {
        error = "encrypted/lifted install rejected; use the Steam install";
        bf6_close(context);
        return nullptr;
    }
    const int mounted = mode == MountMode::WholeInstall
        ? bf6_mount_all(context, 1, native_error,
                        static_cast<int>(sizeof(native_error)))
        : bf6_mount_frontend(context, native_error,
                             static_cast<int>(sizeof(native_error)));
    if (!mounted)
    {
        error = native_error[0] ? native_error : "UI mount failed";
        bf6_close(context);
        return nullptr;
    }
    return std::unique_ptr<DirectInstallScreenSource>(
        new DirectInstallScreenSource(context, mode, true));
}

std::unique_ptr<DirectInstallScreenSource>
DirectInstallScreenSource::attach_mounted(
    bf6_ctx* context, MountMode mode, std::string& error)
{
    error.clear();
    if (!context)
    {
        error = "a mounted libbf6 context is required";
        return nullptr;
    }
    if (bf6_abi_version() != BF6_ABI_VERSION)
    {
        error = "libbf6 ABI does not match the compiled header";
        return nullptr;
    }
    if (bf6_was_lifted(context))
    {
        error = "encrypted/lifted install rejected; use the Steam install";
        return nullptr;
    }
    return std::unique_ptr<DirectInstallScreenSource>(
        new DirectInstallScreenSource(context, mode, false));
}

bool DirectInstallScreenSource::enumerate(std::vector<ScreenIdentity>& out,
                                          std::string& error)
{
    out.clear();
    error.clear();
    if (!context_)
    {
        error = "install context is closed";
        return false;
    }
    const int count = bf6_list_ebx(context_, "/screens/", nullptr, 0);
    if (count <= 0)
    {
        error = "the current mount contains no /screens/ EBX paths";
        return false;
    }
    std::vector<bf6_asset> rows(static_cast<size_t>(count));
    const int filled = bf6_list_ebx(
        context_, "/screens/", rows.data(), static_cast<int>(rows.size()));
    if (filled != count)
    {
        error = "screen catalogue changed during count/fill read";
        return false;
    }
    out.reserve(rows.size());
    for (const bf6_asset& row : rows)
        if (screen_path(row.name))
            out.push_back({row.name, row.size});
    if (out.empty())
    {
        error = "screen search returned no exact /screens/ paths";
        return false;
    }
    return true;
}

std::shared_ptr<AuthoredScreenDocument> DirectInstallScreenSource::load(
    const ScreenIdentity& identity, std::string& error)
{
    error.clear();
    if (!context_)
    {
        error = "install context is closed";
        return nullptr;
    }
    if (!screen_path(identity.path.c_str()))
    {
        error = "requested asset is not a /screens/ path";
        return nullptr;
    }

    bf6_rime_tree_stats stats{};
    const int count = bf6_rime_tree(
        context_, identity.path.c_str(), 6, nullptr, 0, &stats);
    if (count <= 0)
    {
        error = count < 0 ? "screen has no readable Rime tree"
                          : "screen Rime tree is empty";
        return nullptr;
    }
    std::vector<bf6_rime_node> rows(static_cast<size_t>(count));
    const int filled = bf6_rime_tree(
        context_, identity.path.c_str(), 6, rows.data(), count, &stats);
    if (filled != count)
    {
        error = "Rime tree changed during count/fill read";
        return nullptr;
    }

    auto document = std::make_shared<DirectInstallScreenDocument>();
    document->identity_ = identity;
    document->report_.tree = stats;
    document->report_.rows = filled;
    if (!rime::from_live(rows.data(), filled, document->screen_, error))
        return nullptr;

    rime::load_shapes(context_, document->screen_);
    rime::load_images(context_, document->screen_);
    document->report_.authored_text_targets = rime::load_text_bindings(
        context_, document->screen_, &document->report_.ambiguous_text_targets);
    document->report_.interface_graphs =
        rime::load_interface_text_graphs(context_, document->screen_);
    document->report_.conditional_float_bindings =
        rime::load_conditional_float_bindings(context_, document->screen_);
    document->report_.conditional_float_applications =
        rime::apply_conditional_float_bindings(document->screen_);
    document->report_.unresolved_interface_colors =
        rime::mark_unresolved_interface_colors(context_, document->screen_);
    document->report_.unresolved_interface_states =
        rime::mark_unresolved_interface_states(context_, document->screen_);
    document->report_.interface_default_applications =
        rime::apply_interface_defaults(document->screen_);
    document->runtime_ =
        std::make_unique<bf6_rime_runtime::Runtime>(context_);
    std::string runtime_error;
    if (!document->runtime_->compile(
            identity.path, 6, runtime_error)) {
        error = "offline Rime runtime: " + runtime_error;
        return nullptr;
    }
    document->report_.runtime_compile =
        document->runtime_->compile_report();
    /* compile() performs the initial zero-time transaction. Its commits are
     * applied through the same occurrence-scoped renderer boundary used for
     * later provider updates. Internal logic slots are expected to be
     * reported as non-renderer/missing; they are never painted as elements. */
    document->report_.runtime_initial_apply = apply_runtime_commits(
        document->screen_, document->runtime_->commits());
    document->report_.runtime_initial_layout_apply =
        apply_runtime_layout_commits(
            document->screen_, document->runtime_->layout_commits());
    document->report_.runtime_initial_tick = document->runtime_->tick(0.0);
    rime::solve(document->screen_, 1920.f, 1080.f);

    std::set<GraphOccurrence> graph_occurrences;
    document->elements_.reserve(document->screen_.elements.size());
    for (size_t index = 0; index < document->screen_.elements.size(); ++index)
    {
        const rime::Element& element = document->screen_.elements[index];
        rime_state::OccurrencePath native_occurrence;
        if (!rime_state::occurrence_path(
                document->screen_, index, native_occurrence))
        {
            error = "invalid occurrence chain in loaded Rime document";
            return nullptr;
        }
        std::vector<OccurrenceStep> occurrence =
            convert_occurrence(native_occurrence);
        document->elements_.push_back(
            {element.partition, occurrence, element.instance});
        if (!element.partition.empty())
            graph_occurrences.insert({element.partition, std::move(occurrence)});
    }

    for (const GraphOccurrence& graph : graph_occurrences)
    {
        const int edge_count = bf6_rime_event_connections(
            context_, graph.partition.c_str(), nullptr, 0);
        if (edge_count < 0)
        {
            ++document->report_.unreadable_event_graphs;
            continue;
        }
        std::vector<bf6_rime_event_connection> edges(
            static_cast<size_t>(edge_count));
        const int edge_filled = edge_count
            ? bf6_rime_event_connections(
                  context_, graph.partition.c_str(), edges.data(), edge_count)
            : 0;
        if (edge_filled != edge_count)
        {
            error = "event graph changed during count/fill read";
            return nullptr;
        }
        for (const bf6_rime_event_connection& edge : edges)
            document->event_edges_.push_back({
                graph.partition, graph.occurrence,
                edge.source, edge.target, edge.source_event,
                edge.target_event, edge.mode});
    }
    document->report_.authored_event_edges =
        static_cast<int>(document->event_edges_.size());
    return document;
}

bool apply_supported_state(ScreenFrame& frame,
                           rime_state::ApplyReport& report,
                           std::string& error)
{
    report = {};
    error.clear();
    const auto document =
        std::dynamic_pointer_cast<DirectInstallScreenDocument>(frame.document);
    if (!document)
    {
        error = "frame is not backed by a direct-install Rime document";
        return false;
    }
    std::vector<rime_state::StateSlot> slots;
    slots.reserve(frame.explicit_state.size());
    for (const auto& entry : frame.explicit_state)
    {
        if (!document->contains(entry.first.element))
        {
            error = "frame contains state for an element outside its document";
            return false;
        }
        rime_state::StateSlot slot;
        slot.partition = entry.first.element.partition;
        slot.local_instance = entry.first.element.local_instance;
        slot.field = entry.first.field;
        for (const OccurrenceStep& step : entry.first.element.occurrence)
            slot.occurrence.push_back(
                {step.owner_partition, step.reference_instance});
        std::visit([&slot](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, bool>)
                slot.value = rime_state::Value::from_bool(value);
            else if constexpr (std::is_same_v<T, int64_t>)
                slot.value = rime_state::Value::from_int(value);
            else if constexpr (std::is_same_v<T, uint64_t>)
                slot.value = rime_state::Value::from_uint(value);
            else if constexpr (std::is_same_v<T, double>)
                slot.value = rime_state::Value::from_real(value);
            else if constexpr (std::is_same_v<T, std::string>)
                slot.value = rime_state::Value::from_string(value);
        }, entry.second);
        slots.push_back(std::move(slot));
    }
    report = rime_state::apply_state(document->screen(), slots);
    rime::solve(document->screen(), 1920.f, 1080.f);
    return true;
}

} // namespace bf6_ui
