/* The terrain mesh both engines draw: the Unreal plugin's error-bounded
 * adaptive grid (BuildTerrain), moved here so the Godot plugin builds the
 * identical surface. See bf6_terrain_mesh_build in bf6_core.h. */
#include "bf6_core.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

struct bf6_terrain_tile_data {
    std::vector<float> positions, normals, uvs;
    std::vector<uint32_t> indices;
};

struct bf6_terrain_mesh {
    const uint16_t* heights = nullptr;
    int32_t size = 0;
    double wmin[3] = {0, 0, 0}, wmax[3] = {0, 0, 0};
    double yscale = 0.0;
    int32_t side = 0, step = 1, per = 0, tiles = 0, base_cells = 0, max_level = 0;
    bool adaptive = false, flip = false;
    double native_spacing = 0.0;
    std::vector<uint8_t> levels;
    std::vector<std::unique_ptr<bf6_terrain_tile_data>> tile_data;
};

namespace {

int hardware_threads()
{
    const unsigned n = std::thread::hardware_concurrency();
    return (int)std::clamp<unsigned>(n ? n : 4, 1, 64);
}

template <class F>
void parallel_for(int count, F&& fn)
{
    const int workers = std::min(hardware_threads(), std::max(count, 1));
    std::atomic<int> next{0};
    auto run = [&]() {
        for (int i = next++; i < count; i = next++) fn(i);
    };
    std::vector<std::thread> pool;
    for (int w = 1; w < workers; ++w) pool.emplace_back(run);
    run();
    for (auto& t : pool) t.join();
}

inline double H(const bf6_terrain_mesh& m, int x, int z)
{
    return (double)m.heights[(size_t)z * (size_t)m.size + (size_t)x];
}

double max_plane_error(const bf6_terrain_mesh& m, int sx0, int sz0, int span)
{
    const double h00 = H(m, sx0, sz0), h10 = H(m, sx0 + span, sz0);
    const double h01 = H(m, sx0, sz0 + span), h11 = H(m, sx0 + span, sz0 + span);
    double worst = 0.0;
    const double inv = 1.0 / span;
    for (int dz = 0; dz <= span; ++dz) {
        const double fz = dz * inv;
        for (int dx = 0; dx <= span; ++dx) {
            const double fx = dx * inv;
            const double top = h00 + (h10 - h00) * fx;
            const double bottom = h01 + (h11 - h01) * fx;
            const double plane = top + (bottom - top) * fz;
            worst = std::max(worst, std::fabs(H(m, sx0 + dx, sz0 + dz) - plane) * m.yscale);
        }
    }
    return worst;
}

/* Metres: 0.50 above 2 m cells, 0.25 above 1 m, else 0.10. */
int required_step(const bf6_terrain_mesh& m, int sx0, int sz0, int span)
{
    if (span <= 1) return 1;
    const double cell_m = m.native_spacing * span;
    const double max_error = cell_m > 2.01 ? 0.50 : (cell_m > 1.01 ? 0.25 : 0.10);
    if (max_plane_error(m, sx0, sz0, span) <= max_error) return span;
    const int half = span / 2;
    int finest = half;
    finest = std::min(finest, required_step(m, sx0, sz0, half));
    finest = std::min(finest, required_step(m, sx0 + half, sz0, half));
    finest = std::min(finest, required_step(m, sx0, sz0 + half, half));
    finest = std::min(finest, required_step(m, sx0 + half, sz0 + half, half));
    return finest;
}

void vertex_normal(const bf6_terrain_mesh& m, int sx, int sz, float* out)
{
    const int xl = std::max(sx - 1, 0), xr = std::min(sx + 1, m.size - 1);
    const int zl = std::max(sz - 1, 0), zr = std::min(sz + 1, m.size - 1);
    const double dx = (m.wmax[0] - m.wmin[0]) / (m.size - 1);
    const double dz = (m.wmax[2] - m.wmin[2]) / (m.size - 1);
    const double gx = (H(m, xr, sz) - H(m, xl, sz)) * m.yscale / std::max(dx * (xr - xl), 1e-9);
    const double gz = (H(m, sx, zr) - H(m, sx, zl)) * m.yscale / std::max(dz * (zr - zl), 1e-9);
    const double len = std::sqrt(gx * gx + 1.0 + gz * gz);
    out[0] = (float)(-gx / len);
    out[1] = (float)(1.0 / len);
    out[2] = (float)(-gz / len);
}

} // namespace

