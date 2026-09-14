/* BLOCKS: reusable pieces a creator saves from a selection and places again, in
 * either SDK editor. One file format for both ("bf6-block/2", game metres, a
 * basis per object, links between members by index) in one shared library, so
 * a block saved in Unreal places in Godot and the other way round. Blocks the
 * Unreal tool saved before this format (centimetres, rotators, "Key=Value" prop
 * tags) are read and upgraded as they load. See bf6_block_save in bf6_core.h. */
#include "bf6_core.h"
#include "json.hpp"
#include "utf8_fs.h"

#include <algorithm>
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

const double PI = 3.14159265358979323846;

std::string str(const Value* v) { return v && v->is_str() ? v->str : std::string(); }
double num(const Value* v, double d) { return v && v->type == Value::Num ? v->num : d; }

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
std::string q(const std::string& s) { std::string o; jstr(o, s); return o; }

std::string numtxt(double v)
{
    if (std::fabs(v) < 1e-12) v = 0.0;
    char b[40];
    std::snprintf(b, sizeof(b), "%.9g", v);
    return b;
}

// A value as JSON text, preserving its type.
std::string jval(const Value& v)
{
    switch (v.type) {
    case Value::Null: return "null";
    case Value::Bool: return v.b ? "true" : "false";
    case Value::Num: return numtxt(v.num);
    case Value::Str: return q(v.str);
    case Value::Arr: {
        std::string s = "[";
        for (size_t i = 0; i < v.arr.size(); ++i) { if (i) s += ','; s += jval(v.arr[i]); }
        return s + "]";
    }
    case Value::Obj: {
        std::string s = "{";
        for (size_t i = 0; i < v.obj.size(); ++i) { if (i) s += ','; s += q(v.obj[i].first) + ":" + jval(v.obj[i].second); }
        return s + "}";
    }
    }
    return "null";
}

int64_t give(const std::string& text, uint8_t** out)
{
    *out = (uint8_t*)std::malloc(text.size() + 1);
    if (!*out) return -1;
    std::memcpy(*out, text.data(), text.size());
    (*out)[text.size()] = 0;
    return (int64_t)text.size();
}

bool parse(const std::string& text, Value& v)
{
    std::string err;
    bf6json::Parser p(text.c_str(), text.size());
    return p.parse(v, err) && v.is_obj();
}

