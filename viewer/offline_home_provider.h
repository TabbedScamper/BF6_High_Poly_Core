#pragma once

#include "bf6_core.h"
#include "rime_list_provider.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace offline_home {

constexpr const char* kMockPartition =
    "common/ui/home/logic/homescreenmockdata";

enum class ScalarKind { Null, Bool, Int, String };

struct Scalar {
    ScalarKind kind = ScalarKind::Null;
    bool boolean = false;
    int64_t integer = 0;
    std::string string;
};

enum class ReferenceKind {
    Null,
    Scalar,
    Record,
    Collection,
    /* Exact authored object whose payload is intentionally not coerced into a
     * primitive (for example TextureAssetEntityData). */
    Opaque,
    /* Value supplied by the host InterfaceDescriptor at runtime. */
    External
};

struct Reference {
    ReferenceKind kind = ReferenceKind::Null;
    Scalar scalar;
    int instance = -1;
};

struct Field {
    uint32_t property_id = 0;
    std::string name;
    Reference value;
};

struct Record {
    int instance = -1;
    std::string dbd_partition;
    rime_list::DbdContract contract;
    std::vector<Field> fields;
};

struct Collection {
    int instance = -1;
    int input_count = 0;
    std::vector<Reference> items;
};

struct PublicValue {
    uint32_t field = 0;
    Reference value;
    /* Exact TypeRef attached to this InterfaceDescriptor field.  Compound
     * DBD types carry their live import; primitive/opaque types retain the
     * shipped raw type word even when the import is null. */
    std::string declared_partition;
    uint32_t declared_type_raw = 0;
};

struct Audit {
    int instances = 0;
    int property_connections = 0;
    int record_nodes = 0;
    int collection_nodes = 0;
    int scalar_nodes = 0;
    int records_built = 0;
    int collections_built = 0;
    int public_values = 0;
    int interface_declarations = 0;
    int public_declared_values = 0;
    int scalar_propagations_applied = 0;
    int blank_scalar_targets_before = 0;
    int unresolved_inputs = 0;
    int ambiguous_inputs = 0;
    int contract_field_misses = 0;
    int relation_schema_misses = 0;
    int record_output_references = 0;
    int invalid_record_outputs = 0;
    int opaque_inputs = 0;
    int external_inputs = 0;
    int fake_partition_matches = 0;
    int shuffled_records_built = 0;
    int shuffled_collections_built = 0;
    int shuffled_public_declarations = 0;
    int shuffled_record_output_matches = 0;
    int shuffled_scalar_propagations = 0;

    bool passed() const {
        return instances == 108 && property_connections == 262 &&
               record_nodes == 42 && collection_nodes == 12 &&
               records_built == record_nodes &&
               collections_built == collection_nodes &&
               public_values == 3 && interface_declarations == 14 &&
               public_declared_values == public_values &&
               scalar_propagations_applied == 5 &&
               blank_scalar_targets_before == 5 &&
               unresolved_inputs == 0 &&
               ambiguous_inputs == 0 && contract_field_misses == 0 &&
               relation_schema_misses == 0 &&
               record_output_references > 0 && invalid_record_outputs == 0 &&
               opaque_inputs > 0 && external_inputs > 0 &&
               fake_partition_matches == 0 &&
               shuffled_records_built == 0 &&
               shuffled_collections_built == 0 &&
               shuffled_public_declarations == 0 &&
               shuffled_record_output_matches == 0 &&
               shuffled_scalar_propagations == 0;
    }
};

/* Read and assemble BF6's own developer-authored offline Home provider.
 *
 * This is deliberately not called a production/live-menu provider.  The
 * partition ships test rows and is useful as an exact offline host input for
 * the authored Home templates.  Every value is reread from the mounted game;
 * no research table or exported manifest is accepted at runtime.
 *
 * The two unnamed DiceCommons node families are admitted by exact GUID and by
 * their current graph census.  Their DBD inputs are matched against contracts
 * read from the same mount.  A fabricated partition and a deterministically
 * shuffled target-field graph are evaluated beside the real graph.
 */
class Snapshot {
public:
    bool load(bf6_ctx* context, std::string& error);

    const Audit& audit() const { return audit_; }
    const std::vector<Record>& records() const { return records_; }
    const std::vector<Collection>& collections() const { return collections_; }
    const std::vector<PublicValue>& public_values() const { return public_values_; }

    const Record* record(int instance) const;
    const Collection* collection(int instance) const;
    const Field* field(const Record& record, const char* name) const;
    std::vector<const Record*> records_in(const Collection& collection,
                                          const char* dbd_leaf = nullptr) const;

    /* Convert only primitive fields. Compound record/collection values stay
     * in Snapshot and are never coerced into strings or booleans. */
    rime_list::Record primitive_record(const Record& source) const;

private:
    Audit audit_{};
    std::vector<Record> records_;
    std::vector<Collection> collections_;
    std::vector<PublicValue> public_values_;
};

} // namespace offline_home
