#ifndef BF6_EXPRESSION_GRAPH_H
#define BF6_EXPRESSION_GRAPH_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bf6 { namespace expression {

/* Operator arity by operator key, as the executable's reflected registry
 * declares it - bf6_expression_reflected_operators fills exactly this. It is
 * how kind 0x23's record length becomes known rather than searched for:
 *
 *     length = 24 + arity * 8
 *
 * NOT an exported corpus table: it is read from the same game the caller is
 * already reading, which is why it may be used here. Entirely optional. */
using ArityMap = std::map<uint32_t, uint16_t>;

struct Header {
    uint32_t content_hash = 0;
    uint32_t instance_header_size = 0;
    uint32_t constant_pool_size = 0;
    uint32_t slot_file_size = 0;
    uint32_t record_dwords = 0;
    uint32_t pointer_table_entries = 0;
    uint32_t external_bindings = 0;
    uint16_t relocation_count = 0;
    uint16_t secondary_relocation_count = 0;
    uint16_t fixup_count = 0;
    uint16_t type_table_dwords = 0;
    uint16_t instance_value_dwords = 0;
    uint16_t slot_value_dwords = 0;
    uint8_t instance_buffer_count = 0;
    uint8_t register_groups[3] = {};
};

struct TypeRecord {
    uint32_t type_id = 0;
    uint32_t carried[4] = {};
};

struct TypedValueGroup {
    uint32_t data_type_id = 0;
    std::vector<uint32_t> offsets;
};

struct Relocation {
    uint32_t pointer_field = 0;
    uint32_t target = 0;
};

struct Fixup {
    uint32_t key = 0;
    uint32_t record_offset = 0;
};

struct Operand {
    uint32_t region = 0;
    uint32_t offset = 0;
};

struct Record {
    uint32_t offset = 0;
    uint32_t byte_length = 0;
    uint8_t kind = 0;
    uint32_t next = 0;
    uint32_t operator_key = 0;
    bool has_operator = false;
    bool has_trailing_dword = false;
    uint32_t trailing_dword = 0;
    uint32_t control_target = 0;
    std::vector<uint32_t> dispatch_labels;
    std::vector<uint32_t> dispatch_targets;
    std::vector<Operand> operands;
};

struct Graph {
    Header header;
    size_t constant_base = 0;
    size_t region_base = 0;
    size_t relocation_table = 0;
    size_t fixup_table = 0;
    size_t image_at = 0;
    std::vector<TypeRecord> types;
    std::vector<TypedValueGroup> instance_values;
    std::vector<TypedValueGroup> slot_values;
    std::vector<uint8_t> constant_pool;
    std::vector<Relocation> relocations;
    std::vector<Fixup> fixups;
    std::vector<Record> records;
    std::vector<uint8_t> instance_image;

    // True only when the proven per-kind lengths (including a constrained
    // 0x23 solve) cover every byte in the record region exactly. False is an
    // explicit unknown-kind/ambiguous-shape result, never silently promoted.
    /* Why tiling failed: 0 no tiling exists, 1 unique, 2 or more ambiguous.
     * "No tiling" means a length is wrong or missing; "ambiguous" means a
     * constraint is missing. They are opposite problems with opposite fixes. */
    uint8_t tiling_ways = 0;
    /* Set when the arity pins produced NO tiling and were dropped for a
     * search. A non-zero count here means an operator arity is wrong for this
     * graph - worth surfacing rather than silently recovering from. */
    uint8_t arity_pin_rejected = 0;
    bool exact_record_tiling = false;
    size_t discovered_record_bytes = 0;
};

/* Parse the relocatable on-disk image. This function does not consult an
 * exported table, the executable, or a live process. It rejects malformed
 * offsets and patch sites and preserves raw values whose meaning is unknown.
 */
bool parse(const uint8_t* data, size_t size, Graph& out, std::string& error,
           const ArityMap* arity = nullptr);

bool has_operator_pointer(uint8_t kind);

/* Returns a corpus-proven fixed byte length, 0 for a variadic/dynamic or
 * unmeasured kind. This is exposed for diagnostics so callers do not turn an
 * unmeasured kind into a guessed implementation detail. */
uint32_t proven_record_length(uint8_t kind);

}} // namespace bf6::expression

#endif
