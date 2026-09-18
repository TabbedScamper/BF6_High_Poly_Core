/* HOW MUCH OF A THING DOES THE GAME SHIP?
 *
 *   asset_count_probe <game_dir> <level> <substring> [substring ...]
 *
 * A study tool, not a feature test. Before building anything the question is
 * what it is worth, and that means counting what the install actually carries
 * rather than guessing from the board or from memory. This counts EBX and RES
 * names containing each substring, and shows a sample of each so a count can be
 * sanity-checked against what the names actually are.
 *
 * Mounts everything, because assets under common/ do not belong to any level.
 */
#include "bf6_core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 4) { std::printf("usage: asset_count_probe <game> <level> <substr>...\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    if (!bf6_mount_all(c, 1, err, (int)sizeof(err)))
        std::printf("note: mount_all said %s\n", err);

    /* EBX AND RES SEPARATELY. A name can be listed as an EBX partition and have
     * no RES behind it in this mount, which is the difference between "the game
     * ships this" and "this reader can open it" - and reporting only the EBX
     * count would make the second look like the first. */
    const int res_total = bf6_list_res(c, nullptr, nullptr, 0);
    std::vector<bf6_asset> res_all((size_t)(res_total > 0 ? res_total : 1));
    const int res_got = res_total > 0 ? bf6_list_res(c, nullptr, res_all.data(), res_total) : 0;
    std::printf("mount carries %d RES entr(ies)\n\n", res_got);

    std::printf("%-46s %8s %8s  %s\n", "substring", "EBX", "RES", "sample");
    for (int a = 3; a < argc; ++a)
    {
        const char* want = argv[a];
        int res_hits = 0;
        for (int i = 0; i < res_got; ++i)
            if (res_all[(size_t)i].name && std::strstr(res_all[(size_t)i].name, want)) res_hits++;
        const int n = bf6_list_ebx(c, want, nullptr, 0);
        std::vector<bf6_asset> rows((size_t)(n > 0 ? n : 1));
        const int got = n > 0 ? bf6_list_ebx(c, want, rows.data(), n) : 0;
        const char* sample = (got > 0 && rows[0].name) ? rows[0].name : "-";
        /* The tail of the first name, because the interesting part of an asset
         * path is the end and the head is a directory everything shares. */
        const char* tail = std::strrchr(sample, '/');
        std::printf("%-46s %8d %8d  %s\n", want, n, res_hits, tail ? tail + 1 : sample);
    }
    bf6_close(c);
    return 0;
}
