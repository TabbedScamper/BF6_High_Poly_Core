/* locostance_runtime_probe - WHAT DOES 1p.locostance.slc ACTUALLY PRODUCE?
 *
 *   locostance_runtime_probe <game> [item] [stance-asset]
 *
 * EXPLORATORY, NOT PASS/FAIL. The first-person view currently draws one
 * authored stance plus a breathing additive, with no locomotion, aim or
 * transition layers. The game stands in 1p.locostance.slc instead. Before
 * replacing a working path with "more game-derived" output, the question is
 * whether the runtime can run that graph at all and what it produces: which
 * nodes and assets it selects, what it says it cannot do, which DOFs it
 * actually writes, and how far its stationary pose sits from the stance we
 * already draw.
 *
 * Nothing here asserts. A graph that runs with notes still runs; the notes say
 * where it differs from the game, and that difference is the report.
 *
 * Built by hand rather than through CMakeLists, because the core's
 * CMakeLists.txt currently carries unrelated uncommitted work:
 *
 *   cl /nologo /std:c++17 /EHsc /O2 /I core\include test\locostance_runtime_probe.cpp
 *      /Fe:probe.exe /link <build>\Release\bf6_core.lib
 */
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>

namespace {

constexpr const char* kRig = "animations/glacier/global/rigging/soldier_1p.rig";
constexpr const char* kSke = "common/characters/_soldier/ske_soldier_1p";
constexpr const char* kStance = "animations/kingston/controllers/1p.locostance.slc";
constexpr const char* kWeaponPose = "animations/kingston/controllers/1p.weaponpose.cdb";

struct M { float m[12]; };

/* Row-vector convention, matching bf6_bone.local: rows 0..8 are the 3x3, 9..11
 * the translation, and a child composes as child_local * parent_model. */
M mul(const M& a, const M& b)
{
    M r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i * 3 + j] = a.m[i * 3 + 0] * b.m[0 * 3 + j]
                           + a.m[i * 3 + 1] * b.m[1 * 3 + j]
                           + a.m[i * 3 + 2] * b.m[2 * 3 + j];
    for (int j = 0; j < 3; ++j)
        r.m[9 + j] = a.m[9 + 0] * b.m[0 * 3 + j]
                   + a.m[9 + 1] * b.m[1 * 3 + j]
                   + a.m[9 + 2] * b.m[2 * 3 + j] + b.m[9 + j];
    return r;
}

double angle_deg(const float* a, const float* b)
{
    double tr = 0;
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) tr += (double)a[r * 3 + k] * b[r * 3 + k];
    double c = (tr - 1.0) * 0.5;
    c = c > 1 ? 1 : (c < -1 ? -1 : c);
    return std::acos(c) * 57.29577951308232;
}

/* One graph, run to `seconds`, with the weapon equipped the way the game does.
 * Undriven slots are left at the bind pose, which is what the runtime documents
 * and what a caller must respect. */
struct Run {
    std::vector<float> pose;
    std::vector<uint8_t> vrot, vtr;
    std::string node, notes;
    int bones = 0;
    bool ok = false;
};

/* A game state to drive. Names are READ from the installed EBX path table
 * (Animations/Glacier/Global/Gamestates/...), lowercased the way every other
 * asset path here is, rather than invented: a guessed state silently does
 * nothing and looks exactly like a graph that ignores locomotion. */
struct SetState {
    const char* asset;
    char kind;      /* f float, b bool, i int */
    float value;
};

