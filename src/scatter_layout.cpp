/* SCATTER's layout, for every engine: where each copy lands inside a circle,
 * square, ring, drawn outline or painted area, how far apart copies stay, and the
 * rotation, lean, height and size each one rolls. The engine answers one
 * question - what is under this point - through a callback, and spawns what
 * comes back. See bf6_scatter_layout in bf6_core.h. */
#include "bf6_core.h"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace {

using bf6json::Value;

constexpr double PI = 3.14159265358979323846;

double getn(const Value* v, double d) { return v && v->type == Value::Num ? v->num : d; }
bool getb(const Value* v, bool d)
{
    if (!v) return d;
    if (v->type == Value::Bool) return v->b;
    if (v->type == Value::Num) return v->num != 0.0;
    return d;
}

// A small, fixed generator so a seed lays out the same pattern everywhere.
struct Rng {
    uint64_t s;
    explicit Rng(int64_t seed) : s((uint64_t)seed * 0x9E3779B97F4A7C15ull + 0xD1B54A32D192ED03ull) { next(); }
    uint64_t next()
    {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double frand() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }
    int range(int lo, int hi) { return hi <= lo ? lo : lo + (int)std::min<int64_t>(hi - lo, (int64_t)(frand() * (hi - lo + 1))); }
};

struct P3 { double x, y, z; };

std::string num(double v)
{
    if (std::fabs(v) < 1e-9) v = 0.0;
    char b[40];
    std::snprintf(b, sizeof(b), "%.6g", v);
    return b;
}

std::vector<P3> points(const Value* arr)
{
    std::vector<P3> out;
    if (arr && arr->is_arr())
        for (const Value& p : arr->arr)
            if (p.is_arr() && p.arr.size() >= 3)
                out.push_back({getn(&p.arr[0], 0), getn(&p.arr[1], 0), getn(&p.arr[2], 0)});
    return out;
}

struct Stroke { double radius; std::vector<P3> stamps; };

// THE PAINTED AREA, as grid cells: one definition for drawing the area and for
// filling it, so copies never land outside the region shown. Returns the cell
// size, 0 when nothing is painted.
double coverage(const std::vector<Stroke>& strokes, std::vector<std::pair<int, int>>& cells)
{
    cells.clear();
    double max_r = 0.0;
    for (const Stroke& s : strokes) max_r = std::max(max_r, s.radius);
    if (max_r <= 0.0) return 0.0;
    const double cell = std::min(9.0, std::max(1.2, max_r / 5.0));
    std::set<int64_t> seen;
    for (const Stroke& s : strokes) {
        const double r = std::max(s.radius, 0.5);
        const int span = std::max(1, std::min(64, (int)std::ceil(r / cell)));
        for (const P3& p : s.stamps) {
            const int cx = (int)std::floor(p.x / cell), cz = (int)std::floor(p.z / cell);
            for (int dz = -span; dz <= span; ++dz)
                for (int dx = -span; dx <= span; ++dx) {
                    // The cell's centre against the brush, so the edge follows the circle.
                    const double px = (cx + dx + 0.5) * cell, pz = (cz + dz + 0.5) * cell;
                    if ((px - p.x) * (px - p.x) + (pz - p.z) * (pz - p.z) > r * r) continue;
                    const int64_t key = ((int64_t)(cx + dx) << 32) ^ (int64_t)(uint32_t)(cz + dz);
                    if (!seen.insert(key).second) continue;
                    cells.push_back({cx + dx, cz + dz});
                }
        }
    }
    return cell;
}

} // namespace

