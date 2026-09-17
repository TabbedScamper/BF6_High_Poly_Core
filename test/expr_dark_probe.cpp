/* WHERE ARE THE 869 DARK OPERATORS REGISTERED?
 *
 *   expr_dark_probe <game_dir> <exe path>
 *
 * Of 2270 operator keys the expression corpus actually calls, 271 are named by
 * crc32be of a literal in the executable and 1130 carry a reflected typeinfo
 * descriptor. ZERO keys are in both, which is what proved those are two
 * separate registration mechanisms rather than one table with holes. 869 keys -
 * 42.9% of every operator USE in the corpus - reach neither, and the biggest
 * single one is called 66,464 times.
 *
 * Two routes are already measured dead and are not retried here: nine
 * candidate-generation schemes for the hash (only crc32be over whole literals
 * hits at all, and boundary suffixes gained exactly zero against a random-key
 * control), and every pointer field of the reflected descriptor at one and two
 * indirections against twelve hashes (no route round-trips the key).
 *
 * THIS ASKS A DIFFERENT QUESTION. The library already carries two registry
 * readers that were written for other work and never pointed at this set:
 * read_descriptor_operators, which scans self-referential 32-byte descriptor
 * records, and read_method_operators, which looks for compact MethodRegistry
 * records BY KEY. Before inventing a third theory, the honest move is to run
 * the instruments we already have against the keys we cannot explain.
 *
 * It also asks the most direct question available: does the key appear in the
 * executable AT ALL, as a little-endian 4-byte value? A key that is never
 * present as data is registered by something computed at run time; a key that
 * appears in a regular stride alongside a code pointer is a table, and the
 * stride tells us its shape. Those are opposite findings with opposite next
 * steps, so the count is worth having before any more theorising.
 */
