#include "bf6_core.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::fprintf(stderr,
            "usage: ebx_instance_imports_test <game> <asset> <instance>\n");
        return 2;
    }
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, static_cast<int>(sizeof(error)));
    if (!context) return 1;
    if (!bf6_mount_all(context, 0, error, static_cast<int>(sizeof(error))))
    {
        bf6_close(context);
        return 1;
    }
    const int instance = std::atoi(argv[3]);
    const int count = bf6_ebx_instance_imports(
        context, argv[2], instance, nullptr, 0);
    std::vector<bf6_ebx_instance_import> rows(
        count > 0 ? static_cast<size_t>(count) : 0u);
    const int filled = count > 0 ? bf6_ebx_instance_imports(
        context, argv[2], instance, rows.data(), count) : count;
    for (const auto& row : rows)
        std::printf("0x%08x %s partition=%s instance=%s\n",
                    row.field_hash, row.path,
                    row.partition_guid, row.instance_guid);
    std::printf("imports=%d filled=%d\n", count, filled);
    bf6_close(context);
    return count >= 0 && filled == count ? 0 : 1;
}
