#include "bf6_core.h"
#include "rime_runtime.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    char native_error[512]{};
    bf6_ctx* context = bf6_open(argv[1], native_error, sizeof(native_error));
    if (!context || !bf6_mount_frontend(context, native_error,
                                        sizeof(native_error)))
    {
        std::fprintf(stderr, "open/mount: %s\n", native_error);
        if (context) bf6_close(context);
        return 1;
    }

    const std::string route = "common/ui/options/ui/widgets/options_enum";
    const int count = bf6_rime_compare_bools(context, route.c_str(), nullptr, 0);
    std::vector<bf6_rime_compare_bool> rows(
        static_cast<size_t>(count > 0 ? count : 0));
    if (count > 0)
        bf6_rime_compare_bools(context, route.c_str(), rows.data(), count);
    const bool exact = count == 1 && rows[0].instance == 44 &&
        rows[0].input_field == 0x7C80C0CBu &&
        rows[0].on_true_event == 0xAA8E5552u &&
        rows[0].on_false_event == 0xFB1E95D9u &&
        rows[0].authored_value == 0 && rows[0].always_send == 0 &&
        rows[0].trigger_on_property_change == 1 &&
        rows[0].trigger_on_start == 1;

    bf6_rime_runtime::Runtime runtime(context);
    std::string error;
    bool ok = exact && runtime.compile(route, 6, error);
    const auto initial = runtime.emitted_events();
    bool initial_lost = false;
    for (const auto& event : initial)
        initial_lost |= event.source.partition == route &&
            event.source.occurrence.empty() && event.source.instance == 44 &&
            event.source.event == 0xFB1E95D9u &&
            event.target.instance == 29 &&
            event.target.event == 0x7C8AA301u;

    bf6_rime_runtime::Address focus{
        route, {}, 45, 0xCBEC4452u};
    ok &= runtime.set(focus, bf6_rime_runtime::Value::from_bool(true), error);
    const auto focused_report = runtime.tick(0.0);
    const auto focused = runtime.emitted_events();
    const bool exact_focus = focused.size() == 1 &&
        focused[0].source.instance == 44 &&
        focused[0].source.event == 0xAA8E5552u &&
        focused[0].target.instance == 30 &&
        focused[0].target.event == 0x7C8AA301u;
    const auto stable_report = runtime.tick(0.0);
    const bool stable = runtime.emitted_events().empty();
    const int fake = bf6_rime_compare_bools(
        context, "common/ui/__control__/not_a_compare", nullptr, 0);

    std::printf("records=%d exact=%d row=%d/%08X/%d/%d/%d/%d initial=%zu/%d "
                "focus=%d/%d stable=%d/%d "
                "compile=%d/%d fake=%d\n",
                count, exact ? 1 : 0,
                count ? rows[0].instance : -1,
                count ? rows[0].input_field : 0,
                count ? rows[0].authored_value : -1,
                count ? rows[0].always_send : -1,
                count ? rows[0].trigger_on_property_change : -1,
                count ? rows[0].trigger_on_start : -1,
                initial.size(), initial_lost ? 1 : 0,
                focused_report.emitted_compare_bool_events,
                exact_focus ? 1 : 0,
                stable_report.emitted_compare_bool_events, stable ? 1 : 0,
                runtime.compile_report().compare_bool_nodes,
                runtime.compile_report().declined_compare_bool_nodes, fake);
    bf6_close(context);
    return ok && initial_lost && exact_focus && stable &&
        focused_report.emitted_compare_bool_events == 1 &&
        focused_report.delivered_compare_bool_events == 1 &&
        stable_report.emitted_compare_bool_events == 0 && fake < 0 ? 0 : 1;
}
