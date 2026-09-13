/* frontend_named_link_census - does "resolve a named LinkConnections edge to a
 * transform" generalise past one prefab?
 *
 * bf6_frontend_named_transform_read answers ONE (partition, name) question and
 * fails closed. That proves nothing about the population: a rule that resolves
 * uniquely on the single partition it was written against is indistinguishable
 * from a coincidence. This walks every mounted partition, applies the identical
 * rule to EVERY authored link edge, and reports how many (partition, name-hash)
 * keys resolve to exactly one LinearTransform-carrying endpoint, how many are
 * ambiguous, and what a mutated-hash control scores over the same population.
 *
 *   frontend_named_link_census <game_dir> [--all] [--substr S] [--max N]
 *                              [--per-key] [--xor MASK]
 *
 * --all mounts every level instead of the front-end archive families.
 */
#include "source.h"
#include "types.h"
#include "ebx.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>
using namespace bf6;

namespace {

constexpr uint32_t kLinkConnections = 0xF7791E41u;
constexpr uint32_t kLinkSource      = 0x4676BA2Cu;
constexpr uint32_t kLinkTarget      = 0x645B319Cu;
constexpr uint32_t kLinkSourceField = 0xC41FB636u;
constexpr uint32_t kLinkTargetField = 0x2D7314C9u;
constexpr uint32_t kTransform       = 0xD6351EDEu;
constexpr uint32_t kLtRight         = 0xC478CC3Bu;
constexpr uint32_t kLtUp            = 0xBF151EF9u;
constexpr uint32_t kLtForward       = 0x695D12A4u;
constexpr uint32_t kLtTranslation   = 0xBC4B07B4u;
constexpr uint32_t kVecX            = 956422932u;
constexpr uint32_t kVecY            = 1123815262u;
constexpr uint32_t kVecZ            = 849976220u;

int ref_of(const EbxValue* v)
{
    return v && v->kind == EbxValue::Kind::InstanceRef ? v->instance : -1;
}

bool u32_of(const EbxValue* v, uint32_t& out)
{
    if (!v) return false;
    if (v->kind == EbxValue::Kind::Uint) { out = (uint32_t)v->u; return true; }
    if (v->kind == EbxValue::Kind::Int)  { out = (uint32_t)v->i; return true; }
    return false;
}

bool f_of(const EbxValue* v, float& out)
{
    if (!v || v->kind != EbxValue::Kind::Real) return false;
    out = (float)v->f;
    return std::isfinite(out);
}

bool vec3_of(const EbxValue* v, float out[3])
{
    return v && v->kind == EbxValue::Kind::Struct &&
        f_of(v->field(kVecX), out[0]) && f_of(v->field(kVecY), out[1]) &&
        f_of(v->field(kVecZ), out[2]);
}

bool transform_of(const EbxValue& inst, float out[12])
{
    const EbxValue* t = inst.field(kTransform);
    if (!t || t->kind != EbxValue::Kind::Struct) return false;
    const uint32_t cols[4] = { kLtRight, kLtUp, kLtForward, kLtTranslation };
    for (int c = 0; c < 4; ++c)
        if (!vec3_of(t->field(cols[c]), out + c * 3)) return false;
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: frontend_named_link_census <game_dir> [--all]"
                    " [--substr S] [--max N] [--per-key] [--xor MASK]\n");
        return 2;
    }
    const char* game = argv[1];
    bool all = false, per_key = false;
    std::string substr;
    int maxp = 0;
    uint32_t mask = 0xA5A5A5A5u;
    uint32_t find_hash = 0;
    for (int i = 2; i < argc; ++i) {
        if      (!std::strcmp(argv[i], "--all")) all = true;
        else if (!std::strcmp(argv[i], "--per-key")) per_key = true;
        else if (!std::strcmp(argv[i], "--substr") && i + 1 < argc) substr = argv[++i];
        else if (!std::strcmp(argv[i], "--max") && i + 1 < argc) maxp = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--find") && i + 1 < argc)
            find_hash = (uint32_t)std::strtoul(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--xor") && i + 1 < argc)
            mask = (uint32_t)std::strtoul(argv[++i], nullptr, 0);
    }

    Source src; std::string err;
    if (!src.open(game, err)) { std::printf("open: %s\n", err.c_str()); return 1; }
    if (all) {
        if (!src.mount_level("", true, err)) { std::printf("mount: %s\n", err.c_str()); return 1; }
    } else {
        if (!src.mount_frontend(err)) { std::printf("mount: %s\n", err.c_str()); return 1; }
    }

    std::vector<std::string> hits;
    for (const auto& kv : src.ebx())
        if (substr.empty() || kv.first.find(substr) != std::string::npos)
            hits.push_back(kv.first);
    std::sort(hits.begin(), hits.end());

    TypeDb types;
    bool got = false;
    for (const std::string& e : TypeDb::exe_candidates(game)) {
        std::string te;
        if (types.open(e, te)) { got = true; break; }
    }
    if (!got) { std::printf("no usable executable type db\n"); return 1; }

    long parsed = 0, failed = 0;
    long link_partitions = 0, link_roots = 0, link_edges = 0;
    long edges_src_named = 0, edges_tgt_named = 0, edges_both_named = 0,
         edges_none_named = 0, edges_bad_endpoint = 0;
    long edges_one_named = 0, edges_opposite_transform = 0;
    long edges_named_side_transform = 0;   /* polarity control */
    long control_edges_opposite_transform = 0;
    long link_partition_instances = 0, link_partition_transforms = 0;
    int  max_transforms_in_a_link_partition = 0;

    /* key = (partition, name hash) -> transform-carrying resolutions */
    struct Hit { int count = 0; int controller = -1; int transform = -1;
                 float origin[3] = {}; };
    std::map<std::pair<std::string, uint32_t>, Hit> hits_by_key;
    std::map<std::pair<std::string, uint32_t>, int> real_keys;
    std::map<std::pair<std::string, uint32_t>, int> ctrl_keys;
    std::map<uint32_t, long> hash_edges;   /* name hash -> named edges */
    /* Every nonzero field id seen in ANY position, including the both-ends-named
     * edges the resolution rule skips. Needed before saying a hash is absent:
     * "the resolver never returned it" and "the data never carries it" are
     * different claims, and only the second is a negative worth publishing. */
    std::map<uint32_t, long> raw_field_ids;

    for (const std::string& name : hits) {
        if (maxp && parsed >= maxp) break;
        std::string e;
        std::vector<uint8_t> b = src.get_ebx(name, e);
        if (b.empty()) { ++failed; continue; }
        Ebx x(types);
        x.set_guid_index(&src.partition_index());
        if (!x.parse(std::move(b), e)) { ++failed; continue; }
        ++parsed;

        bool partition_has_links = false;
        for (size_t inst = 0; inst < x.instance_count(); ++inst) {
            const EbxValue root = x.read_instance(inst);
            const EbxValue* links = root.field(kLinkConnections);
            if (!links || links->kind != EbxValue::Kind::Array) continue;
            if (links->items.empty()) continue;
            partition_has_links = true;
            ++link_roots;
            for (const EbxValue& link : links->items) {
                ++link_edges;
                uint32_t sf = 0, tf = 0;
                const bool have_sf = u32_of(link.field(kLinkSourceField), sf);
                const bool have_tf = u32_of(link.field(kLinkTargetField), tf);
                const int s = ref_of(link.field(kLinkSource));
                const int t = ref_of(link.field(kLinkTarget));
                const bool sn = have_sf && sf != 0;
                const bool tn = have_tf && tf != 0;
                if (sn) raw_field_ids[sf]++;
                if (tn) raw_field_ids[tf]++;
                if (find_hash && ((sn && sf == find_hash) || (tn && tf == find_hash)))
                    std::printf("F\t%s\troot=%zu\tsrc=%d\ttgt=%d"
                                "\tsf=0x%08X\ttf=0x%08X\n",
                                name.c_str(), inst, s, t, sf, tf);
                if (sn && tn) ++edges_both_named;
                else if (sn) ++edges_src_named;
                else if (tn) ++edges_tgt_named;
                else ++edges_none_named;
                if (s < 0 || t < 0) { ++edges_bad_endpoint; continue; }
                /* Mirror the reader EXACTLY: it is asked for ONE hash and
                 * requires that hash to sit on exactly one end. An edge whose
                 * two ends carry two different names is therefore a candidate
                 * for BOTH of them, and an edge carrying one name on both ends
                 * is a candidate for neither. Collapsing this to "one named
                 * side" undercounts - it silently drops every edge that names
                 * a source slot AND a target slot. */
                uint32_t cand_hash[2] = { sf, tf };
                for (int k = 0; k < 2; ++k) {
                    const uint32_t hash = cand_hash[k];
                    if (hash == 0) continue;
                    if (k == 1 && tf == sf) continue;   /* same query twice */
                    const bool source_named = (sf == hash);
                    const bool target_named = (tf == hash);
                    if (source_named == target_named) continue;
                    ++edges_one_named;
                    hash_edges[hash]++;
                    const int cand = source_named ? t : s;
                    if (cand < 0 || cand >= (int)x.instance_count()) continue;
                    float m[12] = {};
                    if (!transform_of(x.read_instance((size_t)cand), m)) continue;
                    ++edges_opposite_transform;
                    real_keys[{name, hash}]++;
                    ctrl_keys[{name, hash ^ mask}]++;
                    Hit& hit = hits_by_key[{name, hash}];
                    hit.count++;
                    hit.controller = source_named ? s : t;
                    hit.transform = cand;
                    hit.origin[0] = m[9];
                    hit.origin[1] = m[10];
                    hit.origin[2] = m[11];
                    /* POLARITY CONTROL: read the endpoint that CARRIES the
                     * name instead of the one opposite it. If that side were
                     * just as likely to own a LinearTransform, "opposite" would
                     * be decoration rather than the rule. */
                    const int same = source_named ? s : t;
                    float sm[12] = {};
                    if (same >= 0 && same < (int)x.instance_count() &&
                        transform_of(x.read_instance((size_t)same), sm))
                        ++edges_named_side_transform;
                }
                (void)sn; (void)tn;
            }
        }
        if (partition_has_links) {
            ++link_partitions;
            /* How many instances in this partition could a positional guess
             * have picked? That is the size of the problem the name solves. */
            int here = 0;
            for (size_t inst = 0; inst < x.instance_count(); ++inst) {
                float m[12] = {};
                if (transform_of(x.read_instance(inst), m)) ++here;
            }
            link_partition_instances += (long)x.instance_count();
            link_partition_transforms += here;
            max_transforms_in_a_link_partition =
                std::max(max_transforms_in_a_link_partition, here);
        }
    }

    /* The control is not "does a fake hash exist" - it is "would the SAME
     * resolver, asked for a mutated name, hand back a transform". Every real
     * resolution above also registered its mutated key; a mutated key only
     * scores if some OTHER edge genuinely carries that hash. */
    for (const auto& kv : ctrl_keys)
        if (real_keys.count(kv.first)) control_edges_opposite_transform += kv.second;

    long unique_keys = 0, ambiguous_keys = 0;
    for (const auto& kv : real_keys)
        (kv.second == 1 ? unique_keys : ambiguous_keys)++;

    std::printf("# scope %s  substr \"%s\"\n", all ? "all-levels" : "frontend",
                substr.c_str());
    std::printf("partitions matched %zu  parsed %ld  failed %ld\n",
                hits.size(), parsed, failed);
    std::printf("partitions carrying LinkConnections %ld  link roots %ld"
                "  link edges %ld\n", link_partitions, link_roots, link_edges);
    std::printf("edges: src-named-only %ld  tgt-named-only %ld  both-named %ld"
                "  neither %ld  bad-endpoint %ld\n", edges_src_named,
                edges_tgt_named, edges_both_named, edges_none_named,
                edges_bad_endpoint);
    std::printf("edges with exactly one named side %ld;"
                " opposite endpoint carries LinearTransform %ld\n",
                edges_one_named, edges_opposite_transform);
    std::printf("(partition,name) keys resolving to a transform %zu:"
                "  unique %ld  ambiguous %ld\n",
                real_keys.size(), unique_keys, ambiguous_keys);
    std::printf("polarity control - NAMED side carries a LinearTransform %ld"
                " of the same %ld\n", edges_named_side_transform,
                edges_opposite_transform);
    std::printf("mutated-hash (^0x%08X) keys colliding with a real key %ld\n",
                mask, control_edges_opposite_transform);
    std::printf("link-bearing partitions: instances %ld  of which carry a"
                " LinearTransform %ld  (max in one partition %d)\n",
                link_partition_instances, link_partition_transforms,
                max_transforms_in_a_link_partition);
    std::printf("distinct named-link hashes %zu (resolvable rule);"
                " distinct nonzero field ids in ANY position %zu\n",
                hash_edges.size(), raw_field_ids.size());

    if (per_key) {
        std::printf("key\tpartition\tname_hash\tresolutions\tcontroller"
                    "\ttransform\tox\toy\toz\n");
        for (const auto& kv : real_keys) {
            const Hit& h = hits_by_key[kv.first];
            std::printf("K\t%s\t0x%08X\t%d\t%d\t%d\t%.9g\t%.9g\t%.9g\n",
                        kv.first.first.c_str(), kv.first.second, kv.second,
                        h.controller, h.transform,
                        h.origin[0], h.origin[1], h.origin[2]);
        }
        std::printf("hash\tname_hash\tnamed_edges\n");
        for (const auto& kv : hash_edges)
            std::printf("H\t0x%08X\t%ld\n", kv.first, kv.second);
        std::printf("raw\tfield_id\toccurrences\n");
        for (const auto& kv : raw_field_ids)
            std::printf("R\t0x%08X\t%ld\n", kv.first, kv.second);
    }
    return parsed > 0 ? 0 : 1;
}