Run run_graph(bf6_ctx* c, const char* asset, int32_t sw, int32_t wt, float seconds,
              const std::vector<SetState>& states = {})
{
    Run r;
    char err[512] = {0};
    bf6_ant_runtime* rt = bf6_ant_runtime_create(c, asset, kRig, kSke, err, (int)sizeof(err));
    if (!rt) {
        r.notes = std::string("could not create: ") + err;
        return r;
    }
    bf6_ant_runtime_set_weapon(rt, sw, wt);
    for (const SetState& s : states) {
        if (s.kind == 'f') bf6_ant_runtime_set_float(rt, s.asset, s.value);
        else if (s.kind == 'b') bf6_ant_runtime_set_bool(rt, s.asset, (int)s.value);
        else if (s.kind == 'i') bf6_ant_runtime_set_int(rt, s.asset, (int32_t)s.value);
    }
    /* The graph ticks internally at 1/60; step rather than jump so any state
     * machine gets the transitions it would really see. */
    if (seconds <= 0.f) {
        bf6_ant_runtime_update(rt, 0.f);
    } else {
        for (float t = 0.f; t < seconds; t += 1.f / 60.f)
            bf6_ant_runtime_update(rt, 1.f / 60.f);
    }
    r.bones = bf6_ant_runtime_pose(rt, nullptr, 0);
    r.pose.assign((size_t)r.bones * 12, 0.f);
    r.vrot.assign((size_t)r.bones, 0);
    r.vtr.assign((size_t)r.bones, 0);
    bf6_ant_runtime_pose(rt, r.pose.data(), r.bones);
    bf6_ant_runtime_pose_valid(rt, r.vrot.data(), r.vtr.data(), r.bones);
    std::vector<char> nb((size_t)bf6_ant_runtime_notes(rt, nullptr, 0) + 1, 0);
    bf6_ant_runtime_notes(rt, nb.data(), (int)nb.size());
    r.notes = nb.data();
    std::vector<char> node((size_t)bf6_ant_runtime_node(rt, nullptr, 0) + 1, 0);
    bf6_ant_runtime_node(rt, node.data(), (int)node.size());
    r.node = node.data();
    r.ok = true;
    bf6_free(c, rt);
    return r;
}

/* Model-space transforms from a parent-relative pose, bind pose where the
 * graph wrote nothing. Bones are topologically ordered, so one pass suffices. */
std::vector<M> model_of(const bf6_skeleton* s, const Run& r)
{
    std::vector<M> out((size_t)s->bone_count);
    for (int i = 0; i < s->bone_count; ++i) {
        M local{};
        const bool have = i < r.bones && (r.vrot[(size_t)i] || r.vtr[(size_t)i]);
        std::memcpy(local.m, have ? &r.pose[(size_t)i * 12] : s->bones[i].local, sizeof(local.m));
        const int p = s->bones[i].parent;
        out[(size_t)i] = (p < 0) ? local : mul(local, out[(size_t)p]);
    }
    return out;
}

int bone_of(const bf6_skeleton* s, const char* name)
{
    for (int i = 0; i < s->bone_count; ++i)
        if (s->bones[i].name && std::strcmp(s->bones[i].name, name) == 0) return i;
    return -1;
}

double dist_mm(const std::vector<M>& g, int a, int b)
{
    if (a < 0 || b < 0) return -1.0;
    double d = 0;
    for (int k = 0; k < 3; ++k) {
        const double v = (double)g[(size_t)a].m[9 + k] - (double)g[(size_t)b].m[9 + k];
        d += v * v;
    }
    return std::sqrt(d) * 1000.0;
}

void report_grip(const bf6_skeleton* s, const std::vector<M>& g, const char* label)
{
    const int rh = bone_of(s, "RightHand"), lh = bone_of(s, "LeftHand");
    const int rk = bone_of(s, "Wep_IK_RightHand"), lk = bone_of(s, "Wep_IK_LeftHand");
    std::printf("  %-22s right %8.2f mm   left %8.2f mm   (control hands apart %8.2f mm)\n",
                label, dist_mm(g, rh, rk), dist_mm(g, lh, lk), dist_mm(g, rh, lh));
}

void print_notes(const char* label, const std::string& n)
{
    if (n.empty()) { std::printf("  %s: (none)\n", label); return; }
    std::printf("  %s:\n", label);
    std::string line;
    for (char ch : n) {
        if (ch == '\n') { if (!line.empty()) std::printf("      %s\n", line.c_str()); line.clear(); }
        else line += ch;
    }
    if (!line.empty()) std::printf("      %s\n", line.c_str());
}

}  // namespace

