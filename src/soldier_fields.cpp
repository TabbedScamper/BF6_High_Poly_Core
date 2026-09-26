#include "soldier_fields.h"

#include "ebx.h"
#include "source.h"
#include "types.h"

#include <cstring>
#include <vector>

namespace bf6 {

namespace {

const char* const kAsset = "common/gameplay/soldier/soldiermotionmachine";
/* The four field kinds, by the instance type that declares them in the asset. */
struct KindType { const char* guid; int kind; };
const KindType kKinds[] = {
    {"45fa42b3-f08a-fd87-7951-1bcb8f41a5ba", SoldierFields::kBool},   /* 391 fields */
    {"87064bc2-47ce-d8a7-1080-1c04f8461d1c", SoldierFields::kFloat},  /*  85 fields */
    {"1eed978d-3fd5-6baa-b88f-be30bbbc61ed", SoldierFields::kInt},    /*  76 fields */
    {"70d5f40c-24e8-57aa-943d-784840b38efc", SoldierFields::kVec},    /*  19 fields */
    {"34a56106-7b27-37b7-a4e9-28a040cfa9a2", SoldierFields::kXform},  /*  17 fields */
};
const uint32_t kName    = 0x0c59fa06u;
const uint32_t kId      = 0x51480447u;
const uint32_t kLane    = 0xdfd68748u;
const uint32_t kDefault = 0x42c8b257u;
/* The hash -> field links (see load). */
const char* const kLinkType = "66241550-c062-e216-c829-980775109117";
const uint32_t kLinkHash  = 0xbf1ccee0u;
const uint32_t kLinkField = 0xef857f66u;

bool number(const EbxValue& v, double& out)
{
    switch (v.kind) {
    case EbxValue::Kind::Bool: out = v.b ? 1.0 : 0.0; return true;
    case EbxValue::Kind::Int:  out = (double)v.i; return true;
    case EbxValue::Kind::Uint: out = (double)v.u; return true;
    case EbxValue::Kind::Real: out = v.f; return true;
    default: return false;
    }
}

/* A nested descriptor's u16 id (the reference descriptors inside a field instance: one
 * under 81dd8149, three under a562e819, all 0xFFFF on LinearAcceleration and
 * LocalGroundVelocity). */
const uint32_t kDescId = 0x2b6f2936u;

bool any_descriptor_set(const EbxValue& v, int depth)
{
    if (depth > 8) return false;
    if (v.kind == EbxValue::Kind::Struct) {
        for (const auto& kv : v.fields) {
            double d = 0.0;
            if (kv.first == kDescId && number(kv.second, d)) { if ((uint32_t)d != 0xFFFFu) return true; }
            else if (any_descriptor_set(kv.second, depth + 1)) return true;
        }
    } else if (v.kind == EbxValue::Kind::Array) {
        for (const auto& it : v.items) if (any_descriptor_set(it, depth + 1)) return true;
    }
    return false;
}

}  // namespace

SoldierFields& SoldierFields::get()
{
    static SoldierFields s;
    return s;
}

bool SoldierFields::load(Source& src, TypeDb& types, std::string& err)
{
    if (loaded_) return true;
    std::vector<uint8_t> raw = src.get_ebx(std::string(kAsset) + ".ebx", err);
    if (raw.empty()) raw = src.get_ebx(kAsset, err);
    if (raw.empty()) return false;
    Ebx ebx(types);
    ebx.set_guid_index(&src.armory_partition_index());
    if (!ebx.parse(std::move(raw), err)) return false;
    std::map<size_t, uint64_t> by_instance;   /* instance index -> field key */
    for (size_t i = 0; i < ebx.instance_count(); ++i) {
        const std::string t = TypeDb::guid_str(ebx.instance_type(i));
        int kind = -1;
        for (const KindType& k : kKinds) if (t == k.guid) { kind = k.kind; break; }
        if (kind < 0) continue;
        const EbxValue v = ebx.read_instance(i);
        Field f;
        f.kind = kind;
        double id = -1.0, lane = 0.0;
        for (const auto& kv : v.fields) {
            if (kv.first == kName && kv.second.kind == EbxValue::Kind::Str) f.name = kv.second.s;
            else if (kv.first == kId) number(kv.second, id);
            else if (kv.first == kLane) number(kv.second, lane);
            else if (kv.first == kDefault) {
                double d = 0.0;
                if (number(kv.second, d)) f.def[0] = (float)d;
            }
        }
        /* Only DefaultValue is a vector default. The earlier shape-based scan read
         * the replication flags {true,false,2} as LinearVelocity (1,0,2).
         * Verified in soldiermotionmachine: DefaultValue 42C8B257 contains the
         * x/y/z fields 3901DB14/42FC0F5E/32A99B9C, all zero for LinearVelocity. */
        if (kind == kVec)
            for (const auto& kv : v.fields)
                if (kv.first == kDefault && kv.second.kind == EbxValue::Kind::Struct) {
                    const uint32_t xyz[] = {0x3901db14u, 0x42fc0f5eu, 0x32a99b9cu};
                    for (const auto& component : kv.second.fields)
                        for (int j = 0; j < 3; ++j) {
                            double d = 0;
                            if (component.first == xyz[j] && number(component.second, d)) f.def[j] = (float)d;
                        }
                }
        if (id < 0.0 || f.name.empty()) continue;
        f.space_ref = any_descriptor_set(v, 0);
        f.id = (int)id;
        f.lane = (int)lane;
        by_key_[key(f.kind, f.lane, f.id)] = f;
        by_instance[i] = key(f.kind, f.lane, f.id);
    }
    for (const auto& kv : by_key_) by_name_[kv.second.name] = &kv.second;
    /* THE HASH LINKS. Instances of kLinkType carry a 32-bit hash (kLinkHash), a reference
     * to one of the fields above (kLinkField) and the public channel collection it belongs
     * to. The operators that name a field by hash rather than by descriptor - 0x9132CD71,
     * "is this int field equal to N" - look the hash up in exactly this table (native
     * FUN_1442E0F00 over the machine's hash list). Measured: 0x346E1C6B, 0x7F615680 and
     * 0x24E8D360 each resolve through one of these to an int field. */
    for (size_t i = 0; i < ebx.instance_count(); ++i) {
        if (TypeDb::guid_str(ebx.instance_type(i)) != kLinkType) continue;
        const EbxValue v = ebx.read_instance(i);
        double h = -1.0;
        int target = -1;
        for (const auto& kv : v.fields) {
            if (kv.first == kLinkHash) number(kv.second, h);
            else if (kv.first == kLinkField && kv.second.kind == EbxValue::Kind::InstanceRef)
                target = kv.second.instance;
        }
        auto it = target >= 0 ? by_instance.find((size_t)target) : by_instance.end();
        if (h < 0.0 || it == by_instance.end()) continue;
        by_hash_[(uint32_t)h] = &by_key_[it->second];
    }
    loaded_ = !by_key_.empty();
    if (!loaded_) err = "no soldier field instances in " + std::string(kAsset);
    return loaded_;
}

const SoldierFields::Field* SoldierFields::find(int kind, int lane, int id) const
{
    auto it = by_key_.find(key(kind, lane, id));
    return it == by_key_.end() ? nullptr : &it->second;
}

void SoldierFields::value(const Field& f, float out[4]) const
{
    auto it = live_.find(f.name);
    for (int j = 0; j < 4; ++j) out[j] = it != live_.end() ? it->second[(size_t)j] : f.def[j];
}

void SoldierFields::xform(const Field& f, float out[16]) const
{
    auto it = xform_live_.find(f.name);
    if (it != xform_live_.end()) { for (int j = 0; j < 16; ++j) out[j] = it->second[(size_t)j]; return; }
    for (int j = 0; j < 16; ++j) out[j] = (j == 0 || j == 5 || j == 10 || j == 15) ? 1.0f : 0.0f;
}

void SoldierFields::set_live_xform(const std::string& name, const float m[16])
{
    std::array<float, 16> a{};
    for (int j = 0; j < 16; ++j) a[(size_t)j] = m[j];
    xform_live_[name] = a;
    unknown_.erase(name);
}

bool SoldierFields::get_by_name(const std::string& name, float out[4]) const
{
    auto it = by_name_.find(name);
    if (it == by_name_.end()) return false;
    value(*it->second, out);
    return true;
}

void SoldierFields::set_live(const std::string& name, const float* v, int n)
{
    std::array<float, 4> a{0, 0, 0, 0};
    for (int j = 0; j < n && j < 4; ++j) a[(size_t)j] = v[j];
    live_[name] = a;
    unknown_.erase(name);
}

}  // namespace bf6
