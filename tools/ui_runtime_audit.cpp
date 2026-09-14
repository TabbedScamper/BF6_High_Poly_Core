/*
 * Current-install BF6 UI runtime coverage harness.
 *
 * This is intentionally not a second UI implementation.  It mounts the game
 * once, asks libbf6 for the exact shipped Rime graph, executes every runtime
 * operation the current adapter supports, and reports what remains unresolved.
 * The JSON/SVG it writes are diagnostics only and are never runtime inputs.
 *
 * Usage:
 *   ui_runtime_audit <game-dir> [--root <partition>] [--all]
 *                    [--json <report.json>] [--svg <coverage.svg>]
 */
#include "bf6_core.h"
#include "rime.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::string game;
    std::vector<std::string> roots;
    std::string json_path;
    std::string svg_path;
    bool all = false;
};

struct Counts {
    int tree_rows = 0;
    int gated_rows = 0;
    int unknown_types = 0;
    int unresolved_refs = 0;
    int ambiguous_refs = 0;
    int cycles = 0;
    int depth_limited = 0;
    int partitions = 0;
    int property_connections = 0;
    int event_connections = 0;
    int conditional_floats = 0;
    int conditional_properties = 0;
    int interfaces = 0;
    int interface_fields = 0;
    int interface_null_fields = 0;
    int interface_typed_fields = 0;
    int interface_unsupported_fields = 0;
    int interface_struct_fields = 0;
    int interface_struct_types_resolved = 0;
    int interface_unconnected_typed_fields = 0;
    int interface_connected_unpainted_fields = 0;
    int interface_probe_routed_fields = 0;
    int interface_probe_writes = 0;
    int interface_probe_ambiguous = 0;
    int dbd_fields = 0;
    int authored_text_bindings = 0;
    int authored_text_ambiguous = 0;
    int compiled_conditional_bindings = 0;
    int applied_conditional_bindings = 0;
    int compiled_interface_graphs = 0;
    int applied_interface_defaults = 0;
    int runtime_color_inputs = 0;
    int runtime_state_inputs = 0;
    int labels = 0;
    int labels_with_text = 0;
    int labels_runtime_unresolved = 0;
    int shapes = 0;
    int shapes_decoded = 0;
    int lines = 0;
    int lines_decoded = 0;
    int images = 0;
    int images_decoded = 0;
    int images_missing = 0;
    int images_unsupported = 0;
    int font_references = 0;
    int solved_boxes = 0;
    int invalid_boxes = 0;
    int blur_nodes = 0;
    int movie_nodes = 0;
    int repeat_nodes = 0;
    int flipbook_nodes = 0;
    int texture_blend_nodes = 0;
    int input_nodes = 0;
    int remote_presenters = 0;
};

struct Controls {
    int fake_root = 0;
    int fake_connections = 0;
    int fake_events = 0;
    int fake_interfaces = 0;
    int fake_interface_fields = 0;
    int fake_interface_struct_types = 0;
    int fake_line_instance = 0;
    int fake_property_writes = 0;
    int name_hash_real = 0;
    int name_hash_trials = 0;
    int name_hash_rotated = 0;
    int endpoint_real_matches = 0;
    int endpoint_perturbed_matches = 0;
};

struct Gap {
    std::string category;
    std::string partition;
    std::string identity;
    std::string detail;
};

struct RootReport {
    std::string root;
    bool readable = false;
    Counts count;
    Controls controls;
    std::map<int, int> kinds;
    std::map<std::string, int> concrete_types;
    std::vector<Gap> gaps;
    rime::Screen solved;
    double tree_ms = 0.0;
    double runtime_ms = 0.0;
};

struct BuildId {
    uint64_t exe_size = 0;
    uint32_t coff_timestamp = 0;
};

static double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

static uint32_t djb2_xor(const char* value) {
    uint32_t hash = 5381u;
    if (!value) return hash;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + ('a' - 'A'));
        hash = hash * 33u ^ c;
    }
    return hash;
}

static bool contains(const std::string& value, const char* needle) {
    return value.find(needle) != std::string::npos;
}

static bool frontend_screen(const std::string& path) {
    if (!contains(path, "/screens/")) return false;
    return contains(path, "common/ui/bootflow/") ||
           contains(path, "game/glacierflow/flow_mainmenu/ui/screens/") ||
           contains(path, "common/ui/universalmenu/") ||
           contains(path, "common/ui/weapons/") ||
           contains(path, "common/ui/weaponcustomization/");
}

static std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (unsigned char c : value) {
        switch (c) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) out << "\\u" << std::hex << std::setw(4)
                              << std::setfill('0') << static_cast<int>(c) << std::dec;
            else out << static_cast<char>(c);
        }
    }
    return out.str();
}

static std::string xml_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