/* EVERY 1P LOCOMOTION GAME STATE THE INSTALL DECLARES, read out of the EBX path
 * table (Animations/Glacier/Global/Gamestates/1P.Loco.*) and lowercased. Listed
 * in full rather than filtered to the ones that look promising: a graph that
 * ignores the state you guessed looks exactly like a graph with no locomotion,
 * and the only way to tell those apart is to try all of them. */
const char* const kLocoStates[] = {
    "animations/glacier/global/gamestates/1p.loco.additivetransition.weight.float",
    "animations/glacier/global/gamestates/1p.loco.altspeedmagnitude.float",
    "animations/glacier/global/gamestates/1p.loco.blendoffspinex.float",
    "animations/glacier/global/gamestates/1p.loco.blendoffspinexpitch.bool",
    "animations/glacier/global/gamestates/1p.loco.blendoffspinexpitch.float",
    "animations/glacier/global/gamestates/1p.loco.changedmovedirection.bool",
    "animations/glacier/global/gamestates/1p.loco.crawl.disable.lefthand.chooserbool",
    "animations/glacier/global/gamestates/1p.loco.desiredspeed.bool",
    "animations/glacier/global/gamestates/1p.loco.enable.transparentnode.chooserbool",
    "animations/glacier/global/gamestates/1p.loco.freefall.bool",
    "animations/glacier/global/gamestates/1p.loco.incrawl.bool",
    "animations/glacier/global/gamestates/1p.loco.incrawlpause.bool",
    "animations/glacier/global/gamestates/1p.loco.inslide.bool",
    "animations/glacier/global/gamestates/1p.loco.interruptcrawl.bool",
    "animations/glacier/global/gamestates/1p.loco.isbackpeddling.bool",
    "animations/glacier/global/gamestates/1p.loco.movebackwards.bool",
    "animations/glacier/global/gamestates/1p.loco.movedirection.enum",
    "animations/glacier/global/gamestates/1p.loco.movedirection.enumgs",
    "animations/glacier/global/gamestates/1p.loco.nolegloco.bool",
    "animations/glacier/global/gamestates/1p.loco.notinprone.bool",
    "animations/glacier/global/gamestates/1p.loco.proneonback.legsforward.bool",
    "animations/glacier/global/gamestates/1p.loco.pronespeedmagnitude.float",
    "animations/glacier/global/gamestates/1p.loco.pronespinexlogicenabled.bool",
    "animations/glacier/global/gamestates/1p.loco.run.ads.enabled.chooserbool",
    "animations/glacier/global/gamestates/1p.loco.runspeedmagnitude.float",
    "animations/glacier/global/gamestates/1p.loco.skip.sprint.enter.chooserbool",
    "animations/glacier/global/gamestates/1p.loco.spinex.enabled.bool",
    "animations/glacier/global/gamestates/1p.loco.sprint.camera.weight.float",
    "animations/glacier/global/gamestates/1p.loco.sprint.override.conditions.chooserbool",
    "animations/glacier/global/gamestates/1p.loco.sprint.spam.timeout.bool",
    "animations/glacier/global/gamestates/1p.loco.sprintspeed.bool",
    "animations/glacier/global/gamestates/1p.loco.sprintspeedmagnitude.float",
    "animations/glacier/global/gamestates/1p.loco.sprintstate.enum",
    "animations/glacier/global/gamestates/1p.loco.sprintstate.enumgs",
    "animations/glacier/global/gamestates/1p.loco.strafe.dampened.float",
    "animations/glacier/global/gamestates/1p.loco.switchpronestance.bool",
    "animations/glacier/global/gamestates/1p.loco.throttle.dampened.float",
};

char kind_of(const char* asset)
{
    const char* dot = std::strrchr(asset, '.');
    if (!dot) return 'f';
    if (std::strcmp(dot, ".bool") == 0 || std::strstr(dot, "chooserbool")) return 'b';
    if (std::strstr(dot, ".enum")) return 'i';
    return 'f';
}

