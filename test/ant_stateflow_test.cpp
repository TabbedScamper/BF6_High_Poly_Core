/* THE WEAPON INSPECT, END TO END: the state machine, the drag and the roll.
 *
 *   ant_stateflow_test <game>
 *
 * Runs animations/glacier/controllers/1p.weaponinspect.sf as an M4A1
 * (fb.wep.specificweapon = 46, fb.weapontype = 0) at 60 Hz, driven the way the
 * game drives it:
 *   inspect key  -> fb.weaponinspecttoggle, a one-update pulse;
 *   camera yaw   -> bf6_inspect_input_step -> fb.camerainput.yaw.float;
 *   yaw          -> bf6_inspect_aim_step   -> 1p.aimleftright.phased.float.
 *
 * Checks:
 *   - the roll's expression constants read from the installed program;
 *   - the state sequence: enter -> r (at the enter clip's exit window), a left
 *     drag -> rtol -> l, a right drag -> ltor -> r, a second press -> exit.r ->
 *     the transparent node, and the graph reports finished;
 *   - the enter node's tag writes wep.weaponinspectionactive;
 *   - rotations stay unit length and the pose changes while inspecting.
 */
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {
int fails = 0;
void bad(const char* w) { std::printf("  FAIL %s\n", w); ++fails; }
constexpr const char* kRoot   = "animations/glacier/controllers/1p.weaponinspect.sf";
constexpr const char* kRig    = "animations/glacier/global/rigging/soldier_1p.rig";
constexpr const char* kSke    = "common/characters/_soldier/ske_soldier_1p";
constexpr const char* kWeapon = "animations/glacier/global/gamestates/fb.wep.specificweapon.enumgs";
constexpr const char* kType   = "animations/common/fb.weapontype.enumgs";
constexpr const char* kToggle = "animations/common/fb.weaponinspecttoggle.bool";
constexpr const char* kYaw    = "animations/glacier/global/gamestates/fb.camerainput.yaw.float";
constexpr const char* kAim    = "animations/glacier/global/gamestates/1p.aimleftright.phased.float";
constexpr const char* kActive = "animations/glacier/global/gamestates/wep.weaponinspectionactive.bool";

std::string node_of(bf6_ant_runtime* rt)
{
    char b[512] = {0};
    bf6_ant_runtime_node(rt, b, (int)sizeof(b));
    std::string s(b);
    const size_t k = s.rfind('/');
    return k == std::string::npos ? s : s.substr(k + 1);
}
}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) { std::printf("usage: ant_stateflow_test <game>\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_all(c, 1, err, (int)sizeof(err));

    bf6_inspect_aim_params ap{};
    if (!bf6_inspect_aim_params_read(c, &ap, err, (int)sizeof(err))) { std::printf("aim params: %s\n", err); bad("roll expression not read"); }
    std::printf("aim: yaw x %.3f clamp [%.3f, %.3f] spring k %.1f d %.3f step %.3f range [%.1f, %.1f] reset %d wrap %d\n",
                ap.scale, ap.clamp_lo, ap.clamp_hi, ap.stiffness, ap.damping, ap.max_step, ap.lo, ap.hi, ap.reset_input, ap.wrap);
    /* the values the research verified bit-exact against the game's VM */
    if (ap.scale != 5.f || ap.clamp_lo != 0.05f || ap.clamp_hi != 0.95f || ap.stiffness != 400.f ||
        ap.damping != 0.35f || ap.max_step != 0.12f || ap.lo != 0.f || ap.hi != 1.f)
        bad("roll constants differ from the verified program");
    {
        bf6_inspect_aim_state s{};
        float v = 0.f;
        for (int i = 0; i < 60; ++i) v = bf6_inspect_aim_step(&ap, &s, 0.15f, 1.f);
        std::printf("aim at yaw 0.15 settles to %.4f\n", v);
        if (std::fabs(v - 0.75f) > 1e-4f) bad("aim does not settle at 0.75 for yaw 0.15");
    }

    bf6_inspect_input_params ip{};
    if (!bf6_inspect_input_params_read(c, &ip, err, (int)sizeof(err))) { std::printf("drag params: %s\n", err); bad("drag params"); }

    /* Both guns get the same run: the M4A1 (rifle) and the M18 (pistol), whose
     * enumerators in wep.specificweapon.enum are 46 and 54. The state machine
     * is the same for every weapon; what changes is every lookup under it. */
    for (const auto& gun : { std::pair<const char*, std::pair<int, int>>{"M4A1", {46, 0}},
                             std::pair<const char*, std::pair<int, int>>{"M18", {54, 5}} }) {
    std::printf("== %s (specificweapon %d, type %d)\n", gun.first, gun.second.first, gun.second.second);
    bf6_ant_runtime* rt = bf6_ant_runtime_create(c, kRoot, kRig, kSke, err, (int)sizeof(err));
    if (!rt) { std::printf("create: %s\n", err); bf6_close(c); return 1; }
    bf6_ant_runtime_set_int(rt, kWeapon, gun.second.first);
    bf6_ant_runtime_set_int(rt, kType, gun.second.second);
    const int bones = bf6_ant_runtime_pose(rt, nullptr, 0);
    std::vector<float> pose((size_t)bones * 12), first;

    bf6_inspect_input_state is{};
    bf6_inspect_aim_state as{};
    std::vector<std::pair<int, std::string>> seq;
    std::string cur;
    int tick = 0;
    double worst = 0, moved = 0;
    auto step = [&](float raw, bool press) {
        bf6_ant_runtime_set_bool(rt, kToggle, press ? 1 : 0);
        const float yaw = bf6_inspect_input_step(&ip, &is, 1, raw, 1);
        bf6_ant_runtime_set_float(rt, kYaw, yaw);
        bf6_ant_runtime_set_float(rt, kAim, bf6_inspect_aim_step(&ap, &as, yaw, 1.f));
        bf6_ant_runtime_update(rt, 1.f / 60.f);
        ++tick;
        const std::string n = node_of(rt);
        if (n != cur) { seq.emplace_back(tick, n); cur = n; std::printf("  tick %4d  yaw %.3f  -> %s\n", tick, yaw, n.c_str()); }
        bf6_ant_runtime_pose(rt, pose.data(), bones);
        if (first.empty()) first = pose;
        for (int i = 0; i < bones; ++i)
            for (int r = 0; r < 3; ++r) {
                const float* m = &pose[(size_t)i * 12 + (size_t)r * 3];
                worst = std::fmax(worst, std::fabs(std::sqrt((double)m[0] * m[0] + (double)m[1] * m[1] + (double)m[2] * m[2]) - 1.0));
            }
        for (size_t k = 0; k < pose.size(); ++k) moved = std::fmax(moved, std::fabs(pose[k] - first[k]));
    };
    auto reached = [&](const char* n) { for (const auto& s : seq) if (s.second == n) return true; return false; };

    cur = node_of(rt);
    std::printf("start node %s\n", cur.c_str());
    step(0.f, true);                                   /* the press */
    int ok = 0; const int act = bf6_ant_runtime_get_bool(rt, kActive, &ok);
    std::printf("after the first update: weaponinspectionactive %d (ok %d)\n", act, ok);
    if (!act) bad("the enter node's tag did not set wep.weaponinspectionactive");
    for (int i = 0; i < 90; ++i) step(0.f, false);     /* enter plays out, hands to r */
    if (!reached("1p.inspect.r.node")) bad("enter did not hand off to the right-side node");
    for (int i = 0; i < 120; ++i) step(-0.02f, false); /* drag left */
    /* A PISTOL DOES NOT ROLL TO THE LEFT SIDE, and that is the graph's own
     * rule, not this test's: the r node watches an EnumerationEnumeratorPair on
     * fb.weapontype and the r -> rtol transition needs it FALSE. The sweep
     * below asks the graph which type it names, and only type 5 (Pistol)
     * blocks the roll. */
    const bool rolls = gun.second.second != 5;
    if (reached("1p.inspect.rtol.node") != rolls)
        bad(rolls ? "left drag did not start the r->l roll" : "a pistol rolled to the left side");
    if (rolls && !reached("1p.inspect.l.node")) bad("the r->l roll did not reach the left-side node");
    for (int i = 0; i < 120; ++i) step(0.02f, false);  /* drag right */
    if (rolls && !reached("1p.inspect.ltor.node")) bad("right drag did not start the l->r roll");
    step(0.f, true);                                   /* second press */
    for (int i = 0; i < 200; ++i) step(0.f, false);
    if (!reached("1p.exit.inspect.r.node") && !reached("1p.exit.inspect.l.node")) bad("second press did not exit");
    if (!reached("1p.inspect.transparent.node")) bad("exit did not reach the transparent node");
    const int fin = bf6_ant_runtime_finished(rt);
    std::printf("finished %d   worst row |len-1| %.6f   max pose change %.4f\n", fin, worst, moved);
    if (!fin) bad("the graph did not report finished");
    if (worst > 1e-3) bad("rotations not unit");
    if (moved < 1e-2) bad("the pose never moved");

    std::vector<char> notes((size_t)bf6_ant_runtime_notes(rt, nullptr, 0) + 1);
    bf6_ant_runtime_notes(rt, notes.data(), (int)notes.size());
    std::printf("notes:\n%s", notes.data());
    bf6_free(c, rt);
    }

    {
        /* WHICH WEAPON TYPES ROLL TO THE LEFT SIDE. The r node's third watched
         * condition is an EnumerationEnumeratorPair on fb.weapontype, and the
         * r -> rtol transition requires it FALSE, so one type cannot roll. This
         * asks the graph which, rather than assuming it. */
        std::printf("left-side roll by weapon type:");
        for (int t = 0; t <= 9; ++t) {
            bf6_ant_runtime* r = bf6_ant_runtime_create(c, kRoot, kRig, kSke, err, (int)sizeof(err));
            if (!r) break;
            bf6_ant_runtime_set_int(r, kWeapon, 46);
            bf6_ant_runtime_set_int(r, kType, t);
            bf6_inspect_input_state s{};
            bf6_inspect_aim_state a{};
            bool rolled = false;
            for (int i = 0; i < 220; ++i) {
                bf6_ant_runtime_set_bool(r, kToggle, i == 0 ? 1 : 0);
                const float y = bf6_inspect_input_step(&ip, &s, 1, i < 90 ? 0.f : -0.02f, 1);
                bf6_ant_runtime_set_float(r, kYaw, y);
                bf6_ant_runtime_set_float(r, kAim, bf6_inspect_aim_step(&ap, &a, y, 1.f));
                bf6_ant_runtime_update(r, 1.f / 60.f);
                char nb[512] = {0};
                bf6_ant_runtime_node(r, nb, (int)sizeof(nb));
                if (std::string(nb).find("rtol") != std::string::npos) { rolled = true; break; }
            }
            std::printf(" %d:%s", t, rolled ? "yes" : "NO");
            if (rolled == (t == 5)) bad("the type that blocks the left-side roll is not Pistol alone");
            bf6_free(c, r);
        }
        std::printf("\n");
    }

    {
        /* The ids come out of the weapon's own data, not out of this test. */
        int32_t sw = -1, wt = -1;
        if (!bf6_inspect_ids(c, "common/hardware/weapons/carbine/m4a1", &sw, &wt, err, (int)sizeof(err)))
            { std::printf("  ids: %s\n", err); bad("the M4A1's animation ids did not read"); }
        std::printf("m4a1 ids: specificweapon %d, weapontype %d\n", sw, wt);
        if (sw != 46) bad("the M4A1's specific-weapon value is not its enumerator (46)");
        if (wt != 0) bad("the M4A1's weapon type is not Rifle (0)");
        /* The type comes from the base-set conversion table, so a pistol and a
         * light machine gun have to land on their own rows. */
        const struct { const char* item; int32_t sw; int32_t wt; } more[] = {
            { "m18", 54, 5 }, { "minimi", 49, 3 } };
        for (const auto& m : more) {
            int32_t s = -1, t = -1;
            if (!bf6_inspect_ids(c, m.item, &s, &t, err, (int)sizeof(err)))
                { std::printf("  %s ids: %s\n", m.item, err); bad("a weapon's animation ids did not read"); continue; }
            std::printf("%s ids: specificweapon %d, weapontype %d\n", m.item, s, t);
            if (s != m.sw || t != m.wt) bad("a weapon's animation ids differ from its data");
        }
    }
    std::printf("%s\n", fails ? "FAILED" : "PASS");
    bf6_close(c);
    return fails ? 1 : 0;
}
