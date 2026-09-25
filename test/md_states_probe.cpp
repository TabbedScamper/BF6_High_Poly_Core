/* THE GAME STATES A FITTED WEAPON WRITES (bf6_weapon_md_states).
 *
 *   md_states_probe <game> <md partition> [slot=token ...]
 *
 * e.g. md_states_probe <game> common/hardware/weapons/carbine/m4a1/md_m4a1 scp=eotech
 */
#include "bf6_core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: md_states_probe <game> <md partition> [slot=token...]\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_all(c, 1, err, (int)sizeof(err));
    std::vector<std::string> slots, tokens;
    for (int a = 3; a < argc; ++a) {
        const char* eq = std::strchr(argv[a], '=');
        if (!eq) continue;
        slots.emplace_back(argv[a], (size_t)(eq - argv[a]));
        tokens.emplace_back(eq + 1);
    }
    /* "item:<id>" as the partition argument: the configured weapon (factory fits),
     * through bf6_loadout_md_states - what fps_open's md_item uses */
    if (std::strncmp(argv[2], "item:", 5) == 0) {
        char merr[512] = {0};
        const int m = bf6_loadout_md_states(c, argv[2] + 5, "", "", nullptr, 0, merr, (int)sizeof(merr));
        if (m < 0) { std::printf("loadout md states: %s\n", merr); return 1; }
        std::vector<char> t((size_t)m + 1, 0);
        bf6_loadout_md_states(c, argv[2] + 5, "", "", t.data(), (int)t.size(), merr, (int)sizeof(merr));
        std::printf("%s", t.data());
        return 0;
    }
    std::vector<bf6_weapon_fit> fits(slots.size());
    for (size_t i = 0; i < slots.size(); ++i) fits[i] = { slots[i].c_str(), tokens[i].c_str() };
    const int n = bf6_weapon_md_states(c, argv[2], fits.data(), (int)fits.size(), nullptr, 0);
    if (n < 0) { std::printf("md states: failed\n"); return 1; }
    std::vector<char> out((size_t)n + 1, 0);
    bf6_weapon_md_states(c, argv[2], fits.data(), (int)fits.size(), out.data(), (int)out.size());
    std::printf("%s", out.data());
    return 0;
}
