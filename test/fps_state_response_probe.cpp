// WHICH GAME STATES ACTUALLY MOVE THE FIRST-PERSON GRAPH, AND TO WHAT?
//
//   fps_state_response_probe <game> [item]
//
// The install declares 1099 game states. Knowing their names is not knowing
// which ones the 1P stance graph reads, nor - for the enum ones - what integer
// means "crouch". Guessing either is how three earlier attempts in this session
// went wrong, so nothing here is assumed: every candidate is set, the graph is
// updated, and the NODE it lands in is read back.
//
// THE NODE IS THE ANSWER, not the pose. A layer can be transparent, or can be
// posing a bone the weapon hides; either way the pose barely moves and a pose
// comparison reports nothing. The node name says what the state machine chose,
// and it is named after what it is - 1p.standtorun.node, 1p.camera.pronepose -
// so a node change is both the proof the state was read and the label for what
// it did.
//
// Reported in three passes:
//   1. BOOLS  - set true one at a time, from a resting graph.
//   2. FLOATS - driven to 1.0 one at a time.
//   3. ENUMS  - swept 0..5, because the stance is an enum and the value for
//               crouch has to come from the graph's own behaviour.

#include "bf6_core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

constexpr const char* kRig     = "animations/glacier/global/rigging/soldier_1p.rig";
constexpr const char* kSke     = "common/characters/_soldier/ske_soldier_1p";
constexpr const char* kStance  = "animations/kingston/controllers/1p.locostance.slc";
constexpr const char* kGs      = "animations/glacier/global/gamestates/";

// The states this tool needs in order to do what the game does. Named for the
// feature each one belongs to so a partial result is still readable.
static const char* const kBools[] = {
    // sprint
    "1p.insprint.bool", "1p.in.sprintstate.bool", "infastsprint.bool",
    "1p.loco.sprintspeed.bool", "1p.loco.desiredspeed.bool",
    // ads
    "fb.ads.bool", "1p.adsarms.layer.enable.bool", "1p.wep.zoominfinished.bool",
    "1p.toads.blocktransitions.bool",
    // fire and reload
    "fb.inputfireraw.bool", "fb.wep.justfired.bool", "fb.reload.bool",
    "fb.speedreload.bool",
    // stance
    "ant.prone.bool", "loco.stancetransition.toprone.bool",
    "loco.stancetransition.fromprone.bool", "loco.crouchstandtransition.bool",
    "loco.instancetransition.standcrouch.bool", "loco.toorfromcrouch.bool",
    "loco.triggerstancetransition.bool", "loco.stancestatechanged.bool",
    "1p.loco.notinprone.bool", "loco.notinprone.bool",
    // the start/stop layer already proven to work, as a positive control
    "1p.loco.triggerstartanimation.bool", "1p.loco.triggerstopanimation.bool",
    "1p.loco.triggersprintstopanimation.bool",
};

static const char* const kFloats[] = {
    "1p.loco.runspeedmagnitude.float", "1p.loco.sprintspeedmagnitude.float",
    "1p.loco.pronespeedmagnitude.float", "1p.loco.altspeedmagnitude.float",
    "1p.wep.zoompose.weight.float", "1p.adspose.weproot.weight.float",
    "1p.locostance.cdbchange.blend.float", "crouchsettle.float",
    "ant.velocity.forward.float", "ant.velocity.left.float",
};

// Enum game states. `.enumgs` is the state; the `.enumgsitem` siblings name the
// values but not their numbering, which is what the sweep is for.
static const char* const kEnums[] = {
    "ptm.stancestate.current.enumgs", "mm.proneornot.enumgs",
    "1p.loco.movedirection.enum", "1p.proneonback.trans.states.enumgs",
};

static std::string node_of(bf6_ant_runtime* rt)
{
    const int n = bf6_ant_runtime_node(rt, nullptr, 0);
    if (n <= 0) return std::string();
    std::vector<char> b((size_t)n + 1, 0);
    bf6_ant_runtime_node(rt, b.data(), (int)b.size());
    return std::string(b.data());
}

