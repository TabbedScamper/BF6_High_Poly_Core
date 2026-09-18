/* ONE CONTEXT DATABASE, with its compiled match data as raw bytes.
 *
 *   cdb_probe <game> <cdb-asset> [more...]
 *
 * A ContextDatabase (ANT's per-context lookup: "which inspect phase clip for
 * THIS weapon") ships two halves. The asset half names its keys - game states
 * and their order. The runtime half is a compiled SIMD matcher: Vec4 rows the
 * engine compares byte-lane by byte-lane against a broadcast context byte
 * (FUN_14088fa30 builds that context; FUN_1408aab60 takes the argmax score).
 *
 * The generic dump prints those Vec4 rows as floats, which destroys them -
 * packed bytes read as NaNs and denormals. This prints every Vec4 as its 16
 * raw bytes, read back from the partition through the struct's source_pos, so
 * the lanes can be read as what they are.
 *
 * Field offsets and meanings come from the executable's reflection
 * (field_type_probe on ContextDatabase aa2343e0) and from the decompiled
 * matcher, not from inspection of these values.
 */
#include "bf6_core.h"
#include "ebx.h"
#include "types.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void hex16(const uint8_t* p)
{
    for (int i = 0; i < 16; ++i) std::printf("%02x%s", p[i], (i % 4 == 3 && i < 15) ? " " : "");
}

/* An array of 32-bit words whose element type the reflection leaves null:
 * print each as its four little-endian bytes, which is how the matcher reads
 * them (it indexes by char). */
void bytes_of_u32s(const bf6::EbxValue* a)
{
    if (!a) { std::printf("(absent)\n"); return; }
    std::printf("[%d]  ", (int)a->items.size());
    for (const auto& it : a->items) {
        const uint32_t u = (uint32_t)it.u;
        std::printf("%02x %02x %02x %02x | ", u & 0xff, (u >> 8) & 0xff, (u >> 16) & 0xff, u >> 24);
    }
    std::printf("\n");
}

void print_struct_rows(const bf6::EbxValue* a, const char* what)
{
    if (!a) return;
    std::printf("  %s [%d]\n", what, (int)a->items.size());
    for (size_t r = 0; r < a->items.size(); ++r) {
        std::printf("    %2d:", (int)r);
        for (const auto& kv : a->items[r].fields) {
            const bf6::EbxValue& v = kv.second;
            if (v.kind == bf6::EbxValue::Kind::Array) {
                std::printf(" %08x=[", kv.first);
                for (const auto& it : v.items) std::printf("%lld,", it.kind == bf6::EbxValue::Kind::Int ? (long long)it.i : (long long)it.u);
                std::printf("]");
            } else if (v.kind == bf6::EbxValue::Kind::Real) {
                std::printf(" %08x=%g", kv.first, v.f);
            } else if (v.kind == bf6::EbxValue::Kind::ImportRef) {
                std::printf(" %08x=%s", kv.first, v.import_path.empty() ? v.s.c_str() : v.import_path.c_str());
            } else {
                std::printf(" %08x=%lld", kv.first, (long long)(v.kind == bf6::EbxValue::Kind::Int ? v.i : (int64_t)v.u));
            }
        }
        std::printf("\n");
    }
}

}  // namespace

/* --census <list-file>: one line per database - its compare-method row ranges
 * and entry/slot counts - so which of the 33 methods the game actually uses is
 * counted, not assumed. */
static int census(bf6_ctx* c, bf6::TypeDb& db, const char* list)
{
    FILE* f = std::fopen(list, "rb");
    if (!f) { std::printf("cannot read %s\n", list); return 1; }
    char line[1024];
    while (std::fgets(line, sizeof(line), f)) {
        std::string name = line;
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
        if (name.empty()) continue;
        const uint8_t* p = nullptr;
        int64_t n = bf6_read_raw(c, BF6_RAW_EBX, (name + ".ebx").c_str(), &p);
        if (n <= 0) n = bf6_read_raw(c, BF6_RAW_EBX, name.c_str(), &p);
        if (n <= 0 || !p) { std::printf("%s\tMISSING\n", name.c_str()); continue; }
        bf6::Ebx e(db);
        std::string perr;
        if (!e.parse(std::vector<uint8_t>(p, p + n), perr)) { std::printf("%s\tPARSE\n", name.c_str()); continue; }
        for (size_t i = 0; i < e.instance_count(); ++i) {
            if (bf6::TypeDb::guid_str(e.instance_type(i)) != "aa2343e0-3bd2-7f14-cd07-54ccd188c0a7") continue;
            bf6::EbxValue v = e.read_instance(i);
            auto I = [&](uint32_t h) -> long long {
                const bf6::EbxValue* x = v.field(h);
                return x ? (x->kind == bf6::EbxValue::Kind::Int ? x->i : (long long)x->u) : -1;
            };
            std::printf("%s\tentries %lld\trows %lld\tslots %lld\tregions", name.c_str(),
                        I(0x77e41430), I(0x33b68e77), I(0x8cdd2d61));
            if (const bf6::EbxValue* rg = v.field(0xadee1c58))
                for (const auto& r : rg->items)
                    if (const bf6::EbxValue* pre = r.field(0x9ecf87d8)) {
                        std::printf(" [");
                        long long prev = 0;
                        for (size_t m = 0; m < pre->items.size() && m < 32; ++m) {
                            const long long cur = (long long)pre->items[m].u;
                            if (cur > prev) std::printf("m%d:%lld ", (int)m, cur - prev);
                            if (cur > prev) prev = cur;
                        }
                        std::printf("]");
                    }
            std::printf("\n");
        }
    }
    std::fclose(f);
    return 0;
}

