/* The PORTAL BUDGET meter both SDK editors show in their top row: physics cost
 * against the level's cap, and the upload size against the site's per-map
 * limit, as one fraction, one colour and one line of text. The costs come from
 * the SDK's own asset_types.json and the cap from level_info.json.
 * See bf6_budget in bf6_core.h. */
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
#include <string>

namespace {

using bf6json::Value;

std::string str(const Value* v) { return v && v->is_str() ? v->str : std::string(); }
double num(const Value* v, double d) { return v && v->type == Value::Num ? v->num : d; }

// asset_types.json is large; it is parsed once per file version.
struct CostCache {
    std::string path;
    int64_t size = -1, mtime = -1;
    std::map<std::string, int> cost;
};
std::mutex g_mutex;
CostCache g_costs;

void load_costs(const std::string& path)
{
    int64_t size = 0, mtime = 0;
    if (!bf6fs::stat_file(path, size, mtime)) { g_costs = CostCache{}; return; }
    if (g_costs.path == path && g_costs.size == size && g_costs.mtime == mtime) return;
    CostCache c;
    c.path = path; c.size = size; c.mtime = mtime;
    std::string text;
    if (bf6fs::read_all(path, text)) {
        Value v;
        std::string err;
        bf6json::Parser p(text.c_str(), text.size());
        if (p.parse(v, err) && v.is_obj())
            if (const Value* rows = v.find("AssetTypes"); rows && rows->is_arr())
                for (const Value& row : rows->arr) {
                    const std::string type = str(row.find("type"));
                    if (type.empty()) continue;
                    int cost = 0;
                    if (const Value* k = row.find("constants"); k && k->is_arr())
                        for (const Value& con : k->arr)
                            if (str(con.find("name")) == "physicsCost") cost = (int)num(con.find("value"), 0);
                    c.cost[type] = cost;
                }
    }
    g_costs = std::move(c);
}

int level_cap(const std::string& path, const std::string& level)
{
    std::string text;
    if (level.empty() || !bf6fs::read_all(path, text)) return -1;
    Value v;
    std::string err;
    bf6json::Parser p(text.c_str(), text.size());
    if (!p.parse(v, err) || !v.is_obj()) return -1;
    const Value* lv = v.find(level.c_str());
    const Value* bud = lv ? lv->find("budget") : nullptr;
    return bud ? (int)num(bud->find("physicsCostMax"), -1) : -1;
}

// 12,345 - the thousands separator the Unreal tool's FText::AsNumber prints.
std::string grouped(int64_t n)
{
    std::string digits = std::to_string(n < 0 ? -n : n), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out.push_back(',');
        out.push_back(digits[i]);
    }
    return (n < 0 ? "-" : "") + out;
}

double to_linear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double to_srgb(double c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055; }

struct Rgb { double r, g, b; };
Rgb lin(int r, int g, int b) { return {to_linear(r / 255.0), to_linear(g / 255.0), to_linear(b / 255.0)}; }

// The site's own colours: blue, warning yellow, danger red. Mostly blue,
// ramping to red near the end, blended in linear light as the Unreal tool does.
Rgb fill(double frac)
{
    const Rgb low = lin(0x59, 0xBF, 0xF8), mid = lin(0xF2, 0xC5, 0x73), high = lin(0xFB, 0x69, 0x4D);
    frac = std::min(1.0, std::max(0.0, frac));
    auto mix = [](const Rgb& a, const Rgb& b, double t) { return Rgb{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; };
    if (frac < 0.65) return mix(low, mid, std::min(1.0, frac / 0.65) * 0.5);
    return mix(mid, high, std::min(1.0, (frac - 0.65) / 0.35));
}

} // namespace

