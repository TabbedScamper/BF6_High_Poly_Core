/* WHAT IS THE ENLIGHTEN PER-INSTANCE KEY?
 *
 *   enlighten_key_test <game_dir> <level>
 *
 * This is the single blocking unknown for using BF6's one baked lighting term.
 * The level ships a grayscale sky-visibility map in lightmap atlas space and a
 * per-instance atlas rect that addresses it, but each rect is owned by a 32-bit
 * key nobody has tied to a placed object. Until that key resolves, the bake is
 * a set of rectangles with no owner, and an editor cannot darken the right
 * interiors with it.
 *
 * What is already known about the key, and what it rules out:
 *   - sorted, unique, spread over the full 32-bit range
 *   - three levels share ZERO keys, so it is a per-level id and not a hash of
 *     a shared asset or material name
 *
 * The obvious remaining candidate is the placed instance's own identity. Every
 * placement in an EBX partition has a 128-bit instance GUID, so this collects
 * the level's GUIDs and asks whether the key set is some 32-bit function of
 * them. Several derivations are tried at once because guessing one and
 * reporting "no match" would be weak evidence:
 *
 *   the four 32-bit words of the GUID, each endianness
 *   XOR folds of those words
 *   djb2-add and djb2-xor over the raw 16 bytes and over the GUID string
 *
 * THE CONTROL IS THE POINT. A 32-bit key space with N keys and M candidates
 * gives roughly N*M/2^32 hits by chance, which for this data is a handful. So
 * the test prints the chance expectation beside every hit count: a derivation
 * that is RIGHT should match a large fraction of the key set, not a few keys
 * more than luck. Anything in between is reported as inconclusive rather than
 * as a discovery.
 */
#include "ebx.h"
#include "source.h"
#include "types.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace bf6;

