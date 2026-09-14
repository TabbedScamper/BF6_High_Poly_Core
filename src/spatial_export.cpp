/* The .spatial.json a map uploads as, written the same way for both SDK editors.
 *
 * The rules are the Portal SDK's own exporter's (code/gdconverter,
 * export_tscn.py and _tscn_to_json.py), taken over so the Unreal tool and the
 * Godot SDK send the site the same file for the same map:
 *   - an object's id is its path in the scene (the SDK exporter drops authored
 *     ids); a repeated path gets a suffix
 *   - only Portal types go in; anything under a node named "hidden" stays out
 *   - a selection is written by name, a link by the id of what it names, and the
 *     "linked" list says which fields are links
 *   - values equal to the type's default are left out, as the SDK scenes never
 *     store them, except the ObjId shim on six types
 *   - a polygon volume carries world points and a height instead of a transform,
 *     an OBB volume its size, and a waypoint path is its own entity
 *   - the Static layer holds the map's terrain and assets entries
 * See bf6_spatial_export in bf6_core.h. */
#include "bf6_core.h"
#include "json.hpp"
#include "utf8_fs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {

using bf6json::Value;

std::string lower(std::string s)
{
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

std::string str(const Value* v) { return v && v->is_str() ? v->str : std::string(); }

// ---------------------------------------------------------------- the asset catalogue

enum class Kind { Bool, Int, Float, String, Selection, Vector, Ref, RefArray, Other };

struct Prop {
    std::string name;
    Kind kind = Kind::Other;
    std::string ref_type;          // Ref / RefArray: the type it names
    bool has_default = false;
    Value def;
    std::vector<std::string> selections;
};

struct AssetType {
    std::string type, category;
    std::vector<Prop> props;
    std::vector<std::string> restrictions;   // lower case level names
    const Prop* prop(const std::string& n) const
    {
        for (const Prop& p : props) if (p.name == n) return &p;
        return nullptr;
    }
};

struct Catalogue {
    std::string path;
    int64_t size = -1, mtime = -1;
    std::map<std::string, AssetType> types;   // lower case type -> type
};

std::mutex g_mutex;
Catalogue g_cat;

const char* const OBJ_ID_TYPES[] = {"Bomb", "CapturePoint", "DeployCam", "RingOfFire", "MCOM", "Sector"};

bool obj_id_shim(const std::string& type)
{
    for (const char* t : OBJ_ID_TYPES) if (type == t) return true;
    return false;
}

bool load_catalogue(const std::string& path, std::string& err)
{
    int64_t size = 0, mtime = 0;
    if (!bf6fs::stat_file(path, size, mtime)) { err = "asset_types.json was not found at " + path; return false; }
    if (g_cat.path == path && g_cat.size == size && g_cat.mtime == mtime) return true;
    std::string text;
    Value v;
    if (!bf6fs::read_all(path, text)) { err = "asset_types.json could not be read"; return false; }
    bf6json::Parser parser(text.c_str(), text.size());
    if (!parser.parse(v, err) || !v.is_obj()) { err = "asset_types.json is not valid JSON"; return false; }
    const Value* rows = v.find("AssetTypes");
    if (!rows || !rows->is_arr()) { err = "asset_types.json has no AssetTypes"; return false; }

    Catalogue c;
    c.path = path; c.size = size; c.mtime = mtime;
    std::set<std::string> custom = {"CollisionPolygon3D"};
    for (const Value& row : rows->arr) if (const Value* t = row.find("type"); t && t->is_str()) custom.insert(t->str);
    for (const Value& row : rows->arr) {
        AssetType a;
        a.type = str(row.find("type"));
        if (a.type.empty()) continue;
        if (const Value* k = row.find("constants"); k && k->is_arr())
            for (const Value& con : k->arr)
                if (str(con.find("name")) == "category") a.category = lower(str(con.find("value")));
        if (const Value* r = row.find("levelRestrictions"); r && r->is_arr())
            for (const Value& l : r->arr) if (l.is_str()) a.restrictions.push_back(lower(l.str));
        if (const Value* ps = row.find("properties"); ps && ps->is_arr())
            for (const Value& pv : ps->arr) {
                Prop p;
                p.name = str(pv.find("name"));
                std::string t = str(pv.find("type"));
                if (p.name.empty() || t.empty()) continue;
                bool arr = false;
                if (t.size() > 2 && t.compare(t.size() - 2, 2, "[]") == 0) { arr = true; t = t.substr(0, t.size() - 2); }
                else if (t.rfind("Array[", 0) == 0 && t.back() == ']') { arr = true; t = t.substr(6, t.size() - 7); }
                if (custom.count(t)) { p.kind = arr ? Kind::RefArray : Kind::Ref; p.ref_type = t; }
                else if (arr) p.kind = Kind::Other;
                else if (t == "bool") p.kind = Kind::Bool;
                else if (t == "int") p.kind = Kind::Int;
                else if (t == "float") p.kind = Kind::Float;
                else if (t == "string") p.kind = Kind::String;
                else if (t == "selection") p.kind = Kind::Selection;
                else if (t == "vector") p.kind = Kind::Vector;
                else if (t == "reference") p.kind = Kind::Ref;
                if (const Value* d = pv.find("default")) { p.has_default = true; p.def = *d; }
                if (const Value* s = pv.find("selections"); s && s->is_arr())
                    for (const Value& e : s->arr) if (e.is_str()) p.selections.push_back(e.str);
                a.props.push_back(std::move(p));
            }
        c.types[lower(a.type)] = std::move(a);
    }
    g_cat = std::move(c);
    return true;
}

// ---------------------------------------------------------------- output tree

struct Out {
    enum K { Null, Bool, Num, Str, Arr, Obj } k = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Out> a;
    std::vector<std::pair<std::string, Out>> o;

    static Out boolean(bool v) { Out r; r.k = Bool; r.b = v; return r; }
    static Out number(double v) { Out r; r.k = Num; r.n = v; return r; }
    static Out text(const std::string& v) { Out r; r.k = Str; r.s = v; return r; }
    static Out array() { Out r; r.k = Arr; return r; }
    static Out object() { Out r; r.k = Obj; return r; }
    Out& set(const std::string& key, Out v)
    {
        for (auto& kv : o) if (kv.first == key) { kv.second = std::move(v); return kv.second; }
        o.push_back({key, std::move(v)});
        return o.back().second;
    }
    Out* get(const std::string& key)
    {
        for (auto& kv : o) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    void erase(const std::string& key)
    {
        o.erase(std::remove_if(o.begin(), o.end(), [&](const auto& kv) { return kv.first == key; }), o.end());
    }
};

Out vec3(double x, double y, double z)
{
    Out v = Out::object();
    v.set("x", Out::number(x));
    v.set("y", Out::number(y));
    v.set("z", Out::number(z));
    return v;
}

// The SDK's numbers: whole values as integers, others as the shortest text that
// reads back - a float's when the value is one, as the editors' scenes store.
std::string number_text(double v)
{
    if (!std::isfinite(v)) return "0";
    if (v == 0.0) return "0";
    char b[40];
    if (std::fabs(v) < 1e15 && v == std::floor(v)) {
        std::snprintf(b, sizeof(b), "%lld", (long long)v);
        return b;
    }
    // At the precision a Godot scene stores - a float - so the same transform
    // reads the same from either editor, whether it arrives as a float printed
    // to 15 digits (0.101055979728699 is the float 0.10105598) or as a double.
    const float f = (float)v;
    if (std::fabs(v) < 3.0e38) {
        for (int p = 6; p <= 9; ++p) {
            std::snprintf(b, sizeof(b), "%.*g", p, (double)f);
            if ((float)std::strtod(b, nullptr) == f) return b;
        }
    }
    for (int p = 15; p <= 17; ++p) {
        std::snprintf(b, sizeof(b), "%.*g", p, v);
        if (std::strtod(b, nullptr) == v) return b;
    }
    return b;
}

// JSON strings as Python's json.dump writes them: anything past ASCII escaped.
void quote(std::string& out, const std::string& s)
{
    out.push_back('"');
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp = c;
        size_t len = 1;
        if (c >= 0x80) {
            if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); len = 2; }
            else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) { cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); len = 3; }
            else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) { cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); len = 4; }
            else cp = 0xFFFD;
        }
        i += len;
        char e[16];
        switch (cp) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (cp < 0x20 || (cp >= 0x7F && cp < 0x10000)) { std::snprintf(e, sizeof(e), "\\u%04x", cp); out += e; }
            else if (cp >= 0x10000) {
                cp -= 0x10000;
                std::snprintf(e, sizeof(e), "\\u%04x\\u%04x", 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
                out += e;
            } else out.push_back((char)cp);
        }
    }
    out.push_back('"');
}

