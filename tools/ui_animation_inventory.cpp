#include "bf6_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<bf6_asset> assets(bf6_ctx* context, const char* prefix)
{
    const int count = bf6_list_ebx(context, prefix, nullptr, 0);
    std::vector<bf6_asset> rows(count > 0 ? static_cast<size_t>(count) : 0u);
    if (count > 0 && bf6_list_ebx(context, prefix, rows.data(), count) != count)
        rows.clear();
    std::sort(rows.begin(), rows.end(), [](const bf6_asset& a, const bf6_asset& b) {
        return std::strcmp(a.name, b.name) < 0;
    });
    return rows;
}

int scan(bf6_ctx* context, const char* prefix)
{
    const std::vector<bf6_asset> names = assets(context, prefix);
    int partitions = 0, tracks_total = 0, interpolators_total = 0;
    int layout_total = 0, timeline_total = 0;
    for (const bf6_asset& asset : names) {
        const int track_count = bf6_rime_float_tracks(
            context, asset.name, nullptr, 0);
        const int interpolator_count = bf6_rime_float_interpolators(
            context, asset.name, nullptr, 0);
        const int layout_count = bf6_rime_layout_tracks(
            context, asset.name, nullptr, 0);
        const int timeline_count = bf6_rime_timelines(
            context, asset.name, nullptr, 0);
        if (track_count <= 0 && interpolator_count <= 0 &&
            layout_count <= 0 && timeline_count <= 0) continue;
        ++partitions;
        tracks_total += std::max(track_count, 0);
        interpolators_total += std::max(interpolator_count, 0);
        layout_total += std::max(layout_count, 0);
        timeline_total += std::max(timeline_count, 0);
        std::printf("PART\t%s\ttracks=%d\tinterpolators=%d\tlayout=%d\t"
                    "timelines=%d\n", asset.name, track_count,
                    interpolator_count, layout_count, timeline_count);

        std::vector<bf6_rime_float_track> tracks(
            track_count > 0 ? static_cast<size_t>(track_count) : 0u);
        if (track_count > 0)
            bf6_rime_float_tracks(context, asset.name,
                                  tracks.data(), track_count);
        for (const bf6_rime_float_track& track : tracks) {
            std::vector<bf6_rime_float_key> keys(
                track.key_count > 0 ? static_cast<size_t>(track.key_count) : 0u);
            const int key_count = bf6_rime_float_track_keys(
                context, asset.name, track.track_instance,
                keys.data(), static_cast<int>(keys.size()));
            std::printf("TRACK\ttimeline=%d\ttrack=%d\ttarget=%d\t"
                        "pin=0x%08X\tfield=0x%08X\ttype=%d\tweighted=%d\tkeys=%d",
                        track.timeline_instance, track.track_instance,
                        track.target_instance, track.source_pin,
                        track.target_field, track.curve_type,
                        track.is_weighted, key_count);
            for (const bf6_rime_float_key& key : keys)
                std::printf("\t%.9g:%.9g:(%.9g,%.9g):(%.9g,%.9g)",
                            key.time, key.value,
                            key.in_tangent_x, key.in_tangent_y,
                            key.out_tangent_x, key.out_tangent_y);
            std::printf("\n");
        }

        std::vector<bf6_rime_float_interpolator> interpolators(
            interpolator_count > 0 ? static_cast<size_t>(interpolator_count) : 0u);
        if (interpolator_count > 0)
            bf6_rime_float_interpolators(
                context, asset.name, interpolators.data(), interpolator_count);
        for (const bf6_rime_float_interpolator& row : interpolators)
            std::printf("INTERP\tinstance=%d\tin=0x%08X\tout=0x%08X\t"
                        "default=%.9g\tduration=%.9g\tvelocity=%.9g\t"
                        "use_velocity=%d\ttype=%d\tmode=%d\tdynamic=%d\t"
                        "real_clock=%d\tframe_correct=%d\n",
                        row.instance, row.input_field, row.output_field,
                        row.default_value, row.duration, row.velocity,
                        row.use_velocity, row.interpolation_type,
                        row.interpolation_mode, row.dynamic_duration,
                        row.use_real_time_clock,
                        row.force_frame_correct_output);

        std::vector<bf6_rime_layout_track> layouts(
            layout_count > 0 ? static_cast<size_t>(layout_count) : 0u);
        if (layout_count > 0)
            bf6_rime_layout_tracks(
                context, asset.name, layouts.data(), layout_count);
        for (const bf6_rime_layout_track& row : layouts) {
            std::vector<bf6_rime_layout_key> keys(
                row.key_count > 0 ? static_cast<size_t>(row.key_count) : 0u);
            const int key_count = bf6_rime_layout_track_keys(
                context, asset.name, row.track_instance, keys.data(),
                static_cast<int>(keys.size()));
            std::printf("LAYOUT\ttimeline=%d\tdata=%d\tentity=%d\ttrack=%d\t"
                        "element=%d\tsource=0x%08X\ttarget=0x%08X\t"
                        "field=0x%08X\tkeys=%d",
                        row.timeline_instance, row.timeline_data_instance,
                        row.entity_track_instance, row.track_instance,
                        row.target_instance,
                        row.source_pin, row.target_pin, row.target_field,
                        key_count);
            for (const bf6_rime_layout_key& key : keys)
                std::printf("\t%.9g:(%.9g,%.9g,%.9g,%.9g,%.9g,%.9g):%d/%d",
                            key.time, key.layout.anchor_start,
                            key.layout.anchor_end, key.layout.offset_start,
                            key.layout.offset_end, key.layout.pivot,
                            key.layout.weight, key.interpolation_type,
                            key.interpolation_mode);
            std::printf("\n");
        }

        std::vector<bf6_rime_timeline> timelines(
            timeline_count > 0 ? static_cast<size_t>(timeline_count) : 0u);
        if (timeline_count > 0)
            bf6_rime_timelines(
                context, asset.name, timelines.data(), timeline_count);
        for (const bf6_rime_timeline& row : timelines)
            std::printf("TIMELINE\tinstance=%d\tdata=%d\tname=%s\tend=%d\t"
                        "autoplay=%d\tclock=%d\ttime=%.9g:%.9g:%.9g\n",
                        row.timeline_instance, row.timeline_data_instance,
                        row.debug_name, row.end_rule, row.autoplay_option,
                        row.delta_time_clock, row.start_time, row.init_time,
                        row.end_time);
    }
    std::printf("SUMMARY\tprefix=%s\tassets=%zu\tpartitions=%d\ttracks=%d\t"
                "interpolators=%d\tlayout=%d\ttimelines=%d\n",
                prefix, names.size(), partitions, tracks_total,
                interpolators_total, layout_total, timeline_total);
    return tracks_total + interpolators_total + layout_total + timeline_total;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: ui_animation_inventory <game-dir> [name-filter]\n");
        return 2;
    }
    char error[1024]{};
    bf6_ctx* context = bf6_open(argv[1], error, sizeof(error));
    if (!context || !bf6_mount_frontend(context, error, sizeof(error))) {
        std::fprintf(stderr, "mount: %s\n", error);
        if (context) bf6_close(context);
        return 1;
    }
    const char* prefix = argc == 3 ? argv[2] : "common/ui/home/";
    const int real = scan(context, prefix);
    const int fake = scan(context, "common/ui/__animation_control_missing__/");
    bf6_close(context);
    std::printf("CONTROL\treal=%d\tfake=%d\n", real, fake);
    return real > 0 && fake == 0 ? 0 : 1;
}
