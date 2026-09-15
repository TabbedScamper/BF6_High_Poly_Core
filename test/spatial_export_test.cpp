/* bf6_spatial_export: the rules of the Portal SDK's exporter on a small
 * catalogue, and, given a request and an official .spatial.json, a comparison
 * of the two object by object:
 *
 *   spatial_export_test
 *   spatial_export_test <request.json> <official.spatial.json>
 *   spatial_export_test --files <core.spatial.json> <official.spatial.json>
 *   spatial_export_test --tscn <scene.tscn> <asset_types.json> <official.spatial.json> [core_out.json]
 *
 * --tscn builds the request an editor would send for a Godot scene, so the core
 * can be checked against the SDK's gdconverter run on the same scene.
 */
#include "bf6_core.h"
#include "json.hpp"
#include "utf8_fs.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
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

static int compare(const std::string& request_path, const std::string& official_path, bool is_file = false)
{
    std::string req, off;
    if (!bf6fs::read_all(request_path, req) || !bf6fs::read_all(official_path, off)) { std::printf("cannot read inputs\n"); return 2; }
    Result r;
    if (is_file) { r.text = req; parse(req, r.json); }
    else r = run(req);
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

// ---------------------------------------------------------------- a Godot scene as an editor hands it over
//
// What a host sends the core for a .tscn, so a scene can be checked against the
// SDK's own exporter run on the same file: each node's path, type (the stem of
// its instanced scene or script), world transform, raw field text, links as
// the paths their NodePaths name, a polygon volume's outline in world space,
// and the Static layer's terrain and assets entries.

struct TNode {
    std::string name, parent, key, type;
    std::vector<std::pair<std::string, std::string>> attrs;   // raw text, in scene order
    std::vector<std::string> node_paths;
    double basis[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // right, up, front
    double origin[3] = {0, 0, 0};
    bool world = false;
};

static std::string header_attr(const std::string& h, const std::string& key)
{
    const std::string k = " " + key + "=";
    size_t at = h.find(k);
    if (at == std::string::npos) return std::string();
    at += k.size();
    if (at < h.size() && h[at] == '"') {
        const size_t e = h.find('"', at + 1);
        return h.substr(at + 1, e == std::string::npos ? std::string::npos : e - at - 1);
    }
    size_t depth = 0, e = at;
    for (; e < h.size(); ++e) {
        if (h[e] == '(') ++depth;
        else if (h[e] == ')') { if (depth) --depth; if (!depth) { ++e; break; } }
        else if ((h[e] == ' ' || h[e] == ']') && !depth) break;
    }
    return h.substr(at, e - at);
}

static std::vector<std::string> quoted_list(const std::string& s)
{
    std::vector<std::string> out;
    for (size_t at = s.find('"'); at != std::string::npos; at = s.find('"', at)) {
        const size_t e = s.find('"', at + 1);
        if (e == std::string::npos) break;
        out.push_back(s.substr(at + 1, e - at - 1));
        at = e + 1;
    }
    return out;
}

static std::vector<double> numbers_in(const std::string& s)
{
    std::vector<double> out;
    const size_t open = s.find('(');
    const char* p = s.c_str() + (open == std::string::npos ? 0 : open + 1);
    while (*p) {
        char* end = nullptr;
        const double d = std::strtod(p, &end);
        if (end != p) { out.push_back(d); p = end; }
        else ++p;
    }
    return out;
}

static std::string stem_of(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    std::string s = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = s.find('.');
    return dot == std::string::npos ? s : s.substr(0, dot);
}

static std::string num_text(double v)
{
    char b[40];
    std::snprintf(b, sizeof(b), "%.17g", v);
    return b;
}

// A raw scene value as JSON for a Static entry's extra fields.
static std::string raw_json(const std::string& raw)
{
    if (raw == "true" || raw == "false") return raw;
    char* end = nullptr;
    std::strtod(raw.c_str(), &end);
    if (!raw.empty() && end && *end == 0) return raw;
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') return jq(raw.substr(1, raw.size() - 2));
    return jq(raw);
}

static bool tscn_request(const std::string& scene, const std::string& asset_types, std::string& req, std::string& level)
{
    std::string text;
    if (!bf6fs::read_all(scene, text)) return false;
    std::map<std::string, std::string> ext;   // id -> path
    std::vector<TNode> nodes;
    TNode* cur = nullptr;
    bool in_block = false;
    size_t at = 0;
    while (at < text.size()) {
        size_t e = text.find('\n', at);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(at, e - at);
        at = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (in_block) { if (!line.empty() && line[0] == '}') in_block = false; continue; }
        if (line.empty()) continue;
        if (line[0] == '[') {
            cur = nullptr;
            if (line.rfind("[ext_resource", 0) == 0) ext[header_attr(line, "id")] = header_attr(line, "path");
            else if (line.rfind("[node", 0) == 0) {
                nodes.emplace_back();
                cur = &nodes.back();
                cur->name = header_attr(line, "name");
                cur->parent = header_attr(line, "parent");
                std::string inst = header_attr(line, "instance");
                if (!inst.empty()) { const auto q = quoted_list(inst); if (!q.empty()) cur->type = stem_of(ext[q[0]]); }
                if (cur->type.empty()) cur->type = header_attr(line, "type");
                cur->node_paths = quoted_list(header_attr(line, "node_paths"));
            }
            continue;
        }
        if (!cur) continue;
        const size_t eq = line.find(" = ");
        if (eq == std::string::npos) continue;
        const std::string name = line.substr(0, eq), value = line.substr(eq + 3);
        if (value == "{") { in_block = true; continue; }
        if (name == "script") { const auto q = quoted_list(value); if (!q.empty()) cur->type = stem_of(ext[q[0]]); continue; }
        if (name == "transform") {
            const std::vector<double> t = numbers_in(value);
            if (t.size() == 12) {
                // Transform3D rows: the columns are right, up, front.
                const int col[9] = {0, 3, 6, 1, 4, 7, 2, 5, 8};
                for (int i = 0; i < 9; ++i) cur->basis[i] = t[col[i]];
                for (int i = 0; i < 3; ++i) cur->origin[i] = t[9 + i];
            }
            continue;
        }
        cur->attrs.push_back({name, value});
    }
    if (nodes.empty()) return false;
    level = nodes[0].name;

    std::map<std::string, TNode*> by_key;
    for (TNode& n : nodes) {
        if (n.parent.empty()) { n.key.clear(); continue; }
        n.key = n.parent == "." ? n.name : n.parent + "/" + n.name;
        by_key[n.key] = &n;
    }
    // World transforms, parent first.
    std::function<void(TNode&)> resolve_world = [&](TNode& n) {
        if (n.world) return;
        n.world = true;
        if (n.parent.empty() || n.parent == ".") return;
        auto p = by_key.find(n.parent);
        if (p == by_key.end()) return;
        resolve_world(*p->second);
        const double* P = p->second->basis, *L = n.basis;
        double B[9], o[3];
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                B[c * 3 + r] = P[0 * 3 + r] * L[c * 3 + 0] + P[1 * 3 + r] * L[c * 3 + 1] + P[2 * 3 + r] * L[c * 3 + 2];
        for (int r = 0; r < 3; ++r)
            o[r] = P[0 * 3 + r] * n.origin[0] + P[1 * 3 + r] * n.origin[1] + P[2 * 3 + r] * n.origin[2] + p->second->origin[r];
        std::memcpy(n.basis, B, sizeof(B));
        std::memcpy(n.origin, o, sizeof(o));
    };
    for (TNode& n : nodes) resolve_world(n);

    auto rel_key = [&](const TNode& n, std::string rel) {
        std::string base = n.key;
        while (rel.rfind("..", 0) == 0) {
            rel = rel.size() > 3 ? rel.substr(3) : std::string();
            const auto p = by_key.find(base);
            base = p == by_key.end() || p->second->parent == "." ? std::string() : p->second->parent;
        }
        return base.empty() ? rel : base + "/" + rel;
    };

    req = "{\"level\":" + jq(level) + ",\"asset_types\":" + jq(asset_types) + ",\"scene_values\":true,\"objects\":[";
    bool first = true;
    for (const TNode& n : nodes) {
        if (n.key.empty()) continue;
        const bool under_static = n.key.rfind("Static/", 0) == 0;
        const bool is_static = n.parent == "Static"
            && (n.name.size() > 8 && (n.name.compare(n.name.size() - 8, 8, "_Terrain") == 0 || n.name.compare(n.name.size() - 7, 7, "_Assets") == 0));
        if (n.key == "Static" || (under_static && !is_static)) continue;
        std::string o = "{\"key\":" + jq(n.key) + ",\"name\":" + jq(n.name) + ",\"type\":" + jq(n.type);
        o += ",\"origin\":[" + num_text(n.origin[0]) + "," + num_text(n.origin[1]) + "," + num_text(n.origin[2]) + "],\"basis\":[";
        for (int i = 0; i < 9; ++i) o += (i ? "," : "") + num_text(n.basis[i]);
        o += "]";
        if (is_static) {
            o += ",\"static\":true,\"extra\":{";
            bool f = true;
            for (const auto& kv : n.attrs) { o += (f ? "" : ",") + jq(kv.first) + ":" + raw_json(kv.second); f = false; }
            o += "}}";
        } else {
            std::string props, links;
            for (const auto& kv : n.attrs) {
                std::string v = kv.second;
                if (kv.first == "points" && v.rfind("PackedVector2Array", 0) == 0) {
                    const std::vector<double> xz = numbers_in(v);
                    o += ",\"points\":[";
                    for (size_t i = 0; i + 1 < xz.size(); i += 2) {
                        double w[3];
                        for (int r = 0; r < 3; ++r) w[r] = n.basis[0 * 3 + r] * xz[i] + n.basis[2 * 3 + r] * xz[i + 1] + n.origin[r];
                        o += (i ? ",[" : "[") + num_text(w[0]) + "," + num_text(w[1]) + "," + num_text(w[2]) + "]";
                    }
                    o += "]";
                    continue;
                }
                if (std::find(n.node_paths.begin(), n.node_paths.end(), kv.first) != n.node_paths.end()) {
                    // A list of node paths goes as a list, one as its key.
                    std::string keys;
                    for (size_t p = v.find("NodePath(\""); p != std::string::npos; p = v.find("NodePath(\"", p + 1)) {
                        const size_t s = p + 10, e2 = v.find('"', s);
                        if (e2 == std::string::npos) break;
                        keys += (keys.empty() ? "" : ",") + jq(rel_key(n, v.substr(s, e2 - s)));
                    }
                    const std::string link = !v.empty() && v[0] == '[' ? "[" + keys + "]" : (keys.empty() ? std::string("\"\"") : keys);
                    links += (links.empty() ? "" : ",") + jq(kv.first) + ":" + link;
                    continue;
                }
                props += (props.empty() ? "" : ",") + jq(kv.first) + ":" + jq(v);
            }
            o += ",\"links\":{" + links + "},\"props\":{" + props + "}}";
        }
        req += (first ? "" : ",") + o;
        first = false;
    }
    req += "]}";
    return true;
}

static int compare_tscn(const std::string& scene, const std::string& asset_types, const std::string& official, const std::string& save)
{
    std::string req, level;
    if (!tscn_request(scene, asset_types, req, level)) { std::printf("cannot read scene %s\n", scene.c_str()); return 2; }
    const std::string req_path = bf6fs::join(bf6fs::env("TEMP"), "bf6_spatial_tscn_request.json");
    bf6fs::write_all(req_path, req);
    if (!save.empty()) {
        Result r = run(req);
        bf6fs::write_all(save, r.text);
    }
    std::printf("scene %s (level %s)\n", scene.c_str(), level.c_str());
    return compare(req_path, official);
}

// ---------------------------------------------------------------- rules

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 4 && std::string(argv[1]) == "--files") return compare(argv[2], argv[3], true);
    if ((argc == 5 || argc == 6) && std::string(argv[1]) == "--tscn") return compare_tscn(argv[2], argv[3], argv[4], argc == 6 ? argv[5] : "");
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
        {"type":"CombatArea","constants":[{"name":"category","value":"spatial"}],"properties":[{"name":"CombatVolume","type":"PolygonVolume"}]},
        {"type":"HQ_Linked","constants":[{"name":"category","value":"spatial"}],"properties":[
            {"name":"ObjId","type":"int","default":-1},
            {"name":"HQProtectionAreaVolume","type":"PolygonVolume","default":""},
            {"name":"HQProtectionArea","type":"OBBVolume","default":""},
            {"name":"InfantrySpawns","type":"Array[SpawnPoint]","default":""},
            {"name":"VehicleSpawners","type":"Array[VehicleSpawner]","default":""}],"levelRestrictions":[]},
        {"type":"VehicleSpawner","constants":[{"name":"category","value":"spatial"}],"properties":[{"name":"ObjId","type":"int","default":-1}]},
        {"type":"Sector","constants":[{"name":"category","value":"spatial"}],"properties":[
            {"name":"SectorEnabled","type":"bool","default":true},{"name":"ObjId","type":"int","default":-1},
            {"name":"HQs","type":"Array[HQ_Linked]","default":""}],"levelRestrictions":[]},
        {"type":"BlockingSphere","constants":[{"name":"category","value":"spatial"}],"properties":[
            {"name":"ObjId","type":"int","default":-1},{"name":"Radius","type":"float","default":150},
            {"name":"BlockSpotting","type":"bool","default":true},{"name":"RemoveLaserPainting","type":"bool"},
            {"name":"IsCloudBlockingVolume","type":"bool","default":true}],"levelRestrictions":[]},
        {"type":"FX_Cloud_Cluster","constants":[{"name":"category","value":"spatial"}],"properties":[{"name":"ObjId","type":"int","default":-1}],"levelRestrictions":[]}
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
    check(entry(j, "Portal_Dynamic", "Dup") != nullptr && entry(j, "Portal_Dynamic", "TEAM_1_HQ") == hq, "ids are paths; authored ids are dropped");
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
    Result printed = run(head + R"("objects":[{"key":"S","type":"SpawnPoint","origin":[0.101055979728699,285.191009521484,0.5]}]})");
    check(printed.text.find("\"x\": 0.10105598,") != std::string::npos && printed.text.find("\"y\": 285.191,") != std::string::npos
          && printed.text.find("\"z\": 0.5") != std::string::npos, "a float printed to 15 digits comes back to its own short text");

    Result m = run(head + "\"pretty\":false,\"short_ids\":true," + objects);
    const Value* mhq = entry(m.json, "Portal_Dynamic", "a");
    check(mhq && mhq->find("name")->str == "a" && mhq->find("HQArea")->str == "b" && mhq->find("InfantrySpawns")->arr[0].str == "c"
          && m.text.find('\n') == std::string::npos && m.text.size() < r.text.size() / 2
          && m.report.find("short_ids") && m.report.find("short_ids")->find("TEAM_1_HQ") && m.report.find("short_ids")->find("TEAM_1_HQ")->str == "a",
          "minified: no whitespace, short names and ids, links renamed with them, the map reported");

    Result big = run(head + R"("objects":[{"key":"CA","type":"CombatArea","props":{"CombatVolume":"V"}},
        {"key":"V","type":"PolygonVolume","points":[[0,0,0],[5000,0,0],[5000,0,5000],[0,0,5000]]},
        {"key":"Tiny","type":"PolygonVolume","points":[[0,0,0],[1,0,0]]}]})");
    const Value* errs = big.report.find("errors");
    check(errs && errs->arr.size() == 2 && !entry(big.json, "Portal_Dynamic", "Tiny"), "a too-large combat area and a two-point volume are errors");

    // SDK 1.4.3.0 shapes: HQ protection volumes and vehicle spawner lists, a
    // Sector's HQs, BlockingSphere's parameters, and the exporter's handling of
    // links that end up empty and of fields the catalogue does not list.
    const std::string linked_objects = R"J("objects":[
        {"key":"HQ","type":"HQ_Linked","props":{"VehicleSpawners":"Heli,HQ/Spawn,Jet","HQProtectionArea":"HQ/Protect",
         "HQProtectionAreaVolume":"HQ/PV","InfantrySpawns":"Heli","ObjId":"1"}},
        {"key":"HQ/Protect","type":"OBBVolume","origin":[1,2,3],"size":"30,10,40"},
        {"key":"HQ/PV","type":"PolygonVolume","points":[[0,0,0],[4,0,0],[4,0,4]],"props":{"height":"12.5","color":"Color(0, 0.6, 0.7, 0.42)"}},
        {"key":"HQ/Spawn","type":"SpawnPoint"},
        {"key":"Heli","type":"VehicleSpawner"},
        {"key":"Jet","type":"VehicleSpawner"},
        {"key":"Sector","type":"Sector","links":{"HQs":["HQ","Heli"]},"props":{"SectorEnabled":"true"}},
        {"key":"Sphere","type":"BlockingSphere","props":{"ObjId":"40","Radius":"150","BlockSpotting":"false","RemoveLaserPainting":"true",
         "IsCloudBlockingVolume":"true","visible":"false","metadata/_edit_lock_":"true","tags":"[\"a\", 2, NodePath(\"X\")]"}},
        {"key":"Cloud","type":"FX_Cloud_Cluster","links":{"Target":"NoSuch"},"props":{"ObjId":"3"}}]})J";
    Result nl = run(head + linked_objects);
    if (bf6fs::env("BF6_SPATIAL_DEBUG") != "") std::printf("%s\n%s\n", nl.text.c_str(), show(nl.report).c_str());
    const Value* lhq = entry(nl.json, "Portal_Dynamic", "HQ");
    check(lhq && lhq->find("HQProtectionArea") && lhq->find("HQProtectionArea")->str == "HQ/Protect"
          && lhq->find("HQProtectionAreaVolume") && lhq->find("HQProtectionAreaVolume")->str == "HQ/PV",
          "an HQ links its protection OBB and polygon volumes");
    const Value* vsp = lhq ? lhq->find("VehicleSpawners") : nullptr;
    check(vsp && vsp->arr.size() == 2 && vsp->arr[0].str == "Heli" && vsp->arr[1].str == "Jet", "an HQ's VehicleSpawners keep only vehicle spawners");
    const Value* inf = lhq ? lhq->find("InfantrySpawns") : nullptr, *hql = lhq ? lhq->find("linked") : nullptr;
    check(inf && inf->is_arr() && inf->arr.empty() && hql && hql->arr.size() == 4 && hql->arr[0].str == "VehicleSpawners" && hql->arr[3].str == "InfantrySpawns",
          "a link list whose every entry dropped is still written, empty, and linked in the object's field order");
    const Value* prot = entry(nl.json, "Portal_Dynamic", "HQ/Protect"), *pv = entry(nl.json, "Portal_Dynamic", "HQ/PV");
    check(prot && prot->find("size") && prot->find("size")->arr[2].num == 40 && prot->find("position") && pv && !pv->find("position")
          && pv->find("height")->num == 12.5 && pv->find("color") && pv->find("color")->arr.size() == 4 && std::fabs(pv->find("color")->arr[1].num - 0.6) < 1e-9,
          "the protection volumes keep their shapes; a Color field is its numbers");
    const Value* sec = entry(nl.json, "Portal_Dynamic", "Sector");
    check(sec && sec->find("HQs") && sec->find("HQs")->arr.size() == 1 && sec->find("HQs")->arr[0].str == "HQ" && !sec->find("SectorEnabled")
          && sec->find("ObjId") && sec->find("ObjId")->num == 0 && sec->find("linked")->arr.size() == 1,
          "a Sector's HQs keep only HQs; the ObjId shim still applies");
    const Value* sph = entry(nl.json, "Portal_Dynamic", "Sphere");
    check(sph && !sph->find("Radius") && sph->find("BlockSpotting") && !sph->find("BlockSpotting")->b && sph->find("RemoveLaserPainting")
          && sph->find("RemoveLaserPainting")->b && !sph->find("IsCloudBlockingVolume") && sph->find("ObjId")->num == 40,
          "BlockingSphere parameters: defaults stay out, a bool with no default starts false");
    const Value* tags = sph ? sph->find("tags") : nullptr;
    check(sph && sph->find("visible") && sph->find("visible")->type == Value::Bool && !sph->find("visible")->b
          && sph->find("metadata/_edit_lock_") && sph->find("metadata/_edit_lock_")->b
          && tags && tags->arr.size() == 3 && tags->arr[0].str == "a" && tags->arr[1].num == 2 && tags->arr[2].str == "X",
          "fields the catalogue does not list are written from their scene text");
    const Value* cloud = entry(nl.json, "Portal_Dynamic", "Cloud");
    check(cloud && cloud->find("Target") && cloud->find("Target")->type == Value::Null && cloud->find("linked") && cloud->find("linked")->arr[0].str == "Target",
          "a single link that names nothing is null and still linked");
    Result sv = run(head + "\"scene_values\":true," + linked_objects);
    const Value* ssph = entry(sv.json, "Portal_Dynamic", "Sphere"), *ssec = entry(sv.json, "Portal_Dynamic", "Sector");
    check(ssph && ssph->find("Radius") && ssph->find("Radius")->num == 150 && ssph->find("IsCloudBlockingVolume") && ssph->find("IsCloudBlockingVolume")->b
          && ssec && ssec->find("SectorEnabled"), "scene values: a value a scene stores at its default is still written");

    bf6fs::remove_tree(base);
    std::printf("SPATIAL EXPORT %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
