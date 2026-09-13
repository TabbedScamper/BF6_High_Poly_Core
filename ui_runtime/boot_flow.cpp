#include "boot_flow.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace bf6::ui::boot {
namespace {

std::string without_ebx_suffix(std::string value)
{
    if (value.size() >= 4 && value.compare(value.size() - 4, 4, ".ebx") == 0)
        value.resize(value.size() - 4);
    return value;
}

std::string mutated_control_path(std::string_view path)
{
    std::string result(path);
    const size_t marker = result.find("/screens/");
    if (marker != std::string::npos)
        result.insert(marker + std::strlen("/screens/"), "__control__/");
    else
        result.append("/__control__");
    return result;
}

void unique_sort(std::vector<std::string>& values)
{
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

} // namespace

bool Controls::pass() const
{
    return listed_screen_paths > 0 &&
           boot_candidates > 0 &&
           readable_roots > 0 &&
           exact_path_round_trips == boot_candidates &&
           mutated_path_roots == 0 &&
           boot_hub_objects > 0 &&
           fake_screen_search_hits == 0 &&
           fake_root_result < 0 &&
           fake_hub_result < 0 &&
           !duplicate_paths &&
           !runtime_screen_order_proven;
}

int DirectInstallSource::list_screen_paths(std::vector<std::string>& out)
{
    out.clear();
    if (!context_) return -1;
    const int count = bf6_list_ebx(context_, "/screens/", nullptr, 0);
    if (count < 0) return count;
    std::vector<bf6_asset> rows(static_cast<size_t>(count));
    if (count > 0 && bf6_list_ebx(context_, "/screens/", rows.data(), count) != count)
        return -1;
    out.reserve(rows.size());
    for (const bf6_asset& row : rows)
        if (row.name && row.name[0]) out.emplace_back(row.name);
    return count;
}

int DirectInstallSource::read_tree(std::string_view path,
                                   std::vector<bf6_rime_node>& out)
{
    out.clear();
    if (!context_ || path.empty()) return -1;
    const std::string key = without_ebx_suffix(std::string(path));
    bf6_rime_tree_stats stats{};
    const int count = bf6_rime_tree(context_, key.c_str(), 8, nullptr, 0, &stats);
    if (count <= 0) return count;
    out.resize(static_cast<size_t>(count));
    const int got = bf6_rime_tree(context_, key.c_str(), 8, out.data(), count, &stats);
    if (got != count) {
        out.clear();
        return -1;
    }
    return got;
}

int DirectInstallSource::read_hub(std::string_view path,
                                  std::vector<HubObject>& out)
{
    out.clear();
    if (!context_ || path.empty()) return -1;
    const std::string key = without_ebx_suffix(std::string(path));
    const int count = bf6_ui_hub_entries(context_, key.c_str(), nullptr, 0);
    if (count <= 0) return count;
    std::vector<bf6_ui_hub_entry> rows(static_cast<size_t>(count));
    if (bf6_ui_hub_entries(context_, key.c_str(), rows.data(), count) != count)
        return -1;
    out.reserve(rows.size());
    for (const bf6_ui_hub_entry& row : rows)
        out.push_back({row.instance, row.blueprint});
    return count;
}

int DirectInstallSource::search(std::string_view needle)
{
    if (!context_) return -1;
    const std::string key(needle);
    return bf6_list_ebx(context_, key.c_str(), nullptr, 0);
}

bool Runtime::is_boot_surface_path(std::string_view path)
{
    const size_t screens = path.find("/screens/");
    if (screens == std::string_view::npos) return false;
    if (path.find("/bootflow/") != std::string_view::npos) return true;
    const std::string_view leaf = path.substr(path.find_last_of('/') + 1);
    return leaf.rfind("bootflow", 0) == 0;
}

bool Runtime::load(Source& source, std::string* error)
{
    clear_selection();
    surfaces_.clear();
    hub_.clear();
    controls_ = {};

    std::vector<std::string> paths;
    const int listed = source.list_screen_paths(paths);
    controls_.listed_screen_paths = listed;
    if (listed < 0) {
        if (error) *error = "failed to enumerate current-install /screens/ assets";
        return false;
    }

    std::sort(paths.begin(), paths.end());
    const size_t before_unique = paths.size();
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    controls_.duplicate_paths = paths.size() != before_unique;

    for (const std::string& raw_path : paths) {
        const std::string path = without_ebx_suffix(raw_path);
        if (!is_boot_surface_path(path)) continue;

        Surface surface;
        surface.path = path;
        surface.tree_result = source.read_tree(path, surface.nodes);
        surface.readable_rime_root = surface.tree_result > 0;
        controls_.readable_roots += surface.readable_rime_root ? 1 : 0;
        controls_.unreadable_support_assets += surface.readable_rime_root ? 0 : 1;

        for (const bf6_rime_node& node : surface.nodes) {
            if (node.image_asset[0]) surface.visual_assets.emplace_back(node.image_asset);
            if (node.kind != BF6_RIME_MOVIE) continue;
            MediaBinding binding;
            binding.screen_path = path;
            binding.owner_partition = node.partition;
            binding.element_name = node.name;
            binding.instance = node.instance;
            if (node.image_asset[0]) {
                binding.source = MediaSource::AuthoredMovieAsset;
                binding.movie_asset = node.image_asset;
            } else {
                binding.source = MediaSource::RuntimeProperty;
            }
            surface.media.push_back(std::move(binding));
        }
        unique_sort(surface.visual_assets);

        // Exact path round-trip: the current source must enumerate this same
        // identity, not merely return a plausible tree for a nearby substring.
        controls_.exact_path_round_trips += source.search(path) == 1 ? 1 : 0;
        std::vector<bf6_rime_node> mutated;
        controls_.mutated_path_roots +=
            source.read_tree(mutated_control_path(path), mutated) > 0 ? 1 : 0;
        surfaces_.push_back(std::move(surface));
    }
    controls_.boot_candidates = static_cast<int32_t>(surfaces_.size());

    controls_.boot_hub_objects = source.read_hub(kBootHubPath, hub_);
    controls_.fake_screen_search_hits = source.search(kFakeScreenPath);
    std::vector<bf6_rime_node> fake_tree;
    controls_.fake_root_result = source.read_tree(kFakeScreenPath, fake_tree);
    std::vector<HubObject> fake_hub;
    controls_.fake_hub_result = source.read_hub(kFakeHubPath, fake_hub);

    if (!controls_.pass()) {
        if (error) *error = "current-install BootFlow controls failed";
        return false;
    }
    return true;
}

const Surface* Runtime::find(std::string_view path) const
{
    const std::string key = without_ebx_suffix(std::string(path));
    const auto it = std::lower_bound(
        surfaces_.begin(), surfaces_.end(), key,
        [](const Surface& lhs, const std::string& rhs) { return lhs.path < rhs; });
    return it != surfaces_.end() && it->path == key ? &*it : nullptr;
}

const Surface* Runtime::selected() const
{
    return selected_index_ >= 0 && selected_index_ < static_cast<int32_t>(surfaces_.size())
        ? &surfaces_[static_cast<size_t>(selected_index_)] : nullptr;
}

bool Runtime::select(std::string_view path, SelectionAuthority authority)
{
    const Surface* surface = find(path);
    if (!surface || !surface->readable_rime_root) return false;
    selected_index_ = static_cast<int32_t>(surface - surfaces_.data());
    authority_ = authority;
    return true;
}

bool Runtime::apply_provider_selection(std::string_view path)
{
    return select(path, SelectionAuthority::RuntimeProvider);
}

bool Runtime::select_for_inspection(std::string_view path)
{
    return select(path, SelectionAuthority::ExplicitInspection);
}

void Runtime::clear_selection()
{
    selected_index_ = -1;
    authority_ = SelectionAuthority::None;
}

} // namespace bf6::ui::boot
