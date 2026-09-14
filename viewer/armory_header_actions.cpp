#include "armory_header_actions.h"

#include "bf6_core.h"
#include "expression_graph.h"

#include <cstring>
#include <map>
#include <utility>

namespace armory_header {
namespace {

constexpr uint32_t kLocalize = 0x1C07201Fu;
constexpr uint32_t kVariantAssign = 0x94388961u;
constexpr uint32_t kSetField = 0x69A5E560u;
constexpr uint32_t kLabelField = 0x0D000763u;

uint32_t pool_u32(const bf6::expression::Graph& graph, uint32_t at)
{
    if (at > graph.constant_pool.size() || graph.constant_pool.size() - at < 4)
        return 0;
    uint32_t value = 0;
    std::memcpy(&value, graph.constant_pool.data() + at, 4);
    return value;
}

const bf6::expression::Operand* output_slot(const bf6::expression::Record& record)
{
    for (auto it = record.operands.rbegin(); it != record.operands.rend(); ++it)
        if (it->region == 2) return &*it;
    return nullptr;
}

bool uses_slot(const bf6::expression::Record& record, uint32_t slot)
{
    for (const auto& operand : record.operands)
        if (operand.region == 2 && operand.offset == slot) return true;
    return false;
}

bool has_pool_value(const bf6::expression::Graph& graph,
                    const bf6::expression::Record& record, uint32_t value)
{
    for (const auto& operand : record.operands)
        if (operand.region == 0 && pool_u32(graph, operand.offset) == value)
            return true;
    return false;
}

const bf6::expression::Record* consumer(const bf6::expression::Graph& graph,
                                        uint32_t slot, uint32_t key)
{
    for (const auto& record : graph.records)
        if (record.operator_key == key && uses_slot(record, slot)) return &record;
    return nullptr;
}

} // namespace

bool read(bf6_ctx* ctx, const char* graph_asset,
          std::vector<Action>& out, ReadStats* stats)
{
    out.clear();
    ReadStats measured{};
    if (!ctx || !graph_asset || !*graph_asset) {
        if (stats) *stats = measured;
        return false;
    }

    const uint8_t* raw = nullptr;
    const int64_t bytes = bf6_read_raw(ctx, BF6_RAW_RES, graph_asset, &raw);
    bf6::expression::Graph graph;
    std::string error;
    if (bytes <= 0 || !raw ||
        !bf6::expression::parse(raw, (size_t)bytes, graph, error)) {
        if (stats) *stats = measured;
        return false;
    }
    measured.exact_record_tiling = graph.exact_record_tiling;
    if (!graph.exact_record_tiling) {
        if (stats) *stats = measured;
        return false;
    }

    std::map<uint32_t, const bf6::expression::Record*> producers;
    for (const auto& record : graph.records) {
        const auto* output = output_slot(record);
        if (output) producers[output->offset] = &record;
    }
    std::map<uint32_t, size_t> row_by_sid;

    for (const auto& localize : graph.records) {
        if (localize.operator_key != kLocalize) continue;
        const auto* localized_output = output_slot(localize);
        if (!localized_output) continue;
        const auto* boxed = consumer(graph, localized_output->offset, kVariantAssign);
        const auto* boxed_output = boxed ? output_slot(*boxed) : nullptr;
        const auto* write = boxed_output ? consumer(graph, boxed_output->offset, kSetField) : nullptr;
        const bool reaches_label = write && has_pool_value(graph, *write, kLabelField);

        for (const auto& input : localize.operands) {
            if (input.region != 2 || input.offset == localized_output->offset) continue;
            const auto found = producers.find(input.offset);
            if (found == producers.end() ||
                found->second->operator_key != kVariantAssign) continue;
            for (const auto& source : found->second->operands) {
                if (source.region != 0) continue;
                const uint32_t sid = pool_u32(graph, source.offset);
                if (!sid) continue;
                const char* localized = bf6_localized_string(ctx, sid);
                if (!localized || !*localized) continue;
                ++measured.localized_candidates;
                if (!reaches_label) { ++measured.rejected_non_label; continue; }
                ++measured.label_flow_occurrences;
                const auto prior = row_by_sid.find(sid);
                if (prior != row_by_sid.end()) {
                    ++out[prior->second].source_occurrences;
                    continue;
                }
                Action action;
                action.string_id = sid;
                action.label = localized;
                action.source_occurrences = 1;
                row_by_sid[sid] = out.size();
                out.push_back(std::move(action));
            }
        }
    }

    if (stats) *stats = measured;
    return !out.empty();
}

} // namespace armory_header
