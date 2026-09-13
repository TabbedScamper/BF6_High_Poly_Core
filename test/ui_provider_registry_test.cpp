#include "bf6_core.h"
#include "ui_provider_registry.h"

#include <cstdio>
#include <string>

namespace {

int progress(void*, const char* stage, int done, int total)
{
    if (done == 0 || done == total)
        std::fprintf(stderr, "progress %s %d/%d\n", stage ? stage : "?",
                     done, total);
    return 1;
}

bool contains_error(const bf6_ui_provider::ResolveReport& report,
                    const char* text)
{
    for (const std::string& error : report.errors)
        if (error.find(text) != std::string::npos) return true;
    return false;
}

void print_errors(const char* label,
                  const bf6_ui_provider::ResolveReport& report)
{
    for (const std::string& error : report.errors)
        std::fprintf(stderr, "%s: %s\n", label, error.c_str());
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: ui_provider_registry_test <game-dir>\n");
        return 2;
    }

    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error,
                                static_cast<int>(sizeof(error)));
    if (!context) {
        std::fprintf(stderr, "open: %s\n", error);
        return 1;
    }
    bf6_set_progress(context, &progress, nullptr);
    if (!bf6_mount_frontend(context, error,
                            static_cast<int>(sizeof(error)))) {
        std::fprintf(stderr, "mount: %s\n", error);
        bf6_close(context);
        return 1;
    }

    using namespace bf6_ui_provider;

    size_t spec_count = 0;
    const ContractSpec* specs = contract_specs(&spec_count);
    size_t anchor_count = 0;
    const char* const* anchors = attachment_anchor_fields(&anchor_count);
    const ContractAudit audit = audit_current_install_contracts(context);
    const bool contract_ok = specs && spec_count == 23 && anchors &&
        anchor_count == 15 && audit.contracts_expected == 23 &&
        audit.fields_expected == 156 && audit.passed();

    LocalSelection weapon;
    weapon.screen_partition = "common/ui/weapons/screens/menuweaponscreen";
    weapon.kind = PresentationKind::Weapon;
    weapon.weapon_class = "carbine";
    weapon.weapon_token = "m4a1";
    weapon.attachments.push_back({"scp", "xps3"});
    weapon.camera_mode = "weaponbehavior";
    PresentationState weapon_state;
    const ResolveReport weapon_report = resolve(context, weapon, weapon_state);
    const bool weapon_ok = weapon_report.passed() &&
        weapon_report.control_asset_matches == 0 &&
        weapon_report.mutated_package_matches == 0 &&
        weapon_state.screen_partition.current_install_verified &&
        weapon_state.weapon.model_definition.current_install_verified &&
        weapon_state.weapon.equipment_definition.current_install_verified &&
        weapon_state.weapon_item_mesh_definition.resolved() &&
        !weapon_state.item_mesh_definition.resolved() &&
        !weapon_state.item_guid.resolved() &&
        weapon_state.weapon.has_camera && weapon_state.weapon.has_sensor &&
        weapon_state.camera_mode.resolved() &&
        !weapon_state.camera_asset.resolved() &&
        !weapon_state.room_asset.resolved() &&
        !weapon_state.light_asset.resolved() &&
        !weapon_state.animation_asset.resolved() &&
        weapon_report.attachment_queries == 1 &&
        weapon_report.attachment_matches == 1 &&
        !weapon_state.weapon.parts.empty() && !weapon_state.weapon.skin.empty();

    /* Package identity is selected from the current mounted equipment row,
     * then sent back through the presenter.  This exercises the exact unlock
     * key and the prefixed-key negative control without baking a package key
     * into the test. */
    const int package_count = weapon_ok
        ? bf6_weapon_package_configs(
            context, weapon_state.weapon.equipment_definition.value.c_str(),
            nullptr, 0)
        : 0;
    std::vector<bf6_weapon_package_config> package_rows(
        package_count > 0 ? static_cast<size_t>(package_count) : 0);
    const int package_got = package_count > 0
        ? bf6_weapon_package_configs(
            context, weapon_state.weapon.equipment_definition.value.c_str(),
            package_rows.data(), package_count)
        : package_count;
    LocalSelection package = weapon;
    package.attachments.clear();
    if (package_got == package_count && !package_rows.empty())
        package.package_unlock_asset = package_rows.front().unlock_asset;
    PresentationState package_state;
    const ResolveReport package_report = resolve(context, package, package_state);
    const bool package_ok = package_count > 0 && package_got == package_count &&
        package_report.passed() && package_report.package_candidates == package_count &&
        package_report.package_matches == 1 &&
        package_report.mutated_package_matches == 0 &&
        package_state.weapon.package_unlock_asset.current_install_verified &&
        !package_state.weapon.parts.empty();

    /* The kind is live state.  The same screen path must not cause the
     * resolver to invent a character model, camera, light, room, or animation. */
    LocalSelection character;
    character.screen_partition = weapon.screen_partition;
    character.kind = PresentationKind::Character;
    character.has_visual_character_id = true;
    character.visual_character_id = 0x1234;
    character.visual_slot_id = "LocalTestSlot";
    PresentationState character_state;
    const ResolveReport character_report =
        resolve(context, character, character_state);
    const bool boundary_ok = character_report.passed() &&
        character_state.visual_character_id.resolved &&
        character_state.visual_character_id.source ==
            ValueSource::LocalOfflineState &&
        character_state.visual_slot_id.source == ValueSource::LocalOfflineState &&
        !character_state.item_mesh_definition.resolved() &&
        !character_state.weapon_item_mesh_definition.resolved() &&
        !character_state.camera_asset.resolved() &&
        character_state.weapon.parts.empty();

    /* Exact-path and catalogue controls.  Output must remain untouched after
     * a failed transaction. */
    PresentationState sentinel;
    sentinel.item_guid.value = "preserve-on-failure";
    sentinel.item_guid.source = ValueSource::LocalOfflineState;
    LocalSelection fake_screen = weapon;
    fake_screen.screen_partition = "common/ui/__control__/not_a_screen";
    const ResolveReport fake_screen_report =
        resolve(context, fake_screen, sentinel);
    const bool fake_screen_ok = !fake_screen_report.passed() &&
        fake_screen_report.exact_asset_matches == 0 &&
        contains_error(fake_screen_report, "not found exactly") &&
        sentinel.item_guid.value == "preserve-on-failure";

    LocalSelection fake_weapon = weapon;
    fake_weapon.weapon_token = "__fabricated_weapon";
    PresentationState fake_weapon_state;
    const ResolveReport fake_weapon_report =
        resolve(context, fake_weapon, fake_weapon_state);
    const bool fake_weapon_ok = !fake_weapon_report.passed() &&
        contains_error(fake_weapon_report, "catalogue leaf missing") &&
        fake_weapon_state.kind == PresentationKind::None;

    LocalSelection fake_camera = weapon;
    fake_camera.camera_mode = "__fabricated_camera";
    PresentationState fake_camera_state;
    const ResolveReport fake_camera_report =
        resolve(context, fake_camera, fake_camera_state);
    const bool fake_camera_ok = !fake_camera_report.passed() &&
        contains_error(fake_camera_report, "camera mode") &&
        fake_camera_state.kind == PresentationKind::None;

    LocalSelection fake_attachment = weapon;
    fake_attachment.attachments = {{"scp", "__fabricated_attachment"}};
    PresentationState fake_attachment_state;
    const ResolveReport fake_attachment_report =
        resolve(context, fake_attachment, fake_attachment_state);
    const bool fake_attachment_ok = !fake_attachment_report.passed() &&
        contains_error(fake_attachment_report, "current weapon catalogue") &&
        fake_attachment_state.kind == PresentationKind::None;

    LocalSelection fake_animation = character;
    fake_animation.animation_asset =
        "common/ui/__control__/not_an_animation_asset";
    PresentationState fake_animation_state;
    const ResolveReport fake_animation_report =
        resolve(context, fake_animation, fake_animation_state);
    const bool fake_animation_ok = !fake_animation_report.passed() &&
        contains_error(fake_animation_report, "not found exactly") &&
        fake_animation_state.kind == PresentationKind::None;

    LocalSelection fake_light = character;
    fake_light.light_asset = "common/ui/__control__/not_a_light_asset";
    PresentationState fake_light_state;
    const ResolveReport fake_light_report =
        resolve(context, fake_light, fake_light_state);
    const bool fake_light_ok = !fake_light_report.passed() &&
        contains_error(fake_light_report, "not found exactly") &&
        fake_light_state.kind == PresentationKind::None;

    std::printf(
        "ui-provider contracts=%d/%d fields=%d/%d type=%d mutated-type=%d "
        "anchors=%zu weapon-parts=%d skin=%d unresolved=%d controls=%d "
        "package=%d boundary=%d fake-screen=%d fake-weapon=%d "
        "fake-camera=%d fake-attachment=%d fake-animation=%d fake-light=%d\n",
        audit.contracts_loaded, audit.contracts_expected,
        audit.fields_matched, audit.fields_expected, audit.type_mismatches,
        audit.mutated_signature_matches, anchor_count,
        weapon_report.mesh_parts, weapon_report.skin_bones,
        weapon_report.unresolved_provider_values,
        weapon_report.control_asset_matches, package_ok ? 1 : 0,
        boundary_ok ? 1 : 0,
        fake_screen_ok ? 1 : 0, fake_weapon_ok ? 1 : 0,
        fake_camera_ok ? 1 : 0, fake_attachment_ok ? 1 : 0,
        fake_animation_ok ? 1 : 0, fake_light_ok ? 1 : 0);

    bf6_close(context);
    const bool pass = contract_ok && weapon_ok && package_ok && boundary_ok &&
        fake_screen_ok && fake_weapon_ok && fake_camera_ok &&
        fake_attachment_ok && fake_animation_ok && fake_light_ok;
    if (!weapon_ok) print_errors("weapon", weapon_report);
    if (!package_ok) print_errors("package", package_report);
    if (!boundary_ok) print_errors("boundary", character_report);
    if (!fake_screen_ok) print_errors("fake-screen", fake_screen_report);
    if (!fake_weapon_ok) print_errors("fake-weapon", fake_weapon_report);
    if (!fake_camera_ok) print_errors("fake-camera", fake_camera_report);
    if (!fake_attachment_ok)
        print_errors("fake-attachment", fake_attachment_report);
    if (!fake_animation_ok) print_errors("fake-animation", fake_animation_report);
    if (!fake_light_ok) print_errors("fake-light", fake_light_report);
    std::printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