static BuildId read_build_id(const std::string& game) {
    BuildId out{};
    const std::string path = game + "\\bf6.exe";
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return out;
    out.exe_size = static_cast<uint64_t>(file.tellg());
    file.seekg(0x3c);
    uint32_t pe = 0;
    file.read(reinterpret_cast<char*>(&pe), sizeof(pe));
    if (!file || pe + 12 > out.exe_size) return out;
    file.seekg(static_cast<std::streamoff>(pe + 8));
    file.read(reinterpret_cast<char*>(&out.coff_timestamp), sizeof(out.coff_timestamp));
    return out;
}

static const char* kind_name(int kind) {
    switch (kind) {
    case BF6_RIME_UNKNOWN: return "unknown";
    case BF6_RIME_WIDGET_REFERENCE: return "widget_reference";
    case BF6_RIME_CONTAINER: return "container";
    case BF6_RIME_STACK_CONTAINER: return "stack_container";
    case BF6_RIME_LABEL: return "label";
    case BF6_RIME_LAYER: return "layer";
    case BF6_RIME_REPEAT_SHAPE: return "repeat_shape";
    case BF6_RIME_VECTOR_SHAPE: return "vector_shape";
    case BF6_RIME_FILL: return "fill";
    case BF6_RIME_SVG: return "svg";
    case BF6_RIME_TEXTURE: return "texture";
    case BF6_RIME_LINE: return "line";
    case BF6_RIME_MOVIE: return "movie";
    case BF6_RIME_BORDER: return "border";
    case BF6_RIME_BLUR: return "blur";
    case BF6_RIME_PROGRESS: return "progress";
    case BF6_RIME_ARC_PROGRESS: return "arc_progress";
    case BF6_RIME_FLIPBOOK: return "flipbook";
    case BF6_RIME_TEXTURE_BLEND: return "texture_blend";
    case BF6_RIME_INPUT_BEHAVIOR: return "input_behavior";
    case BF6_RIME_LAYERED_ICON_BINDING: return "layered_icon_binding";
    case BF6_RIME_HARDWARE_ICON_BINDING: return "hardware_icon_binding";
    case BF6_RIME_REMOTE_WIDGET_PRESENTER: return "remote_widget_presenter";
    default: return "invalid";
    }
}

static void add_gap(RootReport& report, const char* category,
                    const std::string& partition, const std::string& identity,
                    const std::string& detail) {
    report.gaps.push_back({category, partition, identity, detail});
}

template<typename T>
static int read_rows(int count, std::vector<T>& rows) {
    rows.resize(static_cast<size_t>(std::max(count, 0)));
    return count;
}

static int probe_interface_field(rime::Screen& screen, const std::string& partition,
                                 const bf6_rime_interface_field& field,
                                 int* ambiguous) {
    switch (field.value_kind) {
    case BF6_RIME_VALUE_BOOL:
        return rime::set_interface_bool_field(screen, partition.c_str(), field.field_id,
                                              field.bool_value != 0, ambiguous);
    case BF6_RIME_VALUE_INT:
        return rime::set_interface_int_field(screen, partition.c_str(), field.field_id,
                                             field.int_value, ambiguous);
    case BF6_RIME_VALUE_UINT:
        return rime::set_interface_uint_field(screen, partition.c_str(), field.field_id,
                                              field.uint_value, ambiguous);
    case BF6_RIME_VALUE_REAL:
        return rime::set_interface_real_field(screen, partition.c_str(), field.field_id,
                                              field.real_value, ambiguous);
    case BF6_RIME_VALUE_STRING:
        return rime::set_interface_text_field(screen, partition.c_str(), field.field_id,
                                              field.string_value, ambiguous);
    default:
        return -1;
    }
}

