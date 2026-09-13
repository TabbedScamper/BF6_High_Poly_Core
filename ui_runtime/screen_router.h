#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bf6_ui {

/* Current native BF6 translates the four named concepts to these direction
 * values before dispatch.  The concept ids themselves are derived from the
 * names at runtime; no copied hash table participates. */
enum class NativeDirection : int32_t {
    Up = 2,
    Down = 3,
    Left = 4,
    Right = 5,
    Unresolved = 8,
};

uint32_t action_id(std::string_view name);
std::optional<NativeDirection> direction_for_action(uint32_t id);

struct ScreenIdentity {
    std::string path;
    uint32_t installed_size = 0;

    bool operator==(const ScreenIdentity& other) const {
        return path == other.path && installed_size == other.installed_size;
    }
};

struct OccurrenceStep {
    std::string owner_partition;
    int32_t reference_instance = -1;

    bool operator==(const OccurrenceStep& other) const {
        return owner_partition == other.owner_partition &&
               reference_instance == other.reference_instance;
    }
    bool operator<(const OccurrenceStep& other) const {
        if (owner_partition != other.owner_partition)
            return owner_partition < other.owner_partition;
        return reference_instance < other.reference_instance;
    }
};

struct ElementAddress {
    std::string partition;
    std::vector<OccurrenceStep> occurrence;
    int32_t local_instance = -1;

    bool operator==(const ElementAddress& other) const {
        return partition == other.partition &&
               occurrence == other.occurrence &&
               local_instance == other.local_instance;
    }
    bool operator<(const ElementAddress& other) const {
        if (partition != other.partition) return partition < other.partition;
        if (occurrence != other.occurrence) return occurrence < other.occurrence;
        return local_instance < other.local_instance;
    }
};

struct AuthoredEventEdge {
    std::string partition;
    std::vector<OccurrenceStep> occurrence;
    int32_t source_instance = -1;
    int32_t target_instance = -1;
    uint32_t source_event = 0;
    uint32_t target_event = 0;
    int32_t mode = 0;

    bool operator==(const AuthoredEventEdge& other) const {
        return partition == other.partition &&
               occurrence == other.occurrence &&
               source_instance == other.source_instance &&
               target_instance == other.target_instance &&
               source_event == other.source_event &&
               target_event == other.target_event && mode == other.mode;
    }
};

/* A document is one screen read from the currently mounted install.  Concrete
 * renderer integrations may derive from this and retain their native Rime
 * tree, images and compiled property graphs in memory. */
class AuthoredScreenDocument {
public:
    virtual ~AuthoredScreenDocument() = default;
    virtual const ScreenIdentity& identity() const = 0;
    virtual bool contains(const ElementAddress& address) const = 0;
    virtual const std::vector<AuthoredEventEdge>& event_edges() const = 0;
};

class ScreenSource {
public:
    virtual ~ScreenSource() = default;

    /* Implementations enumerate the active install on every refresh.  A
     * research table or exported manifest is not a valid source. */
    virtual bool enumerate(std::vector<ScreenIdentity>& out,
                           std::string& error) = 0;
    virtual std::shared_ptr<AuthoredScreenDocument> load(
        const ScreenIdentity& identity, std::string& error) = 0;
};

struct FocusState {
    bool resolved = false;
    ElementAddress element;
};

using StateValue = std::variant<bool, int64_t, uint64_t, double, std::string>;

struct StateKey {
    ElementAddress element;
    uint32_t field = 0;

    bool operator<(const StateKey& other) const {
        if (element < other.element) return true;
        if (other.element < element) return false;
        return field < other.field;
    }
};

struct ScreenFrame {
    std::shared_ptr<AuthoredScreenDocument> document;
    FocusState focus;
    std::map<StateKey, StateValue> explicit_state;
};

enum class RouteDecisionKind {
    Unresolved,
    Push,
    Replace,
    Pop,
    Close,
};

