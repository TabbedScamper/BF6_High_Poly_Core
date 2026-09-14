/* bf6_spatial_export: the rules of the Portal SDK's exporter on a small
 * catalogue, and, given a request and an official .spatial.json, a comparison
 * of the two object by object:
 *
 *   spatial_export_test
 *   spatial_export_test <request.json> <official.spatial.json>
 */
#include "bf6_core.h"
#include "json.hpp"
#include "utf8_fs.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using bf6json::Value;

static int failures = 0;

static void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

static std::string jq(const std::string& s)
{
    std::string o = "\"";
    for (char c : s) { if (c == '\\' || c == '"') o.push_back('\\'); o.push_back(c); }
    return o + "\"";
}

static bool parse(const std::string& text, Value& v)
{
    std::string err;
    bf6json::Parser p(text.c_str(), text.size());
    return p.parse(v, err);
}

struct Result { std::string text; Value json, report; };

static Result run(const std::string& req)
{
    Result r;
    uint8_t* out = nullptr, *rep = nullptr;
    const int64_t n = bf6_spatial_export(req.c_str(), req.size(), &out, &rep);
    if (n > 0 && out) { r.text.assign((const char*)out, (size_t)n); parse(r.text, r.json); }
    if (rep) parse((const char*)rep, r.report);
    bf6_blob_free(out);
    bf6_blob_free(rep);
    return r;
}

static const Value* entry(const Value& layers, const char* layer, const std::string& id)
{
    const Value* l = layers.find(layer);
    if (l && l->is_arr())
        for (const Value& e : l->arr) if (const Value* i = e.find("id"); i && i->is_str() && i->str == id) return &e;
    return nullptr;
}

// ---------------------------------------------------------------- comparing with the SDK's file

static std::string show(const Value& v)
{
    char b[64];
    switch (v.type) {
    case Value::Null: return "null";
    case Value::Bool: return v.b ? "true" : "false";
    case Value::Num: std::snprintf(b, sizeof(b), "%.9g", v.num); return b;
    case Value::Str: return jq(v.str);
    case Value::Arr: { std::string s = "["; for (size_t i = 0; i < v.arr.size(); ++i) { if (i) s += ","; s += show(v.arr[i]); } return s + "]"; }
    case Value::Obj: { std::string s = "{"; for (size_t i = 0; i < v.obj.size(); ++i) { if (i) s += ","; s += v.obj[i].first + ":" + show(v.obj[i].second); } return s + "}"; }
    }
    return "";
}

static bool alike(const Value& a, const Value& b, bool as_set)
{
    if (a.type != b.type) return false;
    switch (a.type) {
    case Value::Num: return std::fabs(a.num - b.num) <= 1e-4 * std::max(1.0, std::fabs(b.num));
    case Value::Arr: {
        if (a.arr.size() != b.arr.size()) return false;
        if (as_set) {
            std::multiset<std::string> x, y;
            for (const Value& e : a.arr) x.insert(show(e));
            for (const Value& e : b.arr) y.insert(show(e));
            return x == y;
        }
        for (size_t i = 0; i < a.arr.size(); ++i) if (!alike(a.arr[i], b.arr[i], false)) return false;
        return true;
    }
    case Value::Obj:
        if (a.obj.size() != b.obj.size()) return false;
        for (const auto& kv : b.obj) { const Value* m = a.find(kv.first.c_str()); if (!m || !alike(*m, kv.second, false)) return false; }
        return true;
    default: return show(a) == show(b);
    }
}

static int compare(const std::string& request_path, const std::string& official_path)
{
    std::string req, off;
    if (!bf6fs::read_all(request_path, req) || !bf6fs::read_all(official_path, off)) { std::printf("cannot read inputs\n"); return 2; }
    Result r = run(req);
    Value official;
    if (!parse(off, official)) { std::printf("official file is not JSON\n"); return 2; }
    std::printf("core: %zu bytes, report %s\n", r.text.size(), show(r.report).substr(0, 2000).c_str());
    int diffs = 0;
    for (const char* layer : {"Portal_Dynamic", "Static"}) {
        const Value* ol = official.find(layer), *cl = r.json.find(layer);
        std::set<std::string> seen;
        if (ol && ol->is_arr())
            for (const Value& oe : ol->arr) {
                const std::string id = oe.find("id") ? oe.find("id")->str : std::string();
                seen.insert(id);
                const Value* ce = entry(r.json, layer, id);
                if (!ce) { std::printf("MISSING %s %s\n", layer, id.c_str()); ++diffs; continue; }
                for (const auto& kv : oe.obj) {
                    const Value* cv = ce->find(kv.first.c_str());
                    const bool set = kv.first == "linked" || kv.second.is_arr();
                    if (!cv) { std::printf("  %s: core lacks %s = %s\n", id.c_str(), kv.first.c_str(), show(kv.second).substr(0, 160).c_str()); ++diffs; }
                    else if (!alike(*cv, kv.second, set && kv.first != "points")) {
                        std::printf("  %s: %s core %s official %s\n", id.c_str(), kv.first.c_str(), show(*cv).substr(0, 160).c_str(), show(kv.second).substr(0, 160).c_str());
                        ++diffs;
                    }
                }
                for (const auto& kv : ce->obj)
                    if (!oe.find(kv.first.c_str())) { std::printf("  %s: core adds %s = %s\n", id.c_str(), kv.first.c_str(), show(kv.second).substr(0, 160).c_str()); ++diffs; }
            }
        if (cl && cl->is_arr())
            for (const Value& ce : cl->arr) {
                const std::string id = ce.find("id") ? ce.find("id")->str : std::string();
                if (!seen.count(id)) { std::printf("EXTRA %s %s\n", layer, id.c_str()); ++diffs; }
            }
    }
    std::printf("SPATIAL COMPARE %s (%d differences)\n", diffs ? "DIFFERS" : "MATCHES", diffs);
    return diffs ? 1 : 0;
}

