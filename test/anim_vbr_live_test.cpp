/* Exact front-end VBR control. This exercises the codec the loadout soldiers
 * actually use, including the constant/animated channel interleave. */
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: anim_vbr_live_test <game-dir>\n");
        return 2;
    }
    static const char* kClips[] = {
        "animations/glacier/assets/frontend/mainmenu/loadout/"
        "ui_frontend_standing_idle_assault_02",
        "animations/glacier/assets/frontend/mainmenu/loadout/"
        "ui_frontend_standing_idle_engineer_02",
        "animations/glacier/assets/frontend/mainmenu/loadout/"
        "ui_frontend_standing_idle_support_02",
        "animations/glacier/assets/frontend/mainmenu/loadout/"
        "ui_frontend_standing_idle_recon_02",
        // Class.Loadout.Node -> classloadout.cbd -> the four class RCs.
        // These are the repeated base-idle outcomes in those installed RCs;
        // the idlebreak clips are weighted optional interruptions.
        "animations/glacier/assets/frontend/mainmenu/loadout/"
        "ui_frontend_standing_idle_cl_assault_02",
        "animations/glacier/assets/frontend/mainmenu/loadout/"
        "ui_frontend_standing_idle_cl_engineer_02",
        "animations/glacier/assets/frontend/mainmenu/loadout/"
        "ui_frontend_standing_idle_cl_support_02",
        "animations/glacier/assets/frontend/mainmenu/loadout/"
        "ui_frontend_standing_idle_cl_recon_02",
    };
    char error[512] = {};
    bf6_ctx* context = bf6_open(argv[1], error, (int)sizeof(error));
    if (!context) { std::fprintf(stderr, "open: %s\n", error); return 1; }
    if (!bf6_mount_all(context, 0, error, (int)sizeof(error))) {
        std::fprintf(stderr, "mount: %s\n", error); bf6_close(context); return 1;
    }
    bf6_anim_clip* fake = bf6_anim_clip_open(context,
        "animations/glacier/assets/frontend/mainmenu/loadout/__fake_idle__");
    bool ok = !fake;
    for (size_t clipIndex = 0; clipIndex < sizeof(kClips) / sizeof(kClips[0]);
         ++clipIndex)
    {
        bf6_anim_clip* clip = bf6_anim_clip_open(context, kClips[clipIndex]);
        ok = ok && clip;
        if (!clip) continue;
        ok = ok && clip->framing == BF6_ANIM_VBR &&
            clip->key_time_count > 1 && clip->channel_count > 0 &&
            clip->quat_count > 0 &&
            clip->predicted_stream_bytes == clip->actual_stream_bytes;
        if (clipIndex == 0)
            ok = ok && clip->key_time_count == 576 &&
                clip->channel_count == 505 && clip->quat_count == 145 &&
                clip->vec3_count == 94 && clip->scalar_count == 266 &&
                clip->const_quat_count == 97 && clip->const_vec3_count == 82 &&
                clip->const_scalar_count == 257;
        std::vector<float> first((size_t)clip->channel_count * 4);
        std::vector<float> middle(first.size()), last(first.size());
        const int middleFrame = clip->key_time_count / 2;
        const int lastFrame = clip->key_time_count - 1;
        ok = ok && bf6_anim_clip_sample(context, clip, 0, first.data(), nullptr) &&
            bf6_anim_clip_sample(context, clip, middleFrame, middle.data(), nullptr) &&
            bf6_anim_clip_sample(context, clip, lastFrame, last.data(), nullptr);
        double first_to_middle = 0.0, middle_to_last = 0.0;
        for (size_t i = 0; i < first.size(); ++i) {
            ok = ok && std::isfinite(first[i]) && std::isfinite(middle[i]) &&
                std::isfinite(last[i]);
            first_to_middle += std::fabs(first[i] - middle[i]);
            middle_to_last += std::fabs(middle[i] - last[i]);
        }
        int unit_quaternions = 0;
        for (int q = 0; q < clip->quat_count; ++q) {
            const float* value = first.data() + (size_t)q * 4;
            const double norm = std::sqrt((double)value[0] * value[0] +
                (double)value[1] * value[1] + (double)value[2] * value[2] +
                (double)value[3] * value[3]);
            if (std::fabs(norm - 1.0) < 1e-4) ++unit_quaternions;
        }
        ok = ok && unit_quaternions == clip->quat_count &&
            first_to_middle > 1e-3 && middle_to_last > 1e-3;
        std::printf("clip=%zu family=%s class=%zu vbr=%d keys=%d channels=%d q=%d v=%d f=%d "
                    "const=%d/%d/%d bytes=%lld/%lld unit=%d delta=%.6f/%.6f "
                    "fake=%d\n",
            clipIndex, clipIndex < 4 ? "detail" : "class-loadout",
            clipIndex % 4, clip->framing == BF6_ANIM_VBR ? 1 : 0,
            clip->key_time_count,
            clip->channel_count, clip->quat_count, clip->vec3_count,
            clip->scalar_count, clip->const_quat_count, clip->const_vec3_count,
            clip->const_scalar_count, (long long)clip->predicted_stream_bytes,
            (long long)clip->actual_stream_bytes, unit_quaternions,
            first_to_middle, middle_to_last, fake ? 1 : 0);
        bf6_free(context, clip);
    }
    // The four class controllers also reference four optional idle-break
    // outcomes per class on both the detail and class-lineup branches. Keep
    // these in the live codec surface: they are not filename guesses, and a
    // decoder regression that only preserves the persistent base idle would
    // otherwise leave the front-end animation graph partially untested.
    static const char* kClasses[] = {
        "assault", "engineer", "support", "recon"
    };
    for (int classIndex = 0; classIndex < 4; ++classIndex)
        for (int classLoadout = 0; classLoadout < 2; ++classLoadout)
            for (int breakIndex = 1; breakIndex <= 4; ++breakIndex)
            {
                char ordinal[3]{};
                std::snprintf(ordinal, sizeof(ordinal), "%02d", breakIndex);
                const std::string path =
                    "animations/glacier/assets/frontend/mainmenu/loadout/"
                    "ui_frontend_standing_idlebreak" + std::string(ordinal) +
                    (classLoadout ? "_cl_" : "_") + kClasses[classIndex] +
                    "_02";
                bf6_anim_clip* clip = bf6_anim_clip_open(
                    context, path.c_str());
                ok = ok && clip;
                if (!clip) continue;
                ok = ok && clip->framing == BF6_ANIM_VBR &&
                    clip->key_time_count > 1 && clip->channel_count > 0 &&
                    clip->quat_count > 0 &&
                    clip->predicted_stream_bytes == clip->actual_stream_bytes;
                std::vector<float> middle(
                    (size_t)clip->channel_count * 4);
                ok = ok && bf6_anim_clip_sample(context, clip,
                    clip->key_time_count / 2, middle.data(), nullptr);
                for (float value : middle)
                    ok = ok && std::isfinite(value);
                std::printf(
                    "idlebreak class=%s branch=%s break=%d keys=%d "
                    "channels=%d q=%d v=%d f=%d bytes=%lld/%lld\n",
                    kClasses[classIndex],
                    classLoadout ? "class-loadout" : "detail", breakIndex,
                    clip->key_time_count, clip->channel_count,
                    clip->quat_count, clip->vec3_count, clip->scalar_count,
                    (long long)clip->predicted_stream_bytes,
                    (long long)clip->actual_stream_bytes);
                bf6_free(context, clip);
            }
    if (fake) bf6_free(context, fake);
    bf6_close(context);
    std::printf("RESULT=%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
