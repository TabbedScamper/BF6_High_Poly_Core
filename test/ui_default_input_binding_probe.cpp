#include "ebx.h"
#include "source.h"
#include "types.h"
#include "default_input_bindings.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using bf6::Ebx;
using bf6::EbxValue;
using bf6::Source;
using bf6::TypeDb;
using bf6::TypeGuid;

constexpr const char* kDefaultMap =
    "common/gameplay/input/inputconcepts_default";
constexpr uint32_t kInputActions = 0x0582CD7Cu;
constexpr uint32_t kConcept = 0x442FEE8Au;
constexpr uint32_t kCopyConcept = 0xC44D6ED8u;
constexpr uint32_t kKey = 0x7C7E22C1u;
constexpr uint32_t kButton = 0x412198DBu;
constexpr uint32_t kAlternativeButton = 0x2B1682B7u;
constexpr uint32_t kAxis = 0x876C7F17u;

constexpr const char* kInputActionsType =
    "d795e871-dd2a-7d07-4749-5254782c236d";
constexpr const char* kKeyboardType =
    "8383474f-ffac-8c16-cb2c-972bfceff132";
constexpr const char* kMouseType =
    "5d71ce1d-79e0-b9e1-b284-1b1c0a1649de";
constexpr const char* kPadType =
    "94afd819-f4df-71f5-cd67-2558291c8988";

constexpr const char* kConceptNames[] = {
    "ConceptActivate", "ConceptBack", "ConceptClassTabRight",
    "ConceptDeployNavigateDown", "ConceptDeployNavigateLeft",
    "ConceptDeployNavigateRight", "ConceptDeployNavigateUp", "ConceptEdit",
    "ConceptGraniteInventoryDropSelected", "ConceptHomeMenuNavigateDown",
    "ConceptHomeMenuNavigateUp", "ConceptInventoryNavigateLeft",
    "ConceptInventoryNavigateRight", "ConceptInventoryNavigateUp",
    "ConceptMenuBack", "ConceptMenuPrimary", "ConceptMenuSecondary",
    "ConceptMenuSocial", "ConceptMenuTabLeft", "ConceptMenuTabRight",
    "ConceptMenuTertiary", "ConceptNavigateDown", "ConceptNavigateLeft",
    "ConceptNavigateRight", "ConceptNavigateUp", "ConceptSelect",
};

uint32_t action_id(const char* text)
{
    uint32_t hash = 5381u;
    for (const unsigned char* at =
             reinterpret_cast<const unsigned char*>(text); *at; ++at)
        hash = hash * 33u ^ *at;
    return hash;
}

bool numeric(const EbxValue* value, uint32_t& out)
{
    if (!value) return false;
    switch (value->kind) {
    case EbxValue::Kind::Int: out = static_cast<uint32_t>(value->i); return true;
    case EbxValue::Kind::Uint:
    case EbxValue::Kind::Unknown: out = static_cast<uint32_t>(value->u); return true;
    default: return false;
    }
}

const char* device(const TypeGuid& type)
{
    const std::string id = TypeDb::guid_str(type);
    if (id == kKeyboardType) return "keyboard";
    if (id == kMouseType) return "mouse";
    if (id == kPadType) return "pad";
    return "other";
}

bool open_types(const std::string& game, TypeDb& types, std::string& error)
{
    for (const std::string& candidate : TypeDb::exe_candidates(game)) {
        if (types.open(candidate, error) && !types.looks_encrypted()) return true;
    }
    return false;
}

void scan_value(const EbxValue& value, const std::set<uint32_t>& wanted,
                uint32_t field, const std::string& type, size_t instance,
                int& hits)
{
    uint32_t raw = 0;
    if (numeric(&value, raw) && wanted.count(raw)) {
        std::printf("MATCH asset-instance=%zu type=%s field=0x%08X concept=0x%08X\n",
                    instance, type.c_str(), field, raw);
        ++hits;
    }
    for (const auto& child : value.fields)
        scan_value(child.second, wanted, child.first, type, instance, hits);
    for (const EbxValue& child : value.items)
        scan_value(child, wanted, field, type, instance, hits);
}

