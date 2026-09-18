/* anim_clip_drives_probe - WHICH BONES A CLIP ACTUALLY DRIVES.
 *
 *   anim_clip_drives_probe <game> <clip asset> [bone name ...]
 *
 * Binds the clip to the 1P rig and skeleton and prints, per bone, whether the
 * clip carries a rotation channel, a translation channel, or neither. Named
 * bones are reported individually; with none named it prints the totals.
 *
 * It exists to answer "did the graph write this bone, or did we?" - a bone a
 * clip does not drive must keep whatever the caller had, and a runtime that
 * hands back a bind value for it is inventing a pose.
 */
#include "bf6_core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { std::printf("usage: anim_clip_drives_probe <game> <clip> [bone ...]\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_all(c, 1, err, (int)sizeof(err));
    const char* kRig = "animations/glacier/global/rigging/soldier_1p.rig";
    const char* kSke = "common/characters/_soldier/ske_soldier_1p";
    bf6_skeleton* s = bf6_skeleton_compose(c, kSke, nullptr);
    if (!s) { std::printf("no skeleton\n"); bf6_close(c); return 1; }
    bf6_anim_binding_stats st{};
    const int n = bf6_anim_bindings(c, argv[2], kRig, kSke, nullptr, 0, &st);
    std::vector<bf6_anim_binding> b((size_t)(n > 0 ? n : 0));
    if (n > 0) bf6_anim_bindings(c, argv[2], kRig, kSke, b.data(), n, &st);
    std::vector<int> rot((size_t)s->bone_count, 0), tr((size_t)s->bone_count, 0);
    for (const auto& x : b) {
        if (x.bone < 0 || x.bone >= s->bone_count) continue;
        if (x.component == BF6_ANIM_DOF_QUATERNION) rot[(size_t)x.bone]++;
        else if (x.component == BF6_ANIM_DOF_VECTOR3) tr[(size_t)x.bone]++;
    }
    int nr = 0, nt = 0;
    for (int i = 0; i < s->bone_count; ++i) { nr += rot[(size_t)i] ? 1 : 0; nt += tr[(size_t)i] ? 1 : 0; }
    std::printf("%s: %d binding(s) over %d bones - drives %d rotation(s), %d translation(s)\n",
                argv[2], n, s->bone_count, nr, nt);
    for (int a = 3; a < argc; ++a) {
        int idx = -1;
        for (int i = 0; i < s->bone_count; ++i)
            if (s->bones[i].name && std::strcmp(s->bones[i].name, argv[a]) == 0) { idx = i; break; }
        if (idx < 0) { std::printf("  %-24s not a bone of this skeleton\n", argv[a]); continue; }
        std::printf("  %-24s rotation %s, translation %s\n", argv[a],
                    rot[(size_t)idx] ? "DRIVEN" : "-", tr[(size_t)idx] ? "DRIVEN" : "-");
    }
    bf6_free(c, s);
    bf6_close(c);
    return 0;
}
