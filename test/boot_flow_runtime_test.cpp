#include "boot_flow.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace boot = bf6::ui::boot;

namespace {

bf6_rime_node node(int kind, int instance, const char* name,
                   const char* partition, const char* image = nullptr)
{
    bf6_rime_node value{};
    value.kind = kind;
    value.instance = instance;
    std::snprintf(value.name, sizeof(value.name), "%s", name ? name : "");
    std::snprintf(value.partition, sizeof(value.partition), "%s",
                  partition ? partition : "");
    std::snprintf(value.image_asset, sizeof(value.image_asset), "%s",
                  image ? image : "");
    return value;
}

class FixtureSource final : public boot::Source {
public:
    std::vector<std::string> paths;
    std::map<std::string, std::vector<bf6_rime_node>> trees;
    std::map<std::string, std::vector<boot::HubObject>> hubs;

    int list_screen_paths(std::vector<std::string>& out) override
    {
        out = paths;
        return static_cast<int>(out.size());
    }

    int read_tree(std::string_view path,
                  std::vector<bf6_rime_node>& out) override
    {
        auto it = trees.find(std::string(path));
        if (it == trees.end()) { out.clear(); return -1; }
        out = it->second;
        return static_cast<int>(out.size());
    }

    int read_hub(std::string_view path,
                 std::vector<boot::HubObject>& out) override
    {
        auto it = hubs.find(std::string(path));
        if (it == hubs.end()) { out.clear(); return -1; }
        out = it->second;
        return static_cast<int>(out.size());
    }