struct RouteDecision {
    RouteDecisionKind kind = RouteDecisionKind::Unresolved;
    std::string target;
};

/* Back and tab selection are intentionally outside the router.  The current
 * game does not expose a proven generic destination law for either.  An
 * offline application can opt into a policy without mislabelling it as an
 * authored BF6 result. */
class NavigationPolicy {
public:
    virtual ~NavigationPolicy() = default;
    virtual RouteDecision back(const std::vector<ScreenFrame>& stack) = 0;
    virtual RouteDecision tab(const std::vector<ScreenFrame>& stack,
                              int32_t delta) = 0;
};

enum class RouterEventKind {
    CatalogueRefreshed,
    RoutePushed,
    RouteReplaced,
    RoutePopped,
    RouteClosed,
    RouteRejected,
    DirectionDispatched,
    BackUnresolved,
    TabUnresolved,
    FocusChanged,
    FocusCleared,
    StateChanged,
    AuthoredEventDispatched,
};

struct RouterEvent {
    uint64_t sequence = 0;
    RouterEventKind kind = RouterEventKind::RouteRejected;
    std::string route;
    std::string target_route;
    uint32_t action = 0;
    NativeDirection direction = NativeDirection::Unresolved;
    int32_t tab_delta = 0;
    std::optional<ElementAddress> element;
    uint32_t field = 0;
    std::optional<AuthoredEventEdge> authored_edge;
    std::string detail;
};

class RouterEventSink {
public:
    virtual ~RouterEventSink() = default;
    virtual void on_router_event(const RouterEvent& event) = 0;
};

class ScreenRouter {
public:
    explicit ScreenRouter(ScreenSource& source,
                          RouterEventSink* sink = nullptr,
                          NavigationPolicy* policy = nullptr);

    bool refresh(std::string& error);
    const std::vector<ScreenIdentity>& catalogue() const { return catalogue_; }
    const ScreenIdentity* find(std::string_view path) const;

    bool push(std::string_view exact_path, std::string& error);
    bool replace(std::string_view exact_path, std::string& error);
    bool pop(std::string& error);
    void reset();

    bool request_back(std::string& error);
    bool request_tab(int32_t delta, std::string& error);

    /* This is the exact known native boundary: successful dispatch reports
     * direction 2/3/4/5 but does not invent the next focused element. */
    bool dispatch_action(uint32_t concept_id);

    /* A decoded focus manager may supply an exact authored address here.
     * The router verifies that it exists in the current loaded document. */
    bool set_focus(const ElementAddress& element, std::string& error);
    bool clear_focus(std::string& error);

    /* Explicit provider/evaluator state is occurrence scoped.  Storing it
     * does not imply renderer semantics; the integration applies only fields
     * supported by its decoded state bridge. */
    bool set_state(const StateKey& key, StateValue value, std::string& error);

    /* Dispatch only an edge byte-for-byte present in the loaded screen.
     * Mutated/fabricated edges are rejected. */
    bool dispatch_authored_event(const AuthoredEventEdge& edge,
                                 std::string& error);

    const std::vector<ScreenFrame>& stack() const { return stack_; }
    const ScreenFrame* current() const;
    ScreenFrame* current();
    bool closed() const { return closed_; }

    void set_policy(NavigationPolicy* policy) { policy_ = policy; }
    void set_sink(RouterEventSink* sink) { sink_ = sink; }

private:
    bool load_frame(std::string_view exact_path, ScreenFrame& frame,
                    std::string& error);
    bool apply_decision(const RouteDecision& decision, bool from_tab,
                        int32_t tab_delta, std::string& error);
    void emit(RouterEvent event);

    ScreenSource& source_;
    RouterEventSink* sink_ = nullptr;
    NavigationPolicy* policy_ = nullptr;
    std::vector<ScreenIdentity> catalogue_;
    std::vector<ScreenFrame> stack_;
    uint64_t next_sequence_ = 1;
    bool closed_ = false;
};

} // namespace bf6_ui
