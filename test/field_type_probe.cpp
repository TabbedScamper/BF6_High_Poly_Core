/* WHAT TYPE IS THIS FIELD - asked of the executable's own reflection.
 *
 *   field_type_probe <game> <type-guid> [field-hash ...]
 *
 * Field NAMES do not survive in BF6's reflection, only hashes. Field TYPES do:
 * every field record carries a pointer to its type's record, and that type has
 * a GUID. For an enum-typed field that GUID is the enum itself, which joins
 * against data/type_names.tsv to a real name.
 *
 * Written to settle which enum FloatComparisonGameStateAsset's operator field
 * uses. Three authored samples narrowed it to two candidates and could go no
 * further; the schema does not have to be inferred from samples at all.
 *
 * With no field hashes, prints every field of the type (inherited included).
 */
#include "bf6_core.h"
#include "types.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static bool parse_guid(const char* s, bf6::TypeGuid& out)
{
    /* Try both byte orders and keep the one guid_str round-trips: which order
     * the string form uses is the kind of thing to check, not assume. */
    std::string hex;
    for (const char* p = s; *p; ++p) if (*p != '-') hex += *p;
    if (hex.size() != 32) return false;
    bf6::TypeGuid raw{};
    for (int i = 0; i < 16; ++i) raw[i] = (uint8_t)std::strtoul(hex.substr(i * 2, 2).c_str(), nullptr, 16);
    if (bf6::TypeDb::guid_str(raw) == s) { out = raw; return true; }
    bf6::TypeGuid sw = raw;          /* Microsoft layout: first three groups little-endian */
    std::swap(sw[0], sw[3]); std::swap(sw[1], sw[2]); std::swap(sw[4], sw[5]); std::swap(sw[6], sw[7]);
    if (bf6::TypeDb::guid_str(sw) == s) { out = sw; return true; }
    std::printf("guid %s round-trips in neither byte order (guid_str gives %s)\n",
                s, bf6::TypeDb::guid_str(raw).c_str());
    return false;
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: field_type_probe <game> <type-guid> [field-hash ...]\n"); return 2; }
    bf6::TypeDb db;
    std::string err;
    bool opened = false;
    for (const std::string& exe : bf6::TypeDb::exe_candidates(argv[1])) {
        if (db.open(exe, err)) { std::printf("schema: %s\n", exe.c_str()); opened = true; break; }
    }
    if (!opened) { std::printf("no readable executable: %s\n", err.c_str()); return 1; }

    bf6::TypeGuid g{};
    if (!parse_guid(argv[2], g)) return 1;
    const bf6::TypeLayout& t = db.layout_full(g);
    if (!t.valid) { std::printf("type %s not in the schema\n", argv[2]); return 1; }
    std::printf("type %s: %d field(s), size %u\n", argv[2], (int)t.fields.size(), (unsigned)t.size);

    for (const bf6::FieldInfo& f : t.fields) {
        bool want = argc == 3;
        for (int a = 3; a < argc; ++a)
            if ((uint32_t)std::strtoul(argv[a], nullptr, 16) == f.name_hash) want = true;
        if (!want) continue;
        const bf6::ResolvedType r = db.resolve(f.type_va);
        std::printf("  field 0x%08x  off 0x%03x  ftype %2u  -> type %s  te %u%s",
                    f.name_hash, f.offset, f.ftype_enum,
                    r.valid ? bf6::TypeDb::guid_str(r.guid).c_str() : "(unresolved)",
                    r.te, r.te == 8 ? "  ENUM" : "");
        /* An array's own type says nothing; its ELEMENT is the struct whose
         * fields hold the next layer of meaning (conditions, layers, rows). */
        if (r.valid && r.te == 4 && !r.elem_va && std::getenv("FTP_ARRAYDUMP")) {
            /* No element pointer found at any of resolve()'s known slots: show
             * every qword of the array's TypeInfoData and what each resolves to,
             * so where the element type really sits is read, not guessed. */
            std::printf("\n      array type record qwords (slot: value -> resolves as):");
            const std::vector<uint64_t> q = db.debug_typeinfo_qwords(f.type_va, 12);
            for (size_t k = 0; k < q.size(); ++k) {
                const bf6::ResolvedType t2 = db.resolve(q[k]);
                std::printf("\n        +%02zu: %016llx%s", k * 8, (unsigned long long)q[k],
                            t2.valid ? (std::string("  -> ") + bf6::TypeDb::guid_str(t2.guid) +
                                        " te " + std::to_string(t2.te)).c_str() : "");
            }
        }
        if (r.valid && r.elem_va) {
            const bf6::ResolvedType e = db.resolve(r.elem_va);
            if (e.valid)
                std::printf("  elem %s te %u%s", bf6::TypeDb::guid_str(e.guid).c_str(), e.te,
                            e.te == 8 ? " ENUM" : "");
        }
        std::printf("\n");
    }
    return 0;
}
