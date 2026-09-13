#include "source.h"

#include <chrono>
#include <cstdio>

int main(int argc, char** argv)
{
    if (argc != 2) { std::fprintf(stderr, "usage: armory_index_cache_test <game-dir>\n"); return 2; }
    bf6::Source source;
    std::string error;
    std::fprintf(stderr, "phase=open\n");
    if (!source.open(argv[1], error)) {
        std::fprintf(stderr, "mount: %s\n", error.c_str()); return 1;
    }
    std::fprintf(stderr, "phase=mount\n");
    if (!source.mount_frontend(error)) {
        std::fprintf(stderr, "mount: %s\n", error.c_str()); return 1;
    }
    std::fprintf(stderr, "phase=index\n");
    const auto begin = std::chrono::steady_clock::now();
    const auto& index = source.armory_partition_index();
    const auto& candidates = source.armory_partition_candidates();
    size_t candidate_count = 0;
    for (const auto& row : candidates) candidate_count += row.second.size();
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin).count();
    std::printf("guids=%zu candidates=%zu index_seconds=%.3f\n",
                index.size(), candidate_count, seconds);
    return index.empty() || candidate_count < index.size() ? 1 : 0;
}
