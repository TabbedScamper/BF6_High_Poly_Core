#pragma once

#include "bf6_core.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bf6::ui::boot {

// BootFlow screen availability and BootFlow execution order are deliberately
// separate.  The install authors the former; a runtime provider selects the
// latter.  Keeping that distinction in the API prevents a filename sort from
// quietly becoming a fabricated startup sequence.
enum class MediaSource : uint8_t {
    AuthoredMovieAsset,
    RuntimeProperty
};

enum class SelectionAuthority : uint8_t {
    None,
    RuntimeProvider,
    ExplicitInspection
};

struct MediaBinding {
    std::string screen_path;
    std::string owner_partition;
    std::string element_name;
    int32_t instance = -1;
    MediaSource source = MediaSource::RuntimeProperty;
    // Non-empty only when the RimeMovieElementData record itself imports a
    // MovieTexture2Asset.  An empty value is never replaced with a sibling.
    std::string movie_asset;
};

struct Surface {
    std::string path;
    bool readable_rime_root = false;
    int32_t tree_result = -1;
    std::vector<bf6_rime_node> nodes;
    std::vector<std::string> visual_assets;
    std::vector<MediaBinding> media;
};

struct HubObject {
    int32_t instance = -1;
    std::string blueprint;
};

struct Controls {
    int32_t listed_screen_paths = 0;
    int32_t boot_candidates = 0;
    int32_t readable_roots = 0;
    int32_t unreadable_support_assets = 0;
    int32_t exact_path_round_trips = 0;
    int32_t mutated_path_roots = 0;
    int32_t boot_hub_objects = 0;
    int32_t fake_screen_search_hits = -1;
    int32_t fake_root_result = 0;
    int32_t fake_hub_result = 0;
    bool duplicate_paths = false;
    // Authored hub object order is measured, but it is logic-provider order,
    // not proof of screen presentation order.
    bool runtime_screen_order_proven = false;

    bool pass() const;
};

class Source {
public:
    virtual ~Source() = default;
    virtual int list_screen_paths(std::vector<std::string>& out) = 0;
    virtual int read_tree(std::string_view path,
                          std::vector<bf6_rime_node>& out) = 0;
    virtual int read_hub(std::string_view path,
                         std::vector<HubObject>& out) = 0;
    virtual int search(std::string_view needle) = 0;
};

// Thin adapter over an already-open, already-mounted libbf6 context.  It reads
// the user's current install on each load and consumes no generated manifest.
class DirectInstallSource final : public Source {
public:
    explicit DirectInstallSource(bf6_ctx* context) : context_(context) {}

    int list_screen_paths(std::vector<std::string>& out) override;
    int read_tree(std::string_view path,
                  std::vector<bf6_rime_node>& out) override;
    int read_hub(std::string_view path,
                 std::vector<HubObject>& out) override;
    int search(std::string_view needle) override;

private:
    bf6_ctx* context_ = nullptr;
};

class Runtime {
public:
    bool load(Source& source, std::string* error = nullptr);

    const std::vector<Surface>& surfaces() const { return surfaces_; }
    const std::vector<HubObject>& authored_hub_objects() const { return hub_; }
    const Controls& controls() const { return controls_; }

    const Surface* find(std::string_view path) const;
    const Surface* selected() const;
    SelectionAuthority selection_authority() const { return authority_; }

    // The production runtime/provider must name the selected screen.  This
    // function validates that value against the current mounted install.
    bool apply_provider_selection(std::string_view path);

    // Debugging/inspection is explicit and cannot be mistaken for provider
    // execution by callers inspecting selection_authority().
    bool select_for_inspection(std::string_view path);
    void clear_selection();

    bool requires_runtime_selection() const { return selected_index_ < 0; }

    static bool is_boot_surface_path(std::string_view path);

private:
    bool select(std::string_view path, SelectionAuthority authority);

    std::vector<Surface> surfaces_;
    std::vector<HubObject> hub_;
    Controls controls_{};
    int32_t selected_index_ = -1;
    SelectionAuthority authority_ = SelectionAuthority::None;
};

inline constexpr const char* kBootHubPath =
    "common/ui/bootflow/logic/bootflow_screens_hub";
inline constexpr const char* kFakeScreenPath =
    "common/ui/__control__/screens/not_a_real_boot_screen";
inline constexpr const char* kFakeHubPath =
    "common/ui/__control__/logic/not_a_real_boot_hub";

} // namespace bf6::ui::boot
