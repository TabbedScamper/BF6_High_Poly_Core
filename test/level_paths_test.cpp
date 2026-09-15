/* Level path rules, with no game install.
 *
 * The 1.4.3.0 (Tidal Strike) game update moved the Portal levels under a group
 * folder: game/glacierportal/levels/gr/mp_portal_sand/ and levels/mp/
 * mp_aftermath_portal/, beside the older levels/mp_abbasid/. Both spellings
 * must resolve, and a level's name appearing somewhere else must not.
 *
 *   level_paths_test            exits 0 when every check passes
 */
#include "source.h"

#include <cstdio>
#include <string>

using bf6::Source;

static int checks = 0, failures = 0;
static void expect(bool ok, const char* what)
{
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", what); }
}

int main()
{
    // Level roots, the old layout and the grouped one.
    expect(Source::level_root_tail("game/glaciermp/levels/mp_abbasid/mp_abbasid", "mp_abbasid"), "old layout root");
    expect(Source::level_root_tail("game/glacierportal/levels/gr/mp_portal_sand/mp_portal_sand", "mp_portal_sand"), "grouped root (gr)");
    expect(Source::level_root_tail("game/glacierportal/levels/mp/mp_aftermath_portal/mp_aftermath_portal", "mp_aftermath_portal"), "grouped root (mp)");
    // Controls: things that only look like a root.
    expect(!Source::level_root_tail("game/glacierportal/levels/gr/mp_portal_sand/mp_portal_sand/description", "mp_portal_sand"), "a child of the root is not the root");
    expect(!Source::level_root_tail("game/glacierportal/levels/gr/x/mp_portal_sand/mp_portal_sand", "mp_portal_sand"), "two folders between levels and the level do not count");
    expect(!Source::level_root_tail("game/glaciermp/levels/mp_abbasid_night/mp_abbasid_night", "mp_abbasid"), "a longer level name is not this level");
    expect(!Source::level_root_tail("game/glaciermp/levels/gr/mp_abbasid", "mp_abbasid"), "a group folder alone is not a root");
    expect(!Source::level_root_tail("game/glaciermp/common/mp_abbasid/mp_abbasid", "mp_abbasid"), "no levels folder, no level");
    expect(!Source::level_root_tail("anything", ""), "an empty leaf matches nothing");

    // Level directories.
    const std::string grouped = "game/glacierportal/levels/gr/mp_portal_ocean/_layers_gameplay/conquest";
    const size_t end = Source::level_dir_end(grouped, "mp_portal_ocean");
    expect(end != std::string::npos && grouped.substr(0, end) == "game/glacierportal/levels/gr/mp_portal_ocean", "grouped level directory");
    const std::string flat = "game/glaciermp/levels/mp_dumbo/_layers_gameplay/rush";
    const size_t flat_end = Source::level_dir_end(flat, "mp_dumbo");
    expect(flat_end != std::string::npos && flat.substr(0, flat_end) == "game/glaciermp/levels/mp_dumbo", "old level directory");
    expect(Source::level_dir_end("game/glaciermp/levels/mp_dumbo_2/x", "mp_dumbo") == std::string::npos, "a longer level directory is not this level");
    expect(Source::level_dir_end("game/glaciermp/levels/mp_dumbo", "mp_dumbo") == std::string::npos, "a name ending at the level is not inside its directory");
    expect(Source::level_dir_end("game/glaciermp/levels/a/b/mp_dumbo/x", "mp_dumbo") == std::string::npos, "only one group folder is allowed");

    std::printf("level_paths_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