extern "C" bf6_terrain_mesh* bf6_terrain_mesh_build(const uint16_t* heights, int32_t size,
                                                    const double* world_min, const double* world_max,
                                                    float height_scale,
                                                    const bf6_terrain_mesh_options* options)
{
    if (!heights || size <= 1 || !world_min || !world_max) return nullptr;
    auto m = std::make_unique<bf6_terrain_mesh>();
    m->heights = heights;
    m->size = size;
    for (int i = 0; i < 3; ++i) { m->wmin[i] = world_min[i]; m->wmax[i] = world_max[i]; }
    const int32_t side_opt = options && options->base_side > 1 ? options->base_side : 2049;
    const int32_t tile_opt = options && options->tile_quads > 0 ? options->tile_quads : 128;
    m->flip = options && options->flip_winding;

    const int N = std::min(side_opt, size);
    m->side = N;
    m->step = std::max(1, (size - 1) / (N - 1));
    m->yscale = height_scale > 0.f ? (double)height_scale / 65536.0
                                   : std::max(0.001, m->wmax[1] - m->wmin[1]) / 65535.0;
    const double xspan = m->wmax[0] - m->wmin[0], zspan = m->wmax[2] - m->wmin[2];
    m->native_spacing = std::max(xspan, zspan) / (size - 1);
    m->per = std::clamp(tile_opt, 16, N - 1);
    m->tiles = (N - 1 + m->per - 1) / m->per;
    m->base_cells = N - 1;
    const int step = m->step;
    m->adaptive = step > 1 && (step & (step - 1)) == 0 && m->base_cells * step == size - 1;
    for (int s = step; s > 1; s >>= 1) m->max_level++;

    m->levels.assign((size_t)m->base_cells * (size_t)m->base_cells, 0);
    if (m->adaptive) {
        bf6_terrain_mesh& mr = *m;
        parallel_for(mr.base_cells, [&](int bz) {
            for (int bx = 0; bx < mr.base_cells; ++bx) {
                const int fine = required_step(mr, bx * step, bz * step, step);
                int level = 0;
                for (int s = step; s > fine; s >>= 1) level++;
                mr.levels[(size_t)bz * mr.base_cells + bx] = (uint8_t)std::clamp(level, 0, mr.max_level);
            }
        });
    }
    m->tile_data.resize((size_t)m->tiles * (size_t)m->tiles);
    return m.release();
}

extern "C" int bf6_terrain_mesh_describe(const bf6_terrain_mesh* m, bf6_terrain_mesh_info* out)
{
    if (!m || !out) return 0;
    *out = bf6_terrain_mesh_info{};
    out->native_size = m->size;
    out->base_side = m->side;
    out->native_step = m->step;
    out->tile_quads = m->per;
    out->tiles_per_side = m->tiles;
    out->base_cells = m->base_cells;
    out->max_level = m->max_level;
    out->adaptive = m->adaptive ? 1 : 0;
    out->native_spacing_m = m->native_spacing;
    out->base_spacing_m = m->native_spacing * m->step;
    out->finest_spacing_m = m->native_spacing * (m->step >> m->max_level);
    out->height_per_unit = m->yscale;
    for (uint8_t level : m->levels) out->cells_by_level[std::min<int>(level, 7)]++;
    return 1;
}

