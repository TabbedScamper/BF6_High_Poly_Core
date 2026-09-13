/* Independent audit of two claims before they are written into the research
 * repo as findings.  Written to re-measure, not to agree:
 *
 *  A. UIGadgetAbilityMetadata - how many partitions, how many rows, how many
 *     fields the row type DECLARES (not just how many are populated), what the
 *     AtlasInfo struct holds, and whether any row references a gadget hardware
 *     ENTITY.  The negative carries its own in-scope control: the same walk
 *     reports how many imports land under common/hardware, so "no hardware
 *     entity" is only reportable because hardware paths ARE reachable.
 *
 *  B. common/gameplay/loadouts/loadout_mp_* - every template in the mount, not
 *     just the four "_1" ones, walked from the partition itself rather than
 *     through the library helper that already claims the answer.
 *
 *   gadget_loadout_audit_probe <game_dir>
 */
#include "ebx.h"
#include "source.h"
#include "types.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace bf6;

static const uint32_t kItems     = 0x2c95d7b7u;
static const uint32_t kDebug     = 0x55aded8du;
static const uint32_t kIconAsset = 0x1b9640bbu;
static const uint32_t kReward    = 0x53078b86u;
static const uint32_t kRewardSm  = 0xebe3976fu;
static const uint32_t kAtlas     = 0x651ab360u;
static const uint32_t kMovie     = 0x17ba57a2u;

static const uint32_t kSlots     = 0xBF67CCC6u;   /* claimed default equipment */
static const uint32_t kFieldUpg  = 0xFF7A648Fu;   /* claimed field-upgrade path */

static const char* kind_name(EbxValue::Kind k)
{
    switch (k)
    {
    case EbxValue::Kind::Null:        return "null";
    case EbxValue::Kind::Bool:        return "bool";
    case EbxValue::Kind::Int:         return "int";
    case EbxValue::Kind::Uint:        return "uint";
    case EbxValue::Kind::Real:        return "real";
    case EbxValue::Kind::Guid:        return "guid";
    case EbxValue::Kind::ResRef:      return "resref";
    case EbxValue::Kind::Unknown:     return "unknown";
    case EbxValue::Kind::Str:         return "str";
    case EbxValue::Kind::Array:       return "array";
    case EbxValue::Kind::Struct:      return "struct";
    case EbxValue::Kind::InstanceRef: return "instref";
    case EbxValue::Kind::ImportRef:   return "IMPORT";
    default:                          return "other";
    }
}

static std::string leaf(const std::string& s)
{
    size_t a = s.find_last_of('/');
    std::string t = (a == std::string::npos) ? s : s.substr(a + 1);
    if (t.size() > 4 && t.compare(t.size() - 4, 4, ".ebx") == 0) t.resize(t.size() - 4);
    return t;
}

/* "MBT-LAW" -> "mbt_law", "Frag Grenade" -> "frag_grenade", "PTKM-1R" -> "ptkm_1r".
 * Every non-alphanumeric run collapses to one underscore. */
static std::string snake(const std::string& s)
{
    std::string o;
    bool pend = false;
    for (unsigned char c : s)
    {
        if (std::isalnum(c)) { if (pend && !o.empty()) o += '_'; pend = false;
                               o += (char)std::tolower(c); }
        else pend = true;
    }
    return o;
}

static std::string guid_hex(const TypeGuid& g)
{
    static const char* h = "0123456789abcdef";
    std::string o;
    for (uint8_t b : g) { o += h[b >> 4]; o += h[b & 15]; }
    return o;
}

/* ---------------------------------------------------------------- part A */

struct FieldStat {
    int declared_rows = 0;       // rows whose layout declares the field
    int reflected     = 0;       // rows where read_instance gave a non-null value
    int raw_import    = 0;       // rows where only the raw PointerRef read worked
    std::map<std::string, int> kinds;
    std::string sample;
    /* Whether the executable's reflection can describe the field's type at
     * all. A field the schema cannot type is a field this reader is BLIND to,
     * which is a different statement from "the field is empty". */
    bool schema_typed = false;
    uint8_t ftype_enum = 0;
    uint32_t offset = 0;
};

