#pragma once

#include "authored_event_dispatch.h"
#include "install_screen_source.h"
#include "screen_router.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf6_ui {

enum class ControlActionKind {
    Generic,
    Direction,
    Back,
    TabLeft,
    TabRight,
    Activate,
};

struct ControlActionIdentity {
    const char* name = nullptr;
    uint32_t id = 0;
    ControlActionKind kind = ControlActionKind::Generic;
};

/* Exact concepts observed in the current-install authored input-flow corpus.
 * The ids are always recomputed from names; this array is a read contract,
 * not a physical-key mapping or a selected-screen claim. */
const ControlActionIdentity* control_action_contracts(size_t* count = nullptr);
const ControlActionIdentity* find_control_action(uint32_t id);

/* Select exact source-side activation fanout for one control occurrence.
 * A matching target event is never evidence that the control emitted it. */
bool exact_control_activation_candidates(
    const AuthoredScreenDocument& document, const ElementAddress& control,
    std::vector<AuthoredEventEdge>& out, std::string& error);

enum class InputDevice {
    Keyboard,
    Mouse,
    Gamepad,
    Joystick,
};

/* Device-neutral phase emitted by the host.  It is deliberately separate
 * from a physical key/button: current-install profile selection is not yet a
 * decoded value path. */
enum class InputPhase {
    Pressed,
    Released,
    Repeat,
};

enum class NavigationIntent {
    Back,
    Forward,
    TabLeft,
    TabRight,
};

enum class ActivationMode {
    Unresolved,
    OnPress,
    OnRelease,
    Hold,
};

struct ControllerNeutralInput {
    uint32_t concept_id = 0;
    InputPhase phase = InputPhase::Pressed;
};

struct PhysicalBinding {
    InputDevice device = InputDevice::Keyboard;
    std::string host_code;
    uint32_t concept_id = 0;
};

struct InteractableControl {
    ElementAddress address;
    std::string type_name;
    bool has_authored_rect = false;
    float x0 = 0.f;
    float y0 = 0.f;
    float x1 = 0.f;
    float y1 = 0.f;
    /* These provider values exist in the shipped InteractableButton DBD, but
     * remain unresolved until a live provider supplies them. */
    ActivationMode activation_mode = ActivationMode::Unresolved;
    bool focusable_resolved = false;
    bool focusable = false;
    bool mouse_enabled_resolved = false;
    bool mouse_enabled = false;
    /* These are exact graph edges adjacent to the input behavior whose pin id
     * matches a shipped activation name.  Multiple candidates stay multiple;
     * the runtime never chooses one by graph order. */
    std::vector<AuthoredEventEdge> activation_candidates;
};

struct ControlSurface {
    std::string screen;
    std::vector<InteractableControl> interactables;
    int activation_candidates = 0;
    int ambiguous_activation_controls = 0;
    /* Runtime provider values not serialized by the screen. */
    bool focus_owner_resolved = false;
    bool focusable_values_resolved = false;
    bool hover_pressed_selected_resolved = false;
};

struct LiveControlState {
    ElementAddress address;
    ActivationMode activation_mode = ActivationMode::Unresolved;
    std::optional<bool> focusable;
    std::optional<bool> mouse_enabled;
};

struct InstallControlAudit {
    int action_contracts = 0;
    int exact_exe_action_names = 0;
    int ambiguous_exe_action_names = 0;
    int xor_one_action_hits = 0;
    int shuffled_action_pairs = 0;
    int dbd_contracts_expected = 0;
    int dbd_contracts_loaded = 0;
    int dbd_data_name_mismatches = 0;
    int dbd_fields_expected = 0;
    int dbd_fields_matched = 0;
    int dbd_fields_missing = 0;
    int dbd_type_mismatches = 0;
    int dbd_mutated_type_matches = 0;
    int mapping_assets_expected = 0;
    int mapping_assets_found = 0;
    int fake_mapping_asset_matches = 0;
    /* No current ABI decodes profile-selected physical mappings. */
    int physical_binding_values = 0;
    std::vector<std::string> errors;

