#include "bf6_core.h"
#include "rime_runtime.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

bool close(double a, double b)
{
    return std::fabs(a - b) < 1.0e-5;
}

double read_real(const bf6_rime_runtime::Runtime& runtime,
                 const bf6_rime_runtime::Address& address)
{
    const auto value = runtime.get(address);
    return value && value->kind == bf6_rime_runtime::ValueKind::Real
        ? value->real : -1000000.0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    constexpr const char* kRoute =
        "common/ui/universalmenu/screens/um_matchmakingscreen";
    constexpr int kInterpolator = 16;
    constexpr uint32_t kInput = 0x00597302u;
    constexpr uint32_t kOutput = 0x0B87DF4Bu;

    char native_error[1024]{};
    bf6_ctx* context = bf6_open(argv[1], native_error, sizeof(native_error));
    if (!context || !bf6_mount_frontend(context, native_error,
                                         sizeof(native_error))) {
        std::fprintf(stderr, "mount: %s\n", native_error);
        if (context) bf6_close(context);
        return 1;
    }

    bf6_rime_float_interpolator authored{};
    const int authored_count = bf6_rime_float_interpolators(
        context, kRoute, &authored, 1);
    bf6_rime_runtime::Runtime runtime(context);
    std::string error;
    const bool compiled_ok = runtime.compile(kRoute, 6, error);
    const auto compiled = runtime.compile_report();
    const bf6_rime_runtime::Address input{
        kRoute, {}, kInterpolator, kInput};
    const bf6_rime_runtime::Address output{
        kRoute, {}, kInterpolator, kOutput};
    const double initial = read_real(runtime, output);
    const bool set = runtime.set(
        input, bf6_rime_runtime::Value::from_real(1.0), error);
    runtime.tick(0.0);
    const double start = read_real(runtime, output);
    const auto half_report = runtime.tick(authored.duration * 0.5);
    const double half = read_real(runtime, output);
    const auto end_report = runtime.tick(authored.duration * 0.5);
    const double end = read_real(runtime, output);

    std::printf("authored=%d type=%d mode=%d duration=%.9g "
                "compiled=%d total=%d linear=%d eased=%d declined=%d "
                "values=%.9g/%.9g/%.9g/%.9g advanced=%d/%d complete=%d\n",
                authored_count, authored.interpolation_type,
                authored.interpolation_mode, authored.duration,
                compiled_ok ? 1 : 0, compiled.float_interpolator_nodes,
                compiled.linear_interpolator_nodes,
                compiled.eased_interpolator_nodes,
                compiled.declined_interpolator_nodes,
                initial, start, half, end,
                half_report.advanced_float_interpolators,
                end_report.advanced_float_interpolators,
                end_report.completed_float_interpolators);

    const bool ok = authored_count == 1 && authored.instance == kInterpolator &&
        authored.interpolation_type == 1 && authored.interpolation_mode == 1 &&
        close(authored.duration, 0.4) && compiled_ok && set &&
        compiled.float_interpolator_nodes ==
            compiled.linear_interpolator_nodes +
            compiled.eased_interpolator_nodes +
            compiled.declined_interpolator_nodes &&
        compiled.eased_interpolator_nodes >= 1 &&
        compiled.declined_interpolator_nodes == 0 &&
        close(initial, 0.0) && close(start, 0.0) && close(half, 0.75) &&
        close(end, 1.0) && half_report.advanced_float_interpolators == 1 &&
        end_report.completed_float_interpolators == 1;
    bf6_close(context);
    return ok ? 0 : 1;
}
