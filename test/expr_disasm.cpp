/* DISASSEMBLE A DICEEXPRESSION GRAPH, WITH ITS OPERATORS NAMED.
 *
 *   expr_disasm <game_dir> <res name> [exe path]
 *
 * BF6's soldier movement logic is not in a table of numbers. It is in
 * DiceExpression graphs - mm.onground.expression.diceexp and its siblings - and
 * the surrounding MotionMachine layers only route between them. Reading those
 * graphs is the difference between knowing the game's thresholds and knowing
 * what the game DOES with them.
 *
 * The library already parses the graph (bf6::expression::parse) and can name
 * operator keys from the executable's own registry
 * (bf6::expression::resolve_named_operators). This puts the two together and
 * prints the program: records in order, each with its operator name where the
 * exe can supply one, its operands, and its control flow.
 *
 * AN UNNAMED OPERATOR IS PRINTED AS ITS RAW KEY, never guessed at. The registry
 * resolves a name only when the match is unique, and a plausible-looking guess
 * is exactly how a decode becomes fiction.
 */
#include "bf6_core.h"
#include "expression_graph.h"
#include "expression_registry.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: expr_disasm <game> <res name> [exe]\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string res = argv[2];
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_all(c, 1, err, (int)sizeof(err));

    /* "find:<substring>" lists expression RESOURCES rather than disassembling
     * one. The EBX partition name and the RES name are not the same string -
     * mm.onground.expression.diceexp is an EBX stub with no visible ResRef -
     * so the resource has to be located by listing, not by guessing a spelling. */
    if (res.rfind("find:", 0) == 0) {
        const std::string want = res.substr(5);
        const int total = bf6_list_res(c, nullptr, nullptr, 0);
        std::vector<bf6_asset> rows((size_t)(total > 0 ? total : 0));
        const int got = total > 0 ? bf6_list_res(c, nullptr, rows.data(), total) : 0;
        int shown = 0;
        for (int i = 0; i < got; ++i) {
            if (rows[(size_t)i].type != 0x7dd4cc89u || !rows[(size_t)i].name) continue;
            if (!want.empty() && !std::strstr(rows[(size_t)i].name, want.c_str())) continue;
            std::printf("%s\n", rows[(size_t)i].name);
            if (++shown >= 200) { std::printf("... (stopped at 200)\n"); break; }
        }
        std::printf("%d expression resource(s) matched \"%s\" of %d resources\n",
                    shown, want.c_str(), got);
        bf6_close(c);
        return 0;
    }

    /* "k23:<substring>" hunts the LENGTH RULE for record kind 0x23.
     *
     * Only two kinds lack a fixed length. 0x28 is already solved - its length
     * is 16 + count*8 with the count at +12. 0x23 is not: the parser tries
     * every length from 24 to 232 in 8-byte steps and gives up on a file when
     * more than one tiling fits, which is the sole reason seven traversal
     * graphs (the master among them) read as INCOMPLETE.
     *
     * Where the tiling IS unique the chosen length is known to be right. So
     * print those alongside the record's leading dwords and look for the field
     * that predicts it - the same shape of rule 0x28 turned out to have. */
    if (res.rfind("k23:", 0) == 0) {
        const std::string want = res.substr(4);
        const int total = bf6_list_res(c, nullptr, nullptr, 0);
        std::vector<bf6_asset> rows((size_t)(total > 0 ? total : 0));
        const int got = total > 0 ? bf6_list_res(c, nullptr, rows.data(), total) : 0;
        std::printf("%6s  %s\n", "len", "leading dwords of the 0x23 record");
        std::map<uint32_t, long> by_len;
        long samples = 0, agree = 0, disagree = 0;
        std::map<uint32_t, std::map<std::pair<uint32_t,uint32_t>, long>> combos;
        std::map<std::pair<uint32_t,uint32_t>, long> bad_shape;
        long rule_ok = 0, rule_bad = 0;
        std::map<uint32_t, std::set<uint32_t>> by_op;
        /* THE PRINCIPLED ROUTE. The executable's reflected registry carries a
         * parameter_count per operator. If 0x23's length is header + operands,
         * that count IS the rule - read from the game rather than fitted to a
         * corpus. This loads it so the correlation can be scored directly. */
        std::map<uint32_t, uint16_t> arity;
        if (argc > 3) {
            const int n0 = bf6_expression_reflected_operators(argv[3], nullptr, 0, nullptr, 0);
            if (n0 > 0) {
                std::vector<bf6_expression_reflected_operator> refl((size_t)n0);
                char rerr[256] = {0};
                const int gotr = bf6_expression_reflected_operators(argv[3], refl.data(), n0,
                                                                    rerr, (int)sizeof(rerr));
                for (int k = 0; k < gotr; ++k) arity[refl[(size_t)k].key] = refl[(size_t)k].parameter_count;
                std::printf("executable reflected %d operator(s) with parameter counts\n", gotr);
            }
        }
        std::map<int, std::map<uint32_t, long>> len_by_arity;
        for (int i = 0; i < got && samples < 100000; ++i) {
            if (rows[(size_t)i].type != 0x7dd4cc89u || !rows[(size_t)i].name) continue;
            if (!want.empty() && !std::strstr(rows[(size_t)i].name, want.c_str())) continue;
            const uint8_t* d = nullptr;
            const int64_t n = bf6_read_raw(c, BF6_RAW_RES, rows[(size_t)i].name, &d);
            if (n <= 0 || !d) continue;
            bf6::expression::Graph gg;
            std::string e2;
            if (!bf6::expression::parse(d, (size_t)n, gg, e2)) continue;
            /* Only files whose tiling is unique - elsewhere the length is a
             * guess and would poison the rule being looked for. */
            if (!gg.exact_record_tiling) continue;
            for (const bf6::expression::Record& r : gg.records) {
                if (r.kind != 0x23) continue;
                ++by_len[r.byte_length];
                 
                const uint8_t* p = d + gg.region_base + r.offset;
                uint32_t d0 = 0;
                std::memcpy(&d0, p, 4);
                /* Every leading dword ends in the kind byte, so the high 24
                 * bits are something else. If they are the offset of the NEXT
                 * record, the length is explicit and the whole 24..232 search
                 * is unnecessary. `pred` is that reading; it should equal the
                 * length the tiling proved. */
                const uint32_t hi = d0 >> 8;
                const long pred = (long)hi - (long)r.offset;
                if (pred == (long)r.byte_length) ++agree; else ++disagree;
                /* WHAT ELSE DOES THE LENGTH TRACK? The header pointer is a
                 * control-flow next and coincides with the length only when the
                 * record falls through, so the length must also be derivable
                 * from the record's own fields. Tabulate every leading dword
                 * against the proven length and let the correlation show. */
                uint32_t w[6] = {0, 0, 0, 0, 0, 0};
                for (int k = 0; k < 6 && (uint32_t)(k * 4 + 4) <= r.byte_length; ++k)
                    std::memcpy(&w[k], p + k * 4, 4);
                combos[r.byte_length][std::make_pair(w[3], w[4])]++;
                /* TWO CLEAN FAMILIES show in the correlation table:
                 *   dword3 == 0  ->  len = 24 + dword4 * 8
                 *   dword4 == 2  ->  len = 24 + dword3 * 8
                 * Both are exact wherever they apply. This scores the combined
                 * rule against every proven length so the exceptions are a
                 * number rather than an impression. */
                long rule = -1;
                if (w[3] == 0) rule = 24 + (long)w[4] * 8;
                else rule = 24 + (long)w[3] * 8;
                if (rule == (long)r.byte_length) ++rule_ok;
                else {
                    ++rule_bad;
                    bad_shape[std::make_pair(w[3], w[4])]++;
                }
                /* THE REAL CANDIDATE. 0x23 carries an OPERATOR POINTER
                 * (has_operator_pointer covers it), so its length is probably
                 * header + operands, and the operand count is the operator's
                 * arity - which lives in the executable's registry, not in the
                 * record. That would explain why no self-contained field
                 * predicts the length. If it holds, every record sharing an
                 * operator key shares a length. */
                if (r.has_operator) by_op[r.operator_key].insert(r.byte_length);
                if (r.has_operator) {
                    auto ai = arity.find(r.operator_key);
                    if (ai != arity.end()) len_by_arity[(int)ai->second][r.byte_length]++;
                }
                ++samples;
            }
        }
        std::printf("\n0x23 length from the header's own next-pointer: "
                    "%ld agree, %ld disagree, of %ld record(s) in uniquely tiled files\n",
                    agree, disagree, samples);
        std::printf("\nrule  d3==0 ? 24+d4*8 : 24+d3*8   ->  %ld correct, %ld wrong (%.1f%%)\n",
                    rule_ok, rule_bad,
                    100.0 * (double)rule_ok / (double)((rule_ok + rule_bad) ? rule_ok + rule_bad : 1));
        {
            long one = 0, many = 0;
            size_t worst = 0;
            for (const auto& O : by_op) {
                if (O.second.size() == 1) ++one;
                else { ++many; if (O.second.size() > worst) worst = O.second.size(); }
            }
            std::printf("operator key -> length: %ld key(s) map to ONE length, "
                        "%ld to several (worst %zu)\n", one, many, worst);
        }
        std::printf("\nproven length by the operator's reflected parameter_count:\n");
        for (const auto& A : len_by_arity) {
            std::printf("  arity %3d:", A.first);
            int s4 = 0;
            for (const auto& L2 : A.second) {
                std::printf("  len %u x%ld", L2.first, L2.second);
                if (++s4 >= 5) { std::printf("  ..."); break; }
            }
            std::printf("\n");
        }
        std::printf("worst offending shapes:\n");
        {
            int s3 = 0;
            for (const auto& B : bad_shape) {
                std::printf("   (d3=%u,d4=%u) x%ld\n", B.first.first, B.first.second, B.second);
                if (++s3 >= 8) break;
            }
        }
        std::printf("\nproven length vs (dword3, dword4):\n");
        for (const auto& L : combos) {
            std::printf("  len %4u:", L.first);
            int shown2 = 0;
            for (const auto& C : L.second) {
                std::printf("  (%u,%u)x%ld", C.first.first, C.first.second, C.second);
                if (++shown2 >= 6) { std::printf("  ..."); break; }
            }
            std::printf("\n");
        }
        std::printf("\nlength distribution:\n");
        for (const auto& kv : by_len) std::printf("   %4u bytes  x%ld\n", kv.first, kv.second);
        bf6_close(c);
        return 0;
    }

    /* "ambig:" asks what the still-ambiguous graphs actually contain.
     *
     * Only two kinds are variable-length in the tiler now: 0x28, whose count is
     * in the record and so is deterministic, and 0x23, which is pinned when the
     * executable knows its operator's arity and SEARCHED when it does not. If
     * ambiguity is concentrated in 0x23 records with no reflected arity, the
     * fix is a second source of arity rather than a cleverer solver. */
    if (res.rfind("ambig:", 0) == 0) {
        const std::string exe4 = argc > 3 ? argv[3] : std::string();
        bf6::expression::ArityMap arity4;
        if (!exe4.empty()) {
            const int n0 = bf6_expression_reflected_operators(exe4.c_str(), nullptr, 0, nullptr, 0);
            if (n0 > 0) {
                std::vector<bf6_expression_reflected_operator> refl((size_t)n0);
                char rerr[256] = {0};
                const int gotr = bf6_expression_reflected_operators(exe4.c_str(), refl.data(), n0,
                                                                    rerr, (int)sizeof(rerr));
                for (int k = 0; k < gotr; ++k)
                    arity4[refl[(size_t)k].key] = refl[(size_t)k].parameter_count;
            }
        }
        const int total = bf6_list_res(c, nullptr, nullptr, 0);
        std::vector<bf6_asset> rows((size_t)(total > 0 ? total : 0));
        const int got = total > 0 ? bf6_list_res(c, nullptr, rows.data(), total) : 0;
        long ambiguous = 0, none = 0;
        long k23_total = 0, k23_no_arity = 0, k23_no_key = 0;
        std::set<uint32_t> missing_keys;
        for (int i = 0; i < got; ++i) {
            if (rows[(size_t)i].type != 0x7dd4cc89u || !rows[(size_t)i].name) continue;
            const uint8_t* d = nullptr;
            const int64_t n = bf6_read_raw(c, BF6_RAW_RES, rows[(size_t)i].name, &d);
            if (n <= 0 || !d) continue;
            bf6::expression::Graph gg;
            std::string e2;
            if (!bf6::expression::parse(d, (size_t)n, gg, e2,
                                        arity4.empty() ? nullptr : &arity4)) continue;
            if (gg.exact_record_tiling) continue;
            if (gg.tiling_ways == 0) { ++none; continue; }
            ++ambiguous;
            for (const bf6::expression::Record& r : gg.records) {
                if (r.kind != 0x23) continue;
                ++k23_total;
                if (!r.operator_key) { ++k23_no_key; continue; }
                if (arity4.find(r.operator_key) == arity4.end()) {
                    ++k23_no_arity;
                    missing_keys.insert(r.operator_key);
                }
            }
        }
        /* CAN THE GAP BE CLOSED FROM THE CORPUS ITSELF? A key the executable
         * does not reflect may still appear in a graph that DOES tile uniquely,
         * where its length is proven. A caller may derive an ArityMap that way
         * and hand it to parse - the library still consumes no table of its own,
         * which is the promise that matters. This counts how many of the
         * missing keys are recoverable like that. */
        std::map<uint32_t, std::set<uint32_t>> proven_len;
        for (int i = 0; i < got; ++i) {
            if (rows[(size_t)i].type != 0x7dd4cc89u || !rows[(size_t)i].name) continue;
            const uint8_t* d = nullptr;
            const int64_t n = bf6_read_raw(c, BF6_RAW_RES, rows[(size_t)i].name, &d);
            if (n <= 0 || !d) continue;
            bf6::expression::Graph gg;
            std::string e2;
            if (!bf6::expression::parse(d, (size_t)n, gg, e2,
                                        arity4.empty() ? nullptr : &arity4)) continue;
            if (!gg.exact_record_tiling) continue;
            for (const bf6::expression::Record& r : gg.records)
                if (r.kind == 0x23 && r.operator_key &&
                    missing_keys.count(r.operator_key))
                    proven_len[r.operator_key].insert(r.byte_length);
        }
        long recoverable = 0, conflicting = 0;
        for (const auto& P : proven_len) {
            if (P.second.size() == 1) ++recoverable; else ++conflicting;
        }
        std::printf("of the missing keys, %ld have ONE proven length elsewhere, "
                    "%ld conflict, %zu never appear in a tiled graph\n",
                    recoverable, conflicting,
                    missing_keys.size() - proven_len.size());
        std::printf("%ld ambiguous graph(s), %ld with no tiling at all\n", ambiguous, none);
        std::printf("inside the ambiguous ones: %ld record(s) of kind 0x23\n", k23_total);
        std::printf("   %ld have no operator key at all\n", k23_no_key);
        std::printf("   %ld have a key the executable does not reflect (%zu distinct)\n",
                    k23_no_arity, missing_keys.size());
        bf6_close(c);
        return 0;
    }

    /* "regions:" measures how large an operand's REGION field legitimately gets.
     *
     * 201 graphs still tile ambiguously: more than one tiling fits, so a
     * constraint is missing. Operands are (region, offset) pairs read out of the
     * record body, and a WRONG length shifts that body and manufactures
     * nonsense - so "every implied operand has a plausible region" is a filter
     * the tiler could apply. It is only usable if legitimate regions are small,
     * which is what this measures rather than assumes. */
    if (res.rfind("regions:", 0) == 0) {
        const int total = bf6_list_res(c, nullptr, nullptr, 0);
        std::vector<bf6_asset> rows((size_t)(total > 0 ? total : 0));
        const int got = total > 0 ? bf6_list_res(c, nullptr, rows.data(), total) : 0;
        std::map<uint32_t, long> region_hist;
        long operands = 0;
        uint32_t biggest = 0;
        for (int i = 0; i < got; ++i) {
            if (rows[(size_t)i].type != 0x7dd4cc89u || !rows[(size_t)i].name) continue;
            const uint8_t* d = nullptr;
            const int64_t n = bf6_read_raw(c, BF6_RAW_RES, rows[(size_t)i].name, &d);
            if (n <= 0 || !d) continue;
            bf6::expression::Graph gg;
            std::string e2;
            if (!bf6::expression::parse(d, (size_t)n, gg, e2)) continue;
            /* Only graphs whose tiling is unique: elsewhere the operands are
             * themselves a product of a guessed length. */
            if (!gg.exact_record_tiling) continue;
            for (const bf6::expression::Record& r : gg.records)
                for (const bf6::expression::Operand& o : r.operands) {
                    ++operands;
                    region_hist[o.region < 16 ? o.region : 999]++;
                    if (o.region > biggest && o.region != 999) biggest = o.region;
                }
        }
        std::printf("%ld operand(s) in uniquely tiled graphs; largest region %u\n",
                    operands, biggest);
        for (const auto& R : region_hist)
            std::printf("   region %-4u x%ld\n", R.first, R.second);
        bf6_close(c);
        return 0;
    }

    /* "klen:<kind hex>" measures the LENGTH RULE for any record kind.
     *
     * The kinds still blocking the corpus (0x14, 0x16, 0x17, 0x1A, 0x1F, 0x33,
     * 0x37, 0x3C) appear ONLY in graphs that fail to tile, so there are no
     * proven lengths for them - the usual ground truth is missing.
     *
     * There is another one. When tiling fails the parser falls back to the
     * DISCOVERED record starts (control-flow targets and fixup sites), and a
     * record's reported length becomes the gap to the next discovered start.
     * A gap can SPAN several records when discovery missed one, so it is an
     * upper bound - but never an under-estimate. So the MINIMUM gap observed
     * for a given arity is the candidate length, and a rule that holds at the
     * minimum across many samples is worth trusting.
     *
     * Reports min/median/count per arity so `base + arity * 8` can be read off
     * directly, the way 0x23 gave up 24 + arity * 8. */
    if (res.rfind("klen:", 0) == 0) {
        const unsigned want_kind = (unsigned)std::strtoul(res.substr(5).c_str(), nullptr, 16);
        const std::string exe3 = argc > 3 ? argv[3] : std::string();
        bf6::expression::ArityMap arity3;
        if (!exe3.empty()) {
            const int n0 = bf6_expression_reflected_operators(exe3.c_str(), nullptr, 0, nullptr, 0);
            if (n0 > 0) {
                std::vector<bf6_expression_reflected_operator> refl((size_t)n0);
                char rerr[256] = {0};
                const int gotr = bf6_expression_reflected_operators(exe3.c_str(), refl.data(), n0,
                                                                    rerr, (int)sizeof(rerr));
                for (int k = 0; k < gotr; ++k)
                    arity3[refl[(size_t)k].key] = refl[(size_t)k].parameter_count;
            }
        }
        const int total = bf6_list_res(c, nullptr, nullptr, 0);
        std::vector<bf6_asset> rows((size_t)(total > 0 ? total : 0));
        const int got = total > 0 ? bf6_list_res(c, nullptr, rows.data(), total) : 0;
        std::map<int, std::vector<uint32_t>> by_arity;   // arity -> observed gaps
        std::vector<uint32_t> no_arity;
        long seen_records = 0, in_files = 0;
        for (int i = 0; i < got; ++i) {
            if (rows[(size_t)i].type != 0x7dd4cc89u || !rows[(size_t)i].name) continue;
            const uint8_t* d = nullptr;
            const int64_t n = bf6_read_raw(c, BF6_RAW_RES, rows[(size_t)i].name, &d);
            if (n <= 0 || !d) continue;
            bf6::expression::Graph gg;
            std::string e2;
            if (!bf6::expression::parse(d, (size_t)n, gg, e2,
                                        arity3.empty() ? nullptr : &arity3)) continue;
            bool had = false;
            for (const bf6::expression::Record& r : gg.records) {
                if (r.kind != want_kind) continue;
                had = true;
                ++seen_records;
                if (r.has_operator && r.operator_key) {
                    auto a = arity3.find(r.operator_key);
                    if (a != arity3.end()) { by_arity[(int)a->second].push_back(r.byte_length); continue; }
                }
                no_arity.push_back(r.byte_length);
            }
            if (had) ++in_files;
        }
        std::printf("kind 0x%02x: %ld record(s) across %ld graph(s)\n",
                    want_kind, seen_records, in_files);
        std::printf("%6s %8s %8s %8s   %s\n", "arity", "count", "min", "median",
                    "min - 8*arity");
        for (auto& A : by_arity) {
            std::sort(A.second.begin(), A.second.end());
            const uint32_t mn = A.second.front();
            const uint32_t md = A.second[A.second.size() / 2];
            std::printf("%6d %8zu %8u %8u   %ld\n", A.first, A.second.size(), mn, md,
                        (long)mn - 8L * (long)A.first);
        }
        if (!no_arity.empty()) {
            std::sort(no_arity.begin(), no_arity.end());
            std::printf("no reflected arity: %zu record(s), min %u, median %u\n",
                        no_arity.size(), no_arity.front(), no_arity[no_arity.size() / 2]);
        }
        bf6_close(c);
        return 0;
    }

    /* "kinds:<substring>" censuses RECORD KINDS across a subsystem: how often
     * each appears, whether it has a proven byte length, and which expressions
     * contain the unmeasured ones. Seven of the thirty-five traversal graphs do
     * not tile exactly - including the master - and a tiling gap means part of
     * a program is simply not being read. This says which kinds to go and
     * measure rather than leaving "INCOMPLETE" as a shrug. */
    if (res.rfind("kinds:", 0) == 0) {
        const std::string want = res.substr(6);
        const int total = bf6_list_res(c, nullptr, nullptr, 0);
        std::vector<bf6_asset> rows((size_t)(total > 0 ? total : 0));
        const int got = total > 0 ? bf6_list_res(c, nullptr, rows.data(), total) : 0;
        std::map<uint8_t, long> seen;
        std::map<uint8_t, std::set<std::string>> unmeasured_in;
        for (int i = 0; i < got; ++i) {
            if (rows[(size_t)i].type != 0x7dd4cc89u || !rows[(size_t)i].name) continue;
            if (!want.empty() && !std::strstr(rows[(size_t)i].name, want.c_str())) continue;
            const uint8_t* d = nullptr;
            const int64_t n = bf6_read_raw(c, BF6_RAW_RES, rows[(size_t)i].name, &d);
            if (n <= 0 || !d) continue;
            bf6::expression::Graph gg;
            std::string e2;
            if (!bf6::expression::parse(d, (size_t)n, gg, e2)) continue;
            const char* tail2 = std::strrchr(rows[(size_t)i].name, '/');
            for (const bf6::expression::Record& r : gg.records) {
                ++seen[r.kind];
                if (bf6::expression::proven_record_length(r.kind) == 0)
                    unmeasured_in[r.kind].insert(tail2 ? tail2 + 1 : rows[(size_t)i].name);
            }
        }
        std::printf("%-6s %10s %8s  %s\n", "kind", "count", "length", "where it is unmeasured");
        for (const auto& kv : seen) {
            const uint32_t len = bf6::expression::proven_record_length(kv.first);
            std::printf("0x%02x   %10ld %8s", kv.first, kv.second,
                        len ? std::to_string(len).c_str() : "VARIABLE");
            auto it = unmeasured_in.find(kv.first);
            if (it != unmeasured_in.end()) {
                int shown = 0;
                for (const std::string& s : it->second) {
                    std::printf("%s%s", shown ? ", " : "  ", s.c_str());
                    if (++shown >= 4) { std::printf(", ..."); break; }
                }
            }
            std::printf("\n");
        }
        bf6_close(c);
        return 0;
    }

    /* "names:[tsv path]" is the corpus-wide naming census. survey: resolves
     * per graph, which re-reads the 199 MB executable once per file and cannot
     * tell a key named in one graph from the same key unnamed in another. This
     * gathers every distinct key in the corpus FIRST and asks the registry once,
     * so the number it prints is a property of the corpus, not of a file.
     *
     * The three outcomes are kept apart on purpose. "Unresolved" means no
     * literal in the executable hashes to the key; "ambiguous" means several do
     * and the registry correctly refused to choose. They are different problems:
     * the first needs a wider candidate set, the second needs a tighter one. */
    if (res.rfind("names:", 0) == 0) {
        const std::string tsv = res.substr(6);
        const int total = bf6_list_res(c, nullptr, nullptr, 0);
        std::vector<bf6_asset> rows((size_t)(total > 0 ? total : 0));
        const int got = total > 0 ? bf6_list_res(c, nullptr, rows.data(), total) : 0;
        const std::string exe2 = argc > 3 ? argv[3] : std::string();
        if (exe2.empty()) {
            std::printf("names: needs the executable path as the third argument\n");
            bf6_close(c);
            return 2;
        }
        bf6::expression::ArityMap arity2;
        {
            const int n0 = bf6_expression_reflected_operators(exe2.c_str(), nullptr, 0, nullptr, 0);
            if (n0 > 0) {
                std::vector<bf6_expression_reflected_operator> refl((size_t)n0);
                char rerr[256] = {0};
                const int gotr = bf6_expression_reflected_operators(exe2.c_str(), refl.data(), n0,
                                                                    rerr, (int)sizeof(rerr));
                for (int k = 0; k < gotr; ++k)
                    arity2[refl[(size_t)k].key] = refl[(size_t)k].parameter_count;
            }
        }
        /* use_count is how many records reference the key, not how many graphs.
         * A key used 40,000 times that we cannot name costs far more than a key
         * used once, so the census ranks the unresolved by weight. */
        std::map<uint32_t, long> use_count;
        std::map<uint32_t, std::set<std::string>> used_in;
        int files = 0;
        for (int i = 0; i < got; ++i) {
            if (rows[(size_t)i].type != 0x7dd4cc89u || !rows[(size_t)i].name) continue;
            const uint8_t* d = nullptr;
            const int64_t n = bf6_read_raw(c, BF6_RAW_RES, rows[(size_t)i].name, &d);
            if (n <= 0 || !d) continue;
            bf6::expression::Graph gg;
            std::string e2;
            if (!bf6::expression::parse(d, (size_t)n, gg, e2,
                                        arity2.empty() ? nullptr : &arity2)) continue;
            ++files;
            const char* tail2 = std::strrchr(rows[(size_t)i].name, '/');
            const std::string where = tail2 ? tail2 + 1 : rows[(size_t)i].name;
            for (const bf6::expression::Record& r : gg.records) {
                if (!r.has_operator) continue;
                ++use_count[r.operator_key];
                if (used_in[r.operator_key].size() < 4) used_in[r.operator_key].insert(where);
            }
            for (const bf6::expression::Fixup& f : gg.fixups) {
                ++use_count[f.key];
                if (used_in[f.key].size() < 4) used_in[f.key].insert(where);
            }
        }
        std::vector<uint32_t> kl;
        for (const auto& kv : use_count) kl.push_back(kv.first);
        std::vector<bf6::expression::NamedOperator> ops;
        std::string re2;
        if (!bf6::expression::resolve_named_operators(exe2, kl, ops, re2)) {
            std::printf("registry: %s\n", re2.c_str());
            bf6_close(c);
            return 1;
        }
        std::map<uint32_t, const bf6::expression::NamedOperator*> by_key;
        for (const bf6::expression::NamedOperator& o : ops) by_key[o.key] = &o;

        /* The second namespace. These keys have no hashable name but do have a
         * reflected owner and argument list, so they are not "unknown" in the
         * way an unresolved key with nothing at all behind it is. */
        std::map<uint32_t, std::string> sig;
        {
            std::vector<bf6::expression::ReflectedOperator> refl;
            std::string rerr;
            if (bf6::expression::read_reflected_operators(exe2, refl, rerr)) {
                std::set<uint32_t> want(kl.begin(), kl.end());
                for (const bf6::expression::ReflectedOperator& r : refl) {
                    if (want.find(r.key) == want.end()) continue;
                    if (r.name_space.empty() && r.parameter_names.empty()) continue;
                    std::string s = r.name_space.empty() ? std::string("?") : r.name_space;
                    s += "(";
                    for (size_t p = 0; p < r.parameter_names.size(); ++p) {
                        if (p) s += ", ";
                        s += r.parameter_names[p];
                    }
                    s += ")";
                    sig[r.key] = s;
                }
            }
        }

        /* The third registry: no name, but the implementation address, which
         * is what separates "we cannot read this" from "we have not named it". */
        std::map<uint32_t, uint64_t> impl;
        {
            std::set<uint32_t> want(kl.begin(), kl.end());
            std::vector<bf6::expression::KeyFirstOperator> rows;
            std::string kerr;
            if (bf6::expression::read_key_first_operators(exe2, rows, kerr))
                for (const auto& r : rows)
                    if (want.count(r.key)) impl[r.key] = r.implementation_va;
            /* The other two registries carry an implementation address too, so
             * a census that consulted only the key-first table would report a
             * key as unread when the library can already place it. */
            std::vector<bf6::expression::DescriptorOperator> drows;
            std::string derr;
            if (bf6::expression::read_descriptor_operators(exe2, drows, derr))
                for (const auto& r : drows)
                    if (want.count(r.key) && !impl.count(r.key))
                        impl[r.key] = r.implementation_va;
            std::vector<bf6::expression::MethodOperator> mrows;
            std::string merr;
            if (bf6::expression::read_method_operators(exe2, kl, mrows, merr))
                for (const auto& r : mrows)
                    if (want.count(r.key) && !impl.count(r.key))
                        impl[r.key] = r.implementation_va;
        }

        long named = 0, ambiguous = 0, unresolved = 0;
        long named_uses = 0, ambiguous_uses = 0, unresolved_uses = 0;
        std::vector<std::pair<long, uint32_t>> worst;
        FILE* out_tsv = tsv.empty() ? nullptr : std::fopen(tsv.c_str(), "wb");
        if (out_tsv)
            std::fprintf(out_tsv, "key\tstatus\tname\tarity\tuses\tsignature\texample\n");
        for (const auto& kv : use_count) {
            const auto it = by_key.find(kv.first);
            const char* status = "UNRESOLVED";
            std::string nm;
            if (it != by_key.end() && !it->second->name.empty()) {
                status = "named"; nm = it->second->name;
                ++named; named_uses += kv.second;
            } else if (it != by_key.end() && it->second->match_count > 1) {
                status = "AMBIGUOUS";
                ++ambiguous; ambiguous_uses += kv.second;
                worst.emplace_back(kv.second, kv.first);
            } else {
                ++unresolved; unresolved_uses += kv.second;
                worst.emplace_back(kv.second, kv.first);
            }
            const auto sg = sig.find(kv.first);
            if (out_tsv) {
                const auto ar = arity2.find(kv.first);
                std::fprintf(out_tsv, "0x%08x\t%s\t%s\t%s\t%ld\t%s\t%s\n", kv.first, status,
                             nm.c_str(),
                             ar == arity2.end() ? "" : std::to_string(ar->second).c_str(),
                             kv.second,
                             sg == sig.end() ? "" : sg->second.c_str(),
                             used_in[kv.first].empty() ? "" : used_in[kv.first].begin()->c_str());
            }
        }
        if (out_tsv) std::fclose(out_tsv);
        const long keys = (long)use_count.size();
        std::printf("%d expression(s); %ld distinct operator key(s)\n", files, keys);
        std::printf("  named      %6ld  (%4.1f%% of keys, %4.1f%% of uses)\n", named,
                    keys ? 100.0 * (double)named / (double)keys : 0.0,
                    (named_uses + ambiguous_uses + unresolved_uses)
                        ? 100.0 * (double)named_uses
                              / (double)(named_uses + ambiguous_uses + unresolved_uses) : 0.0);
        std::printf("  AMBIGUOUS  %6ld  (several literals hash to it; withheld)\n", ambiguous);
        std::printf("  UNRESOLVED %6ld  (no literal in the exe hashes to it)\n", unresolved);
        {
            long sig_keys = 0, sig_uses = 0, sig_and_named = 0, dark = 0, dark_uses = 0;
            for (const auto& kv : use_count) {
                const bool has_sig = sig.find(kv.first) != sig.end();
                const auto it = by_key.find(kv.first);
                const bool has_name = it != by_key.end() && !it->second->name.empty();
                if (has_sig) { ++sig_keys; sig_uses += kv.second; }
                if (has_sig && has_name) ++sig_and_named;
                if (!has_sig && !has_name) { ++dark; dark_uses += kv.second; }
            }
            const double all_uses = (double)(named_uses + ambiguous_uses + unresolved_uses);
            std::printf("  signature  %6ld  (reflected namespace + parameter names, "
                        "%4.1f%% of uses)\n", sig_keys,
                        all_uses ? 100.0 * (double)sig_uses / all_uses : 0.0);
            long unread_keys = 0;
            {
                long impl_keys = 0, impl_uses = 0, unread = 0, unread_uses = 0;
                for (const auto& kv : use_count) {
                    const bool has_sig = sig.find(kv.first) != sig.end();
                    const auto it2 = by_key.find(kv.first);
                    const bool has_name = it2 != by_key.end() && !it2->second->name.empty();
                    const bool has_impl = impl.find(kv.first) != impl.end();
                    if (!has_name && !has_sig && has_impl) { ++impl_keys; impl_uses += kv.second; }
                    if (!has_name && !has_sig && !has_impl) { ++unread; unread_uses += kv.second; }
                }
                std::printf("  impl only  %6ld  (key-first registry, address but no name, "
                            "%4.1f%% of uses)\n", impl_keys,
                            all_uses ? 100.0 * (double)impl_uses / all_uses : 0.0);
                std::printf("  UNREAD     %6ld  (no registry reaches them, %4.1f%% of uses)\n",
                            unread, all_uses ? 100.0 * (double)unread_uses / all_uses : 0.0);
                unread_keys = unread;
            }
            /* If the two namespaces were one table with holes, keys would land
             * in both. That they never do is what makes them two mechanisms. */
            std::printf("  keys with BOTH a name and a signature: %ld\n", sig_and_named);
            /* Kept for continuity with the finding that first counted it: this
             * is the set with no NAME and no SIGNATURE. Most of it is now
             * placed by an implementation address, so it is no longer the same
             * thing as unreadable - UNREAD above is that number. */
            std::printf("  no name/signature: %ld keys (%4.1f%% of uses), of which %ld now\n"
                        "    have an implementation address and %ld remain unread\n",
                        dark, all_uses ? 100.0 * (double)dark_uses / all_uses : 0.0,
                        dark - unread_keys, unread_keys);
        }
        std::sort(worst.begin(), worst.end(),
                  std::greater<std::pair<long, uint32_t>>());
        std::printf("\nthe 20 costliest unnamed keys, by how often the corpus uses them:\n");
        for (size_t w = 0; w < worst.size() && w < 20; ++w) {
            const uint32_t k = worst[w].second;
            const auto ar = arity2.find(k);
            std::printf("  0x%08x  %7ld uses  arity %-4s %s\n", k, worst[w].first,
                        ar == arity2.end() ? "?" : std::to_string(ar->second).c_str(),
                        used_in[k].empty() ? "" : used_in[k].begin()->c_str());
        }
        if (!tsv.empty()) std::printf("\nwrote %s\n", tsv.c_str());
        bf6_close(c);
        return 0;
    }

    /* "survey:<substring>" maps a whole subsystem: one line per expression with
     * its size, how much of it the executable can name, and whether the record
     * tiling is exact. A subsystem is easier to judge from its shape than from
     * thirty separate disassemblies. */
    if (res.rfind("survey:", 0) == 0) {
        const std::string want = res.substr(7);
        const int total = bf6_list_res(c, nullptr, nullptr, 0);
        std::vector<bf6_asset> rows((size_t)(total > 0 ? total : 0));
        const int got = total > 0 ? bf6_list_res(c, nullptr, rows.data(), total) : 0;
        const std::string exe2 = argc > 3 ? argv[3] : std::string();
        /* THE ARITY MAP IS WHAT MAKES 0x23 TILEABLE. Built once from the
         * executable and handed to every parse, so the "tiling" column below
         * measures the rule rather than the old search. */
        bf6::expression::ArityMap arity2;
        if (!exe2.empty()) {
            const int n0 = bf6_expression_reflected_operators(exe2.c_str(), nullptr, 0, nullptr, 0);
            if (n0 > 0) {
                std::vector<bf6_expression_reflected_operator> refl((size_t)n0);
                char rerr[256] = {0};
                const int gotr = bf6_expression_reflected_operators(exe2.c_str(), refl.data(), n0,
                                                                    rerr, (int)sizeof(rerr));
                for (int k = 0; k < gotr; ++k)
                    arity2[refl[(size_t)k].key] = refl[(size_t)k].parameter_count;
            }
        }
        std::printf("operator arities from the executable: %zu\n", arity2.size());
        std::printf("%-64s %6s %6s %6s %6s %s\n",
                    "expression", "bytes", "recs", "ops", "named", "tiling");
        int files = 0, exact = 0, all_ops = 0, all_named = 0;
        for (int i = 0; i < got; ++i) {
            if (rows[(size_t)i].type != 0x7dd4cc89u || !rows[(size_t)i].name) continue;
            if (!want.empty() && !std::strstr(rows[(size_t)i].name, want.c_str())) continue;
            const uint8_t* d = nullptr;
            const int64_t n = bf6_read_raw(c, BF6_RAW_RES, rows[(size_t)i].name, &d);
            if (n <= 0 || !d) continue;
            bf6::expression::Graph gg;
            std::string e2;
            if (!bf6::expression::parse(d, (size_t)n, gg, e2, arity2.empty() ? nullptr : &arity2)) {
                std::printf("%-64s  parse failed: %s\n", rows[(size_t)i].name, e2.c_str());
                continue;
            }
            std::set<uint32_t> ks;
            for (const bf6::expression::Record& r : gg.records)
                if (r.has_operator) ks.insert(r.operator_key);
            for (const bf6::expression::Fixup& f : gg.fixups) ks.insert(f.key);
            int nm = 0;
            if (!exe2.empty() && !ks.empty()) {
                std::vector<uint32_t> kl(ks.begin(), ks.end());
                std::vector<bf6::expression::NamedOperator> ops;
                std::string re2;
                if (bf6::expression::resolve_named_operators(exe2, kl, ops, re2))
                    for (const bf6::expression::NamedOperator& o : ops)
                        if (!o.name.empty()) ++nm;
            }
            const char* tail2 = std::strrchr(rows[(size_t)i].name, '/');
            std::printf("%-64s %6lld %6zu %6zu %6d %s\n",
                        tail2 ? tail2 + 1 : rows[(size_t)i].name, (long long)n,
                        gg.records.size(), ks.size(), nm,
                        gg.exact_record_tiling ? "exact" : "INCOMPLETE");
            ++files;
            if (gg.exact_record_tiling) ++exact;
            all_ops += (int)ks.size();
            all_named += nm;
        }
        std::printf("\n%d expression(s); %d tiled exactly; %d of %d operator keys named\n",
                    files, exact, all_named, all_ops);
        bf6_close(c);
        return 0;
    }

    const uint8_t* data = nullptr;
    const int64_t bytes = bf6_read_raw(c, BF6_RAW_RES, res.c_str(), &data);
    if (bytes <= 0 || !data) {
        std::printf("FAIL the expression resource is not in the mount: %s\n", res.c_str());
        std::printf("   try:  expr_disasm <game> find:<substring>\n");
        bf6_close(c);
        return 1;
    }
    bf6::expression::Graph g;
    std::string perr;
    if (!bf6::expression::parse(data, (size_t)bytes, g, perr)) {
        std::printf("FAIL parse: %s\n", perr.c_str());
        bf6_close(c);
        return 1;
    }

    std::printf("=== %s ===\n", res.c_str());
    std::printf("%lld bytes, %zu record(s), %zu fixup(s), %zu type(s), "
                "%u external binding(s)\n",
                (long long)bytes, g.records.size(), g.fixups.size(), g.types.size(),
                g.header.external_bindings);
    std::printf("record tiling is %s\n",
                g.exact_record_tiling ? "EXACT - every byte accounted for"
                                      : "INCOMPLETE - some record kinds are unmeasured");

    /* Every operator key the graph actually references, so the registry is
     * asked one question rather than scanned blindly. */
    std::set<uint32_t> keys;
    for (const bf6::expression::Record& r : g.records)
        if (r.has_operator) keys.insert(r.operator_key);
    for (const bf6::expression::Fixup& f : g.fixups) keys.insert(f.key);
    std::vector<uint32_t> key_list(keys.begin(), keys.end());
    std::printf("%zu distinct operator key(s)\n", key_list.size());

    /* Names come from the executable's registry. A key the exe cannot resolve
     * uniquely stays a number. */
    std::map<uint32_t, std::string> named;
    const std::string exe = argc > 3 ? argv[3] : std::string();
    if (!exe.empty() && !key_list.empty()) {
        std::vector<bf6::expression::NamedOperator> ops;
        std::string rerr;
        if (bf6::expression::resolve_named_operators(exe, key_list, ops, rerr)) {
            for (const bf6::expression::NamedOperator& o : ops)
                if (!o.name.empty()) named[o.key] = o.name;
            std::printf("the executable named %zu of them\n", named.size());
        } else {
            std::printf("operator names unavailable: %s\n", rerr.c_str());
        }
    } else if (exe.empty()) {
        std::printf("no executable given, so operators stay as raw keys\n");
    }

    /* THE OTHER OPERATOR NAMESPACE. The crc32 route above and the reflected
     * registry are disjoint sets - measured at zero overlap across 2270 corpus
     * keys - so a key the hash cannot name may still have a reflected namespace
     * and a named parameter list. "MotionMachine (Inertia)" is not the
     * operator's name, but it says who owns the call and what it is being
     * handed, which is most of what reading the program needs. */
    std::map<uint32_t, std::string> signature;
    if (!exe.empty()) {
        std::vector<bf6::expression::ReflectedOperator> refl;
        std::string rerr;
        if (bf6::expression::read_reflected_operators(exe, refl, rerr)) {
            std::set<uint32_t> want(key_list.begin(), key_list.end());
            size_t with_sig = 0;
            for (const bf6::expression::ReflectedOperator& r : refl) {
                if (want.find(r.key) == want.end()) continue;
                if (r.name_space.empty() && r.parameter_names.empty()) continue;
                std::string s = r.name_space.empty() ? std::string("?") : r.name_space;
                s += "(";
                for (size_t p = 0; p < r.parameter_names.size(); ++p) {
                    if (p) s += ", ";
                    s += r.parameter_names[p];
                }
                s += ")";
                signature[r.key] = s;
                ++with_sig;
            }
            std::printf("the reflected registry gave a signature for %zu of them\n", with_sig);
        }
    }

    /* THE THIRD REGISTRY. Keys that neither route names may still be in the
     * key-first table, which yields no name but does give the address of the
     * function the key dispatches to. That is enough to see that two calls are
     * the same operator, and enough to tell "unknown" apart from "unread". */
    std::map<uint32_t, uint64_t> impl_of;
    if (!exe.empty()) {
        std::vector<bf6::expression::KeyFirstOperator> rows;
        std::string kerr;
        if (bf6::expression::read_key_first_operators(exe, rows, kerr)) {
            std::set<uint32_t> want(key_list.begin(), key_list.end());
            for (const auto& r : rows)
                if (want.count(r.key)) impl_of[r.key] = r.implementation_va;
            if (!impl_of.empty())
                std::printf("the key-first registry gave an implementation for %zu of them\n",
                            impl_of.size());
        }
    }

    /* The constant pool, as floats. Thresholds and speeds live here. */
    std::printf("\n--- constants (%zu bytes) ---\n", g.constant_pool.size());
    const size_t floats = g.constant_pool.size() / 4;
    for (size_t i = 0; i < floats; ++i) {
        float f = 0.f;
        std::memcpy(&f, g.constant_pool.data() + i * 4, 4);
        uint32_t u = 0;
        std::memcpy(&u, g.constant_pool.data() + i * 4, 4);
        const float a = f < 0 ? -f : f;
        if (a >= 1e-4f && a <= 1e6f)
            std::printf("  [%3zu] %-14g (u32 %u)\n", i, (double)f, u);
        else
            std::printf("  [%3zu] u32 %-12u\n", i, u);
    }

    /* The program. */
    std::printf("\n--- records ---\n");
    for (size_t i = 0; i < g.records.size(); ++i) {
        const bf6::expression::Record& r = g.records[i];
        std::printf("[%3zu] @%-6u kind 0x%02x len %-4u", i, r.offset, r.kind, r.byte_length);
        if (r.has_operator) {
            auto it = named.find(r.operator_key);
            auto si = signature.find(r.operator_key);
            auto im = impl_of.find(r.operator_key);
            if (it != named.end()) std::printf("  %s", it->second.c_str());
            else if (si != signature.end())
                std::printf("  op:0x%08x %s", r.operator_key, si->second.c_str());
            else if (im != impl_of.end())
                std::printf("  op:0x%08x impl@0x%llx", r.operator_key,
                            (unsigned long long)im->second);
            else std::printf("  op:0x%08x", r.operator_key);
        }
        /* REGION 0 IS THE CONSTANT POOL, so its operands can be shown as the
         * values they are rather than as offsets. That is the difference
         * between "__GetTweakableFloat (r0+152)" and seeing which tweakable it
         * asks for. Region 1 is the instance image, 2 the scratch registers,
         * 3+ inline immediates - those stay as addresses because their meaning
         * needs the operator's own type knowledge. */
        for (const bf6::expression::Operand& o : r.operands) {
            if (o.region == 0 && o.offset + 4 <= g.constant_pool.size()) {
                uint32_t u = 0;
                float f = 0.f;
                std::memcpy(&u, g.constant_pool.data() + o.offset, 4);
                std::memcpy(&f, g.constant_pool.data() + o.offset, 4);
                const float a = f < 0 ? -f : f;
                if (a >= 1e-4f && a <= 1e6f) std::printf("  k[%u]=%g", o.offset, (double)f);
                else std::printf("  k[%u]=0x%x", o.offset, u);
            } else {
                std::printf("  (r%u+%u)", o.region, o.offset);
            }
        }
        if (r.has_trailing_dword) {
            float f = 0.f;
            std::memcpy(&f, &r.trailing_dword, 4);
            const float a = f < 0 ? -f : f;
            if (a >= 1e-4f && a <= 1e6f) std::printf("  imm=%g", (double)f);
            else std::printf("  imm=0x%x", r.trailing_dword);
        }
        /* FIXUPS ARE WHERE THE NAMES ARE. A __GetTweakableFloat's constant
         * operand reads as zero because the site is PATCHED AT LOAD: the key
         * that says WHICH tweakable lives in the fixup table, not in the
         * constant pool. Without this the call looks like it asks for nothing. */
        for (const bf6::expression::Fixup& f : g.fixups)
            if (f.record_offset >= r.offset && f.record_offset < r.offset + r.byte_length) {
                auto it = named.find(f.key);
                if (it != named.end()) std::printf("  fix:%s", it->second.c_str());
                else std::printf("  fix:0x%08x@%u", f.key, f.record_offset - r.offset);
            }
        if (r.control_target) std::printf("  -> @%u", r.control_target);
        for (size_t d = 0; d < r.dispatch_targets.size(); ++d)
            std::printf("  case %u -> @%u",
                        d < r.dispatch_labels.size() ? r.dispatch_labels[d] : 0u,
                        r.dispatch_targets[d]);
        std::printf("\n");
    }

    bf6_close(c);
    return 0;
}
