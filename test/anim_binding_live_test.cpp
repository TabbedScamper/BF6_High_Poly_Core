#include "bf6_core.h"

#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: anim_binding_live_test <game-dir>\n");
        return 2;
    }
    static const char* kClip =
        "animations/glacier/assets/frontend/mainmenu/loadout/"
        "ui_frontend_standing_idle_assault_01";
    static const char* kRig = "animations/kingston/global/rigging/soldier.rig";
    static const char* kSkeleton = "common/characters/_soldier/ske_soldier_3p";
    char error[512] = {};
    bf6_ctx* context = bf6_open(argv[1], error, (int)sizeof(error));
    if (!context) { std::fprintf(stderr, "open: %s\n", error); return 1; }
    if (!bf6_mount_all(context, 0, error, (int)sizeof(error))) {
        std::fprintf(stderr, "mount: %s\n", error); bf6_close(context); return 1;
    }
    bf6_anim_binding_stats stats{};
    const int count = bf6_anim_bindings(context, kClip, kRig, kSkeleton,
                                         nullptr, 0, &stats);
    std::vector<bf6_anim_binding> rows(count > 0 ? (size_t)count : 0);
    bf6_anim_binding_stats filled{};
    const int got = count > 0 ? bf6_anim_bindings(context, kClip, kRig,
        kSkeleton, rows.data(), count, &filled) : count;
    bf6_anim_binding_stats fake{};
    const int fake_count = bf6_anim_bindings(context,
        "animations/glacier/assets/frontend/mainmenu/loadout/__fake_idle__",
        kRig, kSkeleton, nullptr, 0, &fake);
    int suffix_mismatch = 0, bone_rows = 0;
    for (const bf6_anim_binding& row : rows) {
        const size_t length = std::strlen(row.dof_name);
        const char suffix = length > 2 && row.dof_name[length - 2] == '.'
            ? row.dof_name[length - 1] : 0;
        if ((suffix == 'q' && row.component != BF6_ANIM_DOF_QUATERNION) ||
            (suffix == 't' && row.component != BF6_ANIM_DOF_VECTOR3) ||
            (suffix == 's' && row.component != BF6_ANIM_DOF_SCALAR))
            ++suffix_mismatch;
        if (row.bone >= 0) ++bone_rows;
    }
    const bool ok = count == 505 && got == count && fake_count == -1 &&
        filled.map_keys == 505 && filled.rig_keys == 2271 &&
        filled.dof_sets == 35 && filled.exact_span_matches == 35 &&
        filled.dof_records == 2271 && filled.resolved_rig == 505 &&
        filled.resolved_storage == 505 && filled.resolved_defaults == 505 &&
        filled.resolved_bones == bone_rows &&
        filled.quaternion_bones > 0 && filled.vector_bones > 0 &&
        filled.scalar_bones == 0 && suffix_mismatch == 0 &&
        filled.fake_control_hits == 0;
    std::printf("map=%d rig=%d sets=%d spans=%d records=%d resolved=%d/%d "
                "bones=%d defaults=%d q/v/s=%d/%d/%d unresolved=%d/%d suffix-mismatch=%d "
                "fake=%d\n",
        filled.map_keys, filled.rig_keys, filled.dof_sets,
        filled.exact_span_matches, filled.dof_records, filled.resolved_rig,
        filled.resolved_storage, filled.resolved_bones, filled.resolved_defaults,
        filled.quaternion_bones, filled.vector_bones, filled.scalar_bones,
        filled.unresolved_keys, filled.unresolved_bones, suffix_mismatch,
        fake_count);
    bf6_close(context);
    std::printf("RESULT=%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
