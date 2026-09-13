#include "bf6_core.h"
#include "rime_runtime.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {
bool near(float a, float b) { return std::fabs(a - b) < 1.0e-4f; }

const bf6_rime_runtime::LayoutCommit* root_layout(
    const bf6_rime_runtime::Runtime& runtime, int timeline)
{
    const bf6_rime_runtime::LayoutCommit* found = nullptr;
    for (const auto& row : runtime.layout_commits())
        if (row.address.partition ==
                "common/ui/options/ui/widgets/options_enum" &&
            row.address.occurrence.empty() && row.address.instance == 14 &&
            row.address.field == 0x8A1C220Du &&
            row.timeline_instance == timeline) {
            if (found) return nullptr;
            found = &row;
        }
    return found;
}
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    char native_error[512]{};
    bf6_ctx* context = bf6_open(argv[1], native_error, sizeof(native_error));
    if (!context || !bf6_mount_frontend(context, native_error,
                                        sizeof(native_error))) {
        std::fprintf(stderr, "open/mount: %s\n", native_error);
        if (context) bf6_close(context);
        return 1;
    }
    const std::string route = "common/ui/options/ui/widgets/options_enum";
    bf6_rime_runtime::Runtime runtime(context);
    std::string error;
    bool ok = runtime.compile(route, 6, error);
    const auto* lost = root_layout(runtime, 29);
    const bool initial_lost = lost && near(lost->layout.offset_start, 0.f) &&
        near(lost->layout.offset_end, 0.f);

    auto cell = runtime.clone();
    ok &= cell != nullptr;

    bf6_rime_runtime::Address focus{route, {}, 45, 0xCBEC4452u};
    ok &= cell && cell->set(focus,
        bf6_rime_runtime::Value::from_bool(true), error);
    const auto start_report = cell ? cell->tick(0.0) :
        bf6_rime_runtime::TickReport{};
    const auto* start = cell ? root_layout(*cell, 30) : nullptr;
    const bool exact_start = start && near(start->layout.offset_start, 8.f) &&
        near(start->layout.offset_end, -8.f);
    const auto half_report = cell ? cell->tick(0.1) :
        bf6_rime_runtime::TickReport{};
    const auto* half = cell ? root_layout(*cell, 30) : nullptr;
    const float half_value = half ? half->layout.offset_start : -999.f;
    const bool exact_half = half && near(half_value, 15.5f) &&
        near(half->layout.offset_end, -15.5f);
    const auto key_end_report = cell ? cell->tick(0.1) :
        bf6_rime_runtime::TickReport{};
    const auto* key_end = cell ? root_layout(*cell, 30) : nullptr;
    const float end_value = key_end ? key_end->layout.offset_start : -999.f;
    const bool exact_end = key_end && near(end_value, 18.f) &&
        near(key_end->layout.offset_end, -18.f);
    const auto finish_report = cell ? cell->tick(0.1001) :
        bf6_rime_runtime::TickReport{};
    const auto original_report = runtime.tick(0.0);
    const auto* original_lost = root_layout(runtime, 29);
    const bool clone_isolated = root_layout(runtime, 30) == nullptr &&
        original_lost && near(original_lost->layout.offset_start, 0.f) &&
        near(original_lost->layout.offset_end, 0.f) &&
        original_report.started_timelines == 0;

    std::printf("initial=%d start=%d/%d half=%d/%.3f/%d end=%d/%.3f/%d "
                "finish=%d active=%d clone=%d compiled=%d/%d\n",
                initial_lost ? 1 : 0, exact_start ? 1 : 0,
                start_report.started_timelines,
                exact_half ? 1 : 0, half_value,
                half_report.sampled_layout_tracks,
                exact_end ? 1 : 0, end_value,
                key_end_report.sampled_layout_tracks,
                finish_report.completed_timelines,
                finish_report.active_timelines,
                clone_isolated ? 1 : 0,
                runtime.compile_report().timeline_nodes,
                runtime.compile_report().targeted_layout_track_nodes);
    bf6_close(context);
    return ok && initial_lost && exact_start && exact_half && exact_end &&
        clone_isolated &&
        start_report.started_timelines == 1 &&
        finish_report.completed_timelines == 1 ? 0 : 1;
}
