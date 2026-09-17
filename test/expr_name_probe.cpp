/* WHY THE EXECUTABLE CANNOT NAME MOST OPERATOR KEYS.
 *
 *   expr_name_probe <game_dir> <exe path>
 *
 * The corpus census says 271 of 2270 keys resolve and ZERO are ambiguous. Zero
 * ambiguity with 88% unresolved is not a tie-breaking problem, it is a
 * candidate-generation problem: the literal being hashed is not the literal the
 * scanner is offering. So this tries several candidate shapes and several hash
 * functions against the real keys.
 *
 * THE CONTROL IS THE POINT. Any hypothesis that hashes enough strings will hit
 * some keys by chance, and a 32-bit hash over tens of millions of candidates
 * collides often enough to manufacture a convincing-looking table of names. So
 * every hypothesis is run twice: once against the real corpus keys, and once
 * against an equal number of random keys that cannot possibly be real. The
 * control column is the chance rate. A hypothesis is only interesting when it
 * beats its own control by a wide margin.
 *
 * Operator keys and fixup keys are counted separately because it is not proven
 * they are the same namespace, and averaging them would hide it if they differ.
 */
#include "bf6_core.h"
#include "expression_graph.h"
#include "expression_registry.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

uint32_t crc32be(const uint8_t* b, size_t n)
{
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i << 24;
            for (int k = 0; k < 8; ++k)
                c = (c & 0x80000000u) ? (c << 1) ^ 0x04c11db7u : c << 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; ++i)
        c = (c << 8) ^ table[((c >> 24) ^ b[i]) & 0xffu];
    return ~c;
}

/* BF6's string-id hash: djb2 with an all-ones seed and addition, as proven for
 * asset string ids elsewhere in this core. Whether the expression registry uses
 * the same one is exactly what this probe is asking. */
uint32_t djb2_seed_ones(const uint8_t* b, size_t n)
{
    uint32_t h = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) h = h * 33u + b[i];
    return h;
}

uint32_t fnv1a(const uint8_t* b, size_t n)
{
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 0x01000193u; }
    return h;
}

