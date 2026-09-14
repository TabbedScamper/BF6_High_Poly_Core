/* MODE SETUP and the finished pieces it builds, for every engine: which objects
 * a Conquest or Breakthrough step or a one-click bundle makes, where they stand,
 * what they are called, their ObjIds and how they link. The engines only spawn
 * what the plan says, so a flag placed in either editor comes out the same.
 * See bf6_mode_plan in bf6_core.h. */
#include "bf6_core.h"
#include "json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using bf6json::Value;

constexpr double PI = 3.14159265358979323846;

std::string fmt(const char* f, ...)
{
    char b[2048];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(b, sizeof(b), f, ap);
    va_end(ap);
    return b;
}

void jstr(std::string& out, const std::string& s)
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

std::string num(double v)
{
    if (std::fabs(v) < 1e-9) v = 0.0;
    char b[40];
    std::snprintf(b, sizeof(b), "%.6g", v);
    return b;
}

double getn(const Value* v, double d) { return v && v->type == Value::Num ? v->num : d; }

struct Obj {
    std::string key, type, name, parent;
    double at[3] = {0, 0, 0};
    double facing[2] = {1, 0};
    std::vector<std::pair<std::string, double>> props;
    std::vector<std::pair<std::string, std::vector<std::string>>> links;   // "@name" = an existing object
    bool volume = false;
    double half = 0, height = 0;
};

struct Plan {
    std::vector<Obj> objs;
    std::string root;
};

std::string key_of(const std::string& name) { return name; }

void write_plan(std::string& j, const Plan& p)
{
    j += "\"root\":"; jstr(j, p.root);
    j += ",\"objects\":[";
    for (size_t i = 0; i < p.objs.size(); ++i) {
        const Obj& o = p.objs[i];
        if (i) j += ',';
        j += "{\"key\":"; jstr(j, o.key);
        j += ",\"type\":"; jstr(j, o.type);
        j += ",\"name\":"; jstr(j, o.name);
        j += ",\"parent\":"; jstr(j, o.parent);
        j += ",\"at\":[" + num(o.at[0]) + "," + num(o.at[1]) + "," + num(o.at[2]) + "]";
        j += ",\"facing\":[" + num(o.facing[0]) + "," + num(o.facing[1]) + "]";
        j += ",\"props\":{";
        for (size_t k = 0; k < o.props.size(); ++k) {
            if (k) j += ',';
            jstr(j, o.props[k].first);
            j += ":" + num(o.props[k].second);
        }
        j += "},\"links\":{";
        for (size_t k = 0; k < o.links.size(); ++k) {
            if (k) j += ',';
            jstr(j, o.links[k].first);
            j += ":[";
            for (size_t m = 0; m < o.links[k].second.size(); ++m) {
                if (m) j += ',';
                jstr(j, o.links[k].second[m]);
            }
            j += "]";
        }
        j += "}";
        if (o.volume) {
            // Clockwise in the game's ground plane (x, z), so containment works.
            const double h = o.half;
            j += ",\"volume\":{\"height\":" + num(o.height) + ",\"points\":[[" + num(-h) + "," + num(-h) + "],["
               + num(-h) + "," + num(h) + "],[" + num(h) + "," + num(h) + "],[" + num(h) + "," + num(-h) + "]]}";
        }
        j += '}';
    }
    j += ']';
}

Obj spawn(const std::string& type, const double at[3], double facing_rad, const std::string& name, const std::string& parent = "")
{
    Obj o;
    o.key = key_of(name);
    o.type = type;
    o.name = name;
    o.parent = parent;
    std::memcpy(o.at, at, sizeof(o.at));
    o.facing[0] = std::cos(facing_rad);
    o.facing[1] = std::sin(facing_rad);
    return o;
}

Obj square_volume(const double at[3], double half_m, double height_m, const std::string& name, const std::string& parent)
{
    Obj o = spawn("PolygonVolume", at, 0.0, name, parent);
    o.volume = true;
    o.half = half_m;
    o.height = height_m;
    return o;
}

