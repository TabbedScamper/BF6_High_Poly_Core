/* Export one shipped game-mode layer as a lossless, provenance-bearing JSON
 * manifest. Classification and Godot authoring happen downstream; this file
 * contains only records decoded from the installed game. */
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <string>
#include <vector>
#include "bf6_core.h"

static void json_string(std::ostream& out, const char* value)
{
    if (!value) { out << "null"; return; }
    out << '"';
    for (const unsigned char c : std::string(value)) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    const char* hex = "0123456789abcdef";
                    out << "\\u00" << hex[c >> 4] << hex[c & 15];
                } else out << (char)c;
        }
    }
    out << '"';
}

static void float_array(std::ostream& out, const float* values, int count)
{
    out << '[';
    for (int i = 0; i < count; i++) {
        if (i) out << ',';
        out << values[i];
    }
    out << ']';
}

int main(int argc, char** argv)
{
    if (argc != 5) {
        std::fprintf(stderr, "usage: gamemode_export <game-dir> <level> <mode> <output.json>\n");
        return 2;
    }
    char error[512] = {};
    bf6_ctx* context = bf6_open(argv[1], error, sizeof(error));
    if (!context) {
        std::fprintf(stderr, "open failed: %s\n", error);
        return 1;
    }
    bf6_gm_stats stats{};
    const int count = bf6_level_gamemodes(context, argv[2], nullptr, 0, &stats, error, sizeof(error));
    if (count <= 0) {
        std::fprintf(stderr, "extract failed: %s\n", error);
        bf6_close(context);
        return 1;
    }
    std::vector<bf6_gm_entity> rows((size_t)count);
    bf6_level_gamemodes(context, argv[2], rows.data(), count, nullptr, error, sizeof(error));

    std::ofstream out(argv[4], std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", argv[4]);
        bf6_close(context);
        return 1;
    }
    out << std::setprecision(9);
    out << "{\n  \"schema\":1,\n  \"source\":{\"kind\":\"installed_bf6\",\"level\":";
    json_string(out, argv[2]);
    out << ",\"mode\":";
    json_string(out, argv[3]);
    out << "},\n  \"entities\":[\n";
    bool first = true;
    int written = 0;
    for (const bf6_gm_entity& row : rows) {
        if (!row.mode || std::string(row.mode) != argv[3]) continue;
        if (!first) out << ",\n";
        first = false;
        written++;
        out << "    {\"kind\":" << row.kind << ",\"type\":";
        json_string(out, row.type_name);
        out << ",\"layer\":"; json_string(out, row.layer);
        out << ",\"partition\":"; json_string(out, row.partition);
        out << ",\"instance\":" << row.instance << ",\"root_order\":" << row.root_order
            << ",\"instance_guid\":";
        json_string(out, row.instance_guid);
        out << ",\"blueprint\":"; json_string(out, row.blueprint);
        out << ",\"transform\":"; float_array(out, row.xform, 12);
        out << ",\"team\":" << row.team
            << ",\"enabled\":" << (row.enabled ? "true" : "false")
            << ",\"flags\":" << row.flags;
        if (row.point_count > 0) {
            out << ",\"points\":"; float_array(out, row.points, row.point_count * 3);
            out << ",\"height\":" << row.height;
        }
        if (row.kind == BF6_GM_OBB) {
            out << ",\"half_extents\":"; float_array(out, row.half_extents, 3);
        }
        if (row.owner_type) {
            out << ",\"owner_type\":"; json_string(out, row.owner_type);
            out << ",\"owner_kind\":" << row.owner_kind;
        }
        if (row.gem_link) {
            out << ",\"gem_blueprint\":"; json_string(out, row.gem_link);
            out << ",\"gem_selector\":" << row.gem_value;
        }
        if (row.gem_shape) {
            out << ",\"gem_shape\":"; json_string(out, row.gem_shape);
            out << ",\"gem_shape_property\":" << row.gem_shape_property;
            std::string physics_name(row.gem_shape);
            if (physics_name.size() > 4 &&
                physics_name.compare(physics_name.size() - 4, 4, ".ebx") == 0)
                physics_name.resize(physics_name.size() - 4);
            if (bf6_physics* physics = bf6_physics_read(context, physics_name.c_str())) {
                out << ",\"gem_shape_physics\":{";
                out << "\"shape_count\":" << physics->shape_count
                    << ",\"instance_count\":" << physics->inst_count
                    << ",\"vertices\":";
                float_array(out, physics->vertices, physics->vertex_total * 3);
                out << ",\"indices\":[";
                for (int index = 0; index < physics->index_total; index++) {
                    if (index) out << ',';
                    out << physics->indices[index];
                }
                out << ']';
                out << ",\"shapes\":[";
                for (int shape_index = 0; shape_index < physics->shape_count; shape_index++) {
                    if (shape_index) out << ',';
                    const bf6_phys_shape& shape = physics->shapes[shape_index];
                    out << "{\"vertex_count\":" << shape.vertex_count
                        << ",\"vertex_first\":" << shape.vertex_first
                        << ",\"index_count\":" << shape.index_count
                        << ",\"index_first\":" << shape.index_first << '}';
                }
                out << "],\"instances\":[";
                for (int instance_index = 0; instance_index < physics->inst_count; instance_index++) {
                    if (instance_index) out << ',';
                    const bf6_phys_inst& instance = physics->instances[instance_index];
                    out << "{\"shape_type\":" << instance.shape_type
                        << ",\"shape_index\":" << instance.shape_index
                        << ",\"position\":";
                    float_array(out, instance.pos, 3);
                    out << ",\"rotation\":";
                    float_array(out, instance.quat, 4);
                    out << ",\"scale_or_halfheight\":" << instance.scale_or_halfheight
                        << '}';
                }
                out << "]}";
                bf6_free(context, physics);
            }
        }
        if (row.link_count > 0) {
            out << ",\"links\":[";
            for (int i = 0; i < row.link_count; i++) {
                if (i) out << ',';
                out << "{\"entity\":" << row.links[i] << ",\"field\":" << row.link_fields[i] << '}';
            }
            out << ']';
        }
        out << '}';
    }
    out << "\n  ],\n  \"counts\":{\"entities\":" << written
        << ",\"mounted_entities\":" << count
        << ",\"parse_failures\":" << stats.parse_fail
        << ",\"unresolved_types\":" << stats.unresolved_types << "}\n}\n";
    out.close();
    bf6_close(context);
    std::printf("wrote %d %s/%s entities to %s\n", written, argv[2], argv[3], argv[4]);
    return 0;
}