// Letters, digits, space, '-' and '_'; anything else becomes '_'. A name that
// comes out empty is refused.
std::string safe_name(const std::string& in)
{
    std::string s;
    for (unsigned char c : in) s.push_back((std::isalnum(c) || c == '_' || c == '-' || c == ' ' || c >= 0x80) ? (char)c : '_');
    size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::vector<double> nums(const Value* arr, size_t n, double fill_value)
{
    std::vector<double> out(n, fill_value);
    if (arr && arr->is_arr())
        for (size_t i = 0; i < n && i < arr->arr.size(); ++i) out[i] = num(&arr->arr[i], fill_value);
    return out;
}

std::vector<std::string> list_dirs(const Value& req)
{
    std::vector<std::string> dirs;
    if (const Value* d = req.find("dirs"); d && d->is_arr())
        for (const Value& e : d->arr) if (e.is_str() && !e.str.empty()) dirs.push_back(e.str);
    if (const Value* d = req.find("dir"); d && d->is_str() && !d->str.empty()) dirs.insert(dirs.begin(), d->str);
    return dirs;
}

// ---------------------------------------------------------------- upgrading the Unreal tool's first format

// FRotator(pitch, yaw, roll) degrees -> Unreal's X, Y, Z axes (FRotationMatrix).
void rotator_axes(double pitch, double yaw, double roll, double x[3], double y[3], double z[3])
{
    const double p = pitch * PI / 180.0, w = yaw * PI / 180.0, r = roll * PI / 180.0;
    const double sp = std::sin(p), cp = std::cos(p), sy = std::sin(w), cy = std::cos(w), sr = std::sin(r), cr = std::cos(r);
    x[0] = cp * cy; x[1] = cp * sy; x[2] = sp;
    y[0] = sr * sp * cy - cr * sy; y[1] = sr * sp * sy + cr * cy; y[2] = -sr * cp;
    z[0] = -(cr * sp * cy + sr * sy); z[1] = cy * sr - cr * sp * sy; z[2] = cr * cp;
}

struct Member {
    std::string type, mesh, name;
    double origin[3] = {0, 0, 0};                     // game metres; relative in a file
    double basis[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};    // game basis columns x, y, z, scale included
    std::vector<std::pair<std::string, std::string>> props;   // name -> JSON value text
    std::vector<std::pair<std::string, std::vector<std::string>>> links;   // name -> "@i" or a name
    std::vector<double> points;   // a zone's polygon, x y z triples in the origin's space
};

// A v1 object: pos in centimetres (Unreal X, Y, Z), rot [pitch, yaw, roll],
// scale, props "Key=Value" with member links written "@3".
Member from_v1(const Value& o)
{
    Member m;
    m.type = str(o.find("type"));
    m.mesh = str(o.find("mesh"));
    const std::vector<double> p = nums(o.find("pos"), 3, 0.0), r = nums(o.find("rot"), 3, 0.0), s = nums(o.find("scale"), 3, 1.0);
    m.origin[0] = p[0] / 100.0; m.origin[1] = p[2] / 100.0; m.origin[2] = p[1] / 100.0;
    double ax[3], ay[3], az[3];
    rotator_axes(r[0], r[1], r[2], ax, ay, az);
    // The SDK's own swap, as its Godot exporter writes a transform: Godot's X is
    // Unreal's X, Godot's Y is Unreal's Z, Godot's Z is Unreal's Y, each axis in
    // (x, z, y) order.
    const double gx[3] = {ax[0] * s[0], ax[2] * s[0], ax[1] * s[0]};
    const double gy[3] = {az[0] * s[2], az[2] * s[2], az[1] * s[2]};
    const double gz[3] = {ay[0] * s[1], ay[2] * s[1], ay[1] * s[1]};
    for (int i = 0; i < 3; ++i) { m.basis[i] = gx[i]; m.basis[3 + i] = gy[i]; m.basis[6 + i] = gz[i]; }
    if (const Value* props = o.find("props"); props && props->is_arr())
        for (const Value& pv : props->arr) {
            if (!pv.is_str()) continue;
            const size_t eq = pv.str.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = pv.str.substr(0, eq), val = pv.str.substr(eq + 1);
            // Only values that name a member ("@i") are known to be links here.
            if (val.find('@') != std::string::npos) {
                std::vector<std::string> parts;
                size_t at = 0;
                while (at <= val.size()) {
                    size_t c = val.find(',', at);
                    if (c == std::string::npos) c = val.size();
                    std::string part = val.substr(at, c - at);
                    size_t a = part.find_first_not_of(' '), b = part.find_last_not_of(' ');
                    if (a != std::string::npos) parts.push_back(part.substr(a, b - a + 1));
                    at = c + 1;
                }
                m.links.push_back({key, parts});
            } else {
                m.props.push_back({key, q(val)});
            }
        }
    return m;
}

Member from_v2(const Value& o)
{
    Member m;
    m.type = str(o.find("type"));
    m.mesh = str(o.find("mesh"));
    m.name = str(o.find("name"));
    const std::vector<double> p = nums(o.find("origin"), 3, 0.0);
    for (int i = 0; i < 3; ++i) m.origin[i] = p[i];
    const std::vector<double> b = nums(o.find("basis"), 9, 0.0);
    if (o.find("basis")) for (int i = 0; i < 9; ++i) m.basis[i] = b[i];
    if (const Value* props = o.find("props"); props && props->is_obj())
        for (const auto& kv : props->obj) m.props.push_back({kv.first, jval(kv.second)});
    if (const Value* links = o.find("links"); links && links->is_obj())
        for (const auto& kv : links->obj) {
            std::vector<std::string> parts;
            if (kv.second.is_arr()) for (const Value& e : kv.second.arr) if (e.is_str()) parts.push_back(e.str);
            m.links.push_back({kv.first, parts});
        }
    if (const Value* pts = o.find("points"); pts && pts->is_arr())
        for (const Value& pt : pts->arr) {
            const std::vector<double> xyz = nums(&pt, 3, 0.0);
            m.points.insert(m.points.end(), xyz.begin(), xyz.end());
        }
    return m;
}

std::string member_json(const Member& m, const double shift[3], int index)
{
    std::string j = "{";
    if (index >= 0) j += "\"index\":" + std::to_string(index) + ",";
    j += "\"type\":" + q(m.type) + ",\"mesh\":" + q(m.mesh);
    if (!m.name.empty()) j += ",\"name\":" + q(m.name);
    j += ",\"origin\":[" + numtxt(m.origin[0] + shift[0]) + "," + numtxt(m.origin[1] + shift[1]) + "," + numtxt(m.origin[2] + shift[2]) + "]";
    j += ",\"basis\":[";
    for (int i = 0; i < 9; ++i) { if (i) j += ','; j += numtxt(m.basis[i]); }
    j += "],\"props\":{";
    for (size_t i = 0; i < m.props.size(); ++i) { if (i) j += ','; j += q(m.props[i].first) + ":" + m.props[i].second; }
    j += "},\"links\":{";
    for (size_t i = 0; i < m.links.size(); ++i) {
        if (i) j += ',';
        j += q(m.links[i].first) + ":[";
        for (size_t k = 0; k < m.links[i].second.size(); ++k) { if (k) j += ','; j += q(m.links[i].second[k]); }
        j += "]";
    }
    j += "}";
    if (!m.points.empty()) {
        j += ",\"points\":[";
        for (size_t i = 0; i + 2 < m.points.size(); i += 3) {
            if (i) j += ',';
            j += "[" + numtxt(m.points[i] + shift[0]) + "," + numtxt(m.points[i + 1] + shift[1]) + "," + numtxt(m.points[i + 2] + shift[2]) + "]";
        }
        j += "]";
    }
    return j + "}";
}

struct Block {
    std::string name, level, file, format;
    std::vector<Member> members;
};

bool load_block(const std::string& path, Block& b)
{
    std::string text;
    Value v;
    if (!bf6fs::read_all(path, text) || !parse(text, v)) return false;
    b.file = path;
    b.name = str(v.find("name"));
    b.level = str(v.find("level"));
    b.format = str(v.find("format"));
    const bool v2 = b.format == "bf6-block/2";
    if (!v2) b.format = "bf6-block/1";
    if (const Value* objs = v.find("objects"); objs && objs->is_arr())
        for (const Value& o : objs->arr)
            if (o.is_obj()) b.members.push_back(v2 ? from_v2(o) : from_v1(o));
    if (b.name.empty()) {
        std::string base = path.substr(path.find_last_of("/\\") + 1);
        b.name = base.size() > 5 ? base.substr(0, base.size() - 5) : base;
    }
    return true;
}

bool find_block(const std::vector<std::string>& dirs, const std::string& name, Block& b)
{
    const std::string safe = safe_name(name);
    for (const std::string& d : dirs)
        if (load_block(bf6fs::join(d, safe + ".json"), b)) return true;
    return false;
}

} // namespace