namespace {

uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t rd64(const uint8_t* p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
uint32_t bswap32(uint32_t v) {
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
}

/* 16 raw bytes of a guid, parsed from the canonical hex spelling. Returns false
 * on anything that is not a guid, so a malformed name cannot silently become
 * sixteen zero bytes and match whatever a zero key would. */
bool guid_bytes(const std::string& s, uint8_t out[16]) {
    int n = 0;
    uint8_t hi = 0;
    bool have_hi = false;
    for (char c : s) {
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else if (c == '-') continue;
        else return false;
        if (!have_hi) { hi = (uint8_t)v; have_hi = true; }
        else {
            if (n >= 16) return false;
            out[n++] = (uint8_t)((hi << 4) | v);
            have_hi = false;
        }
    }
    return n == 16 && !have_hi;
}

/* A candidate identity: the instance's own guid, and the partition it lives in.
 * Frostbite identifies an instance by the PAIR, so a derivation is given both
 * halves rather than only the instance guid - that is the obvious thing a
 * per-level, uniformly spread id would be hashed from. */
struct Ident {
    uint8_t     inst[16];
    uint8_t     part[16];
    std::string inst_text;
    std::string part_text;
    /* The partition's NAME, not its guid. Enlighten's precompute identifies an
     * instance by a name string, and a name that includes the level's own path
     * is per-level unique, which is what the key set looks like. Hashing the
     * partition GUID (as the first pass did) is a different question. */
    std::string part_name;
    size_t      index = 0;   /* instance index within the partition */
};

struct Derivation {
    const char* name;
    uint32_t (*fn)(const Ident&);
};

uint32_t fnv1a(const uint8_t* p, size_t n, uint32_t h = 2166136261u) {
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}
uint32_t djb2a(const uint8_t* p, size_t n, uint32_t h = 5381u) {
    for (size_t i = 0; i < n; ++i) h = h * 33u + p[i];
    return h;
}
uint32_t djb2x(const uint8_t* p, size_t n, uint32_t h = 5381u) {
    for (size_t i = 0; i < n; ++i) h = (h * 33u) ^ p[i];
    return h;
}
uint32_t crc32_of(const uint8_t* p, size_t n) {
    static uint32_t tab[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tab[i] = c;
        }
        built = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t d_w0(const Ident& d) { return rd32(d.inst); }
uint32_t d_w1(const Ident& d) { return rd32(d.inst + 4); }
uint32_t d_w2(const Ident& d) { return rd32(d.inst + 8); }
uint32_t d_w3(const Ident& d) { return rd32(d.inst + 12); }
uint32_t d_w0be(const Ident& d) { return bswap32(rd32(d.inst)); }
uint32_t d_w3be(const Ident& d) { return bswap32(rd32(d.inst + 12)); }
uint32_t d_xor_all(const Ident& d) {
    return rd32(d.inst) ^ rd32(d.inst + 4) ^ rd32(d.inst + 8) ^ rd32(d.inst + 12);
}
uint32_t d_djb2x_bytes(const Ident& d) { return djb2x(d.inst, 16); }
uint32_t d_djb2a_bytes(const Ident& d) { return djb2a(d.inst, 16); }
uint32_t d_fnv_bytes(const Ident& d) { return fnv1a(d.inst, 16); }
uint32_t d_crc_bytes(const Ident& d) { return crc32_of(d.inst, 16); }
/* The repo's own string-id hash: djb2-add with seed -1 over the text. */
uint32_t d_stringid(const Ident& d) {
    return djb2a((const uint8_t*)d.inst_text.data(), d.inst_text.size(), 0xFFFFFFFFu);
}
uint32_t d_djb2x_text(const Ident& d) {
    return djb2x((const uint8_t*)d.inst_text.data(), d.inst_text.size());
}
uint32_t d_fnv_text(const Ident& d) {
    return fnv1a((const uint8_t*)d.inst_text.data(), d.inst_text.size());
}

/* THE PAIR. Frostbite identifies an instance by (partition guid, instance
 * guid), and a key that is per-level and uniformly spread is exactly what
 * hashing the pair would produce. */
uint32_t d_pair_xor(const Ident& d) {
    uint32_t h = 0;
    for (int i = 0; i < 16; i += 4) h ^= rd32(d.inst + i) ^ rd32(d.part + i);
    return h;
}
uint32_t d_pair_fnv(const Ident& d) {
    uint8_t buf[32];
    std::memcpy(buf, d.part, 16);
    std::memcpy(buf + 16, d.inst, 16);
    return fnv1a(buf, 32);
}
uint32_t d_pair_fnv_rev(const Ident& d) {
    uint8_t buf[32];
    std::memcpy(buf, d.inst, 16);
    std::memcpy(buf + 16, d.part, 16);
    return fnv1a(buf, 32);
}
uint32_t d_pair_djb2a(const Ident& d) {
    uint8_t buf[32];
    std::memcpy(buf, d.part, 16);
    std::memcpy(buf + 16, d.inst, 16);
    return djb2a(buf, 32);
}
uint32_t d_pair_djb2x(const Ident& d) {
    uint8_t buf[32];
    std::memcpy(buf, d.part, 16);
    std::memcpy(buf + 16, d.inst, 16);
    return djb2x(buf, 32);
}
uint32_t d_pair_crc(const Ident& d) {
    uint8_t buf[32];
    std::memcpy(buf, d.part, 16);
    std::memcpy(buf + 16, d.inst, 16);
    return crc32_of(buf, 32);
}
uint32_t d_pair_text_fnv(const Ident& d) {
    const std::string s = d.part_text + d.inst_text;
    return fnv1a((const uint8_t*)s.data(), s.size());
}
uint32_t d_pair_text_stringid(const Ident& d) {
    const std::string s = d.part_text + "/" + d.inst_text;
    return djb2a((const uint8_t*)s.data(), s.size(), 0xFFFFFFFFu);
}

/* NAME-BASED. Enlighten names its precompute instances, and a name carrying the
 * level path is per-level unique - which is exactly the shape of the key set,
 * and the reason hashing a SHARED asset name was already ruled out. */
uint32_t h_sid(const std::string& s) {
    return djb2a((const uint8_t*)s.data(), s.size(), 0xFFFFFFFFu);
}
uint32_t h_djb2a(const std::string& s) { return djb2a((const uint8_t*)s.data(), s.size()); }
uint32_t h_djb2x(const std::string& s) { return djb2x((const uint8_t*)s.data(), s.size()); }
uint32_t h_fnv(const std::string& s) { return fnv1a((const uint8_t*)s.data(), s.size()); }
uint32_t h_crc(const std::string& s) { return crc32_of((const uint8_t*)s.data(), s.size()); }

uint32_t d_name_sid(const Ident& d) { return h_sid(d.part_name); }
uint32_t d_name_fnv(const Ident& d) { return h_fnv(d.part_name); }
uint32_t d_name_crc(const Ident& d) { return h_crc(d.part_name); }
uint32_t d_name_djb2x(const Ident& d) { return h_djb2x(d.part_name); }
uint32_t d_name_guid_sid(const Ident& d) { return h_sid(d.part_name + "/" + d.inst_text); }
uint32_t d_name_guid_fnv(const Ident& d) { return h_fnv(d.part_name + "/" + d.inst_text); }
uint32_t d_name_guid_crc(const Ident& d) { return h_crc(d.part_name + "/" + d.inst_text); }
uint32_t d_name_guid_nosep(const Ident& d) { return h_fnv(d.part_name + d.inst_text); }
uint32_t d_name_idx_sid(const Ident& d) {
    return h_sid(d.part_name + "/" + std::to_string(d.index));
}
uint32_t d_name_idx_fnv(const Ident& d) {
    return h_fnv(d.part_name + "/" + std::to_string(d.index));
}
uint32_t d_name_idx_crc(const Ident& d) {
    return h_crc(d.part_name + "/" + std::to_string(d.index));
}

const Derivation kDerivations[] = {
    {"instance guid word0 LE", d_w0}, {"instance guid word1 LE", d_w1},
    {"instance guid word2 LE", d_w2}, {"instance guid word3 LE", d_w3},
    {"instance guid word0 BE", d_w0be}, {"instance guid word3 BE", d_w3be},
    {"instance guid xor all", d_xor_all},
    {"djb2-xor instance bytes", d_djb2x_bytes},
    {"djb2-add instance bytes", d_djb2a_bytes},
    {"FNV-1a instance bytes", d_fnv_bytes},
    {"CRC32 instance bytes", d_crc_bytes},
    {"string-id of instance guid text", d_stringid},
    {"djb2-xor of instance guid text", d_djb2x_text},
    {"FNV-1a of instance guid text", d_fnv_text},
    {"PAIR xor words", d_pair_xor},
    {"PAIR FNV-1a part+inst", d_pair_fnv},
    {"PAIR FNV-1a inst+part", d_pair_fnv_rev},
    {"PAIR djb2-add part+inst", d_pair_djb2a},
    {"PAIR djb2-xor part+inst", d_pair_djb2x},
    {"PAIR CRC32 part+inst", d_pair_crc},
    {"PAIR FNV-1a of both texts", d_pair_text_fnv},
    {"PAIR string-id of both texts", d_pair_text_stringid},
    {"NAME string-id of partition name", d_name_sid},
    {"NAME FNV-1a of partition name", d_name_fnv},
    {"NAME CRC32 of partition name", d_name_crc},
    {"NAME djb2-xor of partition name", d_name_djb2x},
    {"NAME string-id of name/guid", d_name_guid_sid},
    {"NAME FNV-1a of name/guid", d_name_guid_fnv},
    {"NAME CRC32 of name/guid", d_name_guid_crc},
    {"NAME FNV-1a of name+guid", d_name_guid_nosep},
    {"NAME string-id of name/index", d_name_idx_sid},
    {"NAME FNV-1a of name/index", d_name_idx_fnv},
    {"NAME CRC32 of name/index", d_name_idx_crc},
};

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: enlighten_key_test <game> <level>\n"); return 2; }
    const std::string level = argv[2];

    Source src;
    std::string err;
    if (!src.open(argv[1], err)) { std::printf("open: %s\n", err.c_str()); return 1; }
    if (!src.mount_level(level, false, err)) { std::printf("mount: %s\n", err.c_str()); return 1; }

    /* 1. FIND THE DATABASE. By scanning the mount for the name inside this
     * level's own directory, never by rebuilding a path: Portal levels moved
     * under group folders in 1.4.3.0 and a rebuilt path misses them. */
    std::string want;
    for (const auto& kv : src.res()) {
        const std::string& n = kv.first;
        if (n.find("enlighten_") == std::string::npos) continue;
        if (n.find("highend") == std::string::npos) continue;
        if (n.find("radiosity") != std::string::npos) continue;   /* that is the texture */
        if (Source::level_dir_end(n, level) == std::string::npos) continue;
        want = n;
        break;
    }
    if (want.empty()) { std::printf("no EnlightenDatabase for %s\n", level.c_str()); return 1; }

    std::vector<uint8_t> db = src.get_res(want, err);
    if (db.size() < 0xF0) { std::printf("get_res %s: %s\n", want.c_str(), err.c_str()); return 1; }

    const uint32_t atlas_w = rd32(db.data() + 0x00);
    const uint32_t atlas_h = rd32(db.data() + 0x04);
    const uint32_t sig     = rd32(db.data() + 0x10);
    const uint32_t count   = rd32(db.data() + 0x48);
    const uint64_t key_at  = rd64(db.data() + 0x54);
    const uint64_t rect_at = rd64(db.data() + 0x64);
    std::printf("%s\n  %zu bytes, atlas %u x %u, signature 0x%08X, %u instance(s)\n",
                want.c_str(), db.size(), atlas_w, atlas_h, sig, count);
    if (sig != 0x74BC7678u) { std::printf("  signature mismatch: not an EnlightenDatabase\n"); return 1; }
    if (count == 0 || key_at + (uint64_t)count * 32 > db.size()) {
        std::printf("  key table does not fit the payload\n"); return 1;
    }

    /* 2. THE KEY TABLE. 32 bytes per record, which the table spacing proves:
     * (slot_table - key_table) / count is exactly 32, and the rect table is
     * exactly 16 per record ending precisely where the system table starts. */
    std::set<uint32_t> keys;
    std::vector<uint32_t> key_list;
    key_list.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t k = rd32(db.data() + key_at + (uint64_t)i * 32);
        key_list.push_back(k);
        keys.insert(k);
    }
    bool sorted = std::is_sorted(key_list.begin(), key_list.end());
    std::printf("  keys: %zu unique of %u, sorted %s, first 0x%08X last 0x%08X\n",
                keys.size(), count, sorted ? "yes" : "NO",
                key_list.front(), key_list.back());
    /* If the first u32 is not the key, this is where it shows: a real id table
     * is unique and sorted, a misread field is neither. */
    if (keys.size() != count || !sorted) {
        std::printf("  the first u32 of each 32-byte record does not behave like the key table\n");
        return 1;
    }

    /* How many instances own a lightmap chart, for scale. */
    uint32_t nonzero_rects = 0;
    if (rect_at + (uint64_t)count * 16 <= db.size()) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* r = db.data() + rect_at + (uint64_t)i * 16;
            if (rd32(r) || rd32(r + 4) || rd32(r + 8) || rd32(r + 12)) nonzero_rects++;
        }
    }
    std::printf("  %u of %u instances own a non-zero atlas rect\n", nonzero_rects, count);

    /* 3. THE LEVEL'S INSTANCE GUIDS. Every instance of every partition in the
     * level's own directory. */
    std::vector<Ident> idents;
    size_t partitions = 0, unparsable = 0;
    for (const auto& kv : src.ebx()) {
        if (Source::level_dir_end(kv.first, level) == std::string::npos) continue;
        std::string e;
        std::vector<uint8_t> raw = src.get_ebx(kv.first, e);
        if (raw.empty()) continue;
        TypeDb dummy;   /* guids do not need the schema */
        Ebx ebx(dummy);
        if (!ebx.parse(std::move(raw), e)) continue;
        partitions++;
        const std::string pg = ebx.partition_guid();
        uint8_t pbytes[16] = {0};
        const bool pok = guid_bytes(pg, pbytes);
        for (size_t i = 0; i < ebx.instance_count(); ++i) {
            std::string g = ebx.instance_guid(i);
            if (g.empty()) continue;
            Ident d{};
            if (!guid_bytes(g, d.inst)) { unparsable++; continue; }
            if (pok) std::memcpy(d.part, pbytes, 16);
            d.inst_text = std::move(g);
            d.part_text = pg;
            d.part_name = kv.first;
            d.index = i;
            idents.push_back(std::move(d));
        }
    }
    std::printf("  %zu partition(s) in the level directory, %zu instance guid(s), %zu unparsable\n",
                partitions, idents.size(), unparsable);
    if (idents.empty()) { std::printf("  no guids to test against\n"); return 1; }

    /* 4. TEST EACH DERIVATION, against chance. */
    std::printf("\n%-42s %8s %10s %10s\n", "derivation", "hits", "by chance", "verdict");
    const double space = 4294967296.0;
    int best = 0;
    std::string best_name;
    for (const Derivation& d : kDerivations) {
        std::set<uint32_t> produced;
        for (const Ident& id : idents) produced.insert(d.fn(id));
        size_t hits = 0;
        for (uint32_t v : produced) if (keys.count(v)) hits++;
        const double expected = (double)produced.size() * (double)keys.size() / space;
        const char* verdict =
            hits == 0 ? "no" :
            ((double)hits > expected * 20.0 && hits > keys.size() / 20) ? "CANDIDATE" :
            ((double)hits > expected * 20.0) ? "above chance, too few to be the answer" :
            "chance";
        std::printf("%-42s %8zu %10.1f %10s\n", d.name, hits, expected, verdict);
        if ((int)hits > best) { best = (int)hits; best_name = d.name; }
    }

    std::printf("\nkeys %zu, distinct candidate values tested per derivation up to %zu\n",
                keys.size(), idents.size());
    if (best == 0)
        std::printf("NO DERIVATION OF THE INSTANCE GUID PRODUCES THE KEYS.\n"
                    "The key is not a function of the placed instance's guid in any form tried.\n");
    else
        std::printf("best: %s with %d hit(s)\n", best_name.c_str(), best);

    /* 5. STOP GUESSING AT HASHES AND LOOK FOR THE KEY ITSELF.
     *
     * If the bake tool wrote an id onto the placed object, some field of some
     * entity simply CONTAINS these values, and searching for that is a direct
     * question rather than another guess. Every integer field of every instance
     * in the level is tested for membership in the key set and tallied by field
     * hash.
     *
     * The control is in the tally: a field that merely holds small numbers will
     * hit a few keys by luck, so a field is only interesting when a large
     * FRACTION of its values land in a set covering 0.0001 percent of the 32-bit
     * range. Chance for a uniformly random u32 is 5012/2^32, about one in a
     * million. */
    std::printf("\nsearching every integer field of every instance for the keys themselves\n");
    TypeDb types;
    bool got_types = false;
    for (const std::string& cand : TypeDb::exe_candidates(src.game_dir()))
        if (types.open(cand, err) && !types.looks_encrypted()) { got_types = true; break; }
    if (!got_types) { std::printf("  no usable type schema: %s\n", err.c_str()); return 0; }

    struct Tally { size_t seen = 0, hit = 0; };
    std::map<uint32_t, Tally> by_field;
    size_t values_seen = 0;

    /* THE COUNT TEST. If the bake binds positionally, the level's GI-relevant
     * instances should number what the database says: 5,012 instances on
     * mp_dumbo, of which 753 own a lightmap chart. RadiosityTypeOverride is the
     * per-instance field that says how an object participates, and its name is
     * in the repo's field dictionary rather than guessed. A value histogram
     * says both how many objects carry it and how they divide. */
    const uint32_t kRadiosityTypeOverride = 1092994207u;   /* from fieldname_dict */
    const uint32_t kRadiosityMaterial     = 1234795602u;
    std::map<uint64_t, size_t> radiosity_values;
    size_t radiosity_carriers = 0, radiosity_material_carriers = 0;

    /* Recursive walk, bounded by the decoder's own depth limit. */
    std::function<void(const EbxValue&, uint32_t)> walk =
        [&](const EbxValue& v, uint32_t field_hash) {
            if (v.kind == EbxValue::Kind::Uint || v.kind == EbxValue::Kind::Int ||
                v.kind == EbxValue::Kind::Unknown) {
                const uint64_t raw = (v.kind == EbxValue::Kind::Int) ? (uint64_t)v.i : v.u;
                if (raw <= 0xFFFFFFFFull) {
                    Tally& t = by_field[field_hash];
                    t.seen++;
                    values_seen++;
                    if (keys.count((uint32_t)raw)) t.hit++;
                }
                if (field_hash == kRadiosityTypeOverride) {
                    radiosity_carriers++;
                    radiosity_values[raw]++;
                } else if (field_hash == kRadiosityMaterial) {
                    radiosity_material_carriers++;
                }
                return;
            }
            for (const auto& kv : v.fields) walk(kv.second, kv.first);
            for (const EbxValue& e : v.items) walk(e, field_hash);
        };

    size_t walked = 0;
    for (const auto& kv : src.ebx()) {
        if (Source::level_dir_end(kv.first, level) == std::string::npos) continue;
        std::string e;
        std::vector<uint8_t> raw = src.get_ebx(kv.first, e);
        if (raw.empty()) continue;
        Ebx ebx(types);
        if (!ebx.parse(std::move(raw), e)) continue;
        ebx.set_guid_index(&src.partition_index());
        for (size_t i = 0; i < ebx.instance_count(); ++i) {
            walk(ebx.read_instance(i), 0u);
            walked++;
        }
    }
    std::printf("  walked %zu instance(s), %zu integer value(s)\n", walked, values_seen);

    /* THE COUNT TEST result, stated against what the database says. */
    std::printf("\nCOUNT TEST: does the level's GI population match the database?\n");
    std::printf("  database says: %u instance(s), %u own a lightmap chart\n", count, nonzero_rects);
    std::printf("  RadiosityTypeOverride carried by %zu instance(s)", radiosity_carriers);
    if (!radiosity_values.empty()) {
        std::printf(", values:");
        for (const auto& kv : radiosity_values)
            std::printf(" %llu x%zu", (unsigned long long)kv.first, kv.second);
    }
    std::printf("\n  RadiosityMaterial carried by %zu instance(s)\n", radiosity_material_carriers);
    if (radiosity_carriers == 0)
        std::printf("  The field is not present on this level's instances, so it cannot be the\n"
                    "  population the database counted.\n");
    else if (radiosity_carriers == count || radiosity_carriers == nonzero_rects)
        std::printf("  EXACT MATCH with the database. Positional binding is live.\n");
    else
        std::printf("  No match: %zu against %u instances and %u charts.\n",
                    radiosity_carriers, count, nonzero_rects);

    std::vector<std::pair<uint32_t, Tally>> ranked(by_field.begin(), by_field.end());
    std::sort(ranked.begin(), ranked.end(),
              [](const std::pair<uint32_t, Tally>& a, const std::pair<uint32_t, Tally>& b) {
                  const double fa = a.second.seen ? (double)a.second.hit / a.second.seen : 0.0;
                  const double fb = b.second.seen ? (double)b.second.hit / b.second.seen : 0.0;
                  if (fa != fb) return fa > fb;
                  return a.second.hit > b.second.hit;
              });
    std::printf("  %-12s %10s %10s %9s\n", "field hash", "values", "in key set", "fraction");
    int shown = 0;
    for (const auto& r : ranked) {
        if (r.second.hit == 0) break;
        std::printf("  0x%08X   %10zu %10zu %8.2f%%\n", r.first, r.second.seen, r.second.hit,
                    100.0 * (double)r.second.hit / (double)r.second.seen);
        if (++shown >= 12) break;
    }
    if (shown == 0)
        std::printf("  NO FIELD ANYWHERE IN THE LEVEL CONTAINS ANY OF THE KEYS.\n"
                    "  The binding is not written onto the placed objects in EBX.\n");

    /* 6. DOES THE DATABASE NAME ITS OWN INSTANCES?
     *
     * Enlighten's precompute API takes named instances, and if those names
     * survive in the shipped image then the key can be tied to an object
     * through them instead of through a hash nobody can guess. If the image
     * carries no names at all, the binding is not recoverable from this file
     * and the search has to move elsewhere - which is worth knowing rather
     * than assuming either way. */
    std::printf("\nstrings in the database image\n");
    size_t runs = 0;
    std::vector<std::string> samples;
    std::string cur;
    /* Bounded to the first 8 MB: the point is whether names exist and what they
     * look like, and the tables of interest are all in the first megabytes. */
    const size_t scan_end = db.size() < 8u * 1024 * 1024 ? db.size() : 8u * 1024 * 1024;
    for (size_t i = 0; i < scan_end; ++i) {
        const uint8_t c = db[i];
        if (c >= 0x20 && c < 0x7F) { cur.push_back((char)c); continue; }
        if (cur.size() >= 6) {
            runs++;
            if (samples.size() < 14) samples.push_back(cur);
        }
        cur.clear();
    }
    std::printf("  %zu printable run(s) of 6+ chars in the first %zu MB\n",
                runs, scan_end / (1024 * 1024));
    for (const std::string& s : samples) std::printf("    %s\n", s.c_str());
    if (runs == 0)
        std::printf("  The image carries no names: the key cannot be tied to an object\n"
                    "  through a name in this file.\n");

    /* 7. CLOSE THE HOLE IN THE NEGATIVE.
     *
     * Step 5 only searched the level's own directory, and the reflection-volume
     * work proved that filter misses prefab content: a level places prefabs that
     * live under common/ and under other levels. If the key is written on an
     * object inside one of those, step 5 would not have seen it and the negative
     * would be wrong.
     *
     * So every mounted partition is searched, as RAW BYTES rather than by
     * decoding: no schema, no walk, just "do these four bytes spell a key".
     * Cheap enough to cover the whole mount, and it cannot be fooled by a field
     * the decoder does not understand.
     *
     * The noise floor is computed, not eyeballed: a random 4-byte window lands
     * in a set of `keys` values with probability keys/2^32, so the expected
     * number of accidental hits is that times the number of windows. Only a
     * partition holding a real table stands out against it. */
    if (argc > 3 && std::strcmp(argv[3], "--scan-all") == 0)
    {
        std::printf("\nraw-byte search of EVERY mounted partition for the key values\n");
        std::map<std::string, size_t> hits_by_partition;
        size_t scanned = 0, windows = 0, total_hits = 0;
        for (const auto& kv : src.ebx()) {
            std::string e;
            std::vector<uint8_t> raw = src.get_ebx(kv.first, e);
            if (raw.size() < 4) continue;
            scanned++;
            windows += raw.size() - 3;
            size_t here = 0;
            for (size_t i = 0; i + 4 <= raw.size(); ++i)
                if (keys.count(rd32(raw.data() + i))) here++;
            if (here) { hits_by_partition[kv.first] = here; total_hits += here; }
        }
        const double expected = (double)windows * (double)keys.size() / 4294967296.0;
        std::printf("  scanned %zu partition(s), %zu window(s), %zu hit(s), %.0f expected by chance\n",
                    scanned, windows, total_hits, expected);
        std::vector<std::pair<std::string, size_t>> top(hits_by_partition.begin(), hits_by_partition.end());
        std::sort(top.begin(), top.end(),
                  [](const std::pair<std::string, size_t>& a, const std::pair<std::string, size_t>& b) {
                      return a.second > b.second;
                  });
        for (size_t i = 0; i < top.size() && i < 10; ++i)
            std::printf("    %6zu  %s\n", top[i].second, top[i].first.c_str());
        if (top.empty() || top[0].second < 20)
            std::printf("  No partition holds a run of keys: the binding is not in EBX at all,\n"
                        "  anywhere in the mount, not merely absent from the level directory.\n");
    }
    else
    {
        std::printf("\n(pass --scan-all to raw-search every mounted partition for the keys)\n");
    }
    return 0;
}
