/* HOW FAST DOES THE GAME'S OWN WALK CYCLE TRAVEL?
 *
 *   loco_speed_probe <game_dir> [clip...]
 *
 * BF6 authors no absolute walk or sprint speed. Every GameRemixer partition
 * carries multipliers and thresholds - 1.857 sprint, a 4.8 m/s slide threshold
 * - and never the base they multiply. That is consistent with motion-matched
 * locomotion, where the speed is not a number the designer typed but a property
 * of the animation: the character moves because the ROOT MOVES.
 *
 * Which means the number is not missing at all. It is measurable. Sample a
 * locomotion cycle, follow the root bone, and divide by time.
 *
 * WHAT WOULD MAKE THIS WRONG, stated up front so the output can be judged:
 *   - a cycle authored IN PLACE has a root that never moves, and would report
 *     ~0 rather than a speed. That is visible in the output, not hidden.
 *   - the frame rate is not in the clip payload. 60 is used because the four
 *     front-end ClipPlayer owners author Fps=60 and the 1P pose assets do too.
 *     Every speed below scales linearly with that assumption.
 *   - a cycle may cover a whole number of strides rather than exactly one, but
 *     distance over duration is insensitive to that.
 */
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char* const kRig = "animations/glacier/global/rigging/soldier_3p.rig";
const char* const kSkeleton = "common/characters/_soldier/ske_soldier_3p";
const char* const kLoco = "animations/glacier/assets/3p/common/rifle/loco/";
const float kFps = 60.0f;

/* The clips worth asking about: one per gait, forward, so the numbers line up
 * against crouch/walk/run/sprint. */
const char* const kWanted[] = {
    "c_3p_rifle_stand_walk_fwd_01",
    "c_3p_rifle_stand_run_fwd_01",
    "c_3p_rifle_stand_sprint_fwd_01",
    "c_3p_rifle_crouch_sneak_fwd_01",
    "c_3p_rifle_crouch_slowsneak_fwd_01",
    "c_3p_rifle_crouch_crouchsprint_fwd_01",
};

struct Motion {
    bool ok = false;
    /* WHY it failed, because "unreadable" covered three different things and
     * sent me looking for a clip that was present all along. */
    const char* why = "";
    int frames = 0;
    int bone = -1;
    const char* bone_name = "";
    double distance = 0.0;      /* total path length travelled by the root  */
    double straight = 0.0;      /* start-to-end displacement                */
    double seconds = 0.0;
};

/* Follow the translation of the lowest-index bone the clip actually drives with
 * a VECTOR3 channel - the root or trajectory joint. */