extern "C" int64_t bf6_budget(const char* request_json, size_t len, uint8_t** out)
{
    if (!out) return -1;
    *out = nullptr;
    if (!request_json) return -1;
    Value req;
    std::string err;
    bf6json::Parser parser(request_json, len ? len : std::strlen(request_json));
    if (!parser.parse(req, err) || !req.is_obj()) return -1;

    int64_t cost = 0, objects = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        load_costs(str(req.find("asset_types")));
        if (const Value* counts = req.find("counts"); counts && counts->is_obj())
            for (const auto& kv : counts->obj) {
                const int64_t n = (int64_t)num(&kv.second, 0);
                objects += n;
                auto it = g_costs.cost.find(kv.first);
                if (it != g_costs.cost.end()) cost += (int64_t)it->second * n;
            }
    }
    const int cap = level_cap(str(req.find("level_info")), str(req.find("level")));
    const bool has_cap = cap > 0;
    const double phys = has_cap ? std::min(1.0, std::max(0.0, (double)cost / cap)) : 0.0;

    const int64_t bytes = (int64_t)num(req.find("upload_bytes"), -1);
    const int64_t raw = (int64_t)num(req.find("upload_raw_bytes"), bytes);
    // 3 MiB per map and 4 MiB for the experience's base64 bundle, until the
    // engine passes the limits it last fetched.
    const int64_t per_map = (int64_t)num(req.find("limit_per_map"), 3145728);
    const int64_t per_exp = (int64_t)num(req.find("limit_experience"), 4194304);
    const double size = bytes > 0 && per_map > 0 ? std::min(1.0, std::max(0.0, (double)bytes / (double)per_map)) : 0.0;
    const double frac = std::max(phys, size);
    const Rgb c = (has_cap || bytes > 0) ? fill(frac) : lin(0x59, 0xBF, 0xF8);

    std::string size_part;
    if (bytes > 0) {
        // The map's bytes base64-encode to about 4/3 in the upload bundle.
        const int64_t b64 = (bytes * 4 + 2) / 3;
        char b[256];
        std::snprintf(b, sizeof(b), "     upload %lld KB min / %lld KB raw   of %lld KB per map   |   uses %lld of %lld KB experience total",
                      (long long)(bytes / 1024), (long long)(raw / 1024), (long long)(per_map / 1024), (long long)(b64 / 1024), (long long)(per_exp / 1024));
        size_part = b;
    }
    const std::string note = str(req.find("upload_note"));
    if (!size_part.empty() && !note.empty()) size_part += "   (" + note + ")";
    std::string text;
    if (has_cap)
        text = "physics " + grouped(cost) + " / " + grouped(cap) + "   (" + std::to_string((int)std::lround(phys * 100.0)) + "%)   "
             + std::to_string(objects) + " obj" + size_part;
    else
        text = "physics " + grouped(cost) + " (no cap)   " + std::to_string(objects) + " obj" + size_part;

    char head[512];
    std::snprintf(head, sizeof(head),
        "{\"objects\":%lld,\"cost\":%lld,\"max\":%d,\"physics_frac\":%.6f,\"size_frac\":%.6f,\"frac\":%.6f,"
        "\"over\":%s,\"color\":[%.6f,%.6f,%.6f],\"text\":",
        (long long)objects, (long long)cost, cap, phys, size, frac,
        (has_cap && cost > cap) || (bytes > per_map && per_map > 0) ? "true" : "false",
        to_srgb(c.r), to_srgb(c.g), to_srgb(c.b));
    std::string j = head;
    j.push_back('"');
    for (unsigned char ch : text) {
        if (ch == '"' || ch == '\\') { j.push_back('\\'); j.push_back((char)ch); }
        else if (ch < 0x20) { char e[8]; std::snprintf(e, sizeof(e), "\\u%04x", ch); j += e; }
        else j.push_back((char)ch);
    }
    j += "\"}";
    *out = (uint8_t*)std::malloc(j.size() + 1);
    if (!*out) return -1;
    std::memcpy(*out, j.data(), j.size());
    (*out)[j.size()] = 0;
    return (int64_t)j.size();
}