static std::string hex_u32(uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

static std::string outgoing_endpoints(
    const std::vector<bf6_rime_connection>& connections,
    int32_t interface_instance, uint32_t field_id,
    const std::map<int32_t, std::string>* target_identity = nullptr) {
    std::set<std::string> endpoints;
    for (size_t connection_index = 0;
         connection_index < connections.size(); ++connection_index) {
        const bf6_rime_connection& connection = connections[connection_index];
        if (connection.source != interface_instance ||
            connection.source_field != field_id)
            continue;
        std::ostringstream endpoint;
        endpoint << "edge=" << connection_index
                 << ":instance=" << connection.target
                 << ":field=" << hex_u32(connection.target_field)
                 << ":mode=" << connection.mode;
        if (target_identity) {
            const auto target = target_identity->find(connection.target);
            endpoint << ":target="
                     << (target == target_identity->end()
                             ? "nonvisual_graph_node"
                             : target->second);
        }
        endpoints.insert(endpoint.str());
    }
    std::ostringstream out;
    size_t index = 0;
    for (const std::string& endpoint : endpoints) {
        if (index++) out << ',';
        out << endpoint;
    }
    return out.str();
}

static RootReport audit_root(bf6_ctx* context, const std::string& root) {
    RootReport report;
    report.root = root;
    const auto tree_start = Clock::now();
    bf6_rime_tree_stats stats{};
    const int count = bf6_rime_tree(context, root.c_str(), 8, nullptr, 0, &stats);
    if (count <= 0) {
        report.controls.fake_root = bf6_rime_tree(
            context, "common/ui/__control__/not_a_real_screen", 8, nullptr, 0, nullptr);
        report.tree_ms = elapsed_ms(tree_start);
        add_gap(report, "root_unreadable", root, "", "bf6_rime_tree returned " + std::to_string(count));
        return report;
    }

    std::vector<bf6_rime_node> rows(static_cast<size_t>(count));
    const int got = bf6_rime_tree(context, root.c_str(), 8, rows.data(), count, &stats);
    report.tree_ms = elapsed_ms(tree_start);
    if (got != count) {
        add_gap(report, "tree_count_mismatch", root, "",
                "count=" + std::to_string(count) + " fill=" + std::to_string(got));
        return report;
    }
    report.readable = true;
    report.count.tree_rows = got;
    report.count.gated_rows = stats.gated_nodes;
    report.count.unknown_types = stats.unknown_types;
    report.count.unresolved_refs = stats.unresolved_refs;
    report.count.ambiguous_refs = stats.ambiguous_refs;
    report.count.cycles = stats.cycles;
    report.count.depth_limited = stats.depth_limited;

    std::set<std::string> partitions;
    std::map<std::string, std::map<int32_t, std::string>> target_identity;
    for (const bf6_rime_node& row : rows) {
        report.kinds[row.kind]++;
        std::ostringstream type_key;
        type_key << row.type_guid << "@0x" << std::hex << std::setw(8)
                 << std::setfill('0') << row.type_signature;
        if (row.type_name[0]) type_key << ":" << row.type_name;
        report.concrete_types[type_key.str()]++;
        if (row.partition[0]) partitions.insert(row.partition);
        if (row.partition[0]) {
            std::ostringstream identity;
            identity << kind_name(row.kind);
            if (row.type_name[0]) identity << ':' << row.type_name;
            if (row.name[0]) identity << ':' << row.name;
            target_identity[row.partition][row.instance] = identity.str();
        }
        if (row.kind == BF6_RIME_UNKNOWN)
            add_gap(report, "unknown_concrete_type", row.partition, row.name, type_key.str());
        if (row.kind == BF6_RIME_WIDGET_REFERENCE && !row.reference[0])
            add_gap(report, "unresolved_widget_reference", row.partition, row.name,
                    "authored reference did not resolve uniquely");
    }
    report.count.partitions = static_cast<int>(partitions.size());

    const auto runtime_start = Clock::now();
    std::string adapter_error;
    if (!rime::from_live(rows.data(), got, report.solved, adapter_error)) {
        add_gap(report, "adapter_failure", root, "", adapter_error);
        report.runtime_ms = elapsed_ms(runtime_start);
        return report;
    }

    rime::load_shapes(context, report.solved);
    rime::load_images(context, report.solved);
    report.count.authored_text_bindings =
        rime::load_text_bindings(context, report.solved, &report.count.authored_text_ambiguous);
    report.count.compiled_conditional_bindings =
        rime::load_conditional_float_bindings(context, report.solved);
    report.count.compiled_interface_graphs =
        rime::load_interface_text_graphs(context, report.solved);
    report.count.applied_interface_defaults = rime::apply_interface_defaults(report.solved);
    report.count.applied_conditional_bindings =
        rime::apply_conditional_float_bindings(report.solved);
    report.count.runtime_color_inputs =
        rime::mark_unresolved_interface_colors(context, report.solved);
    report.count.runtime_state_inputs =
        rime::mark_unresolved_interface_states(context, report.solved);
    rime::solve(report.solved, 1920.f, 1080.f);

    for (const std::string& partition : partitions) {
        int n = bf6_rime_connections(context, partition.c_str(), nullptr, 0);
        std::vector<bf6_rime_connection> connections;
        read_rows(n, connections);
        if (n > 0) {
            bf6_rime_connections(context, partition.c_str(), connections.data(), n);
            report.count.property_connections += n;
        }
        n = bf6_rime_event_connections(context, partition.c_str(), nullptr, 0);
        if (n > 0) report.count.event_connections += n;
        n = bf6_rime_conditional_floats(context, partition.c_str(), nullptr, 0);
        if (n > 0) report.count.conditional_floats += n;
        n = bf6_rime_conditional_properties(context, partition.c_str(), nullptr, 0);
        if (n > 0) report.count.conditional_properties += n;
        n = bf6_rime_interface_descriptors(context, partition.c_str(), nullptr, 0);
        if (n > 0) report.count.interfaces += n;

        const int field_count = bf6_rime_interface_fields(
            context, partition.c_str(), nullptr, 0);
        std::vector<bf6_rime_interface_field> fields;
        read_rows(field_count, fields);
        if (field_count > 0)
            bf6_rime_interface_fields(context, partition.c_str(), fields.data(), field_count);
        report.count.interface_fields += std::max(field_count, 0);

        const int struct_count = bf6_rime_interface_struct_types(
            context, partition.c_str(), nullptr, 0);
        std::vector<bf6_rime_interface_struct_type> struct_types;
        read_rows(struct_count, struct_types);
        if (struct_count > 0)
            bf6_rime_interface_struct_types(
                context, partition.c_str(), struct_types.data(), struct_count);
        std::map<std::pair<int32_t, uint32_t>, std::string> struct_type_by_field;
        for (const bf6_rime_interface_struct_type& type : struct_types)
            struct_type_by_field[{type.interface_instance, type.field_id}] = type.type_guid;

        for (size_t field_index = 0; field_index < fields.size(); ++field_index) {
            const bf6_rime_interface_field& field = fields[field_index];
            const std::string outgoing = outgoing_endpoints(
                connections, field.interface_instance, field.field_id,
                &target_identity[partition]);
            if (!outgoing.empty()) ++report.controls.endpoint_real_matches;
            if (!outgoing_endpoints(connections, field.interface_instance,
                                    field.field_id ^ 1u).empty())
                ++report.controls.endpoint_perturbed_matches;
            if (field.value_kind == BF6_RIME_VALUE_NULL) {
                report.count.interface_null_fields++;
                add_gap(report, "runtime_interface_input", partition,
                        hex_u32(field.field_id),
                        "Null authored default; live provider owns this value");
                continue;
            }
            if (field.value_kind < BF6_RIME_VALUE_BOOL ||
                field.value_kind > BF6_RIME_VALUE_STRING) {
                report.count.interface_unsupported_fields++;
                std::string detail = "value_kind=" + std::to_string(field.value_kind);
                if (field.value_kind == BF6_RIME_VALUE_STRUCT) {
                    ++report.count.interface_struct_fields;
                    const auto type = struct_type_by_field.find(
                        {field.interface_instance, field.field_id});
                    if (type != struct_type_by_field.end() && !type->second.empty()) {
                        ++report.count.interface_struct_types_resolved;
                        detail += ";type_guid=" + type->second;
                    } else {
                        detail += ";type_guid=unresolved";
                    }
                }
                detail += outgoing.empty()
                    ? ";route=unconnected"
                    : ";route_targets=" + outgoing;
                add_gap(report, "unsupported_interface_value", partition,
                        hex_u32(field.field_id), detail);
                continue;
            }
            report.count.interface_typed_fields++;
            rime::Screen probe = report.solved;
            int ambiguous = 0;
            const int writes = probe_interface_field(probe, partition, field, &ambiguous);
            if (writes > 0) {
                report.count.interface_probe_routed_fields++;
                report.count.interface_probe_writes += writes;
            }
            report.count.interface_probe_ambiguous += ambiguous;
            if (writes == 0) {
                if (outgoing.empty()) {
                    ++report.count.interface_unconnected_typed_fields;
                    add_gap(report, "unconnected_typed_interface_default", partition,
                            hex_u32(field.field_id),
                            "no authored outgoing edge; not a paint-route blocker");
                } else {
                    ++report.count.interface_connected_unpainted_fields;
                    add_gap(report, "connected_unpainted_interface_route", partition,
                            hex_u32(field.field_id), "route_targets=" + outgoing);
                }
            }
        }

        char data_name[256]{};
        const int dbd_count = bf6_rime_dbd_fields(
            context, partition.c_str(), data_name, static_cast<int>(sizeof(data_name)),
            nullptr, 0);
        if (dbd_count > 0) {
            report.count.dbd_fields += dbd_count;
            add_gap(report, "provider_schema_requires_runtime_rows", partition, data_name,
                    std::to_string(dbd_count) + " opaque typed provider fields");
        }
    }

    for (const rime::Element& element : report.solved.elements) {
        if (element.kind == rime::Kind::Label) {
            report.count.labels++;
            if (!element.text.empty()) report.count.labels_with_text++;
            else {
                report.count.labels_runtime_unresolved++;
                add_gap(report, "label_text_unresolved", element.partition, element.name,
                        "no authored string reached this label; runtime provider/localization required");
            }
            if (!element.font_style.empty()) report.count.font_references++;
        }
        if (element.kind == rime::Kind::VectorShape ||
            element.kind == rime::Kind::RepeatShape) {
            report.count.shapes++;
            if (!element.shape_verts.empty() || !element.shape_corners.empty())
                report.count.shapes_decoded++;
            else add_gap(report, "shape_geometry_unresolved", element.partition, element.name,
                         "no authored geometry resolved; the shape is null or supplied by a runtime binding");
        }
        if (element.kind == rime::Kind::Line) {
            report.count.lines++;
            if (!element.line_points.empty()) report.count.lines_decoded++;
            else add_gap(report, "line_geometry_missing", element.partition, element.name,
                         "gated line produced no points");
        }
        if (!element.image_asset.empty()) {
            report.count.images++;
            if (element.image_decode_status > 0) report.count.images_decoded++;
            else if (element.image_decode_status == -2) {
                report.count.images_unsupported++;
                add_gap(report, "image_decode_unsupported", element.partition, element.name,
                        element.image_asset);
            } else {
                report.count.images_missing++;
                add_gap(report, "image_missing", element.partition, element.name,
                        element.image_asset);
            }
        }
        if (element.solved) {
            report.count.solved_boxes++;
            if (element.x1 < element.x0 || element.y1 < element.y0) {
                report.count.invalid_boxes++;
                add_gap(report, "invalid_solved_box", element.partition, element.name,
                        "negative width or height after layout solve");
            }
        }
        report.count.blur_nodes += element.kind == rime::Kind::Blur;
        report.count.movie_nodes += element.kind == rime::Kind::Movie;
        report.count.repeat_nodes += element.kind == rime::Kind::RepeatShape;
        report.count.flipbook_nodes += element.kind == rime::Kind::Flipbook;
        report.count.texture_blend_nodes += element.kind == rime::Kind::TextureBlend;
        report.count.input_nodes += element.kind == rime::Kind::InputBehavior;
        report.count.remote_presenters += element.kind == rime::Kind::RemoteWidgetPresenter;
    }

    if (report.count.blur_nodes)
        add_gap(report, "compositor_semantics", root, "blur",
                "authored blur parameters are decoded; exact framebuffer kernel remains open");
    if (report.count.movie_nodes)
        add_gap(report, "runtime_media", root, "movie",
                "movie element is authored; dynamic/null sources still require the runtime provider");
    if (report.count.repeat_nodes)
        add_gap(report, "repeat_shape_placement", root, "repeat_shape",
                "shape/count are decoded; full mode-dependent placement law remains open");
    if (report.count.flipbook_nodes || report.count.texture_blend_nodes)
        add_gap(report, "animated_compositor", root, "flipbook_or_texture_blend",
                "authored node retained; exact compositor execution is not implemented");
    if (report.count.event_connections)
        add_gap(report, "event_execution", root, "event_graph",
                std::to_string(report.count.event_connections) +
                " authored event wires decoded but navigation/host event dispatch is not executed");

    report.controls.fake_root = bf6_rime_tree(
        context, "common/ui/__control__/not_a_real_screen", 8, nullptr, 0, nullptr);
    report.controls.fake_connections = bf6_rime_connections(
        context, "common/ui/__control__/not_a_real_screen", nullptr, 0);
    report.controls.fake_events = bf6_rime_event_connections(
        context, "common/ui/__control__/not_a_real_screen", nullptr, 0);
    report.controls.fake_interfaces = bf6_rime_interface_descriptors(
        context, "common/ui/__control__/not_a_real_screen", nullptr, 0);
    report.controls.fake_interface_fields = bf6_rime_interface_fields(
        context, "common/ui/__control__/not_a_real_screen", nullptr, 0);
    report.controls.fake_interface_struct_types = bf6_rime_interface_struct_types(
        context, "common/ui/__control__/not_a_real_screen", nullptr, 0);
    report.controls.fake_line_instance = bf6_rime_line(
        context, root.c_str(), 0x7fffffff, nullptr, nullptr, 0);
    rime::Screen fake_probe = report.solved;
    report.controls.fake_property_writes = rime::set_interface_text_field(
        fake_probe, root.c_str(), 0xdeadbeefu, "BF6_UI_AUDIT_CONTROL", nullptr);
    for (size_t i = 0; i < rows.size(); ++i) {
        const bf6_rime_node& row = rows[i];
        if (!row.name[0] || !row.name_hash) continue;
        report.controls.name_hash_trials++;
        report.controls.name_hash_real += djb2_xor(row.name) == row.name_hash;
        const bf6_rime_node& rotated = rows[(i + 1) % rows.size()];
        if (rotated.name[0])
            report.controls.name_hash_rotated += djb2_xor(rotated.name) == row.name_hash;
    }

    report.runtime_ms = elapsed_ms(runtime_start);
    return report;
}

static void write_count_object(std::ostream& out, const Counts& c, int indent) {
    const std::vector<std::pair<const char*, int>> values = {
        {"tree_rows", c.tree_rows}, {"gated_rows", c.gated_rows},
        {"unknown_types", c.unknown_types}, {"unresolved_refs", c.unresolved_refs},
        {"ambiguous_refs", c.ambiguous_refs}, {"cycles", c.cycles},
        {"depth_limited", c.depth_limited}, {"partitions", c.partitions},
        {"property_connections", c.property_connections},
        {"event_connections", c.event_connections},
        {"conditional_floats", c.conditional_floats},
        {"conditional_properties", c.conditional_properties},
        {"interfaces", c.interfaces}, {"interface_fields", c.interface_fields},
        {"interface_null_fields", c.interface_null_fields},
        {"interface_typed_fields", c.interface_typed_fields},
        {"interface_unsupported_fields", c.interface_unsupported_fields},
        {"interface_struct_fields", c.interface_struct_fields},
        {"interface_struct_types_resolved", c.interface_struct_types_resolved},
        {"interface_unconnected_typed_fields", c.interface_unconnected_typed_fields},
        {"interface_connected_unpainted_fields", c.interface_connected_unpainted_fields},
        {"interface_probe_routed_fields", c.interface_probe_routed_fields},
        {"interface_probe_writes", c.interface_probe_writes},
        {"interface_probe_ambiguous", c.interface_probe_ambiguous},
        {"dbd_fields", c.dbd_fields},
        {"authored_text_bindings", c.authored_text_bindings},
        {"authored_text_ambiguous", c.authored_text_ambiguous},
        {"compiled_conditional_bindings", c.compiled_conditional_bindings},
        {"applied_conditional_bindings", c.applied_conditional_bindings},
        {"compiled_interface_graphs", c.compiled_interface_graphs},
        {"applied_interface_defaults", c.applied_interface_defaults},
        {"runtime_color_inputs", c.runtime_color_inputs},
        {"runtime_state_inputs", c.runtime_state_inputs},
        {"labels", c.labels}, {"labels_with_text", c.labels_with_text},
        {"labels_runtime_unresolved", c.labels_runtime_unresolved},
        {"shapes", c.shapes}, {"shapes_decoded", c.shapes_decoded},
        {"lines", c.lines}, {"lines_decoded", c.lines_decoded},
        {"images", c.images}, {"images_decoded", c.images_decoded},
        {"images_missing", c.images_missing},
        {"images_unsupported", c.images_unsupported},
        {"font_references", c.font_references},
        {"solved_boxes", c.solved_boxes}, {"invalid_boxes", c.invalid_boxes},
        {"blur_nodes", c.blur_nodes}, {"movie_nodes", c.movie_nodes},
        {"repeat_nodes", c.repeat_nodes}, {"flipbook_nodes", c.flipbook_nodes},
        {"texture_blend_nodes", c.texture_blend_nodes},
        {"input_nodes", c.input_nodes}, {"remote_presenters", c.remote_presenters}
    };
    out << "{\n";
    for (size_t i = 0; i < values.size(); ++i) {
        out << std::string(static_cast<size_t>(indent + 2), ' ') << '"'
            << values[i].first << "\": " << values[i].second
            << (i + 1 == values.size() ? "\n" : ",\n");
    }
    out << std::string(static_cast<size_t>(indent), ' ') << '}';
}

static void write_controls(std::ostream& out, const Controls& c, int indent) {
    out << "{\n";
    const std::vector<std::pair<const char*, int>> values = {
        {"fake_root", c.fake_root}, {"fake_connections", c.fake_connections},
        {"fake_events", c.fake_events}, {"fake_interfaces", c.fake_interfaces},
        {"fake_interface_fields", c.fake_interface_fields},
        {"fake_interface_struct_types", c.fake_interface_struct_types},
        {"fake_line_instance", c.fake_line_instance},
        {"fake_property_writes", c.fake_property_writes},
        {"name_hash_real", c.name_hash_real},
        {"name_hash_trials", c.name_hash_trials},
        {"name_hash_rotated", c.name_hash_rotated},
        {"endpoint_real_matches", c.endpoint_real_matches},
        {"endpoint_perturbed_matches", c.endpoint_perturbed_matches}
    };
    for (size_t i = 0; i < values.size(); ++i) {
        out << std::string(static_cast<size_t>(indent + 2), ' ') << '"'
            << values[i].first << "\": " << values[i].second
            << (i + 1 == values.size() ? "\n" : ",\n");
    }
    out << std::string(static_cast<size_t>(indent), ' ') << '}';
}

static void write_report_json(std::ostream& out, const Options& options,
                              const BuildId& build, double mount_ms,
                              int photon_count, const bf6_photon_bundle_info& photon,
                              const std::vector<RootReport>& reports) {
    out << "{\n  \"schema\": \"bf6-ui-runtime-audit-v2\",\n";
    out << "  \"runtime_inputs\": [\"current installed CAS/catalogues\", "
           "\"current installed bf6.exe build identity\"],\n";
    out << "  \"forbidden_runtime_inputs_used\": [],\n";
    out << "  \"game_dir\": \"" << json_escape(options.game) << "\",\n";
    out << "  \"build\": {\"exe_size\": " << build.exe_size
        << ", \"coff_timestamp\": " << build.coff_timestamp << "},\n";
    out << "  \"timing_ms\": {\"mount_frontend\": " << std::fixed
        << std::setprecision(3) << mount_ms << "},\n";
    out << "  \"photon_offline_assets\": {\"rows\": " << photon_count
        << ", \"asset_count\": " << photon.asset_count
        << ", \"chunk_size\": " << photon.chunk_size
        << ", \"ranges_in_bounds\": " << photon.ranges_in_bounds
        << ", \"ranges_contiguous\": " << photon.ranges_contiguous
        << ", \"signatures_valid\": " << photon.signatures_valid << "},\n";
    out << "  \"roots\": [\n";
    for (size_t ri = 0; ri < reports.size(); ++ri) {
        const RootReport& r = reports[ri];
        out << "    {\n      \"root\": \"" << json_escape(r.root) << "\",\n";
        out << "      \"readable\": " << (r.readable ? "true" : "false") << ",\n";
        out << "      \"timing_ms\": {\"tree\": " << r.tree_ms
            << ", \"runtime\": " << r.runtime_ms << "},\n";
        out << "      \"counts\": "; write_count_object(out, r.count, 6); out << ",\n";
        out << "      \"controls\": "; write_controls(out, r.controls, 6); out << ",\n";
        out << "      \"kinds\": {";
        size_t ki = 0;
        for (const auto& entry : r.kinds) {
            if (ki++) out << ',';
            out << "\n        \"" << kind_name(entry.first) << "\": " << entry.second;
        }
        if (!r.kinds.empty()) out << '\n' << "      ";
        out << "},\n      \"concrete_types\": {";
        size_t ti = 0;
        for (const auto& entry : r.concrete_types) {
            if (ti++) out << ',';
            out << "\n        \"" << json_escape(entry.first) << "\": " << entry.second;
        }
        if (!r.concrete_types.empty()) out << '\n' << "      ";
        out << "},\n      \"gaps\": [";
        for (size_t gi = 0; gi < r.gaps.size(); ++gi) {
            const Gap& g = r.gaps[gi];
            if (gi) out << ',';
            out << "\n        {\"category\": \"" << json_escape(g.category)
                << "\", \"partition\": \"" << json_escape(g.partition)
                << "\", \"identity\": \"" << json_escape(g.identity)
                << "\", \"detail\": \"" << json_escape(g.detail) << "\"}";
        }
        if (!r.gaps.empty()) out << '\n' << "      ";
        out << "]\n    }" << (ri + 1 == reports.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
}

static bool write_svg(const std::string& path, const RootReport& report) {
    if (!report.readable) return false;
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 1920 1080\" "
           "width=\"1920\" height=\"1080\">\n";
    out << "<rect width=\"1920\" height=\"1080\" fill=\"#10151a\"/>\n";
    out << "<style>text{font:10px monospace;fill:#e6edf3} .ok{fill:none;stroke:#3fb950;stroke-width:1} "
           ".dynamic{fill:none;stroke:#d29922;stroke-width:1.5} .unknown{fill:none;stroke:#f85149;stroke-width:2}</style>\n";
    for (const rime::Element& e : report.solved.elements) {
        if (!e.solved || e.x1 <= e.x0 || e.y1 <= e.y0) continue;
        const bool unknown = e.kind == rime::Kind::Unknown;
        const bool dynamic = e.runtime_color_unresolved || e.runtime_visibility_unresolved ||
                             e.runtime_alpha_unresolved ||
                             (e.kind == rime::Kind::Label && e.text.empty());
        out << "<rect class=\"" << (unknown ? "unknown" : dynamic ? "dynamic" : "ok")
            << "\" x=\"" << e.x0 << "\" y=\"" << e.y0 << "\" width=\""
            << (e.x1 - e.x0) << "\" height=\"" << (e.y1 - e.y0) << "\"/>\n";
        if ((unknown || dynamic) && !e.name.empty())
            out << "<text x=\"" << e.x0 + 2 << "\" y=\"" << e.y0 + 11 << "\">"
                << xml_escape(e.name) << "</text>\n";
    }
    out << "<g transform=\"translate(16 24)\"><rect x=\"-8\" y=\"-18\" width=\"650\" "
           "height=\"58\" fill=\"#000\" opacity=\".75\"/>"
           "<text>green=decoded/authored  amber=runtime input  red=unknown concrete type</text>"
           "<text y=\"18\">" << xml_escape(report.root) << "</text></g>\n</svg>\n";
    return true;
}

static bool parse_options(int argc, char** argv, Options& options) {
    if (argc < 2) return false;
    options.game = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--root" && i + 1 < argc) options.roots.emplace_back(argv[++i]);
        else if (arg == "--json" && i + 1 < argc) options.json_path = argv[++i];
        else if (arg == "--svg" && i + 1 < argc) options.svg_path = argv[++i];
        else if (arg == "--all") options.all = true;
        else return false;
    }
    if (options.roots.empty() && !options.all)
        options.roots.emplace_back("common/ui/weapons/screens/menuweaponscreen");
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        std::fprintf(stderr,
            "usage: ui_runtime_audit <game-dir> [--root <partition>] [--all] "
            "[--json <report.json>] [--svg <coverage.svg>]\n");
        return 2;
    }

    char error[1024]{};
    const auto mount_start = Clock::now();
    bf6_ctx* context = bf6_open(options.game.c_str(), error, static_cast<int>(sizeof(error)));
    if (!context) {
        std::fprintf(stderr, "ui_runtime_audit: open failed: %s\n", error);
        return 1;
    }
    if (!bf6_mount_frontend(context, error, static_cast<int>(sizeof(error)))) {
        std::fprintf(stderr, "ui_runtime_audit: frontend mount failed: %s\n", error);
        bf6_close(context);
        return 1;
    }
    const double mount_ms = elapsed_ms(mount_start);

    if (options.all) {
        const int count = bf6_list_ebx(context, "/screens/", nullptr, 0);
        std::vector<bf6_asset> assets(static_cast<size_t>(std::max(count, 0)));
        if (count > 0) bf6_list_ebx(context, "/screens/", assets.data(), count);
        for (const bf6_asset& asset : assets)
            if (asset.name && frontend_screen(asset.name)) options.roots.emplace_back(asset.name);
        std::sort(options.roots.begin(), options.roots.end());
        options.roots.erase(std::unique(options.roots.begin(), options.roots.end()), options.roots.end());
    }

    bf6_photon_bundle_info photon{};
    const int photon_count = bf6_photon_offline_assets(context, &photon, nullptr, 0);
    std::vector<RootReport> reports;
    reports.reserve(options.roots.size());
    for (const std::string& root : options.roots) {
        RootReport report = audit_root(context, root);
        std::fprintf(stderr,
            "ui_runtime_audit: %s rows=%d gated=%d gaps=%zu controls(fake/root/property)=%d/%d\n",
            root.c_str(), report.count.tree_rows, report.count.gated_rows,
            report.gaps.size(), report.controls.fake_root,
            report.controls.fake_property_writes);
        reports.emplace_back(std::move(report));
    }

    const BuildId build = read_build_id(options.game);
    if (!options.svg_path.empty()) {
        if (reports.size() != 1 || !write_svg(options.svg_path, reports.front())) {
            std::fprintf(stderr, "ui_runtime_audit: --svg requires one readable root and a writable path\n");
            bf6_close(context);
            return 1;
        }
    }

    if (options.json_path.empty()) write_report_json(
        std::cout, options, build, mount_ms, photon_count, photon, reports);
    else {
        std::ofstream out(options.json_path, std::ios::binary);
        if (!out) {
            std::fprintf(stderr, "ui_runtime_audit: could not write %s\n", options.json_path.c_str());
            bf6_close(context);
            return 1;
        }
        write_report_json(out, options, build, mount_ms, photon_count, photon, reports);
    }

    bool controls_ok = photon_count > 0 && photon.asset_count == photon_count;
    int readable_roots = 0;
    int name_hash_real = 0, name_hash_rotated = 0, name_hash_trials = 0;
    int endpoint_real_matches = 0, endpoint_perturbed_matches = 0;
    for (const RootReport& report : reports) {
        if (!report.readable) {
            if (!options.all) controls_ok = false;
            continue;
        }
        ++readable_roots;
        name_hash_real += report.controls.name_hash_real;
        name_hash_rotated += report.controls.name_hash_rotated;
        name_hash_trials += report.controls.name_hash_trials;
        endpoint_real_matches += report.controls.endpoint_real_matches;
        endpoint_perturbed_matches += report.controls.endpoint_perturbed_matches;
        controls_ok = controls_ok &&
            report.controls.fake_root < 0 && report.controls.fake_connections < 0 &&
            report.controls.fake_events < 0 && report.controls.fake_interfaces < 0 &&
            report.controls.fake_interface_fields < 0 &&
            report.controls.fake_interface_struct_types < 0 &&
            report.controls.fake_line_instance < 0 &&
            report.controls.fake_property_writes == 0;
    }
    /* Widget-reference NameHash has a distinct producer on at least one tiny
     * boot screen.  The ordinary element law is therefore a population
     * control, not a per-root invariant; keep every per-root score in JSON. */
    controls_ok = controls_ok && readable_roots > 0 && name_hash_trials > 0 &&
                  name_hash_real > name_hash_rotated &&
                  endpoint_real_matches > endpoint_perturbed_matches;
    bf6_close(context);
    return controls_ok ? 0 : 3;
}
