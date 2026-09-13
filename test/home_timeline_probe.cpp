#include "bf6_core.h"

#include <cstdio>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, sizeof(error));
    if (!context || !bf6_mount_frontend(context, error, sizeof(error)))
    {
        std::fprintf(stderr, "open/mount: %s\n", error);
        if (context) bf6_close(context);
        return 1;
    }
    static const char* route = "common/ui/home/screens/home_screen";
    const int count = bf6_rime_float_tracks(context, route, nullptr, 0);
    std::vector<bf6_rime_float_track> tracks(
        static_cast<size_t>(count > 0 ? count : 0));
    const int filled = count > 0 ? bf6_rime_float_tracks(
        context, route, tracks.data(), count) : count;
    for (const bf6_rime_float_track& track : tracks)
    {
        std::vector<bf6_rime_float_key> keys(
            static_cast<size_t>(track.key_count > 0 ? track.key_count : 0));
        const int keyCount = bf6_rime_float_track_keys(
            context, route, track.track_instance, keys.data(), track.key_count);
        std::printf("timeline=%d data=%d track=%d curve=%d target=%d "
                    "pin=%08X field=%08X type=%d weighted=%d keys=%d/%d",
                    track.timeline_instance, track.timeline_data_instance,
                    track.track_instance, track.curve_instance,
                    track.target_instance, track.source_pin,
                    track.target_field, track.curve_type, track.is_weighted,
                    keyCount, track.key_count);
        for (const bf6_rime_float_key& key : keys)
            std::printf(" [%.4g=%.4g]", key.time, key.value);
        std::printf("\n");
    }
    const int interpolatorCount = bf6_rime_float_interpolators(
        context, route, nullptr, 0);
    std::vector<bf6_rime_float_interpolator> interpolators(
        static_cast<size_t>(interpolatorCount > 0 ? interpolatorCount : 0));
    if (interpolatorCount > 0)
        bf6_rime_float_interpolators(
            context, route, interpolators.data(), interpolatorCount);
    for (const bf6_rime_float_interpolator& row : interpolators)
        std::printf("interpolator=%d in=%08X out=%08X default=%.4g "
                    "duration=%.4g velocity=%.4g type=%d mode=%d\n",
                    row.instance, row.input_field, row.output_field,
                    row.default_value, row.duration, row.velocity,
                    row.interpolation_type, row.interpolation_mode);
    const int fake = bf6_rime_float_tracks(
        context, "common/ui/home/screens/__control__", nullptr, 0);
    std::printf("count=%d filled=%d interpolators=%d fake=%d\n",
                count, filled, interpolatorCount, fake);
    bf6_close(context);
    return count >= 0 && count == filled && fake < 0 ? 0 : 1;
}
