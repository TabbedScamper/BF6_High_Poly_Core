#include "synthetic_texture_ids.h"

#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

int main()
{
    bf6_viewer::SyntheticTextureIds first;
    const int home = first.id_for("rime-svg", "common/ui/home/icon");
    const int homeAgain = first.id_for("rime-svg", "common/ui/home/icon");
    const int display = first.id_for("rime-svg", "common/ui/options/icon");
    const int service = first.id_for("live-menu", "common/ui/home/icon");

    bf6_viewer::SyntheticTextureIds reversed;
    const int reversedDisplay =
        reversed.id_for("rime-svg", "common/ui/options/icon");
    const int reversedHome =
        reversed.id_for("rime-svg", "common/ui/home/icon");

    // Model the viewer's process-lifetime SRV cache across the route sequence
    // that exposed the original failure. Every generated upload may retain an
    // existing id only when it represents the same content identity.
    bf6_viewer::SyntheticTextureIds routed;
    std::map<int, std::string> persistentCache;
    bool routeIdentityExact = true;
    const auto installRoute = [&](const std::vector<std::pair<
                                      std::string, std::string>>& textures) {
        for (const auto& texture : textures)
        {
            const int id = routed.id_for(texture.first, texture.second);
            const std::string key = texture.first + ":" + texture.second;
            const auto cached = persistentCache.find(id);
            if (cached != persistentCache.end() && cached->second != key)
                routeIdentityExact = false;
            else
                persistentCache[id] = key;
        }
    };
    const std::vector<std::pair<std::string, std::string>> homeTextures = {
        {"rime-svg", "common/ui/home/assets/timer"},
        {"live-menu", "season-4-header:https://service/season-4.png"},
        {"live-menu", "top-gun-patch:https://service/top-gun.png"},
    };
    const std::vector<std::pair<std::string, std::string>> displayTextures = {
        {"rime-svg", "common/ui/options/assets/error"},
        {"rime-svg", "common/ui/options/assets/navigation-left"},
        // Shared service content must intentionally reuse the same cache id.
        {"live-menu", "season-4-header:https://service/season-4.png"},
    };
    installRoute(homeTextures);
    const std::map<int, std::string> homeCache = persistentCache;
    installRoute(displayTextures);
    installRoute(homeTextures);
    const bool homeDisplayHomeExact = routeIdentityExact &&
        homeCache.size() == 3 && persistentCache.size() == 5;

    const bool pass = home < 0 && display < 0 && service < 0 &&
        home == homeAgain && home != display && home != service &&
        home == reversedHome && display == reversedDisplay &&
        first.size() == 3 && reversed.size() == 2 && homeDisplayHomeExact;
    std::printf("home=%d/%d display=%d service=%d reversed=%d/%d "
                "sizes=%zu/%zu route-cycle=%d cache=%zu\n",
                home, homeAgain, display, service, reversedHome,
                reversedDisplay, first.size(), reversed.size(),
                homeDisplayHomeExact ? 1 : 0, persistentCache.size());
    return pass ? 0 : 1;
}
