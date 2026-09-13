#include "bf6_core.h"

#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: rime_logic_reference_live_test <game>\n");
        return 2;
    }
    constexpr const char* kFocusBorder =
        "common/ui/componentlibrary/components/borders/cl_border_focus";
    constexpr const char* kExpected =
        "common/ui/componentlibrary/animations/cl_animation_blink";
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, sizeof(error));
    if (!context || !bf6_mount_frontend(context, error, sizeof(error)))
    {
        std::fprintf(stderr, "mount: %s\n", error);
        if (context) bf6_close(context);
        return 1;
    }

    const int count = bf6_rime_logic_references(
        context, kFocusBorder, nullptr, 0);
    std::vector<bf6_rime_logic_reference> rows(
        static_cast<size_t>(count > 0 ? count : 0));
    const int filled = count > 0 ? bf6_rime_logic_references(
        context, kFocusBorder, rows.data(), count) : count;
    const int fake = bf6_rime_logic_references(
        context, "common/ui/componentlibrary/components/buttons/__fake__",
        nullptr, 0);
    bf6_close(context);

    int exact = 0;
    int empty = 0;
    for (const bf6_rime_logic_reference& row : rows)
    {
        exact += std::strstr(row.blueprint, kExpected) != nullptr;
        empty += row.blueprint[0] == 0;
        std::printf("instance=%d blueprint=%s\n", row.instance,
                    row.blueprint);
    }
    std::printf("count=%d filled=%d exact=%d empty=%d fake=%d\n",
                count, filled, exact, empty, fake);
    return count > 0 && filled == count && exact == 1 && empty == 0 &&
           fake < 0 ? 0 : 1;
}
