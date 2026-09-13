#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bf6_ui {

enum class InstalledInputDevice {
    Keyboard,
    Mouse,
    Pad,
    MotionController,
    Unknown,
};

/* One authored InputActionData object referenced by one concept row.  Raw
 * enum values are retained exactly, including the installed build's
 * Undefined sentinel.  A consumer must not replace these with a host table
 * and call the result authored data. */
struct InstalledPhysicalAction {
    uint32_t concept_id = 0;
    uint32_t copy_key_binding_from = 0;
    InstalledInputDevice device = InstalledInputDevice::Unknown;
    std::string type_guid;
    int source_instance = -1;
    std::optional<uint32_t> key;
    std::optional<uint32_t> button;
    std::optional<uint32_t> alternative_button;
    std::optional<uint32_t> axis;
};

struct InstalledDefaultBindingAudit {
    std::string source_partition;
    int partition_instances = 0;
    int requested_concepts = 0;
    int matched_concepts = 0;
    int duplicate_concept_rows = 0;
    int xor_one_concept_rows = 0;
    int pointer_rows = 0;
    int invalid_pointer_rows = 0;
    int recognized_action_rows = 0;
    int unknown_action_rows = 0;
    int fake_asset_matches = 0;

    bool passed() const {
        return requested_concepts > 0 &&
               matched_concepts == requested_concepts &&
               duplicate_concept_rows == 0 && xor_one_concept_rows == 0 &&
               pointer_rows > 0 && invalid_pointer_rows == 0 &&
               unknown_action_rows == 0 && fake_asset_matches == 0;
    }
};

/* Reads `common/gameplay/input/inputconcepts_default` from the caller's
 * installed game and resolves its in-partition InputActions references with
 * the readable current executable's reflection.  No research TSV/JSON,
 * exported EBX, SDK dump, or built-in physical binding table participates.
 *
 * `concept_ids` are the exact InputConceptIdentifier u32 values.  BF6 uses the
 * same djb2-xor value as the authored Rime concept/event identifier, so callers
 * may pass the ids exposed by control_action_contracts(). */
bool load_installed_default_bindings(
    const std::string& game_directory,
    const uint32_t* concept_ids,
    size_t concept_count,
    std::vector<InstalledPhysicalAction>& actions,
    InstalledDefaultBindingAudit& audit,
    std::string& error);

} // namespace bf6_ui
