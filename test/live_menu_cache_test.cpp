#include "live_menu_cache.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static bool expect(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

int main() {
    bool ok = true;
    const fs::path root = fs::temp_directory_path() / L"bf6_live_menu_cache_control";
    std::error_code ec;
    fs::create_directories(root, ec);
    {
        std::ofstream payload(root / L"7", std::ios::binary);
        payload << "jpeg";
    }
    for (const wchar_t* name : {L"10", L"11", L"12", L"13", L"14", L"15"}) {
        std::ofstream payload(root / name, std::ios::binary);
        payload << "jpeg";
    }
    {
        std::ofstream manifest(root / L"manifest", std::ios::binary);
        manifest << R"({"version":2,"entries":[)"
                 << R"({"url":"https://eaassets-a.akamaihd.net/battlelog/battlebinary/glacier/navTiles/real.jpg","id":7,"lastUsed":99,"size":4},)"
                 << R"({"url":"https://eaassets-a.akamaihd.net/battlelog/battlebinary/glacier/navBulletins/newest.jpg","id":10,"lastUsed":103,"size":4},)"
                 << R"({"url":"https://eaassets-a.akamaihd.net/battlelog/battlebinary/glacier/navBulletins/same-batch.jpg","id":11,"lastUsed":101,"size":4},)"
                 << R"({"url":"https://eaassets-a.akamaihd.net/battlelog/battlebinary/glacier/navBulletins/old.jpg","id":12,"lastUsed":100,"size":4},)"
                 << R"({"url":"https://eaassets-a.akamaihd.net/battlelog/battlebinary/glacier/navBackgrounds/FE_MBS_Portal_Row_A.jpg","id":13,"lastUsed":105,"size":4},)"
                 << R"({"url":"https://eaassets-a.akamaihd.net/battlelog/battlebinary/glacier/navBackgrounds/unrelated.jpg","id":14,"lastUsed":105,"size":4},)"
                 << R"({"url":"https://eaassets-a.akamaihd.net/battlelog/battlebinary/glacier/navBackgrounds/FE_MBS_MnM_Capstone_Conquest.jpg","id":15,"lastUsed":90,"size":4},)"
                 << R"({"url":"https://evil.example/battlelog/battlebinary/glacier/navTiles/reject.jpg","id":8,"lastUsed":100,"size":4},)"
                 << R"({"url":"https://eaassets-a.akamaihd.net/battlelog/battlebinary/glacier/navTiles/evicted.jpg","id":9,"lastUsed":101,"size":4})"
                 << "]}" << '\0';
    }
    const live_menu::Catalog fixture = live_menu::read((root / L"manifest").wstring());
    ok &= expect(fixture.error.empty(), "fixture manifest rejected");
    ok &= expect(fixture.manifest_entries == 9, "fixture entry count differs");
    ok &= expect(fixture.known_assets.size() == 8,
                 "allow-listed manifest row count differs");
    ok &= expect(fixture.assets.size() == 7, "real/fake/evicted gate differs");
    ok &= expect(fixture.rejected_entries == 1, "mutated host was not rejected");
    ok &= expect(fixture.evicted_entries == 1, "missing payload was not rejected");
    std::vector<uint8_t> bytes;
    std::string error;
    ok &= expect(!fixture.assets.empty() &&
                 live_menu::read_payload(fixture.assets.front(), bytes, error) &&
                 bytes.size() == 4, "exact fixture payload did not round-trip");
    const std::vector<live_menu::Asset> cohort =
        live_menu::newest_cohort(fixture, "navBulletins", 2);
    ok &= expect(cohort.size() == 2,
                 "newest bulletin cohort did not preserve its access batch");
    ok &= expect(cohort.size() == 2 && cohort[0].id == 10 && cohort[1].id == 11,
                 "newest bulletin cohort order changed");
    ok &= expect(live_menu::newest_cohort(
                     fixture, "__control_missing", 2).empty(),
                 "fabricated bulletin family selected a cache asset");
    const std::vector<live_menu::Asset> portal =
        live_menu::newest_cohort_matching(
            fixture, "navBackgrounds", 0, "fe_mbs_portal_row_");
    ok &= expect(portal.size() == 1 && portal.front().id == 13,
                 "Portal identity filter did not retain the exact cache row");
    ok &= expect(live_menu::newest_cohort_matching(
                     fixture, "navTiles", 0, "fe_mbs_portal_row_").empty(),
                 "shuffled Portal family selected a cache asset");
    ok &= expect(live_menu::newest_cohort_matching(
                     fixture, "navBackgrounds", 0,
                     "__control_missing").empty(),
                 "fabricated Portal identity selected a cache asset");
    ok &= expect(live_menu::newest_cohort_matching(
                     fixture, "navBackgrounds", 2,
                     "capstone_conquest").empty(),
                 "old access time leaked into newest family cohort");
    const std::vector<live_menu::Asset> stableIdentity =
        live_menu::matching(fixture, "navBackgrounds", "capstone_conquest");
    ok &= expect(stableIdentity.size() == 1 && stableIdentity.front().id == 15,
                 "stable exact identity was incorrectly access-time gated");
    ok &= expect(live_menu::matching(
                     fixture, "navTiles", "capstone_conquest").empty(),
                 "stable exact identity ignored its provider family");
    const std::vector<live_menu::Asset> knownEvicted =
        live_menu::matching_known(fixture, "navTiles", "evicted.jpg");
    ok &= expect(knownEvicted.size() == 1 && knownEvicted.front().id == 9,
                 "evicted allow-listed CDN identity was discarded");
    live_menu::Asset rejectedOnline = knownEvicted.front();
    rejectedOnline.url = "https://evil.example/evicted.jpg";
    bytes.clear(); error.clear();
    ok &= expect(!live_menu::read_online_payload(
                     rejectedOnline, bytes, error) && bytes.empty(),
                 "mutated online host escaped CDN policy");
    fs::remove_all(root, ec);

    const live_menu::Catalog current = live_menu::discover();
    if (!current.error.empty()) {
        std::fprintf(stderr, "live manifest unavailable: %s\n", current.error.c_str());
        return ok ? 0 : 1;
    }
    std::printf("live manifest entries=%d exact=%zu evicted=%d rejected=%d\n",
                current.manifest_entries, current.assets.size(),
                current.evicted_entries, current.rejected_entries);
    ok &= expect(current.manifest_entries > 0, "live manifest is empty");
    ok &= expect(!current.assets.empty(), "no exact live menu assets remain");
    bytes.clear(); error.clear();
    ok &= expect(read_payload(current.assets.front(), bytes, error),
                 "newest exact live payload failed to read");
    return ok ? 0 : 1;
}
