// Live package-list regression. The positive oracle is the current Steam
// M4A1 UIItemDescriptionAsset: eight authored Items, Factory first. The fake
// token proves the lookup does not widen to a similarly named partition.
#include "bf6_core.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 2) { std::printf("usage: weapon_packages_test <game_dir>\n"); return 2; }
    char err[512]{};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c || !bf6_mount_all(c, 0, err, (int)sizeof(err)))
    { std::printf("mount failed: %s\n", err); return 1; }

    const int n = bf6_weapon_packages(c, "m4a1", nullptr, 0);
    std::vector<bf6_weapon_package_row> rows(n > 0 ? (size_t)n : 0);
    const int got = n > 0 ? bf6_weapon_packages(c, "m4a1", rows.data(), n) : n;
    for (const bf6_weapon_package_row& row : rows)
        std::printf("package\t%d\t%s\t%s\t%s\n", row.ordinal, row.key,
                    row.name, row.icon_asset);
    const int fake = bf6_weapon_packages(c, "absent_fake_weapon_7f93", nullptr, 0);
    const bool factoryFirst = !rows.empty() &&
        (std::string(rows[0].name) == "Factory" || std::string(rows[0].key) == "Factory");
    std::printf("m4a1 rows=%d/%d factory-first=%d; fake=%d (negative control)\n",
                n, got, factoryFirst ? 1 : 0, fake);

    const char* equipment =
        "common/hardware/weapons/carbine/m4a1/equipment_m4a1";
    const int cn = bf6_weapon_package_configs(c, equipment, nullptr, 0);
    std::vector<bf6_weapon_package_config> configs(cn > 0 ? (size_t)cn : 0);
    const int cgot = cn > 0
        ? bf6_weapon_package_configs(c, equipment, configs.data(), cn) : cn;
    std::set<std::string> identities;
    int configured = 0;
    for (const bf6_weapon_package_config& config : configs)
    {
        identities.insert(config.unlock_asset);
        configured += config.fit_count > 0 ? 1 : 0;
        std::printf("assembly\t%d\t%s\t%s\tfits=%d\n", config.ordinal,
                    config.skin, config.unlock_asset, config.fit_count);
    }
    std::set<std::string> presentations;
    const char* md = "common/hardware/weapons/carbine/m4a1/md_m4a1";
    for (const bf6_weapon_package_config& config : configs)
    {
        std::vector<bf6_weapon_fit> fits((size_t)config.fit_count);
        for (int i = 0; i < config.fit_count; ++i)
        {
            fits[(size_t)i].slot = config.fits[i].slot;
            fits[(size_t)i].attachment = config.fits[i].attachment;
        }
        std::vector<bf6_weapon_part> parts(128);
        const int pn = bf6_weapon_package_parts(c, md, fits.data(),
            (int)fits.size(), config.skin, parts.data(), (int)parts.size());
        std::string signature;
        for (int i = 0; i < pn && i < (int)parts.size(); ++i)
            if (parts[(size_t)i].bundle)
                signature += std::string(parts[(size_t)i].bundle) + "\n";
        presentations.insert(signature);
        std::printf("presentation\t%d\tparts=%d\tsignature-bytes=%zu\n",
                    config.ordinal, pn, signature.size());
    }
    const int fakeConfig = bf6_weapon_package_configs(
        c, "common/hardware/weapons/carbine/m4a1/equipment_absent_fake", nullptr, 0);
    std::printf("gameplay assemblies=%d/%d unique=%zu configured=%d presentations=%zu; fake=%d control\n",
                cn, cgot, identities.size(), configured, presentations.size(), fakeConfig);
    bf6_close(c);
    return n == 8 && got == 8 && factoryFirst && fake == -1 &&
           cn == 9 && cgot == 9 && identities.size() == 9 && configured == 9 &&
           presentations.size() >= 8 &&
           fakeConfig == -1 ? 0 : 1;
}
