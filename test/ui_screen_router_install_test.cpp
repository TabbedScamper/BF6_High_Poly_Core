#include "install_screen_source.h"

#include <cstdio>
#include <memory>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr,
            "usage: ui_screen_router_install_test <Steam BF6 directory>\n");
        return 2;
    }

    std::string error;
    std::unique_ptr<bf6_ui::DirectInstallScreenSource> source =
        bf6_ui::DirectInstallScreenSource::open(
            argv[1], bf6_ui::DirectInstallScreenSource::MountMode::WholeInstall,
            error);
    if (!source)
    {
        std::fprintf(stderr, "open: %s\n", error.c_str());
        return 1;
    }

    bf6_ui::ScreenRouter router(*source);
    if (!router.refresh(error))
    {
        std::fprintf(stderr, "catalogue: %s\n", error.c_str());
        return 1;
    }
    const size_t exact_count = router.catalogue().size();
    size_t borrowed_count = 0;
    bool null_borrow_rejected = false;
    {
        std::string attach_error;
        null_borrow_rejected =
            !bf6_ui::DirectInstallScreenSource::attach_mounted(
                nullptr,
                bf6_ui::DirectInstallScreenSource::MountMode::WholeInstall,
                attach_error) && !attach_error.empty();
        auto borrowed = bf6_ui::DirectInstallScreenSource::attach_mounted(
            source->context(),
            bf6_ui::DirectInstallScreenSource::MountMode::WholeInstall,
            attach_error);
        std::vector<bf6_ui::ScreenIdentity> borrowed_catalogue;
        if (borrowed && borrowed->enumerate(borrowed_catalogue, attach_error))
            borrowed_count = borrowed_catalogue.size();
    }
    const bool fake_absent =
        router.find("common/ui/__control__/screens/not_a_real_screen") == nullptr;
    const bool fake_rejected = !router.push(
        "common/ui/__control__/screens/not_a_real_screen", error);

    const char* route = "common/ui/weapons/screens/menuweaponscreen";
    const bf6_ui::ScreenIdentity* identity = router.find(route);
    if (!identity || !router.push(route, error))
    {
        std::fprintf(stderr, "exact route: %s\n", error.c_str());
        return 1;
    }
    const auto document =
        std::dynamic_pointer_cast<bf6_ui::DirectInstallScreenDocument>(
            router.current()->document);
    if (!document)
    {
        std::fprintf(stderr, "exact route returned the wrong document type\n");
        return 1;
    }
    const bf6_ui::DirectScreenLoadReport& report = document->report();
    const bool exact_identity = document->identity() == *identity;
    const bool loaded = report.rows > 0 &&
        document->screen().partition == route;
    bool state_applied = false;
    rime_state::ApplyReport state_report;
    if (!document->element_addresses().empty())
    {
        const bf6_ui::StateKey alpha{
            document->element_addresses().front(), rime::property_hash("Alpha")};
        state_applied = router.set_state(alpha, 0.625, error) &&
            bf6_ui::apply_supported_state(
                *router.current(), state_report, error) &&
            state_report.applied_slots == 1;
    }

    std::printf(
        "screens=%zu expected=257 fake_path=%d fake_push=%d "
        "route=%s rows=%d events=%d unknown=%d unresolved_refs=%d "
        "identity=%d state=%d borrowed=%zu null_borrow=%d\n",
        exact_count, fake_absent ? 0 : 1, fake_rejected ? 0 : 1,
        document->screen().partition.c_str(), report.rows,
        report.authored_event_edges, report.tree.unknown_types,
        report.tree.unresolved_refs, exact_identity ? 1 : 0,
        state_applied ? 1 : 0, borrowed_count,
        null_borrow_rejected ? 1 : 0);

    return exact_count == 257 && fake_absent && fake_rejected &&
        exact_identity && loaded && state_applied && borrowed_count == exact_count &&
        null_borrow_rejected ? 0 : 1;
}
