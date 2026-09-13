#include "bf6_cache_identity.h"
#include <iostream>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto start = std::chrono::steady_clock::now();
    const auto result = bf6_cache::inspect(std::filesystem::u8path(argv[1]));
    uint64_t toc_bytes = 0;
    for (const auto& e : result.entries)
        if (e.kind == bf6_cache::Kind::toc) toc_bytes += e.size;
    std::cout << (result.ok ? result.identity : result.error) << "\nfiles="
        << result.entry_count() << " toc_bytes=" << toc_bytes << " seconds="
        << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << "\n";
    return result.ok ? 0 : 1;
}