// HQ, its protected area, and four linked spawn points facing outward.
void hq_bundle(Plan& p, int team, const double at[3], int obj_id, const std::string& prefix)
{
    Obj hq = spawn("HQ_PlayerSpawner", at, 0.0, prefix);
    hq.props = {{"ObjId", (double)obj_id}, {"Team", (double)team}, {"AltTeam", (double)(team == 1 ? 2 : 1)}};
    std::vector<Obj> kids;
    kids.push_back(square_volume(at, 8.0, 10.0, prefix + "_Area", prefix));
    std::vector<std::string> spawns;
    for (int i = 0; i < 4; ++i) {
        const double a = PI * 0.25 + PI * 0.5 * i;
        const double pos[3] = {at[0] + std::cos(a) * 5.0, at[1], at[2] + std::sin(a) * 5.0};
        Obj s = spawn("SpawnPoint", pos, a, fmt("%s_Spawn%d", prefix.c_str(), i + 1), prefix);
        spawns.push_back(s.key);
        kids.push_back(s);
    }
    hq.links = {{"InfantrySpawns", spawns}, {"HQArea", {prefix + "_Area"}}};
    p.objs.push_back(hq);
    for (Obj& k : kids) p.objs.push_back(k);
}

// Capture point, its capture area, and four spawns per team facing the flag.
void flag_bundle(Plan& p, const double at[3], int obj_id, const std::string& prefix)
{
    Obj cp = spawn("CapturePoint", at, 0.0, prefix);
    cp.props = {{"ObjId", (double)obj_id}};
    std::vector<Obj> kids;
    kids.push_back(square_volume(at, 6.0, 10.0, prefix + "_Area", prefix));
    std::vector<std::string> t1, t2;
    for (int i = 0; i < 8; ++i) {
        const double a = PI * 0.25 * i;
        const double pos[3] = {at[0] + std::cos(a) * 8.0, at[1], at[2] + std::sin(a) * 8.0};
        Obj s = spawn("SpawnPoint", pos, a + PI, fmt("%s_Spawn%d", prefix.c_str(), i + 1), prefix);
        (i % 2 == 0 ? t1 : t2).push_back(s.key);
        kids.push_back(s);
    }
    cp.links = {{"CaptureArea", {prefix + "_Area"}}, {"InfantrySpawnPoints_Team1", t1}, {"InfantrySpawnPoints_Team2", t2}};
    p.objs.push_back(cp);
    for (Obj& k : kids) p.objs.push_back(k);
    p.root = prefix;
}

void sector(Plan& p, const double at[3], int obj_id, const std::string& name, const std::vector<std::string>& flag_names)
{
    Obj s = spawn("Sector", at, 0.0, name);
    s.props = {{"ObjId", (double)obj_id}};
    std::vector<std::string> refs;
    for (const std::string& f : flag_names) refs.push_back("@" + f);
    if (!refs.empty()) s.links = {{"CapturePoints", refs}};
    p.objs.push_back(s);
}

// The next free ObjId in a community band: flags from 200, HQs in the 300s,
// sectors in the 100s, MCOMs in the 400s.
int next_id(const Value* used, int base, int span)
{
    int best = base - 1;
    if (used && used->is_arr())
        for (const Value& u : used->arr)
            if (u.type == Value::Num) {
                const int id = (int)u.num;
                if (id >= base && id < base + span) best = std::max(best, id);
            }
    return best + 1;
}

bool read_at(const Value& req, double at[3])
{
    const Value* a = req.find("at");
    if (!a || !a->is_arr() || a->arr.size() < 3) return false;
    for (int i = 0; i < 3; ++i) at[i] = getn(&a->arr[i], 0.0);
    return true;
}

std::string letter(int i) { return std::string(1, (char)('A' + std::max(0, std::min(25, i)))); }

const char* hq_body_first =
    "An HQ is where this team comes back in after dying, and the one place they cannot be shot while doing it.";
const char* hq_body_rest =
    "\n\nPut it behind the team's side of the map, with cover between it and the fighting. The two HQs facing each other across open ground is how a map ends up as a spawn-camp."
    "\n\nOne click builds the HQ, a protected area around it, and four spawn points inside that area, all linked. They are ordinary objects afterwards - move them, reshape the area, add more spawns.";

} // namespace