void scan_asset(Source& source, TypeDb& types, const char* path,
                const std::set<uint32_t>& wanted)
{
    std::string error;
    std::vector<uint8_t> bytes = source.get_ebx(path, error);
    Ebx ebx(types);
    if (bytes.empty() || !ebx.parse(std::move(bytes), error)) {
        std::printf("SCAN %s FAILED %s\n", path, error.c_str());
        return;
    }
    std::map<std::string, int> types_seen;
    int hits = 0;
    for (size_t i = 0; i < ebx.instance_count(); ++i) {
        const std::string type = TypeDb::guid_str(ebx.instance_type(i));
        ++types_seen[type];
        const EbxValue row = ebx.read_instance(i);
        const int before = hits;
        scan_value(row, wanted, 0, type, i, hits);
        if (hits != before && std::string(path).find("inputconcepts_default") !=
                                  std::string::npos) {
            for (const auto& field : row.fields) {
                std::printf("    owner-field=0x%08X kind=%u u=%llu ref=%d items=%zu\n",
                            field.first, static_cast<unsigned>(field.second.kind),
                            static_cast<unsigned long long>(field.second.u),
                            field.second.instance, field.second.items.size());
            }
        }
    }
    std::printf("SCAN %s instances=%zu types=%zu concept_hits=%d\n",
                path, ebx.instance_count(), types_seen.size(), hits);
    for (const auto& type : types_seen)
        std::printf("  scan-type %s count=%d\n", type.first.c_str(), type.second);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: ui_default_input_binding_probe <game-dir>\n");
        return 2;
    }

    std::string error;
    Source source;
    if (!source.open(argv[1], error) || !source.mount_frontend(error)) {
        std::fprintf(stderr, "mount: %s\n", error.c_str());
        return 1;
    }
    TypeDb types;
    if (!open_types(argv[1], types, error)) {
        std::fprintf(stderr, "types: %s\n", error.c_str());
        return 1;
    }
    std::vector<uint8_t> bytes = source.get_ebx(kDefaultMap, error);
    if (bytes.empty()) bytes = source.get_ebx(std::string(kDefaultMap) + ".ebx", error);
    Ebx ebx(types);
    if (bytes.empty() || !ebx.parse(std::move(bytes), error)) {
        std::fprintf(stderr, "default map: %s\n", error.c_str());
        return 1;
    }

    std::set<uint32_t> wanted;
    for (const char* name : kConceptNames) wanted.insert(action_id(name));
    int matched = 0;
    int mutated = 0;
    int bindings = 0;
    for (size_t i = 0; i < ebx.instance_count(); ++i) {
        const std::vector<uint32_t> fields = {kInputActions, kConcept, kCopyConcept};
        const EbxValue row = ebx.read_instance(i, &fields);
        uint32_t concept = 0, copy = 0;
        if (!numeric(row.field(kConcept), concept)) continue;
        if (wanted.count(concept ^ 1u)) ++mutated;
        if (!wanted.count(concept)) continue;
        ++matched;
        const char* name = "?";
        for (const char* candidate : kConceptNames)
            if (action_id(candidate) == concept) { name = candidate; break; }
        numeric(row.field(kCopyConcept), copy);
        const EbxValue* actions = row.field(kInputActions);
        const size_t count = actions && actions->kind == EbxValue::Kind::Array
            ? actions->items.size() : 0;
        std::printf("%s\t0x%08X\tcopy=0x%08X\tactions=%zu\n",
                    name, concept, copy, count);
        if (!actions || actions->kind != EbxValue::Kind::Array) continue;
        for (const EbxValue& ref : actions->items) {
            if (ref.kind != EbxValue::Kind::InstanceRef || ref.instance < 0 ||
                static_cast<size_t>(ref.instance) >= ebx.instance_count()) continue;
            const size_t target = static_cast<size_t>(ref.instance);
            const std::vector<uint32_t> physical = {
                kKey, kButton, kAlternativeButton, kAxis};
            const EbxValue action = ebx.read_instance(target, &physical);
            uint32_t key = 0, button = 0, alt = 0, axis = 0;
            const bool has_key = numeric(action.field(kKey), key);
            const bool has_button = numeric(action.field(kButton), button);
            const bool has_alt = numeric(action.field(kAlternativeButton), alt);
            const bool has_axis = numeric(action.field(kAxis), axis);
            std::printf("  %s type=%s key=%s%u button=%s%u alt=%s%u axis=%s%u\n",
                        device(ebx.instance_type(target)),
                        TypeDb::guid_str(ebx.instance_type(target)).c_str(),
                        has_key ? "" : "-", key,
                        has_button ? "" : "-", button,
                        has_alt ? "" : "-", alt,
                        has_axis ? "" : "-", axis);
            ++bindings;
        }
    }
    std::printf("control real=%d/26 xor_one_rows=%d bindings=%d instances=%zu\n",
                matched, mutated, bindings, ebx.instance_count());
    std::vector<uint32_t> ids;
    for (const char* name : kConceptNames) ids.push_back(action_id(name));
    std::vector<bf6_ui::InstalledPhysicalAction> api_actions;
    bf6_ui::InstalledDefaultBindingAudit api_audit;
    std::string api_error;
    const bool api_ok = bf6_ui::load_installed_default_bindings(
        argv[1], ids.data(), ids.size(), api_actions, api_audit, api_error);
    std::printf("api ok=%d matched=%d/%d actions=%zu recognized=%d unknown=%d "
                "invalid=%d xor_one=%d fake=%d error=%s\n",
                api_ok ? 1 : 0, api_audit.matched_concepts,
                api_audit.requested_concepts, api_actions.size(),
                api_audit.recognized_action_rows, api_audit.unknown_action_rows,
                api_audit.invalid_pointer_rows, api_audit.xor_one_concept_rows,
                api_audit.fake_asset_matches, api_error.c_str());
    return matched == 26 && mutated == 0 && bindings == 78 && api_ok &&
                   api_audit.passed() && api_actions.size() == 78
        ? 0 : 1;
}
