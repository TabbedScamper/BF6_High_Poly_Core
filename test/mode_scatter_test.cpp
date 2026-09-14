/* bf6_mode_plan and bf6_scatter_layout: the pieces come out named, numbered and
 * linked as the Unreal SDK built them; a scatter is deterministic, spaced,
 * inside its shape, and honours the ground filter. */
#include "bf6_core.h"
#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;

static void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

static std::string raw_plan(const std::string& req)
{
    uint8_t* out = nullptr;
    const int64_t n = bf6_mode_plan(req.c_str(), req.size(), &out);
    std::string s = n > 0 && out ? std::string((const char*)out, (size_t)n) : std::string();
    bf6_blob_free(out);
    return s;
}

static bf6json::Value parse(const std::string& s)
{
    bf6json::Value v;
    std::string err;
    bf6json::Parser p(s.c_str(), s.size());
    p.parse(v, err);
    return v;
}

static const bf6json::Value* obj_named(const bf6json::Value& plan, const char* name)
{
    const auto* objs = plan.find("objects");
    if (!objs) return nullptr;
    for (const auto& o : objs->arr)
        if (o.find("name")->str == name) return &o;
    return nullptr;
}

static int ground_calls = 0;
static int ground_fn(void*, double x, double, double, int terrain_only, double* y)
{
    ++ground_calls;
    if (terrain_only && x > 0.0) return 0;   // "roofs" east of the origin
    *y = 7.0;
    return 1;
}

static std::string layout(const std::string& req, bf6_scatter_ground_fn g = ground_fn)
{
    uint8_t* out = nullptr;
    const int64_t n = bf6_scatter_layout(req.c_str(), req.size(), g, nullptr, &out);
    std::string s = n > 0 && out ? std::string((const char*)out, (size_t)n) : std::string();
    bf6_blob_free(out);
    return s;
}

