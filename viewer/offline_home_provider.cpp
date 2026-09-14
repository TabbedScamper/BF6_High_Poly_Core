#include "offline_home_provider.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace offline_home {
namespace {

constexpr const char* kRecordGuid =
    "7c0c46b2-c76a-f3f1-7ece-b916fb82e8f8";
constexpr const char* kCollectionGuid =
    "0ad62eb7-581f-0815-20c7-7418b2eeae6a";
constexpr const char* kLocalizedStringGuid =
    "7f8406ab-1c16-4449-dea4-5d6c68619c3b";
constexpr const char* kStringGuid =
    "50ea0cc5-169e-2971-9304-165cd67afac0";
constexpr const char* kIntGuid =
    "561e80cc-f9d6-fc1b-e1c4-ccbcc1bf968a";
constexpr const char* kBoolGuid =
    "0f6f47ec-8bd8-73d7-ba3b-3c2b5bca8834";
constexpr const char* kInterfaceGuid =
    "f2e1dfd3-7b56-e6f1-2976-0bf6658c162e";
constexpr const char* kTextureSourceGuid =
    "0e121f71-4967-3205-89a1-d1cbb05fc9a5";
constexpr const char* kTextureAssetGuid =
    "7010956c-cc4a-f46d-3b86-d7675971dd74";

constexpr uint32_t kDbdArrayField = 0x427995A4u;
constexpr uint32_t kDebugStringField = 0x5A7262E1u;
constexpr uint32_t kDefaultStringField = 0x0177B9B0u;
constexpr uint32_t kDefaultValueField = 0x42C8B257u;
constexpr uint32_t kInputCountField = 0xB3D28716u;
constexpr uint32_t kDbdRelationArrayField = 0x917AFD37u;
constexpr uint32_t kNameField = 0x0C59FA06u;
constexpr uint32_t kImportField = 0x73E9A894u;
constexpr uint32_t kRelationCollectionField = 0x0E4C6402u;
constexpr uint32_t kInterfaceFieldsArray = 0x21943083u;
constexpr uint32_t kInterfaceTypeRefField = 0xA38D791Cu;
constexpr uint32_t kInterfaceFieldId = 0xC1AD37DAu;
constexpr uint32_t kTypeRefRawField = 0x3AF2E007u;
constexpr uint32_t kStringOutputField = 0xC5E4E950u;
constexpr uint32_t kLocalizedDebugStringInput = 0xED6069E1u;

struct Node {
    enum class Kind {
        Unknown, Record, Collection, Scalar, Opaque, Interface
    };
    Kind kind = Kind::Unknown;
    std::string guid;
    std::string dbd;
    int input_count = -1;
    Scalar scalar;
};

struct DbdRelation {
    std::string name;
    std::string target_partition;
    bool collection = false;
    uint32_t property_id = 0;
};

struct DbdSchema {
    rime_list::DbdContract primitive;
    std::vector<DbdRelation> relations;
};

struct InterfaceDeclaration {
    std::string partition;
    uint32_t raw_type = 0;
};

uint32_t pin_hash(const std::string& value);

std::string lower(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    for (char& ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (value.size() > 4 && value.compare(value.size() - 4, 4, ".ebx") == 0)
        value.resize(value.size() - 4);
    return value;
}

std::string trim(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

bool starts_field(const std::string& line, uint32_t field, std::string& value)
{
    char marker[16]{};
    std::snprintf(marker, sizeof(marker), "0x%08x", field);
    const size_t at = line.find(marker);
    if (at == std::string::npos) return false;
    value = trim(line.substr(at + 10));
    return true;
}

bool quoted(const std::string& text, std::string& value)
{
    const size_t first = text.find('"');
    const size_t last = text.find_last_of('"');
    if (first == std::string::npos || last == first) return false;
    value = text.substr(first + 1, last - first - 1);
    return true;
}

bool parse_int(const std::string& text, int64_t& value)
{
    char* end = nullptr;
    value = std::strtoll(text.c_str(), &end, 10);
    return end != text.c_str();
}

bool read_dump(bf6_ctx* context, const char* partition, std::string& text)
{
    std::vector<char> buffer(1u << 20);
    int64_t needed = bf6_ebx_dump(context, partition, 6, buffer.data(),
                                  static_cast<int>(buffer.size()));
    if (needed < 0) return false;
    if (needed > static_cast<int64_t>(buffer.size())) {
        buffer.resize(static_cast<size_t>(needed));
        needed = bf6_ebx_dump(context, partition, 6, buffer.data(),
                              static_cast<int>(buffer.size()));
        if (needed < 0 || needed > static_cast<int64_t>(buffer.size()))
            return false;
    }
    text.assign(buffer.data());
    return true;
}

size_t indentation(const std::string& line)
{
    size_t count = 0;
    while (count < line.size() && line[count] == ' ') ++count;
    return count;
}

bool parse_dbd_relations(const std::string& text,
                         std::vector<DbdRelation>& relations)
{
    relations.clear();
    std::istringstream stream(text);
    std::string line;
    bool in_array = false;
    bool in_entry = false;
    size_t array_indent = 0;
    DbdRelation current;
    bool saw_collection_flag = false;
    auto finish = [&]() -> bool {
        if (!in_entry) return true;
        if (current.name.empty() || current.target_partition.empty() ||
            !saw_collection_flag)
            return false;
        current.property_id = pin_hash(current.name);
        relations.push_back(std::move(current));
        current = {};
        saw_collection_flag = false;
        in_entry = false;
        return true;
    };

    while (std::getline(stream, line)) {
        std::string value;
        if (!in_array) {
            if (!starts_field(line, kDbdRelationArrayField, value)) continue;
            in_array = true;
            array_indent = indentation(line);
            continue;
        }
        const size_t indent = indentation(line);
        if (indent <= array_indent) {
            if (!finish()) return false;
            break;
        }
        int index = -1;
        if (std::sscanf(line.c_str(), " %d: {", &index) == 1 &&
            indent > array_indent && indent <= array_indent + 3) {
            if (!finish()) return false;
            in_entry = true;
            continue;
        }
        if (!in_entry) continue;
        if (starts_field(line, kNameField, value)) {
            if (!quoted(value, current.name)) return false;
        } else if (line.find("0x73e9a894") != std::string::npos) {
            const size_t import_at = line.find("import ");
            if (import_at != std::string::npos)
                current.target_partition =
                    lower(trim(line.substr(import_at + 7)));
        } else if (starts_field(line, kRelationCollectionField, value)) {
            if (value != "true" && value != "false") return false;
            current.collection = value == "true";
            saw_collection_flag = true;
        }
    }
    if (in_array && !finish()) return false;
    return true;
}

bool parse_interface_declarations(
    const std::string& text,
    std::map<uint32_t, InterfaceDeclaration>& declarations)
{
    declarations.clear();
    std::istringstream stream(text);
    std::string line;
    bool in_array = false;
    bool in_entry = false;
    bool in_type_ref = false;
    size_t array_indent = 0;
    size_t type_ref_indent = 0;
    uint32_t field_id = 0;
    bool saw_field_id = false;
    InterfaceDeclaration current;
    bool saw_raw_type = false;

    auto finish = [&]() -> bool {
        if (!in_entry) return true;
        if (!saw_field_id || !saw_raw_type || current.raw_type == 0)
            return false;
        if (!declarations.emplace(field_id, std::move(current)).second)
            return false;
        current = {};
        field_id = 0;
        saw_field_id = false;
        saw_raw_type = false;
        in_entry = false;
        in_type_ref = false;
        return true;
    };

    while (std::getline(stream, line)) {
        std::string value;
        if (!in_array) {
            if (!starts_field(line, kInterfaceFieldsArray, value)) continue;
            in_array = true;
            array_indent = indentation(line);
            continue;
        }
        const size_t indent = indentation(line);
        if (indent <= array_indent) {
            if (!finish()) return false;
            break;
        }
        int index = -1;
        if (std::sscanf(line.c_str(), " %d: {", &index) == 1 &&
            indent > array_indent && indent <= array_indent + 3) {
            if (!finish()) return false;
            in_entry = true;
            continue;
        }
        if (!in_entry) continue;
        if (in_type_ref && indent <= type_ref_indent)
            in_type_ref = false;
        if (starts_field(line, kInterfaceTypeRefField, value) &&
            indent == array_indent + 4) {
            in_type_ref = true;
            type_ref_indent = indent;
            continue;
        }
        if (starts_field(line, kInterfaceFieldId, value) &&
            indent == array_indent + 4) {
            int64_t parsed = 0;
            if (!parse_int(value, parsed)) return false;
            field_id = static_cast<uint32_t>(parsed);
            saw_field_id = true;
            continue;
        }
        if (!in_type_ref) continue;
        if (starts_field(line, kImportField, value)) {
            const size_t import_at = line.find("import ");
            if (import_at != std::string::npos)
                current.partition = lower(trim(line.substr(import_at + 7)));
        } else if (starts_field(line, kTypeRefRawField, value)) {
            const size_t raw_at = value.find("raw=");
            if (raw_at == std::string::npos) return false;
            char* end = nullptr;
            const unsigned long long parsed = std::strtoull(
                value.c_str() + raw_at + 4, &end, 10);
            if (end == value.c_str() + raw_at + 4 ||
                parsed > 0xFFFFFFFFull)
                return false;
            current.raw_type = static_cast<uint32_t>(parsed);
            saw_raw_type = true;
        }
    }
    if (in_array && !finish()) return false;
    return in_array;
}

bool parse_nodes(const std::string& text, std::map<int, Node>& nodes,
                 int& declared_instances)
{
    declared_instances = -1;
    nodes.clear();
    std::istringstream stream(text);
    std::string line;
    int current = -1;
    bool expect_record_import = false;
    while (std::getline(stream, line)) {
        int count = 0;
        if (std::sscanf(line.c_str(), "  instances %d", &count) == 1)
            declared_instances = count;

        int instance = -1;
        char guid[64]{};
        if (std::sscanf(line.c_str(), "  [%d] type %63s", &instance, guid) == 2) {
            current = instance;
            Node& node = nodes[current];
            node.guid = lower(guid);
            if (node.guid == kRecordGuid) node.kind = Node::Kind::Record;
            else if (node.guid == kCollectionGuid) node.kind = Node::Kind::Collection;
            else if (node.guid == kLocalizedStringGuid ||
                     node.guid == kStringGuid || node.guid == kIntGuid ||
                     node.guid == kBoolGuid) {
                node.kind = Node::Kind::Scalar;
                node.scalar.kind = node.guid == kBoolGuid ? ScalarKind::Bool
                                  : node.guid == kIntGuid ? ScalarKind::Int
                                  : ScalarKind::String;
            } else if (node.guid == kInterfaceGuid)
                node.kind = Node::Kind::Interface;
            else if (node.guid == kTextureSourceGuid ||
                     node.guid == kTextureAssetGuid)
                node.kind = Node::Kind::Opaque;
            expect_record_import = false;
            continue;
        }
        if (current < 0) continue;
        Node& node = nodes[current];
        std::string value;
        if (node.kind == Node::Kind::Record &&
            starts_field(line, kDbdArrayField, value)) {
            expect_record_import = true;
            continue;
        }
        if (node.kind == Node::Kind::Record && expect_record_import) {
            const size_t import_at = line.find("import ");
            if (import_at != std::string::npos) {
                std::string path = trim(line.substr(import_at + 7));
                node.dbd = lower(path);
                expect_record_import = false;
            }
        }
        if (node.kind == Node::Kind::Collection &&
            starts_field(line, kInputCountField, value)) {
            int64_t parsed = 0;
            if (parse_int(value, parsed)) node.input_count = static_cast<int>(parsed);
        }
        if (node.kind != Node::Kind::Scalar) continue;
        if ((node.guid == kLocalizedStringGuid &&
             starts_field(line, kDebugStringField, value)) ||
            (node.guid == kStringGuid &&
             starts_field(line, kDefaultStringField, value))) {
            std::string parsed;
            if (quoted(value, parsed)) node.scalar.string = parsed;
        } else if ((node.guid == kIntGuid || node.guid == kBoolGuid) &&
                   starts_field(line, kDefaultValueField, value)) {
            if (node.guid == kBoolGuid) node.scalar.boolean = value == "true";
            else {
                int64_t parsed = 0;
                if (parse_int(value, parsed)) node.scalar.integer = parsed;
            }
        }
    }
    return declared_instances >= 0 &&
           nodes.size() == static_cast<size_t>(declared_instances);
}

uint32_t shuffled(uint32_t value)
{
    return ((value << 8u) | (value >> 24u)) ^ 0xA5A5A5A5u;
}

int propagate_scalar_strings(
    std::map<int, Node>& nodes,
    const std::vector<bf6_rime_connection>& wires,
    bool control, int* blank_targets_before)
{
    int applied = 0;
    if (blank_targets_before) *blank_targets_before = 0;
    for (const bf6_rime_connection& wire : wires) {
        const auto source = nodes.find(wire.source);
        const auto target = nodes.find(wire.target);
        if (source == nodes.end() || target == nodes.end() ||
            source->second.guid != kStringGuid ||
            target->second.guid != kLocalizedStringGuid)
            continue;
        const uint32_t source_field =
            control ? shuffled(wire.source_field) : wire.source_field;
        const uint32_t target_field =
            control ? shuffled(wire.target_field) : wire.target_field;
        if (source_field != kStringOutputField ||
            target_field != kLocalizedDebugStringInput)
            continue;
        if (source->second.scalar.string.empty()) continue;
        if (blank_targets_before && target->second.scalar.string.empty())
            ++*blank_targets_before;
        target->second.scalar.string = source->second.scalar.string;
        ++applied;
    }
    return applied;
}

uint32_t pin_hash(const std::string& value)
{
    uint32_t hash = 5381u;
    for (unsigned char ch : value) hash = hash * 33u ^ ch;
    return hash;
}

Reference reference_for(const Node& node, int instance, uint32_t source_field,
                        const DbdSchema* schema, bool control, Audit* audit)
{
    Reference value;
    value.instance = instance;
    switch (node.kind) {
    case Node::Kind::Scalar:
        value.kind = ReferenceKind::Scalar;
        value.scalar = node.scalar;
        break;
    case Node::Kind::Record: {
        if (!schema || schema->primitive.data_name.empty()) {
            if (audit && !control) ++audit->invalid_record_outputs;
            return {};
        }
        const uint32_t expected = pin_hash(schema->primitive.data_name);
        const uint32_t actual = control ? shuffled(source_field) : source_field;
        if (actual != expected) {
            if (audit && !control) ++audit->invalid_record_outputs;
            return {};
        }
        if (audit) {
            if (control) ++audit->shuffled_record_output_matches;
            else ++audit->record_output_references;
        }
        value.kind = ReferenceKind::Record;
        break;
    }
    case Node::Kind::Collection: value.kind = ReferenceKind::Collection; break;
    case Node::Kind::Opaque: value.kind = ReferenceKind::Opaque; break;
    case Node::Kind::Interface: value.kind = ReferenceKind::External; break;
    default: value = {}; break;
    }
    return value;
}

const rime_list::DbdField* contract_input(
    const rime_list::DbdContract& contract, uint32_t target)
{
    for (const rime_list::DbdField& field : contract.fields)
        if (field.property_id == target || field.provider_id == target)
            return &field;
    return nullptr;
}

struct BuildCounts { int records = 0; int collections = 0; };

using Incoming =
    std::map<int, std::vector<const bf6_rime_connection*>>;

bool collection_input_index(uint32_t field, int count, int& index)
{
    index = -1;
    for (int candidate = 0; candidate < count; ++candidate) {
        const uint32_t expected = pin_hash(
            "Input" + std::to_string(candidate));
        if (field != expected) continue;
        index = candidate;
        return true;
    }
    return false;
}

std::vector<const DbdRelation*> compatible_relations(
    const DbdSchema& target_schema, const Node& source, int source_instance,
    const std::map<int, Node>& nodes, const Incoming& incoming)
{
    std::vector<const DbdRelation*> candidates;
    if (source.kind == Node::Kind::Record) {
        for (const DbdRelation& relation : target_schema.relations)
            if (!relation.collection &&
                relation.target_partition == source.dbd)
                candidates.push_back(&relation);
        return candidates;
    }
    if (source.kind != Node::Kind::Collection) return candidates;

    std::set<std::string> item_types;
    const auto item_wires = incoming.find(source_instance);
    if (item_wires != incoming.end()) {
        std::vector<bool> seen(static_cast<size_t>(
            std::max(source.input_count, 0)), false);
        for (const bf6_rime_connection* wire : item_wires->second) {
            int index = -1;
            if (!collection_input_index(wire->target_field,
                                        source.input_count, index))
                return {};
            if (seen[static_cast<size_t>(index)]) return {};
            seen[static_cast<size_t>(index)] = true;
            const auto item = nodes.find(wire->source);
            if (item == nodes.end() || item->second.kind != Node::Kind::Record)
                return {};
            item_types.insert(item->second.dbd);
        }
    }
    /* InputCount is the authored slot capacity.  Unconnected slots are null;
     * they are not evidence that the collection failed to construct. */
    if (item_types.size() > 1) return {};

    for (const DbdRelation& relation : target_schema.relations) {
        if (!relation.collection) continue;
        if (item_types.empty() ||
            *item_types.begin() == relation.target_partition)
            candidates.push_back(&relation);
    }
    return candidates;
}

std::string relation_key(const std::string& owner,
                         const DbdRelation& relation)
{
    return owner + "\n" + relation.name + "\n" +
           relation.target_partition + (relation.collection ? "\n[]" : "\n1");
}

bool learn_relation_pins(
    const std::map<int, Node>& nodes,
    const std::vector<bf6_rime_connection>& wires,
    const std::map<int, DbdSchema>& schemas,
    std::map<std::string, uint32_t>& pins)
{
    pins.clear();
    Incoming incoming;
    for (const bf6_rime_connection& wire : wires)
        incoming[wire.target].push_back(&wire);
    for (const bf6_rime_connection& wire : wires) {
        const auto target = nodes.find(wire.target);
        const auto source = nodes.find(wire.source);
        if (target == nodes.end() || source == nodes.end() ||
            target->second.kind != Node::Kind::Record)
            continue;
        const auto schema = schemas.find(wire.target);
        if (schema == schemas.end()) return false;
        if (contract_input(schema->second.primitive, wire.target_field))
            continue;
        const std::vector<const DbdRelation*> candidates =
            compatible_relations(schema->second, source->second, wire.source,
                                 nodes, incoming);
        if (candidates.size() != 1) continue;
        const std::string key = relation_key(target->second.dbd,
                                             *candidates.front());
        const auto prior = pins.find(key);
        if (prior != pins.end() && prior->second != wire.target_field)
            return false;
        pins[key] = wire.target_field;
    }
    return true;
}

BuildCounts build_graph(
    const std::map<int, Node>& nodes,
    const std::vector<bf6_rime_connection>& wires,
    const std::map<int, DbdSchema>& schemas,
    const std::map<std::string, uint32_t>& relation_pins,
    const std::map<uint32_t, InterfaceDeclaration>& declarations,
    bool control, std::vector<Record>* records,
    std::vector<Collection>* collections,
    std::vector<PublicValue>* public_values,
    Audit* audit)
{
    BuildCounts counts{};
    Incoming incoming;
    for (const bf6_rime_connection& wire : wires)
        incoming[wire.target].push_back(&wire);

    for (const auto& pair : nodes) {
        const int instance = pair.first;
        const Node& node = pair.second;
        if (node.kind == Node::Kind::Record) {
            const auto schema_it = schemas.find(instance);
            if (schema_it == schemas.end()) continue;
            const DbdSchema& schema = schema_it->second;
            Record record;
            record.instance = instance;
            record.dbd_partition = node.dbd;
            record.contract = schema.primitive;
            std::map<uint32_t, Field> bound;
            bool valid = true;
            if (control && incoming[instance].empty()) continue;
            for (const bf6_rime_connection* wire : incoming[instance]) {
                const auto source = nodes.find(wire->source);
                if (source == nodes.end()) { valid = false; if (audit) ++audit->unresolved_inputs; continue; }
                const uint32_t target = control ? shuffled(wire->target_field)
                                                : wire->target_field;
                const rime_list::DbdField* field = contract_input(record.contract, target);
                const DbdRelation* relation = nullptr;
                if (!field) {
                    const std::vector<const DbdRelation*> candidates =
                        compatible_relations(schema, source->second,
                                             source->first, nodes, incoming);
                    if (candidates.size() == 1) {
                        const auto expected = relation_pins.find(
                            relation_key(node.dbd, *candidates.front()));
                        if (expected != relation_pins.end() &&
                            expected->second == target)
                            relation = candidates.front();
                    }
                    if (!relation) {
                        valid = false;
                        if (audit && !control) {
                            if (source->second.kind == Node::Kind::Record ||
                                source->second.kind == Node::Kind::Collection)
                                ++audit->relation_schema_misses;
                            else
                                ++audit->contract_field_misses;
                        }
                        continue;
                    }
                }
                const auto source_schema = schemas.find(source->first);
                Reference value = reference_for(
                    source->second, source->first, wire->source_field,
                    source_schema == schemas.end() ? nullptr : &source_schema->second,
                    control, audit);
                if (value.kind == ReferenceKind::Null) {
                    valid = false;
                    if (audit) ++audit->unresolved_inputs;
                    continue;
                }
                if (audit && !control) {
                    if (value.kind == ReferenceKind::Opaque)
                        ++audit->opaque_inputs;
                    else if (value.kind == ReferenceKind::External)
                        ++audit->external_inputs;
                }
                const uint32_t property = field ? field->property_id
                                                : relation->property_id;
                const auto prior = bound.find(property);
                if (prior != bound.end()) {
                    const bool prior_external =
                        prior->second.value.kind == ReferenceKind::External;
                    const bool next_external =
                        value.kind == ReferenceKind::External;
                    if (prior_external != next_external) {
                        /* The developer mock ships a concrete local value and
                         * a host InterfaceDescriptor fallback on the same
                         * record pin.  Offline, retain the concrete authored
                         * value while preserving all external-only inputs. */
                        if (prior_external)
                            prior->second.value = std::move(value);
                        continue;
                    }
                    valid = false;
                    if (audit) ++audit->ambiguous_inputs;
                    continue;
                }
                bound[property] = {property,
                                   field ? field->name : relation->name,
                                   std::move(value)};
            }
            if (!valid && control) continue;
            if (!valid) continue;
            for (auto& value : bound)
                record.fields.push_back(std::move(value.second));
            ++counts.records;
            if (records) records->push_back(std::move(record));
        } else if (node.kind == Node::Kind::Collection) {
            if (node.input_count < 0) continue;
            if (control && node.input_count == 0) continue;
            std::vector<Reference> ordered(
                static_cast<size_t>(node.input_count));
            std::vector<bool> seen(static_cast<size_t>(node.input_count), false);
            bool valid = true;
            for (const bf6_rime_connection* wire : incoming[instance]) {
                const auto source = nodes.find(wire->source);
                if (source == nodes.end()) { valid = false; if (audit) ++audit->unresolved_inputs; continue; }
                const auto source_schema = schemas.find(source->first);
                Reference value = reference_for(
                    source->second, source->first, wire->source_field,
                    source_schema == schemas.end() ? nullptr : &source_schema->second,
                    control, audit);
                if (value.kind == ReferenceKind::Null) { valid = false; if (audit) ++audit->unresolved_inputs; continue; }
                const uint32_t target = control ? shuffled(wire->target_field)
                                                : wire->target_field;
                int input_index = -1;
                if (!collection_input_index(target, node.input_count,
                                            input_index) ||
                    seen[static_cast<size_t>(input_index)]) {
                    valid = false;
                    continue;
                }
                seen[static_cast<size_t>(input_index)] = true;
                ordered[static_cast<size_t>(input_index)] = std::move(value);
            }
            if (!valid) {
                if (audit && !control) ++audit->unresolved_inputs;
                continue;
            }
            Collection collection;
            collection.instance = instance;
            collection.input_count = node.input_count;
            for (Reference& value : ordered)
                collection.items.push_back(std::move(value));
            ++counts.collections;
            if (collections) collections->push_back(std::move(collection));
        }
    }

    if (public_values) {
        for (const bf6_rime_connection& wire : wires) {
            const auto target = nodes.find(wire.target);
            const auto source = nodes.find(wire.source);
            if (target == nodes.end() || source == nodes.end() ||
                target->second.kind != Node::Kind::Interface)
                continue;
            const auto source_schema = schemas.find(source->first);
            Reference value = reference_for(
                source->second, source->first, wire.source_field,
                source_schema == schemas.end() ? nullptr : &source_schema->second,
                false, audit);
            const auto declaration = declarations.find(wire.target_field);
            if (value.kind != ReferenceKind::Null &&
                declaration != declarations.end()) {
                PublicValue result;
                result.field = wire.target_field;
                result.value = std::move(value);
                result.declared_partition = declaration->second.partition;
                result.declared_type_raw = declaration->second.raw_type;
                public_values->push_back(std::move(result));
                if (audit) ++audit->public_declared_values;
            }
        }
    }
    return counts;
}

std::string leaf(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

bool Snapshot::load(bf6_ctx* context, std::string& error)
{
    audit_ = {};
    records_.clear();
    collections_.clear();
    public_values_.clear();
    error.clear();
    if (!context) { error = "offline Home provider requires a mounted context"; return false; }

    std::string dump;
    if (!read_dump(context, kMockPartition, dump)) {
        error = "installed homescreenmockdata partition is unreadable";
        return false;
    }
    std::string fake;
    audit_.fake_partition_matches =
        read_dump(context, "common/ui/home/logic/__control_missing", fake) ? 1 : 0;

    std::map<int, Node> nodes;
    if (!parse_nodes(dump, nodes, audit_.instances)) {
        error = "homescreenmockdata instance census did not close";
        return false;
    }

    std::map<uint32_t, InterfaceDeclaration> declarations;
    const bool declarations_closed =
        parse_interface_declarations(dump, declarations);
    if (!declarations_closed || declarations.size() != 14) {
        error = "homescreenmockdata InterfaceDescriptor declarations did not close: " +
                std::to_string(declarations.size()) + "/14";
        return false;
    }
    audit_.interface_declarations = static_cast<int>(declarations.size());
    for (const auto& pair : nodes) {
        switch (pair.second.kind) {
        case Node::Kind::Record: ++audit_.record_nodes; break;
        case Node::Kind::Collection: ++audit_.collection_nodes; break;
        case Node::Kind::Scalar: ++audit_.scalar_nodes; break;
        default: break;
        }
    }

    const int count = bf6_rime_connections(context, kMockPartition, nullptr, 0);
    if (count < 0) { error = "homescreenmockdata property graph is unreadable"; return false; }
    std::vector<bf6_rime_connection> wires(static_cast<size_t>(count));
    if (count && bf6_rime_connections(context, kMockPartition,
                                      wires.data(), count) != count) {
        error = "homescreenmockdata property graph changed during count/fill";
        return false;
    }
    audit_.property_connections = count;

    /* These five graph edges are authored propagation, not duplicate stored
     * values: StringEntity.String supplies an initially blank
     * LocalizedString.DebugString.  Apply them before records bind the
     * localized nodes, and perturb both pins for the negative control. */
    std::map<int, Node> scalar_control_nodes = nodes;
    audit_.shuffled_scalar_propagations = propagate_scalar_strings(
        scalar_control_nodes, wires, true, nullptr);
    audit_.scalar_propagations_applied = propagate_scalar_strings(
        nodes, wires, false, &audit_.blank_scalar_targets_before);

    std::map<std::string, DbdSchema> by_path;
    std::map<int, DbdSchema> schemas;
    for (const auto& pair : nodes) {
        if (pair.second.kind != Node::Kind::Record || pair.second.dbd.empty())
            continue;
        auto found = by_path.find(pair.second.dbd);
        if (found == by_path.end()) {
            DbdSchema schema;
            if (!rime_list::load_contract(context, schema.primitive,
                                          pair.second.dbd.c_str())) {
                error = "offline Home DBD is unreadable: " + pair.second.dbd;
                return false;
            }
            std::string dbd_dump;
            if (!read_dump(context, pair.second.dbd.c_str(), dbd_dump) ||
                !parse_dbd_relations(dbd_dump, schema.relations)) {
                error = "offline Home DBD relations are unreadable: " +
                        pair.second.dbd;
                return false;
            }
            found = by_path.emplace(pair.second.dbd, std::move(schema)).first;
        }
        schemas[pair.first] = found->second;
    }
    if (schemas.size() != static_cast<size_t>(audit_.record_nodes)) {
        error = "offline Home record/DBD census mismatch";
        return false;
    }

    std::map<std::string, uint32_t> relation_pins;
    if (!learn_relation_pins(nodes, wires, schemas, relation_pins)) {
        error = "offline Home compound DBD pin identities are inconsistent";
        return false;
    }

    BuildCounts real = build_graph(nodes, wires, schemas, relation_pins,
                                   declarations, false,
                                   &records_, &collections_, &public_values_,
                                   &audit_);
    audit_.records_built = real.records;
    audit_.collections_built = real.collections;
    audit_.public_values = static_cast<int>(public_values_.size());

    /* Target ids are token-shaped in the negative control but no longer match
     * a DBD field or contiguous Collection input range. */
    Audit control_audit{};
    BuildCounts control = build_graph(nodes, wires, schemas, relation_pins,
                                      declarations, true,
                                      nullptr, nullptr, nullptr,
                                      &control_audit);
    audit_.shuffled_records_built = control.records;
    audit_.shuffled_collections_built = control.collections;
    audit_.shuffled_record_output_matches =
        control_audit.shuffled_record_output_matches;
    for (const bf6_rime_connection& wire : wires) {
        const auto target = nodes.find(wire.target);
        if (target != nodes.end() &&
            target->second.kind == Node::Kind::Interface &&
            declarations.find(shuffled(wire.target_field)) != declarations.end())
            ++audit_.shuffled_public_declarations;
    }

    std::sort(records_.begin(), records_.end(),
              [](const Record& a, const Record& b) { return a.instance < b.instance; });
    std::sort(collections_.begin(), collections_.end(),
              [](const Collection& a, const Collection& b) { return a.instance < b.instance; });
    if (!audit_.passed()) {
        std::ostringstream why;
        why << "offline Home provider gate failed: instances=" << audit_.instances
            << " wires=" << audit_.property_connections
            << " records=" << audit_.records_built << '/' << audit_.record_nodes
            << " collections=" << audit_.collections_built << '/'
            << audit_.collection_nodes << " public=" << audit_.public_values
            << " interface=" << audit_.public_declared_values << '/'
            << audit_.interface_declarations
            << " scalar-propagation=" << audit_.scalar_propagations_applied
            << " blank-before=" << audit_.blank_scalar_targets_before
            << " unresolved=" << audit_.unresolved_inputs
            << " ambiguous=" << audit_.ambiguous_inputs
            << " contract-miss=" << audit_.contract_field_misses
            << " relation-miss=" << audit_.relation_schema_misses
            << " record-outputs=" << audit_.record_output_references
            << " invalid-record-output=" << audit_.invalid_record_outputs
            << " opaque=" << audit_.opaque_inputs
            << " external=" << audit_.external_inputs
            << " controls=" << audit_.fake_partition_matches << '/'
            << audit_.shuffled_records_built << '/'
            << audit_.shuffled_collections_built << '/'
            << audit_.shuffled_public_declarations << '/'
            << audit_.shuffled_record_output_matches << '/'
            << audit_.shuffled_scalar_propagations;
        error = why.str();
        records_.clear(); collections_.clear(); public_values_.clear();
        return false;
    }
    return true;
}

const Record* Snapshot::record(int instance) const
{
    const auto found = std::lower_bound(
        records_.begin(), records_.end(), instance,
        [](const Record& row, int value) { return row.instance < value; });
    return found != records_.end() && found->instance == instance ? &*found : nullptr;
}

const Collection* Snapshot::collection(int instance) const
{
    const auto found = std::lower_bound(
        collections_.begin(), collections_.end(), instance,
        [](const Collection& row, int value) { return row.instance < value; });
    return found != collections_.end() && found->instance == instance ? &*found : nullptr;
}

const Field* Snapshot::field(const Record& source, const char* name) const
{
    if (!name) return nullptr;
    for (const Field& candidate : source.fields)
        if (candidate.name == name) return &candidate;
    return nullptr;
}

std::vector<const Record*> Snapshot::records_in(
    const Collection& source, const char* dbd_leaf) const
{
    std::vector<const Record*> result;
    for (const Reference& item : source.items) {
        if (item.kind != ReferenceKind::Record) continue;
        const Record* candidate = record(item.instance);
        if (!candidate) continue;
        if (dbd_leaf && *dbd_leaf && leaf(candidate->dbd_partition) != dbd_leaf)
            continue;
        result.push_back(candidate);
    }
    return result;
}

rime_list::Record Snapshot::primitive_record(const Record& source) const
{
    rime_list::Record result;
    for (const Field& field : source.fields) {
        if (field.value.kind != ReferenceKind::Scalar) continue;
        rime_list::Value value;
        switch (field.value.scalar.kind) {
        case ScalarKind::Bool:
            value.kind = rime_list::ValueKind::Bool;
            value.boolean = field.value.scalar.boolean;
            break;
        case ScalarKind::Int:
            value.kind = rime_list::ValueKind::Int;
            value.integer = field.value.scalar.integer;
            break;
        case ScalarKind::String:
            value.kind = rime_list::ValueKind::String;
            value.string = field.value.scalar.string;
            break;
        default: continue;
        }
        result.put(source.contract, field.property_id, value);
    }
    return result;
}

} // namespace offline_home
