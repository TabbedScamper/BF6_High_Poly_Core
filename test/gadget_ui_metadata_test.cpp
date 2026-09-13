#include "bf6_core.h"

#include <cstdio>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, static_cast<int>(sizeof(error)));
    if (!context) return 1;
    if (!bf6_mount_all(context, 0, error, static_cast<int>(sizeof(error))))
    {
        bf6_close(context);
        return 1;
    }
    const int count = bf6_gadget_ui_metadata_rows(context, nullptr, 0);
    std::vector<bf6_gadget_ui_metadata> rows(
        count > 0 ? static_cast<size_t>(count) : 0u);
    const int filled = count > 0 ? bf6_gadget_ui_metadata_rows(
        context, rows.data(), count) : count;
    int named = 0, direct = 0, atlased = 0;
    for (const auto& row : rows)
    {
        named += row.name[0] ? 1 : 0;
        direct += row.icon_asset[0] ? 1 : 0;
        atlased += row.icon_atlas[0] && row.icon_index >= 0 ? 1 : 0;
        if (row.debug_name[0] && (row.icon_asset[0] || row.icon_atlas[0]))
            std::printf("%d %s | %s | direct=%s atlas=%s[%d]\n",
                row.ordinal, row.debug_name, row.name, row.icon_asset,
                row.icon_atlas, row.icon_index);
    }
    std::printf("rows=%d filled=%d named=%d direct=%d atlased=%d\n",
                count, filled, named, direct, atlased);
    bf6_close(context);
    return count > 0 && filled == count && atlased > 0 ? 0 : 1;
}