int main()
{
    // ---- bundles
    auto list = parse(raw_plan(R"({"op":"bundles"})"));
    check(list.find("bundles") && list.find("bundles")->arr.size() == 5, "five bundles offered");

    auto hq = parse(raw_plan(R"({"op":"bundle","key":"HQ2","at":[10,2,30],"used_ids":[301,302,5]})"));
    check(hq.find("root")->str == "Team2_HQ_303", "an HQ takes the next free id in the 300s and names itself by it");
    const auto* hqo = obj_named(hq, "Team2_HQ_303");
    check(hqo && hqo->find("type")->str == "HQ_PlayerSpawner" && hqo->find("props")->find("Team")->num == 2
          && hqo->find("props")->find("AltTeam")->num == 1 && hqo->find("props")->find("ObjId")->num == 303, "HQ props");
    check(hqo && hqo->find("links")->find("InfantrySpawns")->arr.size() == 4
          && hqo->find("links")->find("HQArea")->arr[0].str == "Team2_HQ_303_Area", "HQ links its four spawns and its area");
    const auto* sp1 = obj_named(hq, "Team2_HQ_303_Spawn1");
    check(sp1 && std::fabs(sp1->find("at")->arr[0].num - (10 + 5 * std::cos(3.14159265358979 / 4))) < 1e-4
          && std::fabs(sp1->find("at")->arr[2].num - (30 + 5 * std::sin(3.14159265358979 / 4))) < 1e-4
          && sp1->find("parent")->str == "Team2_HQ_303", "spawns ring the HQ 5 m out, under it");
    const auto* area = obj_named(hq, "Team2_HQ_303_Area");
    check(area && area->find("volume") && area->find("volume")->find("height")->num == 10
          && area->find("volume")->find("points")->arr[2].arr[0].num == 8, "the HQ area is a 16 m square, 10 m tall");

    auto flag = parse(raw_plan(R"({"op":"bundle","key":"FLAG","at":[0,0,0],"used_ids":[200,201]})"));
    check(flag.find("root")->str == "CapturePoint_C", "a flag's letter follows its id (202 = C)");
    const auto* cp = obj_named(flag, "CapturePoint_C");
    check(cp && cp->find("links")->find("InfantrySpawnPoints_Team1")->arr.size() == 4
          && cp->find("links")->find("InfantrySpawnPoints_Team2")->arr.size() == 4, "a flag splits eight spawns between teams");
    const auto* fs = obj_named(flag, "CapturePoint_C_Spawn1");
    check(fs && std::fabs(fs->find("facing")->arr[0].num + 1.0) < 1e-6, "flag spawns face the flag");

    auto sec = parse(raw_plan(R"({"op":"bundle","key":"SECTOR","at":[0,0,0],"used_ids":[100]})"));
    check(sec.find("root")->str == "Sector_2" && obj_named(sec, "Sector_2_SectorArea"), "a sector bundle brings its area");
    auto mcom = parse(raw_plan(R"({"op":"bundle","key":"MCOM","at":[0,0,0],"used_ids":[400]})"));
    check(mcom.find("root")->str == "MCOM_2", "MCOMs count up from 400");

    // ---- wizard
    auto w0 = parse(raw_plan(R"({"op":"wizard","mode":"Conquest","count":3,"step":0})"));
    check(w0.find("total")->num == 5 && w0.find("title")->str == "Place the Team 1 HQ" && !w0.find("plan"), "conquest status");
    auto w4 = parse(raw_plan(R"({"op":"wizard","mode":"Conquest","count":3,"step":4,"at":[30,0,0],
        "flags":[{"name":"CapturePoint_A","at":[0,0,0]},{"name":"CapturePoint_B","at":[0,0,30]}]})"));
    check(w4.find("title")->str == "Place flag C" && w4.find("finishes")->b && w4.find("record_flag")->b, "the last conquest step");
    const auto* s1 = obj_named(*w4.find("plan"), "Sector_1");
    check(s1 && s1->find("links")->find("CapturePoints")->arr.size() == 3
          && s1->find("links")->find("CapturePoints")->arr[2].str == "@CapturePoint_C"
          && std::fabs(s1->find("at")->arr[0].num - 10) < 1e-9 && std::fabs(s1->find("at")->arr[2].num - 10) < 1e-9,
          "the sector wires every flag at their centre");
    auto b6 = parse(raw_plan(R"({"op":"wizard","mode":"Breakthrough","count":2,"step":7,"at":[0,0,0],"flags":[{"name":"S2_ObjectiveA","at":[0,0,0]}]})"));
    check(b6.find("title")->str == "Sector 2: place objective B" && b6.find("reset_flags")->b && b6.find("finishes")->b
          && obj_named(*b6.find("plan"), "Sector_2") && obj_named(*b6.find("plan"), "S2_ObjectiveB")
          && obj_named(*b6.find("plan"), "S2_ObjectiveB")->find("props")->find("ObjId")->num == 1201, "breakthrough sector 2 objective B");
    check(raw_plan(R"({"op":"nope"})").empty(), "an unknown op is refused");

    // ---- scatter
    const std::string base = R"({"shape":0,"center":[0,0,0],"radius":20,"count":30,"seed":4,"unit_width":1})";
    const std::string a = layout(base), b = layout(base);
    auto la = parse(a);
    check(!a.empty() && a == b, "the same seed lays the same pattern");
    const auto& ts = la.find("targets")->arr;
    check(ts.size() == 30, "a roomy circle fits the whole count");
    bool inside = true, grounded = true, spaced = true;
    for (size_t i = 0; i < ts.size(); ++i) {
        const double x = ts[i].find("at")->arr[0].num, z = ts[i].find("at")->arr[2].num;
        inside = inside && x * x + z * z <= 20.0 * 20.0 + 1e-6;
        grounded = grounded && std::fabs(ts[i].find("at")->arr[1].num - 7.0) < 1e-9;
        for (size_t k = 0; k < i; ++k) {
            const double dx = x - ts[k].find("at")->arr[0].num, dz = z - ts[k].find("at")->arr[2].num;
            spaced = spaced && dx * dx + dz * dz >= 0.7 * 0.7 - 1e-9;
        }
    }
    check(inside && grounded && spaced, "inside the circle, on the ground, spaced by the unit");
    check(a != layout(R"({"shape":0,"center":[0,0,0],"radius":20,"count":30,"seed":5,"unit_width":1})"), "a new seed is a new pattern");

    auto only = parse(layout(R"({"shape":1,"center":[0,0,0],"radius":10,"count":40,"seed":1,"terrain_only":true,"pool":3})"));
    bool west = true, pooled = true;
    for (const auto& t : only.find("targets")->arr) {
        west = west && t.find("at")->arr[0].num <= 0.0;
        pooled = pooled && t.find("pool")->num >= 0 && t.find("pool")->num < 3;
    }
    check(west && !only.find("targets")->arr.empty(), "ground only skips what lands off the terrain");
    check(pooled, "each copy draws one of the pool");

    auto flat = parse(layout(R"({"shape":2,"center":[0,3,0],"radius":10,"count":10,"seed":2,"follow_terrain":false})"));
    bool ring = true;
    for (const auto& t : flat.find("targets")->arr) {
        const double x = t.find("at")->arr[0].num, z = t.find("at")->arr[2].num;
        ring = ring && x * x + z * z >= 0.36 * 100 - 1e-6 && std::fabs(t.find("at")->arr[1].num - 3.0) < 1e-9;
    }
    check(ring, "a ring leaves its centre empty; not following terrain keeps the centre height");

    auto drawn = parse(layout(R"({"shape":3,"center":[0,0,0],"radius":10,"count":20,"seed":3,
        "poly":[[0,1,0],[10,1,0],[10,1,10],[0,1,10]]})"));
    bool in_poly = true;
    for (const auto& t : drawn.find("targets")->arr) {
        const double x = t.find("at")->arr[0].num, z = t.find("at")->arr[2].num;
        in_poly = in_poly && x >= 0 && x <= 10 && z >= 0 && z <= 10;
    }
    check(in_poly && !drawn.find("targets")->arr.empty(), "a drawn outline is filled inside");

    auto paint = parse(layout(R"({"shape":4,"count":15,"seed":3,"strokes":[{"radius":4,"stamps":[[0,0,0],[6,0,0]]}]})"));
    check(paint.find("cells") && !paint.find("cells")->arr.empty() && paint.find("cell")->num >= 1.2
          && !paint.find("targets")->arr.empty(), "a painted stroke becomes cells and gets filled");
    auto empty = parse(layout(R"({"shape":4,"count":15,"strokes":[]})"));
    check(empty.find("targets")->arr.empty(), "nothing painted, nothing laid");

    ground_calls = 0;
    parse(layout(base, nullptr));
    check(ground_calls == 0, "no ground callback is fine");

    std::printf("MODE SCATTER %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
