/* WHERE DOES AN ARRAY TYPE KEEP ITS ELEMENT TYPE - across the whole schema.
 *
 *   array_elem_slot_probe <game> <type-guid-list>
 *
 * TypeDb::resolve finds an array's element by trying qword slots {48,40,56,32,
 * 24} and taking the first whose target is a struct/class/string/enum/guid/
 * resref. Primitive elements (u8, bool, i16, u64, f32 ...) are rejected by that
 * test, so their arrays fall back to a 4-byte stride and read past their end.
 *
 * Widening the test is only safe if the slot is fixed. This measures it: for
 * every array field of every listed type, record which slot the existing rule
 * picks (for non-primitive elements) and what slot 48 holds. If slot 48 agrees
 * every time the existing rule succeeds, and resolves to a valid type for the
 * primitive ones, the element slot is 48 - measured, not assumed.
 */
#include "bf6_core.h"
#include "types.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: array_elem_slot_probe <game> <guid-list>\n"); return 2; }
    bf6::TypeDb db; std::string err; bool ok = false;
    for (const std::string& exe : bf6::TypeDb::exe_candidates(argv[1]))
        if (db.open(exe, err)) { ok = true; break; }
    if (!ok) { std::printf("no schema\n"); return 1; }

    FILE* f = std::fopen(argv[2], "rb");
    if (!f) return 1;
    char line[128];
    long types = 0, arrays = 0, rule_found = 0, rule_agrees48 = 0, rule_disagrees48 = 0,
         rule_missed = 0, missed_48_valid = 0, missed_48_invalid = 0;
    std::map<int, long> te48_when_missed;
    std::set<uint64_t> seen_arrays;
    while (std::fgets(line, sizeof(line), f)) {
        std::string g = line;
        while (!g.empty() && (g.back() == '\n' || g.back() == '\r')) g.pop_back();
        std::string hex;
        for (char ch : g) if (ch != '-') hex += ch;
        if (hex.size() != 32) continue;
        bf6::TypeGuid raw{};
        for (int i = 0; i < 16; ++i) raw[i] = (uint8_t)std::strtoul(hex.substr(i * 2, 2).c_str(), nullptr, 16);
        if (bf6::TypeDb::guid_str(raw) != g) {
            std::swap(raw[0], raw[3]); std::swap(raw[1], raw[2]); std::swap(raw[4], raw[5]); std::swap(raw[6], raw[7]);
        }
        const bf6::TypeLayout& t = db.layout(raw);
        if (!t.valid) continue;
        ++types;
        for (const bf6::FieldInfo& fi : t.fields) {
            const bf6::ResolvedType r = db.resolve(fi.type_va);
            if (!r.valid || r.te != 4) continue;
            if (!seen_arrays.insert(fi.type_va).second) continue;
            ++arrays;
            const std::vector<uint64_t> q = db.debug_typeinfo_qwords(fi.type_va, 8);
            const uint64_t s48 = q.size() > 6 ? q[6] : 0;
            const bf6::ResolvedType e48 = db.resolve(s48);
            if (r.elem_va) {
                ++rule_found;
                if (r.elem_va == s48) ++rule_agrees48; else ++rule_disagrees48;
            } else {
                ++rule_missed;
                if (e48.valid) { ++missed_48_valid; ++te48_when_missed[e48.te]; }
                else ++missed_48_invalid;
            }
        }
    }
    std::fclose(f);
    std::printf("%ld types, %ld distinct array types\n", types, arrays);
    std::printf("existing rule found an element: %ld  (slot 48 agrees %ld, disagrees %ld)\n",
                rule_found, rule_agrees48, rule_disagrees48);
    std::printf("existing rule found nothing:    %ld  (slot 48 resolves %ld, does not %ld)\n",
                rule_missed, missed_48_valid, missed_48_invalid);
    std::printf("element type enums at slot 48 where the rule missed:\n");
    for (auto& kv : te48_when_missed) std::printf("  te %2d : %ld\n", kv.first, kv.second);
    return 0;
}
