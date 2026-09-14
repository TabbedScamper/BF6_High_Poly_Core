#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bf6_core.h"

namespace bf6_ui_provider {

/* This is the native boundary between a Rime screen and the 3D presenter.
 * A screen does not own a concrete menu model.  The selected item is supplied
 * by local runtime state and every asset named by that state is resolved again
 * against the current mount. */
enum class PresentationKind {
    None,
    Weapon,
    Vehicle,
    Character
};

enum class ValueSource {
    Unresolved,
    CurrentInstallCatalogue,
    LocalOfflineState
};

struct StringValue {
    std::string value;
    ValueSource source = ValueSource::Unresolved;
    /* Local state can name an opaque ID or an asset.  This flag distinguishes
     * an exact mounted asset from a merely explicit opaque value. */
    bool current_install_verified = false;

    bool resolved() const
    {
        return source != ValueSource::Unresolved && !value.empty();
    }
};

struct UInt64Value {
    uint64_t value = 0;
    ValueSource source = ValueSource::Unresolved;
    bool resolved = false;
};

struct AttachmentSelection {
    std::string slot;
    std::string attachment;
};

struct LocalSelection {
    /* Never infer presentation type or selected item from the screen path.
     * Both are host state in BF6. */
    std::string screen_partition;
    PresentationKind kind = PresentationKind::None;

    /* Weapon catalogue key.  weapon_class + weapon_token is the exact
     * directory identity used to disambiguate md_<weapon> and equipment_<weapon>.
     */
    std::string weapon_class;
    std::string weapon_token;
    std::string package_unlock_asset;
    std::vector<AttachmentSelection> attachments;

    /* Provider values.  Numeric fields correspond to the exact shipped DBD
     * names Item/Package/Equipment/Kit/VisualCharacterId.  A zero may be an
     * authored value, so each has a separate has_* bit. */
    std::string item_guid;
    uint64_t package_id = 0;
    uint64_t equipment_id = 0;
    uint64_t kit_id = 0;
    uint64_t visual_character_id = 0;
    bool has_package_id = false;
    bool has_equipment_id = false;
    bool has_kit_id = false;
    bool has_visual_character_id = false;
    std::string visual_slot_id;
    std::string variant_id;

    /* Exact asset paths may be supplied for vehicle/character paths whose
     * static selection law is not decoded.  They are accepted only if exactly
     * one mounted asset has the same full logical path. */
    std::string model_definition;
    std::string camera_asset;
    std::string camera_mode;
    std::string room_asset;
    std::string light_asset;
    std::string animation_asset;
};

struct ContractFieldSpec {
    const char* name;
    uint64_t type_signature;
};

struct ContractSpec {
    const char* partition;
    const char* data_name;
    const ContractFieldSpec* fields;
    size_t field_count;
};

/* Contract list compiled into the read path.  Names and opaque signatures are
 * checked against each newly mounted install; no generated evidence table is
 * read by the executable. */
const ContractSpec* contract_specs(size_t* count = nullptr);
const char* const* attachment_anchor_fields(size_t* count = nullptr);

struct ContractAudit {
    int contracts_expected = 0;
    int contracts_loaded = 0;
    int data_name_mismatches = 0;
    int fields_expected = 0;
    int fields_matched = 0;
    int fields_missing = 0;
    int type_mismatches = 0;
    int duplicate_fields = 0;
    /* Negative control: expected signature XOR 1 must not match. */
    int mutated_signature_matches = 0;

    bool passed() const
    {
        return contracts_expected == contracts_loaded &&
               data_name_mismatches == 0 &&
               fields_expected == fields_matched &&
               fields_missing == 0 && type_mismatches == 0 &&
               duplicate_fields == 0 && mutated_signature_matches == 0;
    }
};

ContractAudit audit_current_install_contracts(bf6_ctx* context);

struct MeshPart {
    std::string mesh;
    std::string bundle;
    float attach_transform[12]{};
    bool has_attach_transform = false;
    std::string gameplay_bone_partition;
    std::string gameplay_bone_instance;
    std::string gameplay_bone_path;
};

struct WeaponPresentation {
    StringValue model_definition;
    StringValue equipment_definition;
    StringValue package_unlock_asset;
    StringValue package_skin;
    std::vector<AttachmentSelection> fitted;
    std::vector<MeshPart> parts;
    std::vector<bf6_bone_xform> skin;

    bf6_armory_camera_mode camera{};
    bf6_armory_camera_sensor sensor{};
    bf6_armory_camera_weapon_correction camera_correction{};
    bool has_camera = false;
    bool has_sensor = false;
    bool has_camera_correction = false;
};

struct PresentationState {
    PresentationKind kind = PresentationKind::None;
    StringValue screen_partition;
    StringValue item_guid;
    UInt64Value package_id;
    UInt64Value equipment_id;
    UInt64Value kit_id;
    UInt64Value visual_character_id;
    StringValue visual_slot_id;
    StringValue variant_id;

    /* The concrete model selected by the provider.  For weapons this is
     * discovered from the exact class/token catalogue key.  For vehicles and
     * characters it remains unresolved unless local state supplies an exact
     * mounted path. */
    StringValue item_mesh_definition;
    StringValue weapon_item_mesh_definition;
    StringValue camera_asset;
    StringValue camera_mode;
    StringValue room_asset;
    StringValue light_asset;
    StringValue animation_asset;
    WeaponPresentation weapon;
};

struct ResolveReport {
    ContractAudit contracts;
    int exact_asset_queries = 0;
    int exact_asset_matches = 0;
    int ambiguous_asset_queries = 0;
    int missing_asset_queries = 0;
    int control_asset_queries = 0;
    int control_asset_matches = 0;
    int attachment_queries = 0;
    int attachment_matches = 0;
    int package_candidates = 0;
    int package_matches = 0;
    int mutated_package_matches = 0;
    int mesh_parts = 0;
    int skin_bones = 0;
    int unresolved_provider_values = 0;
    std::vector<std::string> errors;

    bool passed() const
    {
        return contracts.passed() && control_asset_matches == 0 &&
               mutated_package_matches == 0 && errors.empty();
    }
};

/* Resolve one local UI selection through the current mounted install.
 * Requires bf6_mount_frontend (or a broader mount) on `context`.
 *
 * The function is transactional: `out` is replaced only when the contract
 * gate passes and every explicitly supplied asset/package/attachment resolves
 * uniquely.  Missing provider values are counted and retained as Unresolved;
 * they are not errors and are never filled from adjacent paths. */
ResolveReport resolve(bf6_ctx* context, const LocalSelection& selection,
                      PresentationState& out);

} // namespace bf6_ui_provider
