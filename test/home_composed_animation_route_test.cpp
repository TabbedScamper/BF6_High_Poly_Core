#include "install_screen_source.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr const char* kMain =
    "common/ui/universalmenu/logic/mainmenu_universalmenu_logic";
constexpr const char* kForeground = "common/ui/home/screens/home_screen.ebx";
constexpr const char* kBackground =
    "common/ui/home/screens/home_screen_bg";
constexpr const char* kBackgroundEbx =
    "common/ui/home/screens/home_screen_bg.ebx";
constexpr uint32_t kHostInterfaceValue = 0xEB17A5A2u;
constexpr uint32_t kLoaderCondition = 0x77C9D6EFu;
constexpr uint32_t kBackgroundCondition = 0xECC4DA8Fu;
constexpr uint32_t kConditionPin = 0x6B535A76u;
constexpr uint32_t kConditionalOutput = 0xAC996E7Au;
constexpr uint32_t kInterpolatorInput = 0x00597302u;
constexpr uint32_t kInterpolatorOutput = 0x0B87DF4Bu;
constexpr uint32_t kAlpha = 0x0C426471u;

bool close(double left, double right)
{
    return std::fabs(left - right) < 1.0e-5;
}

template <typename Predicate>
int count_matching(const std::vector<bf6_rime_connection>& rows,
                   Predicate predicate)
{
    return static_cast<int>(std::count_if(
        rows.begin(), rows.end(), std::move(predicate)));
}

std::vector<bf6_rime_connection> properties(bf6_ctx* context,
                                             const char* partition)
{
    const int count = bf6_rime_connections(context, partition, nullptr, 0);
    std::vector<bf6_rime_connection> result(
        count > 0 ? static_cast<size_t>(count) : 0u);
    if (count > 0)
        bf6_rime_connections(context, partition, result.data(), count);
    return result;
}

bool edge_is(const bf6_rime_connection& edge, int source,
             uint32_t source_field, int target, uint32_t target_field,
             int mode)
{
    return edge.source == source && edge.source_field == source_field &&
           edge.target == target && edge.target_field == target_field &&
           edge.mode == mode;
}

