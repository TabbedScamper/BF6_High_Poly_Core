/* CAN THE REFLECTED REGISTRY NAME ITS OWN OPERATORS, WITHOUT GUESSING?
 *
 *   expr_registry_probe <exe path>
 *
 * The corpus census found two operator namespaces that do not overlap at all:
 *   1130 corpus keys have a reflected arity and NO crc32be-hashable name;
 *    271 corpus keys have such a name and NO reflected descriptor;
 *      0 keys have both.
 * Zero overlap where chance predicts about 135 is not a gap in one table, it is
 * two different registration mechanisms. The hash route cannot reach the 1130,
 * so their names must come from the reflection data itself.
 *
 * THE VERIFICATION IS A CLOSED LOOP, needing no external answer key. If a
 * pointer field really leads to an operator's name, then the operator's own key
 * should be a hash of that name - that is what a key IS. So this tries each
 * pointer field, and for every string it reaches, asks whether any of eleven
 * hash functions maps that string back to the key it came from.
 *
 * A field and hash that reproduce the key for essentially every descriptor are
 * proven together: chance cannot round-trip a 32-bit key thousands of times.
 * A field that produces plausible-looking strings but never reproduces the key
 * is reported as UNVERIFIED and its strings are not treated as names, because a
 * readable string next to a key is not evidence that it names it.
 */
#include "expression_registry.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

struct Section {
    std::string name;
    uint32_t va = 0, virtual_size = 0, raw_offset = 0, raw_size = 0;
};

uint32_t rd32(const std::vector<uint8_t>& d, size_t o)
{
    return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) |
           ((uint32_t)d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24);
}
uint16_t rd16(const std::vector<uint8_t>& d, size_t o)
{
    return (uint16_t)((uint32_t)d[o] | ((uint32_t)d[o + 1] << 8));
}
uint64_t rd64(const std::vector<uint8_t>& d, size_t o)
{
    return (uint64_t)rd32(d, o) | ((uint64_t)rd32(d, o + 4) << 32);
}

int64_t va_to_file(uint64_t va, uint64_t base, const std::vector<Section>& secs, size_t sz)
{
    if (va < base) return -1;
    const uint64_t rva = va - base;
    for (const Section& s : secs) {
        const uint64_t span = s.virtual_size > s.raw_size ? s.virtual_size : s.raw_size;
        if (rva < s.va || rva >= (uint64_t)s.va + span) continue;
        const uint64_t f = (uint64_t)s.raw_offset + (rva - s.va);
        return f < sz ? (int64_t)f : -1;
    }
    return -1;
}

std::string c_string_at(const std::vector<uint8_t>& d, int64_t at)
{
    if (at < 0) return std::string();
    size_t i = (size_t)at;
    std::string s;
    while (i < d.size() && d[i] >= 0x20 && d[i] <= 0x7e && s.size() <= 191) {
        s.push_back((char)d[i]); ++i;
    }
    if (i >= d.size() || d[i] != 0 || s.size() < 2) return std::string();
    return s;
}

/* ---- candidate hash functions ---- */
const std::array<uint32_t, 256>& crc_table_be()
{
    static const std::array<uint32_t, 256> t = [] {
        std::array<uint32_t, 256> r{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i << 24;
            for (int k = 0; k < 8; ++k)
                c = (c & 0x80000000u) ? (c << 1) ^ 0x04c11db7u : c << 1;
            r[i] = c;
        }
        return r;
    }();
    return t;
}
const std::array<uint32_t, 256>& crc_table_le()
{
    static const std::array<uint32_t, 256> t = [] {
        std::array<uint32_t, 256> r{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xedb88320u : c >> 1;
            r[i] = c;
        }
        return r;
    }();
    return t;
}
uint32_t h_crc32be(const std::string& s)
{
    uint32_t c = 0xffffffffu;
    for (unsigned char ch : s) c = (c << 8) ^ crc_table_be()[((c >> 24) ^ ch) & 0xff];
    return ~c;
}
uint32_t h_crc32be_raw(const std::string& s)
{
    uint32_t c = 0xffffffffu;
    for (unsigned char ch : s) c = (c << 8) ^ crc_table_be()[((c >> 24) ^ ch) & 0xff];
    return c;
}
uint32_t h_crc32le(const std::string& s)
{
    uint32_t c = 0xffffffffu;
    for (unsigned char ch : s) c = (c >> 8) ^ crc_table_le()[(c ^ ch) & 0xff];
    return ~c;
}
uint32_t h_djb2_ones(const std::string& s)
{
    uint32_t h = 0xffffffffu;
    for (unsigned char ch : s) h = h * 33u + ch;
    return h;
}
uint32_t h_djb2_5381(const std::string& s)
{
    uint32_t h = 5381u;
    for (unsigned char ch : s) h = h * 33u + ch;
    return h;
}
uint32_t h_djb2_xor(const std::string& s)
{
    uint32_t h = 5381u;
    for (unsigned char ch : s) h = (h * 33u) ^ ch;
    return h;
}
uint32_t h_fnv1a(const std::string& s)
{
    uint32_t h = 0x811c9dc5u;
    for (unsigned char ch : s) { h ^= ch; h *= 0x01000193u; }
    return h;
}
uint32_t h_fnv1(const std::string& s)
{
    uint32_t h = 0x811c9dc5u;
    for (unsigned char ch : s) { h *= 0x01000193u; h ^= ch; }
    return h;
}
uint32_t h_sdbm(const std::string& s)
{
    uint32_t h = 0;
    for (unsigned char ch : s) h = ch + (h << 6) + (h << 16) - h;
    return h;
}
uint32_t h_murmur_ish(const std::string& s)
{
    uint32_t h = 0;
    for (unsigned char ch : s) { h ^= ch; h *= 0x5bd1e995u; h ^= h >> 15; }
    return h;
}

