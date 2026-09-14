/* The map checks both SDK editors run from VALIDATE: every offline rule, with
 * one wording, one severity and one order, so a map reads the same in either
 * editor. The engines describe their scene; nothing here knows about actors or
 * nodes. See bf6_map_validate in bf6_core.h. */
#include "bf6_core.h"
#include "json.hpp"

#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using bf6json::Value;

struct Item {
    int severity;          // 0 problem, 1 warning, 2 advice
    std::string id;        // the engine's handle, echoed; empty = the map
    std::string message;
    std::string fix;       // "winding" or empty
};

struct Link {
    std::vector<std::string> ids;   // resolved handles
    bool legacy = false;            // a reference the engine could not name
};

struct Object {
    const Value* v = nullptr;
    std::string id, name, type, catalogue_type;
};

std::string fmt(const char* f, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(b, sizeof(b), f, ap);
    va_end(ap);
    return b;
}

void json_str(std::string& out, const std::string& s)
{
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
            else out.push_back((char)c);
        }
    }
    out.push_back('"');
}

double num(const Value* v, double dflt)
{
    return v && v->type == Value::Num ? v->num : dflt;
}

bool flag(const Value* v)
{
    return v && ((v->type == Value::Bool && v->b) || (v->type == Value::Num && v->num != 0.0));
}

std::string text(const Value* v)
{
    static const std::string empty;
    return v ? v->as_str(empty) : empty;
}

std::set<std::string> str_set(const Value* v)
{
    std::set<std::string> out;
    if (v && v->is_arr())
        for (const Value& e : v->arr) if (e.is_str()) out.insert(e.str);
    return out;
}

// Signed-area terms over a loop of [x, z] game metres: s is the winding sum
// sum (x2 - x1)(z2 + z1), positive clockwise in Godot's XZ; area2 twice the
// shoelace area.
bool loop_of(const Object& o, std::vector<std::pair<double, double>>& pts)
{
    pts.clear();
    const Value* l = o.v->find("loop");
    if (!l || !l->is_arr()) return false;
    for (const Value& p : l->arr)
        if (p.is_arr() && p.arr.size() >= 2)
            pts.emplace_back(num(&p.arr[0], 0.0), num(&p.arr[1], 0.0));
    return pts.size() >= 3;
}

} // namespace

