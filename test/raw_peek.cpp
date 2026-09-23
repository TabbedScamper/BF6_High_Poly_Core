/* Dump a RES/EBX resource's raw bytes to a file, for inspection.
 *   raw_peek <res|ebx> <name> <out file> */
#include "bf6_core.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: raw_peek <res|ebx> <name> <out>\n"); return 1; }
    const char* game = std::getenv("BF6_GAME") ? std::getenv("BF6_GAME")
        : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Battlefield 6";
    char err[1024] = {};
    bf6_ctx* ctx = bf6_open(game, err, (int)sizeof(err));
    if (!ctx || !bf6_mount_all(ctx, 1, err, (int)sizeof(err))) { std::fprintf(stderr, "%s\n", err); return 2; }
    const uint8_t* raw = nullptr;
    const int64_t n = bf6_read_raw(ctx, std::strcmp(argv[1], "res") == 0 ? BF6_RAW_RES : BF6_RAW_EBX, argv[2], &raw);
    if (n <= 0) { std::fprintf(stderr, "not found\n"); return 3; }
    FILE* f = std::fopen(argv[3], "wb");
    std::fwrite(raw, 1, (size_t)n, f);
    std::fclose(f);
    std::printf("%lld bytes\n", (long long)n);
    bf6_close(ctx);
    return 0;
}
