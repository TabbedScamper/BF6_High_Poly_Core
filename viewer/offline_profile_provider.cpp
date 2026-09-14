#include "offline_profile_provider.h"

#include <cstring>

namespace offline_profile {
namespace {

constexpr uint64_t kBoolType = 0x0000000080000140ull;
constexpr uint64_t kIntType = 0x00000000800001E0ull;
constexpr uint64_t kRealType = 0x0000000080000260ull;

enum class RuleKind { Bool, Int, Real, String };

struct Rule {
    const char* partition;
    const char* field;
    uint64_t type_signature;
    RuleKind kind;
    bool boolean = false;
    int64_t integer = 0;
    double real = 0.0;
    const char* string = nullptr;
};

/* The contracts and fields below are shipped BF6 data and their read paths are
 * documented by the research corpus. Only their per-account runtime values
 * are placeholders: no new markers, favorites, purchases, mastery, social
 * notifications, or locks. All authored content still comes from the mounted
 * install, so "unlocked" means "available to inspect", not a claim about an
 * EA account or a replacement for researched content. */
constexpr Rule kRules[] = {
    {rime_list::kGridItemDbdPartition, "IsNew", kBoolType,
     RuleKind::Bool, false},
    {rime_list::kGridItemDbdPartition, "ContainsNewItems", kBoolType,
     RuleKind::Bool, false},
    {rime_list::kGridItemDbdPartition, "IsLocked", kBoolType,
     RuleKind::Bool, false},
    {rime_list::kGridItemDbdPartition, "IsPurchasable", kBoolType,
     RuleKind::Bool, false},
    {rime_list::kGridItemDbdPartition, "IsFavorite", kBoolType,
     RuleKind::Bool, false},

    {rime_list::kAttachmentDbdPartition, "IsLocked", kBoolType,
     RuleKind::Bool, false},
    {rime_list::kAttachmentDbdPartition, "HasLockedFactoryPackage", kBoolType,
     RuleKind::Bool, false},
    {rime_list::kAttachmentDbdPartition, "IsSlotLocked", kBoolType,
     RuleKind::Bool, false},

    {rime_list::kCollapseButtonDbdPartition, "IsProficiencyVisible", kBoolType,
     RuleKind::Bool, false},

    {"common/ui/weaponcustomization/assets/databindings/"
     "weaponcollectioncolumndbd", "IsFavorites", kBoolType,
     RuleKind::Bool, false},
    {"common/ui/weaponcustomization/assets/databindings/"
     "weaponcollectioncolumndbd", "HasProficiency", kBoolType,
     RuleKind::Bool, false},
    {"common/ui/weaponcustomization/assets/databindings/"
     "weaponcollectionselectedweapondbd", "IsCustomizationLocked", kBoolType,
     RuleKind::Bool, false},
    {"common/ui/weaponcustomization/assets/databindings/"
     "weaponcustomizationrootdbd", "IsFavorite", kBoolType,
     RuleKind::Bool, false},
    {"common/ui/weaponcustomization/assets/databindings/"
     "weaponcustomizationrootdbd", "HasProficiency", kBoolType,
     RuleKind::Bool, false},

    {"common/ui/playerprofile/assets/databinding/pp_dbd", "IsPrivate",
     kBoolType, RuleKind::Bool, false},
    {"common/ui/playerprofile/assets/databinding/pp_dbd", "IsLocal",
     kBoolType, RuleKind::Bool, true},
    {"common/ui/playerprofile/assets/databinding/pp_dbd", "Success",
     kBoolType, RuleKind::Bool, true},
    {"common/ui/playerprofile/assets/databinding/pp_dbd", "IsLoading",
     kBoolType, RuleKind::Bool, false},
    {"common/ui/playerprofile/assets/databinding/pp_dbd", "IsFriend",
     kBoolType, RuleKind::Bool, false},
    {"common/ui/playerprofile/assets/databinding/pp_dbd", "SoldierClassId",
     kIntType, RuleKind::Int, false, 0},
    {"common/ui/playerprofile/assets/databinding/pp_dbd", "PlayerOnlineHandle",
     0x0000000000000036ull, RuleKind::String, false, 0, 0.0, kDisplayName},

    {"common/ui/playerprofile/assets/databinding/pp_infodbd", "IsPrivate",
     kBoolType, RuleKind::Bool, false},
    {"common/ui/playerprofile/assets/databinding/pp_infodbd", "PlayerName",
     0x0000000000000012ull, RuleKind::String, false, 0, 0.0, kDisplayName},
    {"common/ui/playerprofile/assets/databinding/pp_infodbd", "CurrentValue",
     kRealType, RuleKind::Real, false, 0, 0.0},
    {"common/ui/playerprofile/assets/databinding/pp_infodbd", "MaxValue",
     kRealType, RuleKind::Real, false, 0, 0.0},
    {"common/ui/playerprofile/assets/databinding/pp_infodbd", "RankNumber",
     kIntType, RuleKind::Int, false, 0},
    {"common/ui/playerprofile/assets/databinding/pp_infodbd", "RankName",
     0x0000000000000012ull, RuleKind::String, false, 0, 0.0, kDisplayName},
    {"common/ui/playerprofile/assets/databinding/pp_infodbd", "IsEnabled",
     kBoolType, RuleKind::Bool, true},

    {"common/ui/system/eaconnect/bfuiphotondatabindingdefinition", "IsOpen",
     kBoolType, RuleKind::Bool, false},
    {"common/ui/system/eaconnect/bfuiphotondatabindingdefinition",
     "IsInitialized", kBoolType, RuleKind::Bool, false},
    {"common/ui/system/eaconnect/bfuiphotondatabindingdefinition",
     "NumberOfFriendsOnline", kIntType, RuleKind::Int, false, 0},
    {"common/ui/system/eaconnect/bfuiphotondatabindingdefinition",
     "IsShowingNotificationPopup", kBoolType, RuleKind::Bool, false},
    {"common/ui/system/eaconnect/bfuiphotondatabindingdefinition", "IsReady",
     kBoolType, RuleKind::Bool, false},
    {"common/ui/system/eaconnect/bfuiphotondatabindingdefinition",
     "NumberOfUnseenFriendRequests", kIntType, RuleKind::Int, false, 0},
    {"common/ui/system/eaconnect/bfuiphotondatabindingdefinition",
     "NumberOfUnseenGameInvites", kIntType, RuleKind::Int, false, 0},
    {"common/ui/system/eaconnect/bfuiphotondatabindingdefinition",
     "NumberOfUnseenGifts", kIntType, RuleKind::Int, false, 0},
    {"common/ui/system/eaconnect/bfuiphotondatabindingdefinition",
     "NumberOfUnseenInboxItems", kIntType, RuleKind::Int, false, 0},
    {"common/ui/system/eaconnect/bfuiphotondatabindingdefinition",
     "NumberOfUnseenQuickMessages", kIntType, RuleKind::Int, false, 0},
};

rime_list::Value make_value(const Rule& rule)
{
    rime_list::Value value;
    switch (rule.kind) {
    case RuleKind::Bool:
        value.kind = rime_list::ValueKind::Bool;
        value.boolean = rule.boolean;
        break;
    case RuleKind::Int:
        value.kind = rime_list::ValueKind::Int;
        value.integer = rule.integer;
        break;
    case RuleKind::Real:
        value.kind = rime_list::ValueKind::Real;
        value.real = rule.real;
        break;
    case RuleKind::String:
        value.kind = rime_list::ValueKind::String;
        value.string = rule.string ? rule.string : "";
        break;
    }
    return value;
}

} // namespace

Report append_routed(bf6_ctx* ctx, const char* dbd_partition,
                     const char* graph_partition,
                     const rime_list::DbdContract& contract,
                     rime_list::Record& record)
{
    Report report;
    if (!ctx || !dbd_partition || !graph_partition) return report;
    for (const Rule& rule : kRules) {
        if (std::strcmp(rule.partition, dbd_partition) != 0) continue;
        ++report.candidate_rules;
        const rime_list::DbdField* field =
            rime_list::contract_field(contract, rule.field);
        if (!field) {
            ++report.missing_contract_fields;
            continue;
        }
        if (field->type_signature != rule.type_signature) {
            ++report.type_mismatches;
            continue;
        }
        if (record.find(field->property_id)) {
            ++report.explicit_values_preserved;
            continue;
        }

        rime_list::Record probe;
        if (!probe.put(contract, field->property_id, make_value(rule))) {
            ++report.missing_contract_fields;
            continue;
        }
        const rime_list::RouteReport real = rime_list::route_contract(
            ctx, dbd_partition, graph_partition, nullptr, probe);
        const rime_list::RouteReport shuffled =
            rime_list::route_contract_shuffled_control(
                ctx, dbd_partition, graph_partition, nullptr, probe);
        Evidence evidence;
        evidence.field = rule.field;
        evidence.property_id = field->property_id;
        evidence.type_signature = field->type_signature;
        evidence.real_routes = real.connected;
        evidence.shuffled_routes = shuffled.connected;
        report.evidence.push_back(evidence);
        report.shuffled_control_routes += shuffled.connected;
        if (real.connected <= 0 || shuffled.connected != 0) {
            ++report.unrouted;
            continue;
        }
        if (record.put(contract, field->property_id, make_value(rule)))
            ++report.applied;
        else
            ++report.missing_contract_fields;
    }
    return report;
}

} // namespace offline_profile