static void part_a(Source& src, TypeDb& types)
{
    std::printf("\n================ A. UIGadgetAbilityMetadata ================\n");

    /* Enumerate rather than hardcode: a partition list written by hand is a
     * count that cannot be wrong about anything except what it omits. */
    std::vector<std::string> partitions;
    for (const auto& kv : src.ebx())
        if (kv.first.find("uigadgetability") != std::string::npos)
            partitions.push_back(kv.first);
    std::sort(partitions.begin(), partitions.end());
    std::printf("partitions matching \"uigadgetability\" in the mount: %zu\n",
                partitions.size());

    std::map<uint32_t, FieldStat> stats;
    std::map<std::string, int> import_prefix_reflected, import_prefix_raw;
    std::vector<std::string> hardware_hits;
    int name_sids = 0, desc_sids = 0;
    std::map<std::string, int> sid_holder_types;
    std::map<uint32_t, std::pair<int, int>> cstring_try;   // hash -> {hits, tried}
    std::map<uint32_t, std::string> cstring_sample;
    std::map<std::string, int> atlas_names;
    std::vector<int> atlas_index;
    std::map<std::string, int> row_types;
    std::map<int, int> declared_field_counts;   // field_count -> rows
    int rows_total = 0;

    std::vector<std::string> debug_names, reward_leaves, icon_leaves, movie_leaves;

    for (const std::string& asset : partitions)
    {
        std::string err;
        std::vector<uint8_t> raw = src.get_ebx(asset + ".ebx", err);
        if (raw.empty()) raw = src.get_ebx(asset, err);
        if (raw.empty()) { std::printf("  MISSING %s\n", asset.c_str()); continue; }
        Ebx ebx(types);
        ebx.set_guid_index(&src.armory_partition_index());
        std::string e;
        if (!ebx.parse(std::move(raw), e) || ebx.instance_count() == 0)
        { std::printf("  PARSE FAILED %s: %s\n", asset.c_str(), e.c_str()); continue; }

        const EbxValue root = ebx.read_instance(0);
        const EbxValue* items = root.field(kItems);
        const size_t n = (items && items->kind == EbxValue::Kind::Array)
                       ? items->items.size() : 0;
        std::printf("  %-58s root type %s  instances %zu  Items[0x2c95d7b7] %zu\n",
                    asset.c_str(), guid_hex(ebx.instance_type(0)).c_str(),
                    ebx.instance_count(), n);
        if (!n) continue;

        int inst_refs = 0;
        for (const EbxValue& ref : items->items)
        {
            if (ref.kind != EbxValue::Kind::InstanceRef) continue;
            inst_refs++;
            if (ref.instance < 0 || ref.instance >= (int32_t)ebx.instance_count()) continue;
            const size_t ri = (size_t)ref.instance;
            const EbxValue row = ebx.read_instance(ri);
            rows_total++;

            const TypeGuid rt = ebx.instance_type(ri);
            row_types[guid_hex(rt)]++;
            const TypeLayout& lay = types.layout_full(rt);
            declared_field_counts[(int)lay.fields.size()]++;

            std::string debug;
            if (const EbxValue* d = row.field(kDebug))
                if (d->kind == EbxValue::Kind::Str) debug = d->s;
            debug_names.push_back(debug);

            /* Census over the DECLARED field set, so a field that is null on
             * every row is still visible as a field that exists. */
            for (const FieldInfo& fi : lay.fields)
            {
                FieldStat& st = stats[fi.name_hash];
                st.declared_rows++;
                st.ftype_enum = fi.ftype_enum;
                st.offset = fi.offset;
                st.schema_typed = types.resolve(fi.type_va).valid;
                const EbxValue* v = row.field(fi.name_hash);
                if (v && v->kind != EbxValue::Kind::Null)
                {
                    st.reflected++;
                    st.kinds[kind_name(v->kind)]++;
                    if (v->kind == EbxValue::Kind::ImportRef)
                    {
                        const std::string p = v->import_path.empty() ? v->s : v->import_path;
                        if (st.sample.empty()) st.sample = p;
                        size_t s1 = p.find('/'), s2 = (s1 == std::string::npos)
                            ? std::string::npos : p.find('/', s1 + 1);
                        import_prefix_reflected[s2 == std::string::npos ? p : p.substr(0, s2)]++;
                        if (p.compare(0, 16, "common/hardware/") == 0)
                            hardware_hits.push_back(debug + "  [0x" +
                                std::to_string(fi.name_hash) + "] -> " + p);
                    }
                    else if (st.sample.empty() && v->kind == EbxValue::Kind::Str)
                        st.sample = v->s;
                    continue;
                }
                /* Null under reflection is NOT proof of absence: several of
                 * these fields have a null reflection type and hold an ordinary
                 * PointerRef.  Second pass, same rows, so the two numbers below
                 * are the control for each other. */
                /* THIRD read path, for the fields both of the others miss.
                 * The reflection cannot type them, but the layout still gives
                 * an offset, and the same hashes are CString on sibling types.
                 * A CString field in this container is an i64 SELF-RELATIVE
                 * offset, so a wrong guess reads a bounded but meaningless
                 * region - which is why the same read is also run against
                 * fields the schema DOES type as something else. */
                if (row.source_pos >= 0)
                {
                    const int64_t p = row.source_pos + (int64_t)fi.offset;
                    const std::vector<uint8_t>& d = ebx.raw();
                    auto& slot = cstring_try[fi.name_hash];
                    slot.second++;
                    if (p >= 0 && p + 8 <= (int64_t)d.size())
                    {
                        int64_t off = 0;
                        std::memcpy(&off, d.data() + p, 8);
                        const int64_t loc = p + off;
                        if (off != -1 && loc > 0 && loc < (int64_t)d.size())
                        {
                            std::string s;
                            for (int64_t q = loc; q < (int64_t)d.size() && q < loc + 96; q++)
                            {
                                const unsigned char c = d[(size_t)q];
                                if (c == 0) break;
                                if (c < 0x20 || c > 0x7e) { s.clear(); break; }
                                s += (char)c;
                            }
                            if (s.size() >= 2)
                            {
                                slot.first++;
                                if (!cstring_sample.count(fi.name_hash))
                                    cstring_sample[fi.name_hash] = s;
                            }
                        }
                    }
                }

                std::string ip, ig, ipath;
                if (ebx.import_ref_field(row, fi.name_hash, ip, ig, ipath) && !ipath.empty())
                {
                    st.raw_import++;
                    st.kinds["IMPORT(raw)"]++;
                    if (st.sample.empty()) st.sample = ipath;
                    size_t s1 = ipath.find('/'), s2 = (s1 == std::string::npos)
                        ? std::string::npos : ipath.find('/', s1 + 1);
                    import_prefix_raw[s2 == std::string::npos ? ipath : ipath.substr(0, s2)]++;
                    if (ipath.compare(0, 16, "common/hardware/") == 0)
                        hardware_hits.push_back(debug + "  [raw 0x" +
                            std::to_string(fi.name_hash) + "] -> " + ipath);
                }
            }

            /* The generic UI-item record puts NameSid at +0x030 and
             * DescriptionSid at +0x078.  On THIS type the reflection cannot
             * type either, so print the raw eight bytes and let the shape
             * decide what they are rather than assuming. */
            if (row.source_pos >= 0)
            {
                const std::vector<uint8_t>& d = ebx.raw();
                /* The eight bytes are a SIGNED 32-BIT self-relative value with
                 * a zero upper half - the container's ordinary PointerRef, not
                 * a string id. Even means an INTERNAL instance, and an internal
                 * instance is exactly what import_ref_field is built to
                 * decline, which is why both fields looked empty. */
                auto follow = [&](int64_t off, const char* label, int& hits) {
                    const int64_t p = row.source_pos + off;
                    if (p < 0 || p + 4 > (int64_t)d.size()) return;
                    int32_t rel = 0;
                    std::memcpy(&rel, d.data() + p, 4);
                    if (rel == 0 || (rel & 1)) return;
                    const int64_t tgt = (p + rel) - ebx.payload();
                    int best = -1;
                    for (size_t k = 0; k < ebx.instance_count(); k++)
                        if ((int64_t)ebx.instance_offset(k) == tgt) { best = (int)k; break; }
                    if (best < 0) return;
                    const EbxValue t = ebx.read_instance((size_t)best);
                    sid_holder_types[guid_hex(ebx.instance_type((size_t)best))]++;
                    for (const auto& tf : t.fields)
                        if (tf.second.kind == EbxValue::Kind::Int ||
                            tf.second.kind == EbxValue::Kind::Uint)
                        {
                            const uint32_t sid = (uint32_t)(tf.second.kind == EbxValue::Kind::Int
                                ? (int64_t)tf.second.i : (int64_t)tf.second.u);
                            std::printf("    SID %-34s %-16s 0x%08X\n",
                                        debug.c_str(), label, sid);
                            hits++;
                            return;
                        }
                };
                follow(0x030, "NameSid", name_sids);
                follow(0x078, "DescriptionSid", desc_sids);
            }

            /* AtlasInfo interior.  Each subfield is reported UNDER ITS OWN
             * HASH.  Blending them is how an integer index read through the
             * PointerRef accessor turns into a plausible "atlas name": the
             * accessor will happily decode an int as a pointer, so which field
             * a value came from is part of the measurement. */
            if (const EbxValue* a = row.field(kAtlas))
                if (a->kind == EbxValue::Kind::Struct)
                {
                    if (atlas_names.empty() && atlas_index.empty())
                        for (const auto& sf : a->fields)
                            std::printf("      AtlasInfo subfield 0x%08x kind=%s\n",
                                        sf.first, kind_name(sf.second.kind));
                    for (const auto& sf : a->fields)
                    {
                        if (sf.second.kind == EbxValue::Kind::Int ||
                            sf.second.kind == EbxValue::Kind::Uint)
                        {
                            atlas_index.push_back((int)(sf.second.kind == EbxValue::Kind::Int
                                ? sf.second.i : (int64_t)sf.second.u));
                            continue;
                        }
                        if (sf.second.kind == EbxValue::Kind::ImportRef)
                        { atlas_names[leaf(sf.second.import_path.empty()
                            ? sf.second.s : sf.second.import_path)]++; continue; }
                        std::string p, g, path;
                        if (sf.second.kind == EbxValue::Kind::Null &&
                            ebx.import_ref_field(*a, sf.first, p, g, path) && !path.empty())
                            atlas_names[leaf(path)]++;
                    }
                }

            std::string p, g, path;
            reward_leaves.push_back(
                ebx.import_ref_field(row, kReward, p, g, path) ? leaf(path) : std::string());
            icon_leaves.push_back(
                ebx.import_ref_field(row, kIconAsset, p, g, path) ? leaf(path) : std::string());
            movie_leaves.push_back(
                ebx.import_ref_field(row, kMovie, p, g, path) ? leaf(path) : std::string());
        }
        std::printf("      InstanceRef elements: %d\n", inst_refs);
    }

    std::printf("\n  rows total: %d\n", rows_total);
    std::printf("  distinct row type guids: %zu\n", row_types.size());
    for (const auto& t : row_types) std::printf("      %s  %d rows\n", t.first.c_str(), t.second);
    std::printf("  DECLARED field count per row (layout_full):\n");
    for (const auto& kv : declared_field_counts)
        std::printf("      %d fields x %d rows\n", kv.first, kv.second);

    std::printf("\n  ---- per field: declared / reflected-nonnull / raw-import ----\n");
    std::vector<std::pair<uint32_t, FieldStat*>> ord;
    for (auto& kv : stats) ord.push_back({ kv.first, &kv.second });
    std::sort(ord.begin(), ord.end(), [](const auto& a, const auto& b) {
        return (a.second->reflected + a.second->raw_import) >
               (b.second->reflected + b.second->raw_import); });
    for (const auto& kv : ord)
    {
        std::printf("    0x%08x  +0x%03x  fte %2d  %s  decl %3d  refl %3d  raw %3d  ",
                    kv.first, kv.second->offset, kv.second->ftype_enum,
                    kv.second->schema_typed ? "typed  " : "UNTYPED",
                    kv.second->declared_rows,
                    kv.second->reflected, kv.second->raw_import);
        for (const auto& k : kv.second->kinds) std::printf("%s x%d ", k.first.c_str(), k.second);
        if (!kv.second->sample.empty()) std::printf(" e.g. %.80s", kv.second->sample.c_str());
        std::printf("\n");
    }

    int refl_tot = 0, raw_tot = 0;
    for (const auto& kv : import_prefix_reflected) refl_tot += kv.second;
    for (const auto& kv : import_prefix_raw)       raw_tot  += kv.second;
    std::printf("\n  ---- imports: reflected pass %d, raw-PointerRef pass %d, total %d ----\n",
                refl_tot, raw_tot, refl_tot + raw_tot);
    std::map<std::string, int> both = import_prefix_reflected;
    for (const auto& kv : import_prefix_raw) both[kv.first] += kv.second;
    for (const auto& kv : both)
        std::printf("    %-32s %4d  (refl %d, raw %d)\n", kv.first.c_str(), kv.second,
                    import_prefix_reflected.count(kv.first) ? import_prefix_reflected[kv.first] : 0,
                    import_prefix_raw.count(kv.first) ? import_prefix_raw[kv.first] : 0);

    std::printf("\n  ---- every import under common/hardware (the negative's control) ----\n");
    if (hardware_hits.empty()) std::printf("    NONE - and that would make the negative unprovable\n");
    for (const std::string& h : hardware_hits) std::printf("    %s\n", h.c_str());

    std::printf("\n  ---- AtlasInfo 0x651ab360 ----\n");
    for (const auto& kv : atlas_names) std::printf("    atlas %-40s x%d\n", kv.first.c_str(), kv.second);
    if (!atlas_index.empty())
    {
        std::vector<int> ix = atlas_index;
        std::sort(ix.begin(), ix.end());
        std::set<int> uniq(ix.begin(), ix.end());
        std::printf("    index values: %zu present, %zu distinct, min %d max %d\n",
                    ix.size(), uniq.size(), ix.front(), ix.back());
    }

    /* Naming convention, with a shuffled-pairing control. */
    auto score = [&](int shift) {
        int hit = 0, denom = 0;
        for (size_t i = 0; i < debug_names.size(); i++)
        {
            const std::string& r = reward_leaves[(i + shift) % reward_leaves.size()];
            if (r.empty()) continue;
            denom++;
            const std::string sn = snake(debug_names[i]);
            if (!sn.empty() && r.find(sn) != std::string::npos) hit++;
        }
        return std::pair<int, int>(hit, denom);
    };
    auto real = score(0);
    int best_ctrl = 0, ctrl_denom = 0;
    for (int s = 1; s <= 7; s++) { auto c = score(s); best_ctrl = std::max(best_ctrl, c.first);
                                   ctrl_denom = c.second; }
    std::printf("\n  ---- 0x53078b86 leaf CONTAINS snake_case(debug name) ----\n");
    std::printf("    aligned pairing : %d / %d\n", real.first, real.second);
    std::printf("    best of 7 shifted (control) : %d / %d\n", best_ctrl, ctrl_denom);
    for (size_t i = 0; i < debug_names.size(); i++)
    {
        if (reward_leaves[i].empty()) continue;
        const std::string sn = snake(debug_names[i]);
        if (reward_leaves[i].find(sn) == std::string::npos)
            std::printf("    MISS  %-30s snake=%-28s leaf=%s\n",
                        debug_names[i].c_str(), sn.c_str(), reward_leaves[i].c_str());
    }

    std::printf("\n  ---- NameSid / DescriptionSid as internal PointerRefs ----\n");
    std::printf("    rows resolving a NameSid holder        : %d / %d\n", name_sids, rows_total);
    std::printf("    rows resolving a DescriptionSid holder : %d / %d\n", desc_sids, rows_total);
    for (const auto& kv : sid_holder_types)
        std::printf("    holder type %s x%d\n", kv.first.c_str(), kv.second);

    std::printf("\n  ---- untyped fields read as a CString at their declared offset ----\n");
    std::printf("  (sibling types in the same UI*Description family type 0xFD698F51"
                " as CString, so the bytes are worth reading; the control is the"
                " same read at offsets the schema types as something else)\n");
    for (const auto& kv : cstring_try)
        std::printf("    0x%08x  %3d / %3d rows yield a printable string   e.g. %s\n",
                    kv.first, kv.second.first, kv.second.second,
                    cstring_sample.count(kv.first) ? cstring_sample[kv.first].c_str() : "");

    /* Do the seasonal partitions SUPERSEDE the base table or EXTEND it? A
     * superseding partition would restate rows the base already has. */
    {
        std::map<std::string, int> dup;
        for (const std::string& d : debug_names) dup[d]++;
        int repeated = 0;
        for (const auto& kv : dup) if (kv.second > 1) { repeated++;
            std::printf("  DEBUG NAME APPEARS %d TIMES: %s\n", kv.second, kv.first.c_str()); }
        std::printf("  distinct debug names: %zu of %d rows; repeated: %d\n",
                    dup.size(), rows_total, repeated);
    }

    std::printf("\n  ---- 0x17ba57a2 tutorial-movie leaf, leading token census ----\n");
    std::map<std::string, int> movie_tok;
    for (const std::string& m : movie_leaves)
    {
        if (m.empty()) continue;
        size_t dash = m.find('_');
        std::string tail = (dash == std::string::npos) ? m : m.substr(dash + 1);
        size_t d2 = tail.find('-');
        movie_tok[d2 == std::string::npos ? tail : tail.substr(0, d2)]++;
    }
    for (const auto& kv : movie_tok) std::printf("    %-24s %d\n", kv.first.c_str(), kv.second);

    std::printf("\n  ---- 0x1b9640bb icon leaf, first token after t_ui_ ----\n");
    for (size_t i = 0; i < icon_leaves.size(); i++)
    {
        if (icon_leaves[i].empty()) continue;
        std::string t = icon_leaves[i];
        if (t.compare(0, 5, "t_ui_") == 0) t = t.substr(5);
        size_t u = t.find('_');
        std::printf("    %-30s -> %s\n", debug_names[i].c_str(),
                    (u == std::string::npos ? t : t.substr(0, u)).c_str());
    }
}