struct Hash { const char* name; uint32_t (*fn)(const std::string&); bool lower; };
const Hash HASHES[] = {
    {"crc32be",        h_crc32be,     false},
    {"crc32be-low",    h_crc32be,     true },
    {"crc32be-noxor",  h_crc32be_raw, false},
    {"crc32le",        h_crc32le,     false},
    {"crc32le-low",    h_crc32le,     true },
    {"djb2-seed-1",    h_djb2_ones,   false},
    {"djb2-5381",      h_djb2_5381,   false},
    {"djb2-xor",       h_djb2_xor,    false},
    {"fnv1a",          h_fnv1a,       false},
    {"fnv1",           h_fnv1,        false},
    {"sdbm",           h_sdbm,        false},
    {"murmur-ish",     h_murmur_ish,  false},
};
const size_t NHASH = sizeof(HASHES) / sizeof(HASHES[0]);

std::string lowered(const std::string& s)
{
    std::string r = s;
    for (char& ch : r) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
    return r;
}

/* A route to a string: read a pointer at descriptor+field, optionally follow it
 * and read a second pointer at +second, then read the characters. */
struct Route { int field; int second; bool indirect; };

std::string follow(const std::vector<uint8_t>& d, int64_t desc, const Route& rt,
                   uint64_t base, const std::vector<Section>& secs)
{
    const int64_t at = desc + rt.field;
    if (at < 0 || (size_t)at + 8 > d.size()) return std::string();
    uint64_t va = rd64(d, (size_t)at);
    if (rt.indirect) {
        const int64_t via = va_to_file(va, base, secs, d.size());
        if (via < 0) return std::string();
        const int64_t at2 = via + rt.second;
        if (at2 < 0 || (size_t)at2 + 8 > d.size()) return std::string();
        va = rd64(d, (size_t)at2);
    }
    return c_string_at(d, va_to_file(va, base, secs, d.size()));
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: expr_registry_probe <exe>\n"); return 2; }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string exe = argv[1];

    std::vector<uint8_t> d;
    {
        FILE* f = std::fopen(exe.c_str(), "rb");
        if (!f) { std::printf("cannot open %s\n", exe.c_str()); return 1; }
        std::fseek(f, 0, SEEK_END); const long sz = std::ftell(f); std::fseek(f, 0, SEEK_SET);
        d.resize((size_t)sz);
        const size_t rd = std::fread(d.data(), 1, d.size(), f);
        std::fclose(f);
        if (rd != d.size()) { std::printf("short read\n"); return 1; }
    }
    const uint32_t pe = rd32(d, 0x3c);
    const uint16_t section_count = rd16(d, pe + 6);
    const uint16_t optional_size = rd16(d, pe + 20);
    const size_t optional = (size_t)pe + 24;
    const uint64_t base = rd64(d, optional + 24);
    const size_t table = optional + optional_size;
    std::vector<Section> secs;
    for (uint16_t i = 0; i < section_count; ++i) {
        const size_t at = table + (size_t)i * 40;
        Section s; char nm[9] = {};
        std::memcpy(nm, d.data() + at, 8);
        s.name = nm;
        s.virtual_size = rd32(d, at + 8);
        s.va = rd32(d, at + 12);
        s.raw_size = rd32(d, at + 16);
        s.raw_offset = rd32(d, at + 20);
        secs.push_back(s);
    }

    std::vector<bf6::expression::ReflectedOperator> refl;
    std::string err;
    if (!bf6::expression::read_reflected_operators(exe, refl, err)) {
        std::printf("registry: %s\n", err.c_str()); return 1;
    }
    std::printf("%zu reflected descriptors\n\n", refl.size());

    /* BF6_DESC_KEYS=k1,k2,...: dump each listed operator's descriptor as qwords,
     * marking the ones that point into a section, so the current build's
     * implementation address can be read off rather than taken from a table made
     * against another build. */
    if (const char* ks = std::getenv("BF6_DESC_KEYS")) {
        std::vector<uint32_t> want;
        for (const char* p = ks; *p;) {
            want.push_back((uint32_t)std::strtoul(p, nullptr, 16));
            const char* c = std::strchr(p, ',');
            if (!c) break;
            p = c + 1;
        }
        auto sec_of = [&](uint64_t va) -> std::string {
            if (va < base) return std::string();
            for (const auto& s : secs)
                if (va >= base + s.va && va < base + s.va + s.virtual_size)
                    return s.name;
            return std::string();
        };
        for (uint32_t k : want) {
            bool found = false;
            for (const auto& r : refl) {
                if (r.key != k) continue;
                found = true;
                std::printf("KEY 0x%08X desc 0x%llX params %u ns '%s'\n", k,
                            (unsigned long long)r.descriptor_va, r.parameter_count,
                            r.name_space.c_str());
                const int64_t desc = va_to_file(r.descriptor_va, base, secs, d.size());
                for (int q = 0; q < 16 && desc >= 0; ++q) {
                    const uint64_t v = rd64(d, (size_t)desc + (size_t)q * 8);
                    std::printf("   +0x%02X  0x%016llX  %s\n", q * 8,
                                (unsigned long long)v, sec_of(v).c_str());
                }
            }
            if (!found) std::printf("KEY 0x%08X not in the reflected set\n", k);
        }
        return 0;
    }

    /* ---- enumerate routes ---- */
    std::vector<Route> routes;
    for (int f = -8; f <= 88; f += 8) {
        routes.push_back(Route{f, 0, false});
        for (int s2 = 0; s2 <= 64; s2 += 8) routes.push_back(Route{f, s2, true});
    }

    struct Score { long strings = 0; long match[NHASH] = {}; std::string example; };
    std::vector<Score> score(routes.size());

    for (const auto& r : refl) {
        const int64_t desc = va_to_file(r.descriptor_va, base, secs, d.size());
        if (desc < 0) continue;
        for (size_t i = 0; i < routes.size(); ++i) {
            const std::string s = follow(d, desc, routes[i], base, secs);
            if (s.empty()) continue;
            Score& sc = score[i];
            ++sc.strings;
            if (sc.example.empty()) sc.example = s;
            for (size_t h = 0; h < NHASH; ++h) {
                const uint32_t got = HASHES[h].fn(HASHES[h].lower ? lowered(s) : s);
                if (got == r.key) ++sc.match[h];
            }
        }
    }

    /* ---- report only routes that actually reach strings ---- */
    std::printf("%-22s %8s  %s\n", "route", "strings", "best key round-trip");
    std::vector<size_t> order(routes.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        long ma = 0, mb = 0;
        for (size_t h = 0; h < NHASH; ++h) { if (score[a].match[h] > ma) ma = score[a].match[h];
                                             if (score[b].match[h] > mb) mb = score[b].match[h]; }
        if (ma != mb) return ma > mb;
        return score[a].strings > score[b].strings;
    });
    int shown = 0;
    long best_overall = 0; size_t best_route = 0, best_hash = 0;
    for (size_t oi = 0; oi < order.size(); ++oi) {
        const size_t i = order[oi];
        if (score[i].strings == 0) continue;
        long best = 0; size_t bh = 0;
        for (size_t h = 0; h < NHASH; ++h)
            if (score[i].match[h] > best) { best = score[i].match[h]; bh = h; }
        if (best > best_overall) { best_overall = best; best_route = i; best_hash = bh; }
        if (shown++ >= 16) continue;
        char label[64];
        if (routes[i].indirect)
            std::snprintf(label, sizeof(label), "desc%+d -> +%d", routes[i].field, routes[i].second);
        else
            std::snprintf(label, sizeof(label), "desc%+d", routes[i].field);
        std::printf("%-22s %8ld  %-14s %6ld   e.g. %s\n", label, score[i].strings,
                    best ? HASHES[bh].name : "(none)", best, score[i].example.c_str());
    }

    std::printf("\n");
    if (best_overall == 0) {
        std::printf("NO route round-trips the key under any of the %zu hashes tested.\n", NHASH);
        std::printf("The reflected operators' names are therefore NOT recoverable this way,\n");
        std::printf("and no string reached above may be treated as an operator name.\n");

        /* The operator's own name is absent, but the routes above are not junk:
         * desc+16 reaches a namespace record and desc+80 an array of PARAMETER
         * records, whose names ("ReturnValue", "Success") are exactly what an
         * argument list looks like. A typed signature is not the operator's
         * name, but for reading a program it is worth more than one.
         *
         * The claim is checked structurally rather than by eye: if the array
         * really has one 32-byte record per parameter with its name at +0, then
         * the number of names readable must equal the reflected parameter_count
         * for essentially every descriptor. A stride that is wrong will drift
         * out of step and that count will collapse. */
        std::printf("\n--- parameter array layout ---\n");
        for (int stride = 8; stride <= 64; stride += 8) {
            long exact = 0, tested = 0, names = 0;
            for (const auto& r : refl) {
                if (r.parameter_count == 0 || r.parameters_va == 0) continue;
                const int64_t pa = va_to_file(r.parameters_va, base, secs, d.size());
                if (pa < 0) continue;
                ++tested;
                long got = 0;
                for (uint16_t p = 0; p < r.parameter_count; ++p) {
                    const int64_t at = pa + (int64_t)p * stride;
                    if (at < 0 || (size_t)at + 8 > d.size()) break;
                    const std::string s =
                        c_string_at(d, va_to_file(rd64(d, (size_t)at), base, secs, d.size()));
                    if (s.empty()) break;
                    ++got;
                }
                names += got;
                if (got == r.parameter_count) ++exact;
            }
            std::printf("stride %2d : %5ld of %5ld descriptors yield exactly "
                        "parameter_count names  (%ld names)\n",
                        stride, exact, tested, names);
        }

        /* Having fixed the stride, show what a signature actually looks like.
         * The second qword of a parameter record is tried as a type descriptor
         * whose own name sits at its +0, the same shape the namespace used. */
        std::printf("\n--- sample signatures (stride 32, name at +0) ---\n");
        int printed = 0;
        for (const auto& r : refl) {
            if (r.parameter_count == 0 || r.parameters_va == 0) continue;
            const int64_t pa = va_to_file(r.parameters_va, base, secs, d.size());
            if (pa < 0) continue;
            std::string ns = follow(d, va_to_file(r.descriptor_va, base, secs, d.size()),
                                    Route{16, 0, true}, base, secs);
            std::string line = "0x" + std::string(8, '0');
            std::snprintf(&line[0], 11, "0x%08x", r.key);
            std::printf("  %s  %-24s (", line.c_str(), ns.empty() ? "?" : ns.c_str());
            for (uint16_t p = 0; p < r.parameter_count; ++p) {
                const int64_t at = pa + (int64_t)p * 32;
                if ((size_t)at + 16 > d.size()) break;
                const std::string nm =
                    c_string_at(d, va_to_file(rd64(d, (size_t)at), base, secs, d.size()));
                const std::string ty =
                    c_string_at(d, va_to_file(rd64(d, (size_t)at + 8), base, secs, d.size()));
                std::printf("%s%s%s%s", p ? ", " : "",
                            ty.empty() ? "" : ty.c_str(), ty.empty() ? "" : " ",
                            nm.empty() ? "?" : nm.c_str());
            }
            std::printf(")\n");
            if (++printed >= 20) break;
        }
        return 0;
    }
    char label[64];
    if (routes[best_route].indirect)
        std::snprintf(label, sizeof(label), "desc%+d -> +%d",
                      routes[best_route].field, routes[best_route].second);
    else
        std::snprintf(label, sizeof(label), "desc%+d", routes[best_route].field);
    std::printf("BEST: route %s with %s reproduces the key for %ld of %zu descriptors.\n",
                label, HASHES[best_hash].name, best_overall, refl.size());
    if (best_overall * 10 < (long)refl.size())
        std::printf("That is too few to call proven; treating it as a lead, not a result.\n");
    return 0;
}
