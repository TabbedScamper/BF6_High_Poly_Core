#include "default_input_bindings.h"

#include "ebx.h"
#include "source.h"
#include "types.h"

#include <algorithm>
#include <set>
#include <utility>

namespace bf6_ui {
namespace {

using bf6::Ebx;
using bf6::EbxValue;
using bf6::Source;
using bf6::TypeDb;

constexpr const char* kPartition =
    "common/gameplay/input/inputconcepts_default";
constexpr uint32_t kInputActions = 0x0582CD7Cu;
constexpr uint32_t kConcept = 0x442FEE8Au;
constexpr uint32_t kCopyConcept = 0xC44D6ED8u;
constexpr uint32_t kKey = 0x7C7E22C1u;
constexpr uint32_t kButton = 0x412198DBu;
constexpr uint32_t kAlternativeButton = 0x2B1682B7u;
constexpr uint32_t kAxis = 0x876C7F17u;

constexpr const char* kKeyboard =
    "8383474f-ffac-8c16-cb2c-972bfceff132";
constexpr const char* kMouse =
    "5d71ce1d-79e0-b9e1-b284-1b1c0a1649de";
constexpr const char* kPad =
    "94afd819-f4df-71f5-cd67-2558291c8988";
constexpr const char* kMotionController =
    "6c7aa9ac-a930-c062-309d-ae68ca2db81b";

bool numeric(const EbxValue* value, uint32_t& out)
{
    if (!value) return false;
    switch (value->kind) {
    case EbxValue::Kind::Int:
        out = static_cast<uint32_t>(value->i);
        return true;
    case EbxValue::Kind::Uint:
    case EbxValue::Kind::Unknown:
        out = static_cast<uint32_t>(value->u);
        return true;
    default:
        return false;
    }
}

void optional_numeric(const EbxValue& row, uint32_t field,
                      std::optional<uint32_t>& out)
{
    uint32_t value = 0;
    if (numeric(row.field(field), value)) out = value;
}

InstalledInputDevice device_for(const std::string& guid)
{
    if (guid == kKeyboard) return InstalledInputDevice::Keyboard;
    if (guid == kMouse) return InstalledInputDevice::Mouse;
    if (guid == kPad) return InstalledInputDevice::Pad;
    if (guid == kMotionController) return InstalledInputDevice::MotionController;
    return InstalledInputDevice::Unknown;
}

bool open_readable_types(const std::string& game_directory, TypeDb& types,
                         std::string& error)
{
    for (const std::string& candidate :
         TypeDb::exe_candidates(game_directory)) {
        if (types.open(candidate, error) && !types.looks_encrypted()) return true;
    }
    error = "no readable current-install type database: " + error;
    return false;
}

} // namespace

bool load_installed_default_bindings(
    const std::string& game_directory,
    const uint32_t* concept_ids,
    size_t concept_count,
    std::vector<InstalledPhysicalAction>& actions,
    InstalledDefaultBindingAudit& audit,
    std::string& error)
{
    actions.clear();
    audit = InstalledDefaultBindingAudit{};
    audit.source_partition = kPartition;
    audit.requested_concepts = static_cast<int>(concept_count);
    error.clear();
    if (!concept_ids || concept_count == 0) {
        error = "at least one exact concept id is required";
        return false;
    }

    const std::set<uint32_t> requested(concept_ids,
                                       concept_ids + concept_count);
    if (requested.size() != concept_count) {
        error = "duplicate requested concept id";
        return false;
    }

    Source source;
    if (!source.open(game_directory, error) || !source.mount_frontend(error)) {
        error = "current-install mount failed: " + error;
        return false;
    }
    audit.fake_asset_matches =
        source.ebx().count(std::string(kPartition) + "_mutated_control") ? 1 : 0;

    TypeDb types;
    if (!open_readable_types(game_directory, types, error)) return false;
    std::vector<uint8_t> bytes = source.get_ebx(kPartition, error);
    if (bytes.empty()) {
        error = "installed default input-concept partition is absent: " + error;
        return false;
    }
    Ebx ebx(types);
    if (!ebx.parse(std::move(bytes), error)) {
        error = "installed default input-concept partition did not parse: " + error;
        return false;
    }
    audit.partition_instances = static_cast<int>(ebx.instance_count());

    std::set<uint32_t> matched;
    const std::vector<uint32_t> concept_fields = {
        kInputActions, kConcept, kCopyConcept};
    const std::vector<uint32_t> action_fields = {
        kKey, kButton, kAlternativeButton, kAxis};
    for (size_t i = 0; i < ebx.instance_count(); ++i) {
        const EbxValue row = ebx.read_instance(i, &concept_fields);
        uint32_t concept = 0;
        if (!numeric(row.field(kConcept), concept)) continue;
        if (requested.count(concept ^ 1u)) ++audit.xor_one_concept_rows;
        if (!requested.count(concept)) continue;
        if (!matched.insert(concept).second) {
            ++audit.duplicate_concept_rows;
            continue;
        }
        ++audit.matched_concepts;

        uint32_t copy = 0;
        numeric(row.field(kCopyConcept), copy);
        const EbxValue* references = row.field(kInputActions);
        if (!references || references->kind != EbxValue::Kind::Array) {
            error = "matched concept row has no reflected InputActions array";
            return false;
        }
        for (const EbxValue& reference : references->items) {
            ++audit.pointer_rows;
            if (reference.kind != EbxValue::Kind::InstanceRef ||
                reference.instance < 0 ||
                static_cast<size_t>(reference.instance) >= ebx.instance_count()) {
                ++audit.invalid_pointer_rows;
                continue;
            }
            const size_t target = static_cast<size_t>(reference.instance);
            InstalledPhysicalAction action;
            action.concept_id = concept;
            action.copy_key_binding_from = copy;
            action.source_instance = reference.instance;
            action.type_guid = TypeDb::guid_str(ebx.instance_type(target));
            action.device = device_for(action.type_guid);
            const EbxValue physical = ebx.read_instance(target, &action_fields);
            optional_numeric(physical, kKey, action.key);
            optional_numeric(physical, kButton, action.button);
            optional_numeric(physical, kAlternativeButton,
                             action.alternative_button);
            optional_numeric(physical, kAxis, action.axis);
            if (action.device == InstalledInputDevice::Unknown)
                ++audit.unknown_action_rows;
            else
                ++audit.recognized_action_rows;
            actions.push_back(std::move(action));
        }
    }

    if (!audit.passed()) {
        error = "installed default-binding controls failed";
        return false;
    }
    return true;
}

} // namespace bf6_ui
