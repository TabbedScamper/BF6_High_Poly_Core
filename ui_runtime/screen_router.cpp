#include "screen_router.h"

#include <algorithm>
#include <utility>

namespace bf6_ui {
namespace {

std::string current_route(const std::vector<ScreenFrame>& stack)
{
    if (stack.empty() || !stack.back().document) return {};
    return stack.back().document->identity().path;
}

} // namespace

uint32_t action_id(std::string_view name)
{
    uint32_t value = 5381u;
    for (const unsigned char ch : name)
        value = ((value << 5u) + value) ^ static_cast<uint32_t>(ch);
    return value;
}

std::optional<NativeDirection> direction_for_action(uint32_t id)
{
    struct Mapping { const char* name; NativeDirection direction; };
    static const Mapping mappings[] = {
        {"ConceptNavigateUp", NativeDirection::Up},
        {"ConceptNavigateDown", NativeDirection::Down},
        {"ConceptNavigateLeft", NativeDirection::Left},
        {"ConceptNavigateRight", NativeDirection::Right},
    };
    for (const Mapping& mapping : mappings)
        if (action_id(mapping.name) == id) return mapping.direction;
    return std::nullopt;
}

ScreenRouter::ScreenRouter(ScreenSource& source, RouterEventSink* sink,
                           NavigationPolicy* policy)
    : source_(source), sink_(sink), policy_(policy)
{
}

bool ScreenRouter::refresh(std::string& error)
{
    error.clear();
    if (!stack_.empty())
    {
        error = "cannot refresh screen catalogue while a route is active";
        return false;
    }
    std::vector<ScreenIdentity> fresh;
    if (!source_.enumerate(fresh, error)) return false;
    std::sort(fresh.begin(), fresh.end(), [](const ScreenIdentity& a,
                                             const ScreenIdentity& b) {
        if (a.path != b.path) return a.path < b.path;
        return a.installed_size < b.installed_size;
    });
    for (size_t index = 0; index < fresh.size(); ++index)
    {
        if (fresh[index].path.empty())
        {
            error = "screen source returned an empty path";
            return false;
        }
        if (index && fresh[index - 1].path == fresh[index].path)
        {
            error = fresh[index - 1].installed_size == fresh[index].installed_size
                ? "screen source returned a duplicate path"
                : "screen source returned conflicting identities for one path";
            return false;
        }
    }
    catalogue_ = std::move(fresh);
    closed_ = false;
    RouterEvent event;
    event.kind = RouterEventKind::CatalogueRefreshed;
    event.detail = std::to_string(catalogue_.size());
    emit(std::move(event));
    return true;
}

const ScreenIdentity* ScreenRouter::find(std::string_view path) const
{
    const auto it = std::lower_bound(
        catalogue_.begin(), catalogue_.end(), path,
        [](const ScreenIdentity& item, std::string_view candidate) {
            return item.path < candidate;
        });
    return it != catalogue_.end() && it->path == path ? &*it : nullptr;
}

bool ScreenRouter::push(std::string_view exact_path, std::string& error)
{
    ScreenFrame frame;
    if (!load_frame(exact_path, frame, error)) return false;
    stack_.push_back(std::move(frame));
    closed_ = false;
    RouterEvent event;
    event.kind = RouterEventKind::RoutePushed;
    event.route = current_route(stack_);
    emit(std::move(event));
    return true;
}

bool ScreenRouter::replace(std::string_view exact_path, std::string& error)
{
    const std::string old_route = current_route(stack_);
    ScreenFrame replacement;
    if (!load_frame(exact_path, replacement, error)) return false;
    if (!stack_.empty())
        stack_.back() = std::move(replacement);
    else
        stack_.push_back(std::move(replacement));
    RouterEvent event;
    event.kind = RouterEventKind::RouteReplaced;
    event.route = current_route(stack_);
    event.target_route = old_route;
    emit(std::move(event));
    return true;
}

bool ScreenRouter::pop(std::string& error)
{
    error.clear();
    if (stack_.size() <= 1)
    {
        error = "route pop would invent behavior at the stack root";
        return false;
    }
    const std::string removed = current_route(stack_);
    stack_.pop_back();
    RouterEvent event;
    event.kind = RouterEventKind::RoutePopped;
    event.route = current_route(stack_);
    event.target_route = removed;
    emit(std::move(event));
    return true;
}

void ScreenRouter::reset()
{
    stack_.clear();
    closed_ = false;
}

bool ScreenRouter::request_back(std::string& error)
{
    error.clear();
    if (stack_.empty())
    {
        error = "no active route";
        return false;
    }
    if (!policy_)
    {
        error = "Back destination is unresolved; no offline policy installed";
        RouterEvent event;
        event.kind = RouterEventKind::BackUnresolved;
        event.route = current_route(stack_);
        event.detail = error;
        emit(std::move(event));
        return false;
    }
    return apply_decision(policy_->back(stack_), false, 0, error);
}

bool ScreenRouter::request_tab(int32_t delta, std::string& error)
{
    error.clear();
    if (stack_.empty())
    {
        error = "no active route";
        return false;
    }
    if (!policy_)
    {
        error = "tab destination is unresolved; no offline policy installed";
        RouterEvent event;
        event.kind = RouterEventKind::TabUnresolved;
        event.route = current_route(stack_);
        event.tab_delta = delta;
        event.detail = error;
        emit(std::move(event));
        return false;
    }
    return apply_decision(policy_->tab(stack_, delta), true, delta, error);
}

bool ScreenRouter::dispatch_action(uint32_t concept_id)
{
    if (stack_.empty()) return false;
    const std::optional<NativeDirection> direction =
        direction_for_action(concept_id);
    if (!direction) return false; // exact current native no-dispatch branch
    RouterEvent event;
    event.kind = RouterEventKind::DirectionDispatched;
    event.route = current_route(stack_);
    event.action = concept_id;
    event.direction = *direction;
    if (stack_.back().focus.resolved)
        event.element = stack_.back().focus.element;
    emit(std::move(event));
    return true;
}

bool ScreenRouter::set_focus(const ElementAddress& element, std::string& error)
{
    error.clear();
    ScreenFrame* frame = current();
    if (!frame || !frame->document)
    {
        error = "no active route";
        return false;
    }
    if (!frame->document->contains(element))
    {
        error = "focus target is not an exact element of the active document";
        return false;
    }
    frame->focus.resolved = true;
    frame->focus.element = element;
    RouterEvent event;
    event.kind = RouterEventKind::FocusChanged;
    event.route = frame->document->identity().path;
    event.element = element;
    emit(std::move(event));
    return true;
}

bool ScreenRouter::clear_focus(std::string& error)
{
    error.clear();
    ScreenFrame* frame = current();
    if (!frame)
    {
        error = "no active route";
        return false;
    }
    frame->focus = {};
    RouterEvent event;
    event.kind = RouterEventKind::FocusCleared;
    event.route = frame->document->identity().path;
    emit(std::move(event));
    return true;
}

bool ScreenRouter::set_state(const StateKey& key, StateValue value,
                             std::string& error)
{
    error.clear();
    ScreenFrame* frame = current();
    if (!frame || !frame->document)
    {
        error = "no active route";
        return false;
    }
    if (!frame->document->contains(key.element))
    {
        error = "state target is not an exact element of the active document";
        return false;
    }
    frame->explicit_state[key] = std::move(value);
    RouterEvent event;
    event.kind = RouterEventKind::StateChanged;
    event.route = frame->document->identity().path;
    event.element = key.element;
    event.field = key.field;
    emit(std::move(event));
    return true;
}

bool ScreenRouter::dispatch_authored_event(const AuthoredEventEdge& edge,
                                           std::string& error)
{
    error.clear();
    ScreenFrame* frame = current();
    if (!frame || !frame->document)
    {
        error = "no active route";
        return false;
    }
    const auto& edges = frame->document->event_edges();
    if (std::find(edges.begin(), edges.end(), edge) == edges.end())
    {
        error = "event edge is not present in the active authored graph";
        return false;
    }
    RouterEvent event;
    event.kind = RouterEventKind::AuthoredEventDispatched;
    event.route = frame->document->identity().path;
    event.authored_edge = edge;
    emit(std::move(event));
    return true;
}

const ScreenFrame* ScreenRouter::current() const
{
    return stack_.empty() ? nullptr : &stack_.back();
}

ScreenFrame* ScreenRouter::current()
{
    return stack_.empty() ? nullptr : &stack_.back();
}

bool ScreenRouter::load_frame(std::string_view exact_path, ScreenFrame& frame,
                              std::string& error)
{
    error.clear();
    const ScreenIdentity* identity = find(exact_path);
    if (!identity)
    {
        error = "route is not an exact member of the current-install catalogue";
        RouterEvent event;
        event.kind = RouterEventKind::RouteRejected;
        event.target_route = std::string(exact_path);
        event.detail = error;
        emit(std::move(event));
        return false;
    }
    std::shared_ptr<AuthoredScreenDocument> document =
        source_.load(*identity, error);
    if (!document)
    {
        RouterEvent event;
        event.kind = RouterEventKind::RouteRejected;
        event.target_route = identity->path;
        event.detail = error.empty() ? "source did not load route" : error;
        emit(std::move(event));
        return false;
    }
    if (!(document->identity() == *identity))
    {
        error = "loaded document identity differs from requested install row";
        RouterEvent event;
        event.kind = RouterEventKind::RouteRejected;
        event.target_route = identity->path;
        event.detail = error;
        emit(std::move(event));
        return false;
    }
    frame = ScreenFrame{std::move(document), {}, {}};
    return true;
}

bool ScreenRouter::apply_decision(const RouteDecision& decision,
                                  bool from_tab, int32_t tab_delta,
                                  std::string& error)
{
    switch (decision.kind)
    {
    case RouteDecisionKind::Push:
        return push(decision.target, error);
    case RouteDecisionKind::Replace:
        return replace(decision.target, error);
    case RouteDecisionKind::Pop:
        return pop(error);
    case RouteDecisionKind::Close:
    {
        closed_ = true;
        RouterEvent event;
        event.kind = RouterEventKind::RouteClosed;
        event.route = current_route(stack_);
        emit(std::move(event));
        return true;
    }
    case RouteDecisionKind::Unresolved:
    default:
        error = from_tab ? "tab policy returned unresolved"
                         : "Back policy returned unresolved";
        RouterEvent event;
        event.kind = from_tab ? RouterEventKind::TabUnresolved
                              : RouterEventKind::BackUnresolved;
        event.route = current_route(stack_);
        event.tab_delta = tab_delta;
        event.detail = error;
        emit(std::move(event));
        return false;
    }
}

void ScreenRouter::emit(RouterEvent event)
{
    event.sequence = next_sequence_++;
    if (sink_) sink_->on_router_event(event);
}

} // namespace bf6_ui
