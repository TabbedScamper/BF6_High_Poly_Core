/* Enumerate live shader-depot owners matching a caller-provided token. */
#include "source.h"
#include "depot.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 3 && argc != 4) {
        std::printf("usage: bundle_scope_probe <game_dir> <token> [state-key-hex]\n");
        return 2;
    }
    bf6::Source source;
    std::string err;
    if (!source.open(argv[1], err) || !source.mount_frontend(err)) {
        std::printf("mount failed: %s\n", err.c_str());
        return 1;
    }
    const std::string token = argv[2];
    const uint64_t state_key = argc == 4
        ? static_cast<uint64_t>(std::strtoull(argv[3], nullptr, 16)) : 0;
    int matches = 0;
    const std::string exact_depot = source.depot_for_bundle(token);
    if (!exact_depot.empty())
    {
        const std::vector<uint8_t> bytes = source.get_res(exact_depot, err);
        bf6::Depot depot;
        if (!bytes.empty() && depot.parse(bytes, err) &&
            (!state_key || depot.has_key(state_key)))
        {
            std::printf("EXACT %s\t%s\n", token.c_str(), exact_depot.c_str());
            if (state_key)
            {
                const bf6::MaterialBinding binding =
                    depot.textures_for(state_key, bytes);
                const auto& partition_index = source.partition_index();
                for (const auto& row : binding.textures)
                {
                    const auto resolved = partition_index.find(row.second);
                    std::printf("  TEX %08x %-20s %s\n", row.first,
                        bf6::MaterialBinding::display_name(row.first),
                        resolved == partition_index.end()
                            ? row.second.c_str() : resolved->second.c_str());
                }
                for (const auto& row : binding.constants)
                {
                    std::printf("  CONST %08x bytes=%zu data=", row.first,
                                row.second.size());
                    // Material palettes can span several float4 entries. The
                    // old 16-byte cap hid every entry after the first and made
                    // it impossible to verify whether a tint/opacity rule was
                    // uniform across the authored table.
                    const size_t shown = (std::min)(row.second.size(),
                                                     size_t{128});
                    for (size_t i = 0; i < shown; ++i)
                        std::printf("%02x", row.second[i]);
                    std::printf("\n");
                }
            }
            ++matches;
        }
    }
    for (const auto& row : source.depots_by_bundle())
        if (row.first.find(token) != std::string::npos ||
            row.second.find(token) != std::string::npos) {
            if (state_key)
            {
                const std::vector<uint8_t> bytes = source.get_res(row.second, err);
                bf6::Depot depot;
                if (bytes.empty() || !depot.parse(bytes, err) ||
                    !depot.has_key(state_key))
                    continue;
                const bf6::MaterialBinding binding =
                    depot.textures_for(state_key, bytes);
                std::printf("KEY textures=%zu constants=%zu\t",
                            binding.textures.size(), binding.constants.size());
            }
            std::printf("%s\t%s\n", row.first.c_str(), row.second.c_str());
            ++matches;
        }
    std::printf("matches=%d depots=%zu\n", matches,
                source.depots_by_bundle().size());
    return 0;
}