    bool contracts_passed() const;
    bool physical_mapping_resolved() const {
        return physical_binding_values != 0;
    }
};

/* Reads only the caller's current mounted install and its bf6.exe.  It does
 * not consume research JSON/TSV or an exported input profile. */
InstallControlAudit audit_current_install_controls(
    bf6_ctx* context, const std::string& game_directory);

enum class ControlDispatchKind {
    Rejected,
    DirectionDispatched,
    BackDelegated,
    TabDelegated,
    ActivationUnresolved,
    GenericUnresolved,
    AuthoredActivationDispatched,
    PhaseIgnored,
    ForwardUnresolved,
    FocusTargetApplied,
    PointerAmbiguous,
};

struct ControlDispatch {
    ControlDispatchKind kind = ControlDispatchKind::Rejected;
    uint32_t concept_id = 0;
    std::optional<NativeDirection> direction;
    std::string detail;

    bool dispatched() const {
        return kind == ControlDispatchKind::DirectionDispatched ||
               kind == ControlDispatchKind::BackDelegated ||
               kind == ControlDispatchKind::TabDelegated ||
               kind == ControlDispatchKind::AuthoredActivationDispatched ||
               kind == ControlDispatchKind::FocusTargetApplied;
    }
};

/* Per-screen input bridge coordinated with ScreenRouter. Direction, Back and
 * tabs enter the router's proven boundaries. Activation requires the caller
 * to choose one exact authored candidate; current evidence does not justify
 * auto-picking a Clicked edge from multiple candidates. */
class ControlRuntime {
public:
    explicit ControlRuntime(ScreenRouter& router) : router_(router) {}

    bool refresh_surface(std::string& error);
    bool apply_live_control_state(const LiveControlState& state,
                                  std::string& error);
    const ControlSurface& surface() const { return surface_; }

    std::vector<size_t> hit_test(float x, float y) const;
    ControlDispatch dispatch_input(const ControllerNeutralInput& input,
                                   std::string& error);
    ControlDispatch dispatch_concept(uint32_t concept_id,
                                     std::string& error);
    ControlDispatch dispatch_navigation(NavigationIntent intent,
                                        InputPhase phase,
                                        std::string& error);
    /* Applies only a caller/provider supplied exact target.  The runtime
     * never derives focus adjacency from rectangle proximity. */
    ControlDispatch dispatch_focus_target(uint32_t direction_concept,
                                          InputPhase phase,
                                          const ElementAddress& exact_target,
                                          std::string& error);
    /* A mouse hit may overlap several authored rectangles.  The caller must
     * select an exact occurrence and exact authored edge; ambiguity is
     * reported rather than resolved by draw order. */
    ControlDispatch dispatch_pointer(float x, float y, InputPhase phase,
                                     const ElementAddress* exact_control,
                                     const AuthoredEventEdge* exact_edge,
                                     std::string& error);
    ControlDispatch dispatch_activation(
        const ElementAddress& control, const AuthoredEventEdge& edge,
        std::string& error);

    /* Physical bindings are explicitly an offline host policy until the live
     * BF6 input-manager/profile value path is decoded. */
    bool bind_offline(PhysicalBinding binding, std::string& error);
    ControlDispatch dispatch_physical(InputDevice device,
                                      std::string_view host_code,
                                      std::string& error);
    ControlDispatch dispatch_physical(InputDevice device,
                                      std::string_view host_code,
                                      InputPhase phase,
                                      std::string& error);
    size_t offline_binding_count() const { return bindings_.size(); }

private:
    struct BindingKey {
        InputDevice device{};
        std::string code;
        bool operator<(const BindingKey& other) const {
            if (device != other.device) return device < other.device;
            return code < other.code;
        }
    };

    ScreenRouter& router_;
    ControlSurface surface_;
    std::map<BindingKey, uint32_t> bindings_;
};

} // namespace bf6_ui