#include "bf6_core.h"
#include "expression_graph.h"
#include "expression_registry.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: expr_dark_probe <game> <exe>\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string exe = argv[2];
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_all(c, 1, err, (int)sizeof(err));

    /* ---- the corpus keys, with their use counts ---- */
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
    /* Where does each key appear? A key used as a record's OPERATOR names an
     * engine function. A key that only ever appears in the FIXUP table may not
     * be an operator at all - the fixup table is what the loader patches, and
     * it is not proven that everything in it is a function reference. Counting
     * them apart is what makes that testable rather than assumed. */
    std::set<uint32_t> as_operator, as_fixup;
    std::map<uint32_t, long> uses;
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
                if (r.has_operator) { ++uses[r.operator_key]; as_operator.insert(r.operator_key); }
            for (const bf6::expression::Fixup& f : g.fixups) { ++uses[f.key]; as_fixup.insert(f.key); }
        }
    }
    bf6_close(c);

    /* ---- classify ---- */
    std::vector<uint32_t> all;
    for (const auto& kv : uses) all.push_back(kv.first);
    std::vector<bf6::expression::NamedOperator> named;
    std::string e;
    bf6::expression::resolve_named_operators(exe, all, named, e);
    std::set<uint32_t> has_name;
    for (const auto& n : named) if (!n.name.empty()) has_name.insert(n.key);

    std::set<uint32_t> dark;
    long dark_uses = 0, total_uses = 0;
    for (const auto& kv : uses) {
        total_uses += kv.second;
        if (has_name.count(kv.first) || arity.count(kv.first)) continue;
        dark.insert(kv.first);
        dark_uses += kv.second;
    }
    std::printf("corpus keys %zu   named %zu   reflected %zu   DARK %zu\n",
                uses.size(), has_name.size(), arity.size(), dark.size());
    std::printf("dark share of all operator uses: %.1f%%\n\n",
                total_uses ? 100.0 * (double)dark_uses / (double)total_uses : 0.0);
    if (dark.empty()) return 0;
    std::vector<uint32_t> dark_list(dark.begin(), dark.end());

    /* ---- instrument 1: the descriptor registry ---- */
    {
        std::vector<bf6::expression::DescriptorOperator> rows;
        std::string derr;
        if (!bf6::expression::read_descriptor_operators(exe, rows, derr)) {
            std::printf("descriptor registry unavailable: %s\n", derr.c_str());
        } else {
            std::set<uint32_t> have;
            for (const auto& r : rows) have.insert(r.key);
            long hit = 0;
            for (uint32_t k : dark_list) if (have.count(k)) ++hit;
            std::printf("descriptor registry: %zu record(s), explains %ld of %zu dark keys\n",
                        rows.size(), hit, dark_list.size());
        }
    }

    /* ---- instrument 2: the method registry, queried BY the dark keys ---- */
    {
        std::vector<bf6::expression::MethodOperator> rows;
        std::string merr;
        if (!bf6::expression::read_method_operators(exe, dark_list, rows, merr)) {
            std::printf("method registry unavailable: %s\n", merr.c_str());
        } else {
            std::set<uint32_t> have;
            for (const auto& r : rows) have.insert(r.key);
            long hit = 0;
            for (uint32_t k : dark_list) if (have.count(k)) ++hit;
            std::printf("method registry:     %zu record(s), explains %ld of %zu dark keys\n",
                        rows.size(), hit, dark_list.size());
        }
    }

    /* ---- instrument 3: is the key in the executable at all? ----
     *
     * THE CONTROL MATTERS. A 32-bit value occurs in a 199 MB file by chance
     * often enough that "found it" means nothing on its own: the expected count
     * for a random key is file_size/2^32 times 4 alignments, which is small but
     * not zero. So the same search runs over an equal number of keys that
     * CANNOT be real, and the two rates are printed side by side. */
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
    std::map<uint32_t, uint64_t> key_first_impl;
    std::map<uint32_t, std::vector<size_t>> at;
    std::set<uint32_t> want(dark_list.begin(), dark_list.end());
    // The control: keys that cannot be real, same count, fixed seed.
    std::set<uint32_t> control;
    {
        uint32_t x = 0x13579bdfu;
        while (control.size() < dark_list.size()) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            if (!uses.count(x)) control.insert(x);
        }
    }
    std::map<uint32_t, int> control_hits;
    for (size_t i = 0; i + 4 <= d.size(); ++i) {
        uint32_t v = 0;
        std::memcpy(&v, d.data() + i, 4);
        if (want.count(v)) at[v].push_back(i);
        else if (control.count(v)) ++control_hits[v];
    }
    long present = 0;
    for (const auto& kv : at) if (!kv.second.empty()) ++present;
    long control_present = 0;
    for (const auto& kv : control_hits) if (kv.second > 0) ++control_present;
    std::printf("\nkey bytes present in the executable:\n");
    std::printf("  DARK keys found   %ld of %zu\n", present, dark_list.size());
    std::printf("  control keys found %ld of %zu   <- the chance rate\n",
                control_present, control.size());

    /* If the dark keys really live in a table, the gaps between consecutive
     * occurrences repeat. Report the commonest stride so the shape is visible
     * rather than inferred. */
    std::map<size_t, long> stride;
    std::vector<size_t> flat;
    for (const auto& kv : at)
        for (size_t o : kv.second) flat.push_back(o);
    std::sort(flat.begin(), flat.end());
    for (size_t i = 1; i < flat.size(); ++i) {
        const size_t g = flat[i] - flat[i - 1];
        if (g > 0 && g <= 256) ++stride[g];
    }
    std::vector<std::pair<long, size_t>> top;
    for (const auto& kv : stride) top.emplace_back(kv.second, kv.first);
    std::sort(top.begin(), top.end(), std::greater<std::pair<long, size_t>>());
    std::printf("\ncommonest gaps between dark-key occurrences (a table shows up here):\n");
    for (size_t i = 0; i < top.size() && i < 8; ++i)
        std::printf("  %4zu bytes  x%ld\n", top[i].second, top[i].first);

    /* ---- instrument 4: THE KEY-FIRST DESCRIPTOR TABLE ----
     *
     * The stride histogram above says 32 bytes, and dumping the region around
     * the costliest dark keys shows why the existing descriptor reader misses
     * them: it expects `impl qword; key u32; flags u32; ...`, and these records
     * put the KEY FIRST:
     *
     *     +0   key    u32
     *     +4   flags  u32   (1 on every record seen)
     *     +8   slot   qword -> a data address
     *     +16  zero   qword
     *     +24  impl   qword -> EXECUTABLE code
     *
     * The impl pointer landing in an executable section is the load-bearing
     * check: it is what separates a real registration record from 32 bytes of
     * unrelated data that happens to start with a value we are looking for. */
    {
        const uint32_t pe = *(const uint32_t*)(d.data() + 0x3c);
        const uint16_t nsec = *(const uint16_t*)(d.data() + pe + 6);
        const uint16_t optsz = *(const uint16_t*)(d.data() + pe + 20);
        const size_t opt = (size_t)pe + 24;
        uint64_t base = 0;
        std::memcpy(&base, d.data() + opt + 24, 8);
        struct Sec { uint32_t va, vsz, raw, rsz, ch; };
        std::vector<Sec> secs;
        for (uint16_t i = 0; i < nsec; ++i) {
            const size_t a = opt + optsz + (size_t)i * 40;
            Sec s;
            std::memcpy(&s.vsz, d.data() + a + 8, 4);
            std::memcpy(&s.va, d.data() + a + 12, 4);
            std::memcpy(&s.rsz, d.data() + a + 16, 4);
            std::memcpy(&s.raw, d.data() + a + 20, 4);
            std::memcpy(&s.ch, d.data() + a + 36, 4);
            secs.push_back(s);
        }
        auto exec_va = [&](uint64_t va) {
            if (va < base) return false;
            const uint64_t rva = va - base;
            for (const Sec& s : secs) {
                if (!(s.ch & 0x20000000u)) continue;
                const uint64_t span = s.vsz > s.rsz ? s.vsz : s.rsz;
                if (rva >= s.va && rva < (uint64_t)s.va + span) return true;
            }
            return false;
        };
        auto any_va = [&](uint64_t va) {
            if (va < base) return false;
            const uint64_t rva = va - base;
            for (const Sec& s : secs) {
                const uint64_t span = s.vsz > s.rsz ? s.vsz : s.rsz;
                if (rva >= s.va && rva < (uint64_t)s.va + span) return true;
            }
            return false;
        };

        std::map<uint32_t, uint64_t> impl_of;
        long records = 0;
        for (size_t o = 0; o + 32 <= d.size(); o += 4) {
            uint32_t flags = 0;
            std::memcpy(&flags, d.data() + o + 4, 4);
            if (flags != 1) continue;
            uint64_t zero = 0, slot = 0, impl = 0;
            std::memcpy(&slot, d.data() + o + 8, 8);
            std::memcpy(&zero, d.data() + o + 16, 8);
            std::memcpy(&impl, d.data() + o + 24, 8);
            if (zero != 0 || !any_va(slot) || !exec_va(impl)) continue;
            uint32_t key = 0;
            std::memcpy(&key, d.data() + o, 4);
            ++records;
            if (want.count(key) && !impl_of.count(key)) impl_of[key] = impl;
        }
        std::printf("\nkey-first descriptor table: %ld record(s) matching the shape\n", records);
        std::printf("  explains %zu of %zu dark keys\n", impl_of.size(), dark_list.size());

        // Control: the same shape scan, asked for keys that cannot be real.
        long ctrl = 0;
        for (size_t o = 0; o + 32 <= d.size(); o += 4) {
            uint32_t flags = 0;
            std::memcpy(&flags, d.data() + o + 4, 4);
            if (flags != 1) continue;
            uint64_t zero = 0, slot = 0, impl = 0;
            std::memcpy(&slot, d.data() + o + 8, 8);
            std::memcpy(&zero, d.data() + o + 16, 8);
            std::memcpy(&impl, d.data() + o + 24, 8);
            if (zero != 0 || !any_va(slot) || !exec_va(impl)) continue;
            uint32_t key = 0;
            std::memcpy(&key, d.data() + o, 4);
            if (control.count(key)) ++ctrl;
        }
        std::printf("  control keys in the same table: %ld   <- chance rate\n", ctrl);

        /* Two keys sharing an implementation ARE the same operator, which is
         * worth more than a name for reading a program. */
        std::map<uint64_t, int> shared;
        for (const auto& kv : impl_of) ++shared[kv.second];
        long distinct = (long)shared.size(), aliased = 0;
        for (const auto& kv : shared) if (kv.second > 1) aliased += kv.second;
        std::printf("  %ld distinct implementations; %ld keys share one with another key\n",
                    distinct, aliased);
    }

    /* ---- WHAT DO THE THREE INSTRUMENTS EXPLAIN TOGETHER? ----
     *
     * Adding their individual counts would double-count: a key can sit in more
     * than one table. The union is the only honest coverage number, and the
     * remainder is the part of the system still genuinely unaccounted for. */
    {
        std::set<uint32_t> explained;
        {
            std::vector<bf6::expression::DescriptorOperator> rows;
            std::string derr;
            if (bf6::expression::read_descriptor_operators(exe, rows, derr))
                for (const auto& r : rows) if (want.count(r.key)) explained.insert(r.key);
        }
        {
            std::vector<bf6::expression::MethodOperator> rows;
            std::string merr;
            if (bf6::expression::read_method_operators(exe, dark_list, rows, merr))
                for (const auto& r : rows) if (want.count(r.key)) explained.insert(r.key);
        }
        for (const auto& kv : key_first_impl) explained.insert(kv.first);
        long explained_uses = 0, remaining_uses = 0;
        for (uint32_t k : dark_list)
            (explained.count(k) ? explained_uses : remaining_uses) += uses[k];
        std::printf("\nUNION of all three instruments: %zu of %zu dark keys\n",
                    explained.size(), dark_list.size());
        std::printf("  still unexplained: %zu keys, %.1f%% of all operator uses\n",
                    dark_list.size() - explained.size(),
                    total_uses ? 100.0 * (double)remaining_uses / (double)total_uses : 0.0);
    }

    /* ---- the split that decides what the rest of the dark set IS ----
     *
     * Only a minority of dark keys occur in the executable at all. A key that
     * is never in the binary cannot be registered by any static table there, so
     * either it is computed at run time or it is not an engine function
     * reference in the first place. Splitting by HOW the key is used, against
     * whether its bytes exist, separates those two stories. */
    {
        long op_only = 0, fx_only = 0, both = 0;
        long op_only_present = 0, fx_only_present = 0, both_present = 0;
        for (uint32_t k : dark_list) {
            const bool o = as_operator.count(k) != 0;
            const bool f = as_fixup.count(k) != 0;
            const bool present = at.count(k) && !at[k].empty();
            if (o && f) { ++both; both_present += present ? 1 : 0; }
            else if (o) { ++op_only; op_only_present += present ? 1 : 0; }
            else if (f) { ++fx_only; fx_only_present += present ? 1 : 0; }
        }
        std::printf("\nhow the dark keys are USED, against whether the exe contains them:\n");
        std::printf("  %-28s %6s %10s\n", "", "keys", "in the exe");
        std::printf("  %-28s %6ld %10ld\n", "record operator AND fixup", both, both_present);
        std::printf("  %-28s %6ld %10ld\n", "record operator only", op_only, op_only_present);
        std::printf("  %-28s %6ld %10ld\n", "FIXUP ONLY", fx_only, fx_only_present);
    }

    std::printf("\nthe 10 costliest dark keys and where their bytes appear:\n");
    std::vector<std::pair<long, uint32_t>> worst;
    for (uint32_t k : dark_list) worst.emplace_back(uses[k], k);
    std::sort(worst.begin(), worst.end(), std::greater<std::pair<long, uint32_t>>());
    for (size_t i = 0; i < worst.size() && i < 10; ++i) {
        const uint32_t k = worst[i].second;
        std::printf("  0x%08x  %7ld uses  %zu occurrence(s)", k, worst[i].first, at[k].size());
        for (size_t j = 0; j < at[k].size() && j < 3; ++j)
            std::printf("  @0x%zx", at[k][j]);
        std::printf("\n");
    }
    return 0;
}