/* Set each state ON ITS OWN and report what moved. One at a time, because a
 * bundle that moves the arms does not say WHICH member did it. */
int sweep(bf6_ctx* c, const bf6_skeleton* s, const char* asset, int32_t sw, int32_t wt,
          const char* list_file)
{
    /* A state list from a file beats the built-in one: the interesting set
     * changes with the question being asked, and a rebuild per question is a
     * poor way to run an experiment. */
    std::vector<std::string> from_file;
    if (list_file) {
        if (FILE* f = std::fopen(list_file, "rb")) {
            char line[512];
            while (std::fgets(line, (int)sizeof(line), f)) {
                std::string t(line);
                while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
                if (!t.empty()) from_file.push_back(t);
            }
            std::fclose(f);
        }
        std::printf("  state list: %s (%d states)\n", list_file, (int)from_file.size());
    }
    std::printf("=== state sweep on %s\n", asset);
    Run base = run_graph(c, asset, sw, wt, 1.0f);
    if (!base.ok) { std::printf("  did not run: %s\n", base.notes.c_str()); return 1; }
    print_notes("notes (verbatim)", base.notes);
    std::printf("  baseline node: %s\n", base.node.empty() ? "(not a state machine)" : base.node.c_str());
    std::printf("\n  %-62s %10s %6s  %s\n", "state", "worst(deg)", ">1deg", "worst bone");
    int moved_any = 0;
    const int n = from_file.empty() ? (int)(sizeof(kLocoStates) / sizeof(kLocoStates[0]))
                                    : (int)from_file.size();
    for (int k = 0; k < n; ++k) {
        const char* name = from_file.empty() ? kLocoStates[k] : from_file[(size_t)k].c_str();
        const char kind = kind_of(name);
        Run r = run_graph(c, asset, sw, wt, 1.0f, {{name, kind, 1.0f}});
        if (!r.ok) continue;
        double worst = 0; int over = 0; std::string wb;
        for (int i = 0; i < s->bone_count && i < base.bones && i < r.bones; ++i) {
            if (!base.vrot[(size_t)i] || !r.vrot[(size_t)i]) continue;
            const double a = angle_deg(&base.pose[(size_t)i * 12], &r.pose[(size_t)i * 12]);
            if (a > 1.0) ++over;
            if (a > worst) { worst = a; wb = s->bones[i].name ? s->bones[i].name : ""; }
        }
        /* A STATE MACHINE ANSWERS WITH ITS NODE, not its pose. The locomotion
         * layer is transparent while idle, so comparing poses reports nothing
         * even when a state really did drive a transition. */
        if (r.node != base.node) {
            ++moved_any;
            /* A node change is only useful if the layer then WRITES. */
            int wrote = 0;
            for (int i = 0; i < r.bones; ++i) wrote += r.vrot[(size_t)i] ? 1 : 0;
            const char* lf = std::strrchr(name, '/');
            std::printf("  %-52s wrote %3d  NODE -> %s\n", lf ? lf + 1 : name, wrote, r.node.c_str());
            continue;
        }
        if (worst > 0.1) {
            ++moved_any;
            const char* leaf = std::strrchr(kLocoStates[k], '/');
            std::printf("  %-62s %10.2f %6d  %s\n", leaf ? leaf + 1 : kLocoStates[k],
                        worst, over, wb.c_str());
        }
    }
    std::printf("\n  %d of %d states changed the pose by more than 0.1 deg\n", moved_any, n);
    return 0;
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::printf("usage: locostance_runtime_probe <game> [item] [stance-asset] [sweep]\n");
        return 2;
    }
    const char* item = argc > 2 ? argv[2] : "carbine/m4a1";
    const char* stance_asset = argc > 3 ? argv[3] : kStance;

    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_all(c, 1, err, (int)sizeof(err));

    int32_t sw = -1, wt = -1;
    if (!bf6_inspect_ids(c, item, &sw, &wt, err, (int)sizeof(err))) {
        std::printf("ids for %s: %s\n", item, err);
        bf6_close(c);
        return 1;
    }
    std::printf("item %s: specificweapon %d, weapontype %d\n", item, sw, wt);

    /* What the weapon-pose lookup resolves to, which is the stance we already
     * draw and therefore the thing to compare against. */
    std::vector<char> ref((size_t)bf6_ant_resolve_for_weapon(c, kWeaponPose, sw, wt, nullptr, 0) + 1, 0);
    bf6_ant_resolve_for_weapon(c, kWeaponPose, sw, wt, ref.data(), (int)ref.size());
    std::printf("weaponpose resolves to: %s\n", ref[0] ? ref.data() : "(nothing)");
    std::printf("stance asset under test: %s\n\n", stance_asset);

    const bf6_skeleton* s = bf6_skeleton_compose(c, kSke, nullptr);
    if (!s) { std::printf("could not compose %s\n", kSke); bf6_close(c); return 1; }
    std::printf("skeleton: %d bones (%d from the rig)\n\n", s->bone_count, s->rig_bone_count);

    if (argc > 4 && std::strcmp(argv[4], "sweep") == 0) {
        const int rc = sweep(c, s, stance_asset, sw, wt, argc > 5 ? argv[5] : nullptr);
        bf6_free(c, (void*)s);
        bf6_close(c);
        return rc;
    }

    std::printf("=== 1. the locostance graph at rest\n");
    Run loco = run_graph(c, stance_asset, sw, wt, 0.f);
    if (!loco.ok) {
        std::printf("  DID NOT RUN: %s\n", loco.notes.c_str());
    } else {
        int dr = 0, dt = 0;
        for (int i = 0; i < loco.bones; ++i) { dr += loco.vrot[(size_t)i] ? 1 : 0; dt += loco.vtr[(size_t)i] ? 1 : 0; }
        std::printf("  bones %d, rotations written %d, translations written %d\n", loco.bones, dr, dt);
        std::printf("  current node: %s\n", loco.node.empty() ? "(root is not a state machine)" : loco.node.c_str());
        print_notes("notes", loco.notes);
    }
    std::printf("\n");

    std::printf("=== 2. the stance we already draw\n");
    Run refrun = run_graph(c, ref[0] ? ref.data() : "", sw, wt, 0.f);
    if (!refrun.ok) {
        std::printf("  DID NOT RUN: %s\n", refrun.notes.c_str());
    } else {
        int dr = 0, dt = 0;
        for (int i = 0; i < refrun.bones; ++i) { dr += refrun.vrot[(size_t)i] ? 1 : 0; dt += refrun.vtr[(size_t)i] ? 1 : 0; }
        std::printf("  bones %d, rotations written %d, translations written %d\n", refrun.bones, dr, dt);
        print_notes("notes", refrun.notes);
    }
    std::printf("\n");

    if (loco.ok && refrun.ok) {
        std::printf("=== 3. stationary difference, locostance vs the drawn stance\n");
        auto gl = model_of(s, loco);
        auto gr = model_of(s, refrun);
        report_grip(s, gr, "drawn stance grip");
        report_grip(s, gl, "locostance grip");
        std::printf("\n  per-bone, where BOTH graphs wrote a rotation:\n");
        std::printf("  %-26s %9s %12s\n", "bone", "angle(deg)", "translate(mm)");
        int both = 0;
        double worst = 0; std::string worst_bone;
        for (int i = 0; i < s->bone_count && i < loco.bones && i < refrun.bones; ++i) {
            if (!loco.vrot[(size_t)i] || !refrun.vrot[(size_t)i]) continue;
            ++both;
            const double a = angle_deg(&loco.pose[(size_t)i * 12], &refrun.pose[(size_t)i * 12]);
            double t = 0;
            for (int k = 0; k < 3; ++k) {
                const double v = (double)loco.pose[(size_t)i * 12 + 9 + k]
                               - (double)refrun.pose[(size_t)i * 12 + 9 + k];
                t += v * v;
            }
            t = std::sqrt(t) * 1000.0;
            const char* nm = s->bones[i].name ? s->bones[i].name : "";
            if (a > worst) { worst = a; worst_bone = nm; }
            if (a >= 1.0 || t >= 1.0)
                std::printf("  %-26s %9.2f %12.2f\n", nm, a, t);
        }
        std::printf("\n  %d bones written by both; worst rotation %.2f deg on %s\n",
                    both, worst, worst_bone.c_str());
    }
    std::printf("\n");

    std::printf("=== 4. does the graph move on its own over 2 s?\n");
    if (loco.ok) {
        Run later = run_graph(c, stance_asset, sw, wt, 2.0f);
        if (later.ok) {
            double worst = 0; std::string worst_bone;
            for (int i = 0; i < s->bone_count && i < loco.bones && i < later.bones; ++i) {
                if (!loco.vrot[(size_t)i] || !later.vrot[(size_t)i]) continue;
                const double a = angle_deg(&loco.pose[(size_t)i * 12], &later.pose[(size_t)i * 12]);
                if (a > worst) { worst = a; worst_bone = s->bones[i].name ? s->bones[i].name : ""; }
            }
            std::printf("  largest rotation change after 2 s: %.3f deg on %s\n",
                        worst, worst_bone.c_str());
            std::printf("  node after 2 s: %s\n",
                        later.node.empty() ? "(root is not a state machine)" : later.node.c_str());
            if (later.notes != loco.notes) print_notes("notes after 2 s", later.notes);
        }
    }

    std::printf("\n=== 5. under game-authored locomotion states\n");
    if (loco.ok) {
        const char* kRunSpeed = "animations/glacier/global/gamestates/1p.loco.runspeedmagnitude.float";
        const char* kAltSpeed = "animations/glacier/global/gamestates/1p.loco.altspeedmagnitude.float";
        const char* kBackwards = "animations/glacier/global/gamestates/1p.loco.movebackwards.bool";
        const char* kInSlide = "animations/glacier/global/gamestates/1p.loco.inslide.bool";
        struct Case { const char* label; std::vector<SetState> states; };
        const std::vector<Case> cases = {
            {"run speed 1.0",        {{kRunSpeed, 'f', 1.0f}}},
            {"run speed 0.5",        {{kRunSpeed, 'f', 0.5f}}},
            {"alt speed 1.0",        {{kAltSpeed, 'f', 1.0f}}},
            {"moving backwards",     {{kRunSpeed, 'f', 1.0f}, {kBackwards, 'b', 1.f}}},
            {"in slide",             {{kInSlide, 'b', 1.f}}},
        };
        std::printf("  %-22s %12s %10s  %s\n", "state", "worst(deg)", "bones>1deg", "worst bone");
        for (const Case& cs : cases) {
            Run rr = run_graph(c, stance_asset, sw, wt, 0.5f, cs.states);
            if (!rr.ok) { std::printf("  %-22s did not run\n", cs.label); continue; }
            double worst = 0; int moved = 0; std::string wb;
            for (int i = 0; i < s->bone_count && i < loco.bones && i < rr.bones; ++i) {
                if (!loco.vrot[(size_t)i] || !rr.vrot[(size_t)i]) continue;
                const double a = angle_deg(&loco.pose[(size_t)i * 12], &rr.pose[(size_t)i * 12]);
                if (a > 1.0) ++moved;
                if (a > worst) { worst = a; wb = s->bones[i].name ? s->bones[i].name : ""; }
            }
            std::printf("  %-22s %12.2f %10d  %s\n", cs.label, worst, moved, wb.c_str());
            if (rr.notes != loco.notes) print_notes("    notes", rr.notes);
        }
        std::printf("\n  A row near 0 means the graph ignored that state: either the name is not\n");
        std::printf("  the one this graph reads, or the layer that consumes it is not in this root.\n");
    }

    bf6_free(c, (void*)s);
    bf6_close(c);
    return 0;
}
