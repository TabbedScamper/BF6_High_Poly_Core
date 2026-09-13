#include "screen_router.h"

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace bf6_ui;

class FakeDocument final : public AuthoredScreenDocument {
public:
    explicit FakeDocument(ScreenIdentity identity)
        : identity_(std::move(identity))
    {
        elements_.push_back({identity_.path + "/root", {}, 7});
        edges_.push_back({identity_.path + "/root", {}, 7, 8,
                          0x11111111u, 0x22222222u, 2});
    }

    const ScreenIdentity& identity() const override { return identity_; }
    bool contains(const ElementAddress& address) const override {
        for (const ElementAddress& element : elements_)
            if (element == address) return true;
        return false;
    }
    const std::vector<AuthoredEventEdge>& event_edges() const override {
        return edges_;
    }
    const ElementAddress& element() const { return elements_.front(); }

private:
    ScreenIdentity identity_;
    std::vector<ElementAddress> elements_;
    std::vector<AuthoredEventEdge> edges_;
};

class FakeSource final : public ScreenSource {
public:
    FakeSource()
    {
        for (int index = 256; index >= 0; --index)
            identities.push_back({
                "common/ui/test/screens/screen" + std::to_string(index),
                static_cast<uint32_t>(1000 + index)});
    }

    bool enumerate(std::vector<ScreenIdentity>& out,
                   std::string& error) override {
        error.clear();
        out = identities;
        return true;
    }

    std::shared_ptr<AuthoredScreenDocument> load(
        const ScreenIdentity& identity, std::string& error) override {
        error.clear();
        ++loads;
        ScreenIdentity result = identity;
        if (return_mismatched_identity) result.path += "_shuffled";
        return std::make_shared<FakeDocument>(std::move(result));
    }

    std::vector<ScreenIdentity> identities;
    int loads = 0;
    bool return_mismatched_identity = false;
};

class EventRecorder final : public RouterEventSink {
public:
    void on_router_event(const RouterEvent& event) override {
        events.push_back(event);
    }
    int count(RouterEventKind kind) const {
        int result = 0;
        for (const RouterEvent& event : events)
            if (event.kind == kind) ++result;
        return result;
    }
    std::vector<RouterEvent> events;
};

class ExplicitOfflinePolicy final : public NavigationPolicy {
public:
    RouteDecision back(const std::vector<ScreenFrame>&) override {
        return back_decision;
    }
    RouteDecision tab(const std::vector<ScreenFrame>&, int32_t) override {
        return tab_decision;
    }
    RouteDecision back_decision{RouteDecisionKind::Pop, {}};
    RouteDecision tab_decision{RouteDecisionKind::Unresolved, {}};
};

bool require(bool condition, const char* message)
{
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

} // namespace