Motion measure(bf6_ctx* c, const std::string& clip, bf6_skeleton* ske)
{
    Motion m;
    bf6_anim_binding_stats st{};
    const int n = bf6_anim_bindings(c, clip.c_str(), kRig, kSkeleton, nullptr, 0, &st);
    if (n < 1 || n > 4096) { m.why = "no animation binding"; return m; }
    std::vector<bf6_anim_binding> b((size_t)n);
    if (bf6_anim_bindings(c, clip.c_str(), kRig, kSkeleton, b.data(), n, &st) != n) { m.why = "bindings did not fill"; return m; }

    /* The root translation channel: the lowest bone index carrying a VECTOR3.
     * On this rig that is Reference or AITrajectory, and taking the lowest
     * rather than naming one means a rig that renames them still works. */
    int best_bone = 1 << 30, channel = -1;
    for (const bf6_anim_binding& x : b)
        if (x.component == BF6_ANIM_DOF_VECTOR3 && x.bone >= 0 && x.bone < best_bone
            && x.channel >= 0) {
            best_bone = x.bone;
            channel = x.channel;
        }
    if (channel < 0) { m.why = "no root VECTOR3 channel - authored in place"; return m; }
    m.bone = best_bone;
    m.bone_name = (ske && best_bone < ske->bone_count && ske->bones[best_bone].name)
                ? ske->bones[best_bone].name : "?";

    bf6_anim_clip* h = bf6_anim_clip_open(c, clip.c_str());
    if (!h || h->key_time_count < 2) { if (h) bf6_free(c, h); m.why = "clip will not open"; return m; }
    m.frames = h->key_time_count;
    std::vector<float> s((size_t)h->channel_count * 4, 0.f);
    double prev[3] = {0, 0, 0}, first[3] = {0, 0, 0}, last[3] = {0, 0, 0};
    bool have_prev = false;
    bool ok = true;
    for (int f = 0; f < m.frames; ++f) {
        if (!bf6_anim_clip_sample(c, h, f, s.data(), nullptr)) { ok = false; break; }
        const float* v = s.data() + (size_t)channel * 4;
        const double p[3] = {v[0], v[1], v[2]};
        if (!have_prev) { for (int k = 0; k < 3; ++k) first[k] = p[k]; have_prev = true; }
        else {
            double d = 0.0;
            for (int k = 0; k < 3; ++k) d += (p[k] - prev[k]) * (p[k] - prev[k]);
            m.distance += std::sqrt(d);
        }
        for (int k = 0; k < 3; ++k) { prev[k] = p[k]; last[k] = p[k]; }
    }
    bf6_free(c, h);
    if (!ok) return m;
    double d = 0.0;
    for (int k = 0; k < 3; ++k) d += (last[k] - first[k]) * (last[k] - first[k]);
    m.straight = std::sqrt(d);
    m.seconds = (double)(m.frames - 1) / kFps;
    m.ok = true;
    return m;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: loco_speed_probe <game> [clip...]\n"); return 2; }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_all(c, 1, err, (int)sizeof(err));
    bf6_skeleton* ske = bf6_skeleton_read(c, kSkeleton);

    std::vector<std::string> clips;
    bool sweep = argc > 2 && std::strcmp(argv[2], "sweep") == 0;
    if (sweep) {
        /* HOW MANY CLIPS ARE EVEN READABLE. Walk and run refuse to bind while
         * sprint and the crouch gaits measure cleanly, which is the same split
         * the front-end _01 clips and the 1P pose assets showed. Counting it
         * turns "some clips do not work" into a number. */
        const char* const root = argc > 3 ? argv[3] : kLoco;
        const int n = bf6_list_ebx(c, root, nullptr, 0);
        if (n > 0) {
            std::vector<bf6_asset> rows((size_t)n);
            const int got = bf6_list_ebx(c, root, rows.data(), n);
            for (int i = 0; i < got; ++i)
                if (rows[(size_t)i].name) clips.push_back(rows[(size_t)i].name);
        }
        std::printf("sweeping %zu partition(s) under %s\n", clips.size(), root);
    }
    else if (argc > 2) for (int i = 2; i < argc; ++i) clips.push_back(argv[i]);
    else for (const char* w : kWanted) clips.push_back(std::string(kLoco) + w);

    std::printf("root motion per locomotion cycle, at %.0f fps\n\n", (double)kFps);
    if (!sweep)
        std::printf("%-42s %6s %-16s %9s %9s %9s\n",
                    "clip", "frames", "root bone", "path m", "net m", "m/s");
    int measured = 0, unbound = 0, in_place = 0, other = 0;
    for (const std::string& clip : clips) {
        const Motion m = measure(c, clip, ske);
        const char* tail = std::strrchr(clip.c_str(), '/');
        if (!m.ok) {
            if (std::strstr(m.why, "binding")) ++unbound;
            else if (std::strstr(m.why, "in place")) ++in_place;
            else ++other;
            if (!sweep)
                std::printf("%-42s   %s\n", tail ? tail + 1 : clip.c_str(),
                            m.why[0] ? m.why : "sampling failed");
            continue;
        }
        ++measured;
        std::printf("%-42s %6d %-16s %9.3f %9.3f %9.3f\n",
                    tail ? tail + 1 : clip.c_str(), m.frames, m.bone_name,
                    m.distance, m.straight, m.seconds > 0 ? m.straight / m.seconds : 0.0);
    }
    if (sweep)
        std::printf("\nmeasured %d, would not bind %d, authored in place %d, other %d\n",
                    measured, unbound, in_place, other);

    std::printf("\nA net displacement near zero means the cycle is authored IN PLACE,\n"
                "and its speed lives somewhere else entirely.\n");
    if (ske) bf6_free(c, ske);
    bf6_close(c);
    return 0;
}