const rime::Element* element(const rime::Screen& screen, int instance)
{
    const rime::Element* found = nullptr;
    for (const auto& candidate : screen.elements)
    {
        if (candidate.partition != kBackground ||
            candidate.instance != instance) continue;
        if (found) return nullptr;
        found = &candidate;
    }
    return found;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    std::string error;
    auto source = bf6_ui::DirectInstallScreenSource::open(
        argv[1], bf6_ui::DirectInstallScreenSource::MountMode::FrontendOnly,
        error);
    if (!source)
    {
        std::fprintf(stderr, "open: %s\n", error.c_str());
        return 2;
    }
    bf6_ctx* context = source->context();

    const int hub_count = bf6_ui_hub_entries(context, kMain, nullptr, 0);
    std::vector<bf6_ui_hub_entry> hub(
        hub_count > 0 ? static_cast<size_t>(hub_count) : 0u);
    if (hub_count > 0)
        bf6_ui_hub_entries(context, kMain, hub.data(), hub_count);
    const int foreground_imports = static_cast<int>(std::count_if(
        hub.begin(), hub.end(), [](const auto& entry) {
            return entry.instance == 1 &&
                   std::strcmp(entry.blueprint, kForeground) == 0;
        }));
    const int background_imports = static_cast<int>(std::count_if(
        hub.begin(), hub.end(), [](const auto& entry) {
            return entry.instance == 54 &&
                   std::strcmp(entry.blueprint, kBackgroundEbx) == 0;
        }));

    const auto main_edges = properties(context, kMain);
    const auto bg_edges = properties(context, kBackground);
    const int host_to_background = count_matching(
        main_edges, [](const auto& edge) {
            return edge_is(edge, 174, kHostInterfaceValue,
                           54, kBackgroundCondition, 18);
        });
    const int loader_to_background = count_matching(
        main_edges, [](const auto& edge) {
            return edge_is(edge, 1, kLoaderCondition,
                           54, kBackgroundCondition, 18);
        });
    const int shuffled_main = count_matching(
        main_edges, [](const auto& edge) {
            return edge_is(edge, 174, kHostInterfaceValue,
                           54, kLoaderCondition, 18) ||
                   edge_is(edge, 1, kLoaderCondition,
                           54, kHostInterfaceValue, 18);
        });
    const int background_chain =
        count_matching(bg_edges, [](const auto& edge) {
            return edge_is(edge, 7, kBackgroundCondition,
                           5, kConditionPin, 2);
        }) +
        count_matching(bg_edges, [](const auto& edge) {
            return edge_is(edge, 5, kConditionalOutput,
                           8, kInterpolatorInput, 2);
        }) +
        count_matching(bg_edges, [](const auto& edge) {
            return edge_is(edge, 8, kInterpolatorOutput, 3, kAlpha, 2);
        });

    std::vector<bf6_rime_interface_field> main_fields;
    const int field_count =
        bf6_rime_interface_fields(context, kMain, nullptr, 0);
    if (field_count > 0)
    {
        main_fields.resize(static_cast<size_t>(field_count));
        bf6_rime_interface_fields(
            context, kMain, main_fields.data(), field_count);
    }
    const int authored_false_host = static_cast<int>(std::count_if(
        main_fields.begin(), main_fields.end(), [](const auto& field) {
            return field.interface_instance == 174 &&
                   field.field_id == kHostInterfaceValue &&
                   field.value_kind == BF6_RIME_VALUE_BOOL &&
                   field.bool_value == 0;
        }));

    bf6_rime_conditional_float conditional{};
    bf6_rime_float_interpolator interpolator{};
    const int conditionals = bf6_rime_conditional_floats(
        context, kBackground, &conditional, 1);
    const int interpolators = bf6_rime_float_interpolators(
        context, kBackground, &interpolator, 1);

    std::vector<bf6_ui::ScreenIdentity> catalogue;
    const bool enumerated = source->enumerate(catalogue, error);
    const auto identity = std::find_if(
        catalogue.begin(), catalogue.end(), [](const auto& row) {
            return row.path == kBackground;
        });
    auto document = enumerated && identity != catalogue.end()
        ? std::dynamic_pointer_cast<bf6_ui::DirectInstallScreenDocument>(
              source->load(*identity, error))
        : nullptr;

    bool ok = foreground_imports == 1 && background_imports == 1 &&
        host_to_background == 1 && loader_to_background == 1 &&
        shuffled_main == 0 && background_chain == 3 &&
        authored_false_host == 1 && conditionals == 1 && interpolators == 1 &&
        conditional.instance == 5 && conditional.authored_condition == 0 &&
        close(conditional.value_if_false, 0.9) &&
        close(conditional.value_if_true, 1.0) &&
        interpolator.instance == 8 &&
        interpolator.input_field == kInterpolatorInput &&
        interpolator.output_field == kInterpolatorOutput &&
        close(interpolator.default_value, 0.9) &&
        close(interpolator.duration, 0.3) && document;

    int exact_public = 0;
    int mutated_public = 0;
    double initial_alpha = -1.0;
    double half_alpha = -1.0;
    double end_alpha = -1.0;
    int zero_time_settle_ticks = 0;
    if (document)
    {
        const auto* initial = element(document->screen(), 3);
        initial_alpha = initial ? initial->alpha : -1.0;
        exact_public = document->runtime().set_public(
            kBackground, {}, kBackgroundCondition,
            bf6_rime_runtime::Value::from_bool(true), error);
        mutated_public = document->runtime().set_public(
            kBackground, {}, kBackgroundCondition ^ 1u,
            bf6_rime_runtime::Value::from_bool(true), error);
        const bf6_rime_runtime::Address input{
            kBackground, {}, 8, kInterpolatorInput};
        for (; zero_time_settle_ticks < 8; ++zero_time_settle_ticks)
        {
            bf6_ui::DirectScreenTickReport settle{};
            ok &= document->tick_and_apply(0.0, settle, error);
            const auto resolved = document->runtime().get(input);
            if (resolved &&
                resolved->kind == bf6_rime_runtime::ValueKind::Real &&
                close(resolved->real, 1.0))
            {
                ++zero_time_settle_ticks;
                break;
            }
        }
        if (zero_time_settle_ticks < 8)
        {
            bf6_ui::DirectScreenTickReport arm{};
            ok &= document->tick_and_apply(0.0, arm, error);
            ++zero_time_settle_ticks;
        }
        bf6_ui::DirectScreenTickReport half{}, end{};
        ok &= document->tick_and_apply(
            static_cast<double>(interpolator.duration) * 0.5, half, error);
        const auto* midway = element(document->screen(), 3);
        half_alpha = midway ? midway->alpha : -1.0;
        ok &= document->tick_and_apply(
            static_cast<double>(interpolator.duration) * 0.5, end, error);
        const auto* completed = element(document->screen(), 3);
        end_alpha = completed ? completed->alpha : -1.0;
        ok &= half.runtime.advanced_float_interpolators == 1 &&
              end.runtime.completed_float_interpolators == 1;
    }

    const int fake_main = bf6_rime_connections(
        context, "common/ui/__control__/mainmenu_universalmenu_logic",
        nullptr, 0);
    std::printf(
        "imports=%d/%d main=%d/%d host-default-false=%d bg-chain=%d "
        "conditional=%.3g/%.3g interp=%.3g/%.3g "
        "alpha=%.9g/%.9g/%.9g settle=%d public=%d mutated=%d "
        "shuffled=%d fake=%d\n",
        foreground_imports, background_imports,
        host_to_background, loader_to_background, authored_false_host,
        background_chain, conditional.value_if_false,
        conditional.value_if_true, interpolator.default_value,
        interpolator.duration, initial_alpha, half_alpha, end_alpha,
        zero_time_settle_ticks, exact_public, mutated_public,
        shuffled_main, fake_main);
    ok &= exact_public == 1 && mutated_public == 0 &&
          zero_time_settle_ticks > 0 && zero_time_settle_ticks < 8 &&
          close(initial_alpha, 0.9) && close(half_alpha, 0.95) &&
          close(end_alpha, 1.0) && fake_main < 0;
    return ok ? 0 : 1;
}
