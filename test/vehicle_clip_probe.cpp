/* Bind a vehicle animation clip to a vehicle rig and print what it drives.
 *   vehicle_clip_probe <clip ebx> <rig ebx> <skeleton ebx>
 * Prints the binding stats, then the first and last frame of each bound bone. */
#include "bf6_core.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: vehicle_clip_probe <clip> <rig> <skeleton>\n"); return 2; }
    const char* game = std::getenv("BF6_GAME") ? std::getenv("BF6_GAME")
        : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Battlefield 6";
    char err[1024] = {};
    bf6_ctx* ctx = bf6_open(game, err, (int)sizeof(err));
    if (!ctx || !bf6_mount_all(ctx, 1, err, (int)sizeof(err))) { std::fprintf(stderr, "%s\n", err); return 2; }
    bf6_anim_binding_stats st{};
    const int n = bf6_anim_bindings(ctx, argv[1], argv[2], argv[3], nullptr, 0, &st);
    std::printf("bindings %d: map keys %d rig keys %d resolved bones %d unresolved keys %d unresolved bones %d\n",
                n, st.map_keys, st.rig_keys, st.resolved_bones, st.unresolved_keys, st.unresolved_bones);
    if (n < 1) return 1;
    std::vector<bf6_anim_binding> b((size_t)n);
    bf6_anim_bindings(ctx, argv[1], argv[2], argv[3], b.data(), n, &st);
    bf6_anim_clip* clip = bf6_anim_clip_open(ctx, argv[1]);
    if (!clip) { std::printf("clip does not open\n"); return 1; }
    std::printf("clip channels %d frames %d\n", clip->channel_count, clip->key_time_count);
    bf6_skeleton* sk = bf6_skeleton_read(ctx, argv[3]);
    std::vector<float> a((size_t)clip->channel_count * 4), z((size_t)clip->channel_count * 4);
    bf6_anim_clip_sample_time(ctx, clip, 0.0f, 0, a.data());
    bf6_anim_clip_sample_time(ctx, clip, (float)(clip->key_time_count - 1), 0, z.data());
    if (std::getenv("BF6_CLIP_DOFS"))
        for (const auto& x : b)
            std::printf("  dof %-40s channel %d comp %d bone %d\n", x.dof_name, x.channel, x.component, x.bone);
    for (const auto& x : b) {
        if (x.bone < 0 || x.channel < 0 || x.channel >= clip->channel_count) continue;
        const char* name = sk && x.bone < sk->bone_count && sk->bones[x.bone].name ? sk->bones[x.bone].name : "?";
        const float* p = a.data() + x.channel * 4;
        const float* q = z.data() + x.channel * 4;
        std::printf("  %-36s %s  first (%.3f %.3f %.3f %.3f)  last (%.3f %.3f %.3f %.3f)\n", name,
                    x.component == BF6_ANIM_DOF_QUATERNION ? "rot" : x.component == BF6_ANIM_DOF_VECTOR3 ? "pos" : "scl",
                    p[0], p[1], p[2], p[3], q[0], q[1], q[2], q[3]);
    }
    return 0;
}
