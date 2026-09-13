#include "ui_control_runtime.h"

#include <cstdio>
#include <memory>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr,
            "usage: ui_control_runtime_install_test <Steam BF6 directory>\n");
        return 2;
    }
    std::string error;
    auto source = bf6_ui::DirectInstallScreenSource::open(
        argv[1], bf6_ui::DirectInstallScreenSource::MountMode::WholeInstall,
        error);
    if (!source) {
        std::fprintf(stderr, "open: %s\n", error.c_str());
        return 1;
    }
    const bf6_ui::InstallControlAudit audit =
        bf6_ui::audit_current_install_controls(source->context(), argv[1]);
    bf6_ui::ScreenRouter router(*source);
    if (!router.refresh(error)) {
        std::fprintf(stderr, "catalogue: %s\n", error.c_str());
        return 1;
    }
    bf6_ui::ControlRuntime controls(router);
    int loaded = 0;
    int surfaces = 0;
    int interactables = 0;
    int activation_candidates = 0;
    for (const bf6_ui::ScreenIdentity& screen : router.catalogue()) {
        const bool routed = loaded == 0
            ? router.push(screen.path, error)
            : router.replace(screen.path, error);
        if (!routed) continue;
        ++loaded;
        if (!controls.refresh_surface(error)) continue;
        ++surfaces;
        interactables += static_cast<int>(
            controls.surface().interactables.size());
        activation_candidates += controls.surface().activation_candidates;
    }
    const size_t before_fake = router.stack().size();
    const bool fake_route_rejected = !router.replace(
        "common/ui/__control__/screens/fabricated", error) &&
        router.stack().size() == before_fake;

    std::printf(
        "screens=%zu loaded=%d load_unresolved=%zu surfaces=%d interactables=%d "
        "activation_candidates=%d actions=%d/%d ambiguous_actions=%d "
        "xor_actions=%d shuffled_actions=%d dbds=%d/%d fields=%d/%d "
        "mutated_types=%d mappings=%d/%d fake_mappings=%d "
        "physical_values=%d fake_route=%d\n",
        router.catalogue().size(), loaded,
        router.catalogue().size() - static_cast<size_t>(loaded),
        surfaces, interactables,
        activation_candidates, audit.exact_exe_action_names,
        audit.action_contracts, audit.ambiguous_exe_action_names,
        audit.xor_one_action_hits, audit.shuffled_action_pairs,
        audit.dbd_contracts_loaded, audit.dbd_contracts_expected,
        audit.dbd_fields_matched, audit.dbd_fields_expected,
        audit.dbd_mutated_type_matches, audit.mapping_assets_found,
        audit.mapping_assets_expected, audit.fake_mapping_asset_matches,
        audit.physical_binding_values, fake_route_rejected ? 0 : 1);

    if (!audit.contracts_passed()) {
        for (const std::string& row : audit.errors)
            std::fprintf(stderr, "audit: %s\n", row.c_str());
    }
    return audit.contracts_passed() && !audit.physical_mapping_resolved() &&
        router.catalogue().size() == 257 && loaded > 0 && surfaces == loaded &&
        fake_route_rejected ? 0 : 1;
}
