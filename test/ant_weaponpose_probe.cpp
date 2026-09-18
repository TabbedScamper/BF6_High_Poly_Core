/* ant_weaponpose_probe - HOW FAR IS A WEAPON'S OWN 1P POSE FROM THE GENERIC HOLD?
 *
 *   ant_weaponpose_probe <game> [item]
 *
 * The first-person hold the editor draws is one generic rifle pose
 * (p_1p_rifle_stand_idle_01). The game layers each weapon's own pose over the
 * arms through 1p.weaponpose.cdbchooser, a lookup on the weapon's animation
 * identity. If the two differ a lot on the arm bones, a generic hold holds the
 * wrong gun - the hands miss the weapon and the forearms cross in front of it.
 * This runs both through the ANT runtime and reports the arm bones' angles.
 */
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr const char* kRig = "animations/glacier/global/rigging/soldier_1p.rig";
constexpr const char* kSke = "common/characters/_soldier/ske_soldier_1p";
constexpr const char* kWeapon = "animations/glacier/global/gamestates/fb.wep.specificweapon.enumgs";
constexpr const char* kType = "animations/common/fb.weapontype.enumgs";

bool pose_of(bf6_ctx* c, const char* root, int32_t sw, int32_t wt, std::vector<float>& p,
             std::vector<uint8_t>& vr, int& bones)
{
    char err[512] = {0};
    bf6_ant_runtime* rt = bf6_ant_runtime_create(c, root, kRig, kSke, err, (int)sizeof(err));
    if (!rt) { std::printf("  %s: %s\n", root, err); return false; }
    bf6_ant_runtime_set_weapon(rt, sw, wt);   /* incl. the dual-wield cached states */
    bf6_ant_runtime_update(rt, 0.f);
    bones = bf6_ant_runtime_pose(rt, nullptr, 0);
    p.assign((size_t)bones * 12, 0.f);
    vr.assign((size_t)bones, 0);
    bf6_ant_runtime_pose(rt, p.data(), bones);
    bf6_ant_runtime_pose_valid(rt, vr.data(), nullptr, bones);
    std::vector<char> notes((size_t)bf6_ant_runtime_notes(rt, nullptr, 0) + 1);
    bf6_ant_runtime_notes(rt, notes.data(), (int)notes.size());
    if (notes.size() > 2) std::printf("  notes for %s:\n%s", root, notes.data());
    bf6_free(c, rt);
    return true;
}

/* angle between two row-basis rotations, degrees */
double angle(const float* a, const float* b)
{
    double tr = 0;
    for (int r = 0; r < 3; ++r) for (int k = 0; k < 3; ++k) tr += (double)a[r * 3 + k] * b[r * 3 + k];
    double c = (tr - 1.0) * 0.5;
    c = c > 1 ? 1 : (c < -1 ? -1 : c);
    return std::acos(c) * 57.29577951308232;
}
}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) { std::printf("usage: ant_weaponpose_probe <game> [item]\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_all(c, 1, err, (int)sizeof(err));
    const char* item = argc > 2 ? argv[2] : "carbine/m4a1";
    int32_t sw = -1, wt = -1;
    if (!bf6_inspect_ids(c, item, &sw, &wt, err, (int)sizeof(err))) { std::printf("ids: %s\n", err); return 1; }
    std::printf("%s: specificweapon %d, weapontype %d\n", item, sw, wt);

    std::vector<float> gen, wep;
    std::vector<uint8_t> gv, wv;
    int gb = 0, wb = 0;
    if (!pose_of(c, "animations/glacier/assets/1p/common/rifle/loco/p_1p_rifle_stand_idle_01", sw, wt, gen, gv, gb) ||
        !pose_of(c, (argc > 3 ? argv[3] : "animations/kingston/controllers/1p.weaponpose.cdbchooser"), sw, wt, wep, wv, wb)) {
        bf6_close(c);
        return 1;
    }
    bf6_skeleton* s = bf6_skeleton_compose(c, kSke, nullptr);
    int driven = 0;
    double worst = 0;
    std::string worst_bone;
    std::printf("arm bones, weapon pose vs generic hold (degrees):\n");
    for (int i = 0; s && i < s->bone_count && i < gb && i < wb; ++i) {
        if (!wv[(size_t)i]) continue;
        ++driven;
        const char* nm = s->bones[i].name ? s->bones[i].name : "";
        const double a = angle(&wep[(size_t)i * 12], &gen[(size_t)i * 12]);
        if (a > worst) { worst = a; worst_bone = nm; }
        const bool arm = std::strstr(nm, "Arm") || std::strstr(nm, "Hand") || std::strstr(nm, "Shoulder");
        if (arm && !std::strstr(nm, "Wep") && !std::strstr(nm, "Finger") && std::strchr(nm, '_') == nullptr)
            std::printf("  %-20s %7.2f\n", nm, a);
    }
    std::printf("the weapon pose drives %d bone rotations; worst %.1f deg on %s\n", driven, worst, worst_bone.c_str());
    if (s) bf6_free(c, s);
    bf6_close(c);
    return 0;
}
