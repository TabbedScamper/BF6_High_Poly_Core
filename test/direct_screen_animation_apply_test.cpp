#include "install_screen_source.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

bool close(double left, double right)
{
    return std::fabs(left - right) < 1.0e-5;
}

const rime::Element* exact_element(const rime::Screen& screen,
                                   const std::string& partition,
                                   int32_t instance)
{
    const rime::Element* result = nullptr;
    for (const rime::Element& element : screen.elements) {
        if (element.partition != partition || element.instance != instance)
            continue;
        if (result) return nullptr;
        result = &element;
    }
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr,
                     "usage: direct_screen_animation_apply_test <game-dir>\n");
        return 2;
    }
    constexpr const char* kRoute = "common/ui/home/screens/home_screen_bg";
    std::string error;
    auto source = bf6_ui::DirectInstallScreenSource::open(
        argv[1], bf6_ui::DirectInstallScreenSource::MountMode::FrontendOnly,
        error);
    if (!source) {
        std::fprintf(stderr, "open: %s\n", error.c_str());
        return 1;
    }

    std::vector<bf6_ui::ScreenIdentity> catalogue;
    if (!source->enumerate(catalogue, error)) return 1;
    const auto identity = std::find_if(
        catalogue.begin(), catalogue.end(), [&](const auto& row) {
            return row.path == kRoute;
        });
    if (identity == catalogue.end()) return 1;
    auto document = std::dynamic_pointer_cast<
        bf6_ui::DirectInstallScreenDocument>(source->load(*identity, error));
    if (!document) {
        std::fprintf(stderr, "load: %s\n", error.c_str());
        return 1;
    }

    bf6_rime_float_interpolator interpolator{};
    const int interpolators = bf6_rime_float_interpolators(
        source->context(), kRoute, &interpolator, 1);
    const int edge_count = bf6_rime_connections(
        source->context(), kRoute, nullptr, 0);
    std::vector<bf6_rime_connection> edges(
        edge_count > 0 ? static_cast<size_t>(edge_count) : 0u);
    if (edge_count > 0)
        bf6_rime_connections(source->context(), kRoute,
                             edges.data(), edge_count);
    int target_instance = -1;
    uint32_t target_field = 0;
    int target_matches = 0;
    for (const auto& edge : edges)
        if (edge.source == interpolator.instance &&
            edge.source_field == interpolator.output_field) {
            target_instance = edge.target;
            target_field = edge.target_field;
            ++target_matches;
        }
    const rime::Element* initial = exact_element(
        document->screen(), kRoute, target_instance);
    const double initial_alpha = initial ? initial->alpha : -1.0;

    const bf6_rime_runtime::Address input{
        kRoute, {}, interpolator.instance, interpolator.input_field};
    bool ok = interpolators == 1 && target_matches == 1 &&
        target_field == rime::property_hash("Alpha") && initial &&
        close(initial_alpha, interpolator.default_value);
    ok &= document->runtime().set(
        input, bf6_rime_runtime::Value::from_real(0.0), error);
    bf6_ui::DirectScreenTickReport start{};
    ok &= document->tick_and_apply(0.0, start, error);
    bf6_ui::DirectScreenTickReport half{};
    ok &= document->tick_and_apply(
        static_cast<double>(interpolator.duration) * 0.5, half, error);
    const rime::Element* midway = exact_element(
        document->screen(), kRoute, target_instance);
    const double half_alpha = midway ? midway->alpha : -1.0;

    bf6_ui::DirectScreenTickReport invalid{};
    const bool negative_delta_accepted =
        document->tick_and_apply(-0.001, invalid, error);
    const rime::Element* after_invalid = exact_element(
        document->screen(), kRoute, target_instance);
    const double control_alpha = after_invalid ? after_invalid->alpha : -1.0;

    bf6_ui::DirectScreenTickReport end{};
    ok &= document->tick_and_apply(
        static_cast<double>(interpolator.duration) * 0.5, end, error);
    const rime::Element* completed = exact_element(
        document->screen(), kRoute, target_instance);
    const double end_alpha = completed ? completed->alpha : -1.0;

    std::printf(
        "interpolators=%d target=%d field=0x%08X initial=%.9g "
        "half=%.9g control=%.9g end=%.9g applied=%d/%d completed=%d "
        "negative-delta=%d\n",
        interpolators, target_matches, target_field, initial_alpha,
        half_alpha, control_alpha, end_alpha,
        half.applied.applied_slots, end.applied.applied_slots,
        end.runtime.completed_float_interpolators,
        negative_delta_accepted ? 1 : 0);
    ok &= close(half_alpha, interpolator.default_value * 0.5) &&
        close(control_alpha, half_alpha) && close(end_alpha, 0.0) &&
        half.applied.applied_slots > 0 && end.applied.applied_slots > 0 &&
        end.runtime.completed_float_interpolators == 1 &&
        !negative_delta_accepted;
    return ok ? 0 : 1;
}
