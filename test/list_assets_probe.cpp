/* Every EBX partition and resource whose name contains a search term, mounted whole.
 *
 * Written to find the soldier's net-state descriptor: the motion-machine graphs read
 * soldier values through u16 field ids that no graph asset names, and the engine's
 * NetStateDescriptorResource is the compiled list of a replicated object's fields.
 *
 *   list_assets_probe <game_dir> <term> [<term> ...]
 */
#include "bf6_core.h"

#include <cstdio>
#include <string>
#include <vector>

static void list(bf6_ctx* c, const char* term, bool res)
{
    const int n = res ? bf6_list_res(c, term, nullptr, 0) : bf6_list_ebx(c, term, nullptr, 0);
    std::printf("== %s '%s': %d\n", res ? "res" : "ebx", term, n);
    if (n <= 0) return;
    std::vector<bf6_asset> rows((size_t)n);
    const int got = res ? bf6_list_res(c, term, rows.data(), n) : bf6_list_ebx(c, term, rows.data(), n);
    for (int i = 0; i < got && i < 200; ++i)
        std::printf("   %s  (%u bytes)\n", rows[(size_t)i].name ? rows[(size_t)i].name : "?", rows[(size_t)i].size);
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: list_assets_probe <game_dir> <term>...\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::fprintf(stderr, "open: %s\n", err); return 1; }
    if (!bf6_mount_all(c, 1, err, (int)sizeof(err))) std::fprintf(stderr, "note: mount_all said %s\n", err);
    for (int a = 2; a < argc; ++a) { list(c, argv[a], false); list(c, argv[a], true); }
    return 0;
}
