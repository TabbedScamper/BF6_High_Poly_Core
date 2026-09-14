// Read-ahead throughput: every texture a cached level's meshes name, fetched in
// windows through bf6_precache_texture_chunks.
//   precache_texbench <game_dir> <cache_root> <level> [window]
#include "bf6_core.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 4) { std::fprintf(stderr, "usage: precache_texbench <game_dir> <cache_root> <level> [window]\n"); return 2; }
    const int window = argc > 4 ? std::atoi(argv[4]) : 64;
    char err[1024] = {};
    bf6_precache* c = bf6_precache_open(argv[1], argv[2], err, sizeof err);
    if (!c) { std::fprintf(stderr, "open: %s\n", err); return 1; }
    if (!bf6_precache_map_ready(c, argv[3])) { std::fprintf(stderr, "level not cached\n"); return 1; }
    // Mesh names from the level's placements index are not exposed; ask with the
    // texture names of every mesh the placement rows reference via a wide query.
    std::FILE* f = std::fopen("meshes.txt", "rb");
    if (!f) { std::fprintf(stderr, "put mesh names in meshes.txt\n"); return 1; }
    std::string meshes;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) meshes.append(buf, n);
    std::fclose(f);
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    uint8_t* names_blob = nullptr;
    const int64_t nl = bf6_precache_mesh_texture_names(c, argv[3], meshes.c_str(), &names_blob);
    std::string names_all(nl > 0 ? (const char*)names_blob : "", nl > 0 ? (size_t)nl : 0);
    bf6_blob_free(names_blob);
    std::vector<std::string> names;
    for (size_t s = 0; s < names_all.size();) {
        size_t e = names_all.find('\n', s);
        if (e == std::string::npos) e = names_all.size();
        if (e > s) names.push_back(names_all.substr(s, e - s));
        s = e + 1;
    }
    const double t_names = std::chrono::duration<double>(clk::now() - t0).count();
    auto t1 = clk::now();
    uint64_t bytes = 0;
    for (size_t s = 0; s < names.size(); s += (size_t)window) {
        std::string list;
        for (size_t k = s; k < names.size() && k < s + (size_t)window; ++k) { list += names[k]; list.push_back('\n'); }
        uint8_t* b = nullptr;
        const int64_t len = bf6_precache_texture_chunks(c, list.c_str(), 1024, 0, &b);
        if (len > 0) bytes += (uint64_t)len;
        bf6_blob_free(b);
    }
    const double t_fetch = std::chrono::duration<double>(clk::now() - t1).count();
    std::printf("textures %zu (names %.2f s), fetched %.1f MB in %.2f s = %.0f MB/s, window %d\n",
                names.size(), t_names, bytes / 1048576.0, t_fetch, bytes / 1048576.0 / t_fetch, window);
    bf6_precache_close(c);
    return 0;
}
