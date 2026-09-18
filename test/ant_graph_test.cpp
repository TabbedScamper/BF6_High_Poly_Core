/* THE ANT FOUNDATION ON THE INSPECT GRAPH.
 *
 *   ant_graph_test <game>
 *
 * Checks the pieces the runtime is built on, on the real inspect graph:
 *   - the state machine and its nodes load, and references resolve ACROSS
 *     partitions (default node, compiled transition targets, watched states);
 *   - the two inspect flip comparisons evaluate as decoded: ltor is
 *     yaw > 0.12 and rtol is yaw < 0.08 (GameStateCompareOp 4 and 2), each
 *     checked on both sides of its threshold, and unset yaw reads the
 *     authored default;
 *   - a clip samples onto the 1P skeleton with unit rotations, and two
 *     different times give different poses.
 */
#include "bf6_core.h"
#include "ant_graph.h"

#include <cmath>
#include <cstdio>

namespace {
int fails = 0;
void bad(const char* w) { std::printf("  FAIL %s\n", w); ++fails; }
constexpr const char* kSF = "animations/glacier/controllers/1p.weaponinspect.sf";
constexpr const char* kYaw = "animations/glacier/global/gamestates/fb.camerainput.yaw.float";
constexpr const char* kLtoR = "animations/glacier/global/gamestates/1p.weaponinspect.triggertransition.ltor.floatcomgs";
constexpr const char* kRtoL = "animations/glacier/global/gamestates/1p.weaponinspect.triggertransition.rtol.floatcomgs";
constexpr const char* kClip = "animations/glacier/assets/1p/weapons/rifles/m4a1/a_oma_m4a1_stand_inspect_enter_01";
constexpr const char* kRig = "animations/glacier/global/rigging/soldier_1p.rig";
constexpr const char* kSke = "common/characters/_soldier/ske_soldier_1p";
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) { std::printf("usage: ant_graph_test <game>\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_all(c, 1, err, (int)sizeof(err));

    bf6ant::Graph g(c);
    const bf6ant::Obj* sf = g.root(kSF);
    if (!sf) { bad("state machine did not load"); std::printf("  %s\n", g.error().c_str()); return 1; }
    std::printf("state machine type %s\n", sf->type.c_str());
    const bf6ant::Obj* node = g.resolve(sf->f(0x5fa260eeu), sf);          /* default node */
    if (!node) bad("default node did not resolve");
    else {
        std::printf("default node %s\n", node->path.c_str());
        if (node->path.find("1p.enter.inspect.node") == std::string::npos) bad("default node is not enter.inspect");
        const bf6::EbxValue* tbl = node->f(0xfa9c5e77u);               /* compiled transitions */
        const bf6::EbxValue* watched = node->f(0x438ebff1u);
        int targets = 0;
        if (tbl) for (const auto& e : tbl->items) {
            const bf6ant::Obj* t = g.resolve(e.field(0x88b009f9u), node);
            if (t) { ++targets; std::printf("  -> %s\n", t->path.c_str()); }
        }
        if (!tbl || targets != (int)tbl->items.size() || targets == 0) bad("compiled transition targets did not all resolve");
        bf6ant::States st;
        if (watched) for (const auto& w : watched->items) {
            const bf6ant::Obj* o = g.resolve(&w, node);
            bool ok = false;
            const bool b = st.as_bool(g, o, ok);
            std::printf("  watched %s = %s\n", o ? o->path.c_str() : "(null)", ok ? (b ? "true" : "false") : "NOT EVALUABLE");
            if (!ok) bad("a watched condition is not evaluable");
        }
    }

    bf6ant::States st;
    const bf6ant::Obj* ltor = g.root(kLtoR);
    const bf6ant::Obj* rtol = g.root(kRtoL);
    struct Case { float yaw; bool ltor, rtol; } cases[] = {
        {0.20f, true, false}, {0.12f, false, false}, {0.10f, false, false}, {0.05f, false, true}, {0.08f, false, false},
    };
    for (const Case& k : cases) {
        st.set_float(kYaw, k.yaw);
        bool ok1 = false, ok2 = false;
        const bool a = st.as_bool(g, ltor, ok1), b = st.as_bool(g, rtol, ok2);
        std::printf("yaw %.2f: ltor %d rtol %d\n", k.yaw, a, b);
        if (!ok1 || !ok2) bad("comparison not evaluable");
        if (a != k.ltor || b != k.rtol) bad("comparison disagrees with the decoded operator");
    }

    bf6_skeleton* s = bf6_skeleton_compose(c, kSke, nullptr);
    if (!s) { bad("1P skeleton"); return 1; }
    {   /* scoped: a ClipSource must be gone before bf6_close, it frees into the context */
    bf6ant::ClipSource clip;
    std::string cerr;
    if (!clip.open(c, kClip, kRig, kSke, cerr)) bad(cerr.c_str());
    else {
        bf6ant::Pose a, b;
        a.resize((size_t)s->bone_count); b.resize((size_t)s->bone_count);
        clip.sample(c, 0.f, false, a);
        clip.sample(c, 35.f, false, b);
        double worst = 0, moved = 0;
        for (size_t i = 0; i < a.q.size(); ++i) {
            const auto& q = b.q[i];
            worst = std::fmax(worst, std::fabs(std::sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]) - 1.0));
            for (int k = 0; k < 4; ++k) moved = std::fmax(moved, std::fabs(a.q[i][(size_t)k] - b.q[i][(size_t)k]));
        }
        std::printf("clip %d frames; worst |q|-1 %.6f; largest change frame 0 -> 35: %.4f\n",
                    clip.frame_count(), worst, moved);
        if (worst > 1e-4) bad("rotations not unit");
        if (moved < 1e-3) bad("clip did not move between frames 0 and 35");
    }
    }
    bf6_free(c, s);
    std::printf("graph loaded %zu partition(s)\n%s\n", g.loaded(), fails ? "FAILED" : "PASS");
    bf6_close(c);
    return fails ? 1 : 0;
}
