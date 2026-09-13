#include "bf6_core.h"
#include "rime_runtime.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

bool close(double left, double right)
{
    return std::fabs(left - right) < 1.0e-5;
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
    if (argc != 2) {
        std::fprintf(stderr,
                     "usage: home_linear_animation_runtime_test <game-dir>\n");
        return 2;
    }
    constexpr const char* kHomeBackground =
        "common/ui/home/screens/home_screen_bg";
    constexpr int kInterpolator = 8;
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

    bf6_rime_runtime::Runtime runtime(context);
    std::string error;
    bf6_rime_float_interpolator authored{};
    if (bf6_rime_float_interpolators(
            context, kHomeBackground, &authored, 1) != 1) {
        std::fprintf(stderr, "authored interpolator read failed\n");
        bf6_close(context);
        return 1;
    }
    if (!runtime.compile(kHomeBackground, 6, error)) {
        std::fprintf(stderr, "compile: %s\n", error.c_str());
        bf6_close(context);
        return 1;
    }
    const auto compiled = runtime.compile_report();
    const bf6_rime_runtime::Address input{
        kHomeBackground, {}, kInterpolator, kInput};
    const bf6_rime_runtime::Address output{
        kHomeBackground, {}, kInterpolator, kOutput};
    const double authored_default = read_real(runtime, output);

    bool ok = close(authored_default, 0.9);
    ok &= runtime.set(input, bf6_rime_runtime::Value::from_real(0.0), error);
    runtime.tick(0.0);
    const double at_start = read_real(runtime, output);
    const double half_duration = static_cast<double>(authored.duration) * 0.5;
    const auto half = runtime.tick(half_duration);
    const double at_half = read_real(runtime, output);
    const auto end = runtime.tick(half_duration);
    const double at_end = read_real(runtime, output);

    /* The one-bit pin is accepted as provider storage because the occurrence
     * is real, but it has no authored edge into the interpolator and therefore
     * cannot change the completed trajectory. */
    bf6_rime_runtime::Address wrong_input = input;
    wrong_input.field ^= 1u;
    ok &= runtime.set(wrong_input,
                      bf6_rime_runtime::Value::from_real(1.0), error);
    const auto control = runtime.tick(0.15);
    const double after_wrong_pin = read_real(runtime, output);

    bf6_rime_runtime::Runtime fake(context);
    const bool fake_compiled = fake.compile(
        "common/ui/home/screens/home_screen_bg__control__", 6, error);

    bf6_rime_runtime::Runtime whole_home(context);
    const bool whole_home_compiled = whole_home.compile(
        /* Match DirectInstallScreenSource's production recursion bound. */
        "common/ui/home/screens/home_screen", 6, error);
    const auto whole = whole_home.compile_report();

    std::printf(
        "linear=%d declined=%d default=%.9g start=%.9g half=%.9g end=%.9g "
        "advanced=%d/%d completed=%d wrong-pin=%.9g fake=%d "
        "whole=%d whole-interpolators=%d/%d/%d/%d\n",
        compiled.linear_interpolator_nodes,
        compiled.declined_interpolator_nodes,
        authored_default, at_start, at_half, at_end,
        half.advanced_float_interpolators,
        end.advanced_float_interpolators,
        end.completed_float_interpolators,
        after_wrong_pin, fake_compiled ? 1 : 0,
        whole_home_compiled ? 1 : 0,
        whole.float_interpolator_nodes,
        whole.linear_interpolator_nodes,
        whole.eased_interpolator_nodes,
        whole.declined_interpolator_nodes);

    ok &= compiled.float_interpolator_nodes == 1 &&
        compiled.linear_interpolator_nodes == 1 &&
        compiled.declined_interpolator_nodes == 0 &&
        close(at_start, 0.9) && close(at_half, 0.45) && close(at_end, 0.0) &&
        half.advanced_float_interpolators == 1 &&
        end.advanced_float_interpolators == 1 &&
        end.completed_float_interpolators == 1 &&
        close(after_wrong_pin, 0.0) && !fake_compiled && control.converged &&
        control.advanced_float_interpolators == 0 &&
        whole_home_compiled && whole.float_interpolator_nodes ==
            whole.linear_interpolator_nodes +
            whole.eased_interpolator_nodes +
            whole.declined_interpolator_nodes;
    bf6_close(context);
    return ok ? 0 : 1;
}