extern "C" int64_t bf6_block_library(uint8_t** out)
{
    if (!out) return -1;
#if defined(_WIN32)
    std::string base = bf6fs::env("LOCALAPPDATA");
    std::string dir = base.empty() ? std::string() : bf6fs::join(bf6fs::join(base, "BF6"), "blocks");
#else
    std::string base = bf6fs::env("XDG_DATA_HOME");
    if (base.empty()) { const std::string home = bf6fs::env("HOME"); base = home.empty() ? home : bf6fs::join(home, ".local/share"); }
    std::string dir = base.empty() ? std::string() : bf6fs::join(bf6fs::join(base, "bf6"), "blocks");
#endif
    return give("{\"dir\":" + q(dir) + "}", out);
}

extern "C" int64_t bf6_block_list(const char* request_json, size_t len, uint8_t** out)
{
    if (!out) return -1;
    *out = nullptr;
    Value req;
    if (!request_json || !parse(std::string(request_json, len ? len : std::strlen(request_json)), req)) return -1;
    std::set<std::string> seen;
    std::vector<Block> blocks;
    // Earlier folders win a name clash: the shared library first, then any
    // folder an editor kept before it.
    for (const std::string& d : list_dirs(req))
        for (const std::string& f : bf6fs::list(d, false, "", ".json")) {
            Block b;
            if (!load_block(bf6fs::join(d, f), b) || !seen.insert(safe_name(b.name)).second) continue;
            blocks.push_back(std::move(b));
        }
    std::sort(blocks.begin(), blocks.end(), [](const Block& a, const Block& b) { return a.name < b.name; });
    std::string j = "{\"blocks\":[";
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i) j += ',';
        j += "{\"name\":" + q(blocks[i].name) + ",\"level\":" + q(blocks[i].level) + ",\"count\":" + std::to_string(blocks[i].members.size())
           + ",\"file\":" + q(blocks[i].file) + ",\"format\":" + q(blocks[i].format) + "}";
    }
    return give(j + "]}", out);
}