extern "C" int64_t bf6_map_validate(const char* scene_json, size_t len, uint8_t** out)
{
    if (!out) return -1;
    *out = nullptr;
    if (!scene_json) return -1;
    Value root;
    std::string err;
    bf6json::Parser parser(scene_json, len ? len : std::strlen(scene_json));
    if (!parser.parse(root, err) || !root.is_obj()) return -1;

    std::vector<Item> items;
    auto add = [&items](int sev, const std::string& id, const std::string& msg, const char* fix = "") {
        items.push_back({sev, id, msg, fix});
    };

    std::vector<Object> objects;
    std::map<std::string, const Object*> by_name;
    if (const Value* arr = root.find("objects"); arr && arr->is_arr()) {
        objects.reserve(arr->arr.size());
        for (const Value& v : arr->arr) {
            if (!v.is_obj()) continue;
            Object o;
            o.v = &v;
            o.id = text(v.find("id"));
            o.name = text(v.find("name"));
            o.type = text(v.find("type"));
            o.catalogue_type = text(v.find("catalogue_type"));
            if (o.catalogue_type.empty()) o.catalogue_type = o.type;
            objects.push_back(std::move(o));
        }
    }
    for (const Object& o : objects)
        if (!o.name.empty()) by_name[o.name] = &o;

    // A link property: an array of link names, or the string "legacy" for a
    // reference the engine holds but cannot name (skipped quietly).
    auto link = [&by_name](const Object& o, const char* prop) {
        Link l;
        const Value* props = o.v->find("props");
        const Value* p = props ? props->find(prop) : nullptr;
        if (!p) return l;
        if (p->is_str()) {
            if (p->str == "legacy") { l.legacy = true; return l; }
            size_t at = 0;
            while (at <= p->str.size()) {
                size_t comma = p->str.find(',', at);
                if (comma == std::string::npos) comma = p->str.size();
                std::string part = p->str.substr(at, comma - at);
                const size_t a = part.find_first_not_of(" \t"), b = part.find_last_not_of(" \t");
                if (a != std::string::npos) {
                    auto it = by_name.find(part.substr(a, b - a + 1));
                    if (it != by_name.end()) l.ids.push_back(it->second->id);
                }
                at = comma + 1;
            }
            return l;
        }
        if (p->is_arr())
            for (const Value& e : p->arr)
                if (e.is_str()) {
                    auto it = by_name.find(e.str);
                    if (it != by_name.end()) l.ids.push_back(it->second->id);
                }
        return l;
    };
    auto find_id = [&objects](const std::string& id) -> const Object* {
        for (const Object& o : objects) if (o.id == id) return &o;
        return nullptr;
    };
    auto height = [](const Object& o) { return num(o.v->find("height"), 0.0); };

    // ---- will the game read this, and is it listed for this map? ----
    // Keyed on type alone. Objects move folder and drop off map lists between
    // SDK releases and usually still load, so a known type this level does not
    // list is a warning; only a type the catalogue has never heard of is broken,
    // and then only for objects placed from the library.
    {
        const std::set<std::string> level_types = str_set(root.find("level_types"));
        const std::set<std::string> all_types = str_set(root.find("all_types"));
        const std::string level = text(root.find("level"));
        if (!all_types.empty())
            for (const Object& o : objects) {
                const std::string& ct = o.catalogue_type;
                if (ct.empty()) continue;
                if (!all_types.count(ct)) {
                    if (flag(o.v->find("from_library")))
                        add(0, o.id, fmt("'%s' is not in this SDK's catalogue at all - the game has nothing to load for it. It may have been renamed or removed in an SDK update.", ct.c_str()));
                } else if (!level_types.empty() && !level_types.count(ct)) {
                    add(1, o.id, fmt("'%s' is not listed for %s in this SDK release. It usually still loads - objects move between folders and map lists between releases - but check it in game before shipping.", ct.c_str(), level.c_str()));
                }
            }
    }

    std::vector<std::pair<double, double>> pts;
    for (const Object& o : objects) {
        const std::string& ty = o.type;

        // gdconverter's REQUIRED_PROPS: a RingOfFire with either shape unset is
        // refused at conversion.
        if (ty == "RingOfFire") {
            // Set at all, resolvable or not: the converter only asks for a value.
            auto set = [&o](const char* prop) {
                const Value* props = o.v->find("props");
                const Value* p = props ? props->find(prop) : nullptr;
                if (!p) return false;
                if (p->is_str()) return p->str.find_first_not_of(" \t") != std::string::npos;
                return p->is_arr() && !p->arr.empty();
            };
            const Link hard = {{}, set("HardRestrictOBB")};
            const Link shape = {{}, set("RestrictShapeData")};
            if (hard.ids.empty() && !hard.legacy)
                add(0, o.id, "Ring of fire has no HardRestrictOBB volume - the SDK refuses to convert the map without one.");
            if (shape.ids.empty() && !shape.legacy)
                add(0, o.id, "Ring of fire has no RestrictShapeData volume - the SDK refuses to convert the map without one.");
        }

        if (ty == "CombatArea") {
            const Link t = link(o, "CombatVolume");
            // Advice: CombatArea.gd exports CombatVolume as optional. Shipped maps
            // link one 91 times out of 95.
            if (t.ids.empty() && !t.legacy)
                add(2, o.id, "Combat area has no combat volume linked. Most maps link one; check this is deliberate.");
            for (const std::string& vid : t.ids) {
                const Object* vol = find_id(vid);
                if (!vol || !loop_of(*vol, pts)) continue;
                double s = 0.0, area2 = 0.0;
                for (size_t i = 0; i < pts.size(); ++i) {
                    const auto& p1 = pts[i];
                    const auto& p2 = pts[(i + 1) % pts.size()];
                    s += (p2.first - p1.first) * (p2.second + p1.second);
                    area2 += p1.first * p2.second - p2.first * p1.second;
                }
                // The SDK's hard limit: gdconverter refuses a combat volume over
                // 16,640,000 square metres.
                const double area = std::fabs(area2) * 0.5;
                if (area > 16640000.0)
                    add(0, vid, fmt("Combat volume covers %.0f km2 - over the SDK's %.1f km2 limit, and the converter rejects the map.", area / 1e6, 16.64));
                // Clockwise in Godot's XZ; counter-clockwise turns the zone inside out.
                if (s <= 0.0)
                    add(1, vid, "Combat volume looks counter-clockwise - in game that makes everything OUTSIDE it playable. Fix reverses the point order.", "winding");
            }
        } else if (ty == "HQ_PlayerSpawner" || ty == "PlayerSpawner") {
            const Link t = link(o, ty == "HQ_PlayerSpawner" ? "InfantrySpawns" : "SpawnPoints");
            if (t.ids.empty() && !t.legacy)
                add(0, o.id, "Spawner has no spawn points linked - players get 'deployment unavailable'.");
            else if (!t.ids.empty() && t.ids.size() < 4)
                add(2, o.id, fmt("Only %d spawn point%s linked - 4 or more, spread out, avoids the deploy-availability bug.", (int)t.ids.size(), t.ids.size() == 1 ? "" : "s"));
            if (ty == "HQ_PlayerSpawner") {
                const Link hq = link(o, "HQArea");
                if (hq.ids.empty() && !hq.legacy)
                    add(1, o.id, "HQ has no HQArea volume - each HQ needs its own protected area.");
            }
        } else if (ty == "CapturePoint") {
            const Link t = link(o, "CaptureArea");
            if (t.ids.empty() && !t.legacy)
                add(0, o.id, "Capture point has no capture area - the flag can never be taken.");
            for (const std::string& vid : t.ids) {
                // Height 0 is infinite (Season 4); only a tiny nonzero height is suspect.
                const Object* vol = find_id(vid);
                const double h = vol ? height(*vol) : 0.0;
                if (h > 0.01 && h < 0.5)
                    add(2, vid, "Capture area height is nearly zero - players will barely register. Set a real height, or 0 for infinite (Season 4).");
            }
            for (const char* team : {"InfantrySpawnPoints_Team1", "InfantrySpawnPoints_Team2"}) {
                const Link sp = link(o, team);
                if (!sp.ids.empty() && sp.ids.size() < 4)
                    add(2, o.id, fmt("%s has only %d spawn point%s - 4 or more, spread out, is the safe pattern.", team, (int)sp.ids.size(), sp.ids.size() == 1 ? "" : "s"));
            }
        } else if (ty == "AreaTrigger") {
            const Link t = link(o, "Area");
            for (const std::string& vid : t.ids) {
                const Object* vol = find_id(vid);
                const double h = vol ? height(*vol) : 0.0;
                if (h > 0.01 && h < 0.5)
                    add(2, vid, "Area trigger height is nearly zero - it will barely fire. Set a real height, or 0 for infinite (Season 4).");
            }
        } else if (ty == "Sector") {
            const Link cp = link(o, "CapturePoints");
            const Link mc = link(o, "MCOMs");
            if (cp.ids.empty() && mc.ids.empty() && !cp.legacy && !mc.legacy)
                add(1, o.id, "Sector owns no capture points or MCOMs - without a sector, every flag shows as 'A'.");
        }

        // Non-uniform scale desyncs collision from the visual mesh.
        if (flag(o.v->find("from_library"))) {
            const Value* sc = o.v->find("scale");
            if (sc && sc->is_arr() && sc->arr.size() >= 3) {
                const double x = num(&sc->arr[0], 1.0), y = num(&sc->arr[1], 1.0), z = num(&sc->arr[2], 1.0);
                const double mx = std::max({x, y, z}), mn = std::min({x, y, z});
                if (mn > 0.0 && mx / mn > 1.01)
                    add(1, o.id, fmt("Non-uniform scale (%.2f, %.2f, %.2f). The shipped maps almost never do this (2 objects in 380) and collision can disagree with the visual - check it in game.", x, y, z));
            }
        }
    }

    // Duplicate ObjIds break scripts silently. The shipped maps reuse an id
    // across types (a DeployCam 1 beside an HQ 1), so that is advice; the same
    // id twice within one type is the real problem.
    {
        std::map<int, std::vector<const Object*>> by_id;
        for (const Object& o : objects) {
            const Value* id = o.v->find("obj_id");
            if (id && id->type == Value::Num && id->num >= 0.0) by_id[(int)id->num].push_back(&o);
        }
        for (const auto& kv : by_id) {
            if (kv.second.size() < 2) continue;
            std::set<std::string> types;
            for (const Object* o : kv.second) types.insert(o->type);
            const bool same_type = types.size() < kv.second.size();
            add(same_type ? 0 : 2, kv.second[0]->id, same_type
                ? fmt("ObjId %d is used by %d objects of the same type - scripts can't tell them apart. Fix it in OBJECT IDS.", kv.first, (int)kv.second.size())
                : fmt("ObjId %d is shared by %d objects of different types. The base maps do this too, but unique ids everywhere are safer for scripts.", kv.first, (int)kv.second.size()));
        }
    }

    // Upload size: the site rejects a per-map file over the limit outright.
    {
        const double bytes = num(root.find("upload_bytes"), 0.0), limit = num(root.find("upload_limit"), 0.0);
        if (bytes > 0.0 && limit > 0.0) {
            if (bytes > limit)
                add(0, "", fmt("This map exports to %lld KB minified - over the %lld KB per-map upload limit, the Portal site rejects it. Remove detail or split the map.", (long long)bytes / 1024, (long long)limit / 1024));
            else if (bytes > limit * 0.9)
                add(1, "", fmt("This map is %lld KB minified, close to the %lld KB per-map limit. Not much room left.", (long long)bytes / 1024, (long long)limit / 1024));
        }
    }

    // Problems first, then warnings, then advice.
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.severity < b.severity; });

    std::string j = "{\"items\":[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) j += ',';
        j += fmt("{\"severity\":%d,\"id\":", items[i].severity);
        json_str(j, items[i].id);
        j += ",\"message\":";
        json_str(j, items[i].message);
        j += ",\"fix\":";
        json_str(j, items[i].fix);
        j += '}';
    }
    j += "]}";
    *out = (uint8_t*)std::malloc(j.size() + 1);
    if (!*out) return -1;
    std::memcpy(*out, j.data(), j.size());
    (*out)[j.size()] = 0;
    return (int64_t)j.size();
}