extern "C" int bf6_terrain_mesh_tile(bf6_terrain_mesh* m, int32_t index, bf6_terrain_tile* out)
{
    if (!m || !out || index < 0 || index >= (int32_t)m->tile_data.size()) return 0;
    *out = bf6_terrain_tile{};
    const int tx = index % m->tiles, tz = index / m->tiles;
    out->tile_x = tx;
    out->tile_z = tz;
    const int N = m->side, per = m->per, step = m->step, size = m->size;
    const int x0 = tx * per, x1 = std::min(x0 + per, N - 1);
    const int z0 = tz * per, z1 = std::min(z0 + per, N - 1);
    if (x1 <= x0 || z1 <= z0) return 0;

    auto& slot = m->tile_data[(size_t)index];
    if (!slot) {
        auto d = std::make_unique<bf6_terrain_tile_data>();
        std::unordered_map<uint64_t, uint32_t> by_native;
        by_native.reserve((size_t)(x1 - x0 + 1) * (size_t)(z1 - z0 + 1));
        const double xlo = m->wmin[0], xspan = m->wmax[0] - m->wmin[0];
        const double zlo = m->wmin[2], zspan = m->wmax[2] - m->wmin[2];
        auto vertex = [&](int sx, int sz) -> uint32_t {
            const uint64_t key = (uint64_t)(uint32_t)sx | ((uint64_t)(uint32_t)sz << 32);
            auto it = by_native.find(key);
            if (it != by_native.end()) return it->second;
            const uint32_t id = (uint32_t)(d->positions.size() / 3);
            d->positions.push_back((float)(xlo + xspan * ((double)sx / (size - 1))));
            d->positions.push_back((float)(H(*m, sx, sz) * m->yscale));
            d->positions.push_back((float)(zlo + zspan * ((double)sz / (size - 1))));
            float n[3];
            vertex_normal(*m, sx, sz, n);
            d->normals.insert(d->normals.end(), n, n + 3);
            d->uvs.push_back((float)sx / (size - 1));
            d->uvs.push_back((float)sz / (size - 1));
            by_native.emplace(key, id);
            return id;
        };
        auto subdiv_at = [&](int bx, int bz) {
            if (bx < 0 || bz < 0 || bx >= m->base_cells || bz >= m->base_cells) return 1;
            return 1 << m->levels[(size_t)bz * m->base_cells + bx];
        };
        struct P { int x, z; };
        std::vector<P> ring;
        ring.reserve(36);
        for (int bz = z0; bz < z1; ++bz) {
            for (int bx = x0; bx < x1; ++bx) {
                const int subdiv = subdiv_at(bx, bz);
                const int cell = step / subdiv;
                const int left = std::max(subdiv, subdiv_at(bx - 1, bz));
                const int right = std::max(subdiv, subdiv_at(bx + 1, bz));
                const int top = std::max(subdiv, subdiv_at(bx, bz - 1));
                const int bottom = std::max(subdiv, subdiv_at(bx, bz + 1));
                const int nx = bx * step, nz = bz * step;
                for (int lz = 0; lz < step; lz += cell) {
                    for (int lx = 0; lx < step; lx += cell) {
                        const int lx1 = lx + cell, lz1 = lz + cell;
                        ring.clear();
                        ring.push_back({lx, lz});
                        if (lx == 0)
                            for (int q = lz + step / left; q < lz1; q += step / left) ring.push_back({lx, q});
                        ring.push_back({lx, lz1});
                        if (lz1 == step)
                            for (int q = lx + step / bottom; q < lx1; q += step / bottom) ring.push_back({q, lz1});
                        ring.push_back({lx1, lz1});
                        if (lx1 == step)
                            for (int q = lz1 - step / right; q > lz; q -= step / right) ring.push_back({lx1, q});
                        ring.push_back({lx1, lz});
                        if (lz == 0)
                            for (int q = lx1 - step / top; q > lx; q -= step / top) ring.push_back({q, lz});
                        /* Fan the ring. The added edge points are exactly the
                         * points the finer neighbour asks for, so both sides
                         * describe the same boundary with no T-junction. */
                        for (size_t i = 1; i + 1 < ring.size(); ++i) {
                            const uint32_t a = vertex(nx + ring[0].x, nz + ring[0].z);
                            const uint32_t b = vertex(nx + ring[i].x, nz + ring[i].z);
                            const uint32_t c = vertex(nx + ring[i + 1].x, nz + ring[i + 1].z);
                            d->indices.push_back(a);
                            d->indices.push_back(m->flip ? c : b);
                            d->indices.push_back(m->flip ? b : c);
                        }
                    }
                }
            }
        }
        slot = std::move(d);
    }
    out->vertex_count = (int32_t)(slot->positions.size() / 3);
    out->index_count = (int32_t)slot->indices.size();
    out->positions = slot->positions.data();
    out->normals = slot->normals.data();
    out->uvs = slot->uvs.data();
    out->indices = slot->indices.data();
    return out->index_count > 0 ? 1 : 0;
}