void write(std::string& out, const Out& v, bool pretty, int depth)
{
    auto newline = [&](int d) { if (pretty) { out.push_back('\n'); out.append((size_t)d * 4, ' '); } };
    switch (v.k) {
    case Out::Null: out += "null"; break;
    case Out::Bool: out += v.b ? "true" : "false"; break;
    case Out::Num: out += number_text(v.n); break;
    case Out::Str: quote(out, v.s); break;
    case Out::Arr:
        if (v.a.empty()) { out += "[]"; break; }
        out.push_back('[');
        for (size_t i = 0; i < v.a.size(); ++i) {
            if (i) out.push_back(',');
            newline(depth + 1);
            write(out, v.a[i], pretty, depth + 1);
        }
        newline(depth);
        out.push_back(']');
        break;
    case Out::Obj:
        if (v.o.empty()) { out += "{}"; break; }
        out.push_back('{');
        for (size_t i = 0; i < v.o.size(); ++i) {
            if (i) out.push_back(',');
            newline(depth + 1);
            quote(out, v.o[i].first);
            out += pretty ? ": " : ":";
            write(out, v.o[i].second, pretty, depth + 1);
        }
        newline(depth);
        out.push_back('}');
        break;
    }
}

Out copy_value(const Value& v)
{
    switch (v.type) {
    case Value::Null: return Out();
    case Value::Bool: return Out::boolean(v.b);
    case Value::Num: return Out::number(v.num);
    case Value::Str: return Out::text(v.str);
    case Value::Arr: { Out r = Out::array(); for (const Value& e : v.arr) r.a.push_back(copy_value(e)); return r; }
    case Value::Obj: { Out r = Out::object(); for (const auto& kv : v.obj) r.set(kv.first, copy_value(kv.second)); return r; }
    }
    return Out();
}