    int search(std::string_view needle) override
    {
        int count = 0;
        for (const std::string& path : paths)
            count += path.find(needle) != std::string::npos;
        return count;
    }
};

bool unit_test()
{
    constexpr const char* logo =
        "game/glacierflow/flow_mainmenu/ui/screens/bootflowlogoscreen";
    constexpr const char* cinematic =
        "common/ui/bootflow/screens/bootflow_backgroundcinematicscreen";
    constexpr const char* callback =
        "game/glacierflow/flow_mainmenu/ui/screens/bootflowloginqueue_callback_viewmodelex";

    FixtureSource fixture;
    // Deliberately unsorted. Runtime must be deterministic without treating
    // that stable output order as presentation order.
    fixture.paths = {
        "common/ui/weapons/screens/menuweaponscreen",
        cinematic,
        callback,
        logo
    };
    fixture.trees[logo] = {
        node(BF6_RIME_SVG, 2, "DICE logo", logo,
             "common/ui/assets/images/logos/dice"),
        node(BF6_RIME_MOVIE, 3, "Logo movie", logo,
             "common/ui/assets/movies/authored_logo")
    };
    fixture.trees[cinematic] = {
        node(BF6_RIME_MOVIE, 7, "Background movie", cinematic)
    };
    fixture.hubs[boot::kBootHubPath] = {
        {6, "common/ui/bootflow/logic/bootflow_ftue_logic.ebx"},
        {4, "common/ui/bootflow/legal/logic/bootflow_legallogic.ebx"}
    };

    boot::Runtime runtime;
    std::string error;
    if (!runtime.load(fixture, &error)) {
        std::fprintf(stderr, "fixture load failed: %s\n", error.c_str());
        return false;
    }
    if (!runtime.controls().pass() || runtime.surfaces().size() != 3 ||
        runtime.controls().readable_roots != 2 ||
        runtime.controls().unreadable_support_assets != 1 ||
        runtime.authored_hub_objects().size() != 2 ||
        runtime.authored_hub_objects()[0].instance != 6 ||
        !runtime.requires_runtime_selection()) return false;

    const boot::Surface* logo_surface = runtime.find(logo);
    const boot::Surface* cinematic_surface = runtime.find(cinematic);
    if (!logo_surface || !cinematic_surface ||
        logo_surface->visual_assets.size() != 2 ||
        logo_surface->media.size() != 1 ||
        logo_surface->media[0].source != boot::MediaSource::AuthoredMovieAsset ||
        cinematic_surface->media.size() != 1 ||
        cinematic_surface->media[0].source != boot::MediaSource::RuntimeProperty)
        return false;

    if (runtime.apply_provider_selection(callback) ||
        runtime.apply_provider_selection("common/ui/__control__/missing"))
        return false;
    if (!runtime.select_for_inspection(logo) ||
        runtime.selection_authority() != boot::SelectionAuthority::ExplicitInspection)
        return false;
    runtime.clear_selection();
    if (!runtime.apply_provider_selection(cinematic) ||
        runtime.selection_authority() != boot::SelectionAuthority::RuntimeProvider ||
        !runtime.selected() || runtime.selected()->path != cinematic)
        return false;

    return true;
}

bool live_test(const char* game_dir)
{
    char error_buffer[512]{};
    bf6_ctx* context = bf6_open(game_dir, error_buffer,
                                static_cast<int>(sizeof(error_buffer)));
    if (!context) {
        std::fprintf(stderr, "open: %s\n", error_buffer);
        return false;
    }
    if (!bf6_mount_all(context, 1, error_buffer,
                       static_cast<int>(sizeof(error_buffer)))) {
        std::fprintf(stderr, "mount: %s\n", error_buffer);
        bf6_close(context);
        return false;
    }

    boot::DirectInstallSource source(context);
    boot::Runtime runtime;
    std::string error;
    const bool loaded = runtime.load(source, &error);
    if (!loaded) std::fprintf(stderr, "BootFlow: %s\n", error.c_str());

    int authored_movies = 0;
    int runtime_movies = 0;
    for (const boot::Surface& surface : runtime.surfaces()) {
        for (const boot::MediaBinding& media : surface.media) {
            authored_movies += media.source == boot::MediaSource::AuthoredMovieAsset;
            runtime_movies += media.source == boot::MediaSource::RuntimeProperty;
            std::printf("  movie source=%s instance=%d owner=%s element=%s asset=%s\n",
                        media.source == boot::MediaSource::AuthoredMovieAsset
                            ? "authored" : "runtime-property",
                        media.instance, media.owner_partition.c_str(),
                        media.element_name.c_str(),
                        media.movie_asset.empty() ? "<provider-required>"
                                                  : media.movie_asset.c_str());
        }
        std::printf("boot surface rime=%d rows=%d media=%zu assets=%zu %s\n",
                    surface.readable_rime_root ? 1 : 0, surface.tree_result,
                    surface.media.size(), surface.visual_assets.size(),
                    surface.path.c_str());
    }
    for (const boot::HubObject& row : runtime.authored_hub_objects())
        std::printf("boot hub object instance=%d %s\n", row.instance,
                    row.blueprint.c_str());

    const boot::Controls& c = runtime.controls();
    std::printf("controls candidates=%d readable=%d support=%d exact=%d "
                "mutated=%d hub=%d fake=%d/%d/%d order_proven=%d\n",
                c.boot_candidates, c.readable_roots,
                c.unreadable_support_assets, c.exact_path_round_trips,
                c.mutated_path_roots, c.boot_hub_objects,
                c.fake_screen_search_hits, c.fake_root_result,
                c.fake_hub_result, c.runtime_screen_order_proven ? 1 : 0);
    std::printf("movie sources authored=%d runtime=%d\n",
                authored_movies, runtime_movies);

    constexpr const char* logo =
        "game/glacierflow/flow_mainmenu/ui/screens/bootflowlogoscreen";
    constexpr const char* start =
        "game/glacierflow/flow_mainmenu/ui/screens/bootflowstartscreen";
    constexpr const char* legal =
        "common/ui/bootflow/legal/screens/bootflow_legalscreen";
    constexpr const char* background =
        "common/ui/bootflow/screens/bootflow_backgroundcinematicscreen";
    constexpr const char* season =
        "common/ui/bootflow/screens/bootflow_seasoncinematicscreen";
    constexpr const char* callback =
        "game/glacierflow/flow_mainmenu/ui/screens/bootflowloginqueue_callback_viewmodelex";

    const boot::Surface* logo_surface = runtime.find(logo);
    const boot::Surface* background_surface = runtime.find(background);
    const boot::Surface* season_surface = runtime.find(season);
    const std::vector<boot::HubObject> expected_hub = {
        {6, "common/ui/bootflow/logic/bootflow_ftue_logic.ebx"},
        {4, "common/ui/bootflow/legal/logic/bootflow_legallogic.ebx"},
        {3, "common/ui/bootflow/logic/bootflow_loadingdebuglogic.ebx"},
        {5, "common/ui/bootflow/logic/bootflow_killswitchprovider.ebx"}
    };
    bool exact_hub = runtime.authored_hub_objects().size() == expected_hub.size();
    for (size_t i = 0; exact_hub && i < expected_hub.size(); ++i)
        exact_hub = runtime.authored_hub_objects()[i].instance == expected_hub[i].instance &&
                    runtime.authored_hub_objects()[i].blueprint == expected_hub[i].blueprint;
    const bool pass = loaded && c.pass() &&
        c.boot_candidates == 14 && c.readable_roots == 13 &&
        c.unreadable_support_assets == 1 && c.boot_hub_objects == 4 &&
        exact_hub && authored_movies == 2 && runtime_movies == 2 &&
        logo_surface && logo_surface->readable_rime_root &&
        runtime.find(start) && runtime.find(start)->readable_rime_root &&
        runtime.find(legal) && runtime.find(legal)->readable_rime_root &&
        background_surface && !background_surface->media.empty() &&
        background_surface->media[0].source == boot::MediaSource::RuntimeProperty &&
        season_surface && !season_surface->media.empty() &&
        season_surface->media[0].source == boot::MediaSource::RuntimeProperty &&
        runtime.find(callback) && !runtime.find(callback)->readable_rime_root &&
        !runtime.selected() && runtime.requires_runtime_selection();

    bf6_close(context);
    return pass;
}

} // namespace

int main(int argc, char** argv)
{
    if (!unit_test()) {
        std::fprintf(stderr, "boot_flow_runtime_test: fixture FAIL\n");
        return 1;
    }
    std::printf("boot_flow_runtime_test: fixture PASS\n");
    if (argc < 2) return 0;
    if (!live_test(argv[1])) {
        std::fprintf(stderr, "boot_flow_runtime_test: live FAIL\n");
        return 1;
    }
    std::printf("boot_flow_runtime_test: live PASS\n");
    return 0;
}