extern "C" int64_t bf6_block_save(const char* request_json, size_t len, uint8_t** out)
{
    if (!out) return -1;
    *out = nullptr;
    Value req;
    if (!request_json || !parse(std::string(request_json, len ? len : std::strlen(request_json)), req)) return -1;
    const std::string dir = str(req.find("dir"));
    const std::string name = safe_name(str(req.find("name")));
    std::vector<Member> members;
    if (const Value* objs = req.find("objects"); objs && objs->is_arr())
        for (const Value& o : objs->arr) if (o.is_obj()) members.push_back(from_v2(o));
    if (dir.empty() || name.empty() || members.empty())
        return give("{\"error\":" + q(name.empty() ? "Name the block first." : members.empty() ? "Select what goes in the block." : "No block folder.") + "}", out);

    // The anchor: the centroid on the ground plane at the lowest point, so a
    // placed block lands on the surface it was dropped on.
    double anchor[3] = {0, 0, 0};
    double low = 1e300;
    // A zone stands where its polygon is, wherever its own pivot sits.
    for (const Member& m : members) {
        if (m.points.size() >= 3) {
            double cx = 0, cz = 0;
            const size_t n = m.points.size() / 3;
            for (size_t i = 0; i < n; ++i) { cx += m.points[i * 3]; cz += m.points[i * 3 + 2]; low = std::min(low, m.points[i * 3 + 1]); }
            anchor[0] += cx / (double)n; anchor[2] += cz / (double)n;
        } else {
            anchor[0] += m.origin[0]; anchor[2] += m.origin[2]; low = std::min(low, m.origin[1]);
        }
    }
    anchor[0] /= (double)members.size(); anchor[2] /= (double)members.size(); anchor[1] = low;

    // Links between members by index, so they survive a copy getting fresh names.
    std::map<std::string, int> index;
    for (size_t i = 0; i < members.size(); ++i) if (!members[i].name.empty()) index[members[i].name] = (int)i;
    for (Member& m : members)
        for (auto& l : m.links)
            for (std::string& ref : l.second) {
                auto it = index.find(ref);
                if (it != index.end()) ref = "@" + std::to_string(it->second);
            }

    const double shift[3] = {-anchor[0], -anchor[1], -anchor[2]};
    std::string j = "{\"format\":\"bf6-block/2\",\"name\":" + q(name) + ",\"level\":" + q(str(req.find("level"))) + ",\"objects\":[";
    for (size_t i = 0; i < members.size(); ++i) {
        if (i) j += ',';
        Member m = members[i];
        m.name.clear();   // a placed copy names itself
        j += member_json(m, shift, -1);
    }
    j += "]}";
    if (!bf6fs::make_dirs(dir)) return give("{\"error\":" + q("Could not create " + dir) + "}", out);
    const std::string file = bf6fs::join(dir, name + ".json");
    const std::string staging = file + ".writing";
    if (!bf6fs::write_all(staging, j) || !bf6fs::move_replace(staging, file)) {
        bf6fs::remove_file(staging);
        return give("{\"error\":" + q("Could not write " + file) + "}", out);
    }
    return give("{\"name\":" + q(name) + ",\"file\":" + q(file) + ",\"count\":" + std::to_string(members.size())
              + ",\"anchor\":[" + numtxt(anchor[0]) + "," + numtxt(anchor[1]) + "," + numtxt(anchor[2]) + "]}", out);
}

extern "C" int64_t bf6_block_load(const char* request_json, size_t len, uint8_t** out)
{
    if (!out) return -1;
    *out = nullptr;
    Value req;
    if (!request_json || !parse(std::string(request_json, len ? len : std::strlen(request_json)), req)) return -1;
    Block b;
    if (!find_block(list_dirs(req), str(req.find("name")), b))
        return give("{\"error\":" + q("Block '" + str(req.find("name")) + "' could not be read.") + "}", out);
    const std::vector<double> at = nums(req.find("at"), 3, 0.0);
    const double shift[3] = {at[0], at[1], at[2]};
    std::string j = "{\"name\":" + q(b.name) + ",\"level\":" + q(b.level) + ",\"format\":" + q(b.format) + ",\"file\":" + q(b.file) + ",\"objects\":[";
    for (size_t i = 0; i < b.members.size(); ++i) {
        if (i) j += ',';
        j += member_json(b.members[i], shift, (int)i);
    }
    return give(j + "]}", out);
}

extern "C" int64_t bf6_block_delete(const char* request_json, size_t len, uint8_t** out)
{
    if (!out) return -1;
    *out = nullptr;
    Value req;
    if (!request_json || !parse(std::string(request_json, len ? len : std::strlen(request_json)), req)) return -1;
    Block b;
    const bool found = find_block(list_dirs(req), str(req.find("name")), b);
    if (found) bf6fs::remove_file(b.file);
    return give(std::string("{\"deleted\":") + (found ? "true" : "false") + ",\"file\":" + q(found ? b.file : std::string()) + "}", out);
}