// ---------------------------------------------------------------- values

std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// A scene's quoted text ("\"TEAM_1_HQ\"") read as the text inside.
std::string unquote(const std::string& s)
{
    std::string t = trim(s);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') return t.substr(1, t.size() - 2);
    return t;
}

std::vector<std::string> split_list(const std::string& s)
{
    std::vector<std::string> out;
    std::string t = trim(s);
    if (!t.empty() && t.front() == '[' && t.back() == ']') t = t.substr(1, t.size() - 2);
    size_t at = 0;
    while (at <= t.size()) {
        size_t c = t.find(',', at);
        if (c == std::string::npos) c = t.size();
        std::string part = unquote(t.substr(at, c - at));
        if (!part.empty()) out.push_back(part);
        at = c + 1;
    }
    return out;
}

bool as_number(const Value& v, double& out)
{
    if (v.type == Value::Num) { out = v.num; return true; }
    if (v.type == Value::Bool) { out = v.b ? 1 : 0; return true; }
    if (v.is_str()) {
        const std::string t = unquote(v.str);
        if (t.empty()) return false;
        char* end = nullptr;
        out = std::strtod(t.c_str(), &end);
        return end && *end == 0;
    }
    return false;
}

bool as_vector(const Value& v, double out[3])
{
    if (v.is_arr() && v.arr.size() >= 3) {
        for (int i = 0; i < 3; ++i) if (!as_number(v.arr[i], out[i])) return false;
        return true;
    }
    if (v.is_obj()) {
        const Value* x = v.find("x"), *y = v.find("y"), *z = v.find("z");
        return x && y && z && as_number(*x, out[0]) && as_number(*y, out[1]) && as_number(*z, out[2]);
    }
    if (v.is_str()) {
        std::string t = trim(v.str);
        if (t.rfind("Vector3(", 0) == 0) t = t.substr(8);
        std::vector<std::string> parts = split_list(t.back() == ')' ? t.substr(0, t.size() - 1) : t);
        if (parts.size() < 3) return false;
        for (int i = 0; i < 3; ++i) {
            char* end = nullptr;
            out[i] = std::strtod(parts[i].c_str(), &end);
            if (!end || *end) return false;
        }
        return true;
    }
    return false;
}

