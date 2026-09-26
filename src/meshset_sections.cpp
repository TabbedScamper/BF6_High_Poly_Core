// Renderable MeshSet sections, engine-neutral.
//
// A faithful port of the Godot add-on's bf6_meshset.gd read_lod(), which is the
// reference: every rule below (LOD list filtering, shadow/zonly/depth drop, UV
// channel choice, the declB TexCoord4 unwrap, absolute-index retry, winding
// swap, degenerate drop, destruction parts, palette selector) is that reader's,
// kept in the same order so the output is identical field for field. The Godot
// parity test compares both over every mesh of a map.
//
// Output is one flat little-endian record (see bf6_core.h) so any engine can
// unpack it without per-section allocations crossing the ABI.
#include "bf6_core.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

const int64_t BASE = 16;
const int64_t SECTION_SIZE = 368;
const int64_t DECL_SIZE = 100;
const int U_POS = 1, U_BONE = 2, U_NORMAL = 6, U_UV0 = 33, U_UV4 = 37, U_SUBMAT = 0x33;

struct Bytes {
    const uint8_t* p;
    int64_t n;
    uint8_t u8(int64_t o) const { return (o >= 0 && o < n) ? p[o] : 0; }
    uint16_t u16(int64_t o) const { uint16_t v = 0; if (o >= 0 && o + 2 <= n) std::memcpy(&v, p + o, 2); return v; }
    uint32_t u32(int64_t o) const { uint32_t v = 0; if (o >= 0 && o + 4 <= n) std::memcpy(&v, p + o, 4); return v; }
    int32_t s32(int64_t o) const { return (int32_t)u32(o); }
    int16_t s16(int64_t o) const { return (int16_t)u16(o); }
    uint64_t u64(int64_t o) const { uint64_t v = 0; if (o >= 0 && o + 8 <= n) std::memcpy(&v, p + o, 8); return v; }
    int64_t s64(int64_t o) const { return (int64_t)u64(o); }
    float f32(int64_t o) const { float v = 0; if (o >= 0 && o + 4 <= n) std::memcpy(&v, p + o, 4); return v; }
};

