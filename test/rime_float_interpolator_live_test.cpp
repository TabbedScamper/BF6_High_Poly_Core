#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: rime_float_interpolator_live_test <game-dir>\n");
        return 2;
    }
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, sizeof(error));
    if (!context || !bf6_mount_frontend(context, error, sizeof(error))) {
        std::fprintf(stderr, "%s\n", error);
        if (context) bf6_close(context);
        return 2;
    }
    const char* route =
        "game/glacierflow/flow_mainmenu/ui/screens/bootflowbackgroundmovie";
    const int count = bf6_rime_float_interpolators(context, route, nullptr, 0);
    std::vector<bf6_rime_float_interpolator> rows(
        count > 0 ? static_cast<size_t>(count) : 0u);
    const int filled = count > 0
        ? bf6_rime_float_interpolators(context, route, rows.data(), count)
        : count;

    int exact = 0, shuffled = 0, mutated = 0;
    for (const auto& row : rows) {
        const bool payload = row.instance == 27 &&
            std::fabs(row.default_value - 1.f) < 1e-6f &&
            std::fabs(row.duration - .4f) < 1e-6f &&
            row.interpolation_type == 0 && row.interpolation_mode == 1;
        exact += payload && row.input_field == 0x00597302u &&
                 row.output_field == 0x0B87DF4Bu;
        shuffled += payload && row.input_field == 0x0B87DF4Bu &&
                    row.output_field == 0x00597302u;
        mutated += payload && row.input_field == (0x00597302u ^ 0x80000000u);
    }
    const int fake = bf6_rime_float_interpolators(
        context, "game/glacierflow/flow_mainmenu/ui/screens/not_a_screen",
        nullptr, 0);
    std::printf("rows=%d filled=%d exact=%d shuffled=%d mutated=%d fake=%d\n",
                count, filled, exact, shuffled, mutated, fake);
    bf6_close(context);
    return count == 1 && filled == 1 && exact == 1 &&
           shuffled == 0 && mutated == 0 && fake == -1 ? 0 : 1;
}
