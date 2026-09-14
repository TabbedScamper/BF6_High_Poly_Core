/* bf6_budget and bf6_block_*: the meter sums costs against the level's cap and
 * shows the upload share; a block saves anchored with member links by index,
 * loads where it is dropped, lists across folders, reads the Unreal tool's
 * first format upgraded, and deletes. */
#include "bf6_core.h"
#include "json.hpp"
#include "utf8_fs.h"

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

static std::string jq(const std::string& s)
{
    std::string o = "\"";
    for (char c : s) { if (c == '\\' || c == '"') o.push_back('\\'); o.push_back(c); }
    return o + "\"";
}

static bf6json::Value call(int64_t (*fn)(const char*, size_t, uint8_t**), const std::string& req)
{
    uint8_t* out = nullptr;
    const int64_t n = fn(req.c_str(), req.size(), &out);
    bf6json::Value v;
    if (n > 0 && out) {
        std::string err;
        bf6json::Parser p((const char*)out, (size_t)n);
        p.parse(v, err);
    }
    bf6_blob_free(out);
    return v;
}

static bool close_to(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string base = bf6fs::join(bf6fs::env("TEMP"), "bf6_budget_blocks_test");
    bf6fs::remove_tree(base);
    bf6fs::make_dirs(base);

    // ---- budget
    const std::string types = bf6fs::join(base, "asset_types.json");
    const std::string info = bf6fs::join(base, "level_info.json");
    bf6fs::write_all(types, R"({"AssetTypes":[
        {"type":"Crate","constants":[{"name":"physicsCost","type":"int","value":8}]},
        {"type":"Tree","constants":[{"name":"mesh","value":"Tree"},{"name":"physicsCost","type":"int","value":2}]},
        {"type":"SpawnPoint","constants":[]}]})");
    bf6fs::write_all(info, R"({"MP_Test":{"budget":{"physicsCostMax":100}},"MP_Open":{}})");
    const std::string paths = ",\"asset_types\":" + jq(types) + ",\"level_info\":" + jq(info);
    auto b = call(bf6_budget, "{\"level\":\"MP_Test\"" + paths + ",\"counts\":{\"Crate\":5,\"Tree\":10,\"SpawnPoint\":4,\"Unknown\":1}}");
    check(b.find("cost")->num == 60 && b.find("objects")->num == 20 && b.find("max")->num == 100, "costs sum by type against the level's cap");
    check(b.find("text")->str == "physics 60 / 100   (60%)   20 obj", "the text reads as the Unreal tool's");
    check(close_to(b.find("frac")->num, 0.6) && !b.find("over")->b, "the fraction and not over");
    auto over = call(bf6_budget, "{\"level\":\"MP_Test\"" + paths + ",\"counts\":{\"Crate\":15}}");
    const auto& col = over.find("color")->arr;
    check(over.find("over")->b && close_to(over.find("frac")->num, 1.0) && close_to(col[0].num, 0xFB / 255.0, 1e-3) && close_to(col[1].num, 0x69 / 255.0, 1e-3), "over the cap is full and the site's red");
    auto open = call(bf6_budget, "{\"level\":\"MP_Open\"" + paths + ",\"counts\":{\"Crate\":1234},\"upload_bytes\":1572864,\"upload_raw_bytes\":3000000}");
    check(open.find("max")->num == -1 && std::string(open.find("text")->str).find("physics 9,872 (no cap)   1234 obj     upload 1536 KB min / 2929 KB raw   of 3072 KB per map") == 0,
          "no cap, thousands grouped, the upload share");
    check(close_to(open.find("size_frac")->num, 0.5) && close_to(open.find("frac")->num, 0.5), "the upload fraction drives the bar when there is no cap");

    // ---- blocks
    const std::string lib = bf6fs::join(base, "library");
    const std::string legacy = bf6fs::join(base, "legacy");
    std::string save = "{\"dir\":" + jq(lib) + ",\"name\":\"Guard Post!\",\"level\":\"MP_Test\",\"objects\":["
        "{\"type\":\"HQ_PlayerSpawner\",\"name\":\"HQ\",\"origin\":[10,2,20],\"basis\":[1,0,0,0,1,0,0,0,1],\"props\":{\"ObjId\":301,\"Team\":\"Team1\"},\"links\":{\"InfantrySpawns\":[\"S1\",\"Elsewhere\"]}},"
        "{\"type\":\"SpawnPoint\",\"name\":\"S1\",\"origin\":[14,1,20],\"basis\":[0,0,-1,0,1,0,1,0,0]}]}";
    auto saved = call(bf6_block_save, save);
    check(saved.find("name") && saved.find("name")->str == "Guard Post_" && saved.find("count")->num == 2, "a block saves under a safe name");
    check(saved.find("anchor") && close_to(saved.find("anchor")->arr[0].num, 12) && close_to(saved.find("anchor")->arr[1].num, 1) && close_to(saved.find("anchor")->arr[2].num, 20),
          "the anchor is the centroid at the lowest point");
    std::string file_text;
    bf6fs::read_all(bf6fs::join(lib, "Guard Post_.json"), file_text);
    check(file_text.find("\"@1\"") != std::string::npos && file_text.find("\"Elsewhere\"") != std::string::npos && file_text.find("bf6-block/2") != std::string::npos,
          "member links are stored by index, outside ones by name");

    auto loaded = call(bf6_block_load, "{\"dirs\":[" + jq(lib) + "],\"name\":\"Guard Post_\",\"at\":[100,50,-30]}");
    const auto& objs = loaded.find("objects")->arr;
    check(objs.size() == 2 && close_to(objs[0].find("origin")->arr[0].num, 98) && close_to(objs[0].find("origin")->arr[1].num, 51) && close_to(objs[1].find("origin")->arr[0].num, 102),
          "a block loads where it is dropped");
    check(objs.size() == 2 && objs[1].find("basis")->arr[2].num == -1 && objs[0].find("props")->find("ObjId")->num == 301
          && objs[0].find("props")->find("Team")->str == "Team1" && objs[0].find("links")->find("InfantrySpawns")->arr[0].str == "@1",
          "basis, typed props and links come back");

    // The Unreal tool's first format: centimetres, a rotator, props as tags.
    bf6fs::make_dirs(legacy);
    bf6fs::write_all(bf6fs::join(legacy, "Old Bunker.json"), R"({"name":"Old Bunker","level":"MP_Test","objects":[
        {"type":"CapturePoint","mesh":"","pos":[100,200,300],"rot":[0,90,0],"scale":[1,1,2],"props":["ObjId=200","CaptureArea=@1"]},
        {"type":"PolygonVolume","mesh":"","pos":[0,0,0],"rot":[0,0,0],"scale":[1,1,1],"props":[]}]})");
    auto listed = call(bf6_block_list, "{\"dirs\":[" + jq(lib) + "," + jq(legacy) + "]}");
    check(listed.find("blocks")->arr.size() == 2 && listed.find("blocks")->arr[1].find("format")->str == "bf6-block/1", "the list spans the library and an older folder");
    auto old = call(bf6_block_load, "{\"dirs\":[" + jq(lib) + "," + jq(legacy) + "],\"name\":\"Old Bunker\",\"at\":[0,0,0]}");
    const auto& o0 = old.find("objects")->arr[0];
    const auto& ob = o0.find("basis")->arr;
    // yaw 90: Unreal X axis points along Unreal +Y, which is game +Z; Z axis
    // (scaled 2) is game +Y; Unreal Y axis points along -X, game -X.
    check(close_to(o0.find("origin")->arr[0].num, 1) && close_to(o0.find("origin")->arr[1].num, 3) && close_to(o0.find("origin")->arr[2].num, 2)
          && close_to(ob[0].num, 0) && close_to(ob[2].num, 1) && close_to(ob[4].num, 2) && close_to(ob[6].num, -1),
          "an old block's centimetres and rotator upgrade to game metres and a basis");
    check(o0.find("props")->find("ObjId")->str == "200" && o0.find("links")->find("CaptureArea")->arr[0].str == "@1", "old prop tags upgrade to props and links");

    auto del = call(bf6_block_delete, "{\"dirs\":[" + jq(lib) + "],\"name\":\"Guard Post_\"}");
    auto after = call(bf6_block_list, "{\"dirs\":[" + jq(lib) + "]}");
    check(del.find("deleted")->b && after.find("blocks")->arr.empty(), "a block deletes");
    auto nameless = call(bf6_block_save, "{\"dir\":" + jq(lib) + ",\"name\":\"  \",\"objects\":[{\"type\":\"Crate\"}]}");
    check(nameless.find("error") != nullptr, "a nameless block is refused");

    uint8_t* out = nullptr;
    bf6_block_library(&out);
    check(out && std::strstr((const char*)out, "blocks") != nullptr, "the shared library has a folder");
    bf6_blob_free(out);

    bf6fs::remove_tree(base);
    std::printf("BUDGET BLOCKS %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
