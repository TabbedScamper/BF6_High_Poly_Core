#include "bf6_core.h"
#include "offline_home_provider.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    const char* game = argc > 1 ? argv[1]
        : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Battlefield 6";
    char error_buffer[512]{};
    bf6_ctx* context = bf6_open(game, error_buffer,
                                static_cast<int>(sizeof(error_buffer)));
    if (!context) { std::fprintf(stderr, "open: %s\n", error_buffer); return 1; }
    if (!bf6_mount_frontend(context, error_buffer,
                            static_cast<int>(sizeof(error_buffer)))) {
        std::fprintf(stderr, "mount: %s\n", error_buffer);
        bf6_close(context); return 1;
    }

    offline_home::Snapshot snapshot;
    std::string error;
    const bool loaded = snapshot.load(context, error);
    const offline_home::Audit& audit = snapshot.audit();
    std::printf("offline-home loaded=%d instances=%d wires=%d "
                "records=%d/%d collections=%d/%d scalars=%d public=%d "
                "interface=%d declared-public=%d "
                "scalar-propagation=%d blank-before=%d "
                "unresolved=%d ambiguous=%d contract-miss=%d relation-miss=%d "
                "record-outputs=%d invalid-record-output=%d "
                "opaque=%d external=%d controls(fake/shuffled-record/"
                "shuffled-collection/shuffled-public/shuffled-output)="
                "%d/%d/%d/%d/%d shuffled-scalar=%d\n",
                loaded ? 1 : 0, audit.instances, audit.property_connections,
                audit.records_built, audit.record_nodes,
                audit.collections_built, audit.collection_nodes,
                audit.scalar_nodes, audit.public_values,
                audit.interface_declarations, audit.public_declared_values,
                audit.scalar_propagations_applied,
                audit.blank_scalar_targets_before,
                audit.unresolved_inputs, audit.ambiguous_inputs,
                audit.contract_field_misses, audit.relation_schema_misses,
                audit.record_output_references, audit.invalid_record_outputs,
                audit.opaque_inputs, audit.external_inputs,
                audit.fake_partition_matches,
                audit.shuffled_records_built,
                audit.shuffled_collections_built,
                audit.shuffled_public_declarations,
                audit.shuffled_record_output_matches,
                audit.shuffled_scalar_propagations);
    if (!loaded) std::fprintf(stderr, "%s\n", error.c_str());

    bool public_gate = loaded;
    for (const offline_home::PublicValue& value : snapshot.public_values()) {
        std::printf("public 0x%08X kind=%d instance=%d declared=%s raw=%u\n",
                    value.field, static_cast<int>(value.value.kind),
                    value.value.instance,
                    value.declared_partition.empty()
                        ? "<null-import>" : value.declared_partition.c_str(),
                    value.declared_type_raw);
        const offline_home::Record* record = snapshot.record(value.value.instance);
        if (value.field == 0xA3FAEFDAu) {
            public_gate = public_gate && value.value.instance == 51 &&
                          value.declared_partition.empty() &&
                          value.declared_type_raw == 66;
        } else if (value.field == 0x7C610D08u) {
            public_gate = public_gate && value.value.instance == 60 && record &&
                          value.declared_partition == record->dbd_partition &&
                          value.declared_partition ==
                              "common/ui/home/data/playmenudbd";
        } else if (value.field == 0x805B91EEu) {
            public_gate = public_gate && value.value.instance == 52 && record &&
                          value.declared_partition == record->dbd_partition &&
                          value.declared_partition ==
                              "common/ui/home/data/bulletinlistdbd";
        } else {
            public_gate = false;
        }
    }
    struct ExpectedLabel { int record; const char* text; };
    const ExpectedLabel expected_labels[] = {
        {53, "01_Bulletin"}, {49, "02_Recommended"},
        {77, "03_Multiplayer"}, {55, "02_Recommended"},
        {46, "03_Multiplayer"}
    };
    bool label_gate = loaded;
    for (const ExpectedLabel& expected : expected_labels) {
        const offline_home::Record* record = snapshot.record(expected.record);
        const offline_home::Field* name =
            record ? snapshot.field(*record, "Name") : nullptr;
        const bool found = name && name->property_id == 0x7C82D882u &&
            name->value.kind == offline_home::ReferenceKind::Scalar &&
            name->value.scalar.kind == offline_home::ScalarKind::String &&
            name->value.scalar.string == expected.text;
        std::printf("resolved-label record=%d text=%s found=%d\n",
                    expected.record, expected.text, found ? 1 : 0);
        label_gate = label_gate && found;
    }
    for (const offline_home::Record& record : snapshot.records())
        std::printf("record %d dbd=%s primitive=%zu fields=%zu\n",
                    record.instance, record.dbd_partition.c_str(),
                    snapshot.primitive_record(record).entries().size(),
                    record.fields.size());

    const offline_home::Record* root = snapshot.record(60);
    const offline_home::Field* sections_field =
        root ? snapshot.field(*root, "Sections") : nullptr;
    const offline_home::Collection* sections =
        sections_field && sections_field->value.kind ==
                              offline_home::ReferenceKind::Collection
            ? snapshot.collection(sections_field->value.instance) : nullptr;
    if (sections) {
        std::printf("traversal root=60 Sections=%d items=", sections->instance);
        for (const offline_home::Reference& section_ref : sections->items)
            std::printf("%d,", section_ref.instance);
        std::printf("\n");
        for (const offline_home::Reference& section_ref : sections->items) {
            if (section_ref.kind != offline_home::ReferenceKind::Record) continue;
            const offline_home::Record* section = snapshot.record(section_ref.instance);
            const offline_home::Field* rows_field =
                section ? snapshot.field(*section, "Rows") : nullptr;
            const offline_home::Collection* rows =
                rows_field && rows_field->value.kind ==
                                  offline_home::ReferenceKind::Collection
                    ? snapshot.collection(rows_field->value.instance) : nullptr;
            if (!rows) continue;
            std::printf("traversal section=%d Rows=%d items=",
                        section->instance, rows->instance);
            for (const offline_home::Reference& row_ref : rows->items)
                std::printf("%d,", row_ref.instance);
            std::printf("\n");
            for (const offline_home::Reference& row_ref : rows->items) {
                if (row_ref.kind != offline_home::ReferenceKind::Record) continue;
                const offline_home::Record* row = snapshot.record(row_ref.instance);
                const offline_home::Field* interactables_field =
                    row ? snapshot.field(*row, "Interactables") : nullptr;
                const offline_home::Collection* interactables =
                    interactables_field && interactables_field->value.kind ==
                                               offline_home::ReferenceKind::Collection
                        ? snapshot.collection(interactables_field->value.instance)
                        : nullptr;
                if (!interactables) continue;
                std::printf("traversal row=%d Interactables=%d items=",
                            row->instance, interactables->instance);
                for (const offline_home::Reference& item : interactables->items)
                    std::printf("%d,", item.instance);
                std::printf("\n");
            }
        }
    }

    bf6_close(context);
    return loaded && audit.passed() && public_gate && label_gate ? 0 : 1;
}
