/* WHERE THE HANDS ARE AGAINST THE WEAPON'S IK MARKERS, frame by frame.
 *
 * The 1P graph's upper body is masked by 13p.wep.handikdisable: 0 (Neither disabled)
 * picks 1p.wep.upperbody.onlywep.noik.bml - the layer owns the weapon only and an IK
 * solver is expected to place both hands on Wep_IK_LeftHand / Wep_IK_RightHand; 3 (Both
 * disabled) picks botharms.ik.bml and the clips drive the arms. This runs the game's top
 * graph in a mode and prints, in model space, each hand's distance to its marker and how
 * far the markers themselves move - the check that the markers ARE the authored hand
 * targets before any solver trusts them.
 *
 *   hand_ik_probe <game_dir> <item> [handikdisable=3] [frames=90] [State=value ...]
 */
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct M { float m[12]; };

static M mul(const M& a, const M& b)   /* row vectors: a then b */
{
    M o{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c) {
            float s = a.m[r * 3 + 0] * b.m[0 * 3 + c] + a.m[r * 3 + 1] * b.m[1 * 3 + c] + a.m[r * 3 + 2] * b.m[2 * 3 + c];
            if (r == 3) s += b.m[9 + c];
            o.m[r * 3 + c] = s;
        }
    return o;
}

