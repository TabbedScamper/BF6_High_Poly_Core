/* WHICH IDLE CLIP EACH ROLE ACTUALLY GETS.
 *
 *   idle_clip_probe <game_dir>
 *
 * Reported as "the poses for recon and support are wrong". The soldier preview
 * takes the first front-end standing idle that resolves for a role, trying
 * "_01" then "_02", because under the FRONT-END mount assault had an "_01" and
 * the other three did not. That made assault play one take and everyone else a
 * different one, which is exactly what "wrong pose" looks like.
 *
 * The shipped data has more in it than that fallback assumes: every role has
 * _01, _02 AND an "_01_(loop)" variant, and there are idlebreak01/idlebreak02
 * families beside them. A looping idle is a different thing from a 38-second
 * take with breaks in it, and the menu plays one of them, not whichever happens
 * to resolve first.
 *
 * So this prints, for every role and every candidate spelling, whether it
 * resolves under the front-end mount, under a full mount, and how long it is.
 * The answer to "which one should we play" is in the shape of that table, not
 * in a guess. */
#include "bf6_core.h"

#include <cstdio>
#include <ctime>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char* const kRoles[4] = {"assault", "engineer", "support", "recon"};
const char* const kRig = "animations/glacier/global/rigging/soldier_3p.rig";
const char* const kSkeleton = "common/characters/_soldier/ske_soldier_3p";
const char* const kBase =
    "animations/glacier/assets/frontend/mainmenu/loadout/ui_frontend_standing_";

/* Every spelling worth asking about, as a suffix after the role stem. */
struct Candidate { const char* stem; const char* suffix; };
const Candidate kCandidates[] = {
    {"idle_",            "_01"},
    {"idle_",            "_01_(loop)"},
    {"idle_",            "_02"},
    {"idle_cl_",         "_01"},
    {"idle_cl_",         "_01_(loop)"},
    {"idle_cl_",         "_02"},
    {"idlebreak01_",     "_02"},
    {"idlebreak02_",     "_02"},
};

/* Bindings + frame count for one clip, or 0 bindings when it is absent. */
void describe(bf6_ctx* c, const std::string& path, int& bindings, int& frames)
{
    bindings = 0;
    frames = 0;
    bf6_anim_binding_stats stats{};
    const int n = bf6_anim_bindings(c, path.c_str(), kRig, kSkeleton, nullptr, 0, &stats);
    if (n < 1 || n > 4096) return;
    bindings = n;
    bf6_anim_clip* clip = bf6_anim_clip_open(c, path.c_str());
    if (clip) {
        frames = clip->key_time_count;
        bf6_free(c, clip);
    }
}

