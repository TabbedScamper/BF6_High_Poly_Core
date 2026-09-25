/* IS THE SEATED DRIVER STRETCHED? The steering controller drive mode runs on the 3P
 * soldier, stepped as drive mode steps it, and every bone whose TRANSLATION it writes is
 * compared with that bone's bind offset on ske_soldier_3p. A limb whose offset grows or
 * shrinks by more than a few millimetres is what reads as a morphed body.
 *
 *   driver_stretch_probe <game_dir> <controller asset> [frames=90]
 */
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: driver_stretch_probe <game_dir> <controller> [frames]\n"); return 2; }
    const int frames = argc > 3 ? std::atoi(argv[3]) : 90;
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_frontend(c, err, (int)sizeof(err));
    bf6_mount_all(c, 0, err, (int)sizeof(err));
    const char* skp = "common/characters/_soldier/ske_soldier_3p";
    bf6_skeleton* sk = bf6_skeleton_read(c, skp);
    if (!sk) { std::printf("no skeleton\n"); return 1; }
    bf6_ant_runtime* rt = bf6_ant_runtime_create(c, argv[2], "animations/glacier/global/rigging/soldier_3p.rig",
                                                 skp, err, (int)sizeof(err));
    if (!rt) { std::printf("create: %s\n", err); return 1; }
    bf6_ant_runtime_set_float(rt, "animations/common/mm.vehicle.steeringangle.float", 0.5f);
    for (int f = 0; f < frames; ++f) bf6_ant_runtime_update(rt, 1.0f / 60.0f);
    std::vector<float> pose((size_t)sk->bone_count * 12);
    std::vector<uint8_t> vr((size_t)sk->bone_count), vt((size_t)sk->bone_count);
    const int n = bf6_ant_runtime_pose(rt, pose.data(), sk->bone_count);
    bf6_ant_runtime_pose_valid(rt, vr.data(), vt.data(), sk->bone_count);
    int written_t = 0, stretched = 0;
    for (int b = 0; b < n; ++b) {
        if (!vt[(size_t)b]) continue;
        ++written_t;
        const float* t = pose.data() + (size_t)b * 12 + 9;
        const float* bt = sk->bones[b].local + 9;
        const float lp = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        const float lb = std::sqrt(bt[0] * bt[0] + bt[1] * bt[1] + bt[2] * bt[2]);
        if (std::fabs(lp - lb) > 0.005f) {
            ++stretched;
            if (stretched <= 25)
                std::printf("  %-28s bind offset %7.1f mm  posed %7.1f mm  (%+.1f)\n",
                            sk->bones[b].name ? sk->bones[b].name : "?", lb * 1000, lp * 1000, (lp - lb) * 1000);
        }
    }
    std::printf("%s: %d bone(s) with written translation, %d off their bind length by > 5 mm\n",
                argv[2], written_t, stretched);
    bf6_free(c, rt);
    return 0;
}
