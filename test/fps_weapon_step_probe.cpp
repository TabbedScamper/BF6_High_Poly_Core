/* How long one step of the 1P session graph takes with a given item in the hands.
 *
 * The per-item action probe timed out for every held item (knife, grenade, C4, sidearm,
 * mine) where the rifle's run finishes quickly - so either set_weapon sends the graph
 * somewhere that loops, or one step becomes very slow. Each step's time is printed as it
 * happens, so a hang shows as the last line printed.
 *
 *   fps_weapon_step_probe <game_dir> <item> [steps]
 */
#include "bf6_core.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: fps_weapon_step_probe <game_dir> <item> [steps]\n"); return 2; }
    const int steps = argc > 3 ? std::atoi(argv[3]) : 30;
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    /* As the plugin's binding does (fps_mount): the equipment catalogue needs the
     * frontend mounted, or every held item reads "no weapon blueprint". */
    bf6_mount_frontend(c, err, (int)sizeof(err));
    if (!bf6_mount_all(c, 0, err, (int)sizeof(err))) std::printf("note: mount_all said %s\n", err);
    int32_t w = -1, t = -1;
    if (!bf6_inspect_ids(c, argv[2], &w, &t, err, (int)sizeof(err))) { std::printf("ids: %s\n", err); return 1; }
    std::printf("%s: weapon %d type %d\n", argv[2], w, t);
    std::fflush(stdout);
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    bf6_ant_runtime* rt = bf6_ant_runtime_create(c, "animations/kingston/controllers/.1p.soldier.top.sf",
        "animations/glacier/global/rigging/soldier_1p.rig", "common/characters/_soldier/ske_soldier_1p",
        err, (int)sizeof(err));
    if (!rt) { std::printf("create: %s\n", err); return 1; }
    std::printf("create %.0f ms\n", std::chrono::duration<double, std::milli>(clk::now() - t0).count());
    std::fflush(stdout);
    t0 = clk::now();
    bf6_ant_runtime_set_weapon(rt, w, t);
    std::printf("set_weapon %.0f ms\n", std::chrono::duration<double, std::milli>(clk::now() - t0).count());
    std::fflush(stdout);
    for (int i = 0; i < steps; ++i) {
        t0 = clk::now();
        bf6_ant_runtime_update(rt, 1.0f / 60.0f);
        std::printf("step %2d %.1f ms\n", i, std::chrono::duration<double, std::milli>(clk::now() - t0).count());
        std::fflush(stdout);
    }
    bf6_free(c, rt);
    return 0;
}