inline bool ident_char(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

struct Hypothesis {
    const char* name;
    uint32_t (*hash)(const uint8_t*, size_t);
    bool lowercase;
    bool suffixes; // also offer word- and camelCase-boundary suffixes
};

struct Tally {
    std::set<uint32_t> real_hit;
    std::set<uint32_t> control_hit;
    std::map<uint32_t, std::string> example;
    long candidates = 0;
};

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: expr_name_probe <game> <exe>\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_all(c, 1, err, (int)sizeof(err));
    const std::string exe = argv[2];

    /* ---- the real keys, operator and fixup kept apart ---- */
    bf6::expression::ArityMap arity;
    {
        const int n0 = bf6_expression_reflected_operators(exe.c_str(), nullptr, 0, nullptr, 0);
        if (n0 > 0) {
            std::vector<bf6_expression_reflected_operator> refl((size_t)n0);
            char rerr[256] = {0};
            const int gotr = bf6_expression_reflected_operators(exe.c_str(), refl.data(), n0,
                                                                rerr, (int)sizeof(rerr));
            for (int k = 0; k < gotr; ++k)
                arity[refl[(size_t)k].key] = refl[(size_t)k].parameter_count;
        }
    }
    std::set<uint32_t> op_keys, fx_keys;
    {
        const int total = bf6_list_res(c, nullptr, nullptr, 0);
        std::vector<bf6_asset> rows((size_t)(total > 0 ? total : 0));
        const int got = total > 0 ? bf6_list_res(c, nullptr, rows.data(), total) : 0;
        for (int i = 0; i < got; ++i) {
            if (rows[(size_t)i].type != 0x7dd4cc89u || !rows[(size_t)i].name) continue;
            const uint8_t* d = nullptr;
            const int64_t n = bf6_read_raw(c, BF6_RAW_RES, rows[(size_t)i].name, &d);
            if (n <= 0 || !d) continue;
            bf6::expression::Graph g;
            std::string e2;
            if (!bf6::expression::parse(d, (size_t)n, g, e2, arity.empty() ? nullptr : &arity))
                continue;
            for (const bf6::expression::Record& r : g.records)
                if (r.has_operator) op_keys.insert(r.operator_key);
            for (const bf6::expression::Fixup& f : g.fixups) fx_keys.insert(f.key);
        }
    }
    bf6_close(c);
    std::set<uint32_t> real_keys = op_keys;
    real_keys.insert(fx_keys.begin(), fx_keys.end());
    std::printf("corpus keys: %zu operator, %zu fixup, %zu distinct overall\n",
                op_keys.size(), fx_keys.size(), real_keys.size());
    std::printf("of those, the exe reflects an arity for %zu\n", arity.size());

    /* ---- the control: the same number of keys that CANNOT be real ---- */
    std::set<uint32_t> control;
    {
        std::mt19937 rng(0x5eed1234u);
        while (control.size() < real_keys.size()) {
            const uint32_t k = rng();
            if (real_keys.find(k) == real_keys.end()) control.insert(k);
        }
    }

    static const Hypothesis HYP[] = {
        {"whole literal, crc32be          ", crc32be,        false, false},
        {"whole literal, lowercased, crc32", crc32be,        true,  false},
        {"whole literal, djb2 seed -1     ", djb2_seed_ones, false, false},
        {"whole lower,   djb2 seed -1     ", djb2_seed_ones, true,  false},
        {"whole literal, fnv1a            ", fnv1a,          false, false},
        {"+ boundary suffixes, crc32be    ", crc32be,        false, true},
        {"+ boundary suffixes, djb2 -1    ", djb2_seed_ones, false, true},
        {"+ boundary suffixes low, djb2 -1", djb2_seed_ones, true,  true},
        {"+ boundary suffixes, fnv1a      ", fnv1a,          false, true},
    };
    const size_t NH = sizeof(HYP) / sizeof(HYP[0]);
    std::vector<Tally> tally(NH);

    /* ---- one pass over the executable ---- */
    std::vector<uint8_t> data;
    {
        FILE* f = std::fopen(exe.c_str(), "rb");
        if (!f) { std::printf("cannot open %s\n", exe.c_str()); return 1; }
        std::fseek(f, 0, SEEK_END);
        const long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        data.resize((size_t)sz);
        const size_t rd = std::fread(data.data(), 1, data.size(), f);
        std::fclose(f);
        if (rd != data.size()) { std::printf("short read\n"); return 1; }
    }
    std::printf("executable: %zu bytes\n\n", data.size());

    std::vector<uint8_t> buf;
    size_t at = 0;
    while (at < data.size()) {
        if (data[at] < 0x20 || data[at] > 0x7e) { ++at; continue; }
        const size_t begin = at;
        while (at < data.size() && data[at] >= 0x20 && data[at] <= 0x7e) ++at;
        const size_t run = at - begin;
        if (at < data.size()) ++at; // step past the terminator
        if (run < 2 || run > 127) continue;
        if (begin + run >= data.size() || data[begin + run] != 0) continue;

        const uint8_t* p = data.data() + begin;
        for (size_t h = 0; h < NH; ++h) {
            Tally& t = tally[h];
            // Offer the whole literal, then optionally each boundary suffix.
            // Suffix starts are restricted to word and camelCase boundaries:
            // an arbitrary mid-word cut is not a name any compiler emitted, and
            // offering them only inflates the chance-collision rate.
            for (size_t s = 0; s < run; ++s) {
                if (s > 0) {
                    if (!HYP[h].suffixes) break;
                    if (!ident_char(p[s])) continue;
                    const bool word_start = !ident_char(p[s - 1]);
                    const bool camel = p[s] >= 'A' && p[s] <= 'Z' &&
                                       p[s - 1] >= 'a' && p[s - 1] <= 'z';
                    if (!word_start && !camel) continue;
                    if (run - s < 3) continue;
                }
                const size_t len = run - s;
                const uint8_t* q = p + s;
                if (HYP[h].lowercase) {
                    buf.assign(q, q + len);
                    for (size_t i = 0; i < len; ++i)
                        if (buf[i] >= 'A' && buf[i] <= 'Z') buf[i] = (uint8_t)(buf[i] + 32);
                    q = buf.data();
                }
                const uint32_t k = HYP[h].hash(q, len);
                ++t.candidates;
                if (real_keys.find(k) != real_keys.end()) {
                    t.real_hit.insert(k);
                    if (t.example.find(k) == t.example.end())
                        t.example[k] = std::string((const char*)p + s, len);
                } else if (control.find(k) != control.end()) {
                    t.control_hit.insert(k);
                }
            }
        }
    }

    std::printf("%-34s %8s %8s %8s %9s\n",
                "hypothesis", "cands", "REAL", "control", "verdict");
    for (size_t h = 0; h < NH; ++h) {
        const Tally& t = tally[h];
        const long real_n = (long)t.real_hit.size();
        const long ctrl_n = (long)t.control_hit.size();
        const char* verdict = "chance";
        if (real_n >= 10 && real_n > 4 * (ctrl_n + 1)) verdict = "SIGNAL";
        else if (real_n > ctrl_n * 2 && real_n >= 5) verdict = "weak";
        std::printf("%-34s %8ld %8ld %8ld %9s\n",
                    HYP[h].name, t.candidates, real_n, ctrl_n, verdict);
    }

    /* Which hypothesis explains keys the others cannot? A hypothesis that only
     * re-finds what crc32-whole already found has added nothing. */
    std::printf("\nkeys gained over the current rule (whole literal, crc32be):\n");
    for (size_t h = 1; h < NH; ++h) {
        std::vector<uint32_t> gained;
        for (uint32_t k : tally[h].real_hit)
            if (tally[0].real_hit.find(k) == tally[0].real_hit.end()) gained.push_back(k);
        std::printf("  %-34s +%ld\n", HYP[h].name, (long)gained.size());
        int shown = 0;
        for (uint32_t k : gained) {
            if (shown++ >= 6) break;
            std::printf("      0x%08x  %s%s\n", k, tally[h].example[k].c_str(),
                        op_keys.count(k) ? "   [operator]" : "   [fixup]");
        }
    }
    return 0;
}