// A selection by name; an index, or its text, picks by position as the SDK does
// (out of range: the first).
std::string selection_name(const Prop& p, const Value& v)
{
    if (p.selections.empty()) return v.is_str() ? unquote(v.str) : std::string();
    if (v.is_str()) {
        const std::string t = unquote(v.str);
        if (std::find(p.selections.begin(), p.selections.end(), t) != p.selections.end()) return t;
    }
    double idx = 0;
    if (as_number(v, idx)) {
        const long i = (long)idx;
        return i >= 0 && i < (long)p.selections.size() ? p.selections[(size_t)i] : p.selections[0];
    }
    return v.is_str() ? unquote(v.str) : p.selections[0];
}

// What the SDK's generated scripts start a field at.
Out default_of(const Prop& p, const std::string& type)
{
    if (p.name == "ObjId" && obj_id_shim(type)) return Out::number(0);
    switch (p.kind) {
    case Kind::Bool: return Out::boolean(p.has_default && p.def.type == Value::Bool ? p.def.b : false);
    case Kind::Int:
    case Kind::Float: { double d = 0; if (p.has_default) as_number(p.def, d); return Out::number(p.kind == Kind::Int ? std::round(d) : d); }
    case Kind::String: return Out::text(p.has_default && p.def.is_str() ? p.def.str : std::string());
    case Kind::Selection: {
        if (p.selections.empty()) return Out::text(std::string());
        std::string d = p.has_default && p.def.is_str() ? p.def.str : std::string();
        if (std::find(p.selections.begin(), p.selections.end(), d) == p.selections.end()) d = p.selections[0];
        return Out::text(d);
    }
    case Kind::Vector: {
        double d[3] = {0, 0, 0};
        if (p.has_default) as_vector(p.def, d);
        Out r = Out::array();
        for (double x : d) r.a.push_back(Out::number(x));
        return r;
    }
    default: return Out();
    }
}

bool same(const Out& a, const Out& b)
{
    if (a.k != b.k) return false;
    switch (a.k) {
    case Out::Null: return true;
    case Out::Bool: return a.b == b.b;
    case Out::Num: return std::fabs(a.n - b.n) <= 1e-9 * std::max(1.0, std::fabs(a.n));
    case Out::Str: return a.s == b.s;
    case Out::Arr:
        if (a.a.size() != b.a.size()) return false;
        for (size_t i = 0; i < a.a.size(); ++i) if (!same(a.a[i], b.a[i])) return false;
        return true;
    case Out::Obj:
        if (a.o.size() != b.o.size()) return false;
        for (size_t i = 0; i < a.o.size(); ++i) if (a.o[i].first != b.o[i].first || !same(a.o[i].second, b.o[i].second)) return false;
        return true;
    }
    return false;
}

// A plain field value in the type the catalogue gives it; false to leave it out.
bool plain_value(const Prop& p, const Value& v, Out& out)
{
    switch (p.kind) {
    case Kind::Bool:
        if (v.type == Value::Bool) { out = Out::boolean(v.b); return true; }
        if (v.is_str()) { const std::string t = lower(unquote(v.str)); if (t == "true" || t == "false") { out = Out::boolean(t == "true"); return true; } }
        { double d; if (as_number(v, d)) { out = Out::boolean(d != 0); return true; } }
        return false;
    case Kind::Int: { double d; if (!as_number(v, d)) return false; out = Out::number(std::round(d)); return true; }
    case Kind::Float: { double d; if (!as_number(v, d)) return false; out = Out::number(d); return true; }
    case Kind::String: out = Out::text(v.is_str() ? unquote(v.str) : std::string()); return v.is_str();
    case Kind::Selection: out = Out::text(selection_name(p, v)); return true;
    case Kind::Vector: {
        double d[3];
        if (!as_vector(v, d)) return false;
        out = Out::array();
        for (double x : d) out.a.push_back(Out::number(x));
        return true;
    }
    default:
        if (v.is_str() && (v.str.find("NodePath") != std::string::npos || v.str.find("ExtResource") != std::string::npos)) return false;
        out = copy_value(v);
        return v.type != Value::Null;
    }
}