void table(bf6_ctx* c, const char* what)
{
    std::printf("\n=== %s ===\n", what);
    std::printf("%-24s", "clip");
    for (int r = 0; r < 4; ++r) std::printf("%18s", kRoles[r]);
    std::printf("\n");
    for (const Candidate& cand : kCandidates) {
        char label[64];
        std::snprintf(label, sizeof(label), "%s<role>%s", cand.stem, cand.suffix);
        std::printf("%-24s", label);
        for (int r = 0; r < 4; ++r) {
            const std::string path = std::string(kBase) + cand.stem + kRoles[r] + cand.suffix;
            int bindings = 0, frames = 0;
            describe(c, path, bindings, frames);
            char cell[32];
            if (bindings == 0) std::snprintf(cell, sizeof(cell), "%s", "-");
            else std::snprintf(cell, sizeof(cell), "%d bind/%d fr", bindings, frames);
            std::printf("%18s", cell);
        }
        std::printf("\n");
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: idle_clip_probe <game>\n"); return 2; }
    char err[512] = {0};

    /* WHAT WIDENING COSTS. The front-end mount cannot see three of the four
     * roles' _01 clips, so the preview quietly plays a different take for
     * them. Mounting wider fixes that, and whether it can be done on demand
     * depends entirely on this number. */
    {
        bf6_ctx* t = bf6_open(argv[1], err, (int)sizeof(err));
        if (t) {
            const int64_t a = (int64_t)clock();
            bf6_mount_frontend(t, err, (int)sizeof(err));
            const int64_t b = (int64_t)clock();
            bf6_mount_all(t, 0, err, (int)sizeof(err));
            const int64_t d = (int64_t)clock();
            std::printf("mount_frontend %.2f s, then mount_all(levels=0) a further %.2f s\n",
                        (double)(b - a) / CLOCKS_PER_SEC, (double)(d - b) / CLOCKS_PER_SEC);
            bf6_close(t);
        }
    }

    bf6_ctx* fe = bf6_open(argv[1], err, (int)sizeof(err));
    if (!fe) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_frontend(fe, err, (int)sizeof(err));
    table(fe, "FRONT-END MOUNT (what the soldier preview sees)");
    bf6_close(fe);

    /* Widening WITHOUT levels, which is the cheap option and therefore the one
     * worth knowing about: if the clips are not in here, the fix has to be a
     * targeted mount rather than a broader one. */
    {
        bf6_ctx* nl = bf6_open(argv[1], err, (int)sizeof(err));
        if (nl) {
            bf6_mount_frontend(nl, err, (int)sizeof(err));
            const int64_t a = (int64_t)clock();
            bf6_mount_all(nl, 0, err, (int)sizeof(err));
            const int64_t b = (int64_t)clock();
            char title[128];
            std::snprintf(title, sizeof(title),
                          "FRONT-END + mount_all(levels=0)  [+%.2f s]",
                          (double)(b - a) / CLOCKS_PER_SEC);
            table(nl, title);
            bf6_close(nl);
        }
    }

    bf6_ctx* all = bf6_open(argv[1], err, (int)sizeof(err));
    if (!all) { std::printf("open: %s\n", err); return 1; }
    const int64_t ta = (int64_t)clock();
    bf6_mount_all(all, 1, err, (int)sizeof(err));
    const int64_t tb = (int64_t)clock();
    {
        char title[128];
        std::snprintf(title, sizeof(title), "FULL MOUNT levels=1 (what the game ships)  [%.2f s]",
                      (double)(tb - ta) / CLOCKS_PER_SEC);
        table(all, title);
    }

    /* And what else is in that folder, since the fallback only ever guessed at
     * names rather than reading the list. */
    std::printf("\n=== every ui_frontend_standing_* partition under a full mount ===\n");
    const int n = bf6_list_ebx(all, "ui_frontend_standing_", nullptr, 0);
    std::printf("%d partition(s)\n", n);
    if (n > 0 && n < 400) {
        std::vector<bf6_asset> rows((size_t)n);
        const int got = bf6_list_ebx(all, "ui_frontend_standing_", rows.data(), n);
        for (int i = 0; i < got; ++i) {
            const char* name = rows[(size_t)i].name ? rows[(size_t)i].name : "";
            const char* tail = std::strrchr(name, '/');
            std::printf("   %s\n", tail ? tail + 1 : name);
        }
    }
    /* ARE THE FOUR ROLES ACTUALLY DIFFERENT? Every _01 comes back 505
     * bindings and 1139 frames, which is either four takes cut to the same
     * length or one take under four names. It matters: if _01 is shared then
     * playing it for all four roles makes the whole squad move identically,
     * and the per-role stance lives in _02 instead. Frame 0 answers it. */
    std::printf("\n=== is each role's clip its own? frame 0, vs assault ===\n");
    for (const char* stem : {"idle_", "idle_"}) {
        static bool second = false;
        const char* suffix = second ? "_02" : "_01";
        second = true;
        std::vector<float> ref;
        std::printf("%s%s:\n", stem, suffix);
        for (int r = 0; r < 4; ++r) {
            const std::string path = std::string(kBase) + stem + kRoles[r] + suffix;
            bf6_anim_clip* clip = bf6_anim_clip_open(all, path.c_str());
            if (!clip) { std::printf("   %-10s absent\n", kRoles[r]); continue; }
            std::vector<float> s((size_t)clip->channel_count * 4, 0.f);
            const int ok = bf6_anim_clip_sample(all, clip, 0, s.data(), nullptr);
            const int frames = clip->key_time_count;
            bf6_free(all, clip);
            if (!ok) { std::printf("   %-10s unsampleable\n", kRoles[r]); continue; }
            if (r == 0) { ref = s; std::printf("   %-10s %d fr  (reference)\n", kRoles[r], frames); continue; }
            double worst = 0.0;
            const size_t n2 = s.size() < ref.size() ? s.size() : ref.size();
            for (size_t k = 0; k < n2; ++k) {
                const double d = (double)s[k] - (double)ref[k];
                worst = worst > (d < 0 ? -d : d) ? worst : (d < 0 ? -d : d);
            }
            std::printf("   %-10s %d fr  worst channel difference vs assault: %.6g%s\n",
                        kRoles[r], frames, worst,
                        worst < 1e-6 ? "   <- IDENTICAL" : "");
        }
    }
    /* PAIRWISE, because "every role differs from assault by the same number"
     * is the kind of statistic that reads as "identical" and can just as
     * easily be a coincidence of maxima. Comparing every pair over the whole
     * sample settles it. */
    std::printf("\n=== pairwise, frame 0, whole channel vector ===\n");
    for (const char* suffix : {"_01", "_02"}) {
        std::printf("idle_<role>%s\n", suffix);
        std::vector<std::vector<float>> got(4);
        for (int r = 0; r < 4; ++r) {
            const std::string path = std::string(kBase) + "idle_" + kRoles[r] + suffix;
            bf6_anim_clip* clip = bf6_anim_clip_open(all, path.c_str());
            if (!clip) continue;
            got[(size_t)r].assign((size_t)clip->channel_count * 4, 0.f);
            if (!bf6_anim_clip_sample(all, clip, 0, got[(size_t)r].data(), nullptr))
                got[(size_t)r].clear();
            bf6_free(all, clip);
        }
        for (int a = 0; a < 4; ++a)
            for (int b = a + 1; b < 4; ++b) {
                if (got[(size_t)a].empty() || got[(size_t)b].empty()) continue;
                double worst = 0.0;
                int differing = 0;
                const size_t n2 = got[(size_t)a].size() < got[(size_t)b].size()
                                ? got[(size_t)a].size() : got[(size_t)b].size();
                for (size_t k = 0; k < n2; ++k) {
                    double d = (double)got[(size_t)a][k] - (double)got[(size_t)b][k];
                    if (d < 0) d = -d;
                    if (d > 1e-6) ++differing;
                    if (d > worst) worst = d;
                }
                std::printf("   %-9s vs %-9s  worst %.6g over %zu floats, %d differ%s\n",
                            kRoles[a], kRoles[b], worst, n2, differing,
                            differing == 0 ? "   <- SAME CLIP" : "");
            }
    }
    bf6_close(all);
    return 0;
}
