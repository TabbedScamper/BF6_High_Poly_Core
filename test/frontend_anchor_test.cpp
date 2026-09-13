#include "bf6_core.h"

#include <cmath>
#include <cstdio>

namespace {

bool close(float a, float b)
{
    return std::fabs(a - b) < 1.0e-4f;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2 && argc < 4) {
        std::fprintf(stderr,
            "usage: frontend_anchor_test <game_dir> [prefab pin-name ...]\n");
        return 2;
    }
    char err[512] = {};
    bf6_ctx* ctx = bf6_open(argv[1], err, static_cast<int>(sizeof(err)));
    if (!ctx) {
        std::fprintf(stderr, "open failed: %s\n", err);
        return 1;
    }
    if (!bf6_mount_frontend(ctx, err, static_cast<int>(sizeof(err)))) {
        std::fprintf(stderr, "mount failed: %s\n", err);
        bf6_close(ctx);
        return 1;
    }

    const bool customQuery = argc >= 4;
    const char* prefab = customQuery ? argv[2] :
        "game/glacierflow/flow_mainmenu/prefabs/pf_customization";
    const char* pinName = customQuery ? argv[3] : "UIOnlyPosition";
    bf6_frontend_named_transform position{};
    const bool real = !customQuery && bf6_frontend_named_transform_read(
        ctx, prefab, pinName, &position, err,
        static_cast<int>(sizeof(err))) != 0;
    if (customQuery)
    {
        bool any = false;
        for (int argument = 3; argument < argc; ++argument)
        {
            bf6_frontend_named_transform queried{};
            err[0] = 0;
            const bool found = bf6_frontend_named_transform_read(
                ctx, prefab, argv[argument], &queried, err,
                static_cast<int>(sizeof(err))) != 0;
            any = any || found;
            std::printf(
                "%s %s real=%d links=%d controller=%d transform=%d "
                "pos=(%.9g %.9g %.9g) "
                "basis=[%.9g %.9g %.9g | %.9g %.9g %.9g | %.9g %.9g %.9g] "
                "error=%s\n",
                prefab, argv[argument], found ? 1 : 0,
                queried.matching_links, queried.controller_instance,
                queried.transform_instance, queried.transform[9],
                queried.transform[10], queried.transform[11],
                queried.transform[0], queried.transform[1],
                queried.transform[2], queried.transform[3],
                queried.transform[4], queried.transform[5],
                queried.transform[6], queried.transform[7],
                queried.transform[8], found ? "none" : err);
        }
        bf6_close(ctx);
        return any ? 0 : 1;
    }
    bf6_frontend_named_transform control{};
    const bool fake = bf6_frontend_named_transform_read(
        ctx, prefab, "UIOnlyPosition__control", &control, err,
        static_cast<int>(sizeof(err))) != 0;
    /* CASE CONTROL. The property name is hashed case-SENSITIVELY (djb2-exact:
     * seed 5381, multiply 33, XOR the byte). The case-folding sibling djb2
     * coincides with it on already-lowercase input, so the ONLY thing that
     * separates the two on this data is that a folded spelling must resolve
     * NOTHING. Both a lowered and an uppered spelling must fail closed. */
    bf6_frontend_named_transform lower{}, upper{};
    const bool fake_lower = bf6_frontend_named_transform_read(
        ctx, prefab, "uionlyposition", &lower, err,
        static_cast<int>(sizeof(err))) != 0;
    const bool fake_upper = bf6_frontend_named_transform_read(
        ctx, prefab, "UIONLYPOSITION", &upper, err,
        static_cast<int>(sizeof(err))) != 0;
    bf6_frontend_named_transform look_at{};
    const bool have_look_at = bf6_frontend_named_transform_read(
        ctx, prefab, "UIOnlyLookAtEntity", &look_at, err,
        static_cast<int>(sizeof(err))) != 0;

    /* Expected values carry the full decoded precision. Six significant figures
     * is NOT enough against an absolute 1e-4 tolerance: 140.879 differs from
     * the shipped 140.879395 by 3.95e-4 and fails this check on real data. */
    const bool exact = real && !fake && !fake_lower && !fake_upper &&
        position.matching_links == 1 &&
        position.controller_instance == 48 &&
        position.transform_instance == 22 &&
        close(position.transform[0], 0.f) &&
        close(position.transform[2], -1.f) &&
        close(position.transform[3], -0.0876825601f) &&
        close(position.transform[4], 0.996148467f) &&
        close(position.transform[6], 0.996048868f) &&
        close(position.transform[7], 0.0876737908f) &&
        close(position.transform[9], 140.879395f) &&
        close(position.transform[10], 6.8115921f) &&
        close(position.transform[11], -9.56495667f);
    std::printf(
        "UIOnlyPosition real=%d fake=%d fake_lower=%d fake_upper=%d "
        "controller=%d transform=%d "
        "pos=(%.6f %.6f %.6f) forward=(%.6f %.6f %.6f)\n",
        real ? 1 : 0, fake ? 1 : 0, fake_lower ? 1 : 0, fake_upper ? 1 : 0,
        position.controller_instance,
        position.transform_instance, position.transform[9],
        position.transform[10], position.transform[11],
        position.transform[6], position.transform[7], position.transform[8]);
    std::printf(
        "UIOnlyLookAtEntity real=%d controller=%d transform=%d "
        "pos=(%.6f %.6f %.6f)\n",
        have_look_at ? 1 : 0, look_at.controller_instance,
        look_at.transform_instance, look_at.transform[9],
        look_at.transform[10], look_at.transform[11]);
    bf6_close(ctx);
    return exact ? 0 : 1;
}
