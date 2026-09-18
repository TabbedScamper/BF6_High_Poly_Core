/* WHICH JOINT IS THE SOLDIER'S EYE?
 *
 *   eye_joint_probe <game_dir>
 *
 * The first attempt used CameraJoint, because it exists on both the 3P and 1P
 * skeletons and sounds like the camera. On the 3P soldier it reported a 2.20 m
 * eye height, which is nobody's eye - the 3P camera rig sits above and behind
 * the head, which is exactly what a third-person camera should do and exactly
 * what a first-person eye should not.
 *
 * So rather than pick another plausible name, this measures every candidate at
 * the pose actually shown and prints its height above the soldier's lowest
 * point. The one that lands in human range is the answer.
 */
#include "bf6_core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct M43 { float m[12]; };

M43 from12(const float* p) { M43 r{}; for (int i = 0; i < 12; ++i) r.m[i] = p[i]; return r; }

M43 mul(const M43& a, const M43& b)
{
    M43 o{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            o.m[r * 3 + c] = a.m[r * 3 + 0] * b.m[0 + c] + a.m[r * 3 + 1] * b.m[3 + c]
                           + a.m[r * 3 + 2] * b.m[6 + c];
    for (int c = 0; c < 3; ++c)
        o.m[9 + c] = a.m[9 + 0] * b.m[0 + c] + a.m[9 + 1] * b.m[3 + c]
                   + a.m[9 + 2] * b.m[6 + c] + b.m[9 + c];
    return o;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: eye_joint_probe <game>\n"); return 2; }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_all(c, 1, err, (int)sizeof(err));

    for (const char* skeleton : {"common/characters/_soldier/ske_soldier_3p",
                                 "common/characters/_soldier/ske_soldier_1p"}) {
        bf6_skeleton* s = bf6_skeleton_compose(c, skeleton, nullptr);
        if (!s) { std::printf("%s UNREADABLE\n", skeleton); continue; }
        std::printf("\n=== %s (%d bones), at BIND pose ===\n", skeleton, s->bone_count);

        /* Model transforms from the bind locals. The bind pose is enough to
         * rank the joints by height; the animated pose moves them together. */
        std::vector<M43> model((size_t)s->bone_count);
        float lowest = 1e30f;
        for (int i = 0; i < s->bone_count; ++i) {
            const int p = s->bones[i].parent;
            model[(size_t)i] = p < 0 ? from12(s->bones[i].local)
                                     : mul(from12(s->bones[i].local), model[(size_t)p]);
            if (model[(size_t)i].m[10] < lowest) lowest = model[(size_t)i].m[10];
        }
        std::printf("lowest joint in the rig sits at y = %.4f\n", lowest);

        /* Anything whose name suggests a head, an eye or a camera. */
        for (int i = 0; i < s->bone_count; ++i) {
            const char* n = s->bones[i].name;
            if (!n) continue;
            if (std::strstr(n, "Head") || std::strstr(n, "Eye") || std::strstr(n, "Camera")
                || std::strstr(n, "Neck")) {
                std::printf("   %-28s y = %7.4f   (%.4f above the lowest joint)\n",
                            n, model[(size_t)i].m[10], model[(size_t)i].m[10] - lowest);
            }
        }
        bf6_free(c, s);
    }
    bf6_close(c);
    return 0;
}