/* ---------------------------------------------------------------- part B */

static void part_b(Source& src, TypeDb& types)
{
    std::printf("\n================ B. common/gameplay/loadouts/loadout_mp_* ================\n");
    std::vector<std::string> templates;
    for (const auto& kv : src.ebx())
        if (kv.first.find("loadouts/loadout_mp_") != std::string::npos)
            templates.push_back(kv.first);
    std::sort(templates.begin(), templates.end());
    std::printf("templates in the mount: %zu\n", templates.size());

    /* The denominator the "38 slot assets" number should be read against. */
    int all_slots = 0, inf_slots = 0;
    for (const auto& kv : src.ebx())
    {
        if (kv.first.find("/equipmentslot_") == std::string::npos) continue;
        all_slots++;
        if (kv.first.compare(0, 26, "common/gameplay/loadouts/e") == 0) inf_slots++;
    }
    std::printf("equipmentslot_* partitions in the mount: %d, of which directly under"
                " common/gameplay/loadouts/: %d\n", all_slots, inf_slots);

    int with_slots = 0, slot_rows = 0, defaults_resolved = 0, with_fieldupgrade = 0;
    std::map<int, int> slot_counts;
    std::map<std::string, int> default_prefix;
    std::set<std::string> distinct_slots;
    std::map<std::string, std::string> fieldupgrade_of;

    for (const std::string& t : templates)
    {
        std::string err;
        std::vector<uint8_t> raw = src.get_ebx(t + ".ebx", err);
        if (raw.empty()) raw = src.get_ebx(t, err);
        if (raw.empty()) { std::printf("  MISSING %s\n", t.c_str()); continue; }
        Ebx ebx(types);
        ebx.set_guid_index(&src.armory_partition_index());
        std::string e;
        if (!ebx.parse(std::move(raw), e) || ebx.instance_count() == 0)
        { std::printf("  PARSE FAILED %s\n", t.c_str()); continue; }

        const EbxValue root = ebx.read_instance(0);
        std::string fp, fg, fpath;
        const bool fu = ebx.import_ref_field(root, kFieldUpg, fp, fg, fpath) && !fpath.empty();
        if (fu) with_fieldupgrade++;

        /* Find every array on the root and report which one holds the slots,
         * rather than assuming the claimed field is the only candidate. */
        std::printf("\n  %s\n", t.c_str());
        std::printf("    root type %s   fieldupgrade[0xFF7A648F] = %s\n",
                    guid_hex(ebx.instance_type(0)).c_str(), fu ? leaf(fpath).c_str() : "(none)");
        for (const auto& f : root.fields)
        {
            if (f.second.kind != EbxValue::Kind::Array) continue;
            std::printf("    array 0x%08x : %zu elements\n", f.first, f.second.items.size());
        }

        /* The slots array: elements that are imports to equipmentslot_* assets. */
        int here = 0, here_def = 0;
        for (const auto& f : root.fields)
        {
            if (f.second.kind != EbxValue::Kind::Array || f.second.items.empty()) continue;
            for (size_t i = 0; i < f.second.items.size(); i++)
            {
                const EbxValue& el = f.second.items[i];
                std::string sp, sg, spath;
                if (el.kind == EbxValue::Kind::ImportRef)
                    spath = el.import_path.empty() ? el.s : el.import_path;
                std::string dp, dg, dpath;
                if (el.kind == EbxValue::Kind::Struct)
                {
                    for (const auto& sf : el.fields)
                    {
                        std::string a, b, c;
                        if (ebx.import_ref_field(el, sf.first, a, b, c) && !c.empty())
                        {
                            if (c.find("equipmentslot") != std::string::npos) spath = c;
                            else if (sf.first == kSlots) dpath = c;
                            else if (dpath.empty()) dpath = c;
                        }
                    }
                }
                if (spath.empty() && dpath.empty()) continue;
                here++;
                if (!spath.empty())
                {
                    std::string clean = spath;
                    if (clean.size() > 4 && clean.compare(clean.size() - 4, 4, ".ebx") == 0)
                        clean.resize(clean.size() - 4);
                    distinct_slots.insert(clean);
                }
                if (!dpath.empty())
                {
                    here_def++;
                    size_t s1 = dpath.find('/'), s2 = (s1 == std::string::npos)
                        ? std::string::npos : dpath.find('/', s1 + 1);
                    default_prefix[s2 == std::string::npos ? dpath : dpath.substr(0, s2)]++;
                }
                std::printf("      %zu  %-52s -> %s\n", i,
                            leaf(spath).c_str(), dpath.empty() ? "(no default)" : dpath.c_str());
            }
            if (here) break;
        }
        if (here) { with_slots++; slot_counts[here]++; slot_rows += here; defaults_resolved += here_def; }
    }

    /* The loadout row names SLOT ASSETS, not equipment.  If the default lives
     * one hop away, it is on the equipmentslot_* partition and 0xBF67CCC6 is a
     * field there, not on the loadout.  Follow every distinct slot named above
     * and read it. */
    std::printf("\n  ---- one hop: equipmentslot_* partitions, field 0xBF67CCC6 ----\n");
    int slots_read = 0, slots_with_default = 0;
    std::map<std::string, int> eq_prefix;
    for (const std::string& sname : distinct_slots)
    {
        std::string err;
        std::vector<uint8_t> raw = src.get_ebx(sname + ".ebx", err);
        if (raw.empty()) raw = src.get_ebx(sname, err);
        if (raw.empty()) { std::printf("    MISSING %s\n", sname.c_str()); continue; }
        Ebx ebx(types);
        ebx.set_guid_index(&src.armory_partition_index());
        std::string e;
        if (!ebx.parse(std::move(raw), e) || ebx.instance_count() == 0) continue;
        slots_read++;
        const EbxValue root = ebx.read_instance(0);
        std::string p, g, path;
        const bool ok = ebx.import_ref_field(root, kSlots, p, g, path) && !path.empty();
        if (ok)
        {
            slots_with_default++;
            size_t s1 = path.find('/'), s2 = (s1 == std::string::npos)
                ? std::string::npos : path.find('/', s1 + 1);
            eq_prefix[s2 == std::string::npos ? path : path.substr(0, s2)]++;
        }
        std::printf("    %-56s -> %s\n", leaf(sname).c_str(), ok ? path.c_str() : "(none)");
    }
    std::printf("    slot partitions read: %d / %zu, with 0xBF67CCC6: %d\n",
                slots_read, distinct_slots.size(), slots_with_default);
    for (const auto& kv : eq_prefix)
        std::printf("    default target prefix %-28s %d\n", kv.first.c_str(), kv.second);

    /* Control for "0xBF67CCC6 is the default equipment field": the identical
     * read against a hash that is not a field on this type must return nothing
     * on every one of the same partitions. */
    {
        int false_hits = 0;
        for (const std::string& sname : distinct_slots)
        {
            std::string err;
            std::vector<uint8_t> raw = src.get_ebx(sname + ".ebx", err);
            if (raw.empty()) continue;
            Ebx ebx(types);
            ebx.set_guid_index(&src.armory_partition_index());
            std::string e;
            if (!ebx.parse(std::move(raw), e) || ebx.instance_count() == 0) continue;
            const EbxValue root = ebx.read_instance(0);
            std::string p, g, path;
            if (ebx.import_ref_field(root, 0xBF67CCC7u, p, g, path) && !path.empty())
                false_hits++;
        }
        std::printf("    control, neighbouring hash 0xBF67CCC7 on the same %zu"
                    " partitions: %d resolve\n", distinct_slots.size(), false_hits);
    }

    std::printf("\n  templates with a slot array : %d / %zu\n", with_slots, templates.size());
    std::printf("  templates with 0xFF7A648F   : %d / %zu\n", with_fieldupgrade, templates.size());
    std::printf("  slot rows total             : %d\n", slot_rows);
    std::printf("  slot rows with a default    : %d\n", defaults_resolved);
    for (const auto& kv : slot_counts)
        std::printf("  slot-count histogram: %d slots x %d templates\n", kv.first, kv.second);
    for (const auto& kv : default_prefix)
        std::printf("  default equipment prefix: %-28s %d\n", kv.first.c_str(), kv.second);

    /* In-scope control for the "0xBF67CCC6 is the default" claim: the same walk
     * over a name that does not exist must return nothing, and the same walk
     * over a REAL sibling family that is not a loadout must not be mistaken
     * for one. */
    {
        Ebx probe(types);
        std::string err, e;
        std::vector<uint8_t> raw = src.get_ebx(
            "common/gameplay/loadouts/loadout_mp_assault_1__control.ebx", err);
        std::printf("\n  control, nonexistent partition name: %s\n",
                    raw.empty() ? "0 bytes (correct)" : "RETURNED DATA - the lookup is not exact");
    }
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: gadget_loadout_audit_probe <game_dir>\n"); return 2; }
    Source src;
    std::string err;
    if (!src.open(argv[1], err)) { std::fprintf(stderr, "open: %s\n", err.c_str()); return 1; }
    if (!src.mount_level(std::string(), false, err))
    { std::fprintf(stderr, "mount: %s\n", err.c_str()); return 1; }
    std::string fe;
    src.mount_frontend(fe);
    std::printf("mounted EBX partitions: %zu\n", src.ebx_count());

    TypeDb types;
    bool typed = false, opened = false;
    for (const std::string& cand : TypeDb::exe_candidates(src.game_dir()))
    {
        std::string te;
        if (!types.open(cand, te)) continue;
        opened = true;
        if (types.looks_encrypted()) continue;
        typed = true;
        std::printf("type schema: %s\n", cand.c_str());
        break;
    }
    if (!typed)
    {
        std::fprintf(stderr, "%s\n", opened ? "type table encrypted" : "no readable executable");
        return 1;
    }
    part_a(src, types);
    part_b(src, types);
    return 0;
}
