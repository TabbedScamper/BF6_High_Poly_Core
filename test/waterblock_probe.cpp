/* DOES THIS LEVEL HAVE A WATER SURFACE BLOCK AT ALL?
 *
 *   waterblock_probe <game_dir> <level> [level...]
 *
 * bf6_read_water_heightfield returns null for mp_portal_ocean while
 * bf6_read_terrain succeeds, and a null from a reader is ambiguous: it can mean
 * the reader failed to find something that is there, or that the thing is
 * genuinely absent. Those need opposite responses - fix the reader, or fix the
 * caller that assumed every level has one - so the difference has to be
 * measured rather than guessed.
 *
 * The water heightfield is block 2 of the level's streaming-tree resource. This
 * reports, per level, whether that resource exists, whether block 0 (ground)
 * parses, whether block 2 parses, and the exact parser error when it does not.
 */
#include "bf6_core.h"
#include "terrain.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: waterblock_probe <game_dir> <level> [level...]\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_all(c, 1, err, (int)sizeof(err));

    std::printf("%-22s %-10s %-9s %-9s  %s\n",
                "level", "streamtree", "block0", "block2", "block2 error");
    for (int a = 2; a < argc; ++a) {
        const std::string level = argv[a];
        std::string lvl = level;
        for (char& ch : lvl) ch = (char)std::tolower((unsigned char)ch);

        std::string want;
        const int total = bf6_list_res(c, nullptr, nullptr, 0);
        std::vector<bf6_asset> rows((size_t)(total > 0 ? total : 0));
        const int got = total > 0 ? bf6_list_res(c, nullptr, rows.data(), total) : 0;
        for (int i = 0; i < got; ++i) {
            if (!rows[(size_t)i].name) continue;
            std::string n = rows[(size_t)i].name;
            for (char& ch : n) ch = (char)std::tolower((unsigned char)ch);
            if (n.find("streamingtree") != std::string::npos &&
                n.find(lvl) != std::string::npos) { want = rows[(size_t)i].name; break; }
        }
        if (want.empty()) {
            std::printf("%-22s %-10s %-9s %-9s  %s\n", level.c_str(), "MISSING", "-", "-", "");
            continue;
        }
        const uint8_t* d = nullptr;
        const int64_t n = bf6_read_raw(c, BF6_RAW_RES, want.c_str(), &d);
        if (n <= 0 || !d) {
            std::printf("%-22s %-10s %-9s %-9s  %s\n", level.c_str(), "UNREADABLE", "-", "-", "");
            continue;
        }
        const std::vector<uint8_t> res(d, d + n);
        std::string e0, e2;
        bf6::Terrain t0, t2;
        const bool ok0 = t0.parse(res, e0);
        const bool ok2 = t2.parse_water_surface(res, e2);
        std::printf("%-22s %-10lld %-9s %-9s  %s\n", level.c_str(), (long long)n,
                    ok0 ? "ok" : "FAIL", ok2 ? "ok" : "ABSENT", ok2 ? "" : e2.c_str());
    }
    bf6_close(c);
    return 0;
}
