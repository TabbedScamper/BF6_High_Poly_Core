/* WHICH ARCHIVE OWNS THE MISSING POSE CLIPS?
 *
 *   pose_owner_probe <game_dir>
 *
 * Changing a spawner's Pose to Engineer, Support or Recon fails with "no
 * readable animation binding". The clips are NOT missing and the decode is NOT
 * broken - under a full mount all four resolve identically, 505 bindings and
 * 1,139 keys each. Under bf6_mount_frontend, which is what the soldier preview
 * actually calls, only assault resolves and the other three are absent:
 *
 *   partitions matching "ui_frontend_standing_idle_":  24 (all) vs 10 (frontend)
 *
 * So the fix is to widen the mount, and widening it correctly means knowing
 * WHICH archive family holds them rather than adding likely-looking names. This
 * mounts one TOC at a time into a fresh context and reports which ones can see
 * each clip.
 */
#include "bf6_core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char* const kRoles[4] = {"assault", "engineer", "support", "recon"};

std::string clip_of(const char* role)
{
    return std::string("animations/glacier/assets/frontend/mainmenu/loadout/"
                       "ui_frontend_standing_idle_") + role + "_01";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: pose_owner_probe <game>\n"); return 2; }
    char err[512] = {0};

    /* First: the whole-install answer, so the per-TOC results have something to
     * be measured against. */
    bf6_ctx* all = bf6_open(argv[1], err, (int)sizeof(err));
    if (!all) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_all(all, 1, err, (int)sizeof(err));
    std::printf("under a FULL mount:\n");
    for (int i = 0; i < 4; ++i) {
        const std::string clip = clip_of(kRoles[i]);
        const int n = bf6_list_ebx(all, clip.c_str(), nullptr, 0);
        std::printf("   %-9s partitions=%d\n", kRoles[i], n);
    }
    bf6_close(all);

    /* Then the same question of the FRONTEND mount, which is what fails. */
    bf6_ctx* fe = bf6_open(argv[1], err, (int)sizeof(err));
    if (!fe) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_frontend(fe, err, (int)sizeof(err));
    std::printf("\nunder the FRONT-END mount (what the soldier preview uses):\n");
    for (int i = 0; i < 4; ++i) {
        const std::string clip = clip_of(kRoles[i]);
        const int n = bf6_list_ebx(fe, clip.c_str(), nullptr, 0);
        std::printf("   %-9s partitions=%d %s\n", kRoles[i], n,
                    n > 0 ? "" : "<- MISSING FROM THE MOUNT");
    }

    /* What DOES the frontend mount carry that looks like these? Printing the
     * ten it has, against the twenty-four that exist, usually names the
     * convention difference outright. */
    std::printf("\nthe %d idle clips the front-end mount can see:\n",
                bf6_list_ebx(fe, "ui_frontend_standing_idle_", nullptr, 0));
    const int have = bf6_list_ebx(fe, "ui_frontend_standing_idle_", nullptr, 0);
    if (have > 0) {
        std::vector<bf6_asset> rows((size_t)have);
        const int got = bf6_list_ebx(fe, "ui_frontend_standing_idle_", rows.data(), have);
        for (int i = 0; i < got; ++i)
            std::printf("   %s\n", rows[(size_t)i].name ? rows[(size_t)i].name : "");
    }
    bf6_close(fe);
    return 0;
}
