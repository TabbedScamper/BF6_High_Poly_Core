/* bf6_map_validate: each rule fires on the scene that breaks it, stays quiet on
 * the one that does not, and the list comes back problems first. */
#include "bf6_core.h"
#include "json.hpp"

#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;

static void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

static bf6json::Value run(const std::string& scene)
{
    uint8_t* out = nullptr;
    const int64_t n = bf6_map_validate(scene.c_str(), scene.size(), &out);
    bf6json::Value v;
    if (n < 0 || !out) return v;
    std::string err;
    bf6json::Parser p((const char*)out, (size_t)n);
    p.parse(v, err);
    bf6_blob_free(out);
    return v;
}

static int count(const bf6json::Value& v, int sev, const char* needle, const char* id = nullptr)
{
    const bf6json::Value* items = v.find("items");
    int c = 0;
    if (!items) return 0;
    for (const auto& it : items->arr) {
        const std::string msg = it.find("message")->str;
        if (it.find("severity")->as_int(-1) == sev && msg.find(needle) != std::string::npos
            && (!id || it.find("id")->str == id))
            ++c;
    }
    return c;
}

int main()
{
    uint8_t* bad = nullptr;
    check(bf6_map_validate("not json", 0, &bad) == -1 && bad == nullptr, "unreadable input is refused");

    // Spawners, HQ areas, capture points, sectors.
    auto v = run(R"({"objects":[
        {"id":"hq1","name":"HQ1","type":"HQ_PlayerSpawner","props":{"InfantrySpawns":[]}},
        {"id":"hq2","name":"HQ2","type":"HQ_PlayerSpawner","props":{"InfantrySpawns":["S1","S2"],"HQArea":["A1"]}},
        {"id":"s1","name":"S1","type":"SpawnPoint"},{"id":"s2","name":"S2","type":"SpawnPoint"},
        {"id":"a1","name":"A1","type":"PolygonVolume","loop":[[0,0],[0,10],[10,10],[10,0]]},
        {"id":"ps","name":"PS","type":"PlayerSpawner","props":{"SpawnPoints":"legacy"}},
        {"id":"cp","name":"CP","type":"CapturePoint","props":{"CaptureArea":["CA"],"InfantrySpawnPoints_Team1":["S1"]}},
        {"id":"ca","name":"CA","type":"PolygonVolume","height":0.2},
        {"id":"cp2","name":"CP2","type":"CapturePoint","props":{}},
        {"id":"sec","name":"SEC","type":"Sector","props":{"CapturePoints":[]}},
        {"id":"rof","name":"ROF","type":"RingOfFire","props":{"HardRestrictOBB":"Somewhere"}}
    ]})");
    check(count(v, 0, "no spawn points linked", "hq1") == 1, "a spawner with no spawns is a problem");
    check(count(v, 2, "Only 2 spawn points linked", "hq2") == 1, "two spawns is advice");
    check(count(v, 1, "no HQArea", "hq1") == 1 && count(v, 1, "no HQArea", "hq2") == 0, "HQ area missing only where missing");
    check(count(v, 0, "no spawn points", "ps") == 0, "a legacy reference is skipped quietly");
    check(count(v, 2, "height is nearly zero", "ca") == 1, "a nearly-zero capture area height is advice on the volume");
    check(count(v, 2, "InfantrySpawnPoints_Team1 has only 1 spawn point -", "cp") == 1, "team spawn count advice");
    check(count(v, 0, "no capture area", "cp2") == 1, "a capture point with no area is a problem");
    check(count(v, 1, "every flag shows as 'A'", "sec") == 1, "an empty sector is a warning");
    check(count(v, 0, "HardRestrictOBB", "rof") == 0 && count(v, 0, "RestrictShapeData", "rof") == 1, "ring of fire needs both shapes set");
    {
        const auto* items = v.find("items");
        int last = -1;
        bool sorted = true;
        for (const auto& it : items->arr) {
            const int s = it.find("severity")->as_int(-1);
            sorted = sorted && s >= last;
            last = s;
        }
        check(sorted && !items->arr.empty(), "problems first, then warnings, then advice");
    }

    // Combat volume winding and area, in Godot XZ metres.
    v = run(R"({"objects":[
        {"id":"area","name":"AREA","type":"CombatArea","props":{"CombatVolume":["CW","CCW","HUGE"]}},
        {"id":"cw","name":"CW","type":"PolygonVolume","loop":[[0,0],[10,0],[10,10],[0,10]]},
        {"id":"ccw","name":"CCW","type":"PolygonVolume","loop":[[0,0],[0,10],[10,10],[10,0]]},
        {"id":"huge","name":"HUGE","type":"PolygonVolume","loop":[[0,0],[5000,0],[5000,5000],[0,5000]]},
        {"id":"lonely","name":"LONELY","type":"CombatArea","props":{}}
    ]})");
    // (x2-x1)(z2+z1) over 0,0 -> 10,0 -> 10,10 -> 0,10: 0 + 0 + (-10)(20) + 0 = -200, so this one is CCW.
    check(count(v, 1, "counter-clockwise", "cw") == 1 && count(v, 1, "counter-clockwise", "ccw") == 0, "winding follows the sign of sum (x2-x1)(z2+z1)");
    {
        bool fix = false;
        for (const auto& it : v.find("items")->arr)
            if (it.find("id")->str == "cw") fix = it.find("fix")->str == "winding";
        check(fix, "the winding item carries its fix");
    }
    check(count(v, 0, "over the SDK's 16.6 km2 limit", "huge") == 1, "a combat volume over 16.64 km2 is a problem");
    check(count(v, 2, "no combat volume linked", "lonely") == 1, "an unlinked combat area is advice");

    // Catalogue, scale, ObjIds, upload size.
    v = run(R"({"level":"MP_Test","level_types":["Crate"],"all_types":["Crate","Barrel"],
        "upload_bytes":9500,"upload_limit":10000,
        "objects":[
        {"id":"a","name":"A","type":"Crate","from_library":true,"obj_id":5,"scale":[1,1,1]},
        {"id":"b","name":"B","type":"Crate","from_library":true,"obj_id":5,"scale":[1,2,1]},
        {"id":"c","name":"C","type":"Barrel","from_library":true,"obj_id":7},
        {"id":"d","name":"D","type":"Ghost","from_library":true,"obj_id":7},
        {"id":"e","name":"E","type":"Ghost","obj_id":-1}
    ]})");
    check(count(v, 1, "'Barrel' is not listed for MP_Test", "c") == 1, "a known type off the level list is a warning");
    check(count(v, 0, "'Ghost' is not in this SDK's catalogue", "d") == 1 && count(v, 0, "'Ghost'", "e") == 0, "an unknown type is a problem only from the library");
    check(count(v, 1, "Non-uniform scale (1.00, 2.00, 1.00)", "b") == 1 && count(v, 1, "Non-uniform", "a") == 0, "non-uniform scale is a warning");
    check(count(v, 0, "ObjId 5 is used by 2 objects of the same type") == 1, "the same id in one type is a problem");
    check(count(v, 2, "ObjId 7 is shared by 2 objects of different types") == 1, "the same id across types is advice");
    check(count(v, 1, "close to the 9 KB per-map limit", "") == 1, "near the upload limit is a warning on the map");

    v = run(R"({"objects":[]})");
    check(v.find("items") && v.find("items")->arr.empty(), "an empty map is all clear");

    std::printf("MAP VALIDATE %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
