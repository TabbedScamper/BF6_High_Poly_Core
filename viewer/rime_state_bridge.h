#pragma once

#include "rime.h"

#include <cstdint>
#include <istream>
#include <map>
#include <string>
#include <vector>

namespace rime_state {

/* A numeric scope id is not portable between the schematic evaluator and the
 * renderer.  The evaluator counts every EBX instance while the renderer's
 * scope is an expanded-tree row.  The authored WidgetReference chain is the
 * common identity: each step names the owning partition and that partition's
 * local reference instance. */
struct OccurrenceStep {
    std::string owner_partition;
    int32_t reference_instance = -1;

    bool operator==(const OccurrenceStep& other) const {
        return owner_partition == other.owner_partition &&
               reference_instance == other.reference_instance;
    }
};

using OccurrencePath = std::vector<OccurrenceStep>;

enum class ValueKind {
    Null,
    Bool,
    Int,
    UInt,
    Real,
    String,
};

struct Value {
    ValueKind kind = ValueKind::Null;
    bool boolean = false;
    int64_t integer = 0;
    uint64_t unsigned_integer = 0;
    double real = 0.0;
    std::string string;

    static Value from_bool(bool value);
    static Value from_int(int64_t value);
    static Value from_uint(uint64_t value);
    static Value from_real(double value);
    static Value from_string(std::string value);
};

/* One post-binding slot emitted by the current-install evaluator.  `partition`
 * and `local_instance` retain the shipped EBX identity; no route-specific
 * alias or evidence table participates. */
struct StateSlot {
    std::string partition;
    std::string source_type;
    OccurrencePath occurrence;
    int32_t local_instance = -1;
    uint32_t field = 0;
    Value value;
};

struct ApplyReport {
    int input_slots = 0;
    int applied_slots = 0;
    int missing_occurrences = 0;
    int missing_instances = 0;
    int ambiguous_targets = 0;
    int non_renderer_targets = 0;
    int unsupported_fields = 0;
    int type_mismatches = 0;
    int invalid_scope_chains = 0;
    std::map<uint32_t, int> missing_occurrence_field_counts;
    std::map<uint32_t, int> missing_instance_field_counts;
    std::map<uint32_t, int> unsupported_field_counts;
    std::vector<StateSlot> missing_occurrence_examples;
};

struct WireReport {
    std::string route;
    std::map<std::string, int> controls; // 1 pass, 0 fail, -1 not applicable
    bool requirement_metadata_present = false;
    int host_requirement_slots = 0;
    int external_host_requirement_slots = 0;
    int consumed_external_host_requirement_slots = 0;
    bool provider_metadata_present = false;
    int resolved_consumed_external_provider_slots = 0;
    int remaining_consumed_external_provider_slots = 0;
    bool dependency_metadata_present = false;
    int dependency_closed_slots = 0;
    int excluded_provider_dependent_slots = 0;
    int declared_slots = 0;
    int omitted_non_scalar_slots = 0;
};

/* Read the strict V3 stdout stream emitted by current_install_ui_state_capture
 * --wire. V3 contains only proved host/provider seeds and values derived by an
 * executed supported primitive; untouched authored operator defaults are not
 * renderer state and never cross the pipe. Values transitively dependent on
 * an unresolved provider are excluded and counted in dependency metadata.
 * The stream is intended for a pipe
 * from that live current-install process; it is not a staged-file format. */
bool read_wire(std::istream& input, std::vector<StateSlot>& slots,
               WireReport& report, std::string& error);

/* Recover the portable authored identity for an element.  The Screen must
 * have passed through load_interface_text_graphs(), which assigns occurrence
 * scopes to every expanded element. */
bool occurrence_path(const rime::Screen& screen, size_t element_index,
                     OccurrencePath& out);

/* Apply final resolved element slots to a live renderer screen.  Only fields
 * whose renderer semantics are already decoded are accepted: visibility,
 * alpha, size, stack spacing, progress/border/line animation scalars, and the
 * four text aliases used by Rime labels. Unknown fields are reported and left
 * untouched instead of being guessed. */
ApplyReport apply_state(rime::Screen& screen,
                        const std::vector<StateSlot>& slots);

} // namespace rime_state
