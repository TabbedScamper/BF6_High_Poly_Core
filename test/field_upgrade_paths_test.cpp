#include "bf6_core.h"

#include <array>
#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, (int)sizeof(error));
    if (!context || !bf6_mount_all(context, 0, error, (int)sizeof(error)))
    {
        std::printf("mount failed: %s\n", error);
        return 1;
    }
    const char* roles[] = {"assault", "engineer", "support", "recon"};
    int paths = 0, abilities = 0, named = 0;
    std::array<bf6_field_upgrade_path, 2> assault{};
    bool ok = true;
    for (int role = 0; role < 4; ++role)
    {
        std::array<bf6_field_upgrade_path, 2> rows{};
        const int count = bf6_field_upgrade_paths(
            context, roles[role], rows.data(), (int)rows.size());
        std::printf("%s paths=%d\n", roles[role], count);
        ok = ok && count == 2;
        paths += count > 0 ? count : 0;
        for (int path = 0; path < count && path < 2; ++path)
        {
            ok = ok && rows[(size_t)path].ability_count == 4;
            abilities += rows[(size_t)path].ability_count;
            for (int ability = 0; ability < rows[(size_t)path].ability_count;
                 ++ability)
            {
                const auto& item = rows[(size_t)path].abilities[ability];
                if (item.name[0]) ++named;
                std::printf("  %d.%d %s | %s\n", path + 1, ability + 1,
                            item.id, item.name);
            }
        }
        if (role == 0) assault = rows;
    }
    const char* expected[] = {
        "spawnadrenaline", "featherweight", "fasterhealing", "activeattack"
    };
    int exact = 0, rotated = 0;
    for (int i = 0; i < 4; ++i)
    {
        exact += std::string(assault[0].abilities[i].id) == expected[i];
        rotated += std::string(assault[0].abilities[i].id) == expected[(i + 1) % 4];
    }
    const int fake = bf6_field_upgrade_paths(
        context, "absent_fake_role", nullptr, 0);
    std::printf("paths=%d abilities=%d named=%d exact=%d rotated=%d fake=%d\n",
                paths, abilities, named, exact, rotated, fake);
    bf6_close(context);
    return ok && paths == 8 && abilities == 32 && exact == 4 &&
        rotated == 0 && fake == -1 ? 0 : 1;
}