static float rot_angle(const M& a, const M& b)   /* angle between two bases, degrees */
{
    float tr = 0.f;   /* trace(a * b^T) over the 3x3 */
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) tr += a.m[r * 3 + c] * b.m[r * 3 + c];
    const float cs = std::fmax(-1.f, std::fmin(1.f, (tr - 1.f) * 0.5f));
    return std::acos(cs) * 57.29578f;
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: hand_ik_probe <game_dir> <item> [handikdisable] [frames] [State=v ...]\n"); return 2; }
    const int mode = argc > 3 ? std::atoi(argv[3]) : 3;
    const int frames = argc > 4 ? std::atoi(argv[4]) : 90;
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_frontend(c, err, (int)sizeof(err));
    bf6_mount_all(c, 0, err, (int)sizeof(err));
    int32_t w = -1, t = -1;
    if (!bf6_inspect_ids(c, argv[2], &w, &t, err, (int)sizeof(err))) { std::printf("ids: %s\n", err); return 1; }
    const char* skel_path = "common/characters/_soldier/ske_soldier_1p";
    bf6_skeleton* sk = bf6_skeleton_read(c, skel_path);
    if (!sk) { std::printf("no skeleton\n"); return 1; }
    bf6_ant_runtime* rt = bf6_ant_runtime_create(c, "animations/kingston/controllers/.1p.soldier.top.sf",
        "animations/glacier/global/rigging/soldier_1p.rig", skel_path, err, (int)sizeof(err));
    if (!rt) { std::printf("create: %s\n", err); return 1; }
    bf6_ant_runtime_set_weapon(rt, w, t);
    /* BF6_HAND_IK=1: the runtime's hand IK on, so the printed distances are AFTER it */
    if (std::getenv("BF6_HAND_IK")) bf6_ant_runtime_set_hand_ik(rt, 1);
    /* mode -1: leave the state alone and report what the graph itself holds for it */
    const char* kHik = "animations/glacier/global/gamestates/13p.wep.handikdisable.enumgs";
    if (mode >= 0) bf6_ant_runtime_set_int(rt, kHik, mode);
    for (int i = 5; i < argc; ++i) {
        std::string a = argv[i];
        const size_t eq = a.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = a.substr(0, eq), v = a.substr(eq + 1);
        if (k.size() > 5 && k.compare(k.size() - 5, 5, ".bool") == 0) bf6_ant_runtime_set_bool(rt, k.c_str(), std::atoi(v.c_str()));
        else if (k.size() > 6 && k.compare(k.size() - 6, 6, ".float") == 0) bf6_ant_runtime_set_float(rt, k.c_str(), (float)std::atof(v.c_str()));
        else bf6_ant_runtime_set_int(rt, k.c_str(), std::atoi(v.c_str()));
    }
    auto find = [&](const char* n) {
        for (int i = 0; i < sk->bone_count; ++i) if (sk->bones[i].name && std::strcmp(sk->bones[i].name, n) == 0) return i;
        return -1;
    };
    const int lh = find("LeftHand"), rh = find("RightHand"), il = find("Wep_IK_LeftHand"), ir = find("Wep_IK_RightHand");
    std::printf("%s: weapon %d type %d, handikdisable %d; bones LeftHand %d RightHand %d Wep_IK_LeftHand %d Wep_IK_RightHand %d\n",
                argv[2], w, t, mode, lh, rh, il, ir);
    if (lh < 0 || rh < 0 || il < 0 || ir < 0) return 1;
    if (std::getenv("BF6_LIST_WEP_BONES"))
        for (int i = 0; i < sk->bone_count; ++i) {
            const char* n = sk->bones[i].name ? sk->bones[i].name : "";
            if (std::strncmp(n, "Wep", 3) == 0 || std::strstr(n, "Camera") || std::strstr(n, "Aim") || std::strstr(n, "Sight"))
                std::printf("bone %3d %-28s parent %-24s bind local t (%.4f %.4f %.4f) model t (%.4f %.4f %.4f)\n", i, n,
                            sk->bones[i].parent >= 0 && sk->bones[sk->bones[i].parent].name ? sk->bones[sk->bones[i].parent].name : "-",
                            sk->bones[i].local[9], sk->bones[i].local[10], sk->bones[i].local[11],
                            sk->bones[i].model[9], sk->bones[i].model[10], sk->bones[i].model[11]);
        }
    for (int b : { il, ir, lh, rh }) {
        std::string chain;
        for (int k = b; k >= 0; k = sk->bones[k].parent) { chain += sk->bones[k].name ? sk->bones[k].name : "?"; chain += k == 0 ? "" : " < "; }
        std::printf("chain: %s\n", chain.c_str());
    }
    std::vector<float> pose((size_t)sk->bone_count * 12);
    std::vector<uint8_t> vr((size_t)sk->bone_count), vt((size_t)sk->bone_count);
    std::vector<M> model((size_t)sk->bone_count);
    float il0[3] = {0, 0, 0}, max_l = 0, max_r = 0, move_l = 0;
    for (int f = 0; f < frames; ++f) {
        bf6_ant_runtime_update(rt, 1.0f / 60.0f);
        const int n = bf6_ant_runtime_pose(rt, pose.data(), sk->bone_count);
        bf6_ant_runtime_pose_valid(rt, vr.data(), vt.data(), sk->bone_count);
        for (int b = 0; b < n && b < sk->bone_count; ++b) {
            M l;
            std::memcpy(l.m, pose.data() + (size_t)b * 12, 48);
            const int p = sk->bones[b].parent;
            model[(size_t)b] = p < 0 ? l : mul(l, model[(size_t)p]);
        }
        auto d = [&](int a, int b) {
            const float* x = model[(size_t)a].m + 9; const float* y = model[(size_t)b].m + 9;
            return std::sqrt((x[0] - y[0]) * (x[0] - y[0]) + (x[1] - y[1]) * (x[1] - y[1]) + (x[2] - y[2]) * (x[2] - y[2]));
        };
        const float dl = d(lh, il), dr = d(rh, ir);
        if (f == 0) std::memcpy(il0, model[(size_t)il].m + 9, 12);
        const float* p = model[(size_t)il].m + 9;
        const float mv = std::sqrt((p[0] - il0[0]) * (p[0] - il0[0]) + (p[1] - il0[1]) * (p[1] - il0[1]) + (p[2] - il0[2]) * (p[2] - il0[2]));
        if (f > 10) { max_l = std::fmax(max_l, dl); max_r = std::fmax(max_r, dr); }
        move_l = std::fmax(move_l, mv);
        if (mode < 0 && (f % 15 == 0 || f == frames - 1)) {
            int ok = 0;
            const int v = bf6_ant_runtime_get_int(rt, kHik, &ok);
            std::printf("frame %3d  handikdisable as the graph holds it: %d (%s)\n", f, v, ok ? "evaluable" : "not evaluable");
        }
        if (f % 15 == 0 || f == frames - 1)
            std::printf("frame %3d  left hand->marker %6.1f mm %5.1f deg   right hand->marker %6.1f mm %5.1f deg   "
                        "left marker moved %6.1f mm   written: LH %d/%d RH %d/%d IKL %d/%d\n",
                        f, dl * 1000, rot_angle(model[(size_t)lh], model[(size_t)il]),
                        dr * 1000, rot_angle(model[(size_t)rh], model[(size_t)ir]), mv * 1000,
                        vr[(size_t)lh], vt[(size_t)lh], vr[(size_t)rh], vt[(size_t)rh], vr[(size_t)il], vt[(size_t)il]);
    }
    std::printf("after frame 10: worst left %.1f mm, worst right %.1f mm; left marker travel %.1f mm\n",
                max_l * 1000, max_r * 1000, move_l * 1000);
    bf6_free(c, rt);
    return 0;
}