extern "C" int64_t bf6_scatter_layout(const char* request_json, size_t len, bf6_scatter_ground_fn ground,
                                      void* user, uint8_t** out)
{
    if (!out) return -1;
    *out = nullptr;
    if (!request_json) return -1;
    Value req;
    std::string err;
    bf6json::Parser parser(request_json, len ? len : std::strlen(request_json));
    if (!parser.parse(req, err) || !req.is_obj()) return -1;

    const int shape = (int)getn(req.find("shape"), 0);
    P3 c = {0, 0, 0};
    if (const Value* a = req.find("center"); a && a->is_arr() && a->arr.size() >= 3)
        c = {getn(&a->arr[0], 0), getn(&a->arr[1], 0), getn(&a->arr[2], 0)};
    // 1000 is the ceiling: every copy is a real object in the editor.
    const int count = std::max(1, std::min(1000, (int)getn(req.find("count"), 24)));
    const double R = std::max(getn(req.find("radius"), 20.0), 1.0);
    const double rot = getn(req.find("rotation"), 360.0);
    const double wob_x = getn(req.find("wobble_x"), 0.0), wob_y = getn(req.find("wobble_y"), 0.0);
    const double elev = getn(req.find("elevation"), 0.0);
    const double vary = getn(req.find("vary"), 0.15);
    const int64_t seed = (int64_t)getn(req.find("seed"), 1);
    const bool follow = getb(req.find("follow_terrain"), true);
    const bool terrain_only = getb(req.find("terrain_only"), false);
    const int pool = std::max(0, (int)getn(req.find("pool"), 0));
    const double unit_w = std::max(0.0, getn(req.find("unit_width"), 1.0));
    const bool keep_center = getb(req.find("keep_center"), pool == 0);
    const std::vector<P3> poly = points(req.find("poly"));
    std::vector<Stroke> strokes;
    if (const Value* st = req.find("strokes"); st && st->is_arr())
        for (const Value& s : st->arr)
            if (s.is_obj()) strokes.push_back({getn(s.find("radius"), R), points(s.find("stamps"))});

    const bool drawn = shape == 3 && poly.size() >= 3;
    double min_x = std::numeric_limits<double>::max(), max_x = -min_x, min_z = min_x, max_z = -min_x;
    double area = PI * R * R;
    if (shape == 1) area = 4.0 * R * R;
    else if (shape == 2) area = PI * R * R * (1.0 - 0.36);
    else if (drawn) {
        double a2 = 0.0;
        for (size_t i = 0; i < poly.size(); ++i) {
            const P3& a = poly[i];
            const P3& b = poly[(i + 1) % poly.size()];
            a2 += a.x * b.z - b.x * a.z;
            min_x = std::min(min_x, a.x); max_x = std::max(max_x, a.x);
            min_z = std::min(min_z, a.z); max_z = std::max(max_z, a.z);
        }
        area = std::max(std::fabs(a2) * 0.5, 1.0);
    }
    std::vector<std::pair<int, int>> cells;
    double cell = 0.0;
    if (shape == 4) {
        cell = coverage(strokes, cells);
        area = std::max((double)cells.size() * cell * cell, 1.0);
    }

    std::string j = "{\"targets\":[";
    int made = 0;
    const Value* inc = shape == 4 ? req.find("incremental") : nullptr;
    const bool can_lay = !(shape == 4 && cells.empty()) && !(shape == 3 && !drawn);
    if (inc && inc->is_obj() && can_lay) {
        // WHILE A STROKE RUNS: fill only the cells it just gained, at the density
        // the whole painting carries now. Rebuilding every copy ten times a second
        // is what made painting crawl; the authoritative full fill still runs
        // when the stroke ends.
        std::set<int64_t> filled;
        if (const Value* f = inc->find("filled"); f && f->is_arr())
            for (const Value& e : f->arr)
                if (e.is_arr() && e.arr.size() >= 2)
                    filled.insert(((int64_t)(int)getn(&e.arr[0], 0) << 32) ^ (int64_t)(uint32_t)(int)getn(&e.arr[1], 0));
        std::vector<P3> near = points(inc->find("existing"));
        std::vector<std::pair<int, int>> fresh;
        for (const auto& cp : cells) {
            const int64_t key = ((int64_t)cp.first << 32) ^ (int64_t)(uint32_t)cp.second;
            if (!filled.count(key)) fresh.push_back(cp);
        }
        const double density = (double)count / std::max(area, 1.0);
        int want = (int)std::lround(density * (double)fresh.size() * cell * cell);
        want = std::min(want, 400);   // one stamp is never a whole map
        if (!fresh.empty() && want > 0) {
            const double fit = std::sqrt(1.0 / std::max(density, 1e-12) / 2.6);
            const double min_dist = std::min(std::max(std::min(unit_w * 0.7, fit), 0.4), 40.0);
            Rng rng(seed * 7919 + (int64_t)near.size() * 104729 + (int64_t)fresh.size());
            for (int s = 0; s < want * 20 && made < want; ++s) {
                const auto& cp = fresh[(size_t)rng.range(0, (int)fresh.size() - 1)];
                P3 p = {(cp.first + rng.frand()) * cell, 0.0, (cp.second + rng.frand()) * cell};
                bool close = false;
                for (const P3& q : near)
                    if ((p.x - q.x) * (p.x - q.x) + (p.z - q.z) * (p.z - q.z) < min_dist * min_dist) { close = true; break; }
                if (close) continue;
                double y = 0.0;
                // Nothing under it is a skip either way: there is no drawn height to fall back on.
                if (!ground || !ground(user, p.x, p.z, 500.0, terrain_only ? 1 : 0, &y)) continue;
                p.y = y;
                const double yaw = (rng.frand() - 0.5) * rot;
                const double tx = (rng.frand() - 0.5) * wob_x;
                const double ty = (rng.frand() - 0.5) * wob_y;
                const double scale = 1.0 + (rng.frand() * 2.0 - 1.0) * vary;
                p.y += (rng.frand() * 2.0 - 1.0) * elev;
                const int pick = pool > 0 ? rng.range(0, pool - 1) : -1;
                near.push_back(p);
                if (made) j += ',';
                j += "{\"at\":[" + num(p.x) + "," + num(p.y) + "," + num(p.z) + "],\"yaw\":" + num(yaw)
                   + ",\"tilt_x\":" + num(tx) + ",\"tilt_y\":" + num(ty) + ",\"scale\":" + num(scale)
                   + ",\"pool\":" + std::to_string(pick) + "}";
                ++made;
            }
        }
    } else if (can_lay) {
        const double fit = std::sqrt(area / ((double)count * 2.6));
        const double min_dist = std::min(std::max(std::min(unit_w * 0.7, fit), 0.5), std::max(R, 1.0));
        std::vector<P3> placed;
        // The centre is occupied only when the original is standing there.
        if (keep_center) placed.push_back(c);
        Rng rng(seed);
        for (int s = 0; s < count * 40 && made < count; ++s) {
            // The same draws for every sample, so the pattern holds while sliders move.
            const double u = rng.frand(), v = rng.frand();
            const double yaw = (rng.frand() - 0.5) * rot;
            const double tx = (rng.frand() - 0.5) * wob_x;
            const double ty = (rng.frand() - 0.5) * wob_y;
            const double scale = 1.0 + (rng.frand() * 2.0 - 1.0) * vary;
            const double lift = (rng.frand() * 2.0 - 1.0) * elev;
            P3 p = c;
            if (shape == 4) {
                // Uniform over the painted area, not over the stamps: density
                // follows what was painted, not how slowly the brush moved.
                const auto& cp = cells[(size_t)rng.range(0, (int)cells.size() - 1)];
                p.x = (cp.first + rng.frand()) * cell;
                p.z = (cp.second + rng.frand()) * cell;
                double best = std::numeric_limits<double>::max();
                for (const Stroke& st : strokes) {
                    const size_t step = std::max<size_t>(1, st.stamps.size() / 32);
                    for (size_t i = 0; i < st.stamps.size(); i += step) {
                        const double dd = (st.stamps[i].x - p.x) * (st.stamps[i].x - p.x) + (st.stamps[i].z - p.z) * (st.stamps[i].z - p.z);
                        if (dd < best) { best = dd; p.y = st.stamps[i].y; }
                    }
                }
            } else if (drawn) {
                p.x = min_x + (max_x - min_x) * u;
                p.z = min_z + (max_z - min_z) * v;
                bool in = false;
                for (size_t i = 0, k = poly.size() - 1; i < poly.size(); k = i++) {
                    const P3& a = poly[i];
                    const P3& b = poly[k];
                    if ((a.z > p.z) != (b.z > p.z) && p.x < (b.x - a.x) * (p.z - a.z) / (b.z - a.z) + a.x) in = !in;
                }
                if (!in) continue;
                // The outline was drawn on the surface; its height starts the ground search.
                double best = std::numeric_limits<double>::max();
                const size_t step = std::max<size_t>(1, poly.size() / 64);
                for (size_t i = 0; i < poly.size(); i += step) {
                    const double dd = (poly[i].x - p.x) * (poly[i].x - p.x) + (poly[i].z - p.z) * (poly[i].z - p.z);
                    if (dd < best) { best = dd; p.y = poly[i].y; }
                }
            } else if (shape == 1) {
                p.x += (u * 2.0 - 1.0) * R;
                p.z += (v * 2.0 - 1.0) * R;
            } else {
                const double rr = shape == 2 ? R * std::sqrt(0.36 + (1.0 - 0.36) * u) : R * std::sqrt(u);
                const double th = v * 2.0 * PI;
                p.x += std::cos(th) * rr;
                p.z += std::sin(th) * rr;
            }
            bool close = false;
            for (const P3& q : placed)
                if ((p.x - q.x) * (p.x - q.x) + (p.z - q.z) * (p.z - q.z) < min_dist * min_dist) { close = true; break; }
            if (close) continue;
            if (terrain_only || follow) {
                double y = p.y;
                const int hit = ground ? ground(user, p.x, p.z, p.y, terrain_only ? 1 : 0, &y) : 0;
                // Ground only: a miss is a skip, never a fallback onto a roof.
                if (terrain_only && !hit) continue;
                if (hit) p.y = y;
            } else {
                p.y = c.y;
            }
            p.y += lift;
            const int pick = pool > 0 ? rng.range(0, pool - 1) : -1;
            placed.push_back(p);
            if (made) j += ',';
            j += "{\"at\":[" + num(p.x) + "," + num(p.y) + "," + num(p.z) + "],\"yaw\":" + num(yaw)
               + ",\"tilt_x\":" + num(tx) + ",\"tilt_y\":" + num(ty) + ",\"scale\":" + num(scale)
               + ",\"pool\":" + std::to_string(pick) + "}";
            ++made;
        }
    }
    j += "]";
    if (shape == 4) {
        j += ",\"cell\":" + num(cell) + ",\"cells\":[";
        for (size_t i = 0; i < cells.size(); ++i) {
            if (i) j += ',';
            j += "[" + std::to_string(cells[i].first) + "," + std::to_string(cells[i].second) + "]";
        }
        j += "]";
    }
    j += "}";
    *out = (uint8_t*)std::malloc(j.size() + 1);
    if (!*out) return -1;
    std::memcpy(*out, j.data(), j.size());
    (*out)[j.size()] = 0;
    return (int64_t)j.size();
}
