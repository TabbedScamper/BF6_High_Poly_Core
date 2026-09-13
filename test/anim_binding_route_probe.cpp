#include "bf6_core.h"

#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr,
            "usage: anim_binding_route_probe <game> <clip-ebx> <rig-ebx> [skeleton-ebx]\n");
        return 2;
    }
    const char* skeleton = argc > 4 ? argv[4]
        : "common/characters/_soldier/ske_soldier_3p";
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, sizeof(error));
    if (!context) { std::fprintf(stderr, "open: %s\n", error); return 1; }
    if (!bf6_mount_all(context, 0, error, sizeof(error))) {
        std::fprintf(stderr, "mount: %s\n", error);
        bf6_close(context);
        return 1;
    }
    bf6_anim_binding_stats stats{};
    const int count = bf6_anim_bindings(context, argv[2], argv[3], skeleton,
                                         nullptr, 0, &stats);
    std::vector<bf6_anim_binding> rows(count > 0 ? (size_t)count : 0);
    const int filled = count > 0
        ? bf6_anim_bindings(context, argv[2], argv[3], skeleton, rows.data(),
                            count, &stats)
        : count;
    int q = 0, t = 0, s = 0, suffixMismatch = 0;
    for (const bf6_anim_binding& row : rows) {
        const size_t length = std::strlen(row.dof_name);
        const char suffix = length > 2 && row.dof_name[length - 2] == '.'
            ? row.dof_name[length - 1] : 0;
        if (suffix == 'q') ++q; else if (suffix == 't') ++t;
        else if (suffix == 's') ++s;
        if ((suffix == 'q' && row.component != BF6_ANIM_DOF_QUATERNION) ||
            (suffix == 't' && row.component != BF6_ANIM_DOF_VECTOR3) ||
            (suffix == 's' && row.component != BF6_ANIM_DOF_SCALAR))
            ++suffixMismatch;
    }
    std::printf("count=%d/%d map=%d rig=%d sets=%d spans=%d records=%d "
                "resolved=%d/%d bones=%d defaults=%d suffix=%d/%d/%d "
                "mismatch=%d unresolved=%d/%d\n",
        count, filled, stats.map_keys, stats.rig_keys, stats.dof_sets,
        stats.exact_span_matches, stats.dof_records, stats.resolved_rig,
        stats.resolved_storage, stats.resolved_bones, stats.resolved_defaults,
        q, t, s, suffixMismatch, stats.unresolved_keys,
        stats.unresolved_bones);
    bf6_close(context);
    return count > 0 && filled == count && suffixMismatch == 0 ? 0 : 1;
}
