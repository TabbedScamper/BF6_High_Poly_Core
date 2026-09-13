/* Every field on every UIGadgetAbilityMetadata row, so the gadget join can
 * stop being a name-similarity score.
 *
 * bf6_gadget_ui_metadata_rows reads 8 of the row's 31 fields, and the viewer
 * joins those rows to the armory roster by scoring token overlap between the
 * debug name and the icon path. That join currently resolves 53 of 110 roster
 * rows. A scored join is the right fallback but the wrong primary: if the row
 * carries an authored reference to its gadget, the join is exact and the score
 * is unnecessary.
 *
 * This reports, over every row in every metadata partition:
 *   - per field hash: how many rows populate it, and the kinds seen
 *   - every ImportRef path, bucketed by top-level prefix, so an authored link
 *     into common/hardware would be impossible to miss
 *   - the rows whose Name and IconAsset are BOTH absent, which are the rows a
 *     name/icon score has nothing to work with
 *
 *   gadget_metadata_fields_probe <game_dir>
 */
#include "ebx.h"
#include "source.h"
#include "types.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace bf6;

static const uint32_t kItems       = 0x2c95d7b7u;
static const uint32_t kDebug       = 0x55aded8du;
static const uint32_t kName        = 0xfd698f51u;
static const uint32_t kIconAsset   = 0x1b9640bbu;

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

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: gadget_metadata_fields_probe <game_dir>\n");
        return 2;
    }
    Source src;
    std::string err;
    if (!src.open(argv[1], err)) { std::fprintf(stderr, "open: %s\n", err.c_str()); return 1; }
    if (!src.mount_level(std::string(), false, err))
    { std::fprintf(stderr, "mount: %s\n", err.c_str()); return 1; }

    /* The type db is NOT optional: without it every field layout is unresolved
     * and the root's Items array reads as absent, which looks exactly like an
     * empty table. Same candidate walk bf6_core uses - first that OPENS is not
     * first that WORKS, because an encrypted EA App build opens and yields
     * nothing. */
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
        std::fprintf(stderr, "%s\n", opened
            ? "this install's type table is encrypted (EA App build)"
            : "no readable executable for the type schema");
        return 1;
    }
    const char* assets[] = {
        "common/ui/static/metadata/uigadgetabilitymetadata",
        "common/ui/static/metadata/uigadgetabilitymetadata_s2",
        "common/ui/static/metadata/s3/uigadgetabilitymetadata_s3",
    };

    struct FieldStat {
        int populated = 0;
        std::map<std::string, int> kinds;
        std::string sample;
    };
    std::map<uint32_t, FieldStat> stats;
    std::map<std::string, int> import_prefix;
    std::map<uint32_t, std::vector<std::string>> hardware_imports;
    int rows_total = 0, rows_no_name_no_icon = 0, icon_assets_seen = 0;
    int item_id_rows = 0, item_id_examples = 0;
    size_t item_id_total = 0;
    std::set<uint32_t> item_ids_seen;
    std::vector<std::string> blind_rows;

    for (const char* asset : assets)
    {
        std::vector<uint8_t> raw = src.get_ebx(std::string(asset) + ".ebx", err);
        if (raw.empty()) raw = src.get_ebx(asset, err);
        if (raw.empty()) { std::printf("MISSING %s\n", asset); continue; }
        Ebx ebx(types);
        ebx.set_guid_index(&src.armory_partition_index());
        std::string e;
        if (!ebx.parse(std::move(raw), e) || ebx.instance_count() == 0)
        { std::printf("PARSE FAILED %s: %s\n", asset, e.c_str()); continue; }

        const EbxValue root = ebx.read_instance(0);
        const EbxValue* items = root.field(kItems);
        if (!items || items->kind != EbxValue::Kind::Array)
        { std::printf("NO ITEMS %s\n", asset); continue; }
        std::printf("%s : %zu rows\n", asset, items->items.size());

        for (const EbxValue& ref : items->items)
        {
            if (ref.kind != EbxValue::Kind::InstanceRef || ref.instance < 0 ||
                ref.instance >= (int32_t)ebx.instance_count()) continue;
            const EbxValue row = ebx.read_instance((size_t)ref.instance);
            rows_total++;

            std::string debug;
            const EbxValue* d = row.field(kDebug);
            if (d && d->kind == EbxValue::Kind::Str) debug = d->s;

            /* ItemIds (0x1636B8BD) is on 82/82 rows and is the only field that
             * could carry an EXACT roster join. If the union of these ids is
             * close to the 110-row gadget roster, then one metadata row serves
             * several roster items and "55 of 110" is being measured against
             * the wrong denominator. */
            {
                const EbxValue* ids = row.field(0x1636b8bdu);
                if (ids && ids->kind == EbxValue::Kind::Array)
                {
                    item_id_rows++;
                    item_id_total += ids->items.size();
                    std::string joined;
                    for (const EbxValue& e : ids->items)
                    {
                        uint64_t v = e.kind == EbxValue::Kind::Uint ? e.u
                                   : e.kind == EbxValue::Kind::Int  ? (uint64_t)e.i
                                   : 0;
                        item_ids_seen.insert((uint32_t)v);
                        joined += " " + std::to_string((unsigned long)v);
                    }
                    if (item_id_examples < 6)
                    {
                        item_id_examples++;
                        std::printf("IDS %-26s n=%zu :%s\n", debug.c_str(),
                                    ids->items.size(), joined.c_str());
                    }
                }
            }

            const EbxValue* nm = row.field(kName);
            const bool has_name = nm && nm->kind != EbxValue::Kind::Null;
            std::string np, ng, icon_path;
            const bool has_icon =
                ebx.import_ref_field(row, kIconAsset, np, ng, icon_path) &&
                !icon_path.empty();
            if (has_icon) icon_assets_seen++;
            /* The identity strings a join could score on, side by side, so the
             * question "would another field join more rows" is answerable
             * without guessing at the naming convention. */
            {
                std::string p2, g2, reward, p3, g3, movie;
                ebx.import_ref_field(row, 0x53078b86u, p2, g2, reward);
                ebx.import_ref_field(row, 0x17ba57a2u, p3, g3, movie);
                auto leaf = [](const std::string& s) {
                    size_t a = s.find_last_of('/');
                    std::string t = a == std::string::npos ? s : s.substr(a + 1);
                    if (t.size() > 4 && t.compare(t.size() - 4, 4, ".ebx") == 0)
                        t.resize(t.size() - 4);
                    return t;
                };
                std::printf("ROW %-26s | icon=%-46s | reward=%-34s | movie=%s\n",
                            debug.c_str(), leaf(icon_path).c_str(),
                            leaf(reward).c_str(), leaf(movie).c_str());
            }
            if (!has_name && !has_icon)
            {
                rows_no_name_no_icon++;
                if (blind_rows.size() < 30)
                    blind_rows.push_back(debug.empty() ? "(no debug name)" : debug);
            }

            /* A FIELD THAT READS Null IS NOT AN ABSENT IMPORT.
             *
             * Several of these rows carry their icon as a raw PointerRef whose
             * executable reflection type is null. Reflected-kind inspection
             * reports those as Null and a probe that skips them concludes the
             * table has no imports at all - which is wrong, and is exactly the
             * false negative this pass was written to avoid. import_ref_field
             * is the same accessor bf6_gadget_ui_metadata_rows uses, so this
             * sees precisely what the shipping decoder sees. */
            for (const auto& f : row.fields)
            {
                std::string ip, ig, ipath;
                if (f.second.kind == EbxValue::Kind::Null &&
                    ebx.import_ref_field(row, f.first, ip, ig, ipath) &&
                    !ipath.empty())
                {
                    FieldStat& raw = stats[f.first];
                    raw.populated++;
                    raw.kinds["IMPORT(raw)"]++;
                    if (raw.sample.empty()) raw.sample = ipath;
                    const size_t s1 = ipath.find('/');
                    const size_t s2 = s1 == std::string::npos
                        ? std::string::npos : ipath.find('/', s1 + 1);
                    import_prefix[s2 == std::string::npos ? ipath
                                                          : ipath.substr(0, s2)]++;
                    if (ipath.compare(0, 16, "common/hardware/") == 0)
                        hardware_imports[f.first].push_back(debug + " -> " + ipath);
                }
                if (f.second.kind == EbxValue::Kind::Null) continue;
                FieldStat& st = stats[f.first];
                st.populated++;
                st.kinds[kind_name(f.second.kind)]++;
                if (f.second.kind == EbxValue::Kind::ImportRef)
                {
                    const std::string p = f.second.import_path.empty()
                                        ? f.second.s : f.second.import_path;
                    if (st.sample.empty()) st.sample = p;
                    const size_t slash = p.find('/');
                    const size_t slash2 = slash == std::string::npos
                        ? std::string::npos : p.find('/', slash + 1);
                    import_prefix[slash2 == std::string::npos ? p
                                                              : p.substr(0, slash2)]++;
                    if (p.compare(0, 16, "common/hardware/") == 0)
                        hardware_imports[f.first].push_back(debug + " -> " + p);
                }
                else if (st.sample.empty() && f.second.kind == EbxValue::Kind::Str)
                    st.sample = f.second.s;
            }
        }
    }

    std::printf("\n==== %d rows ====\n", rows_total);
    std::printf("rows with NEITHER a localized Name NOR an IconAsset: %d\n",
                rows_no_name_no_icon);
    for (const std::string& b : blind_rows) std::printf("    %s\n", b.c_str());

    std::printf("\n---- field population (of %d rows) ----\n", rows_total);
    std::vector<std::pair<uint32_t, FieldStat*>> ordered;
    for (auto& kv : stats) ordered.push_back({ kv.first, &kv.second });
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& a, const auto& b)
              { return a.second->populated > b.second->populated; });
    for (const auto& kv : ordered)
    {
        std::printf("  0x%08x  %4d rows  ", kv.first, kv.second->populated);
        for (const auto& k : kv.second->kinds)
            std::printf("%s x%d ", k.first.c_str(), k.second);
        if (!kv.second->sample.empty())
            std::printf(" e.g. %.90s", kv.second->sample.c_str());
        std::printf("\n");
    }

    std::printf("\n---- ItemIds (0x1636B8BD) ----\n");
    std::printf("rows carrying the array=%d   total ids=%zu   DISTINCT ids=%zu\n",
                item_id_rows, item_id_total, item_ids_seen.size());
    std::printf("If the distinct count lands near the gadget roster size, one\n"
                "metadata row serves several roster items and a per-roster-row\n"
                "join rate is being measured against the wrong denominator.\n");

    std::printf("\n---- ImportRef targets by prefix ----\n");
    for (const auto& kv : import_prefix)
        std::printf("  %-52s %d\n", kv.first.c_str(), kv.second);

    std::printf("\n---- imports into common/hardware (the authored link, if any) ----\n");
    if (hardware_imports.empty())
        std::printf("  NONE. No row references a hardware asset directly, so an\n"
                    "  exact join cannot be made from this partition alone and the\n"
                    "  scored join is not merely a shortcut.\n");
    for (const auto& kv : hardware_imports)
    {
        std::printf("  field 0x%08x : %zu rows\n", kv.first, kv.second.size());
        for (size_t i = 0; i < kv.second.size() && i < 12; i++)
            std::printf("      %s\n", kv.second[i].c_str());
    }
    return 0;
}
