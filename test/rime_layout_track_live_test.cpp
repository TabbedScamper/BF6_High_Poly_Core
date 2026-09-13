#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static bool near(float a, float b)
{
    return std::fabs(a - b) < 1e-5f;
}

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

    static const char* route = "common/ui/system/popups/nudgepopup";
    const int count = bf6_rime_layout_tracks(context, route, nullptr, 0);
    std::vector<bf6_rime_layout_track> tracks((size_t)(count > 0 ? count : 0));
    if (count > 0)
        bf6_rime_layout_tracks(context, route, tracks.data(), count);

    int horizontal = 0, vertical = 0, keys_total = 0, owned = 0, targeted = 0;
    bool exact_horizontal = false, exact_vertical = false;
    int malformed = 0;
    for (const bf6_rime_layout_track& track : tracks)
    {
        horizontal += track.target_field == 0x36BBCDA1u;
        vertical += track.target_field == 0x8A1C220Du;
        owned += track.timeline_instance >= 0 &&
            track.timeline_data_instance >= 0 &&
            track.entity_track_instance >= 0;
        targeted += track.target_instance >= 0;
        std::vector<bf6_rime_layout_key> keys((size_t)track.key_count);
        const int got = bf6_rime_layout_track_keys(
            context, route, track.track_instance, keys.data(), track.key_count);
        if (got != track.key_count) { ++malformed; continue; }
        keys_total += got;
        for (int i = 1; i < got; ++i)
            malformed += keys[(size_t)i].time < keys[(size_t)i - 1].time;
        if (track.target_field == 0x36BBCDA1u && got == 2 &&
            keys[0].interpolation_type == 1 &&
            keys[0].interpolation_mode == 1 && near(keys[0].time, 0.f) &&
            near(keys[0].layout.anchor_start, 0.4f) &&
            near(keys[0].layout.anchor_end, 0.6f) &&
            near(keys[1].time, 0.25f) &&
            near(keys[1].layout.anchor_start, 0.f) &&
            near(keys[1].layout.anchor_end, 1.f))
            exact_horizontal = true;
        if (track.target_field == 0x8A1C220Du && got == 2 &&
            keys[0].interpolation_type == 1 &&
            keys[0].interpolation_mode == 1 && near(keys[0].time, 0.f) &&
            near(keys[0].layout.anchor_start, 0.45f) &&
            near(keys[0].layout.anchor_end, 0.55f) &&
            near(keys[0].layout.offset_start, -64.f) &&
            near(keys[0].layout.offset_end, -64.f) &&
            near(keys[1].time, 0.25f) &&
            near(keys[1].layout.anchor_start, 0.f) &&
            near(keys[1].layout.anchor_end, 1.f))
            exact_vertical = true;
    }

    const int fake_track = bf6_rime_layout_track_keys(
        context, route, 0x7fffffff, nullptr, 0);
    const int fake_route = bf6_rime_layout_tracks(
        context, "common/ui/__control__/not_a_layout", nullptr, 0);
    const int wrong_type = bf6_rime_layout_track_keys(
        context, route, 0, nullptr, 0);
    const int timeline_count = bf6_rime_timelines(context, route, nullptr, 0);
    std::vector<bf6_rime_timeline> timelines(
        (size_t)(timeline_count > 0 ? timeline_count : 0));
    if (timeline_count > 0)
        bf6_rime_timelines(context, route, timelines.data(), timeline_count);
    const bool exact_timeline = timeline_count == 1 &&
        std::string(timelines[0].debug_name) == "Anim" &&
        timelines[0].end_rule == 0 && timelines[0].autoplay_option == 0 &&
        timelines[0].delta_time_clock == 2 && near(timelines[0].start_time, 0.f) &&
        near(timelines[0].end_time, 0.5f) &&
        near(timelines[0].encoded_runtime_framerate, -60.f) &&
        timelines[0].reset_time_on_started == 1;
    const int fake_timelines = bf6_rime_timelines(
        context, "common/ui/__control__/not_a_layout", nullptr, 0);
    static const char* options_route =
        "common/ui/options/ui/widgets/options_enum";
    const int options_count = bf6_rime_layout_tracks(
        context, options_route, nullptr, 0);
    std::vector<bf6_rime_layout_track> options(
        (size_t)(options_count > 0 ? options_count : 0));
    if (options_count > 0)
        bf6_rime_layout_tracks(
            context, options_route, options.data(), options_count);
    int options_exact_targets = 0;
    for (const bf6_rime_layout_track& track : options)
        options_exact_targets += track.target_instance == 14 &&
            track.entity_track_instance >= 0 &&
            track.timeline_instance >= 0 &&
            track.target_field == 0x8A1C220Du;
    std::printf("tracks=%d horizontal=%d vertical=%d keys=%d owned=%d "
                "targeted=%d exact=%d/%d timeline=%d/%d options=%d/%d "
                "malformed=%d fake-track=%d "
                "fake-route=%d fake-timelines=%d wrong-type=%d\n",
                count, horizontal, vertical, keys_total, owned, targeted,
                exact_horizontal ? 1 : 0, exact_vertical ? 1 : 0,
                timeline_count, exact_timeline ? 1 : 0,
                options_count, options_exact_targets, malformed,
                fake_track, fake_route, fake_timelines, wrong_type);
    bf6_close(context);
    return count == 2 && horizontal == 1 && vertical == 1 && keys_total == 4 &&
        owned == 2 && targeted == 2 && options_count == 2 &&
        options_exact_targets == 2 &&
        exact_horizontal && exact_vertical && malformed == 0 &&
        exact_timeline && fake_track < 0 && fake_route < 0 &&
        fake_timelines < 0 && wrong_type < 0 ? 0 : 1;
}
