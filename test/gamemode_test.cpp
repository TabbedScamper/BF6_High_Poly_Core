/* gamemode_test - read a level's per-mode gameplay entities from the install,
 * then classify each mode the way the builders will see it.
 *
 *   gamemode_test <game dir> <level> [<level> ...]
 *
 * CONTROLS, because a reader that returns numbers is not evidence:
 *   1. The fake-root negative control must be 0: a mode root name with one
 *      character changed must resolve to no partition.
 *   2. A made-up level name must return 0 with an error, not an empty success.
 *   3. Every returned volume must be planar in its own space (Y span ~0 on
 *      all 52 measured by the vector-shape reader) and closed by at least 3
 *      points; a misread Points array breaks this at once.
 *   4. Every spawn must carry a transform whose row1 is (0,1,0): the spawns
 *      are yaw-only (247/247 measured), so a wrong LinearTransform read shows
 *      up here rather than as a plausibly-placed marker.
 *   5. Link edges must point inside the returned array.
 *   6. THE GROUND. Spawns stand on the terrain, so the heightfield sampled
 *      under them must agree with their authored Y: a transposed axis or a
 *      wrong scale shows as tens of metres, not the two or three of a spawn
 *      on a deck or a stair.
 *   7. No mode may claim more than 16 flags after classification (the cap).
 *
 * ORACLES, from the Godot fork's measurements on the same install (CHANGES.md
 * and highpoly_gmmine.gd, 2026-09-01..04), checked when the level matches:
 *   MP_Isolated      conquest        9 flags (the big-flag rescue)
 *   MP_GolmudRailway conquest        12 capture polygons (five authored pairs
 *                                    plus two singles; the builder merges them)
 *   MP_Contaminated  breakthrough    11 capture polygons (the game plays ten)
 *   MP_Isolated      conquest        40 game-authored gem_vehiclespawner rows,
 *                                    using selectors 0,1,4,5,7,8,9,11-14
 * The census per mode is printed so the count can be compared against the
 * GDScript miner's numbers for the same map. */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "bf6_core.h"

static const char* kind_name(int k)
{
    switch (k) {
        case BF6_GM_SPAWN: return "spawn"; case BF6_GM_VOLUME: return "volume";
        case BF6_GM_OBB: return "obb"; case BF6_GM_COMBAT: return "combat";
        case BF6_GM_CAPTURE: return "capture"; case BF6_GM_SECTOR: return "sector";
        case BF6_GM_OBJECTIVE: return "objective"; case BF6_GM_VEHICLE_SPAWN: return "vehicle_spawn";
        case BF6_GM_SOLDIER_SPAWN: return "soldier_spawn"; case BF6_GM_GEM: return "gem";
    }
    return "?";
}

static const char* role_name(int r)
{
    switch (r) {
        case BF6_GMR_SPAWN: return "spawn"; case BF6_GMR_CAPTURE: return "capture"; case BF6_GMR_ZONE: return "zone";
        case BF6_GMR_COMBAT: return "combat"; case BF6_GMR_OBB: return "obb"; case BF6_GMR_VEHICLE: return "vehicle";
        case BF6_GMR_RESUPPLY: return "resupply"; case BF6_GMR_MCOM: return "mcom"; case BF6_GMR_BOMB: return "bomb";
        case BF6_GMR_SPECIALAREA: return "specialarea"; case BF6_GMR_SLOT: return "slot"; case BF6_GMR_UNLINKED: return "unlinked";
    }
    return "?";
}

static std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }

struct Oracle { const char* level; const char* mode; int captures; };
static const Oracle kOracles[] = {
    { "mp_isolated",      "conquest",     9  },
    { "mp_golmudrailway", "conquest",     12 },
    { "mp_contaminated",  "breakthrough", 11 },
};

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: gamemode_test <game> <level> [...]\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* ctx = bf6_open(argv[1], err, sizeof(err));
    if (!ctx) { std::printf("open: %s\n", err); return 1; }

    int fails = 0;
    auto ck = [&](const char* what, bool ok) { std::printf("   %s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; };

    /* control 2: a level that does not exist */
    {
        bf6_gm_stats st{};
        err[0] = 0;
        const int n = bf6_level_gamemodes(ctx, "mp_thisleveldoesnotexist", nullptr, 0, &st, err, sizeof(err));
        std::printf("control fake level: %d rows, err \"%s\"  %s\n", n, err, (n == 0 && err[0]) ? "OK" : "FAIL");
        if (!(n == 0 && err[0])) fails++;
    }

    for (int a = 2; a < argc; a++) {
        const char* level = argv[a];
        const std::string lvl = lower(level);
        bf6_gm_stats st{};
        err[0] = 0;
        const clock_t t0 = clock();
        const int n = bf6_level_gamemodes(ctx, level, nullptr, 0, &st, err, sizeof(err));
        std::printf("\n== %s: %d entities in %.1f s%s%s\n", level, n,
                    (double)(clock() - t0) / CLOCKS_PER_SEC, err[0] ? "  err: " : "", err);
        if (n <= 0) { fails++; continue; }
        std::vector<bf6_gm_entity> rows((size_t)n);
        bf6_level_gamemodes(ctx, level, rows.data(), n, nullptr, err, sizeof(err));

        int layer_rows = bf6_level_gamemode_layers(ctx, level, nullptr, 0, err, sizeof(err));
        std::printf("   layers %d walked (%d skipped), %d listed; modes %d; partitions %d instances %d; "
                    "parse_fail %d missing %d cycles %d unresolved %d\n",
                    st.layers, st.layers_skipped, layer_rows, st.modes, st.partitions, st.instances,
                    st.parse_fail, st.missing, st.cycles, st.unresolved_types);
        std::printf("   spawns %d volumes %d obbs %d combat %d capture %d sector %d objective %d "
                    "vehicle_spawns %d soldier_spawns %d gems %d (%d linked); links %d kept, %d dropped\n",
                    st.spawns, st.volumes, st.obbs, st.combat, st.capture, st.sector, st.objective,
                    st.vehicle_spawns, st.soldier_spawns, st.gems, st.gems_linked, st.links, st.links_dropped);
        std::printf("   control fake root hits: %d  %s\n", st.control_fake_root_hits,
                    st.control_fake_root_hits == 0 ? "OK" : "FAIL");
        if (st.control_fake_root_hits) fails++;

        /* census per mode, raw */
        std::map<std::string, std::map<int, int>> per;
        std::map<std::string, int> link_words;
        int bad_planar = 0, bad_yaw = 0, bad_link = 0, own_xf = 0, gem_own_xf = 0, gem_rows = 0, gem_team = 0;
        std::map<int, int> gem_teams, gem_values;
        float worst_span = 0.f, worst_up = 0.f;
        int isolated_conquest_vehicle_gems = 0;
        int isolated_selector_min = 1000000, isolated_selector_max = -1000000;
        std::map<int, int> isolated_selectors;
        int isolated_capture_gems = 0, isolated_capture_shapes = 0;
        std::vector<int> isolated_hq_order, isolated_capture_order;
        std::set<int> isolated_root_order;
        for (const bf6_gm_entity& e : rows) {
            per[e.mode ? e.mode : "?"][e.kind]++;
            if (e.has_own_transform) own_xf++;
            if (e.kind == BF6_GM_GEM) {
                gem_rows++;
                if (e.has_own_transform) gem_own_xf++;
                if (e.team) gem_team++;
                gem_teams[e.team]++; gem_values[e.gem_value]++;
                link_words[e.gem_link ? (*e.gem_link ? e.gem_link : "(none)") : "(null!)"]++;
                if (lvl == "mp_isolated" && std::strcmp(e.mode ? e.mode : "", "conquest") == 0 &&
                    std::strcmp(e.gem_link ? e.gem_link : "", "gem_vehiclespawner") == 0) {
                    isolated_conquest_vehicle_gems++;
                    isolated_selector_min = std::min(isolated_selector_min, e.gem_value);
                    isolated_selector_max = std::max(isolated_selector_max, e.gem_value);
                    isolated_selectors[e.gem_value]++;
                }
                if (lvl == "mp_isolated" && std::strcmp(e.mode ? e.mode : "", "conquest") == 0 &&
                    std::strcmp(e.gem_link ? e.gem_link : "", "gem_capturepoint") == 0) {
                    isolated_capture_gems++;
                    isolated_capture_order.push_back(e.root_order);
                    if (e.gem_shape && e.gem_shape_property == 0x5C3A072Bu)
                        isolated_capture_shapes++;
                }
                if (lvl == "mp_isolated" && std::strcmp(e.mode ? e.mode : "", "conquest") == 0) {
                    if (e.root_order >= 0) isolated_root_order.insert(e.root_order);
                    if (std::strcmp(e.gem_link ? e.gem_link : "", "gem_hq") == 0)
                        isolated_hq_order.push_back(e.root_order);
                }
            }
            if (e.kind == BF6_GM_VOLUME) {
                if (e.point_count < 3) bad_planar++;
                else {
                    float lo = 1e30f, hi = -1e30f;
                    for (int i = 0; i < e.point_count; i++) { const float y = e.points[i * 3 + 1]; if (y < lo) lo = y; if (y > hi) hi = y; }
                    if (hi - lo > worst_span) worst_span = hi - lo;
                    if (hi - lo > 0.5f) bad_planar++;
                }
            }
            if (e.kind == BF6_GM_SPAWN && e.has_own_transform) {
                const float* m = e.xform;
                const float len = std::sqrt(m[3] * m[3] + m[4] * m[4] + m[5] * m[5]);
                const float up = len > 0.f ? m[4] / len : 0.f;
                if (1.f - up > worst_up) worst_up = 1.f - up;
                if (up < 0.9f) bad_yaw++;
            }
            for (int i = 0; i < e.link_count; i++) if (e.links[i] < 0 || e.links[i] >= n) bad_link++;
        }
        std::map<std::string, int> owners;
        for (const bf6_gm_entity& e : rows)
            if (e.kind == BF6_GM_VOLUME || e.kind == BF6_GM_OBB)
                owners[e.owner_type ? e.owner_type : "(unowned)"]++;
        for (const auto& o : owners) std::printf("   shapes owned by %-40s %d\n", o.first.c_str(), o.second);
        for (const auto& m : per) {
            std::printf("   raw  %-24s", m.first.c_str());
            for (const auto& k : m.second) std::printf(" %s %d", kind_name(k.first), k.second);
            std::printf("\n");
        }
        std::printf("   own transforms %d; volumes with Y span > 0.5 m: %d (worst %.3f m) %s; spawns tilted past 25 deg: %d (worst 1-up %.3f) %s; bad links %d %s\n",
                    own_xf, bad_planar, worst_span, bad_planar == 0 ? "OK" : "FAIL", bad_yaw, worst_up, bad_yaw == 0 ? "OK" : "FAIL",
                    bad_link, bad_link == 0 ? "OK" : "FAIL");
        if (bad_planar || bad_yaw || bad_link) fails++;

        /* the gem read */
        if (gem_rows) {
            std::printf("   gems: %d rows, %d with their own transform, %d with a team; link words:\n", gem_rows, gem_own_xf, gem_team);
            std::vector<std::pair<int, std::string>> lw;
            for (const auto& kv : link_words) lw.push_back({ kv.second, kv.first });
            std::sort(lw.begin(), lw.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
            for (size_t i = 0; i < lw.size() && i < 14; i++) std::printf("      %-44s %d\n", lw[i].second.c_str(), lw[i].first);
            std::printf("      teams:"); for (const auto& kv : gem_teams) std::printf(" %d x%d", kv.first, kv.second); std::printf("\n");
            std::printf("      values:"); for (const auto& kv : gem_values) std::printf(" %d x%d", kv.first, kv.second); std::printf("\n");
            ck("every gem carries a transform of its own", gem_own_xf == gem_rows);
            ck("no gem link is NULL (a gem must read as a gem)", link_words.count("(null!)") == 0);
            if (lvl == "mp_isolated") {
                std::printf("      MP_Isolated conquest vehicle selectors: min %d max %d\n",
                            isolated_selector_min, isolated_selector_max);
                std::printf("      selector census:");
                for (const auto& kv : isolated_selectors) std::printf(" %d x%d", kv.first, kv.second);
                std::printf("\n");
                ck("MP_Isolated Conquest has 40 game-authored vehicle spawners",
                   isolated_conquest_vehicle_gems == 40);
                const std::map<int, int> expected_selectors = {
                    {0, 2}, {1, 2}, {4, 2}, {5, 2}, {7, 4}, {8, 6}, {9, 3},
                    {11, 10}, {12, 1}, {13, 4}, {14, 4}
                };
                ck("MP_Isolated vehicle selector census matches shipped data",
                   isolated_selectors == expected_selectors);
                ck("all 9 capture GEMs bind their game-authored vector shape",
                   isolated_capture_gems == 9 && isolated_capture_shapes == 9);
                std::sort(isolated_hq_order.begin(), isolated_hq_order.end());
                std::sort(isolated_capture_order.begin(), isolated_capture_order.end());
                ck("MP_Isolated HQ order is preserved from root Objects[]",
                   isolated_hq_order == std::vector<int>({1, 2}));
                ck("MP_Isolated capture order is preserved from root Objects[]",
                   isolated_capture_order == std::vector<int>({3, 4, 5, 6, 7, 44, 45, 69, 70}));
                ck("all 94 MP_Isolated Conquest GEMs have unique root order",
                   isolated_root_order.size() == 94 && *isolated_root_order.begin() == 0 &&
                   *isolated_root_order.rbegin() == 93);
            }
        }

        /* ---- the layout, per mode ---- */
        std::printf("   %-22s %6s %7s %5s %6s %6s %5s %5s %5s %5s %5s %5s %5s | dropped junk/owned/box/gem/dup  rescued cap\n",
                    "layout", "objs", "capture", "zone", "combat", "spawn", "veh", "resup", "mcom", "bomb", "spec", "slot", "unlnk");
        int ground_n = 0, ground_far = 0; float ground_med = 0.f;
        std::vector<float> ground_dy;
        for (const auto& m : per) {
            bf6_gm_layout ls{};
            err[0] = 0;
            const int on = bf6_level_gamemode_layout(ctx, level, m.first.c_str(), nullptr, 0, &ls, err, sizeof(err));
            std::vector<bf6_gm_object> objs((size_t)(on > 0 ? on : 0));
            if (on > 0) bf6_level_gamemode_layout(ctx, level, m.first.c_str(), objs.data(), on, nullptr, err, sizeof(err));
            std::printf("   %-22s %6d %7d %5d %6d %6d %5d %5d %5d %5d %5d %5d %5d | %4d/%4d/%3d/%3d/%3d  %3d  %s%s\n",
                        m.first.c_str(), ls.objects, ls.captures, ls.zones, ls.combat, ls.spawns, ls.vehicles, ls.resupply,
                        ls.mcoms, ls.bombs, ls.specialareas, ls.slots, ls.unlinked,
                        ls.dropped_junk, ls.dropped_owned, ls.dropped_prop_box, ls.dropped_gem_other, ls.dropped_dup,
                        ls.big_flag_rescued, ls.sanity_capped ? "CAPPED" : "", on <= 0 ? err : "");
            if (on <= 0) continue;
            if (ls.captures > 16) { std::printf("   FAIL %s claims %d flags after the cap\n", m.first.c_str(), ls.captures); fails++; }
            for (const Oracle& o : kOracles)
                if (lvl == o.level && m.first == o.mode) {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), "oracle %s %s: %d capture polygon(s) (fork measured %d)", level, o.mode, ls.captures, o.captures);
                    ck(buf, ls.captures == o.captures);
                }
            /* control 6: spawns stand on the terrain */
            if (ls.has_terrain)
                for (const bf6_gm_object& o : objs) {
                    if (o.role != BF6_GMR_SPAWN) continue;
                    const bf6_gm_entity& e = rows[(size_t)o.entity];
                    float y = -1e9f, wy = 0.f;
                    const float xz[2] = { e.xform[9], e.xform[11] };
                    if (bf6_level_gamemode_ground(ctx, level, xz, 1, &y, &wy, err, sizeof(err)) == 1 && y > -1e8f)
                        ground_dy.push_back(std::fabs(e.xform[10] - y));
                }
            /* a few rows per mode so a human can eyeball them */
            int shown = 0;
            for (const bf6_gm_object& o : objs) {
                if (o.role == BF6_GMR_SPAWN) continue;
                if (shown++ >= 6) break;
                std::printf("      %-11s %-14s at (%.1f %.1f %.1f) area %.0f%s%s%s%s\n", role_name(o.role), o.label,
                            o.centre[0], o.centre[1], o.centre[2], o.area_m2,
                            o.gem_link ? "  link=" : "", o.gem_link ? o.gem_link : "",
                            o.has_ground ? (o.water ? "  WATER" : "  land") : "", o.has_land ? "  land-pt" : "");
            }
            if (ls.grid_h)
                std::printf("      ground grid %dx%d at %.0f,%.0f step %.0f; water y %.1f\n", ls.grid_nx, ls.grid_nz, ls.grid_x0, ls.grid_z0, ls.grid_step, ls.water_y);
        }
        if (!ground_dy.empty()) {
            std::sort(ground_dy.begin(), ground_dy.end());
            ground_n = (int)ground_dy.size();
            ground_med = ground_dy[(size_t)ground_n / 2];
            const float p90 = ground_dy[(size_t)(ground_n * 9 / 10)];
            for (float d : ground_dy) if (d > 10.f) ground_far++;
            std::printf("   ground under %d spawn(s): median |dy| %.2f m, p90 %.2f m, %d over 10 m\n", ground_n, ground_med, p90, ground_far);
            ck("spawns stand on the sampled terrain (median under 3 m)", ground_med < 3.f);
        } else std::printf("   no heightfield sampled for %s\n", level);
        for (int i = 0; i < st.dropped_link_type_count && i < 6; i++) std::printf("   dropped link types: %s\n", st.dropped_link_types[i]);
        if (st.other_type_count) {
            std::printf("   other top-level types on the mode layers (guid count), top %d:\n",
                        st.other_type_count < 8 ? st.other_type_count : 8);
            for (int i = 0; i < st.other_type_count && i < 8; i++) std::printf("     %s\n", st.other_types[i]);
        }
        for (int i = 0; i < st.spawner_path_count; i++) std::printf("   spawner path: %s\n", st.spawner_paths[i]);
    }
    bf6_close(ctx);
    std::printf("\n%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