extern "C" int64_t bf6_mode_plan(const char* request_json, size_t len, uint8_t** out)
{
    if (!out) return -1;
    *out = nullptr;
    if (!request_json) return -1;
    Value req;
    std::string err;
    bf6json::Parser parser(request_json, len ? len : std::strlen(request_json));
    if (!parser.parse(req, err) || !req.is_obj()) return -1;
    const std::string op = req.find("op") ? req.find("op")->as_str("") : "";
    std::string j = "{";

    if (op == "bundles") {
        j += "\"bundles\":["
             "{\"key\":\"HQ1\",\"label\":\"HQ TEAM 1\",\"sub\":\"hq, area, 4 spawns\"},"
             "{\"key\":\"HQ2\",\"label\":\"HQ TEAM 2\",\"sub\":\"hq, area, 4 spawns\"},"
             "{\"key\":\"FLAG\",\"label\":\"FLAG\",\"sub\":\"capture point, area, 8 spawns\"},"
             "{\"key\":\"SECTOR\",\"label\":\"SECTOR\",\"sub\":\"sector and its area\"},"
             "{\"key\":\"MCOM\",\"label\":\"MCOM\",\"sub\":\"one objective, on its own\"}]";
    } else if (op == "bundle") {
        double at[3];
        if (!read_at(req, at)) return -1;
        const std::string key = req.find("key") ? req.find("key")->as_str("") : "";
        const Value* used = req.find("used_ids");
        Plan p;
        if (key == "HQ1" || key == "HQ2") {
            const int team = key == "HQ2" ? 2 : 1;
            const int id = next_id(used, 301, 99);
            p.root = fmt("Team%d_HQ_%d", team, id);
            hq_bundle(p, team, at, id, p.root);
        } else if (key == "FLAG") {
            // A is 200, so the letter follows from the id rather than a counter.
            const int id = next_id(used, 200, 99);
            flag_bundle(p, at, id, "CapturePoint_" + letter(id - 200));
        } else if (key == "SECTOR") {
            const int id = next_id(used, 100, 99);
            p.root = fmt("Sector_%d", id - 99);
            sector(p, at, id, p.root, {});
            // A sector with no area covers nothing, so it gets the one field it
            // cannot work without and the creator shapes it.
            p.objs.push_back(square_volume(at, 20.0, 10.0, p.root + "_SectorArea", p.root));
            p.objs[0].links.push_back({"SectorArea", {p.root + "_SectorArea"}});
        } else if (key == "MCOM") {
            // MCOM has no link fields: the bundle is the object. It carries its id
            // so the next one lands on the next number.
            const int id = next_id(used, 400, 99);
            p.root = fmt("MCOM_%d", id - 399);
            Obj m = spawn("MCOM", at, 0.0, p.root);
            m.props = {{"ObjId", (double)id}};
            p.objs.push_back(m);
        } else {
            return -1;
        }
        write_plan(j, p);
    } else if (op == "wizard") {
        const bool conquest = (req.find("mode") ? req.find("mode")->as_str("") : "") != "Breakthrough";
        const int count = std::max(1, std::min(conquest ? 7 : 6, (int)getn(req.find("count"), 3)));
        const int total = conquest ? count + 2 : count * 4;
        const int step = std::max(0, (int)getn(req.find("step"), 0));
        j += fmt("\"mode\":\"%s\",\"count\":%d,\"total\":%d,\"step\":%d", conquest ? "Conquest" : "Breakthrough", count, total, step);
        const bool done = step >= total;
        j += done ? ",\"done\":true" : ",\"done\":false";
        if (!done) {
            std::string title;
            bool hq, first = false;
            if (conquest) {
                hq = step <= 1;
                first = step == 0;
                title = step == 0 ? "Place the Team 1 HQ" : step == 1 ? "Place the Team 2 HQ" : "Place flag " + letter(step - 2);
            } else {
                const int s = step / 4 + 1, sub = step % 4;
                hq = sub <= 1;
                first = sub == 0;
                title = sub == 0 ? fmt("Sector %d: place the attacker HQ (Team 1)", s)
                      : sub == 1 ? fmt("Sector %d: place the defender HQ (Team 2)", s)
                      : fmt("Sector %d: place objective %s", s, sub == 2 ? "A" : "B");
            }
            std::string body;
            if (hq)
                body = std::string(hq_body_first) + (first ? "" : " This is the second one, so keep it well away from the first.") + hq_body_rest;
            else if (conquest)
                body = "A flag is a place worth fighting over, so it wants cover, more than one way in, and no single window that owns it."
                       "\n\nSpread flags out: players walk between them, and two flags close together turn the map into one fight instead of several."
                       "\n\nOne click builds the capture point, its capture area, and eight spawn points split between the two teams and facing the flag. When every flag is down, the sector is wired up for you and the checks run.";
            else
                body = "An objective is what the attackers have to take before the sector moves on. Two per sector, far enough apart that one defensive position cannot hold both."
                       "\n\nOne click builds the capture point, its area, and spawns for both teams. When both are down this sector is wired and the next one begins.";
            j += ",\"title\":"; jstr(j, title);
            j += ",\"body\":"; jstr(j, body);
            j += fmt(",\"step_label\":\"Step %d of %d\"", step + 1, total);

            double at[3];
            if (read_at(req, at)) {
                // The flags placed so far in this sector (conquest: all of them).
                std::vector<std::string> names;
                std::vector<std::array<double, 3>> pos;
                if (const Value* f = req.find("flags"); f && f->is_arr())
                    for (const Value& e : f->arr) {
                        if (!e.is_obj()) continue;
                        names.push_back(e.find("name") ? e.find("name")->as_str("") : "");
                        std::array<double, 3> q = {0, 0, 0};
                        if (const Value* a = e.find("at"); a && a->is_arr() && a->arr.size() >= 3)
                            for (int i = 0; i < 3; ++i) q[i] = getn(&a->arr[i], 0.0);
                        pos.push_back(q);
                    }
                Plan p;
                bool record = false, reset = false;
                if (conquest) {
                    if (step == 0) { p.root = "Team1_HQ"; hq_bundle(p, 1, at, 301, p.root); }
                    else if (step == 1) { p.root = "Team2_HQ"; hq_bundle(p, 2, at, 302, p.root); }
                    else { flag_bundle(p, at, 200 + (step - 2), "CapturePoint_" + letter(step - 2)); record = true; }
                    if (step + 1 >= total) {
                        if (record) { names.push_back(p.root); pos.push_back({at[0], at[1], at[2]}); }
                        double c[3] = {0, 0, 0};
                        for (const auto& q : pos) for (int i = 0; i < 3; ++i) c[i] += q[i];
                        if (!pos.empty()) for (int i = 0; i < 3; ++i) c[i] /= (double)pos.size();
                        sector(p, c, 100, "Sector_1", names);
                    }
                } else {
                    const int s = step / 4, sub = step % 4;
                    if (sub == 0) { p.root = fmt("S%d_Team1_HQ", s + 1); hq_bundle(p, 1, at, 301 + s, p.root); }
                    else if (sub == 1) { p.root = fmt("S%d_Team2_HQ", s + 1); hq_bundle(p, 2, at, 401 + s, p.root); }
                    else { flag_bundle(p, at, 1100 + s * 100 + (sub - 2), fmt("S%d_Objective%s", s + 1, sub == 2 ? "A" : "B")); record = true; }
                    if ((step + 1) % 4 == 0) {
                        if (record) { names.push_back(p.root); pos.push_back({at[0], at[1], at[2]}); }
                        double c[3] = {0, 0, 0};
                        for (const auto& q : pos) for (int i = 0; i < 3; ++i) c[i] += q[i];
                        if (!pos.empty()) for (int i = 0; i < 3; ++i) c[i] /= (double)pos.size();
                        sector(p, c, 100 + s + 1, fmt("Sector_%d", s + 1), names);
                        reset = true;
                    }
                }
                j += ",\"plan\":{";
                write_plan(j, p);
                j += "}";
                j += record ? ",\"record_flag\":true" : ",\"record_flag\":false";
                j += reset ? ",\"reset_flags\":true" : ",\"reset_flags\":false";
                j += step + 1 >= total ? ",\"finishes\":true" : ",\"finishes\":false";
            }
        }
        const char* m = conquest ? "Conquest" : "Breakthrough";
        j += ",\"finish_clean\":"; jstr(j, fmt("%s setup complete - checks came back clean. Everything is a normal object now, move and edit freely.", m));
        j += ",\"finish_problems\":"; jstr(j, fmt("%s setup complete - CHECKS found {n} problem{s} worth a look.", m));
    } else {
        return -1;
    }
    j += "}";
    *out = (uint8_t*)std::malloc(j.size() + 1);
    if (!*out) return -1;
    std::memcpy(*out, j.data(), j.size());
    (*out)[j.size()] = 0;
    return (int64_t)j.size();
}