// Godot's decode_half: IEEE half to float.
float half_to_float(uint16_t h)
{
    const int s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    float v;
    if (e == 0) v = std::ldexp((float)m, -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp((float)(m | 0x400), e - 25);
    return s ? -v : v;
}

int components(int fmt)
{
    switch (fmt) {
    case 1: case 5: case 14: case 18: case 50: return 1;
    case 2: case 6: case 15: case 19: case 22: case 24: return 2;
    case 3: case 7: case 16: case 20: return 3;
    case 4: case 8: case 10: case 11: case 12: case 13: case 17: case 21: case 23: case 25: return 4;
    }
    return 0;
}

int fmt_size(int fmt)
{
    switch (fmt) {
    case 1: return 4; case 2: return 8; case 3: return 12; case 4: return 16;
    case 5: return 2; case 6: return 4; case 7: return 6; case 8: return 8;
    case 10: case 11: case 12: case 13: return 4;
    case 14: return 2; case 15: return 4; case 16: return 6; case 17: return 8;
    case 18: return 2; case 19: return 4; case 20: return 6; case 21: return 8;
    case 22: return 4; case 23: return 8; case 24: return 4; case 25: return 8;
    case 50: return 1;
    }
    return 0;
}

struct Element { int usage, fmt, off, stream; };
struct Stream { int stride, cls; };
struct Decl { std::vector<Element> elements; std::vector<Stream> streams; };

std::string cstr(const Bytes& d, int64_t off)
{
    if (off <= 0 || off >= d.n) return {};
    const void* z = std::memchr(d.p + off, 0, (size_t)(d.n - off));
    int64_t e = z ? (int64_t)((const uint8_t*)z - d.p) : d.n;
    return std::string((const char*)d.p + off, (size_t)(e - off));
}

Decl read_decl(const Bytes& d, int64_t off)
{
    Decl out;
    if (off + DECL_SIZE > d.n) return out;
    const int ne = d.u8(off + 96), ns = d.u8(off + 97);
    for (int i = 0; i < 16 && i < ne; ++i) {
        const int64_t p = off + i * 4;
        out.elements.push_back({ d.u8(p), d.u8(p + 1), d.u8(p + 2), d.u8(p + 3) });
    }
    for (int i = 0; i < 16 && i < ns; ++i) {
        const int64_t p = off + 64 + i * 2;
        out.streams.push_back({ d.u8(p), d.u8(p + 1) });
    }
    return out;
}

struct Section {
    std::string material;
    uint64_t state_key = 0;
    int material_id = 0;
    int64_t prim_count = 0, start_index = 0, vertex_offset = 0, vertex_count = 0;
    Decl decl, decl0, decl1;
};

struct Lod {
    std::vector<Section> sections;
    bool idx32 = false;
    int64_t index_size = 0, vertex_size = 0, inline_offset = 0;
    bool has_chunk = false;
};

struct Info { std::vector<Lod> lods; std::string error; bool ok = false; };

Info parse(const Bytes& d)
{
    Info info;
    if (d.n < BASE + 0xA0) {
        info.error = "too short to be a MeshSet (" + std::to_string(d.n) + " bytes)";
        return info;
    }
    int64_t lod_stride = d.u32(0);
    if (lod_stride != 176 && lod_stride != 192) lod_stride = 176;
    const int64_t h = BASE;
    int64_t lod_offs[6];
    for (int i = 0; i < 6; ++i) lod_offs[i] = d.s64(h + 0x20 + i * 8);
    const int lod_count = d.u16(h + 0x9C);
    for (int li = 0; li < lod_count && li < 6; ++li) {
        const int64_t lo = lod_offs[li] + BASE;
        if (lo <= 0 || lo >= d.n - lod_stride) continue;
        const int64_t sec_count = d.u32(lo + 0x08);
        const int64_t sec_off = d.s64(lo + 0x0C) + BASE;
        const int64_t idx_fmt = d.u32(lo + 0x54);
        Lod L;
        L.index_size = d.s32(lo + 0x58);
        L.vertex_size = d.s32(lo + 0x5C);
        L.inline_offset = d.u32(lo + 0x84);
        L.idx32 = idx_fmt == 46;
        if (sec_count > 4096 || sec_off <= 0 || sec_off >= d.n) continue;
        for (int64_t i = 0; i < sec_count; ++i) {
            const int64_t p = sec_off + i * SECTION_SIZE;
            if (p + SECTION_SIZE > d.n) break;
            Section s;
            s.material = cstr(d, d.s64(p + 0x08) + BASE);
            const int bpv = d.u8(p + 0x1A);
            s.decl0 = read_decl(d, p + 0x64);
            s.decl1 = read_decl(d, p + 0xC8);
            s.decl = s.decl0;
            if (bpv > 0) {
                bool any = false;
                for (const Element& e : s.decl1.elements) if (e.fmt != 0) { any = true; break; }
                if (any) s.decl = s.decl1;
            }
            s.state_key = d.u64(p + 0x130);
            s.material_id = d.u16(p + 0x1C);
            s.prim_count = d.u32(p + 0x20);
            s.start_index = d.u32(p + 0x24);
            s.vertex_offset = d.u32(p + 0x28);
            s.vertex_count = d.u32(p + 0x2C);
            L.sections.push_back(std::move(s));
        }
        info.lods.push_back(std::move(L));
    }
    info.ok = true;
    return info;
}

// _read_attr: [values, components] or false.
bool read_attr(const Bytes& buf, int64_t base, int64_t count, const Element& el,
               const std::vector<Stream>& streams, std::vector<float>& out, int& comps)
{
    if (count <= 0 || base < 0 || base > buf.n) return false;
    const int fmt = el.fmt, off = el.off, si = el.stream;
    if (si < 0 || si >= (int)streams.size() || off < 0) return false;
    const int64_t sstride = streams[(size_t)si].stride;
    const int64_t size = fmt_size(fmt);
    if (size == 0 || sstride <= 0) return false;
    int64_t sbase = base;
    for (int s = 0; s < si; ++s) {
        const int64_t prefix = streams[(size_t)s].stride;
        if (prefix < 0 || prefix > (buf.n - sbase) / count) return false;
        sbase += prefix * count;
    }
    if (off > buf.n - sbase || size > buf.n - sbase - off) return false;
    if (count - 1 > (buf.n - sbase - off - size) / sstride) return false;
    comps = components(fmt);
    if (comps == 0) return false;
    // Native decode first, exactly as the script does, then its own fallback.
    const int64_t floats = count * comps;
    if (floats <= 256LL * 1024 * 1024 && count <= INT32_MAX) {
        const int q = bf6_decode_vertex_attribute(buf.p, buf.n, sbase + off, sstride, (int32_t)count, fmt, nullptr, 0);
        if (q == comps) {
            out.assign((size_t)floats, 0.0f);
            if (bf6_decode_vertex_attribute(buf.p, buf.n, sbase + off, sstride, (int32_t)count, fmt, out.data(), floats) == comps)
                return true;
        }
    }
    out.assign((size_t)floats, 0.0f);
    for (int64_t i = 0; i < count; ++i) {
        const int64_t p = sbase + i * sstride + off;
        const int64_t o = i * comps;
        for (int c = 0; c < comps; ++c) {
            float v;
            switch (fmt) {
            case 1: case 2: case 3: case 4: v = buf.f32(p + c * 4); break;
            case 5: case 6: case 7: case 8: v = half_to_float(buf.u16(p + c * 2)); break;
            case 14: case 15: case 16: case 17: v = (float)buf.s16(p + c * 2); break;
            case 18: case 19: case 20: case 21: v = (float)((double)buf.s16(p + c * 2) / 32767.0); break;
            case 22: case 23: v = (float)buf.u16(p + c * 2); break;
            case 24: case 25: v = (float)((double)buf.u16(p + c * 2) / 65535.0); break;
            case 11: case 13: v = (float)((double)buf.u8(p + c) / 255.0); break;
            case 10: case 12: v = (float)buf.u8(p + c); break;
            default: return false;
            }
            out[(size_t)(o + c)] = v;
        }
    }
    return true;
}

bool read_indices(const Bytes& buf, int64_t vsize, int64_t isize, bool idx32, int64_t start,
                  int64_t prim_count, int64_t vcount, int64_t voff, std::vector<int32_t>& out)
{
    out.clear();
    const int64_t stride = idx32 ? 4 : 2;
    const int64_t need = prim_count * 3;
    const int64_t first = vsize + start * stride;
    if (first + need * stride > vsize + isize) return false;
    std::vector<int64_t> raw((size_t)need);
    for (int64_t i = 0; i < need; ++i)
        raw[(size_t)i] = idx32 ? (int64_t)buf.u32(first + i * 4) : (int64_t)buf.u16(first + i * 2);
    int64_t hi = 0;
    for (int64_t v : raw) hi = v > hi ? v : hi;
    if (hi >= vcount && voff > 0) {
        bool ok = true;
        for (int64_t i = 0; i < need; ++i) {
            const int64_t v2 = raw[(size_t)i] - voff;
            if (v2 < 0 || v2 >= vcount) { ok = false; break; }
        }
        if (ok) for (int64_t& v : raw) v -= voff;
    }
    out.reserve((size_t)need);
    for (int64_t t = 0; t < prim_count; ++t) {
        const int64_t a = raw[(size_t)(t * 3)], b = raw[(size_t)(t * 3 + 1)], c = raw[(size_t)(t * 3 + 2)];
        if (a < 0 || b < 0 || c < 0 || a >= vcount || b >= vcount || c >= vcount) continue;
        if (a == b || b == c || a == c) continue;
        out.push_back((int32_t)a);
        out.push_back((int32_t)c);
        out.push_back((int32_t)b);
    }
    return !out.empty();
}

void read_parts(const Bytes& buf, int64_t base, int64_t count, const Section& s, std::vector<int32_t>& out)
{
    out.clear();
    for (const Decl* dc : { &s.decl0, &s.decl1 }) {
        for (const Element& el : dc->elements) {
            if (el.usage != U_BONE) continue;
            int lanes = 0;
            if (el.fmt == 22 || el.fmt == 24) lanes = 2;
            else if (el.fmt == 17 || el.fmt == 21 || el.fmt == 23 || el.fmt == 25) lanes = 4;
            if (lanes == 0) continue;
            if (el.stream >= (int)dc->streams.size()) continue;
            const int64_t sstride = dc->streams[(size_t)el.stream].stride;
            if (sstride == 0) continue;
            int64_t sbase = base;
            for (int k = 0; k < el.stream; ++k) sbase += (int64_t)dc->streams[(size_t)k].stride * count;
            const int64_t lane_off = el.off + (lanes - 1) * 2;
            if (sbase + (count - 1) * sstride + lane_off + 2 > buf.n) continue;
            out.resize((size_t)count);
            for (int64_t i = 0; i < count; ++i)
                out[(size_t)i] = (int32_t)(buf.u16(sbase + i * sstride + lane_off) & 0x7FFF);
            return;
        }
    }
}

bool read_sel(const Bytes& buf, int64_t base, int64_t count, const Section& s,
              std::vector<uint8_t>& out, uint32_t& mask)
{
    for (const Decl* dc : { &s.decl0, &s.decl1 }) {
        for (const Element& el : dc->elements) {
            if (el.usage != U_SUBMAT || el.fmt != 12) continue;
            if (el.stream >= (int)dc->streams.size()) continue;
            const int64_t sstride = dc->streams[(size_t)el.stream].stride;
            if (sstride == 0) continue;
            int64_t sbase = base;
            for (int k = 0; k < el.stream; ++k) sbase += (int64_t)dc->streams[(size_t)k].stride * count;
            if (sbase + (count - 1) * sstride + el.off + 4 > buf.n) continue;
            out.resize((size_t)count);
            mask = 0;
            for (int64_t i = 0; i < count; ++i) {
                const uint8_t b = buf.u8(sbase + i * sstride + el.off);
                out[(size_t)i] = b;
                mask |= 1u << (b < 8 ? b : 8);
            }
            return true;
        }
    }
    return false;
}

std::string lower(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

struct Writer {
    std::string o;
    void u32(uint32_t v) { o.append((const char*)&v, 4); }
    void s32(int32_t v) { o.append((const char*)&v, 4); }
    void u64(uint64_t v) { o.append((const char*)&v, 8); }
    void pad4() { while (o.size() % 4) o.push_back('\0'); }
    void str(const std::string& s) { u32((uint32_t)s.size()); o += s; pad4(); }
    // Components beyond `want` are dropped, as _vectors2/_vectors3 do.
    void vecs(const std::vector<float>& v, int comps, int want, int64_t count)
    {
        for (int64_t i = 0; i < count; ++i)
            for (int c = 0; c < want; ++c) {
                float f = v[(size_t)(i * comps + c)];
                o.append((const char*)&f, 4);
            }
    }
};

// One decoded, renderable section: read_lod_script's dictionary as a struct.
struct Decoded {
    std::string material;
    uint64_t state_key = 0;
    uint32_t material_id = 0;
    int64_t vcount = 0;
    std::vector<float> pos, uvs, normals, uv2;   // 3V, 2V, 3V or empty, 2V or empty
    int uv2_src = 0, uv_rule = 0, uv_usage = -1;
    uint32_t pal_mask = 0;
    bool has_pal = false;
    std::vector<uint32_t> usages;
    std::vector<int32_t> idx, parts;
    std::vector<uint8_t> pal;
    std::vector<std::pair<uint32_t, std::vector<float>>> uv_all;   // every set, 2V each
    bool carpaint = false;
};

void take(const std::vector<float>& v, int comps, int want, int64_t count, std::vector<float>& out)
{
    out.resize((size_t)(count * want));
    for (int64_t i = 0; i < count; ++i)
        for (int c = 0; c < want; ++c) out[(size_t)(i * want + c)] = v[(size_t)(i * comps + c)];
}

bool decode_lod(const uint8_t* res, int64_t res_len, int lod, const uint8_t* chunk, int64_t chunk_len,
                bool keep_shadow, std::vector<Decoded>& out, std::string& error)
{
    const Bytes d{ res, res ? res_len : 0 };
    Info info = parse(d);
    error = info.error;
    if (!info.ok || lod < 0 || lod >= (int)info.lods.size()) return true;
    const Lod& L = info.lods[(size_t)lod];
    std::vector<uint8_t> inline_copy;
    Bytes buf{ chunk, chunk ? chunk_len : 0 };
    if (buf.n <= 0) {
        int64_t span = 0;
        for (const Lod& x : info.lods) {
            const int64_t e = x.inline_offset + x.vertex_size + x.index_size;
            span = e > span ? e : span;
        }
        const int64_t base = d.n - span;
        if (base < 0) {
            error = "inline span " + std::to_string(span) + " exceeds file " + std::to_string(d.n);
            buf = Bytes{ nullptr, 0 };
        } else {
            // slice(start, end) clamps to the file, as Godot's does.
            int64_t start = base + L.inline_offset, end = start + L.vertex_size + L.index_size;
            start = start < 0 ? 0 : (start > d.n ? d.n : start);
            end = end < start ? start : (end > d.n ? d.n : end);
            inline_copy.assign(d.p + start, d.p + end);
            buf = Bytes{ inline_copy.data(), (int64_t)inline_copy.size() };
        }
    }
    const int64_t vsize = L.vertex_size, isize = L.index_size;
    if (buf.n < vsize + isize) {
        error = "geometry short: " + std::to_string(buf.n) + " < " + std::to_string(vsize) + "+" + std::to_string(isize);
        return true;
    }
    std::vector<float> pos, nrm, tmp, uv2v;
    struct UvSet { int usage; std::vector<float> v; int comps; };
    for (const Section& s : L.sections) {
        const int64_t vcount = s.vertex_count, pcount = s.prim_count;
        if (pcount == 0 || vcount == 0) continue;
        const std::string low = lower(s.material);
        if (!keep_shadow && (low.find("shadow") != std::string::npos || low.find("zonly") != std::string::npos
                             || low.find("depth") != std::string::npos))
            continue;
        const int64_t voff = s.vertex_offset;
        pos.clear(); nrm.clear();
        int pos_comps = 0, nrm_comps = 0;
        std::vector<UvSet> uv_sets;
        for (const Element& el : s.decl.elements) {
            int c = 0;
            if (el.usage == U_POS && pos.empty()) {
                if (read_attr(buf, voff, vcount, el, s.decl.streams, tmp, c)) { pos.swap(tmp); pos_comps = c; }
            } else if (el.usage == U_NORMAL && nrm.empty()) {
                if (read_attr(buf, voff, vcount, el, s.decl.streams, tmp, c)) { nrm.swap(tmp); nrm_comps = c; }
            } else if (el.usage >= U_UV0 && el.usage <= 36) {
                if (read_attr(buf, voff, vcount, el, s.decl.streams, tmp, c) && c >= 2)
                    uv_sets.push_back({ el.usage, tmp, c });
            }
        }
        if (pos.empty() || pos_comps < 3) continue;
        Decoded o;
        o.carpaint = low.find("carpaint") != std::string::npos;
        if (!uv_sets.empty()) {
            o.uv_rule = low.find("unique") != std::string::npos ? 1 : (o.carpaint ? 2 : 3);
            o.uv_usage = uv_sets[0].usage;
        }
        int uv2_comps = 0;
        uv2v.clear();
        const Element* b_tc0 = nullptr, *b_tc4 = nullptr;
        for (const Element& el : s.decl1.elements) {
            if (el.usage == U_UV0 && !b_tc0) b_tc0 = &el;
            else if (el.usage == U_UV4 && !b_tc4) b_tc4 = &el;
        }
        if (b_tc4 && b_tc0) {
            if (b_tc4->stream == b_tc0->stream && b_tc4->off == b_tc0->off) {
                o.uv2_src = 1;
            } else {
                int c = 0;
                if (read_attr(buf, voff, vcount, *b_tc4, s.decl.streams, tmp, c) && c >= 2) {
                    uv2v.swap(tmp); uv2_comps = c; o.uv2_src = 2;
                }
            }
        }
        if (o.uv2_src == 0 && uv_sets.size() > 1) {
            uv2v = uv_sets[1].v; uv2_comps = uv_sets[1].comps; o.uv2_src = 3;
        }
        if (!read_indices(buf, vsize, isize, L.idx32, s.start_index, pcount, vcount, voff, o.idx)) continue;
        read_parts(buf, voff, vcount, s, o.parts);
        o.has_pal = read_sel(buf, voff, vcount, s, o.pal, o.pal_mask);
        o.material = s.material;
        o.state_key = s.state_key;
        o.material_id = (uint32_t)s.material_id;
        o.vcount = vcount;
        take(pos, pos_comps, 3, vcount, o.pos);
        if (uv_sets.empty()) o.uvs.assign((size_t)(vcount * 2), 0.0f);
        else take(uv_sets[0].v, uv_sets[0].comps, 2, vcount, o.uvs);
        if (nrm_comps >= 3) take(nrm, nrm_comps, 3, vcount, o.normals);
        if (!uv2v.empty()) take(uv2v, uv2_comps, 2, vcount, o.uv2);
        for (const UvSet& u : uv_sets) {
            o.usages.push_back((uint32_t)u.usage);
            o.uv_all.emplace_back((uint32_t)u.usage, std::vector<float>());
            take(u.v, u.comps, 2, vcount, o.uv_all.back().second);
        }
        out.push_back(std::move(o));
    }
    return true;
}

uint8_t* finish(Writer& w, int64_t& len)
{
    uint8_t* blob = (uint8_t*)std::malloc(w.o.size());
    if (!blob) return nullptr;
    std::memcpy(blob, w.o.data(), w.o.size());
    len = (int64_t)w.o.size();
    return blob;
}

void put_floats(Writer& w, const std::vector<float>& v) { w.o.append((const char*)v.data(), v.size() * 4); }

}  // namespace

extern "C" BF6_API int64_t bf6_meshset_sections(const uint8_t* res, int64_t res_len, int lod,
    const uint8_t* chunk, int64_t chunk_len, int flags, uint8_t** out)
{
    if (!out) return -1;
    *out = nullptr;
    try {
        std::vector<Decoded> secs;
        std::string error;
        decode_lod(res, res_len, lod, chunk, chunk_len, (flags & BF6_MESHSET_KEEP_SHADOW) != 0, secs, error);
        const bool keep_all_uvs = (flags & BF6_MESHSET_KEEP_ALL_UVS) != 0;
        Writer w;
        w.u32(0x4C534D42);   // "BMSL"
        w.u32(1);
        w.u32((uint32_t)secs.size());
        w.str(error);
        for (const Decoded& s : secs) {
            uint32_t present = 0;
            if (!s.normals.empty()) present |= 1;
            if (!s.uv2.empty()) present |= 2;
            if (!s.parts.empty()) present |= 4;
            if (s.has_pal) present |= 8;
            w.str(s.material);
            w.u64(s.state_key);
            w.u32(s.material_id);
            w.u32((uint32_t)s.vcount);
            w.u32(present);
            w.u32((uint32_t)s.uv2_src);
            w.u32((uint32_t)s.uv_rule);
            w.s32(s.uv_usage);
            w.u32(s.pal_mask);
            w.u32((uint32_t)s.usages.size());
            for (uint32_t u : s.usages) w.u32(u);
            put_floats(w, s.pos);
            put_floats(w, s.uvs);
            if (present & 1) put_floats(w, s.normals);
            if (present & 2) put_floats(w, s.uv2);
            w.u32((uint32_t)s.idx.size());
            w.o.append((const char*)s.idx.data(), s.idx.size() * 4);
            if (present & 4) w.o.append((const char*)s.parts.data(), s.parts.size() * 4);
            if (present & 8) { w.o.append((const char*)s.pal.data(), s.pal.size()); w.pad4(); }
            const bool all = keep_all_uvs || s.carpaint;
            w.u32(all ? (uint32_t)s.uv_all.size() : 0);
            if (all)
                for (const auto& u : s.uv_all) { w.u32(u.first); put_floats(w, u.second); }
        }
        int64_t len = 0;
        *out = finish(w, len);
        return *out ? len : -1;
    } catch (...) {
        return -1;
    }
}

// The Godot add-on's merge (highpoly_gamesource._mesh_for_body): one surface per
// shader state key, or per canonical colour where a palette split applies,
// hidden destruction parts dropped per triangle, attributes kept only when every
// section feeding a surface carries them.
extern "C" BF6_API int64_t bf6_meshset_surfaces(const uint8_t* res, int64_t res_len, int lod,
    const uint8_t* chunk, int64_t chunk_len, int flags,
    const int32_t* hidden, int32_t hidden_count,
    const int32_t* uv_overrides, int32_t uv_override_count,
    const int64_t* canon_keys, const uint8_t* canon_tables, int32_t canon_count,
    uint8_t** out)
{
    if (!out) return -1;
    *out = nullptr;
    try {
        std::vector<Decoded> secs;
        std::string error;
        decode_lod(res, res_len, lod, chunk, chunk_len, (flags & BF6_MESHSET_KEEP_SHADOW) != 0, secs, error);
        // Car paint wrap channel, chosen by the caller from the depot.
        for (int32_t i = 0; i + 0 < uv_override_count; ++i) {
            const int32_t si = uv_overrides[i * 2], usage = uv_overrides[i * 2 + 1];
            if (si < 0 || si >= (int32_t)secs.size()) continue;
            for (auto& u : secs[(size_t)si].uv_all)
                if ((int32_t)u.first == usage) {
                    if ((int64_t)u.second.size() == secs[(size_t)si].vcount * 2) secs[(size_t)si].uvs = u.second;
                    break;
                }
        }
        std::vector<int32_t> hid(hidden, hidden + (hidden && hidden_count > 0 ? hidden_count : 0));
        std::sort(hid.begin(), hid.end());
        auto is_hidden = [&](int32_t p) { return std::binary_search(hid.begin(), hid.end(), p); };

        // key -> entries its vertices select (bit 8: unusable).
        std::vector<std::pair<int64_t, uint32_t>> key_sel;
        auto sel_of = [&](int64_t key) -> uint32_t* {
            for (auto& ks : key_sel) if (ks.first == key) return &ks.second;
            key_sel.emplace_back(key, 0u);
            return &key_sel.back().second;
        };
        for (const Decoded& s : secs) {
            const uint32_t m = s.has_pal ? s.pal_mask : 0x100;
            *sel_of((int64_t)s.state_key) |= m;
        }
        for (auto& ks : key_sel) if (ks.second & 0x100) ks.second = 0;
        auto bits = [](uint32_t mask) {
            std::vector<int> r;
            if (mask == 0 || (mask & 0x100)) return r;
            for (int k = 0; k < 8; ++k) if (mask & (1u << k)) r.push_back(k);
            return r;
        };
        // Canon tables apply only where the key's entries are different colours.
        std::vector<std::pair<int64_t, const uint8_t*>> key_canon;
        for (int32_t i = 0; i < canon_count; ++i) {
            const int64_t key = canon_keys[i];
            const uint8_t* canon = canon_tables + (size_t)i * 8;
            uint32_t mask = 0;
            for (auto& ks : key_sel) if (ks.first == key) mask = ks.second;
            const auto entries = bits(mask);
            if (entries.size() < 2) continue;
            bool distinct = false;
            for (int e : entries) if (canon[e] != canon[entries[0]]) distinct = true;
            if (distinct) key_canon.emplace_back(key, canon);
        }
        auto canon_of = [&](int64_t key) -> const uint8_t* {
            for (auto& kc : key_canon) if (kc.first == key) return kc.second;
            return nullptr;
        };

        struct Bucket {
            std::string bid;
            int64_t key = 0;
            int piece = -1;
            std::vector<float> v, n, u, u2;
            std::vector<int32_t> i;
            bool want_n = true, want_u = true, want_u2 = true;
        };
        std::vector<Bucket> buckets;
        auto bucket_of = [&](const std::string& bid, int64_t key, int piece) -> size_t {
            for (size_t b = 0; b < buckets.size(); ++b) if (buckets[b].bid == bid) return b;
            buckets.push_back(Bucket{});
            buckets.back().bid = bid;
            buckets.back().key = key;
            buckets.back().piece = piece;
            return buckets.size() - 1;
        };
        for (const Decoded& s : secs) {
            if (s.pos.empty() || s.idx.empty()) continue;
            const int64_t n = s.vcount;
            const int64_t key = (int64_t)s.state_key;
            const uint8_t* canon = canon_of(key);
            std::vector<int> gk;
            std::vector<size_t> bids;
            if (canon && s.has_pal && (int64_t)s.pal.size() == n) {
                bool seen[256] = {};
                for (uint8_t v : s.pal) seen[canon[v]] = true;
                for (int p = 0; p < 256; ++p)
                    if (seen[p]) {
                        gk.push_back(p);
                        bids.push_back(bucket_of(std::to_string(key) + "@" + std::to_string(p), key, p));
                    }
            } else {
                bids.push_back(bucket_of(std::to_string(key), key, -1));
            }
            std::vector<int64_t> bases;
            for (size_t b : bids) {
                Bucket& B = buckets[b];
                bases.push_back((int64_t)(B.v.size() / 3));
                B.v.insert(B.v.end(), s.pos.begin(), s.pos.end());
                if ((int64_t)s.normals.size() == n * 3) B.n.insert(B.n.end(), s.normals.begin(), s.normals.end());
                else B.want_n = false;
                B.u.insert(B.u.end(), s.uvs.begin(), s.uvs.end());
                if (s.uv2_src == 2 && (int64_t)s.uv2.size() == n * 2) B.u2.insert(B.u2.end(), s.uv2.begin(), s.uv2.end());
                else B.want_u2 = false;
            }
            const std::vector<int32_t>& ii = s.idx;
            const std::vector<int32_t>& pv = s.parts;
            const bool filter = !hid.empty() && !pv.empty();
            const int64_t tri_end = (int64_t)ii.size() - 2;
            if (bids.size() == 1) {
                Bucket& B = buckets[bids[0]];
                const int32_t base = (int32_t)bases[0];
                if (filter) {
                    for (int64_t k = 0; k < tri_end; k += 3) {
                        const int32_t v0 = ii[(size_t)k];
                        if (v0 < (int32_t)pv.size() && is_hidden(pv[(size_t)v0])) continue;
                        B.i.push_back(ii[(size_t)k] + base);
                        B.i.push_back(ii[(size_t)k + 1] + base);
                        B.i.push_back(ii[(size_t)k + 2] + base);
                    }
                } else {
                    for (int32_t x : ii) B.i.push_back(x + base);
                }
            } else {
                for (size_t j = 0; j < bids.size(); ++j) {
                    Bucket& B = buckets[bids[j]];
                    const int32_t base = (int32_t)bases[j];
                    for (int64_t k = 0; k < tri_end; k += 3) {
                        const int32_t v0 = ii[(size_t)k];
                        if (canon[s.pal[(size_t)v0]] != gk[j]) continue;
                        if (filter && v0 < (int32_t)pv.size() && is_hidden(pv[(size_t)v0])) continue;
                        B.i.push_back(v0 + base);
                        B.i.push_back(ii[(size_t)k + 1] + base);
                        B.i.push_back(ii[(size_t)k + 2] + base);
                    }
                }
            }
        }

        Writer w;
        w.u32(0x46534D42);   // "BMSF"
        w.u32(1);
        uint32_t surfaces = 0;
        for (const Bucket& B : buckets) if (!B.v.empty() && !B.i.empty()) ++surfaces;
        w.u32(surfaces);
        w.u32((uint32_t)secs.size());
        w.str(error);
        // Section metadata, for the caller's material and wrap decisions.
        for (const Decoded& s : secs) {
            w.str(s.material);
            w.u64(s.state_key);
            w.u32((uint32_t)s.vcount);
            w.u32((uint32_t)s.uv_rule);
            w.s32(s.uv_usage);
            w.u32((uint32_t)s.usages.size());
            for (uint32_t u : s.usages) w.u32(u);
        }
        w.u32((uint32_t)key_sel.size());
        for (const auto& ks : key_sel) { w.u64((uint64_t)ks.first); w.u32(ks.second); }
        w.u32((uint32_t)key_canon.size());
        for (const auto& kc : key_canon) w.u64((uint64_t)kc.first);
        for (const Bucket& B : buckets) {
            if (B.v.empty() || B.i.empty()) continue;
            const int64_t vc = (int64_t)(B.v.size() / 3);
            // The surface name: "<key>" or "<key>@e1,e2" (the raw entries this bucket draws).
            std::string name = std::to_string(B.key);
            uint32_t mask = 0;
            for (auto& ks : key_sel) if (ks.first == B.key) mask = ks.second;
            auto sel = bits(mask);
            if (!sel.empty()) {
                std::vector<int> want = sel;
                if (B.piece >= 0) {
                    const uint8_t* canon = canon_of(B.key);
                    want.clear();
                    for (int e : sel) if (canon && canon[e] == B.piece) want.push_back(e);
                }
                if (!want.empty()) {
                    name += "@";
                    for (size_t k = 0; k < want.size(); ++k) name += (k ? "," : "") + std::to_string(want[k]);
                }
            }
            uint32_t present = 0;
            if (B.want_n && (int64_t)B.n.size() == vc * 3) present |= 1;
            if (B.want_u && (int64_t)B.u.size() == vc * 2) present |= 2;
            if (B.want_u2 && (int64_t)B.u2.size() == vc * 2) present |= 4;
            w.str(name);
            w.u32((uint32_t)vc);
            w.u32(present);
            put_floats(w, B.v);
            if (present & 1) put_floats(w, B.n);
            if (present & 2) put_floats(w, B.u);
            if (present & 4) put_floats(w, B.u2);
            w.u32((uint32_t)B.i.size());
            w.o.append((const char*)B.i.data(), B.i.size() * 4);
        }
        int64_t len = 0;
        *out = finish(w, len);
        return *out ? len : -1;
    } catch (...) {
        return -1;
    }
}

#include "meshset.h"

/* THE BONE EACH VERTEX RIDES (see bf6_core.h). A vehicle body is a skinned MeshSet
 * whose skin indices are skeleton bones directly - measured on the quad's body:
 * Chassis 8454 vertices, Handlebars 4244, every control arm, damper and tie rod its
 * own - so the vehicle graphs' bone poses move it once it is skinned to the rig. */
extern "C" BF6_API int64_t bf6_meshset_vertex_bones(const uint8_t* res, int64_t res_len, int lod,
    const uint8_t* chunk, int64_t chunk_len, float** out)
{
    if (!out) return -1;
    *out = nullptr;
    try {
        std::string err;
        bf6::MeshSet ms = bf6::meshset_parse(res, (size_t)res_len, err);
        if (ms.lods.empty() || lod < 0 || lod >= (int)ms.lods.size()) return -1;
        const auto secs = bf6::meshset_read_lod(ms, lod, chunk, (size_t)chunk_len, err);
        std::vector<float> o;
        for (const auto& s : secs) {
            const size_t vc = s.positions.size() / 3;
            const bool skinned = s.influences > 0 && s.skin_bones.size() >= vc * (size_t)s.influences &&
                                 s.skin_weights.size() >= vc * (size_t)s.influences;
            for (size_t v = 0; v < vc; ++v) {
                float bone = -1.0f, best = -1.0f;
                if (skinned)
                    for (int k = 0; k < s.influences; ++k) {
                        const float wgt = s.skin_weights[v * (size_t)s.influences + (size_t)k];
                        if (wgt > best) { best = wgt; bone = (float)s.skin_bones[v * (size_t)s.influences + (size_t)k]; }
                    }
                o.push_back(s.positions[v * 3]);
                o.push_back(s.positions[v * 3 + 1]);
                o.push_back(s.positions[v * 3 + 2]);
                o.push_back(bone);
            }
        }
        float* blob = (float*)std::malloc(o.size() * sizeof(float) + 4);
        if (!blob) return -1;
        std::memcpy(blob, o.data(), o.size() * sizeof(float));
        *out = blob;
        return (int64_t)(o.size() / 4);
    } catch (...) {
        return -1;
    }
}
