#include "ui_provider_registry.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace bf6_ui_provider {
namespace {

constexpr uint64_t kBool = 0x0000000080000140ull;
constexpr uint64_t kInt = 0x00000000800001E0ull;
constexpr uint64_t kUInt64 = 0x0000000080000200ull;
constexpr uint64_t kReal = 0x0000000080000260ull;
constexpr uint64_t kCString = 0x0000000000000012ull;
constexpr uint64_t kVec3 = 0x000000000000001Eull;
constexpr uint64_t kLinearTransform = 0x000000000000000Eull;
constexpr uint64_t kEnum32 = 0x000000000000001Aull;
constexpr uint64_t kWeakPtr = 0x0000000000000026ull;
constexpr uint64_t kGuid = 0x00000000800002A0ull;

constexpr ContractFieldSpec kPresentable3dFields[] = {
    {"CharacterId", kUInt64},
    {"ItemMeshDefinition", kWeakPtr},
    {"ItemMeshDefinitionUnlocks", kCString},
    {"KitId", kUInt64},
    {"PositionOffset", kLinearTransform},
    {"Rarity", kEnum32},
    {"UnlockType", 0x0000000000000022ull},
    {"VehicleCameraFov", kReal},
    {"VehicleLookAtOffset", kVec3},
    {"VisualCharacterId", kUInt64},
    {"WeaponItemMeshDefinition", kWeakPtr},
    {"WeaponMeshDefinitionUnlocks", kCString},
};

constexpr ContractFieldSpec kPresentableIdentityFields[] = {
    {"ItemGuid", kGuid},
    {"PackageId", kUInt64},
    {"EquipmentId", kUInt64},
    {"KitId", kUInt64},
    {"VisualCharacterId", kUInt64},
    {"VisualSlotId", kCString},
};

constexpr ContractFieldSpec kAttachmentTransformFields[] = {
    {"AmmunitionAnchor", kEnum32},
    {"BarrelAnchor", kEnum32},
    {"DecalSlotOne", kLinearTransform},
    {"DecalSlotTwo", kLinearTransform},
    {"FlashLightAnchor", kEnum32},
    {"LaserAnchor", kEnum32},
    {"LauncherAnchor", kEnum32},
    {"LeftRailAnchor", kEnum32},
    {"MagazineAnchor", kEnum32},
    {"MuzzleAnchor", kEnum32},
    {"OpticAccessoryAnchor", kEnum32},
    {"RangefinderAnchor", kEnum32},
    {"SightAnchor", kEnum32},
    {"UnderbarrelAnchor", kEnum32},
    {"WeaponZeroAnchor", kEnum32},
};

constexpr const char* kAttachmentAnchorNames[] = {
    "AmmunitionAnchor", "BarrelAnchor", "DecalSlotOne", "DecalSlotTwo",
    "FlashLightAnchor", "LaserAnchor", "LauncherAnchor", "LeftRailAnchor",
    "MagazineAnchor", "MuzzleAnchor", "OpticAccessoryAnchor",
    "RangefinderAnchor", "SightAnchor", "UnderbarrelAnchor",
    "WeaponZeroAnchor",
};

constexpr ContractFieldSpec kVehicleTransforms[] = {
    {"LeftVehicleTransform", kLinearTransform},
    {"RightVehicleTransform", kLinearTransform},
};

constexpr ContractFieldSpec kSoldierCustomizationTransforms[] = {
    {"AssaultSoldierUIAnchor", kCString},
    {"EngineerSoldierUIAnchor", kCString},
    {"ReconSoldierUIAnchor", kCString},
    {"SupportSoldierUIAnchor", kCString},
};

constexpr ContractFieldSpec kSoldierLoadoutTransforms[] = {
    {"LeftSoldierUIWorldAnchor", kCString},
};

constexpr ContractFieldSpec kWeaponCameraFields[] = {
    {"IsWeaponCameraActive", kBool},
};

constexpr ContractFieldSpec kVehicleCameraFields[] = {
    {"IsCameraActive", kBool},
};

constexpr ContractFieldSpec kSoldierCameraFields[] = {
    {"IsHomeActive", kBool},
    {"IsLoadoutOverviewCameraActive", kBool},
    {"IsSoldierCustomizationCameraActive", kBool},
    {"IsSoldierLoadoutCameraActive", kBool},
    {"SoldierClassIdx", kInt},
};

// Exact provider boundary for each row in the class-select FooterList.  The
// text/icon signatures remain opaque Frostbite TypeRef values here; only the
// two boolean state fields are interpreted by the standalone presentation.
constexpr ContractFieldSpec kLoadoutClassItemFields[] = {
    {"Title", 0x0000000000000022ull},
    {"Description", 0x0000000000000022ull},
    {"Icon", 0x000000000000002Aull},
    {"IsDisabled", kBool},
    {"IsSelected", kBool},
    {"IsFocused", kBool},
};

constexpr ContractFieldSpec kLoadoutNavigationFields[] = {
    {"InputConceptNavigateLeft", kEnum32},
    {"InputConceptNavigateRight", kEnum32},
    {"CenterBackgroundDisabled", kBool},
    {"UISoundStyle_Primary", 0x0000000000000022ull},
};

constexpr ContractFieldSpec kLoadoutItemFields[] = {
    {"Title", 0x000000000000001Eull},
    {"Id", 0x0000000000000036ull},
    {"IsChanged", kBool},
    {"IsEquipped", kBool},
    {"IsDisabled", kBool},
    {"AllowedForCurrentCharacter", kBool},
    {"IsSelected", kBool},
    {"EquippedPackageId", 0x000000000000002Aull},
    {"PointCost", kInt},
    {"MaxPointCost", kInt},
    {"EquippedPackageName", 0x000000000000001Eull},
    {"IsLocked", kBool},
    {"IsModified", kBool},
    {"Description", 0x000000000000001Eull},
};

constexpr ContractFieldSpec kLoadoutCollectionFields[] = {
    {"EquippedLoadoutId", 0x0000000000000036ull},
    {"IsLoadoutVisible", kBool},
    {"HasAnyActiveNewMarkers", kBool},
};

constexpr ContractFieldSpec kLoadoutClassDetailsFields[] = {
    {"Icon", 0x0000000000000022ull},
    {"Title", 0x000000000000000Eull},
    {"Description", 0x000000000000000Eull},
    {"SimplifiedTitle", 0x000000000000000Eull},
    {"HardwareIcon", 0x0000000000000012ull},
    {"Image", 0x000000000000001Eull},
    {"ClassAbilityType", 0x0000000000000016ull},
};

constexpr ContractFieldSpec kLoadoutFieldUpgradeFields[] = {
    {"Title", 0x000000000000001Aull},
    {"Description", 0x000000000000001Aull},
    {"IsLocked", kBool},
    {"IsEquipped", kBool},
    {"Icon", 0x000000000000002Aull},
};

constexpr ContractFieldSpec kLoadoutCollectionAbilityFields[] = {
    {"UIAbilityCategory", 0x000000000000004Aull},
    {"AbilityName", 0x0000000000000022ull},
    {"AbilityDescription", 0x0000000000000022ull},
    {"AbilityImage", 0x0000000000000042ull},
    {"HardwareIconReference", 0x0000000000000026ull},
    {"HasHardwareIconReference", kBool},
    {"MeshDefinitionUnlocks", 0x000000000000001Aull},
    {"LoadoutPositionOffset", 0x0000000000000016ull},
    {"IsAbilityIdEmpty", kBool},
    {"AllowedForCurrentCharacter", kBool},
    {"PointCost", kInt},
    {"MaxPointCost", kInt},
    {"EquippedPackageName", 0x0000000000000022ull},
    {"EquippedPackageId", 0x0000000000000032ull},
    {"SlotId", 0x0000000000000036ull},
    {"IsWeapon", kBool},
    {"IsCustomizationLocked", kBool},
    {"HasVisualCustomization", kBool},
    {"HasProficiency", kBool},
    {"AbilitySimplifiedName", 0x0000000000000022ull},
    {"AbilityAbbreviatedName", 0x0000000000000022ull},
    {"TypeIcon", 0x0000000000000046ull},
    {"IsClassUnique", kBool},
};

constexpr ContractFieldSpec kVehicleArchetypeListItemFields[] = {
    {"ArchetypeId", 0x0000000000000026ull},
    {"SelectedState", kBool},
    {"Icon", 0x000000000000002Eull},
    {"Name", 0x0000000000000012ull},
    {"KitId", 0x000000000000001Eull},
    {"AbbreviatedName", 0x0000000000000012ull},
    {"VehicleSceneType", 0x0000000000000022ull},
};

constexpr ContractFieldSpec kVehicleArchetypeSelectedFields[] = {
    {"ArchetypeId", 0x000000000000002Eull},
    {"Name", 0x000000000000001Aull},
    {"Description", 0x000000000000001Aull},
    {"MeshDefinitionUnlocks", 0x0000000000000012ull},
    {"KitId", 0x0000000000000026ull},
    {"AbbreviatedName", 0x000000000000001Aull},
};

constexpr ContractFieldSpec kVehicleAbilityListFields[] = {
    {"Id", 0x000000000000001Eull},
    {"Name", 0x0000000000000016ull},
    {"IsSelected", kBool},
    {"IsLocked", kBool},
    {"IsEquipped", kBool},
};

constexpr ContractFieldSpec kVehicleLoadoutFields[] = {
    {"Name", 0x000000000000001Eull},
    {"Description", 0x000000000000001Eull},
    {"HasAbilities", kBool},
    {"IsSelected", kBool},
    {"Id", kInt},
    {"IsLocked", kBool},
    {"IsModified", kBool},
};

constexpr ContractFieldSpec kVehicleLoadoutEquipmentFields[] = {
    {"UIAbilityCategory", 0x0000000000000032ull},
    {"EquipmentName", 0x0000000000000016ull},
    {"EquipmentDescription", 0x0000000000000016ull},
    {"EquipmentImage", 0x000000000000002Aull},
    {"LoadoutPositionOffset", 0x0000000000000012ull},
    {"SlotId", 0x0000000000000022ull},
    {"EquipmentSimplifiedName", 0x0000000000000016ull},
    {"EquipmentAbbreviatedName", 0x0000000000000016ull},
    {"TypeIcon", 0x000000000000002Eull},
    {"HardwareIconReference", 0x000000000000001Aull},
    {"HasHardwareIconReference", kBool},
    {"IsDisabled", kBool},
};

constexpr ContractFieldSpec kSelectedVehicleAbilityFields[] = {
    {"Id", 0x0000000000000032ull},
    {"Name", 0x000000000000001Eull},
    {"Description", 0x000000000000001Eull},
    {"CollectionIcon", 0x0000000000000026ull},
    {"IsLocked", kBool},
    {"IsEquipped", kBool},
    {"MeshDefinitionUnlocks", 0x000000000000001Aull},
    {"HasArchetypes", kBool},
};

constexpr ContractFieldSpec kVehicleLoadoutSeatFields[] = {
    {"VehicleSeat", 0x000000000000001Aull},
    {"VehicleSeatName", 0x0000000000000016ull},
};

constexpr ContractSpec kContracts[] = {
    {"common/ui/metacore/3d/shared/databindings/3dpresentableitemdbd",
     "3DPresentableItemDBD", kPresentable3dFields,
     std::size(kPresentable3dFields)},
    {"common/ui/metacore/3d/shared/databindings/presentableitemdbd",
     "PresentableItem", kPresentableIdentityFields,
     std::size(kPresentableIdentityFields)},
    {"common/ui/metacore/3d/shared/databindings/weaponattachmenttransformdbd",
     "WeaponAttachmentTransformData", kAttachmentTransformFields,
     std::size(kAttachmentTransformFields)},
    {"common/ui/metacore/3d/shared/databindings/menuvehicletransformdbd",
     "MenuVehicleTransformDBD", kVehicleTransforms,
     std::size(kVehicleTransforms)},
    {"common/ui/metacore/3d/shared/databindings/soldiercustomizationtransformdbd",
     "SoldierCustomizationTransformDBD", kSoldierCustomizationTransforms,
     std::size(kSoldierCustomizationTransforms)},
    {"common/ui/metacore/3d/shared/databindings/soldierloadouttransformdbd",
     "SoldierLoadoutTransformDBD", kSoldierLoadoutTransforms,
     std::size(kSoldierLoadoutTransforms)},
    {"common/ui/metacore/camera/weapon/databinding/weapon_camerainterfacedbd",
     "Weapon_CameraInterfaceDBD", kWeaponCameraFields,
     std::size(kWeaponCameraFields)},
    {"common/ui/metacore/camera/vehicle/databinding/vehicle_camerainterfacedbd",
     "Vehicle_CameraInterfaceDBD", kVehicleCameraFields,
     std::size(kVehicleCameraFields)},
    {"common/ui/metacore/camera/soldier/databinding/soldier_camerainterfacedbd",
     "Soldier_CameraInterfaceDBD", kSoldierCameraFields,
     std::size(kSoldierCameraFields)},
    {"common/ui/loadout/shared/assets/databindings/loadoutclassitemdbd",
     "LoadoutClassItemData", kLoadoutClassItemFields,
     std::size(kLoadoutClassItemFields)},
    {"common/ui/universalmenu/assets/databindings/um_navigationdatadbd",
     "NavigationData", kLoadoutNavigationFields,
     std::size(kLoadoutNavigationFields)},
    {"common/ui/loadout/shared/assets/databindings/loadoutitemdbd",
     "LoadoutCollectionItemData", kLoadoutItemFields,
     std::size(kLoadoutItemFields)},
    {"common/ui/loadout/shared/assets/databindings/loadoutdbd",
     "LoadoutCollectionData", kLoadoutCollectionFields,
     std::size(kLoadoutCollectionFields)},
    {"common/ui/loadout/shared/assets/databindings/loadoutclassdetailsdbd",
     "LoadoutClassDetailsDBD", kLoadoutClassDetailsFields,
     std::size(kLoadoutClassDetailsFields)},
    {"common/ui/loadout/shared/assets/databindings/loadoutfieldupgradedbd",
     "LoadoutFieldUpgradeData", kLoadoutFieldUpgradeFields,
     std::size(kLoadoutFieldUpgradeFields)},
    {"common/ui/weaponcustomization/assets/databindings/"
     "loadoutcollectionabilitydbd",
     "LoadoutCollectionAbilityData", kLoadoutCollectionAbilityFields,
     std::size(kLoadoutCollectionAbilityFields)},
    {"common/ui/vehicles/shared/assets/databindings/"
     "vehiclearchetypelistitemdbd",
     "VehicleArchetypeListItemData", kVehicleArchetypeListItemFields,
     std::size(kVehicleArchetypeListItemFields)},
    {"common/ui/vehicles/shared/assets/databindings/"
     "vehiclearchetypeselectedvehicledbd",
     "VehicleArchetypeSelectedVehicleData", kVehicleArchetypeSelectedFields,
     std::size(kVehicleArchetypeSelectedFields)},
    {"common/ui/vehicles/shared/assets/databindings/vehicleabilitylistdbd",
     "VehicleAbilityListData", kVehicleAbilityListFields,
     std::size(kVehicleAbilityListFields)},
    {"common/ui/vehicles/shared/assets/databindings/vehicleloadoutdbd",
     "VehicleLoadoutData", kVehicleLoadoutFields,
     std::size(kVehicleLoadoutFields)},
    {"common/ui/weaponcustomization/assets/databindings/"
     "vehicleloadoutequipmentsdbd",
     "VehicleLoadoutEquipmentData", kVehicleLoadoutEquipmentFields,
     std::size(kVehicleLoadoutEquipmentFields)},
    {"common/ui/vehicles/shared/assets/databindings/selectedvehicleabilitydbd",
     "SelectedVehicleAbilityData", kSelectedVehicleAbilityFields,
     std::size(kSelectedVehicleAbilityFields)},
    {"common/ui/vehicles/shared/assets/databindings/vehicleloadoutseatdbd",
     "VehicleLoadoutSeatData", kVehicleLoadoutSeatFields,
     std::size(kVehicleLoadoutSeatFields)},
};

std::string canonical_path(const char* text)
{
    std::string value = text ? text : "";
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.size() >= 4 && value.compare(value.size() - 4, 4, ".ebx") == 0)
        value.resize(value.size() - 4);
    return value;
}