// ---------------------------------------------------------------- rules

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 3) return compare(argv[1], argv[2]);

    const std::string base = bf6fs::join(bf6fs::env("TEMP"), "bf6_spatial_export_test");
    bf6fs::remove_tree(base);
    bf6fs::make_dirs(base);
    const std::string types = bf6fs::join(base, "asset_types.json");
    bf6fs::write_all(types, R"({"AssetTypes":[
        {"type":"HQ_PlayerSpawner","constants":[{"name":"category","value":"spatial"}],"properties":[
            {"name":"Team","type":"selection","default":"Team1","selections":["TeamNeutral","Team1","Team2"]},
            {"name":"AltTeam","type":"selection","default":"Team2","selections":["TeamNeutral","Team1","Team2"]},
            {"name":"HQEnabled","type":"bool","default":true},
            {"name":"ObjId","type":"int","default":-1},
            {"name":"HQArea","type":"PolygonVolume"},
            {"name":"InfantrySpawns","type":"Array[SpawnPoint]"}]},
        {"type":"SpawnPoint","constants":[{"name":"category","value":"spatial"}],"properties":[{"name":"ObjId","type":"int","default":-1}]},
        {"type":"CapturePoint","constants":[{"name":"category","value":"spatial"}],"properties":[
            {"name":"ObjId","type":"int","default":-1},{"name":"PipCamOffset","type":"vector","default":[40,10,0]}],
            "levelRestrictions":["MP_Other"]},
        {"type":"PolygonVolume","constants":[{"name":"category","value":"polygonvolume"}],"properties":[
            {"name":"points","type":"vector[]","default":[]},{"name":"height","type":"float","default":0}]},
        {"type":"OBBVolume","constants":[{"name":"category","value":"obbvolume"}],"properties":[{"name":"size","type":"vector","default":[]}]},
        {"type":"WaypointPath","constants":[{"name":"category","value":"waypointpath"}],"properties":[
            {"name":"points","type":"vector[]","default":[]},{"name":"isClosed","type":"bool","default":false}]},
        {"type":"AI_Spawner","constants":[{"name":"category","value":"spatial"}],"properties":[{"name":"Waypoints","type":"WaypointPath"}]},
        {"type":"CombatArea","constants":[{"name":"category","value":"spatial"}],"properties":[{"name":"CombatVolume","type":"PolygonVolume"}]}
    ]})");

    const std::string head = "{\"level\":\"MP_Test\",\"asset_types\":" + jq(types) + ",";
    const std::string objects = R"("objects":[
        {"key":"TEAM_1_HQ","type":"HQ_PlayerSpawner","id":"\"TEAM_1_HQ\"","origin":[1,2,3],"basis":[1,0,0,0,1,0,0,0,1],
         "props":{"Team":1,"AltTeam":"0","HQEnabled":"true","ObjId":"1","HQArea":"TEAM_1_HQ/HQ_Team1","InfantrySpawns":"SpawnPoint_1_1,Crate"}},
        {"key":"TEAM_1_HQ/HQ_Team1","type":"PolygonVolume","origin":[5,6,7],"points":[[0,0,0],[1,0,0],[1,0,1]],"props":{"height":"10"}},
        {"key":"TEAM_1_HQ/SpawnPoint_1_1","type":"SpawnPoint","origin":[1,2,4],"basis":[0.70710677,0,0.70710677,0,1,0,-0.70710677,0,0.70710677]},
        {"key":"Crate","type":"PolygonVolume","points":[[0,0,0],[1,0,0],[1,0,1]]},
        {"key":"Flag","type":"CapturePoint","props":{"ObjId":-1,"PipCamOffset":"40,10,0"}},
        {"key":"Flag2","type":"CapturePoint"},
        {"key":"Box","type":"OBBVolume","size":"2,3,4"},
        {"key":"Patrol","type":"AI_Spawner","path":{"points":[[0,0,0],[5,0,0]],"closed":true}},
        {"key":"Camera3D","type":"Camera3D"},
        {"key":"HiddenStuff/SpawnPoint","type":"SpawnPoint"},
        {"key":"Dup","type":"SpawnPoint","id":"TEAM_1_HQ"},
        {"key":"Static/MP_Test_Terrain","name":"MP_Test_Terrain","type":"MP_Test_Terrain","static":true,"extra":{"metadata/_edit_lock_":true}}]})";
    Result r = run(head + objects);
    if (bf6fs::env("BF6_SPATIAL_DEBUG") != "") std::printf("%s\n%s\n", r.text.c_str(), show(r.report).c_str());
    const Value& j = r.json;
    const Value* hq = entry(j, "Portal_Dynamic", "TEAM_1_HQ");
    check(hq && hq->find("Team") == nullptr && hq->find("AltTeam") && hq->find("AltTeam")->str == "TeamNeutral",
          "a selection at its default stays out; an index is written by name");
    check(hq && !hq->find("HQEnabled") && hq->find("ObjId") && hq->find("ObjId")->num == 1, "a bool at its default stays out; text numbers are numbers");
    check(hq && hq->find("HQArea") && hq->find("HQArea")->str == "TEAM_1_HQ/HQ_Team1" && hq->find("InfantrySpawns")->arr.size() == 1
          && hq->find("linked")->arr.size() == 2, "links hold ids; a link to the wrong type is dropped; linked lists them");
    check(hq && hq->find("right") && hq->find("position")->find("y")->num == 2, "a spatial object carries its transform");
    const Value* vol = entry(j, "Portal_Dynamic", "TEAM_1_HQ/HQ_Team1");
    check(vol && !vol->find("position") && vol->find("height")->num == 10 && vol->find("points")->arr.size() == 3 && vol->find("points")->arr[1].find("x")->num == 1,
          "a polygon volume is points and height, no transform");
    const Value* flag = entry(j, "Portal_Dynamic", "Flag"), *flag2 = entry(j, "Portal_Dynamic", "Flag2");
    check(flag && !flag->find("ObjId") && !flag->find("PipCamOffset") && flag2 && flag2->find("ObjId") && flag2->find("ObjId")->num == 0,
          "the ObjId shim: -1 drops, unset is 0; a vector at its default stays out");
    const Value* box = entry(j, "Portal_Dynamic", "Box");
    check(box && box->find("size")->arr.size() == 3 && box->find("size")->arr[2].num == 4 && box->find("front"), "an OBB volume keeps its transform and gains a size");
    const Value* patrol = entry(j, "Portal_Dynamic", "Patrol"), *path = entry(j, "Portal_Dynamic", "Patrol/Waypoints");
    check(patrol && patrol->find("Waypoints") && patrol->find("Waypoints")->str == "Patrol/Waypoints" && path && path->find("isClosed")->b
          && path->find("points")->arr.size() == 2 && !path->find("position"), "a waypoint path is its own entity, linked as Waypoints");
    check(!entry(j, "Portal_Dynamic", "Camera3D") && !entry(j, "Portal_Dynamic", "HiddenStuff/SpawnPoint"), "engine nodes and hidden nodes stay out");
    check(entry(j, "Portal_Dynamic", "Dup") != nullptr, "a repeated authored id falls back to the path");
    const Value* ter = entry(j, "Static", "Static/MP_Test_Terrain");
    check(ter && ter->find("metadata/_edit_lock_") && ter->find("type")->str == "MP_Test_Terrain", "a Static entry is written as given");
    const Value* warn = r.report.find("warnings");
    bool restricted = false, wrong = false;
    if (warn && warn->is_arr()) for (const Value& w : warn->arr) {
        if (w.str.find("not usable in MP_Test") != std::string::npos) restricted = true;
        if (w.str.find("wants SpawnPoint") != std::string::npos) wrong = true;
    }
    check(restricted && wrong && r.report.find("skipped")->arr.size() == 2, "the report names restrictions, dropped links and skipped objects");
    check(r.text.find("{\n    \"Portal_Dynamic\": [\n        {\n            \"name\": \"TEAM_1_HQ\"") == 0, "the SDK's four-space layout");
    check(r.text.find("0.70710677") != std::string::npos, "a float stays as short as the scene wrote it");

    Result m = run(head + "\"pretty\":false,\"short_ids\":true," + objects);
    const Value* mhq = entry(m.json, "Portal_Dynamic", "a");
    check(mhq && mhq->find("name")->str == "a" && mhq->find("HQArea")->str == "b" && mhq->find("InfantrySpawns")->arr[0].str == "c"
          && m.text.find('\n') == std::string::npos && m.text.size() < r.text.size() / 2,
          "minified: no whitespace, short names and ids, links renamed with them");

    Result big = run(head + R"("objects":[{"key":"CA","type":"CombatArea","props":{"CombatVolume":"V"}},
        {"key":"V","type":"PolygonVolume","points":[[0,0,0],[5000,0,0],[5000,0,5000],[0,0,5000]]},
        {"key":"Tiny","type":"PolygonVolume","points":[[0,0,0],[1,0,0]]}]})");
    const Value* errs = big.report.find("errors");
    check(errs && errs->arr.size() == 2 && !entry(big.json, "Portal_Dynamic", "Tiny"), "a too-large combat area and a two-point volume are errors");

    bf6fs::remove_tree(base);
    std::printf("SPATIAL EXPORT %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