extern "C" void bf6_terrain_mesh_release_tile(bf6_terrain_mesh* m, int32_t index)
{
    if (!m || index < 0 || index >= (int32_t)m->tile_data.size()) return;
    m->tile_data[(size_t)index].reset();
}

/* The height of the drawn surface, as the Unreal plugin's RenderedGroundAt:
 * the containing cell at its own subdivision, split on the (0,0)-(1,1)
 * diagonal. */
extern "C" double bf6_terrain_mesh_height_at(const bf6_terrain_mesh* m, double x, double z)
{
    if (!m || m->size <= 1 || m->base_cells <= 0 || m->step <= 0) return 0.0;
    const double sx = m->wmax[0] - m->wmin[0], sz = m->wmax[2] - m->wmin[2];
    if (sx <= 0.0 || sz <= 0.0) return 0.0;
    const int size = m->size, step = m->step;
    const double gx = std::clamp((x - m->wmin[0]) / sx, 0.0, 1.0) * (size - 1);
    const double gz = std::clamp((z - m->wmin[2]) / sz, 0.0, 1.0) * (size - 1);
    const int bx = std::clamp((int)std::floor(gx / step), 0, m->base_cells - 1);
    const int bz = std::clamp((int)std::floor(gz / step), 0, m->base_cells - 1);
    const int subdiv = 1 << m->levels[(size_t)bz * m->base_cells + bx];
    const double cell = (double)step / subdiv;
    const int subx = std::clamp((int)std::floor((gx - bx * step) / cell), 0, subdiv - 1);
    const int subz = std::clamp((int)std::floor((gz - bz * step) / cell), 0, subdiv - 1);
    const double x0f = bx * step + subx * cell, z0f = bz * step + subz * cell;
    /* FMath::RoundToInt is floor(v + 0.5). */
    auto round_i = [](double v) { return (int)std::floor(v + 0.5); };
    const int x0 = std::clamp(round_i(x0f), 0, size - 1);
    const int z0 = std::clamp(round_i(z0f), 0, size - 1);
    const int x1 = std::clamp(round_i(x0f + cell), 0, size - 1);
    const int z1 = std::clamp(round_i(z0f + cell), 0, size - 1);
    const double fx = std::clamp((gx - x0f) / cell, 0.0, 1.0);
    const double fz = std::clamp((gz - z0f) / cell, 0.0, 1.0);
    const double h00 = H(*m, x0, z0) * m->yscale, h10 = H(*m, x1, z0) * m->yscale;
    const double h01 = H(*m, x0, z1) * m->yscale, h11 = H(*m, x1, z1) * m->yscale;
    return fz >= fx ? h00 * (1.0 - fz) + h01 * (fz - fx) + h11 * fx
                    : h00 * (1.0 - fx) + h11 * fz + h10 * (fx - fz);
}

extern "C" int64_t bf6_terrain_mesh_cell_levels(const bf6_terrain_mesh* m, uint8_t* out, int64_t out_max)
{
    if (!m) return 0;
    const int64_t n = (int64_t)m->levels.size();
    if (out && out_max > 0) std::memcpy(out, m->levels.data(), (size_t)std::min(n, out_max));
    return n;
}

extern "C" void bf6_terrain_mesh_free(bf6_terrain_mesh* m)
{
    delete m;
}