// ---------------------------------------------------------------- the scene

struct Obj {
    const Value* src = nullptr;
    std::string key, name, type, authored_id, id;
    const AssetType* asset = nullptr;
    bool is_static = false;
};

double polygon_area(const Out& points)
{
    double s = 0;
    const size_t n = points.a.size();
    for (size_t i = 0; i < n; ++i) {
        const Out& p = points.a[i];
        const Out& q = points.a[(i + 1) % n];
        const Out* px = const_cast<Out&>(p).get("x"), *pz = const_cast<Out&>(p).get("z");
        const Out* qx = const_cast<Out&>(q).get("x"), *qz = const_cast<Out&>(q).get("z");
        if (px && pz && qx && qz) s += px->n * qz->n - qx->n * pz->n;
    }
    return std::fabs(s) / 2.0;
}

std::string short_name(int n)
{
    std::string r;
    while (n > 0) { --n; r.insert(r.begin(), (char)('a' + n % 26)); n /= 26; }
    return r;
}

int64_t give(const std::string& text, uint8_t** out)
{
    if (!out) return (int64_t)text.size();
    *out = (uint8_t*)std::malloc(text.size() + 1);
    if (!*out) return -1;
    std::memcpy(*out, text.data(), text.size());
    (*out)[text.size()] = 0;
    return (int64_t)text.size();
}

void json_list(std::string& j, const std::vector<std::string>& items)
{
    j.push_back('[');
    for (size_t i = 0; i < items.size(); ++i) { if (i) j.push_back(','); quote(j, items[i]); }
    j.push_back(']');
}

} // namespace

