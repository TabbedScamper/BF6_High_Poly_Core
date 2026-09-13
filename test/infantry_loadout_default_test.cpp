#include "bf6_core.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::string normalized_asset(const char* raw)
{
    std::string value = raw ? raw : "";
    std::replace(value.begin(), value.end(), '\\', '/');
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (value.size() >= 4 &&
        value.compare(value.size() - 4, 4, ".ebx") == 0)
        value.resize(value.size() - 4);
    return value;
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr,
                     "usage: infantry_loadout_default_test <game_dir>\n");
        return 2;
    }
    char err[512] = {};
    bf6_ctx* context = bf6_open(
        argv[1], err, static_cast<int>(sizeof(err)));
    if (!context || !bf6_mount_frontend(
            context, err, static_cast<int>(sizeof(err))))
    {
        std::fprintf(stderr, "open/mount failed: %s\n", err);
        if (context) bf6_close(context);
        return 1;
    }

    const char* roles[] = {"assault", "engineer", "support", "recon"};
    int failures = 0;
    for (const char* role : roles)
    {
        bf6_infantry_loadout_default row{};
        const bool ok = bf6_infantry_loadout_default_read(
            context, role, &row) != 0;
        std::printf("%s ok=%d slots=%d defaults=%d field=%s\n",
                    role, ok ? 1 : 0, row.slot_count,
                    row.resolved_defaults, row.default_field_upgrade);
        for (int slot = 0; slot < row.slot_count; ++slot)
            std::printf("  %d %s -> %s\n", slot,
                        row.slot_asset[slot], row.default_equipment[slot]);
        int allowedTotal = 0;
        for (int slot = 0; slot < row.slot_count; ++slot)
        {
            const int allowed = bf6_equipment_slot_items(
                context, row.slot_asset[slot], nullptr, 0, 0);
            if (allowed <= 0) {
                ++failures;
                continue;
            }
            allowedTotal += allowed;
            std::vector<char> paths(static_cast<size_t>(allowed) * 256u, '\0');
            const int got = bf6_equipment_slot_items(
                context, row.slot_asset[slot], paths.data(), 256, allowed);
            bool containsDefault = false;
            const std::string wanted =
                normalized_asset(row.default_equipment[slot]);
            for (int index = 0; index < got; ++index)
                if (normalized_asset(paths.data() +
                        static_cast<size_t>(index) * 256u) == wanted)
                {
                    containsDefault = true;
                    break;
                }
            std::printf("    allowed=%d default-member=%d\n",
                        got, containsDefault ? 1 : 0);
            if (got != allowed || !containsDefault) ++failures;
        }
        std::printf("  allowed equipment rows=%d\n", allowedTotal);
        if (!ok || row.slot_count != 7 || row.resolved_defaults != 7 ||
            !row.default_field_upgrade[0]) ++failures;
    }
    bf6_infantry_loadout_default control{};
    const int fake = bf6_infantry_loadout_default_read(
        context, "assault__control", &control);
    std::printf("mutated-role-control=%d\n", fake);
    if (fake != 0) ++failures;
    bf6_close(context);
    return failures ? 1 : 0;
}
