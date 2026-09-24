/* Run soldier motion-machine graphs frame by frame and show what they store.
 *
 *   soldier_sim_probe <game_dir> <graph[,graph...]> [ticks] [Field=v[:v:v:v] ...]
 *
 * Graph names are full paths (common/gameplay/soldier/traversal/...). Fields are set by
 * the asset's own names before the first tick. Prints each tick's report and, at the end, every field written with its
 * value (or UNKNOWN when the last store's value was not known).
 */
#include "bf6_core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: soldier_sim_probe <game_dir> <graphs> [ticks] [Field=v ...]\n");
        return 2;
    }
    const int ticks = argc > 3 ? std::atoi(argv[3]) : 3;
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    /* Levels too: some soldier graphs ship only in level packages (as the scoreboard mounts). */
    if (!bf6_mount_all(c, 1, err, (int)sizeof(err))) std::printf("note: mount_all said %s\n", err);
    std::printf("soldier fields: %d\n", bf6_soldier_fields_load(c));
    bf6_soldier* s = bf6_soldier_open(c, argv[2], nullptr, 0, err, (int)sizeof(err));
    if (!s) { std::printf("soldier_open: %s\n", err); return 1; }
    for (int i = 4; i < argc; ++i) {
        std::string a = argv[i];
        const size_t eq = a.find('=');
        if (eq == std::string::npos) continue;
        float v[4] = {0, 0, 0, 0};
        int n = 0;
        for (size_t p = eq + 1; p <= a.size() && n < 4;) {
            size_t q = a.find(':', p);
            if (q == std::string::npos) q = a.size();
            v[n++] = std::strtof(a.substr(p, q - p).c_str(), nullptr);
            p = q + 1;
        }
        const std::string name = a.substr(0, eq);
        float probe[4];
        if (bf6_soldier_field_get(name.c_str(), probe) < 0) std::printf("note: no field named %s\n", name.c_str());
        bf6_soldier_field_set(name.c_str(), v, n);
    }
    std::vector<char> buf(1 << 20);
    for (int t = 0; t < ticks; ++t) {
        bf6_soldier_tick(s, 1.0f / 60.0f);
        bf6_soldier_report(s, buf.data(), (int32_t)buf.size());
        std::printf("---- tick %d\n%s\n", t, buf.data());
    }
    bf6_soldier_written(buf.data(), (int32_t)buf.size());
    std::printf("---- fields written\n");
    std::string names = buf.data();
    for (size_t p = 0; p < names.size();) {
        size_t q = names.find('\n', p);
        if (q == std::string::npos) q = names.size();
        const std::string n = names.substr(p, q - p);
        p = q + 1;
        if (n.empty()) continue;
        float v[4] = {0, 0, 0, 0};
        const int k = bf6_soldier_field_get(n.c_str(), v);
        if (k == 1) std::printf("  %-44s %g %g %g %g\n", n.c_str(), v[0], v[1], v[2], v[3]);
        else std::printf("  %-44s UNKNOWN\n", n.c_str());
    }
    bf6_soldier_close(s);
    return 0;
}