extern "C" int64_t bf6_spatial_export(const char* request_json, size_t len, uint8_t** out, uint8_t** report)
{
    if (out) *out = nullptr;
    if (report) *report = nullptr;
    if (!out || !request_json) return -1;
    Value req;
    std::string err;
    bf6json::Parser parser(request_json, len ? len : std::strlen(request_json));
    if (!parser.parse(req, err) || !req.is_obj()) return -1;

    std::vector<std::string> warnings, errors;
    std::vector<std::pair<std::string, std::string>> skipped;
    const std::string level = str(req.find("level"));
    const bool pretty = !(req.find("pretty") && req.find("pretty")->type == Value::Bool && !req.find("pretty")->b);
    const bool short_ids = req.find("short_ids") && req.find("short_ids")->type == Value::Bool && req.find("short_ids")->b;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!load_catalogue(str(req.find("asset_types")), err)) {
        if (report) give("{\"errors\":[" + [&] { std::string q; quote(q, err); return q; }() + "]}", report);
        return -1;
    }

    // Who is in: Portal types and the Static entries, never anything hidden.
    std::vector<Obj> objs;
    if (const Value* list = req.find("objects"); list && list->is_arr())
        for (const Value& o : list->arr) {
            if (!o.is_obj()) continue;
            Obj ob;
            ob.src = &o;
            ob.key = str(o.find("key"));
            ob.name = str(o.find("name"));
            ob.type = str(o.find("type"));
            if (ob.name.empty()) ob.name = ob.key.substr(ob.key.find_last_of('/') + 1);
            if (ob.key.empty()) ob.key = ob.name;
            if (ob.key.empty()) continue;
            if (lower(ob.key).find("hidden") != std::string::npos) { skipped.push_back({ob.key, "hidden"}); continue; }
            ob.is_static = o.find("static") && o.find("static")->type == Value::Bool && o.find("static")->b;
            if (!ob.is_static) {
                auto it = g_cat.types.find(lower(ob.type));
                if (it == g_cat.types.end()) { skipped.push_back({ob.key, ob.type.empty() ? "no type" : "not a Portal type: " + ob.type}); continue; }
                ob.asset = &it->second;
                ob.type = it->second.type;
            }
            if (const Value* id = o.find("id"); id && id->is_str()) ob.authored_id = unquote(id->str);
            else if (const Value* props = o.find("props")) if (const Value* pid = props->find("id"); pid && pid->is_str()) ob.authored_id = unquote(pid->str);
            objs.push_back(ob);
        }

    // Ids are paths, as the SDK exporter writes them - it discards authored ids -
    // and a repeated path gets a suffix. An authored id still names its object
    // in a link.
    std::set<std::string> used;
    for (Obj& ob : objs) {
        std::string id = ob.key;
        if (used.count(id)) {
            int n = 2;
            while (used.count(id + "_" + std::to_string(n))) ++n;
            warnings.push_back("Two objects share the path " + id + ": the second exports as " + id + "_" + std::to_string(n));
            id += "_" + std::to_string(n);
        }
        used.insert(id);
        ob.id = id;
    }
    // A link names an object by key, authored id or (when only one has it) name.
    std::map<std::string, const Obj*> by_key, by_id;
    std::map<std::string, std::vector<const Obj*>> by_name;
    for (const Obj& ob : objs) {
        by_key.emplace(ob.key, &ob);
        by_id.emplace(ob.id, &ob);
        if (!ob.authored_id.empty()) by_id.emplace(ob.authored_id, &ob);
        by_name[ob.name].push_back(&ob);
    }
    auto resolve = [&](const std::string& ref) -> const Obj* {
        if (auto k = by_key.find(ref); k != by_key.end()) return k->second;
        if (auto i = by_id.find(ref); i != by_id.end()) return i->second;
        if (auto n = by_name.find(ref); n != by_name.end() && !n->second.empty()) return n->second[0];
        return nullptr;
    };

    Out dynamic = Out::array(), statics = Out::array();
    std::vector<Out> paths;
    std::map<const Obj*, size_t> entry_of;   // index into dynamic
    const bool autotest = lower(level).find("autotest") != std::string::npos;

    for (const Obj& ob : objs) {
        const Value& o = *ob.src;
        std::vector<double> origin(3, 0.0), basis = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        if (const Value* a = o.find("origin"); a && a->is_arr())
            for (size_t i = 0; i < 3 && i < a->arr.size(); ++i) as_number(a->arr[i], origin[i]);
        if (const Value* b = o.find("basis"); b && b->is_arr() && b->arr.size() >= 9)
            for (size_t i = 0; i < 9; ++i) as_number(b->arr[i], basis[i]);
        auto transform = [&](Out& e) {
            e.set("right", vec3(basis[0], basis[1], basis[2]));
            e.set("up", vec3(basis[3], basis[4], basis[5]));
            e.set("front", vec3(basis[6], basis[7], basis[8]));
            e.set("position", vec3(origin[0], origin[1], origin[2]));
        };
        Out e = Out::object();
        e.set("name", Out::text(ob.name));

        if (ob.is_static) {
            if (const Value* ex = o.find("extra"); ex && ex->is_obj())
                for (const auto& kv : ex->obj) e.set(kv.first, copy_value(kv.second));
            e.set("type", Out::text(ob.type.empty() ? ob.name : ob.type));
            transform(e);
            e.set("id", Out::text(ob.id));
            statics.a.push_back(std::move(e));
            continue;
        }

        const AssetType& at = *ob.asset;
        if (!autotest && !at.restrictions.empty() && std::find(at.restrictions.begin(), at.restrictions.end(), lower(level)) == at.restrictions.end())
            warnings.push_back(ob.type + " " + ob.name + " (" + ob.id + ") is not usable in " + level);
        const bool polygon = at.category == "polygonvolume", obb = at.category == "obbvolume", waypoint = at.category == "waypointpath";
        e.set("type", Out::text(ob.type));

        // Fields in the catalogue's order; links hold the ids they name.
        const Value* props = o.find("props");
        const Value* links = o.find("links");
        std::vector<std::string> linked;
        bool objid_unset = false;
        for (const Prop& p : at.props) {
            if ((polygon && (p.name == "points" || p.name == "height")) || (obb && p.name == "size")
                || (waypoint && (p.name == "points" || p.name == "isClosed")))
                continue;
            const Value* v = links ? links->find(p.name.c_str()) : nullptr;
            if (!v && props) v = props->find(p.name.c_str());
            if (p.kind == Kind::Ref || p.kind == Kind::RefArray) {
                if (p.name == "Waypoints" && o.find("path")) continue;   // the owned path fills it below
                if (!v) continue;
                std::vector<std::string> refs;
                if (v->is_arr()) { for (const Value& r : v->arr) if (r.is_str()) for (const std::string& s : split_list(r.str)) refs.push_back(s); }
                else if (v->is_str()) refs = split_list(v->str);
                std::vector<std::string> ids;
                for (const std::string& r : refs) {
                    const Obj* t = resolve(r);
                    if (!t) { warnings.push_back(ob.id + " - " + p.name + " names " + r + ", which is not in the export"); continue; }
                    if (p.kind == Kind::RefArray && !p.ref_type.empty() && t->type != p.ref_type) {
                        warnings.push_back(ob.id + " - " + p.name + " wants " + p.ref_type + " but " + t->id + " is " + t->type + ": dropped");
                        continue;
                    }
                    ids.push_back(t->id);
                }
                if (p.kind == Kind::Ref) {
                    if (ids.empty()) continue;
                    e.set(p.name, Out::text(ids[0]));
                } else {
                    if (ids.empty()) continue;
                    Out arr = Out::array();
                    for (const std::string& id : ids) arr.a.push_back(Out::text(id));
                    e.set(p.name, std::move(arr));
                }
                linked.push_back(p.name);
                continue;
            }
            Out val;
            if (!v || !plain_value(p, *v, val)) continue;
            if (p.name == "ObjId" && obj_id_shim(ob.type)) {
                if (val.k == Out::Num && val.n == -1) objid_unset = true;   // the shim drops an unset id
                else e.set("ObjId", std::move(val));
                continue;
            }
            if (same(val, default_of(p, ob.type))) continue;
            e.set(p.name, std::move(val));
        }
        // The SDK's shim: these six always carry an ObjId, 0 when none was set.
        if (obj_id_shim(ob.type) && !objid_unset && !e.get("ObjId")) e.set("ObjId", Out::number(0));

        if (!polygon && !waypoint) transform(e);
        e.set("id", Out::text(ob.id));

        // A path this object owns is its own entity, linked as Waypoints.
        if (const Value* path = o.find("path"); path && path->is_obj()) {
            Out pe = Out::object();
            const std::string pid = ob.id + "/Waypoints";
            pe.set("name", Out::text(pid.substr(pid.find_last_of('/') + 1)));
            pe.set("type", Out::text("WaypointPath"));
            pe.set("id", Out::text(pid));
            Out pts = Out::array();
            if (const Value* pp = path->find("points"); pp && pp->is_arr())
                for (const Value& p : pp->arr) { double d[3]; if (as_vector(p, d)) pts.a.push_back(vec3(d[0], d[1], d[2])); }
            if (pts.a.empty()) warnings.push_back(ob.id + " has a waypoint path with no points");
            else {
                if (const Value* c = path->find("closed"); c && c->type == Value::Bool && c->b) pe.set("isClosed", Out::boolean(true));
                pe.set("points", std::move(pts));
                if (at.prop("Waypoints")) {
                    e.set("Waypoints", Out::text(pid));
                    linked.push_back("Waypoints");
                }
                paths.push_back(std::move(pe));
            }
        }
        if (!linked.empty()) {
            Out l = Out::array();
            for (const std::string& n : linked) l.a.push_back(Out::text(n));
            e.set("linked", std::move(l));
        }

        if (polygon) {
            Out h = Out::number(0);
            if (const Value* hv = o.find("height")) { double d; if (as_number(*hv, d)) h = Out::number(d); }
            else if (props) if (const Value* hp = props->find("height")) { double d; if (as_number(*hp, d)) h = Out::number(d); }
            e.set("height", std::move(h));
            Out pts = Out::array();
            if (const Value* pv = o.find("points"); pv && pv->is_arr())
                for (const Value& p : pv->arr) { double d[3]; if (as_vector(p, d)) pts.a.push_back(vec3(d[0], d[1], d[2])); }
            if (pts.a.size() < 3) {
                errors.push_back(ob.id + ": a volume with less than 3 points is unsupported");
                continue;
            }
            e.set("points", std::move(pts));
        } else if (obb) {
            Out size = Out::array();
            double d[3] = {1, 1, 1};
            if (const Value* sv = o.find("size")) as_vector(*sv, d);
            else if (props) if (const Value* sp = props->find("size")) as_vector(*sp, d);
            for (double x : d) size.a.push_back(Out::number(x));
            e.set("size", std::move(size));
        } else if (waypoint) {
            if (const Value* cv = props ? props->find("isClosed") : nullptr; cv) { Out b; Prop bp; bp.kind = Kind::Bool; if (plain_value(bp, *cv, b) && b.b) e.set("isClosed", Out::boolean(true)); }
            Out pts = Out::array();
            if (const Value* pv = o.find("points"); pv && pv->is_arr())
                for (const Value& p : pv->arr) { double d[3]; if (as_vector(p, d)) pts.a.push_back(vec3(d[0], d[1], d[2])); }
            e.set("points", std::move(pts));
        }
        if (const Value* ex = o.find("extra"); ex && ex->is_obj())
            for (const auto& kv : ex->obj) if (!e.get(kv.first)) e.set(kv.first, copy_value(kv.second));

        entry_of[&ob] = dynamic.a.size();
        dynamic.a.push_back(std::move(e));
    }
    for (Out& p : paths) dynamic.a.push_back(std::move(p));

    // The SDK exporter's hard stops, reported rather than silently written.
    for (const auto& oe : entry_of) {
        const Obj& ob = *oe.first;
        Out& e = dynamic.a[oe.second];
        const std::string t = lower(ob.type);
        if (t == "ringoffire") {
            for (const char* need : {"HardRestrictOBB", "RestrictShapeData"}) {
                bool found = false;
                for (const auto& kv : e.o) if (lower(kv.first) == lower(need)) found = true;
                if (!found) errors.push_back(ob.id + " - " + need + " must be set!");
            }
        } else if (t == "combatarea") {
            for (const auto& kv : e.o) {
                if (lower(kv.first) != "combatvolume" || kv.second.k != Out::Str) continue;
                for (Out& other : dynamic.a) {
                    Out* id = other.get("id");
                    Out* pts = other.get("points");
                    if (id && id->k == Out::Str && id->s == kv.second.s && pts && polygon_area(*pts) > 16640000.0)
                        errors.push_back(ob.id + " - CombatVolume total area exceeds limit 16640000");
                }
            }
        }
    }

    // Unreal's minifier: every Portal_Dynamic name and id becomes a, b, ... aa.
    std::map<std::string, std::string> map;
    if (short_ids) {
        int n = 1;
        for (Out& e : dynamic.a) if (Out* id = e.get("id"); id && id->k == Out::Str && !map.count(id->s)) map[id->s] = short_name(n++);
        auto rename = [&](Out& v) { if (v.k == Out::Str) if (auto it = map.find(v.s); it != map.end()) v.s = it->second; };
        for (Out& e : dynamic.a) {
            Out* id = e.get("id");
            const std::string mine = id && id->k == Out::Str ? id->s : std::string();
            if (id) rename(*id);
            if (Out* nm = e.get("name"); nm && map.count(mine)) nm->s = map[mine];
            if (Out* l = e.get("linked"); l && l->k == Out::Arr)
                for (Out& f : l->a) {
                    if (f.k != Out::Str) continue;
                    if (Out* v = e.get(f.s)) {
                        if (v->k == Out::Arr) for (Out& x : v->a) rename(x);
                        else rename(*v);
                    }
                }
        }
    }

    const size_t dynamic_count = dynamic.a.size(), static_count = statics.a.size();
    Out layers = Out::object();
    layers.set("Portal_Dynamic", std::move(dynamic));
    layers.set("Static", std::move(statics));
    std::string text;
    write(text, layers, pretty, 0);
    const int64_t n = give(text, out);
    if (report) {
        std::string r = "{\"dynamic\":" + std::to_string(dynamic_count) + ",\"static\":" + std::to_string(static_count)
                      + ",\"bytes\":" + std::to_string(text.size()) + ",\"skipped\":[";
        for (size_t i = 0; i < skipped.size(); ++i) {
            if (i) r.push_back(',');
            r += "{\"key\":"; quote(r, skipped[i].first); r += ",\"why\":"; quote(r, skipped[i].second); r += "}";
        }
        r += "],\"short_ids\":{";
        bool first = true;
        for (const auto& kv : map) { if (!first) r.push_back(','); first = false; quote(r, kv.first); r.push_back(':'); quote(r, kv.second); }
        r += "},\"warnings\":";
        json_list(r, warnings);
        r += ",\"errors\":";
        json_list(r, errors);
        r += "}";
        give(r, report);
    }
    return n;
}
