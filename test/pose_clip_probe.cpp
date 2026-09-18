/* WHICH FRONT-END IDLE CLIPS ACTUALLY SHIP?
 *
 *   pose_clip_probe <game_dir> [substring]
 *
 * The soldier preview builds one clip path per role:
 *
 *   animations/glacier/assets/frontend/mainmenu/loadout/ui_frontend_standing_idle_<role>_01
 *
 * `assault` resolves and engineer, support and recon do not - which is why
 * changing a spawner's Pose to any of those three fails. A path built by string
 * concatenation only works while every member of the set is named to the same
 * pattern, so the question is not "why is the binding unreadable" but "what are
 * those clips really called".
 *
 * So list every partition whose name matches, and let the naming answer it
 * rather than guessing at another suffix.
 */
#include "bf6_core.h"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: pose_clip_probe <game> [substring]\n"); return 2; }
    const std::string needle = argc > 2 ? argv[2] : "standing_idle";
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }

    /* WHICH MOUNT MATTERS. The soldier preview does NOT mount everything:
     * loadout.cpp calls bf6_mount_frontend, a focused mount over the shared
     * UI / weapons / characters archive families. A clip that resolves under
     * mount_all but not under mount_frontend means the clip is fine and the
     * MOUNT is the bug - a completely different fix from decoding a binding.
     * Pass "frontend" as the third argument to see what the preview sees. */
    const bool frontend_only = argc > 3 && std::strcmp(argv[3], "frontend") == 0;
    if (frontend_only) {
        const int ok = bf6_mount_frontend(c, err, (int)sizeof(err));
        std::printf("mount: FRONTEND ONLY (as the soldier preview does) -> %d %s\n",
                    ok, err[0] ? err : "");
    } else {
        if (bf6_mount_all(c, 1, err, (int)sizeof(err)) != 0)
            std::printf("note: mount_all said '%s'\n", err[0] ? err : "(nothing)");
        std::printf("mount: ALL\n");
    }

    const int total = bf6_list_ebx(c, needle.c_str(), nullptr, 0);
    std::printf("partitions matching \"%s\": %d\n", needle.c_str(), total);
    if (total > 0) {
        const int cap = total > 500 ? 500 : total;
        std::vector<bf6_asset> rows((size_t)cap);
        const int got = bf6_list_ebx(c, needle.c_str(), rows.data(), cap);
        std::vector<std::string> names;
        for (int i = 0; i < got; ++i)
            if (rows[(size_t)i].name) names.push_back(rows[(size_t)i].name);
        /* The mount's order is a hash order, so sort: the question here is what
         * the naming CONVENTION is, and an unsorted list hides it. */
        std::sort(names.begin(), names.end());
        for (const std::string& n : names) std::printf("   %s\n", n.c_str());
        if (got < total) std::printf("   ... %d more\n", total - got);
    }
    /* THE ACTUAL QUESTION: the clips all exist, so does the BINDING resolve?
     * Same call the soldier preview makes, once per role, against the same rig
     * and skeleton. */
    static const char* const kRig = "animations/glacier/global/rigging/soldier_3p.rig";
    static const char* const kSkeleton = "common/characters/_soldier/ske_soldier_3p";
    static const char* const kRoles[4] = {"assault", "engineer", "support", "recon"};
    std::printf("\nbf6_anim_bindings against rig=%s\n", kRig);
    for (int i = 0; i < 4; ++i) {
        const std::string clip =
            std::string("animations/glacier/assets/frontend/mainmenu/loadout/"
                        "ui_frontend_standing_idle_") + kRoles[i] + "_01";
        bf6_anim_binding_stats st{};
        const int n = bf6_anim_bindings(c, clip.c_str(), kRig, kSkeleton, nullptr, 0, &st);
        bf6_anim_clip* cl = bf6_anim_clip_open(c, clip.c_str());
        std::printf("   %-9s bindings=%-6d clip=%s", kRoles[i], n, cl ? "open" : "NULL");
        if (cl) {
            std::printf(" channels=%d keys=%d", cl->channel_count, cl->key_time_count);
            bf6_free(c, cl);
        }
        std::printf("\n");
    }

    bf6_close(c);
    return 0;
}
