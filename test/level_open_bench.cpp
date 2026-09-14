// End-to-end level open through the public API, timed, with a digest of every
// placement row, so a faster open can be checked against a slower one.
//
//   level_open_bench <game_dir> <level> [level...]
//
// Run it with BF6_DISABLE_MOUNT_SNAPSHOT=1 and without; the digests must match.
#include "bf6_core.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using clk = std::chrono::steady_clock;
static double since(clk::time_point t) { return std::chrono::duration<double>(clk::now() - t).count(); }

static void mix(uint64_t& h, const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
}
static void mix_str(uint64_t& h, const char* s)
{
    const size_t n = s ? std::strlen(s) : 0;
    mix(h, &n, sizeof(n));
    if (n) mix(h, s, n);
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: level_open_bench <game_dir> <level> [level...]\n"); return 2; }
    char err[512] = {0};
    auto t0 = clk::now();
    bf6_ctx* c = bf6_open(argv[1], err, sizeof(err));
    if (!c) { std::printf("open failed: %s\n", err); return 1; }
    std::printf("bf6_open: %.3f s\n", since(t0));
    for (int a = 2; a < argc; ++a) {
        t0 = clk::now();
        if (bf6_open_level(c, argv[a], nullptr, 0, err, sizeof(err)) != 0) {
            std::printf("%s: open failed: %s\n", argv[a], err);
            continue;
        }
        const double open_s = since(t0);
        int n = bf6_level_instances(c, argv[a], nullptr, 0);
        std::vector<bf6_instance> rows((size_t)(n > 0 ? n : 0));
        if (n > 0) n = bf6_level_instances(c, argv[a], rows.data(), n);
        uint64_t h = 1469598103934665603ull;
        for (int i = 0; i < n; ++i) {
            const bf6_instance& r = rows[(size_t)i];
            mix_str(h, r.res_name);
            mix(h, r.xform, sizeof(r.xform));
            mix(h, &r.material_scope, sizeof(r.material_scope));
            mix_str(h, r.placing_bundle);
            mix_str(h, r.variation);
            mix_str(h, r.source);
        }
        std::printf("%s: bf6_open_level %.3f s, %d placements, digest %016llx\n",
                    argv[a], open_s, n, (unsigned long long)h);
    }
    bf6_close(c);
    return 0;
}
