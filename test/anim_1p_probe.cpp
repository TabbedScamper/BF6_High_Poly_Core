/* IS FIRST-PERSON ANIMATION READABLE TODAY?
 *
 *   anim_1p_probe <game_dir> <level> [clip ...]
 *
 * The machinery exists - bf6_anim_clip_open / _sample / _sample_time - and
 * Unreal already uses it, but for ONE static third-person pose sampled at frame
 * 0. Nobody plays a clip and Godot reads none of it. Before proposing first
 * person as a feature, this asks whether the clips open, how many frames they
 * carry, and whether successive frames actually differ - a clip that opens and
 * returns the same pose every frame would animate nothing.
 */
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: anim_1p_probe <game> <level> [clip...]\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    if (!bf6_mount_all(c, 1, err, (int)sizeof(err)))
        std::printf("note: mount_all said %s\n", err);

    /* Default set: whatever the mount actually carries with a 1p clip name, so
     * this is not a test against a path typed from memory. */
    std::vector<std::string> clips;
    for (int a = 3; a < argc; ++a) clips.push_back(argv[a]);
    if (clips.empty()) {
        const int n = bf6_list_ebx(c, "p_1p_", nullptr, 0);
        std::vector<bf6_asset> rows((size_t)(n > 0 ? n : 1));
        const int got = n > 0 ? bf6_list_ebx(c, "p_1p_", rows.data(), n) : 0;
        std::printf("the mount carries %d name(s) containing \"p_1p_\"\n", n);
        for (int i = 0; i < got && (int)clips.size() < 6; ++i)
            if (rows[(size_t)i].name) clips.push_back(rows[(size_t)i].name);
    }

    int opened = 0, moving = 0;
    for (const std::string& path : clips)
    {
        bf6_anim_clip* clip = bf6_anim_clip_open(c, path.c_str());
        if (!clip) { std::printf("  --    could not open  %s\n", path.c_str()); continue; }
        opened++;
        const int channels = clip->channel_count;
        const int frames = clip->key_time_count;
        /* DOES IT MOVE? Sample the first and a later frame and compare. A clip
         * that opens and returns one pose forever would animate nothing, and
         * that failure would otherwise look like success. */
        double delta = 0.0;
        if (channels > 0 && frames > 1) {
            std::vector<float> a((size_t)channels * 4), b((size_t)channels * 4);
            const int oka = bf6_anim_clip_sample(c, clip, 0, a.data(), nullptr);
            const int okb = bf6_anim_clip_sample(c, clip, frames / 2, b.data(), nullptr);
            if (oka && okb)
                for (size_t i = 0; i < a.size(); ++i) delta += std::fabs((double)a[i] - (double)b[i]);
        }
        if (delta > 0.001) moving++;
        std::printf("  %-4s %4d channel(s) %5d frame(s)  motion %8.3f  %s\n",
                    delta > 0.001 ? "ok" : "flat", channels, frames, delta,
                    path.substr(path.rfind('/') + 1).c_str());
        bf6_free(c, clip);
    }
    std::printf("\n%d of %zu clip(s) opened, %d carry motion\n", opened, clips.size(), moving);
    bf6_close(c);
    return opened && moving ? 0 : 1;
}
