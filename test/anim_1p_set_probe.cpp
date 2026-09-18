/* WHAT FIRST-PERSON CLIPS DOES THE INSTALL ACTUALLY CARRY, AND WHICH BIND?
 *
 *   anim_1p_set_probe <game> [substring ...]
 *
 * The 1P hold is currently a turn-in-place transition used for its first
 * frame, chosen back when the authored rest pose would not bind. The pose and
 * RAW containers read now, so the question is open again - and it is a
 * question about the install, not about a path typed from memory.
 *
 * For every name matching the substring this reports: does bf6_anim_bindings
 * resolve it on the 1P rig, how many channels does the clip declare, how many
 * frames does it carry, and does frame 0 differ from the last frame. Those four
 * numbers separate the three things that all look like "a clip" from outside:
 *
 *   frames 1                 - a POSE. The base of an additive stack.
 *   frames n, no change      - a clip that opens and animates nothing.
 *   frames n, change         - real motion.
 *
 * Binding is reported separately from opening because they fail apart: a clip
 * whose payload reads fine can still have no route onto the skeleton, and that
 * is the failure that made the turn-in-place clip necessary in the first place.
 */
#include "bf6_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char* const kRig1p = "animations/glacier/global/rigging/soldier_1p.rig";
const char* const kSkeleton1p = "common/characters/_soldier/ske_soldier_1p";

struct Tally {
    int total = 0, bound = 0, unbound = 0, no_open = 0;
    int pose = 0, motion = 0, flat = 0;
};
Tally tally;

void report(bf6_ctx* c, const std::string& name, bool quiet)
{
    bf6_anim_binding_stats st{};
    const int keys = bf6_anim_bindings(c, name.c_str(), kRig1p, kSkeleton1p, nullptr, 0, &st);

    bf6_anim_clip* clip = bf6_anim_clip_open(c, name.c_str());
    int frames = -1, channels = -1;
    double moved = -1.0;
    if (clip) {
        frames = clip->key_time_count;
        channels = clip->channel_count;
        if (frames > 0 && channels > 0 && channels < 8192) {
            /* NOT first against last. A LOOPING clip ends where it began, so
             * that comparison reports a 799-frame idle as "flat - animates
             * nothing" purely because it loops - which is what this probe told
             * me about ladtv_1p_rifle_stand_idle_01 on its first run, and it
             * was the probe that was wrong, not the clip.
             *
             * Frame 0 against EVERY frame instead. The largest excursion from
             * the start cannot be faked by looping, and cannot be missed by a
             * clip that moves slowly. */
            std::vector<float> a((size_t)channels * 4, 0.f), b = a;
            if (bf6_anim_clip_sample(c, clip, 0, a.data(), nullptr)) {
                moved = 0.0;
                for (int f = 1; f < frames; ++f) {
                    if (!bf6_anim_clip_sample(c, clip, f, b.data(), nullptr)) { moved = -1.0; break; }
                    for (size_t i = 0; i < a.size(); ++i)
                        moved = std::fmax(moved, std::fabs((double)a[i] - b[i]));
                }
            }
        }
        bf6_free(c, clip);
    }

    const char* kind = "?";
    if (frames < 0)            kind = "does not open";
    else if (frames == 1)      kind = "POSE";
    else if (moved < 0.0)      kind = "opens, would not sample";
    else if (moved < 1e-6)     kind = "flat - animates nothing";
    else                       kind = "MOTION";

    ++tally.total;
    if (frames < 0) ++tally.no_open;
    else if (frames == 1) ++tally.pose;
    else if (moved >= 1e-6) ++tally.motion;
    else ++tally.flat;
    if (keys > 0) ++tally.bound; else ++tally.unbound;

    /* In a sweep only the failures are worth the line. A wall of successes is
     * the thing that hides the four that did not. */
    if (quiet && keys > 0) return;
    std::printf("  %-64s bind %4d  chan %4d  frames %4d  delta %8.4f  %s\n",
                name.c_str(), keys, channels, frames, moved, kind);
}

} /* namespace */

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: anim_1p_set_probe <game> [substring ...]\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    if (!bf6_mount_all(c, 1, err, (int)sizeof(err)))
        std::printf("note: mount_all said %s\n", err);

    std::vector<std::string> pats;
    for (int a = 2; a < argc; ++a) pats.push_back(argv[a]);
    if (pats.empty()) pats.push_back("1p_rifle_stand");

    for (const std::string& pat : pats) {
        const int n = bf6_list_ebx(c, pat.c_str(), nullptr, 0);
        std::printf("\n=== \"%s\": %d name(s)\n", pat.c_str(), n);
        if (n < 1) continue;
        std::vector<bf6_asset> rows((size_t)n);
        const int got = bf6_list_ebx(c, pat.c_str(), rows.data(), n);
        std::vector<std::string> names;
        for (int i = 0; i < got; ++i) if (rows[(size_t)i].name) names.push_back(rows[(size_t)i].name);
        std::sort(names.begin(), names.end());
        /* Past a certain size, print only what failed. The question a sweep
         * answers is "how many and which", not "list everything that worked". */
        const bool quiet = names.size() > 40;
        if (quiet) std::printf("  (sweeping %d, listing only clips that do not bind)\n",
                               (int)names.size());
        for (const std::string& s : names) report(c, s, quiet);
    }

    std::printf("\n=== TOTAL %d clip(s): %d bind, %d DO NOT BIND, %d do not open\n",
                tally.total, tally.bound, tally.unbound, tally.no_open);
    std::printf("    of those: %d pose(1 frame), %d motion, %d open-but-flat\n",
                tally.pose, tally.motion, tally.flat);

    bf6_close(c);
    return 0;
}
