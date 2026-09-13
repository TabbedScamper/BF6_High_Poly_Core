#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, (int)sizeof(error));
    if (!context || !bf6_mount_frontend(context, error, (int)sizeof(error)))
    {
        std::fprintf(stderr, "open/mount: %s\n", error);
        if (context) bf6_close(context);
        return 1;
    }
    static const char* route =
        "game/glacierflow/flow_mainmenu/ui/screens/bootflowstartscreen";
    const int count = bf6_rime_float_tracks(context, route, nullptr, 0);
    std::vector<bf6_rime_float_track> tracks((size_t)(count > 0 ? count : 0));
    if (count > 0)
        bf6_rime_float_tracks(context, route, tracks.data(), count);
    int alpha_tracks = 0;
    bool exact_curve = false;
    int shuffled_matches = 0;
    for (const bf6_rime_float_track& track : tracks)
    {
        std::vector<bf6_rime_float_key> keys((size_t)track.key_count);
        const int key_count = bf6_rime_float_track_keys(
            context, route, track.track_instance, keys.data(), track.key_count);
        if (track.target_field == 0x0C426471u) ++alpha_tracks;
        if (track.target_field == 0x0C426471u && key_count == 2 &&
            std::fabs(keys[0].time - 0.4f) < 1e-5f &&
            std::fabs(keys[0].value) < 1e-6f &&
            std::fabs(keys[1].time - 0.7f) < 1e-5f &&
            std::fabs(keys[1].value - 1.f) < 1e-6f)
            exact_curve = true;
        /* Shuffled control: target property and a different track's source pin
         * must not accidentally form another accepted identity. */
        for (const bf6_rime_float_track& other : tracks)
            shuffled_matches += (&other != &track &&
                other.source_pin == track.source_pin &&
                other.target_field == track.target_field);
    }
    const int fake_track = bf6_rime_float_track_keys(
        context, route, 0x7fffffff, nullptr, 0);
    const int fake_route = bf6_rime_float_tracks(
        context, "game/ui/__control__/not_a_screen", nullptr, 0);
    std::printf("tracks=%d alpha=%d exact=%d shuffled=%d fake-track=%d fake-route=%d\n",
                count, alpha_tracks, exact_curve ? 1 : 0, shuffled_matches,
                fake_track, fake_route);
    bf6_close(context);
    return count > 0 && alpha_tracks == 1 && exact_curve &&
        shuffled_matches == 0 && fake_track < 0 && fake_route < 0 ? 0 : 1;
}
