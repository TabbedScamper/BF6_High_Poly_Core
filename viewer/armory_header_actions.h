#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct bf6_ctx;

namespace armory_header {

struct Action {
    uint32_t string_id = 0;
    std::string label;
    int source_occurrences = 0;
};

struct ReadStats {
    bool exact_record_tiling = false;
    int localized_candidates = 0;
    int label_flow_occurrences = 0;
    int rejected_non_label = 0;
};

// Read the action collection authored by MenuWeapon's DiceExpression graph.
// The path is:
//
//   Localize(SID) -> boxed variant -> SetField(Label) -> action element
//
// Both operator ids and Label's field hash are serialized graph identifiers;
// no exported table and no screenshot text participates in the result.
bool read(bf6_ctx* ctx, const char* graph_asset,
          std::vector<Action>& out, ReadStats* stats = nullptr);

} // namespace armory_header
