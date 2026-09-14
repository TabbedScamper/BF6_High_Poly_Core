#include "rime_list_provider.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unordered_set>

namespace rime_list {
namespace {

constexpr FieldSpec kRootFields[] = {
    {0x4A54B0EFu, "EmptyListText"},
    {0xD0FA5854u, "BackgroundHeight"},
    {0x94EE2A34u, "IsTopLineVisible"},
    {0xCE54D6F0u, "IsBottomLineVisible"},
    {0xA470728Au, "IsEmpty"},
    {0x67481048u, "GridViewType"},
    {0xC683BEBAu, "VerticalGridViewType"},
    {kGridItemCollectionData, "GridItemCollectionData"},
};

constexpr FieldSpec kItemFields[] = {
    {0xB60D9C7Au, "Header"},
    {0xB71C036Eu, "IsHeaderVisible"},
    {0x4D8A03BEu, "SubHeader"},
    {0x5BE3A26Du, "IsHeaderNumberVisible"},
    {0xF754CA19u, "HeaderNumber"},
    {0x4D299CC7u, "IsTextBackingVisible"},
    {0x941096FAu, "IsIconVisible"},
    {0x7C8264EEu, "Icon"},
    {0x4EBBAF8Eu, "IsTextureVisible"},
    {0xBDD7E0DAu, "Texture"},
    {0x6BF18547u, "TextureSource"},
    {0x927F0233u, "IsTextureSourceVisible"},
    {0x17B8C426u, "IsStateVisible"},
    {0x86BC1BD6u, "IsEquipped"},
    {0x0CD84343u, "IsNew"},
    {0x28E567BCu, "ContainsNewItems"},
    {0x30A05175u, "IsLocked"},
    {0x795050BBu, "IsPurchasable"},
    {0x9DC5AB26u, "NumberOfPackages"},
    {0x98F4D133u, "CellData"},
    {0xFE59922Bu, "IsFavorite"},
    {0x6AB909BAu, "IsTopTitleVisible"},
    {0x6D5263A2u, "PointCost"},
    {0x8C62C676u, "MaxPointCost"},
    {0x76CDEAECu, "PointCostVisible"},
    {0x8A82E5DAu, "IsEnabled"},
    {0x91D58B35u, "SvgBaseImage"},
    {0x74172081u, "IsSvgBaseImageVisible"},
    {0xADA9829Eu, "IsSecondaryActionEnabled"},
    {0xCDFC3935u, "Category"},
    {0x33759A81u, "IsCategoryVisible"},
    {0x414F43D2u, "PackageId"},
    {0xE667F3FEu, "IsModified"},
    {0x76262D6Eu, "FocusOnHover"},
    {0xBB7D24FEu, "UISoundStyle_Primary"},
};

constexpr FieldSpec kHeaderFields[] = {
    {0x30A05175u, "IsLocked"},
    {0xF8D0B771u, "IsVisible"},
    {0x8B3FE95Au, "HeaderTitle"},
    {0x34482FFCu, "HeaderDescription"},
    {0xCDFC3935u, "Category"},
    {0x33759A81u, "IsCategoryVisible"},
    {0xF44CC299u, "IsDisallowed"},
    {0x092FDCC0u, "SvgImage"},
    {0x3F6A34B0u, "MasteryData"},
    {0x89714B76u, "PointCostIsVisible"},
    {0x4BB1A7CBu, "PointCostCurrent"},
    {0x7AB38BD6u, "PointCostMax"},
    {0xBF757535u, "IsTagCollectionVisible"},
    {0x4FC89337u, "IsSlashFormatVisible"},
    {0xE667F3FEu, "IsModified"},
    {0xADA6F114u, "IsSVGImageVisible"},
    {0x3D31AEB1u, "IsSubtitleHidden"},
    {0xDB55E0D5u, "SubHeaderTop"},
    {0xFC237CF0u, "FactionId"},
    {0xC2782957u, "IsDescriptionVisible"},
    {0x31F17642u, "GameplayLockLabel"},
    {0x1DAFE5D0u, "IsGameplayLockVisible"},
    {0x414F43D2u, "PackageId"},
};

template <size_t N>
const FieldSpec* find_field(const FieldSpec (&fields)[N], uint32_t id)
{
    for (const FieldSpec& field : fields)
        if (field.id == id) return &field;
    return nullptr;
}

uint32_t shuffled(uint32_t id)
{
    /* A reversible byte rotation keeps the control token-shaped while breaking
     * the actual property identity.  The xor prevents symmetric-byte ids from
     * surviving unchanged. */
    return ((id << 8) | (id >> 24)) ^ 0xA5A5A5A5u;
}

uint32_t property_hash(const std::string& value)
{
    uint32_t hash = 5381u;
    for (unsigned char ch : value) hash = hash * 33u ^ ch;
    return hash;
}

bool quoted_after(const std::string& text, size_t at, std::string& value)
{
    const size_t first = text.find('"', at);
    if (first == std::string::npos) return false;
    const size_t last = text.find('"', first + 1);
    if (last == std::string::npos) return false;
    value = text.substr(first + 1, last - first - 1);
    return true;
}

bool unsigned_after(const std::string& text, size_t at, const char* marker,
                    uint64_t& value)
{
    const size_t begin = text.find(marker, at);
    if (begin == std::string::npos) return false;
    const char* first = text.c_str() + begin + std::strlen(marker);
    char* last = nullptr;
    const unsigned long long parsed = std::strtoull(first, &last, 10);
    if (last == first) return false;
    value = static_cast<uint64_t>(parsed);
    return true;
}

RouteReport route_impl(bf6_ctx* ctx, const char* dbd_partition,
                       const char* partition,
                       const char* target_widget_partition,
                       const Record& record, bool control)
{
    RouteReport report{};
    report.supplied = static_cast<int>(record.entries().size());
    if (!ctx || !partition || !*partition) {
        report.unrouted = report.supplied;
        return report;
    }

    DbdContract contract;
    if (!load_contract(ctx, contract, dbd_partition)) {
        report.unrouted = report.supplied;
        return report;
    }

    std::unordered_set<int32_t> target_widgets;
    if (target_widget_partition && *target_widget_partition) {
        bf6_rime_tree_stats stats{};
        const int nr = bf6_rime_tree(ctx, partition, 6, nullptr, 0, &stats);
        if (nr > 0) {
            std::vector<bf6_rime_node> rows(static_cast<size_t>(nr));
            const int got = bf6_rime_tree(ctx, partition, 6, rows.data(), nr,
                                          &stats);
            if (got == nr) {
                for (const bf6_rime_node& row : rows) {
                    if (row.kind == BF6_RIME_WIDGET_REFERENCE &&
                        std::strcmp(row.partition, partition) == 0 &&
                        std::strcmp(row.reference,
                                    target_widget_partition) == 0)
                        target_widgets.insert(row.instance);
                }
            }
        }
    }

    const int nc = bf6_rime_connections(ctx, partition, nullptr, 0);
    if (nc < 0) {
        report.unrouted = report.supplied;
        return report;
    }
    std::vector<bf6_rime_connection> connections(static_cast<size_t>(nc));
    if (nc) bf6_rime_connections(ctx, partition, connections.data(), nc);

    for (const Entry& entry : record.entries()) {
        const auto dbd = std::find_if(
            contract.fields.begin(), contract.fields.end(),
            [&](const DbdField& field) { return field.property_id == entry.field; });
        if (dbd == contract.fields.end()) {
            ++report.unrouted;
            report.unrouted_fields.push_back(entry.field);
            continue;
        }
        const uint32_t id = control ? shuffled(dbd->provider_id)
                                    : dbd->provider_id;
        RouteReport::Evidence evidence{};
        evidence.field = entry.field;
        evidence.provider_field = id;
        evidence.partition = partition;
        for (const bf6_rime_connection& wire : connections) {
            if (wire.source_field == id) ++evidence.source_wires;
            if (wire.target_field == id) ++evidence.target_wires;
            if (wire.source_field == id &&
                (!target_widget_partition || !*target_widget_partition ||
                 target_widgets.count(wire.target)))
                ++evidence.target_widget_wires;
        }
        const bool declared = false;
        const bool connected = evidence.target_widget_wires > 0;
        if (evidence.source_wires || evidence.target_wires)
            report.evidence.push_back(std::move(evidence));
        report.declared += declared ? 1 : 0;
        report.connected += connected ? 1 : 0;
        report.unrouted += connected ? 0 : 1;
        if (declared) report.declared_fields.push_back(entry.field);
        if (connected) report.connected_fields.push_back(entry.field);
        else report.unrouted_fields.push_back(entry.field);
    }
    return report;
}

} // namespace

const FieldSpec* root_fields(size_t* count)
{
    if (count) *count = sizeof(kRootFields) / sizeof(kRootFields[0]);
    return kRootFields;
}

const FieldSpec* item_fields(size_t* count)
{
    if (count) *count = sizeof(kItemFields) / sizeof(kItemFields[0]);
    return kItemFields;
}

const FieldSpec* header_fields(size_t* count)
{
    if (count) *count = sizeof(kHeaderFields) / sizeof(kHeaderFields[0]);
    return kHeaderFields;
}

const FieldSpec* root_field(uint32_t id) { return find_field(kRootFields, id); }
const FieldSpec* item_field(uint32_t id) { return find_field(kItemFields, id); }
const FieldSpec* header_field(uint32_t id) { return find_field(kHeaderFields, id); }

const DbdField* contract_field(const DbdContract& contract,
                               uint32_t property_id)
{
    for (const DbdField& field : contract.fields)
        if (field.property_id == property_id) return &field;
    return nullptr;
}

const DbdField* contract_field(const DbdContract& contract,
                               const char* property_name)
{
    if (!property_name || !*property_name) return nullptr;
    const uint32_t id = property_hash(property_name);
    const DbdField* field = contract_field(contract, id);
    return field && field->name == property_name ? field : nullptr;
}

bool load_item_contract(bf6_ctx* ctx, DbdContract& out, const char* partition)
{
    return load_contract(ctx, out, partition);
}

bool load_contract(bf6_ctx* ctx, DbdContract& out, const char* partition)
{
    out = DbdContract{};
    if (!ctx || !partition || !*partition) return false;

    /* One bounded live call avoids the diagnostic bf6_ebx_dump path.  That
     * path builds the complete game partition GUID index merely to decorate
     * ImportRefs for humans; a DBD has no such dependency. */
    std::vector<bf6_rime_dbd_field> rows(256);
    char data_name[256]{};
    const int expected = bf6_rime_dbd_fields(
        ctx, partition, data_name, static_cast<int>(sizeof(data_name)),
        rows.data(), static_cast<int>(rows.size()));
    if (expected <= 0 || expected > static_cast<int>(rows.size()) ||
        !data_name[0])
        return false;
    out.data_name = data_name;

    std::unordered_set<uint32_t> properties;
    std::unordered_set<uint32_t> providers;
    for (int index = 0; index < expected; ++index) {
        DbdField field{};
        field.name = rows[static_cast<size_t>(index)].name;
        field.type_signature = rows[static_cast<size_t>(index)].type_signature;
        if (field.name.empty()) return false;
        field.property_id = property_hash(field.name);
        field.provider_id = property_hash(out.data_name + "." + field.name);
        if (!properties.insert(field.property_id).second ||
            !providers.insert(field.provider_id).second)
            return false;
        out.fields.push_back(std::move(field));
    }
    return out.fields.size() == static_cast<size_t>(expected);
}

bool Record::put(uint32_t field, const Value& value)
{
    if (!item_field(field)) return false;
    for (Entry& entry : entries_) {
        if (entry.field == field) {
            entry.value = value;
            return true;
        }
    }
    entries_.push_back({field, value});
    return true;
}

bool find_unique_list(const bf6_rime_node* rows, int row_count,
                      const char* owner_partition, const char* list_name,
                      DataList& out, int* matches)
{
    out = DataList{};
    int found = 0;
    if (!rows || row_count < 0 || !owner_partition || !list_name) {
        if (matches) *matches = 0;
        return false;
    }
    for (int i = 0; i < row_count; ++i) {
        const bf6_rime_node& row = rows[i];
        if (std::strcmp(row.type_guid, kDataListTypeGuid) != 0 &&
            std::strcmp(row.type_name, "DiceUIUniformGridElementData") != 0)
            continue;
        if (std::strcmp(row.partition, owner_partition) != 0 ||
            std::strcmp(row.name, list_name) != 0)
            continue;
        ++found;
        out.row = i;
        out.instance = row.instance;
        out.parent = row.parent;
        out.partition = row.partition;
        out.name = row.name;
        out.item_template = row.item_template;
        out.item_templates.clear();
        for (int template_index = 0;
             template_index < row.item_template_count && template_index < 8;
             ++template_index)
            if (row.item_templates[template_index][0])
                out.item_templates.emplace_back(
                    row.item_templates[template_index]);
        out.item_spacing = row.item_spacing;
        out.orientation = row.stack_orientation;
        out.size_distribution = row.data_list_size_distribution;
        out.flow_direction = row.data_list_flow_direction;
        out.space_distribution = row.data_list_space_distribution;
        out.preserve_fit_content = row.data_list_preserve_fit_content;
    }
    if (matches) *matches = found;
    if (found != 1) out = DataList{};
    return found == 1;
}

bool Record::put(const DbdContract& contract, uint32_t field,
                 const Value& value)
{
    if (!contract_field(contract, field)) return false;
    for (Entry& entry : entries_) {
        if (entry.field == field) {
            entry.value = value;
            return true;
        }
    }
    entries_.push_back({field, value});
    return true;
}

const Entry* Record::find(uint32_t field) const
{
    for (const Entry& entry : entries_)
        if (entry.field == field) return &entry;
    return nullptr;
}

ContractReport inspect(bf6_ctx* ctx, const char* partition,
                       const bf6_rime_node* rows, int row_count,
                       uint32_t root_property)
{
    ContractReport report{};
    if (!ctx || !partition || !rows || row_count < 0) return report;

    const int ni = bf6_rime_interface_descriptors(ctx, partition, nullptr, 0);
    std::vector<int32_t> interfaces(static_cast<size_t>(ni > 0 ? ni : 0));
    if (ni > 0)
        bf6_rime_interface_descriptors(ctx, partition, interfaces.data(), ni);
    report.interface_descriptors = ni > 0 ? ni : 0;
    const std::unordered_set<int32_t> descriptor_set(interfaces.begin(),
                                                     interfaces.end());

    const int nc = bf6_rime_connections(ctx, partition, nullptr, 0);
    std::vector<bf6_rime_connection> wires(static_cast<size_t>(nc > 0 ? nc : 0));
    if (nc > 0) bf6_rime_connections(ctx, partition, wires.data(), nc);
    for (const bf6_rime_connection& wire : wires) {
        if (!descriptor_set.count(wire.source) ||
            wire.source_field != root_property ||
            wire.target_field != root_property)
            continue;
        ++report.exact_root_wires;
        const bf6_rime_node* target = nullptr;
        for (int i = 0; i < row_count; ++i) {
            /* The expanded tree intentionally repeats local instance numbers
             * in referenced partitions.  This public root wire projects onto
             * the referenced greeble's BottomCap, which is why restricting the
             * lookup to the root partition loses the authored destination. */
            if (rows[i].instance == wire.target &&
                std::strcmp(rows[i].name, "[Container] BottomCap") == 0) {
                target = &rows[i];
                break;
            }
        }
        if (target && target->kind == BF6_RIME_CONTAINER)
            ++report.bottom_cap_wires;
        else
            ++report.wrong_target_wires;
    }

    for (int i = 0; i < row_count; ++i) {
        const bf6_rime_node& row = rows[i];
        if (std::strcmp(row.type_guid, kDataListTypeGuid) != 0) continue;
        DataList list{};
        list.row = i;
        list.instance = row.instance;
        list.parent = row.parent;
        list.partition = row.partition;
        list.name = row.name;
        list.item_template = row.item_template;
        for (int template_index = 0;
             template_index < row.item_template_count && template_index < 8;
             ++template_index)
            if (row.item_templates[template_index][0])
                list.item_templates.emplace_back(
                    row.item_templates[template_index]);
        list.item_spacing = row.item_spacing;
        list.orientation = row.stack_orientation;
        list.size_distribution = row.data_list_size_distribution;
        list.flow_direction = row.data_list_flow_direction;
        list.space_distribution = row.data_list_space_distribution;
        list.preserve_fit_content = row.data_list_preserve_fit_content;
        report.lists.push_back(std::move(list));
    }
    return report;
}

RouteReport route(bf6_ctx* ctx, const char* item_partition,
                  const Record& record)
{
    return route_impl(ctx, kGridItemDbdPartition, item_partition, nullptr,
                      record, false);
}

RouteReport route_shuffled_control(bf6_ctx* ctx, const char* item_partition,
                                   const Record& record)
{
    return route_impl(ctx, kGridItemDbdPartition, item_partition, nullptr,
                      record, true);
}

RouteReport route_contract(bf6_ctx* ctx, const char* dbd_partition,
                           const char* graph_partition,
                           const char* target_widget_partition,
                           const Record& record)
{
    return route_impl(ctx, dbd_partition, graph_partition,
                      target_widget_partition, record, false);
}

RouteReport route_contract_shuffled_control(
    bf6_ctx* ctx, const char* dbd_partition, const char* graph_partition,
    const char* target_widget_partition, const Record& record)
{
    return route_impl(ctx, dbd_partition, graph_partition,
                      target_widget_partition, record, true);
}

namespace {

BindReport bind_impl(rime::Screen& item, const char* graph_partition,
                     const DbdContract& contract, const Record& record,
                     bool control)
{
    BindReport report{};
    report.supplied = static_cast<int>(record.entries().size());
    if (!graph_partition || !*graph_partition) {
        report.unrouted = report.supplied;
        return report;
    }
    const rime::Screen baseline = item;
    rime::Screen working = item;
    for (const Entry& entry : record.entries()) {
        const DbdField* field = contract_field(contract, entry.field);
        if (!field) {
            ++report.unrouted;
            continue;
        }
        ++report.declared;
        if (entry.value.kind == ValueKind::Null ||
            entry.value.kind == ValueKind::Opaque) {
            ++report.unsupported_values;
            continue;
        }
        const uint32_t provider = control ? shuffled(field->provider_id)
                                          : field->provider_id;
        /* set_provider_* evaluates the whole graph, including inputs already
         * supplied earlier in this snapshot.  Probe each field on the same
         * pristine live template so a later unrouted field cannot take credit
         * for re-applying an earlier label (HeaderInfo.Category previously
         * reported success by repainting HeaderTitle). */
        rime::Screen probe = baseline;
        int ambiguous = 0;
        int targets = 0;
        switch (entry.value.kind) {
        case ValueKind::Bool:
            targets = rime::set_provider_bool(probe, graph_partition, provider,
                                               entry.value.boolean, &ambiguous);
            break;
        case ValueKind::Int:
            targets = rime::set_provider_int(probe, graph_partition, provider,
                                              entry.value.integer, &ambiguous);
            break;
        case ValueKind::UInt:
            targets = rime::set_provider_uint(
                probe, graph_partition, provider,
                entry.value.unsigned_integer, &ambiguous);
            break;
        case ValueKind::Real:
            targets = rime::set_provider_real(probe, graph_partition, provider,
                                               entry.value.real, &ambiguous);
            break;
        case ValueKind::String:
            targets = rime::set_provider_text(probe, graph_partition, provider,
                                               entry.value.string, &ambiguous);
            break;
        default:
            break;
        }
        report.ambiguous += ambiguous;
        if (targets > 0) {
            /* The isolated probe proved this input has a supported terminal.
             * Now add it to the aggregate snapshot.  Its aggregate return
             * count is intentionally ignored because it includes prior
             * fields by design. */
            switch (entry.value.kind) {
            case ValueKind::Bool:
                rime::set_provider_bool(working, graph_partition, provider,
                                        entry.value.boolean, nullptr);
                break;
            case ValueKind::Int:
                rime::set_provider_int(working, graph_partition, provider,
                                       entry.value.integer, nullptr);
                break;
            case ValueKind::UInt:
                rime::set_provider_uint(working, graph_partition, provider,
                                        entry.value.unsigned_integer, nullptr);
                break;
            case ValueKind::Real:
                rime::set_provider_real(working, graph_partition, provider,
                                        entry.value.real, nullptr);
                break;
            case ValueKind::String:
                rime::set_provider_text(working, graph_partition, provider,
                                        entry.value.string, nullptr);
                break;
            default:
                break;
            }
            ++report.applied_fields;
            report.applied_targets += targets;
        } else {
            ++report.unrouted;
        }
    }
    /* A partially populated cell is more dangerous than an empty one: it can
     * look plausible while pairing the wrong label and state.  Commit the
     * snapshot only when every supplied primitive reached an unambiguous
     * terminal in isolation. */
    if (!report.unrouted && !report.unsupported_values && !report.ambiguous &&
        report.applied_fields == report.supplied)
        item = std::move(working);
    return report;
}

} // namespace

BindReport bind_record(rime::Screen& item, const char* graph_partition,
                       const DbdContract& contract, const Record& record)
{
    return bind_impl(item, graph_partition, contract, record, false);
}

BindReport bind_record_shuffled_control(
    rime::Screen& item, const char* graph_partition,
    const DbdContract& contract, const Record& record)
{
    return bind_impl(item, graph_partition, contract, record, true);
}

namespace {

BindReport bind_transaction_impl(
    rime::Screen& item, const char* graph_partition,
    const DbdContract& contract, const Record& record, bool control)
{
    BindReport report{};
    report.supplied = static_cast<int>(record.entries().size());
    if (!graph_partition || !*graph_partition || !report.supplied) {
        report.unrouted = report.supplied;
        return report;
    }
    rime::Screen working = item;
    auto apply = [&](uint32_t provider, const Value& value, int* ambiguous) {
        switch (value.kind) {
        case ValueKind::Bool:
            return rime::set_provider_bool(
                working, graph_partition, provider, value.boolean, ambiguous);
        case ValueKind::Int:
            return rime::set_provider_int(
                working, graph_partition, provider, value.integer, ambiguous);
        case ValueKind::UInt:
            return rime::set_provider_uint(
                working, graph_partition, provider,
                value.unsigned_integer, ambiguous);
        case ValueKind::Real:
            return rime::set_provider_real(
                working, graph_partition, provider, value.real, ambiguous);
        case ValueKind::String:
            return rime::set_provider_text(
                working, graph_partition, provider, value.string, ambiguous);
        default:
            return 0;
        }
    };

    int final_targets = 0;
    for (const Entry& entry : record.entries()) {
        const DbdField* field = contract_field(contract, entry.field);
        if (!field) { ++report.unrouted; continue; }
        ++report.declared;
        if (entry.value.kind == ValueKind::Null ||
            entry.value.kind == ValueKind::Opaque) {
            ++report.unsupported_values;
            continue;
        }
        const uint32_t provider = control ? shuffled(field->provider_id)
                                          : field->provider_id;
        std::set<int32_t> sources;
        for (const rime::Screen::InterfaceTextGraph& graph :
             working.interface_text_graphs)
            if (graph.partition == graph_partition)
                for (const bf6_rime_connection& wire : graph.connections)
                    if (wire.source_field == provider)
                        sources.insert(wire.source);
        if (sources.size() != 1) { ++report.unrouted; continue; }
        int ambiguous = 0;
        final_targets = apply(provider, entry.value, &ambiguous);
        report.ambiguous += ambiguous;
    }
    if (!report.unrouted && !report.unsupported_values && !report.ambiguous &&
        report.declared == report.supplied && final_targets > 0) {
        report.applied_fields = report.supplied;
        report.applied_targets = final_targets;
        item = std::move(working);
    } else if (!report.unrouted && !report.unsupported_values &&
               !report.ambiguous) {
        report.unrouted = report.supplied;
    }
    return report;
}

} // namespace

BindReport bind_record_transaction(
    rime::Screen& item, const char* graph_partition,
    const DbdContract& contract, const Record& record)
{
    return bind_transaction_impl(item, graph_partition, contract, record,
                                 false);
}

BindReport bind_record_transaction_shuffled_control(
    rime::Screen& item, const char* graph_partition,
    const DbdContract& contract, const Record& record)
{
    return bind_transaction_impl(item, graph_partition, contract, record,
                                 true);
}

bool append_bound_grid_item(
    const rime::Screen& item_template,
    const rime::UniformGridLayout& layout,
    const DbdContract& contract, const Record& record,
    const char* graph_partition, float cell_x, float cell_y,
    int runtime_state, std::vector<rime::Element>& out,
    BindReport* binding)
{
    rime::Screen item = item_template;
    const BindReport report = bind_record(item, graph_partition, contract,
                                          record);
    if (binding) *binding = report;
    if (report.ambiguous || report.unrouted || report.unsupported_values)
        return false;
    return rime::append_grid_item(item, layout, cell_x, cell_y,
                                  runtime_state, out);
}

} // namespace rime_list
