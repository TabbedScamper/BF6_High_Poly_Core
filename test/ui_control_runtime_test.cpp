#include "ui_control_runtime.h"

#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace bf6_ui;

class FakeDocument final : public AuthoredScreenDocument {
public:
    FakeDocument() : identity_{"common/ui/test/screens/controls", 42}
    {
        elements_.push_back({"common/ui/test/controls", {}, 7});
        edges_.push_back({"common/ui/test/controls", {}, 7, 8,
                          action_id("Clicked"), action_id("OnActivate"), 2});
        /* Negative polarity control: the control is only the target and the
         * target pin happens to carry an activation-looking identity. */
        edges_.push_back({"common/ui/test/controls", {}, 99, 7,
                          action_id("UnrelatedOutput"), action_id("Clicked"), 2});
    }
    const ScreenIdentity& identity() const override { return identity_; }
    bool contains(const ElementAddress& value) const override {
        return value == elements_.front();
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
    bool enumerate(std::vector<ScreenIdentity>& out, std::string& error) override {
        error.clear(); out = {{"common/ui/test/screens/controls", 42}}; return true;
    }
    std::shared_ptr<AuthoredScreenDocument> load(
        const ScreenIdentity&, std::string& error) override {
        error.clear(); document = std::make_shared<FakeDocument>(); return document;
    }
    std::shared_ptr<FakeDocument> document;
};

bool require(bool value, const char* message)
{
    if (value) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}
}

int main()
{
    bool ok = true;
    size_t count = 0;
    const ControlActionIdentity* actions = control_action_contracts(&count);
    ok &= require(count == 26, "current action contract count changed");
    std::set<uint32_t> unique;
    for (size_t i = 0; i < count; ++i) {
        ok &= require(actions[i].id == action_id(actions[i].name),
                      "action id was not computed from its exact name");
        unique.insert(actions[i].id);
        ok &= require(find_control_action(actions[i].id ^ 1u) == nullptr,
                      "xor-one action control was accepted");
    }
    ok &= require(unique.size() == count, "action contract contains an id collision");

    FakeSource source;
    ScreenRouter router(source);
    ControlRuntime runtime(router);
    std::string error;
    ok &= require(router.refresh(error), "router refresh failed");
    ok &= require(router.push("common/ui/test/screens/controls", error),
                  "exact test route failed");

    std::vector<AuthoredEventEdge> activation;
    ok &= require(exact_control_activation_candidates(
                      *source.document, source.document->element(),
                      activation, error),
                  "source-side activation candidate was not found");
    ok &= require(activation.size() == 1 &&
                      activation.front().source_instance == 7,
                  "target-side activation lookalike passed polarity gate");
    ElementAddress shuffled_source = source.document->element();
    shuffled_source.local_instance = 99;
    ok &= require(!exact_control_activation_candidates(
                      *source.document, shuffled_source, activation, error),
                  "non-element shuffled source passed activation gate");

    const char* directions[] = {"ConceptNavigateUp", "ConceptNavigateDown",
                                "ConceptNavigateLeft", "ConceptNavigateRight"};
    for (const char* name : directions) {
        auto pressed = runtime.dispatch_input(
            {action_id(name), InputPhase::Pressed}, error);
        ok &= require(pressed.kind == ControlDispatchKind::DirectionDispatched,
                      "direction press did not dispatch");
        auto repeat = runtime.dispatch_input(
            {action_id(name), InputPhase::Repeat}, error);
        ok &= require(repeat.kind == ControlDispatchKind::DirectionDispatched,
                      "direction repeat did not dispatch");
        auto released = runtime.dispatch_input(
            {action_id(name), InputPhase::Released}, error);
        ok &= require(released.kind == ControlDispatchKind::PhaseIgnored,
                      "direction release was not fail-closed");
    }
    ok &= require(runtime.dispatch_input(
        {action_id("ConceptMenuBack"), InputPhase::Repeat}, error).kind ==
            ControlDispatchKind::PhaseIgnored,
        "Back repeat was accepted");
    ok &= require(runtime.dispatch_navigation(
        NavigationIntent::Forward, InputPhase::Pressed, error).kind ==
            ControlDispatchKind::ForwardUnresolved,
        "unproven Forward law was accepted");
    ok &= require(runtime.dispatch_navigation(
        NavigationIntent::Back, InputPhase::Pressed, error).kind ==
            ControlDispatchKind::Rejected,
        "Back without an explicit router policy was accepted");

    ok &= require(runtime.dispatch_focus_target(
        action_id("ConceptNavigateRight"), InputPhase::Pressed,
        source.document->element(), error).kind ==
            ControlDispatchKind::FocusTargetApplied,
        "provider-supplied exact focus target was rejected");
    ElementAddress fake = source.document->element();
    fake.local_instance ^= 1;
    ok &= require(runtime.dispatch_focus_target(
        action_id("ConceptNavigateRight"), InputPhase::Pressed, fake, error).kind ==
            ControlDispatchKind::Rejected,
        "mutated focus target was accepted");

    ok &= require(runtime.bind_offline(
        {InputDevice::Keyboard, "test:key-up", action_id("ConceptNavigateUp")},
        error), "explicit offline binding failed");
    ok &= require(runtime.dispatch_physical(
        InputDevice::Keyboard, "test:key-up", error).kind ==
            ControlDispatchKind::DirectionDispatched,
        "explicit offline binding did not dispatch");
    ok &= require(!runtime.bind_offline(
        {InputDevice::Keyboard, "test:key-up", action_id("ConceptNavigateDown")},
        error), "conflicting offline binding was accepted");
    ok &= require(runtime.dispatch_physical(
        InputDevice::Gamepad, "test:key-up", error).kind ==
            ControlDispatchKind::Rejected,
        "unbound device/control pairing was accepted");

    std::printf("actions=%zu directions=4 pressed=4 repeat=4 released=0 "
                "forward=unresolved source_activation=1 target_activation=0 "
                "fake_action=0 fake_focus=0 "
                "physical_profile=explicit-only\n", count);
    return ok ? 0 : 1;
}
