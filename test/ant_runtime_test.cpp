/* THE ANT RUNTIME, below the state machine, on inspect's left-side layers.
 *
 *   ant_runtime_test <game>
 *
 * Runs animations/glacier/controllers/1p.inspect.l.slc - base pose from a
 * weapon lookup, a right-hand weapon-pose layer through a bone mask, the
 * phase-driven inspect roll as an additive layer, and an idle additive - as
 * an M4A1 (fb.wep.specificweapon = 46, the M4A1 row of the inspect lookup).
 *
 * Checks:
 *   - the runtime builds and updates;
 *   - every rotation stays unit length;
 *   - the pose DEPENDS on the roll phase: two different
 *     1p.aimleftright.phased.float values give different poses, and the same
 *     value twice gives the same pose (the phase controller drives time
 *     absolutely, so this must be repeatable);
 *   - notes are printed, so what is not yet implemented is visible.
 */
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {
int fails = 0;
void bad(const char* w) { std::printf("  FAIL %s\n", w); ++fails; }
constexpr const char* kRoot = "animations/glacier/controllers/1p.inspect.l.slc";
constexpr const char* kRig = "animations/glacier/global/rigging/soldier_1p.rig";
constexpr const char* kSke = "common/characters/_soldier/ske_soldier_1p";
constexpr const char* kPhase = "animations/glacier/global/gamestates/1p.aimleftright.phased.float";
constexpr const char* kWeapon = "animations/glacier/global/gamestates/fb.wep.specificweapon.enumgs";
/* The weapon TYPE is a separate key. Its authored default (1) is not rifle:
 * fb.weapontype.rifle.enumgsitem names value 0 (lmg 3, pistol 5, sniper 6,
 * shotgun 7), so an M4A1 must set it. */
constexpr const char* kType = "animations/common/fb.weapontype.enumgs";

std::vector<float> pose_at(bf6_ant_runtime* rt, float phase, int bones)
{
    /* ZERO-time updates: the phase controller drives time absolutely, while
     * the idle additive runs on real time - advancing time would move it
     * between two samples and make "same phase, same pose" a false test. */
    bf6_ant_runtime_set_float(rt, kPhase, phase);
    bf6_ant_runtime_update(rt, 0.f);
    std::vector<float> p((size_t)bones * 12);
    bf6_ant_runtime_pose(rt, p.data(), bones);
    return p;
}
double diff(const std::vector<float>& a, const std::vector<float>& b)
{
    double d = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) d = std::fmax(d, std::fabs(a[i] - b[i]));
    return d;
}
}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) { std::printf("usage: ant_runtime_test <game>\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_all(c, 1, err, (int)sizeof(err));
    int rc = 0;
    {
        bf6_ant_runtime* rt = bf6_ant_runtime_create(c, kRoot, kRig, kSke, err, (int)sizeof(err));
        if (!rt) { std::printf("create: %s\n", err); bf6_close(c); return 1; }
        bf6_ant_runtime_set_int(rt, kWeapon, 46);
        bf6_ant_runtime_set_int(rt, kType, 0);
        for (int i = 0; i < 30; ++i) bf6_ant_runtime_update(rt, 1.f / 60.f);   /* let layer ramps settle */
        const int bones = bf6_ant_runtime_pose(rt, nullptr, 0);
        std::printf("bones %d\n", bones);
        const auto p0 = pose_at(rt, 0.0f, bones);
        const auto p5 = pose_at(rt, 0.5f, bones);
        const auto p1 = pose_at(rt, 1.0f, bones);
        const auto p5b = pose_at(rt, 0.5f, bones);
        double worst = 0;
        for (int i = 0; i < bones; ++i) {
            const float* m = &p5[(size_t)i * 12];
            for (int r = 0; r < 3; ++r)
                worst = std::fmax(worst, std::fabs(std::sqrt((double)m[r*3]*m[r*3] + (double)m[r*3+1]*m[r*3+1] + (double)m[r*3+2]*m[r*3+2]) - 1.0));
        }
        std::printf("phase 0 vs 0.5: %.4f   0.5 vs 1: %.4f   0.5 vs 0.5 again: %.6f   worst row |len-1| %.6f\n",
                    diff(p0, p5), diff(p5, p1), diff(p5, p5b), worst);
        if (worst > 1e-3) bad("rotations not unit");
        if (diff(p0, p5) < 1e-3 || diff(p5, p1) < 1e-3) bad("the pose does not follow the roll phase");
        if (diff(p5, p5b) > 1e-3) bad("the same phase twice gave different poses");
        std::vector<char> notes((size_t)bf6_ant_runtime_notes(rt, nullptr, 0) + 1);
        bf6_ant_runtime_notes(rt, notes.data(), (int)notes.size());
        std::printf("notes:\n%s", notes.data());
        bf6_free(c, rt);
        rc = fails ? 1 : 0;
    }
    {
        /* THE DRAG. Parameters come from the installed prefab; the decode says
         * start 0.15, modifiers 0.005 / 1 (mouse), base 0.15, max 0.25, and a
         * yaw ClampMin pin with no link. */
        bf6_inspect_input_params ip{};
        if (!bf6_inspect_input_params_read(c, &ip, err, (int)sizeof(err))) { std::printf("  drag params: %s\n", err); bad("inspect input params did not read"); }
        else {
            std::printf("drag params: start %.3f mod %.3f mouse %.3f base %.3f max %.3f min %.3f (linked %d)\n",
                        ip.accumulated_at_start, ip.value_modifier, ip.value_modifier_mouse, ip.base,
                        ip.clamp_max, ip.clamp_min, ip.clamp_min_linked);
            if (std::fabs(ip.accumulated_at_start - 0.15f) > 1e-6f || std::fabs(ip.base - 0.15f) > 1e-6f ||
                std::fabs(ip.clamp_max - 0.25f) > 1e-6f || std::fabs(ip.value_modifier - 0.005f) > 1e-6f ||
                std::fabs(ip.value_modifier_mouse - 1.f) > 1e-6f)
                bad("drag parameters differ from the decoded prefab values");
            bf6_inspect_input_state s{};
            const float y0 = bf6_inspect_input_step(&ip, &s, 1, 0.f, 1);
            const float y1 = bf6_inspect_input_step(&ip, &s, 1, 0.05f, 1);     /* right */
            for (int i = 0; i < 20; ++i) bf6_inspect_input_step(&ip, &s, 1, 0.05f, 1);
            const float ymax = s.yaw;
            const float yback = bf6_inspect_input_step(&ip, &s, 1, -0.05f, 1);  /* straight back */
            std::printf("drag: start %.3f, +0.05 -> %.3f, held right -> %.3f, one step back -> %.3f\n", y0, y1, ymax, yback);
            if (std::fabs(y0 - 0.15f) > 1e-6f) bad("inspect does not start at 0.15");
            if (!(y1 > 0.12f)) bad("a right drag does not cross the 0.12 flip threshold");
            if (std::fabs(ymax - 0.25f) > 1e-6f) bad("held right does not stop at the clamp");
            if (!(yback < ymax - 0.04f)) bad("reversing wound up against the clamp");
        }
    }
    std::printf("%s\n", fails ? "FAILED" : "PASS");
    bf6_close(c);
    return rc;
}
