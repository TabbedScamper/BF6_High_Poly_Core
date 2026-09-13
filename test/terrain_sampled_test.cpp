/* Proves Terrain::composite_sampled is bit-for-bit the integer nearest
 * subsample of the existing native composite, on the mounted game's own
 * ground (block 0) and water-surface (block 2) heightfields.
 *
 * Experiment: for every compact pixel, compact[y*N+x] must equal
 *             full[(y*F/N)*F + x*F/N] where full = composite(full, 0).
 *             size, lo[3], hi[3] and world_size_y must match bitwise.
 * Controls:   the same compact grid scored against a lattice shifted by one
 *             full sample (+1 on both axes, clamped) and against the
 *             transposed expectation must each MISMATCH somewhere; an invalid
 *             sample size must be rejected.
 *
 *   terrain_sampled_test <game_dir> <level> [ground_N=1024] [water_N=2048]
 */
#include "source.h"
#include "terrain.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace bf6;

namespace {

using clk = std::chrono::steady_clock;

double ms(clk::time_point a, clk::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

bool same_bits(float a, float b)
{
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}

// Returns true when the block passes; prints the counts either way.
bool check_block(Source& src, const std::string& tree, bool water, int N)
{
    const char* name = water ? "water(block 2)" : "ground(block 0)";
    std::string err;
    std::vector<uint8_t> res = src.get_res(tree, err);
    if (res.empty()) { std::fprintf(stderr, "%s get_res: %s\n", name, err.c_str()); return false; }

    Terrain t;
    if (water ? !t.parse_water_surface(res, err) : !t.parse(res, err))
    { std::fprintf(stderr, "%s parse: %s\n", name, err.c_str()); return false; }
    const int resolved = t.resolve_external([&](const std::string& guid)
    {
        std::string e;
        return src.get_chunk(guid, e);
    });

    const auto t0 = clk::now();
    TerrainGrid compact;
    if (!t.composite_sampled(compact, N, err))
    { std::fprintf(stderr, "%s composite_sampled: %s\n", name, err.c_str()); return false; }
    const auto t1 = clk::now();
    TerrainGrid full;
    if (!t.composite(full, 0, err))
    { std::fprintf(stderr, "%s composite: %s\n", name, err.c_str()); return false; }
    const auto t2 = clk::now();

    const int64_t F = full.size;
    bool bounds = compact.size == N && F > 0 &&
                  same_bits(compact.world_size_y, full.world_size_y) &&
                  compact.heights.size() == (size_t)N * (size_t)N;
    for (int i = 0; i < 3; i++)
        bounds = bounds && same_bits(compact.lo[i], full.lo[i]) && same_bits(compact.hi[i], full.hi[i]);

    auto at = [&](int64_t fy, int64_t fx) { return full.heights[(size_t)(fy * F + fx)]; };
    uint64_t exact_bad = 0, shift_bad = 0, transpose_bad = 0, nonzero = 0;
    int first_x = -1, first_y = -1;
    if (bounds)
    {
        for (int64_t y = 0; y < N; y++)
        {
            const int64_t fy  = y * F / N;
            const int64_t fys = fy + 1 < F ? fy + 1 : F - 1;
            for (int64_t x = 0; x < N; x++)
            {
                const int64_t fx  = x * F / N;
                const int64_t fxs = fx + 1 < F ? fx + 1 : F - 1;
                const uint16_t got = compact.heights[(size_t)(y * N + x)];
                nonzero += got != 0;
                if (got != at(fy, fx))
                {
                    if (!exact_bad) { first_x = (int)x; first_y = (int)y; }
                    exact_bad++;
                }
                shift_bad     += got != at(fys, fxs);
                transpose_bad += got != at(x * F / N, y * F / N);
            }
        }
    }

    const uint64_t total = (uint64_t)N * (uint64_t)N;
    std::printf("%s: nodes %zu, resolved external %d, with values %zu\n",
                name, t.node_count(), resolved, t.nodes_with_values());
    std::printf("  full F=%lld (%.1f MiB, %.1f ms)  compact N=%d (%.2f MiB, %.1f ms)\n",
                (long long)F, (double)full.heights.size() * 2.0 / 1048576.0, ms(t1, t2),
                N, (double)compact.heights.size() * 2.0 / 1048576.0, ms(t0, t1));
    std::printf("  bounds lo %.3f %.3f %.3f hi %.3f %.3f %.3f ysize %.3f: %s\n",
                compact.lo[0], compact.lo[1], compact.lo[2],
                compact.hi[0], compact.hi[1], compact.hi[2], compact.world_size_y,
                bounds ? "PASS" : "FAIL");
    std::printf("  experiment exact subsample: %llu/%llu mismatched (nonzero %llu) %s\n",
                (unsigned long long)exact_bad, (unsigned long long)total,
                (unsigned long long)nonzero, bounds && exact_bad == 0 ? "PASS" : "FAIL");
    if (exact_bad) std::printf("  first mismatch at compact (%d,%d)\n", first_x, first_y);
    std::printf("  control shifted lattice (+1,+1): %llu/%llu mismatched %s\n",
                (unsigned long long)shift_bad, (unsigned long long)total,
                shift_bad ? "PASS/detected" : "FAIL/undetected");
    std::printf("  control transposed: %llu/%llu mismatched %s\n",
                (unsigned long long)transpose_bad, (unsigned long long)total,
                transpose_bad ? "PASS/detected" : "FAIL/undetected");

    TerrainGrid bad;
    std::string bad_err;
    const bool zero_rejected = !t.composite_sampled(bad, 0, bad_err);
    std::printf("  control sample_size 0: %s\n", zero_rejected ? "PASS/rejected" : "FAIL/accepted");

    return bounds && exact_bad == 0 && nonzero > 0 && shift_bad > 0 && transpose_bad > 0 &&
           zero_rejected;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr,
            "usage: terrain_sampled_test <game_dir> <level> [ground_N=1024] [water_N=2048]\n");
        return 2;
    }
    const std::string game = argv[1], level = argv[2];
    const int ground_n = argc > 3 ? std::atoi(argv[3]) : 1024;
    const int water_n  = argc > 4 ? std::atoi(argv[4]) : 2048;

    Source src;
    std::string err;
    if (!src.open(game, err)) { std::fprintf(stderr, "open: %s\n", err.c_str()); return 1; }
    if (!src.mount_level(level, false, err)) { std::fprintf(stderr, "mount: %s\n", err.c_str()); return 1; }

    std::string tree, lvl = level;
    for (char& c : lvl) c = (char)std::tolower((unsigned char)c);
    for (const auto& kv : src.res())
    {
        std::string n = kv.first;
        for (char& c : n) c = (char)std::tolower((unsigned char)c);
        if (n.find("streamingtree") != std::string::npos && n.find(lvl) != std::string::npos)
        { tree = kv.first; break; }
    }
    if (tree.empty()) { std::fprintf(stderr, "no streaming tree for %s\n", level.c_str()); return 1; }
    std::printf("tree: %s\n", tree.c_str());

    const bool ground_ok = check_block(src, tree, false, ground_n);
    const bool water_ok  = check_block(src, tree, true, water_n);
    std::printf("RESULT: %s\n", ground_ok && water_ok ? "PASS" : "FAIL");
    return ground_ok && water_ok ? 0 : 1;
}