// Settle the graph so a change is attributable to the state just set, not to a
// transition still running from the previous one.
static void settle(bf6_ant_runtime* rt, int ticks = 30)
{
    for (int i = 0; i < ticks; ++i) bf6_ant_runtime_update(rt, 1.0f / 60.0f);
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: %s <game dir> [item]\n", argv[0]); return 2; }
    const char* item = argc > 2 ? argv[2] : "carbine/m4a1";

    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("FAIL: %s\n", err); return 1; }

    int weapon = -1, wtype = -1;
    if (!bf6_inspect_ids(c, item, &weapon, &wtype, err, (int)sizeof(err)))
        std::printf("note: weapon ids unavailable (%s); using 46/0\n", err),
        weapon = 46, wtype = 0;
    std::printf("item %s -> weapon %d type %d\n", item, weapon, wtype);

    bf6_ant_runtime* rt = bf6_ant_runtime_create(c, kStance, kRig, kSke, err, (int)sizeof(err));
    if (!rt) { std::printf("FAIL: the stance graph did not open: %s\n", err); bf6_close(c); return 1; }
    bf6_ant_runtime_set_weapon(rt, weapon, wtype);
    settle(rt, 60);
    const std::string base = node_of(rt);
    std::printf("root  %s\nresting node: %s\n", kStance, base.c_str());
    {
        std::vector<char> nb((size_t)bf6_ant_runtime_notes(rt, nullptr, 0) + 1, 0);
        bf6_ant_runtime_notes(rt, nb.data(), (int)nb.size());
        if (nb[0]) std::printf("notes:\n%s\n", nb.data());
    }
    std::printf("\n");

    // 1. BOOLS
    std::printf("BOOLS THAT MOVE THE GRAPH\n");
    int moved = 0;
    for (const char* nm : kBools) {
        const std::string path = std::string(kGs) + nm;
        bf6_ant_runtime_set_bool(rt, path.c_str(), 1);
        settle(rt);
        const std::string after = node_of(rt);
        if (after != base) {
            ++moved;
            std::printf("  %-46s -> %s\n", nm, after.c_str());
        }
        bf6_ant_runtime_set_bool(rt, path.c_str(), 0);
        settle(rt);
    }
    if (!moved) std::printf("  none of %zu\n", sizeof(kBools) / sizeof(kBools[0]));
    std::printf("\n");

    // 2. FLOATS
    std::printf("FLOATS THAT MOVE THE GRAPH\n");
    moved = 0;
    for (const char* nm : kFloats) {
        const std::string path = std::string(kGs) + nm;
        bf6_ant_runtime_set_float(rt, path.c_str(), 1.0f);
        settle(rt);
        const std::string after = node_of(rt);
        if (after != base) {
            ++moved;
            std::printf("  %-46s -> %s\n", nm, after.c_str());
        }
        bf6_ant_runtime_set_float(rt, path.c_str(), 0.0f);
        settle(rt);
    }
    if (!moved) std::printf("  none of %zu\n", sizeof(kFloats) / sizeof(kFloats[0]));
    std::printf("\n");

    // 3. ENUMS, swept. This is the only way the crouch and prone values can be
    //    learned without reading them off a guess.
    std::printf("ENUM VALUES AND WHAT EACH ONE SELECTS\n");
    for (const char* nm : kEnums) {
        const std::string path = std::string(kGs) + nm;
        std::printf("  %s\n", nm);
        bool any = false;
        for (int v = 0; v <= 5; ++v) {
            bf6_ant_runtime_set_int(rt, path.c_str(), v);
            settle(rt);
            const std::string after = node_of(rt);
            if (after != base) {
                any = true;
                std::printf("      = %d -> %s\n", v, after.c_str());
            }
        }
        if (!any) std::printf("      no value 0..5 changed the node\n");
        bf6_ant_runtime_set_int(rt, path.c_str(), 0);
        settle(rt);
    }

    bf6_free(c, rt);
    bf6_close(c);
    return 0;
}
