// Runs a real precache build the way an engine would, printing progress.
//   precache_run <game_dir> <cache_root> <Level> [Level...] [--textures-2048] [--sweep]
#include "bf6_core.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 4) { std::printf("usage: precache_run <game_dir> <cache_root> <Level>... [--textures-2048] [--sweep]\n"); return 2; }
    int flags = 0;
    bool sweep = false;
    std::vector<const char*> levels;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--textures-2048")) flags |= BF6_PRECACHE_BUILD_TEXTURES_2048;
        else if (!std::strcmp(argv[i], "--sweep")) sweep = true;
        else levels.push_back(argv[i]);
    }
    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    char err[1024] = {};
    bf6_precache* c = bf6_precache_open(argv[1], argv[2], err, sizeof err);
    const double open_s = std::chrono::duration<double>(clk::now() - t0).count();
    if (!c) { std::printf("open failed: %s\n", err); return 1; }
    std::printf("opened cache %s in %.2f s (install identity scan)\n", bf6_precache_key(c), open_s);
    if (sweep) std::printf("swept %d stale caches\n", bf6_precache_sweep_stale(c, err, sizeof err));

    const auto t1 = clk::now();
    const int rc = bf6_precache_build_start(c, levels.data(), (int)levels.size(), flags);
    if (rc != 0) { std::printf("build_start returned %d\n", rc); return 1; }
    double last_print = -10;
    for (;;) {
        bf6_precache_progress p{};
        p.struct_size = sizeof p;
        bf6_precache_progress_get(c, &p);
        const double t = std::chrono::duration<double>(clk::now() - t1).count();
        if (t - last_print >= 5.0 || p.state == BF6_PRECACHE_DONE || p.state == BF6_PRECACHE_FAILED) {
            last_print = t;
            std::printf("[%7.1f s] %5.1f%%  maps %d/%d  %s  %s  %s  (%.1f s since update)\n", t, p.overall * 100.0,
                        p.maps_done, p.map_count, p.current_map, p.current_layer, p.current_item, p.seconds_since_update);
            std::fflush(stdout);
        }
        if (p.state == BF6_PRECACHE_DONE || p.state == BF6_PRECACHE_FAILED || p.state == BF6_PRECACHE_IDLE && t > 1) {
            if (p.error[0]) std::printf("error: %s\n", p.error);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const int final_state = bf6_precache_build_wait(c);
    const double build_s = std::chrono::duration<double>(clk::now() - t1).count();
    std::printf("build finished state=%d in %.1f s; ready=%d\n", final_state, build_s, bf6_precache_ready(c));
    for (const char* level : levels) {
        std::string lower = level;
        for (char& ch : lower) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        std::ifstream in(std::string(argv[2]) + "/bf6hp-cache/v1/" + bf6_precache_key(c) + "/maps/" + lower + "/stats.json");
        std::stringstream ss; ss << in.rdbuf();
        std::printf("%s\n", ss.str().c_str());
    }
    bf6_precache_close(c);
    return final_state == BF6_PRECACHE_DONE ? 0 : 1;
}