/* --json <list> <out>: every database's runtime half as JSON, byte-exact, for
 * the Unicorn oracle to rebuild in emulated memory. Arrays whose elements the
 * reflection leaves untyped are emitted as their raw little-endian words, and
 * Vec4 rows as their 16 raw bytes: nothing here is reinterpreted. */
static void jhex(std::string& o, const uint8_t* p, size_t n)
{
    static const char* H = "0123456789abcdef";
    o += '"';
    for (size_t i = 0; i < n; ++i) { o += H[p[i] >> 4]; o += H[p[i] & 15]; }
    o += '"';
}
/* Each element at its OWN width, from the reflection (u8 / u16 / i32). */
static void jwords(std::string& o, const bf6::EbxValue* a, int width = 4)
{
    std::vector<uint8_t> b;
    if (a) for (const auto& it : a->items) {
        const uint64_t u = it.kind == bf6::EbxValue::Kind::Int ? (uint64_t)it.i : it.u;
        for (int k = 0; k < width; ++k) b.push_back((uint8_t)(u >> (8 * k)));
    }
    jhex(o, b.data(), b.size());
}
static long long jint(const bf6::EbxValue* x)
{
    if (!x) return 0;
    return x->kind == bf6::EbxValue::Kind::Int ? x->i : (long long)x->u;
}
static int json_export(bf6_ctx* c, bf6::TypeDb& db, const char* list, const char* outp)
{
    FILE* f = std::fopen(list, "rb");
    FILE* o = std::fopen(outp, "wb");
    if (!f || !o) return 1;
    std::fputs("[\n", o);
    bool first = true;
    char line[1024];
    int n_out = 0;
    while (std::fgets(line, sizeof(line), f)) {
        std::string name = line;
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
        if (name.empty()) continue;
        const uint8_t* p = nullptr;
        int64_t n = bf6_read_raw(c, BF6_RAW_EBX, (name + ".ebx").c_str(), &p);
        if (n <= 0) n = bf6_read_raw(c, BF6_RAW_EBX, name.c_str(), &p);
        if (n <= 0 || !p) continue;
        bf6::Ebx e(db);
        std::string perr;
        if (!e.parse(std::vector<uint8_t>(p, p + n), perr)) continue;
        for (size_t i = 0; i < e.instance_count(); ++i) {
            if (bf6::TypeDb::guid_str(e.instance_type(i)) != "aa2343e0-3bd2-7f14-cd07-54ccd188c0a7") continue;
            bf6::EbxValue v = e.read_instance(i);
            const std::vector<uint8_t>& raw = e.raw();
            std::string j = first ? "" : ",\n";
            first = false;
            j += "{\"name\":\"" + name + "\"";
            char b[256];
            std::snprintf(b, sizeof(b), ",\"a0\":%lld,\"a4\":%lld,\"entries\":%lld,\"rows_n\":%lld,\"slots\":%lld,\"b4\":%lld,\"b8\":%lld",
                          jint(v.field(0xf7e4ff3d)), jint(v.field(0xd95f9913)), jint(v.field(0x77e41430)),
                          jint(v.field(0x33b68e77)), jint(v.field(0x8cdd2d61)), jint(v.field(0xa62d74a2)),
                          jint(v.field(0x1bd17c7e)));
            j += b;
            j += ",\"w38\":"; jwords(j, v.field(0x74701328), 2);
            j += ",\"w50\":"; jwords(j, v.field(0x69e2f3b6));
            j += ",\"w88\":"; jwords(j, v.field(0xcefcf711), 1);
            /* +0x58 struct */
            if (const bf6::EbxValue* s58 = v.field(0xf834e6ec)) {
                std::snprintf(b, sizeof(b), ",\"s58\":{\"flag\":%d,\"n\":%lld,\"w8\":",
                              s58->field(0xc85c7364) && s58->field(0xc85c7364)->b ? 1 : 0,
                              jint(s58->field(0x9ed408fd)));
                j += b; jwords(j, s58->field(0xfe23fac1), 1);
                j += ",\"w10\":"; jwords(j, s58->field(0xa6c330ab), 1);
                j += "}";
            }
            j += ",\"groups\":[";
            if (const bf6::EbxValue* gs = v.field(0x2b123790))
                for (size_t k = 0; k < gs->items.size(); ++k) {
                    const bf6::EbxValue& g = gs->items[k];
                    std::snprintf(b, sizeof(b), "%s{\"hash\":%lld,\"u10\":%lld,\"u11\":%lld,\"bytes\":",
                                  k ? "," : "", jint(g.field(0x58ac9cec)), jint(g.field(0xe58491f6)),
                                  jint(g.field(0x16e8e080)));
                    j += b; jwords(j, g.field(0x0424c3f9), 1); j += "}";
                }
            j += "],\"keys\":[";
            if (const bf6::EbxValue* ks = v.field(0x03f14c46))
                for (size_t k = 0; k < ks->items.size(); ++k) {
                    const bf6::EbxValue& kd = ks->items[k];
                    /* byte-exact: take the struct's own 48 bytes */
                    j += k ? "," : "";
                    if (kd.source_pos >= 0 && (size_t)kd.source_pos + 48 <= raw.size())
                        jhex(j, raw.data() + kd.source_pos, 48);
                    else j += "null";
                }
            j += "],\"regions\":[";
            if (const bf6::EbxValue* rs = v.field(0xadee1c58))
                for (size_t k = 0; k < rs->items.size(); ++k) {
                    const bf6::EbxValue& r = rs->items[k];
                    std::snprintf(b, sizeof(b), "%s{\"i0\":%lld,\"i4\":%lld,\"pre\":", k ? "," : "",
                                  jint(r.field(0x1c07749d)), jint(r.field(0x1557814d)));
                    j += b; jwords(j, r.field(0x9ecf87d8)); j += "}";
                }
            j += "],\"rows\":[";
            if (const bf6::EbxValue* rows = v.field(0x9f6586cd))
                for (size_t k = 0; k < rows->items.size(); ++k) {
                    j += k ? "," : "";
                    const int64_t at = rows->items[k].source_pos;
                    if (at >= 0 && (size_t)at + 16 <= raw.size()) jhex(j, raw.data() + at, 16);
                    else j += "null";
                }
            j += "],\"entry_names\":[";
            if (const bf6::EbxValue* en = v.field(0x1a054761))
                for (size_t k = 0; k < en->items.size(); ++k)
                    j += std::string(k ? "," : "") + "\"" + en->items[k].s + "\"";
            j += "],\"key_names\":[";
            if (const bf6::EbxValue* kn = v.field(0xdb58cdd1))
                for (size_t k = 0; k < kn->items.size(); ++k)
                    j += std::string(k ? "," : "") + "\"" + kn->items[k].s + "\"";
            j += "]}";
            std::fputs(j.c_str(), o);
            ++n_out;
        }
    }
    std::fputs("\n]\n", o);
    std::fclose(o);
    std::fclose(f);
    std::printf("exported %d database(s)\n", n_out);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: cdb_probe <game> <cdb-asset> [more...] | --census <list> | --json <list> <out>\n"); return 2; }
    if (!std::strcmp(argv[2], "--json") && argc > 4) {
        char err0[512] = {0};
        bf6_ctx* c0 = bf6_open(argv[1], err0, (int)sizeof(err0));
        if (!c0) { std::printf("open: %s\n", err0); return 1; }
        bf6_mount_all(c0, 1, err0, (int)sizeof(err0));
        bf6::TypeDb db0; std::string te; bool ok = false;
        for (const std::string& exe : bf6::TypeDb::exe_candidates(argv[1]))
            if (db0.open(exe, te)) { ok = true; break; }
        if (!ok) { std::printf("no schema\n"); return 1; }
        const int r = json_export(c0, db0, argv[3], argv[4]);
        bf6_close(c0);
        return r;
    }
    if (!std::strcmp(argv[2], "--census") && argc > 3) {
        char err0[512] = {0};
        bf6_ctx* c0 = bf6_open(argv[1], err0, (int)sizeof(err0));
        if (!c0) { std::printf("open: %s\n", err0); return 1; }
        bf6_mount_all(c0, 1, err0, (int)sizeof(err0));
        bf6::TypeDb db0; std::string te; bool ok = false;
        for (const std::string& exe : bf6::TypeDb::exe_candidates(argv[1]))
            if (db0.open(exe, te)) { ok = true; break; }
        if (!ok) { std::printf("no schema\n"); return 1; }
        const int r = census(c0, db0, argv[3]);
        bf6_close(c0);
        return r;
    }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    if (!bf6_mount_all(c, 1, err, (int)sizeof(err))) std::printf("note: mount_all said %s\n", err);

    bf6::TypeDb db;
    std::string terr;
    bool opened = false;
    for (const std::string& exe : bf6::TypeDb::exe_candidates(argv[1]))
        if (db.open(exe, terr)) { opened = true; break; }
    if (!opened) { std::printf("no schema: %s\n", terr.c_str()); return 1; }

    for (int a = 2; a < argc; ++a) {
        const uint8_t* p = nullptr;
        std::string name = argv[a];
        int64_t n = bf6_read_raw(c, BF6_RAW_EBX, (name + ".ebx").c_str(), &p);
        if (n <= 0) n = bf6_read_raw(c, BF6_RAW_EBX, name.c_str(), &p);
        if (n <= 0 || !p) { std::printf("== %s: not in the mount\n", argv[a]); continue; }
        bf6::Ebx e(db);
        std::string perr;
        if (!e.parse(std::vector<uint8_t>(p, p + n), perr)) {
            std::printf("== %s: parse failed: %s\n", argv[a], perr.c_str());
            continue;
        }
        std::printf("== %s\n", argv[a]);
        for (size_t i = 0; i < e.instance_count(); ++i) {
            const std::string tg = bf6::TypeDb::guid_str(e.instance_type(i));
            bf6::EbxValue v = e.read_instance(i);
            if (tg == "6fa5a6cc-ab97-ef80-9a98-59b5bd2edbda") {          /* ContextDatabaseAsset */
                print_struct_rows(v.field(0x0cd0a1f8), "asset keys (game state, ?, index, ?, ?)");
            } else if (tg == "aa2343e0-3bd2-7f14-cd07-54ccd188c0a7") {   /* ContextDatabase */
                auto I = [&](uint32_t h) -> long long {
                    const bf6::EbxValue* f = v.field(h);
                    return f ? (f->kind == bf6::EbxValue::Kind::Int ? f->i : (long long)f->u) : -1;
                };
                std::printf("  counts +a0 %lld  +a4 %lld  +a8(entries) %lld  +ac %lld  +b0(slots) %lld  +b4 %lld  +b8 %lld\n",
                            I(0xf7e4ff3d), I(0xd95f9913), I(0x77e41430), I(0x33b68e77),
                            I(0x8cdd2d61), I(0xa62d74a2), I(0x1bd17c7e));
                if (const bf6::EbxValue* en = v.field(0x1a054761)) {
                    std::printf("  entries [%d]\n", (int)en->items.size());
                    for (size_t k = 0; k < en->items.size(); ++k)
                        std::printf("    %2d: %s\n", (int)k, en->items[k].import_path.c_str());
                }
                if (const bf6::EbxValue* kn = v.field(0xdb58cdd1)) {
                    std::printf("  key names:");
                    for (const auto& it : kn->items) std::printf(" %s", it.s.c_str());
                    std::printf("\n");
                }
                std::printf("  +38 0x74701328 "); bytes_of_u32s(v.field(0x74701328));
                std::printf("  +50 0x69e2f3b6 "); bytes_of_u32s(v.field(0x69e2f3b6));
                std::printf("  +88 0xcefcf711 "); bytes_of_u32s(v.field(0xcefcf711));
                print_struct_rows(v.field(0x2b123790), "+40 groups");
                print_struct_rows(v.field(0x03f14c46), "+48 key descriptors");
                print_struct_rows(v.field(0xadee1c58), "+78 regions");
                if (const bf6::EbxValue* rows = v.field(0x9f6586cd)) {
                    std::printf("  +80 match rows [%d], raw lanes:\n", (int)rows->items.size());
                    const std::vector<uint8_t>& raw = e.raw();
                    for (size_t r = 0; r < rows->items.size(); ++r) {
                        const int64_t at = rows->items[r].source_pos;
                        std::printf("    %2d: ", (int)r);
                        if (at >= 0 && (size_t)at + 16 <= raw.size()) hex16(raw.data() + at);
                        else std::printf("(no source position)");
                        std::printf("\n");
                    }
                }
            }
        }
    }
    bf6_close(c);
    return 0;
}