int main()
{
    bool ok = true;
    ok &= require(action_id("ConceptNavigateUp") == 0xC0107327u,
                  "Up hash differs from current executable proof");
    ok &= require(action_id("ConceptNavigateDown") == 0x05F1DDB0u,
                  "Down hash differs from current executable proof");
    ok &= require(action_id("ConceptNavigateLeft") == 0x05F63519u,
                  "Left hash differs from current executable proof");
    ok &= require(action_id("ConceptNavigateRight") == 0xC4A19042u,
                  "Right hash differs from current executable proof");

    FakeSource source;
    EventRecorder events;
    ScreenRouter router(source, &events);
    std::string error;
    ok &= require(router.refresh(error), "257-row catalogue refresh failed");
    ok &= require(router.catalogue().size() == 257,
                  "router did not preserve all 257 screen rows");
    ok &= require(router.catalogue().front().path ==
                      "common/ui/test/screens/screen0",
                  "catalogue was not sorted deterministically");

    const int before_fake_load = source.loads;
    ok &= require(!router.push("common/ui/test/screens/fake", error),
                  "fabricated route was accepted");
    ok &= require(source.loads == before_fake_load,
                  "fabricated route reached the source loader");

    const std::string screen0 = "common/ui/test/screens/screen0";
    const std::string screen1 = "common/ui/test/screens/screen1";
    const std::string screen2 = "common/ui/test/screens/screen2";
    ok &= require(router.push(screen0, error), "exact route push failed");
    ok &= require(router.stack().size() == 1,
                  "route stack size after push is wrong");

    const size_t unresolved_depth = router.stack().size();
    ok &= require(!router.request_back(error),
                  "Back without a policy fabricated a destination");
    ok &= require(!router.request_tab(1, error),
                  "tab without a policy fabricated a destination");
    ok &= require(router.stack().size() == unresolved_depth,
                  "unresolved navigation mutated the stack");

    const struct {
        const char* name;
        NativeDirection direction;
    } directions[] = {
        {"ConceptNavigateUp", NativeDirection::Up},
        {"ConceptNavigateDown", NativeDirection::Down},
        {"ConceptNavigateLeft", NativeDirection::Left},
        {"ConceptNavigateRight", NativeDirection::Right},
    };
    const int direction_events_before =
        events.count(RouterEventKind::DirectionDispatched);
    for (const auto& direction : directions)
    {
        ok &= require(router.dispatch_action(action_id(direction.name)),
                      "exact direction action was rejected");
        ok &= require(events.events.back().direction == direction.direction,
                      "exact action mapped to the wrong native direction");
        ok &= require(!router.dispatch_action(action_id(direction.name) ^ 1u),
                      "xor-one direction control dispatched");
    }
    ok &= require(events.count(RouterEventKind::DirectionDispatched) ==
                      direction_events_before + 4,
                  "direction dispatch count did not match four real controls");

    const auto document = std::dynamic_pointer_cast<FakeDocument>(
        router.current()->document);
    ok &= require(static_cast<bool>(document), "test document type mismatch");
    const ElementAddress exact_element = document->element();
    ok &= require(router.set_focus(exact_element, error),
                  "exact focus address rejected");
    ElementAddress fake_element = exact_element;
    fake_element.local_instance ^= 0x40000000;
    ok &= require(!router.set_focus(fake_element, error),
                  "fabricated focus address accepted");
    ok &= require(router.set_state(
                      {exact_element, 0xABCDEF01u}, true, error),
                  "exact occurrence-scoped state rejected");
    ok &= require(!router.set_state(
                      {fake_element, 0xABCDEF01u}, true, error),
                  "fabricated occurrence-scoped state accepted");

    const AuthoredEventEdge exact_edge =
        document->event_edges().front();
    ok &= require(router.dispatch_authored_event(exact_edge, error),
                  "exact authored event edge rejected");
    AuthoredEventEdge fake_edge = exact_edge;
    fake_edge.target_event ^= 1u;
    ok &= require(!router.dispatch_authored_event(fake_edge, error),
                  "mutated authored event edge accepted");

    ok &= require(router.push(screen1, error), "second exact push failed");
    ExplicitOfflinePolicy policy;
    router.set_policy(&policy);
    ok &= require(router.request_back(error), "explicit pop policy failed");
    ok &= require(router.stack().size() == 1 &&
                      router.current()->document->identity().path == screen0,
                  "explicit pop policy reached the wrong screen");
    policy.tab_decision = {RouteDecisionKind::Replace, screen1};
    ok &= require(router.request_tab(1, error),
                  "explicit tab replace policy failed");
    ok &= require(router.stack().size() == 1 &&
                      router.current()->document->identity().path == screen1,
                  "explicit tab policy reached the wrong screen");
    policy.tab_decision = {
        RouteDecisionKind::Push, "common/ui/test/screens/fake"};
    ok &= require(!router.request_tab(1, error),
                  "policy-provided fabricated route was accepted");

    router.reset();
    source.return_mismatched_identity = true;
    ok &= require(!router.push(screen2, error),
                  "shuffled loaded identity was accepted");
    ok &= require(router.stack().empty(),
                  "identity-control failure mutated the stack");

    FakeSource duplicate_source;
    duplicate_source.identities.push_back(duplicate_source.identities.front());
    ScreenRouter duplicate_router(duplicate_source);
    ok &= require(!duplicate_router.refresh(error),
                  "duplicate catalogue control was accepted");

    for (size_t index = 1; index < events.events.size(); ++index)
        ok &= require(events.events[index - 1].sequence <
                          events.events[index].sequence,
                      "event sequence is not strictly increasing");

    std::printf(
        "catalogue=%zu real_directions=%d xor_directions=0 "
        "fake_route=0 fake_focus=0 fake_state=0 fake_event=0 "
        "shuffled_identity=0 duplicate_catalogue=0\n",
        router.catalogue().size(),
        events.count(RouterEventKind::DirectionDispatched));
    return ok ? 0 : 1;
}
