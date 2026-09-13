#include "bf6_core.h"
#include "armory.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

static std::string token(const char* raw)
{
    std::string out;
    if (!raw) return out;
    for (const unsigned char ch : std::string(raw))
        if (std::isalnum(ch))
            out.push_back(static_cast<char>(std::tolower(ch)));
    return out;
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, static_cast<int>(sizeof(error)));
    if (!context) return 1;
    if (!bf6_mount_all(context, 0, error, static_cast<int>(sizeof(error))))
    {
        bf6_close(context);
        return 1;
    }

    const int count = bf6_melee_ui_metadata_rows(context, nullptr, 0);
    std::vector<bf6_melee_ui_metadata> rows(
        count > 0 ? static_cast<size_t>(count) : 0u);
    const int filled = count > 0 ? bf6_melee_ui_metadata_rows(
        context, rows.data(), count) : count;

    const int assetCount = bf6_list_ebx(context, "", nullptr, 0);
    std::vector<bf6_asset> assets(
        assetCount > 0 ? static_cast<size_t>(assetCount) : 0u);
    const int assetFilled = assetCount > 0 ? bf6_list_ebx(
        context, "", assets.data(), assetCount) : assetCount;
    std::vector<std::string> names;
    names.reserve(assets.size());
    for (int i = 0; i < assetFilled; ++i)
        if (assets[static_cast<size_t>(i)].name)
            names.emplace_back(assets[static_cast<size_t>(i)].name);
    const bf6::Armory armory = bf6::armory_from_names(names);

    int roster = 0, joined = 0, ambiguous = 0, named = 0, atlased = 0;
    for (const bf6_melee_ui_metadata& row : rows)
    {
        named += row.name[0] ? 1 : 0;
        atlased += row.icon_atlas[0] && row.icon_index >= 0 ? 1 : 0;
        std::printf("%d %s | %s | atlas=%s[%d] ids=%d\n",
            row.ordinal, row.debug_name, row.name, row.icon_atlas,
            row.icon_index, row.item_id_count);
    }
    for (const bf6::ArmoryWeapon& weapon : armory.weapons)
    {
        if (weapon.cls != "melee") continue;
        ++roster;
        const std::string wanted = token(weapon.name.c_str());
        int matches = 0;
        for (const bf6_melee_ui_metadata& row : rows)
            if (token(row.debug_name) == wanted) ++matches;
        if (matches == 1) ++joined;
        else if (matches > 1) ++ambiguous;
    }
    int fake = 0;
    for (const bf6_melee_ui_metadata& row : rows)
        fake += token(row.debug_name) == "bf6meleecontrolnotreal" ? 1 : 0;

    std::printf("rows=%d filled=%d named=%d atlased=%d roster=%d "
                "joined=%d ambiguous=%d fake=%d\n",
                count, filled, named, atlased, roster, joined, ambiguous, fake);
    bf6_close(context);
    return count > 0 && filled == count && roster > 0 && joined == roster &&
           ambiguous == 0 && fake == 0 ? 0 : 1;
}
