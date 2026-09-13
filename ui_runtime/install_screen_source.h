#pragma once

#include "screen_router.h"

#include "bf6_core.h"
#include "rime.h"
#include "rime_runtime.h"
#include "rime_state_bridge.h"

#include <memory>
#include <string>
#include <vector>

namespace bf6_ui {

struct DirectScreenLoadReport {
    bf6_rime_tree_stats tree{};
    int rows = 0;
    int authored_text_targets = 0;
    int ambiguous_text_targets = 0;
    int interface_graphs = 0;
    int conditional_float_bindings = 0;
    int conditional_float_applications = 0;
    int unresolved_interface_colors = 0;
    int unresolved_interface_states = 0;
    int interface_default_applications = 0;
    int authored_event_edges = 0;
    int unreadable_event_graphs = 0;
    bf6_rime_runtime::CompileReport runtime_compile{};
    bf6_rime_runtime::TickReport runtime_initial_tick{};
    rime_state::ApplyReport runtime_initial_apply{};
    int runtime_initial_layout_apply = 0;
};

struct DirectScreenTickReport {
    bf6_rime_runtime::TickReport runtime{};
    rime_state::ApplyReport applied{};
    int applied_layouts = 0;
};

class DirectInstallScreenDocument final : public AuthoredScreenDocument {
public:
    const ScreenIdentity& identity() const override { return identity_; }
    bool contains(const ElementAddress& address) const override;
    const std::vector<AuthoredEventEdge>& event_edges() const override {
        return event_edges_;
    }

    const rime::Screen& screen() const { return screen_; }
    rime::Screen& screen() { return screen_; }
    const DirectScreenLoadReport& report() const { return report_; }
    const std::vector<ElementAddress>& element_addresses() const {
        return elements_;
    }
    bf6_rime_runtime::Runtime& runtime() { return *runtime_; }
    const bf6_rime_runtime::Runtime& runtime() const { return *runtime_; }

    /* Advance the compiled property graph and apply only decoded renderer
     * fields to this document's retained tree.  No libbf6 read occurs here,
     * so a document may be transferred from the serialized install worker to
     * the render thread after load completes. */
    bool tick_and_apply(double delta_seconds, DirectScreenTickReport& report,
                        std::string& error);

private:
    friend class DirectInstallScreenSource;
    ScreenIdentity identity_;
    rime::Screen screen_;
    DirectScreenLoadReport report_;
    std::vector<ElementAddress> elements_;
    std::vector<AuthoredEventEdge> event_edges_;
    std::unique_ptr<bf6_rime_runtime::Runtime> runtime_;
};

/* Owns one libbf6 context and reads only the caller's currently installed
 * game.  No evidence file, screen manifest, exported tree or derived table is
 * accepted by this interface.  libbf6 contexts are single-threaded; call all
 * methods from the owning worker. */
class DirectInstallScreenSource final : public ScreenSource {
public:
    ~DirectInstallScreenSource() override;

    DirectInstallScreenSource(const DirectInstallScreenSource&) = delete;
    DirectInstallScreenSource& operator=(const DirectInstallScreenSource&) = delete;

    /* WholeInstall mounts level archives as well as shared frontend content.
     * That is the required mode for the measured 257 `/screens/` paths.
     * FrontendOnly is useful for a smaller application but is not complete. */
    enum class MountMode { FrontendOnly, WholeInstall };

    static std::unique_ptr<DirectInstallScreenSource> open(
        const std::string& game_directory, MountMode mode, std::string& error);

    /* Bind the router to a context already opened and mounted by a native
     * host.  The source never closes a borrowed context.  This is the viewer
     * integration path: it avoids opening/indexing the same install twice and
     * keeps every libbf6 read on the viewer's existing serialized worker. */
    static std::unique_ptr<DirectInstallScreenSource> attach_mounted(
        bf6_ctx* context, MountMode mode, std::string& error);

    bool enumerate(std::vector<ScreenIdentity>& out,
                   std::string& error) override;
    std::shared_ptr<AuthoredScreenDocument> load(
        const ScreenIdentity& identity, std::string& error) override;

    bf6_ctx* context() const { return context_; }
    MountMode mount_mode() const { return mount_mode_; }

private:
    DirectInstallScreenSource(bf6_ctx* context, MountMode mode,
                              bool owns_context)
        : context_(context), mount_mode_(mode), owns_context_(owns_context) {}

    bf6_ctx* context_ = nullptr;
    MountMode mount_mode_ = MountMode::FrontendOnly;
    bool owns_context_ = false;
};

/* Apply a frame's explicit occurrence-scoped values through the existing
 * decoded renderer bridge, then re-solve the authored 1920x1080 layout.
 * Unsupported target fields remain reported by ApplyReport; this function
 * never assigns semantics to them. */
bool apply_supported_state(ScreenFrame& frame,
                           rime_state::ApplyReport& report,
                           std::string& error);

} // namespace bf6_ui