std::string leaf_of(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool safe_token(const std::string& token)
{
    if (token.empty()) return false;
    for (unsigned char c : token)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    return true;
}

std::vector<std::string> list_assets(bf6_ctx* context, const std::string& query)
{
    std::vector<std::string> result;
    if (!context || query.empty()) return result;
    const int count = bf6_list_ebx(context, query.c_str(), nullptr, 0);
    if (count <= 0) return result;
    std::vector<bf6_asset> rows(static_cast<size_t>(count));
    const int got = bf6_list_ebx(context, query.c_str(), rows.data(), count);
    if (got <= 0 || got > count) return result;
    result.reserve(static_cast<size_t>(got));
    for (int i = 0; i < got; ++i)
        if (rows[static_cast<size_t>(i)].name)
            result.push_back(canonical_path(rows[static_cast<size_t>(i)].name));
    return result;
}

std::vector<std::string> exact_paths(bf6_ctx* context,
                                     const std::string& full_path)
{
    const std::string wanted = canonical_path(full_path.c_str());
    std::vector<std::string> result;
    for (const std::string& candidate : list_assets(context, leaf_of(wanted)))
        if (candidate == wanted) result.push_back(candidate);
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<std::string> exact_leaf_paths(bf6_ctx* context,
                                          const std::string& leaf,
                                          const std::string& required_segment)
{
    std::vector<std::string> result;
    for (const std::string& candidate : list_assets(context, leaf))
        if (leaf_of(candidate) == leaf &&
            (required_segment.empty() ||
             candidate.find(required_segment) != std::string::npos))
            result.push_back(candidate);
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool accept_unique_asset(bf6_ctx* context, const std::string& path,
                         StringValue& destination, ResolveReport& report)
{
    if (path.empty()) return true;
    ++report.exact_asset_queries;
    const std::vector<std::string> matches = exact_paths(context, path);
    ++report.control_asset_queries;
    report.control_asset_matches += static_cast<int>(
        exact_paths(context, "__bf6_control__/" + canonical_path(path.c_str())).size());
    if (matches.empty()) {
        ++report.missing_asset_queries;
        report.errors.push_back("mounted asset not found exactly: " + path);
        return false;
    }
    if (matches.size() != 1) {
        ++report.ambiguous_asset_queries;
        report.errors.push_back("mounted asset is ambiguous: " + path);
        return false;
    }
    ++report.exact_asset_matches;
    destination.value = matches.front();
    destination.source = ValueSource::LocalOfflineState;
    destination.current_install_verified = true;
    return true;
}

bool discover_unique_leaf(bf6_ctx* context, const std::string& leaf,
                          const std::string& segment, StringValue& destination,
                          ResolveReport& report)
{
    ++report.exact_asset_queries;
    const std::vector<std::string> matches =
        exact_leaf_paths(context, leaf, segment);
    ++report.control_asset_queries;
    report.control_asset_matches += static_cast<int>(
        exact_leaf_paths(context, "__bf6_control_" + leaf, segment).size());
    if (matches.empty()) {
        ++report.missing_asset_queries;
        report.errors.push_back("current-install catalogue leaf missing: " + leaf);
        return false;
    }
    if (matches.size() != 1) {
        ++report.ambiguous_asset_queries;
        report.errors.push_back("current-install catalogue leaf ambiguous: " + leaf);
        return false;
    }
    ++report.exact_asset_matches;
    destination.value = matches.front();
    destination.source = ValueSource::CurrentInstallCatalogue;
    destination.current_install_verified = true;
    return true;
}

StringValue local_string(const std::string& value)
{
    StringValue result;
    if (!value.empty()) {
        result.value = value;
        result.source = ValueSource::LocalOfflineState;
    }
    return result;
}

UInt64Value local_uint64(uint64_t value, bool supplied)
{
    UInt64Value result;
    if (supplied) {
        result.value = value;
        result.source = ValueSource::LocalOfflineState;
        result.resolved = true;
    }
    return result;
}

std::string required_weapon_segment(const LocalSelection& selection)
{
    return "/" + selection.weapon_class + "/" +
           selection.weapon_token + "/";
}

bool validate_attachment(bf6_ctx* context, const LocalSelection& selection,
                         const AttachmentSelection& fit,
                         ResolveReport& report)
{
    ++report.attachment_queries;
    if (!safe_token(fit.slot) || !safe_token(fit.attachment)) {
        report.errors.push_back("invalid attachment key: " + fit.slot + "/" +
                                fit.attachment);
        return false;
    }
    const std::string leaf = "attachment_" + selection.weapon_token + "_" +
                             fit.slot + "_" + fit.attachment;
    const std::vector<std::string> matches = exact_leaf_paths(
        context, leaf, required_weapon_segment(selection));
    ++report.control_asset_queries;
    report.control_asset_matches += static_cast<int>(exact_leaf_paths(
        context, "__bf6_control_" + leaf,
        required_weapon_segment(selection)).size());
    if (matches.size() == 1) {
        ++report.attachment_matches;
        return true;
    }
    report.errors.push_back(matches.empty()
        ? "attachment is not in the current weapon catalogue: " + leaf
        : "attachment catalogue identity is ambiguous: " + leaf);
    return false;
}

void upsert_fit(std::vector<AttachmentSelection>& fits,
                const AttachmentSelection& incoming)
{
    for (AttachmentSelection& fit : fits)
        if (fit.slot == incoming.slot) {
            fit = incoming;
            return;
        }
    fits.push_back(incoming);
}

std::string mutate_package(const std::string& value)
{
    return "__bf6_control__" + value;
}

int count_unresolved(const PresentationState& state)
{
    int count = 0;
    count += state.item_guid.resolved() ? 0 : 1;
    count += state.package_id.resolved ? 0 : 1;
    count += state.equipment_id.resolved ? 0 : 1;
    count += state.kit_id.resolved ? 0 : 1;
    count += state.visual_character_id.resolved ? 0 : 1;
    count += state.visual_slot_id.resolved() ? 0 : 1;
    count += state.variant_id.resolved() ? 0 : 1;
    count += state.item_mesh_definition.resolved() ? 0 : 1;
    count += state.weapon_item_mesh_definition.resolved() ? 0 : 1;
    count += state.camera_asset.resolved() ? 0 : 1;
    count += state.camera_mode.resolved() ? 0 : 1;
    count += state.room_asset.resolved() ? 0 : 1;
    count += state.light_asset.resolved() ? 0 : 1;
    count += state.animation_asset.resolved() ? 0 : 1;
    return count;
}

} // namespace

const ContractSpec* contract_specs(size_t* count)
{
    if (count) *count = std::size(kContracts);
    return kContracts;
}

const char* const* attachment_anchor_fields(size_t* count)
{
    if (count) *count = std::size(kAttachmentAnchorNames);
    return kAttachmentAnchorNames;
}

ContractAudit audit_current_install_contracts(bf6_ctx* context)
{
    ContractAudit report;
    report.contracts_expected = static_cast<int>(std::size(kContracts));
    if (!context) {
        for (const ContractSpec& expected : kContracts) {
            report.fields_expected += static_cast<int>(expected.field_count);
            report.fields_missing += static_cast<int>(expected.field_count);
        }
        return report;
    }
    for (const ContractSpec& expected : kContracts) {
        std::array<bf6_rime_dbd_field, 256> rows{};
        char data_name[256]{};
        const int got = bf6_rime_dbd_fields(
            context, expected.partition, data_name,
            static_cast<int>(sizeof(data_name)), rows.data(),
            static_cast<int>(rows.size()));
        if (got <= 0 || got > static_cast<int>(rows.size())) {
            report.fields_expected += static_cast<int>(expected.field_count);
            report.fields_missing += static_cast<int>(expected.field_count);
            continue;
        }
        ++report.contracts_loaded;
        if (std::strcmp(data_name, expected.data_name) != 0)
            ++report.data_name_mismatches;

        std::unordered_map<std::string, std::vector<uint64_t>> actual;
        for (int i = 0; i < got; ++i)
            actual[rows[static_cast<size_t>(i)].name].push_back(
                rows[static_cast<size_t>(i)].type_signature);

        for (size_t i = 0; i < expected.field_count; ++i) {
            const ContractFieldSpec& field = expected.fields[i];
            ++report.fields_expected;
            const auto found = actual.find(field.name);
            if (found == actual.end()) {
                ++report.fields_missing;
                continue;
            }
            if (found->second.size() != 1) {
                ++report.duplicate_fields;
                continue;
            }
            if (found->second.front() == field.type_signature)
                ++report.fields_matched;
            else
                ++report.type_mismatches;
            if (found->second.front() == (field.type_signature ^ 1ull))
                ++report.mutated_signature_matches;
        }
    }
    return report;
}

ResolveReport resolve(bf6_ctx* context, const LocalSelection& selection,
                      PresentationState& out)
{
    ResolveReport report;
    if (!context) {
        report.errors.push_back("null mounted context");
        return report;
    }
    report.contracts = audit_current_install_contracts(context);
    if (!report.contracts.passed()) {
        report.errors.push_back("current-install UI presentation contracts failed");
        return report;
    }

    PresentationState working;
    working.kind = selection.kind;
    if (selection.kind == PresentationKind::None) {
        report.errors.push_back("presentation kind is unresolved");
        return report;
    }
    if (selection.screen_partition.empty()) {
        report.errors.push_back("screen partition is unresolved");
        return report;
    }
    if (!accept_unique_asset(context, selection.screen_partition,
                             working.screen_partition, report))
        return report;

    working.item_guid = local_string(selection.item_guid);
    working.package_id = local_uint64(selection.package_id,
                                      selection.has_package_id);
    working.equipment_id = local_uint64(selection.equipment_id,
                                        selection.has_equipment_id);
    working.kit_id = local_uint64(selection.kit_id, selection.has_kit_id);
    working.visual_character_id = local_uint64(
        selection.visual_character_id, selection.has_visual_character_id);
    working.visual_slot_id = local_string(selection.visual_slot_id);
    working.variant_id = local_string(selection.variant_id);

    if (!selection.model_definition.empty()) {
        if (!accept_unique_asset(context, selection.model_definition,
                                 working.item_mesh_definition, report))
            return report;
    }
    if (!accept_unique_asset(context, selection.camera_asset,
                             working.camera_asset, report) ||
        !accept_unique_asset(context, selection.room_asset,
                             working.room_asset, report) ||
        !accept_unique_asset(context, selection.light_asset,
                             working.light_asset, report) ||
        !accept_unique_asset(context, selection.animation_asset,
                             working.animation_asset, report))
        return report;

    if (selection.kind == PresentationKind::Weapon) {
        if (!safe_token(selection.weapon_class) ||
            !safe_token(selection.weapon_token)) {
            report.errors.push_back("weapon class/token is unresolved or invalid");
            return report;
        }
        const std::string segment = required_weapon_segment(selection);
        if (selection.model_definition.empty()) {
            if (!discover_unique_leaf(context, "md_" + selection.weapon_token,
                                      segment,
                                      working.weapon.model_definition, report))
                return report;
        } else {
            working.weapon.model_definition = working.item_mesh_definition;
        }
        working.weapon_item_mesh_definition = working.weapon.model_definition;

        if (!discover_unique_leaf(context,
                                  "equipment_" + selection.weapon_token,
                                  segment,
                                  working.weapon.equipment_definition, report))
            return report;

        working.weapon.fitted = selection.attachments;
        std::unordered_set<std::string> explicit_slots;
        for (const AttachmentSelection& fit : selection.attachments) {
            if (!explicit_slots.insert(fit.slot).second) {
                report.errors.push_back(
                    "multiple explicit attachments selected for slot: " + fit.slot);
                return report;
            }
            if (!validate_attachment(context, selection, fit, report))
                return report;
        }

        bf6_weapon_package_config selected_package{};
        bool has_package = false;
        if (!selection.package_unlock_asset.empty()) {
            const int total = bf6_weapon_package_configs(
                context, working.weapon.equipment_definition.value.c_str(),
                nullptr, 0);
            report.package_candidates = total > 0 ? total : 0;
            if (total <= 0) {
                report.errors.push_back("weapon package catalogue unavailable");
                return report;
            }
            std::vector<bf6_weapon_package_config> packages(
                static_cast<size_t>(total));
            const int got = bf6_weapon_package_configs(
                context, working.weapon.equipment_definition.value.c_str(),
                packages.data(), total);
            if (got != total) {
                report.errors.push_back("weapon package catalogue changed during read");
                return report;
            }
            for (const bf6_weapon_package_config& candidate : packages) {
                if (selection.package_unlock_asset == candidate.unlock_asset) {
                    selected_package = candidate;
                    ++report.package_matches;
                }
                if (mutate_package(selection.package_unlock_asset) ==
                    candidate.unlock_asset)
                    ++report.mutated_package_matches;
            }
            if (report.package_matches != 1) {
                report.errors.push_back(report.package_matches == 0
                    ? "selected package is absent from current equipment data"
                    : "selected package identity is ambiguous");
                return report;
            }
            if (report.mutated_package_matches != 0) {
                report.errors.push_back("mutated package control resolved");
                return report;
            }
            has_package = true;
            working.weapon.package_unlock_asset.value =
                selected_package.unlock_asset;
            working.weapon.package_unlock_asset.source =
                ValueSource::CurrentInstallCatalogue;
            working.weapon.package_unlock_asset.current_install_verified = true;
            if (selected_package.skin[0]) {
                working.weapon.package_skin.value = selected_package.skin;
                working.weapon.package_skin.source =
                    ValueSource::CurrentInstallCatalogue;
                working.weapon.package_skin.current_install_verified = true;
            }
            std::vector<AttachmentSelection> merged;
            for (int i = 0; i < selected_package.fit_count; ++i)
                merged.push_back({selected_package.fits[i].slot,
                                  selected_package.fits[i].attachment});
            for (const AttachmentSelection& fit : selection.attachments)
                upsert_fit(merged, fit);
            working.weapon.fitted = std::move(merged);
        }

        std::vector<bf6_weapon_fit> fit_rows;
        fit_rows.reserve(working.weapon.fitted.size());
        for (const AttachmentSelection& fit : working.weapon.fitted)
            fit_rows.push_back({fit.slot.c_str(), fit.attachment.c_str()});

        std::vector<bf6_weapon_part_pose> parts(512);
        std::vector<bf6_bone_xform> skin(512);
        int bone_count = 0;
        const int part_count = has_package ||
                !selection.package_unlock_asset.empty()
            ? bf6_weapon_package_assembly(
                context, working.weapon.model_definition.value.c_str(),
                fit_rows.empty() ? nullptr : fit_rows.data(),
                static_cast<int>(fit_rows.size()),
                working.weapon.package_skin.value.c_str(), parts.data(),
                static_cast<int>(parts.size()), skin.data(),
                static_cast<int>(skin.size()), &bone_count)
            : bf6_weapon_configured_assembly(
                context, working.weapon.model_definition.value.c_str(),
                fit_rows.empty() ? nullptr : fit_rows.data(),
                static_cast<int>(fit_rows.size()), parts.data(),
                static_cast<int>(parts.size()), skin.data(),
                static_cast<int>(skin.size()), &bone_count);
        if (part_count <= 0 || part_count > static_cast<int>(parts.size()) ||
            bone_count < 0 || bone_count > static_cast<int>(skin.size())) {
            report.errors.push_back("current-install weapon assembly failed or exceeded bounds");
            return report;
        }
        working.weapon.parts.reserve(static_cast<size_t>(part_count));
        for (int i = 0; i < part_count; ++i) {
            const bf6_weapon_part_pose& source = parts[static_cast<size_t>(i)];
            if (!source.mesh || !source.bundle) {
                report.errors.push_back("weapon assembly returned an incomplete part");
                return report;
            }
            MeshPart copy;
            copy.mesh = source.mesh;
            copy.bundle = source.bundle;
            std::copy(std::begin(source.attach_transform),
                      std::end(source.attach_transform),
                      std::begin(copy.attach_transform));
            copy.has_attach_transform = source.has_attach_transform != 0;
            copy.gameplay_bone_partition = source.gameplay_bone_partition;
            copy.gameplay_bone_instance = source.gameplay_bone_instance;
            copy.gameplay_bone_path = source.gameplay_bone_path;
            working.weapon.parts.push_back(std::move(copy));
        }
        working.weapon.skin.assign(skin.begin(), skin.begin() + bone_count);
        report.mesh_parts = part_count;
        report.skin_bones = bone_count;

        if (!selection.camera_mode.empty()) {
            if (!safe_token(selection.camera_mode) ||
                !bf6_armory_camera_mode_read(context,
                    selection.camera_mode.c_str(), &working.weapon.camera)) {
                report.errors.push_back("weapon camera mode is not exact current-install data");
                return report;
            }
            working.weapon.has_camera = true;
            working.camera_mode.value = selection.camera_mode;
            working.camera_mode.source = ValueSource::CurrentInstallCatalogue;
            working.camera_mode.current_install_verified = true;
            working.weapon.has_sensor =
                bf6_armory_camera_sensor_read(context,
                                              &working.weapon.sensor) != 0;
            if (!working.weapon.has_sensor) {
                report.errors.push_back("weapon camera sensor is unresolved");
                return report;
            }
            working.weapon.has_camera_correction =
                bf6_armory_camera_weapon_correction_read(
                    context, selection.weapon_token.c_str(),
                    &working.weapon.camera_correction) != 0;
        }
    }

    report.unresolved_provider_values = count_unresolved(working);
    out = std::move(working);
    return report;
}

} // namespace bf6_ui_provider
