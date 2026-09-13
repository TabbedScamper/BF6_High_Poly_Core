#include "ui_control_runtime.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <unordered_map>
#include <utility>

namespace bf6_ui {
namespace {

struct NamedAction {
    const char* name;
    ControlActionKind kind;
};

constexpr NamedAction kNamedActions[] = {
    {"ConceptActivate", ControlActionKind::Activate},
    {"ConceptBack", ControlActionKind::Back},
    {"ConceptClassTabRight", ControlActionKind::Generic},
    {"ConceptDeployNavigateDown", ControlActionKind::Generic},
    {"ConceptDeployNavigateLeft", ControlActionKind::Generic},
    {"ConceptDeployNavigateRight", ControlActionKind::Generic},
    {"ConceptDeployNavigateUp", ControlActionKind::Generic},
    {"ConceptEdit", ControlActionKind::Generic},
    {"ConceptGraniteInventoryDropSelected", ControlActionKind::Generic},
    {"ConceptHomeMenuNavigateDown", ControlActionKind::Generic},
    {"ConceptHomeMenuNavigateUp", ControlActionKind::Generic},
    {"ConceptInventoryNavigateLeft", ControlActionKind::Generic},
    {"ConceptInventoryNavigateRight", ControlActionKind::Generic},
    {"ConceptInventoryNavigateUp", ControlActionKind::Generic},
    {"ConceptMenuBack", ControlActionKind::Back},
    {"ConceptMenuPrimary", ControlActionKind::Generic},
    {"ConceptMenuSecondary", ControlActionKind::Generic},
    {"ConceptMenuSocial", ControlActionKind::Generic},
    {"ConceptMenuTabLeft", ControlActionKind::TabLeft},
    {"ConceptMenuTabRight", ControlActionKind::TabRight},
    {"ConceptMenuTertiary", ControlActionKind::Generic},
    {"ConceptNavigateDown", ControlActionKind::Direction},
    {"ConceptNavigateLeft", ControlActionKind::Direction},
    {"ConceptNavigateRight", ControlActionKind::Direction},
    {"ConceptNavigateUp", ControlActionKind::Direction},
    {"ConceptSelect", ControlActionKind::Generic},
};

const std::vector<ControlActionIdentity>& actions()
{
    static const std::vector<ControlActionIdentity> result = [] {
        std::vector<ControlActionIdentity> rows;
        rows.reserve(std::size(kNamedActions));
        for (const NamedAction& row : kNamedActions)
            rows.push_back({row.name, action_id(row.name), row.kind});
        return rows;
    }();
    return result;
}

constexpr uint64_t kString = 0x12ull;
constexpr uint64_t kLocalizedString = 0x1Eull;
constexpr uint64_t kEnum = 0x1Aull;
constexpr uint64_t kOpaque22 = 0x22ull;
constexpr uint64_t kOpaque26 = 0x26ull;
constexpr uint64_t kOpaque2a = 0x2Aull;
constexpr uint64_t kBool = 0x80000140ull;
constexpr uint64_t kInt = 0x800001E0ull;
constexpr uint64_t kReal = 0x80000260ull;
constexpr uint64_t kUnique = 0x800000E0ull;

struct FieldSpec { const char* name; uint64_t type; };
struct DbdSpec {
    const char* path;
    const char* data_name;
    const FieldSpec* fields;
    size_t count;
};

constexpr FieldSpec kInteractable[] = {
    {"Label", kLocalizedString}, {"IsSelected", kBool}, {"IsVisible", kBool},
    {"SecondaryInputConcept", kEnum}, {"IsFocusable", kBool},
    {"IsDisabled", kBool}, {"Icon", kOpaque22}, {"IconVisible", kBool},
    {"IsPremiumColor", kBool}, {"HasPayload", kBool},
    {"Payload", kOpaque26}, {"IsShownPromptIcon", kBool},
    {"IsHoldButtonEnabled", kBool}, {"HoldDurationOverride", kReal},
    {"IsActivatedOnButtonUp", kBool}, {"IsPrimaryLabelButton", kBool},
    {"DisableMouseInput", kBool}, {"UISoundStyle_Primary", kOpaque2a},
    {"UISoundStyle_Secondary", kOpaque2a}, {"UseEffect", kBool},
};
constexpr FieldSpec kNavigation[] = {
    {"InputConceptNavigateLeft", kEnum},
    {"InputConceptNavigateRight", kEnum},
    {"CenterBackgroundDisabled", kBool},
    {"UISoundStyle_Primary", kOpaque22},
};
constexpr FieldSpec kInputIcon[] = {
    {"IsAssigned", kBool}, {"FilterType", kEnum},
    {"Progress", kReal}, {"ProgressShape", kString},
    {"IsInteractHold", kBool}, {"IsEnabled", kBool},
};
constexpr FieldSpec kMapping[] = {
    {"KeybindingInputType", kInt}, {"Label", kString},
    {"IsF2P", kBool}, {"IsDefault", kBool}, {"IsVisible", kBool},
};
constexpr FieldSpec kKeyboardMouse[] = {
    {"Title", kEnum}, {"Keyboard", kEnum}, {"Mouse", kEnum},
    {"Available", kBool}, {"Description", kEnum}, {"IsF2P", kBool},
    {"IsMouseDefault", kBool}, {"IsKeyboardDefault", kBool},
    {"IsDefault", kBool}, {"UniqueId", kUnique},
    {"HideMouseBinding", kBool},
};
constexpr FieldSpec kGamepad[] = {
    {"Title", kEnum}, {"Pad", kEnum}, {"Available", kBool},
    {"Description", kEnum}, {"IsF2P", kBool},
    {"IsPadDefault", kBool}, {"IsDefault", kBool},
    {"UniqueId", kUnique},
};
constexpr FieldSpec kJoystick[] = {
    {"Title", kEnum}, {"Joystick", kEnum}, {"Available", kBool},
    {"Description", kEnum}, {"IsF2P", kBool},
    {"IsJoystickDefault", kBool}, {"IsDefault", kBool},
    {"UniqueId", kUnique},
};

constexpr DbdSpec kDbdSpecs[] = {
    {"common/ui/universalmenu/assets/databindings/um_interactablebuttondbd",
     "ButtonData", kInteractable, std::size(kInteractable)},
    {"common/ui/universalmenu/assets/databindings/um_navigationdatadbd",
     "NavigationData", kNavigation, std::size(kNavigation)},
    {"common/ui/static/inputactionicondbd", "Data", kInputIcon,
     std::size(kInputIcon)},
    {"common/ui/options/ui/assets/databindings/optionskeybindingmappingdbd",
     "Data", kMapping, std::size(kMapping)},
    {"common/ui/options/ui/assets/databindings/"
     "uioptionskeyboardmouseinlinekeybindingdbd", "Data", kKeyboardMouse,
     std::size(kKeyboardMouse)},
    {"common/ui/options/ui/assets/databindings/"
     "uioptionsgamepadinlinekeybindingdbd", "Data", kGamepad,
     std::size(kGamepad)},
    {"common/ui/options/ui/assets/databindings/"
     "uioptionsjoystickinlinekeybindingdbd", "Data", kJoystick,
     std::size(kJoystick)},
};

constexpr const char* kMappingAssets[] = {
    "common/gameplay/input/uiinputactionmapping",
    "systems/config/inputs/default_inputactionmaps",
    "common/ui/static/config/inputicon/inputiconmappingkeyboardsetup",
    "common/ui/static/config/inputicon/inputiconmappingmousesetup",
    "common/ui/static/config/inputicon/inputiconmappingjoysticksetup",
    "common/ui/static/config/inputicon/inputiconmappingxboxcontrollersetup",
    "common/ui/static/config/inputicon/inputiconmappingplaystation4controllersetup",
    "common/ui/static/config/inputicon/inputiconmappingplaystationcontrollersetup",
};

bool token_start(unsigned char c)
{
    return std::isalpha(c) != 0 || c == '_';
}

bool token_continue(unsigned char c)
{
    return std::isalnum(c) != 0 || c == '_';
}

std::unordered_map<uint32_t, std::set<std::string>> executable_identifiers(
    const std::filesystem::path& path, std::string& error)
{
    std::unordered_map<uint32_t, std::set<std::string>> result;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "cannot open current bf6.exe";
        return result;
    }
    const std::vector<unsigned char> bytes(
        std::istreambuf_iterator<char>(stream), {});
    for (size_t at = 0; at < bytes.size();) {
        if (!token_start(bytes[at])) { ++at; continue; }
        const size_t begin = at++;
        while (at < bytes.size() && token_continue(bytes[at])) ++at;
        if (at - begin < 3) continue;
        const std::string token(reinterpret_cast<const char*>(bytes.data() + begin),
                                at - begin);
        result[action_id(token)].insert(token);
    }
    return result;
}

std::vector<std::string> exact_asset_paths(bf6_ctx* context,
                                           const std::string& path)
{
    std::vector<std::string> result;
    const size_t slash = path.find_last_of('/');
    const std::string leaf = slash == std::string::npos
        ? path : path.substr(slash + 1);
    const int count = bf6_list_ebx(context, leaf.c_str(), nullptr, 0);
    if (count <= 0) return result;
    std::vector<bf6_asset> assets(static_cast<size_t>(count));
    const int got = bf6_list_ebx(context, leaf.c_str(), assets.data(), count);
    if (got <= 0 || got > count) return result;
    for (int i = 0; i < got; ++i)
        if (assets[static_cast<size_t>(i)].name == path)
            result.push_back(path);
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool activation_pin(uint32_t id)
{
    constexpr const char* names[] = {
        "Clicked", "ClickedWithPayload", "ClickedWithDirection",
        "OnPrimaryActivation", "OnSecondaryActivation",
        "OnTertiaryActivation", "OnQuaterneryActivation", "Activate",
        "OnActivate",
    };
    for (const char* name : names)
        if (action_id(name) == id) return true;
    return false;
}

} // namespace

const ControlActionIdentity* control_action_contracts(size_t* count)
{
    const auto& rows = actions();
    if (count) *count = rows.size();
    return rows.data();
}

const ControlActionIdentity* find_control_action(uint32_t id)
{
    const auto& rows = actions();
    const auto found = std::find_if(rows.begin(), rows.end(),
        [id](const ControlActionIdentity& row) { return row.id == id; });
    return found == rows.end() ? nullptr : &*found;
}

bool exact_control_activation_candidates(
    const AuthoredScreenDocument& document, const ElementAddress& control,
    std::vector<AuthoredEventEdge>& out, std::string& error)
{
    error.clear();
    out.clear();
    if (!document.contains(control)) {
        error = "activation source is not an exact element of the document";
        return false;
    }
    std::set<uint32_t> emitted;
    for (const AuthoredEventEdge& edge : document.event_edges()) {
        if (edge.partition == control.partition &&
            edge.occurrence == control.occurrence &&
            edge.source_instance == control.local_instance &&
            activation_pin(edge.source_event))
            emitted.insert(edge.source_event);
    }
    for (uint32_t event : emitted) {
        AuthoredEventBatch batch;
        std::string fanout_error;
        if (!exact_focused_activation(document, control, event, batch,
                                      fanout_error))
            continue;
        out.insert(out.end(), batch.edges.begin(), batch.edges.end());
    }
    if (out.empty()) {
        error = "control has no exact source-side activation fanout";
        return false;
    }
    return true;
}

bool InstallControlAudit::contracts_passed() const
{
    return errors.empty() && action_contracts == exact_exe_action_names &&
        ambiguous_exe_action_names == 0 && xor_one_action_hits == 0 &&
        shuffled_action_pairs == 0 &&
        dbd_contracts_expected == dbd_contracts_loaded &&
        dbd_data_name_mismatches == 0 &&
        dbd_fields_expected == dbd_fields_matched &&
        dbd_fields_missing == 0 && dbd_type_mismatches == 0 &&
        dbd_mutated_type_matches == 0 &&
        mapping_assets_expected == mapping_assets_found &&
        fake_mapping_asset_matches == 0;
}

InstallControlAudit audit_current_install_controls(
    bf6_ctx* context, const std::string& game_directory)
{
    InstallControlAudit report;
    report.action_contracts = static_cast<int>(actions().size());
    report.dbd_contracts_expected = static_cast<int>(std::size(kDbdSpecs));
    report.mapping_assets_expected = static_cast<int>(std::size(kMappingAssets));
    if (!context) {
        report.errors.push_back("null mounted context");
        return report;
    }
    if (game_directory.empty()) {
        report.errors.push_back("current install directory is empty");
        return report;
    }

    std::string exe_error;
    const auto identifiers = executable_identifiers(
        std::filesystem::path(game_directory) / "bf6.exe", exe_error);
    if (!exe_error.empty()) report.errors.push_back(exe_error);
    for (const ControlActionIdentity& action : actions()) {
        const auto exact = identifiers.find(action.id);
        if (exact != identifiers.end()) {
            if (exact->second.size() == 1 && *exact->second.begin() == action.name)
                ++report.exact_exe_action_names;
            else
                ++report.ambiguous_exe_action_names;
        }
        if (identifiers.find(action.id ^ 1u) != identifiers.end())
            ++report.xor_one_action_hits;
    }
    for (size_t i = 0; i < actions().size(); ++i)
        if (action_id(actions()[(i + 1) % actions().size()].name) ==
            actions()[i].id)
            ++report.shuffled_action_pairs;

    for (const DbdSpec& spec : kDbdSpecs) {
        report.dbd_fields_expected += static_cast<int>(spec.count);
        std::array<bf6_rime_dbd_field, 128> rows{};
        char data_name[128]{};
        const int got = bf6_rime_dbd_fields(
            context, spec.path, data_name, static_cast<int>(sizeof(data_name)),
            rows.data(), static_cast<int>(rows.size()));
        if (got <= 0 || got > static_cast<int>(rows.size())) {
            report.dbd_fields_missing += static_cast<int>(spec.count);
            report.errors.push_back(std::string("DBD unreadable: ") + spec.path);
            continue;
        }
        ++report.dbd_contracts_loaded;
        if (data_name != std::string(spec.data_name))
        {
            ++report.dbd_data_name_mismatches;
            report.errors.push_back(std::string("DBD data-name mismatch: ") +
                                    spec.path);
        }
        std::unordered_map<std::string, std::vector<uint64_t>> actual;
        for (int i = 0; i < got; ++i)
            actual[rows[static_cast<size_t>(i)].name].push_back(
                rows[static_cast<size_t>(i)].type_signature);
        for (size_t i = 0; i < spec.count; ++i) {
            const auto found = actual.find(spec.fields[i].name);
            if (found == actual.end() || found->second.size() != 1) {
                ++report.dbd_fields_missing;
                report.errors.push_back(std::string("DBD field missing/ambiguous: ") +
                                        spec.path + "::" + spec.fields[i].name);
                continue;
            }
            if (found->second.front() == spec.fields[i].type)
                ++report.dbd_fields_matched;
            else {
                ++report.dbd_type_mismatches;
                report.errors.push_back(std::string("DBD type mismatch: ") +
                                        spec.path + "::" + spec.fields[i].name);
            }
            if (found->second.front() == (spec.fields[i].type ^ 1ull))
                ++report.dbd_mutated_type_matches;
        }
    }

    for (const char* path : kMappingAssets) {
        if (exact_asset_paths(context, path).size() == 1)
            ++report.mapping_assets_found;
        report.fake_mapping_asset_matches += static_cast<int>(
            exact_asset_paths(context, std::string("__bf6_control__/") + path).size());
    }
    return report;
}

bool ControlRuntime::refresh_surface(std::string& error)
{
    error.clear();
    const ScreenFrame* frame = router_.current();
    if (!frame || !frame->document) {
        error = "no active route";
        return false;
    }
    const auto document = std::dynamic_pointer_cast<DirectInstallScreenDocument>(
        frame->document);
    if (!document) {
        error = "active route is not backed by a direct-install Rime document";
        return false;
    }
    const auto& elements = document->screen().elements;
    const auto& addresses = document->element_addresses();
    if (elements.size() != addresses.size()) {
        error = "screen element/address identity count mismatch";
        return false;
    }
    ControlSurface next;
    next.screen = document->identity().path;
    for (size_t i = 0; i < elements.size(); ++i) {
        const rime::Element& element = elements[i];
        if (element.kind != rime::Kind::InputBehavior) continue;
        InteractableControl control;
        control.address = addresses[i];
        control.type_name = element.type_name;
        control.has_authored_rect = element.solved &&
            element.x1 > element.x0 && element.y1 > element.y0;
        control.x0 = element.x0;
        control.y0 = element.y0;
        control.x1 = element.x1;
        control.y1 = element.y1;
        std::string candidate_error;
        exact_control_activation_candidates(
            *document, control.address, control.activation_candidates,
            candidate_error);
        next.activation_candidates +=
            static_cast<int>(control.activation_candidates.size());
        if (control.activation_candidates.size() > 1)
            ++next.ambiguous_activation_controls;
        next.interactables.push_back(std::move(control));
    }
    surface_ = std::move(next);
    return true;
}

bool ControlRuntime::apply_live_control_state(const LiveControlState& state,
                                              std::string& error)
{
    error.clear();
    const auto found = std::find_if(
        surface_.interactables.begin(), surface_.interactables.end(),
        [&state](const InteractableControl& row) {
            return row.address == state.address;
        });
    if (found == surface_.interactables.end()) {
        error = "live control state targets a non-existent occurrence";
        return false;
    }
    found->activation_mode = state.activation_mode;
    if (state.focusable) {
        found->focusable_resolved = true;
        found->focusable = *state.focusable;
    }
    if (state.mouse_enabled) {
        found->mouse_enabled_resolved = true;
        found->mouse_enabled = *state.mouse_enabled;
    }
    surface_.focusable_values_resolved = std::all_of(
        surface_.interactables.begin(), surface_.interactables.end(),
        [](const InteractableControl& row) { return row.focusable_resolved; });
    return true;
}

std::vector<size_t> ControlRuntime::hit_test(float x, float y) const
{
    std::vector<size_t> result;
    for (size_t i = 0; i < surface_.interactables.size(); ++i) {
        const InteractableControl& control = surface_.interactables[i];
        if (control.has_authored_rect && x >= control.x0 && x <= control.x1 &&
            y >= control.y0 && y <= control.y1)
            result.push_back(i);
    }
    return result;
}

ControlDispatch ControlRuntime::dispatch_input(
    const ControllerNeutralInput& input, std::string& error)
{
    const ControlActionIdentity* action = find_control_action(input.concept_id);
    if (!action) return dispatch_concept(input.concept_id, error);
    if (input.phase == InputPhase::Released) {
        error.clear();
        ControlDispatch result;
        result.kind = ControlDispatchKind::PhaseIgnored;
        result.concept_id = input.concept_id;
        result.detail = "release has no proven generic action dispatch law";
        return result;
    }
    if (input.phase == InputPhase::Repeat &&
        action->kind != ControlActionKind::Direction) {
        error.clear();
        ControlDispatch result;
        result.kind = ControlDispatchKind::PhaseIgnored;
        result.concept_id = input.concept_id;
        result.detail = "repeat is accepted only for proven directional actions";
        return result;
    }
    return dispatch_concept(input.concept_id, error);
}

ControlDispatch ControlRuntime::dispatch_concept(uint32_t concept_id,
                                                 std::string& error)
{
    error.clear();
    ControlDispatch result;
    result.concept_id = concept_id;
    const ControlActionIdentity* action = find_control_action(concept_id);
    if (!action) {
        error = "input concept is not in the current control contract";
        result.detail = error;
        return result;
    }
    switch (action->kind) {
    case ControlActionKind::Direction:
        if (router_.dispatch_action(concept_id)) {
            result.kind = ControlDispatchKind::DirectionDispatched;
            result.direction = direction_for_action(concept_id);
            return result;
        }
        error = "direction has no active route";
        break;
    case ControlActionKind::Back:
        if (router_.request_back(error)) {
            result.kind = ControlDispatchKind::BackDelegated;
            return result;
        }
        break;
    case ControlActionKind::TabLeft:
    case ControlActionKind::TabRight:
        if (router_.request_tab(
                action->kind == ControlActionKind::TabLeft ? -1 : 1, error)) {
            result.kind = ControlDispatchKind::TabDelegated;
            return result;
        }
        break;
    case ControlActionKind::Activate:
        error = "activation target/edge is unresolved; select an exact authored candidate";
        result.kind = ControlDispatchKind::ActivationUnresolved;
        result.detail = error;
        return result;
    case ControlActionKind::Generic:
    default:
        error = "generic concept handler is unresolved";
        result.kind = ControlDispatchKind::GenericUnresolved;
        result.detail = error;
        return result;
    }
    result.detail = error;
    return result;
}

ControlDispatch ControlRuntime::dispatch_navigation(
    NavigationIntent intent, InputPhase phase, std::string& error)
{
    if (phase != InputPhase::Pressed) {
        error.clear();
        ControlDispatch result;
        result.kind = ControlDispatchKind::PhaseIgnored;
        result.detail = "navigation abstraction dispatches on press only";
        return result;
    }
    switch (intent) {
    case NavigationIntent::Back:
        return dispatch_concept(action_id("ConceptMenuBack"), error);
    case NavigationIntent::TabLeft:
        return dispatch_concept(action_id("ConceptMenuTabLeft"), error);
    case NavigationIntent::TabRight:
        return dispatch_concept(action_id("ConceptMenuTabRight"), error);
    case NavigationIntent::Forward:
    default: {
        error = "forward destination law is unresolved in current evidence";
        ControlDispatch result;
        result.kind = ControlDispatchKind::ForwardUnresolved;
        result.detail = error;
        return result;
    }
    }
}

ControlDispatch ControlRuntime::dispatch_focus_target(
    uint32_t direction_concept, InputPhase phase,
    const ElementAddress& exact_target, std::string& error)
{
    ControlDispatch result;
    result.concept_id = direction_concept;
    const ControlActionIdentity* action = find_control_action(direction_concept);
    if (!action || action->kind != ControlActionKind::Direction) {
        error = "focus move requires an exact native direction concept";
        result.detail = error;
        return result;
    }
    if (phase == InputPhase::Released) {
        error.clear();
        result.kind = ControlDispatchKind::PhaseIgnored;
        result.detail = "direction release does not move focus";
        return result;
    }
    if (!router_.dispatch_action(direction_concept)) {
        error = "direction has no active route";
        result.detail = error;
        return result;
    }
    if (!router_.set_focus(exact_target, error)) {
        result.detail = error;
        return result;
    }
    result.kind = ControlDispatchKind::FocusTargetApplied;
    result.direction = direction_for_action(direction_concept);
    surface_.focus_owner_resolved = true;
    return result;
}

ControlDispatch ControlRuntime::dispatch_pointer(
    float x, float y, InputPhase phase, const ElementAddress* exact_control,
    const AuthoredEventEdge* exact_edge, std::string& error)
{
    error.clear();
    ControlDispatch result;
    const std::vector<size_t> hits = hit_test(x, y);
    if (hits.empty()) {
        error = "pointer does not hit an authored control rectangle";
        result.detail = error;
        return result;
    }
    if (!exact_control && hits.size() != 1) {
        error = "pointer hit is ambiguous; select an exact occurrence";
        result.kind = ControlDispatchKind::PointerAmbiguous;
        result.detail = error;
        return result;
    }
    const InteractableControl* control = nullptr;
    for (size_t index : hits) {
        const InteractableControl& candidate = surface_.interactables[index];
        if ((!exact_control && hits.size() == 1) ||
            (exact_control && candidate.address == *exact_control)) {
            control = &candidate;
            break;
        }
    }
    if (!control) {
        error = "selected pointer control is not in the exact hit set";
        result.detail = error;
        return result;
    }
    if (!control->mouse_enabled_resolved || !control->mouse_enabled) {
        error = control->mouse_enabled_resolved
            ? "mouse input is disabled for this control"
            : "mouse-enabled provider value is unresolved";
        result.detail = error;
        return result;
    }
    if (control->activation_mode == ActivationMode::Unresolved) {
        error = "control activation phase is unresolved";
        result.detail = error;
        return result;
    }
    const bool phase_matches =
        (control->activation_mode == ActivationMode::OnPress &&
         phase == InputPhase::Pressed) ||
        (control->activation_mode == ActivationMode::OnRelease &&
         phase == InputPhase::Released);
    if (!phase_matches) {
        error = control->activation_mode == ActivationMode::Hold
            ? "hold duration/progress scheduling is unresolved"
            : "pointer phase does not match the live activation mode";
        result.detail = error;
        return result;
    }
    if (!exact_edge) {
        error = "pointer activation requires an exact authored event edge";
        result.detail = error;
        return result;
    }
    return dispatch_activation(control->address, *exact_edge, error);
}

ControlDispatch ControlRuntime::dispatch_activation(
    const ElementAddress& control, const AuthoredEventEdge& edge,
    std::string& error)
{
    error.clear();
    ControlDispatch result;
    const auto found = std::find_if(
        surface_.interactables.begin(), surface_.interactables.end(),
        [&control](const InteractableControl& row) {
            return row.address == control;
        });
    if (found == surface_.interactables.end() ||
        std::find(found->activation_candidates.begin(),
                  found->activation_candidates.end(), edge) ==
            found->activation_candidates.end()) {
        error = "activation edge is not an exact candidate for this control";
        result.detail = error;
        return result;
    }
    const ScreenFrame* frame = router_.current();
    if (!frame || !frame->document) {
        error = "activation has no active authored document";
        result.detail = error;
        return result;
    }
    AuthoredEventBatch exact;
    if (!exact_focused_activation(*frame->document, control,
                                  edge.source_event, exact, error) ||
        std::find(exact.edges.begin(), exact.edges.end(), edge) ==
            exact.edges.end()) {
        if (error.empty())
            error = "activation edge is not exact source-side fanout";
        result.detail = error;
        return result;
    }
    if (!router_.dispatch_authored_event(edge, error)) {
        result.detail = error;
        return result;
    }
    result.kind = ControlDispatchKind::AuthoredActivationDispatched;
    return result;
}

bool ControlRuntime::bind_offline(PhysicalBinding binding,
                                  std::string& error)
{
    error.clear();
    if (binding.host_code.empty()) {
        error = "offline physical code is empty";
        return false;
    }
    if (!find_control_action(binding.concept_id)) {
        error = "offline binding targets an unknown input concept";
        return false;
    }
    BindingKey key{binding.device, std::move(binding.host_code)};
    const auto found = bindings_.find(key);
    if (found != bindings_.end()) {
        error = found->second == binding.concept_id
            ? "duplicate offline physical binding"
            : "conflicting offline physical binding";
        return false;
    }
    bindings_.emplace(std::move(key), binding.concept_id);
    return true;
}

ControlDispatch ControlRuntime::dispatch_physical(
    InputDevice device, std::string_view host_code, std::string& error)
{
    return dispatch_physical(device, host_code, InputPhase::Pressed, error);
}

ControlDispatch ControlRuntime::dispatch_physical(
    InputDevice device, std::string_view host_code, InputPhase phase,
    std::string& error)
{
    const auto found = bindings_.find({device, std::string(host_code)});
    if (found == bindings_.end()) {
        error = "physical input has no explicit offline binding";
        ControlDispatch result;
        result.detail = error;
        return result;
    }
    return dispatch_input({found->second, phase}, error);
}

} // namespace bf6_ui
