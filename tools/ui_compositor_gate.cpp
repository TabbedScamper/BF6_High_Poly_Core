/*
 * Current-install Rime compositor exactness gate.
 *
 * This is deliberately an inventory/gate, not another renderer. It mounts the
 * selected BF6 install, expands one authored screen through bf6_rime_tree, and
 * reports every compositor semantic whose presence prevents a pixel-exact
 * frame in the current viewer. No research TSV/JSON is read at runtime.
 *
 * Exit 0: controls pass and this route needs none of the unresolved semantics.
 * Exit 3: controls pass, but the route is not compositor-exact yet.
 * Exit 4: a structural control failed.
 */
#include "bf6_core.h"
#include "rime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::string game;
    std::string root = "common/ui/weapons/screens/menuweaponscreen";
    std::string json_path;
    int depth = 8;
    bool allow_partial = false;
};

struct Semantic {
    const char* id;
    const char* guid;
    uint32_t signature;
    int kind;
    const char* exact_boundary;
};

/* These pairs are the shipped old-schema gates, not executable reflection.
 * A row only enters the inventory when both values agree. */
static const Semantic kSemantics[] = {
    {"masking", "562c3c80-3e40-00dd-2fa0-1028b84db932", 0x2B373A07u,
     BF6_RIME_CONTAINER,
     "general off-screen mask rasterization, label/texture masks, multiple and nested masks"},
    {"repeat_shape", "13cc8964-abd5-122b-5574-852ff36ab2df", 0x5E25FA53u,
     BF6_RIME_REPEAT_SHAPE,
     "mode-dependent pitch, sizing, radial placement, grid wrapping and clipping"},
    {"blur", "b2d8708a-ed17-4b72-1a78-f0707ba9ec41", 0x986517FFu,
     BF6_RIME_BLUR,
     "framebuffer kernel/radius, preset merge, dynamic curvature and World compositing"},
    {"movie", "73b1bf98-032b-4ba3-117b-07486dcfd9ac", 0x2B6B0D83u,
     BF6_RIME_MOVIE,
     "runtime-fed sources plus exact decode, timing, colour and subtitle composition"},
    {"flipbook", "ef91e7d0-29ce-b702-387f-e44dc49bcfa0", 0xF5E7D854u,
     BF6_RIME_FLIPBOOK,
     "frame selection, timing, blend and texture sampling"},
    {"texture_blend", "c8693168-e1ff-5aaf-9b94-e4b22a971686", 0x4F337E5Fu,
     BF6_RIME_TEXTURE_BLEND,
     "child render-target composition and animated blend execution"},
};

struct Item {
    const Semantic* semantic = nullptr;
    const bf6_rime_node* row = nullptr;
};

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
            if (c < 0x20) {
                const char* hex = "0123456789abcdef";
                out << "\\u00" << hex[(c >> 4) & 15] << hex[c & 15];
            } else out << static_cast<char>(c);
        }
    }
    return out.str();
}

static const Semantic* semantic_for_guid(const char* guid) {
    for (const Semantic& semantic : kSemantics)
        if (std::strcmp(guid, semantic.guid) == 0) return &semantic;
    return nullptr;
}

static std::string mutate_guid(const char* guid) {
    std::string value = guid ? guid : "";
    if (!value.empty()) value[0] = value[0] == '0' ? '1' : '0';
    return value;
}

static bool parse_options(int argc, char** argv, Options& options) {
    if (argc < 2) return false;
    options.game = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--root" && i + 1 < argc) options.root = argv[++i];
        else if (arg == "--depth" && i + 1 < argc)
            options.depth = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--json" && i + 1 < argc) options.json_path = argv[++i];
        else if (arg == "--allow-partial") options.allow_partial = true;
        else return false;
    }
    return true;
}

static void write_json(std::ostream& out, const Options& options,
                       const bf6_rime_tree_stats& stats,
                       const std::vector<bf6_rime_node>& rows,
                       const std::vector<Item>& items,
                       int fake_root, int perturbed_root,
                       int real_pair_matches, int perturbed_signature_matches,
                       int rotated_signature_matches, int mutated_guid_matches,
                       int kind_matches, int kind_trials,
                       const std::map<std::string, bool>& exact_execution,
                       int repeat_execution_trials, int repeat_execution_passes,
                       int repeat_shuffled_rejected, int repeat_radial_rejected,
                       int mask_execution_trials, int mask_execution_passes,
                       int mask_shuffled_rejected, int mask_complex_routes,
                       bool controls_passed, bool exact,
                       double mount_ms, double tree_ms) {
    std::map<std::string, int> counts;
    std::map<std::string, int> repeat_distributions;
    int repeat_instance_total = 0;
    for (const Item& item : items) {
        ++counts[item.semantic->id];
        if (std::strcmp(item.semantic->id, "repeat_shape") == 0) {
            const int distribution = item.row->repeat_distribution;
            const char* name = distribution == BF6_RIME_DIST_RADIAL ? "radial" :
                               distribution == BF6_RIME_DIST_HORIZONTAL ? "horizontal" :
                               distribution == BF6_RIME_DIST_VERTICAL ? "vertical" :
                               distribution == BF6_RIME_DIST_GRID ? "grid" : "invalid";
            ++repeat_distributions[name];
            repeat_instance_total += std::max(item.row->repeat_instances, 0);
        }
    }

    out << "{\n"
        << "  \"schema\": \"bf6-current-install-ui-compositor-gate-v1\",\n"
        << "  \"source\": \"current BF6 install through bf6_core frontend mount and raw Rime tree ABI\",\n"
        << "  \"runtime_inputs\": [\"" << json_escape(options.game)
        << "\", \"" << json_escape(options.root) << "\"],\n"
        << "  \"forbidden_research_inputs_used\": [],\n"
        << "  \"root\": \"" << json_escape(options.root) << "\",\n"
        << "  \"max_reference_depth\": " << options.depth << ",\n"
        << "  \"tree\": {\"rows\": " << rows.size()
        << ", \"nodes\": " << stats.nodes
        << ", \"gated_nodes\": " << stats.gated_nodes
        << ", \"unknown_types\": " << stats.unknown_types
        << ", \"unresolved_refs\": " << stats.unresolved_refs
        << ", \"ambiguous_refs\": " << stats.ambiguous_refs
        << ", \"cycles\": " << stats.cycles
        << ", \"depth_limited\": " << stats.depth_limited << "},\n"
        << "  \"controls\": {\n"
        << "    \"real_pair_trials\": " << items.size() << ",\n"
        << "    \"real_pair_matches\": " << real_pair_matches << ",\n"
        << "    \"perturbed_signature_matches\": " << perturbed_signature_matches << ",\n"
        << "    \"rotated_signature_matches\": " << rotated_signature_matches << ",\n"
        << "    \"mutated_guid_matches\": " << mutated_guid_matches << ",\n"
        << "    \"kind_trials\": " << kind_trials << ",\n"
        << "    \"kind_matches\": " << kind_matches << ",\n"
        << "    \"repeat_execution_trials\": " << repeat_execution_trials << ",\n"
        << "    \"repeat_execution_passes\": " << repeat_execution_passes << ",\n"
        << "    \"repeat_shuffled_count_rejected\": " << repeat_shuffled_rejected << ",\n"
        << "    \"repeat_unsupported_radial_rejected\": " << repeat_radial_rejected << ",\n"
        << "    \"mask_execution_trials\": " << mask_execution_trials << ",\n"
        << "    \"mask_exact_inverse_fill_passes\": " << mask_execution_passes << ",\n"
        << "    \"mask_non_inverted_shuffle_rejected\": " << mask_shuffled_rejected << ",\n"
        << "    \"mask_general_compositor_routes\": " << mask_complex_routes << ",\n"
        << "    \"fake_root_result\": " << fake_root << ",\n"
        << "    \"perturbed_root_result\": " << perturbed_root << ",\n"
        << "    \"passed\": " << (controls_passed ? "true" : "false") << "\n"
        << "  },\n"
        << "  \"semantics\": [\n";
    for (size_t i = 0; i < std::size(kSemantics); ++i) {
        const Semantic& semantic = kSemantics[i];
        const int count = counts[semantic.id];
        const auto implementation = exact_execution.find(semantic.id);
        const bool implemented = implementation != exact_execution.end() &&
                                 implementation->second;
        out << "    {\"id\": \"" << semantic.id << "\", \"element_count\": "
            << count << ", \"required_for_selected_route\": "
            << (count ? "true" : "false")
            << ", \"screen_can_be_exact_without_semantic\": "
            << ((!count || implemented) ? "true" : "false")
            << ", \"exact_execution_available\": "
            << (implemented ? "true" : "false")
            << ", \"implementation\": \""
            << (implemented ? "selected-route execution is linked and controlled"
                            : "exact compositor execution unavailable") << "\""
            << ", \"missing_exact_boundary\": \""
            << (implemented ? "" : json_escape(semantic.exact_boundary)) << "\"}";
        out << (i + 1 == std::size(kSemantics) ? "\n" : ",\n");
    }
    out << "  ],\n"
        << "  \"repeat_shape_summary\": {\"authored_instance_total\": "
        << repeat_instance_total << ", \"distributions\": {";
    bool first = true;
    for (const auto& entry : repeat_distributions) {
        if (!first) out << ", ";
        out << "\"" << entry.first << "\": " << entry.second;
        first = false;
    }
    out << "}},\n"
        << "  \"mask_summary\": {\"authored_containers\": "
        << mask_execution_trials << ", \"exact_inverse_flat_fill\": "
        << mask_execution_passes << ", \"general_compositor_required\": "
        << mask_complex_routes << "},\n"
        << "  \"elements\": [\n";
    for (size_t i = 0; i < items.size(); ++i) {
        const Item& item = items[i];
        const bf6_rime_node& row = *item.row;
        out << "    {\"semantic\": \"" << item.semantic->id
            << "\", \"partition\": \"" << json_escape(row.partition)
            << "\", \"instance\": " << row.instance
            << ", \"depth\": " << row.depth
            << ", \"name\": \"" << json_escape(row.name)
            << "\", \"type_guid\": \"" << row.type_guid
            << "\", \"type_signature\": \"0x" << std::hex
            << std::uppercase << row.type_signature << std::dec << "\"";
        if (row.kind == BF6_RIME_REPEAT_SHAPE)
            out << ", \"repeat_instances\": " << row.repeat_instances
                << ", \"repeat_distribution\": " << row.repeat_distribution;
        if (row.kind == BF6_RIME_MOVIE || row.kind == BF6_RIME_FLIPBOOK)
            out << ", \"authored_image_asset\": \""
                << json_escape(row.image_asset) << "\"";
        out << "}" << (i + 1 == items.size() ? "\n" : ",\n");
    }
    out << "  ],\n"
        << "  \"verdict\": {\n"
        << "    \"compositor_exact_for_selected_route\": "
        << (exact ? "true" : "false") << ",\n"
        << "    \"blocking_semantics\": [";
    first = true;
    for (const Semantic& semantic : kSemantics) {
        if (!counts[semantic.id]) continue;
        const auto implementation = exact_execution.find(semantic.id);
        if (implementation != exact_execution.end() && implementation->second)
            continue;
        if (!first) out << ", ";
        out << "\"" << semantic.id << "\"";
        first = false;
    }
    out << "],\n"
        << "    \"statement\": \""
        << (exact ? "No unresolved compositor semantic is present on this route."
                  : "The selected route cannot be claimed pixel-exact until every listed blocking semantic has exact execution.")
        << "\"\n"
        << "  },\n"
        << "  \"timing_ms\": {\"mount_frontend\": " << mount_ms
        << ", \"tree_and_controls\": " << tree_ms << "}\n"
        << "}\n";
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        std::fprintf(stderr,
            "usage: ui_compositor_gate <game-dir> [--root <partition>] "
            "[--depth N] [--json <report.json>] [--allow-partial]\n");
        return 2;
    }

    char error[1024]{};
    const auto mount_start = Clock::now();
    bf6_ctx* context = bf6_open(options.game.c_str(), error, sizeof(error));
    if (!context) {
        std::fprintf(stderr, "ui_compositor_gate: open failed: %s\n", error);
        return 1;
    }
    if (!bf6_mount_frontend(context, error, sizeof(error))) {
        std::fprintf(stderr, "ui_compositor_gate: frontend mount failed: %s\n", error);
        bf6_close(context);
        return 1;
    }
    const double mount_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - mount_start).count();

    const auto tree_start = Clock::now();
    bf6_rime_tree_stats stats{};
    const int count = bf6_rime_tree(context, options.root.c_str(), options.depth,
                                    nullptr, 0, &stats);
    if (count < 0) {
        std::fprintf(stderr, "ui_compositor_gate: unreadable root %s\n",
                     options.root.c_str());
        bf6_close(context);
        return 1;
    }
    std::vector<bf6_rime_node> rows(static_cast<size_t>(count));
    const int got = count ? bf6_rime_tree(context, options.root.c_str(), options.depth,
                                          rows.data(), count, &stats) : 0;
    rows.resize(static_cast<size_t>(std::max(got, 0)));

    std::vector<Item> items;
    int real_pair_matches = 0;
    int perturbed_signature_matches = 0;
    int rotated_signature_matches = 0;
    int mutated_guid_matches = 0;
    int kind_matches = 0;
    int kind_trials = 0;
    for (const bf6_rime_node& row : rows) {
        const Semantic* semantic = semantic_for_guid(row.type_guid);
        if (semantic) {
            const bool exact_pair = row.type_signature == semantic->signature;
            real_pair_matches += exact_pair;
            ++kind_trials;
            kind_matches += row.kind == semantic->kind;
            if (exact_pair) items.push_back({semantic, &row});
            perturbed_signature_matches +=
                row.type_signature == (semantic->signature ^ 1u);
            const size_t index = static_cast<size_t>(semantic - kSemantics);
            const Semantic& rotated = kSemantics[(index + 1) % std::size(kSemantics)];
            rotated_signature_matches += row.type_signature == rotated.signature;
        }
        for (const Semantic& candidate : kSemantics)
            mutated_guid_matches += row.type_guid == mutate_guid(candidate.guid);
    }

    std::map<std::string, bool> exact_execution;
    int repeat_execution_trials = 0;
    int repeat_execution_passes = 0;
    int repeat_shuffled_rejected = 0;
    int repeat_radial_rejected = 0;
    for (const Item& item : items) {
        if (std::strcmp(item.semantic->id, "repeat_shape") != 0) continue;
        ++repeat_execution_trials;
        const bf6_rime_node& row = *item.row;
        bf6_rime_shape_info shape{};
        const int shape_result = bf6_rime_shape(
            context, row.partition, row.instance, &shape,
            nullptr, 0, nullptr, 0, nullptr, 0);
        const bool has_geometry = shape_result > 0 &&
            ((shape.vertex_count > 0 && shape.index_count > 0) ||
             shape.corner_count > 0);
        rime::Element repeat;
        repeat.kind = rime::Kind::RepeatShape;
        repeat.solved = true;
        repeat.x0 = repeat.y0 = 0.f;
        repeat.x1 = row.width;
        repeat.y1 = row.height;
        repeat.repeat_instances = row.repeat_instances;
        repeat.repeat_distribution = row.repeat_distribution;
        std::vector<rime::RepeatCell> cells;
        const bool linear = rime::repeat_shape_cells(repeat, cells) &&
            cells.size() == static_cast<size_t>(row.repeat_instances);
        bool positive_cells = linear;
        for (const rime::RepeatCell& cell : cells)
            positive_cells = positive_cells && cell.x1 > cell.x0 && cell.y1 > cell.y0;

        rime::Element shuffled = repeat;
        ++shuffled.repeat_instances;
        std::vector<rime::RepeatCell> shuffled_cells;
        const bool shuffled_ok = rime::repeat_shape_cells(shuffled, shuffled_cells);
        if (linear && shuffled_ok && !shuffled_cells.empty() &&
            std::abs((cells[0].x1 - cells[0].x0) -
                     (shuffled_cells[0].x1 - shuffled_cells[0].x0)) > 0.001f)
            ++repeat_shuffled_rejected;

        rime::Element unsupported = repeat;
        unsupported.repeat_distribution = BF6_RIME_DIST_RADIAL;
        std::vector<rime::RepeatCell> unsupported_cells;
        if (!rime::repeat_shape_cells(unsupported, unsupported_cells))
            ++repeat_radial_rejected;
        if (has_geometry && positive_cells) ++repeat_execution_passes;
    }
    if (repeat_execution_trials > 0)
        exact_execution["repeat_shape"] =
            repeat_execution_passes == repeat_execution_trials &&
            repeat_shuffled_rejected == repeat_execution_trials &&
            repeat_radial_rejected == repeat_execution_trials;

    int mask_execution_trials = 0;
    int mask_execution_passes = 0;
    int mask_shuffled_rejected = 0;
    int mask_complex_routes = 0;
    rime::Screen mask_screen;
    std::string mask_error;
    std::vector<unsigned char> mask_visible;
    std::vector<float> mask_alpha;
    const bool mask_tree_ok = rime::from_live(
        rows.data(), (int)rows.size(), mask_screen, mask_error);
    if (mask_tree_ok)
    {
        rime::solve(mask_screen, 1920.f, 1080.f);
        rime::effective_paint_state(mask_screen.elements, mask_visible,
                                    mask_alpha);
        for (int owner = 0; owner < (int)mask_screen.elements.size(); ++owner)
        {
            rime::Element& element = mask_screen.elements[(size_t)owner];
            if (element.mask_owner >= 0 || element.masking_mode < 0) continue;
            ++mask_execution_trials;
            const int fill = rime::simple_inverse_fill_mask(
                mask_screen.elements, owner, mask_visible, mask_alpha);
            if (fill >= 0)
            {
                ++mask_execution_passes;
                const int saved = element.invert_mask;
                element.invert_mask = 0;
                mask_shuffled_rejected += rime::simple_inverse_fill_mask(
                    mask_screen.elements, owner, mask_visible, mask_alpha) < 0;
                element.invert_mask = saved;
            }
            else ++mask_complex_routes;
        }
    }
    if (mask_execution_trials > 0)
        exact_execution["masking"] =
            mask_execution_passes == mask_execution_trials &&
            mask_shuffled_rejected == mask_execution_passes;

    bf6_rime_tree_stats ignored{};
    const int fake_root = bf6_rime_tree(
        context, "common/ui/__control__/not_a_real_screen", options.depth,
        nullptr, 0, &ignored);
    const std::string perturbed_name = options.root + "__control__";
    const int perturbed_root = bf6_rime_tree(
        context, perturbed_name.c_str(), options.depth, nullptr, 0, &ignored);
    const double tree_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - tree_start).count();

    const bool repeat_controls_passed = repeat_execution_trials == 0 ||
        (repeat_shuffled_rejected == repeat_execution_trials &&
         repeat_radial_rejected == repeat_execution_trials);
    const bool mask_controls_passed = mask_tree_ok &&
        mask_shuffled_rejected == mask_execution_passes;
    const bool controls_passed = got == count && stats.unknown_types == 0 &&
        stats.unresolved_refs == 0 && stats.ambiguous_refs == 0 &&
        stats.depth_limited == 0 &&
        real_pair_matches == kind_trials &&
        real_pair_matches == static_cast<int>(items.size()) &&
        kind_matches == kind_trials && perturbed_signature_matches == 0 &&
        rotated_signature_matches == 0 && mutated_guid_matches == 0 &&
        fake_root < 0 && perturbed_root < 0 && repeat_controls_passed &&
        mask_controls_passed;
    bool exact = controls_passed;
    for (const Item& item : items) {
        const auto implementation = exact_execution.find(item.semantic->id);
        if (implementation == exact_execution.end() || !implementation->second) {
            exact = false;
            break;
        }
    }

    if (options.json_path.empty()) {
        write_json(std::cout, options, stats, rows, items, fake_root,
                   perturbed_root, real_pair_matches,
                   perturbed_signature_matches, rotated_signature_matches,
                   mutated_guid_matches, kind_matches, kind_trials,
                   exact_execution, repeat_execution_trials,
                   repeat_execution_passes, repeat_shuffled_rejected,
                   repeat_radial_rejected, mask_execution_trials,
                   mask_execution_passes, mask_shuffled_rejected,
                   mask_complex_routes,
                   controls_passed, exact, mount_ms, tree_ms);
    } else {
        std::ofstream output(options.json_path, std::ios::binary);
        if (!output) {
            std::fprintf(stderr, "ui_compositor_gate: cannot write %s\n",
                         options.json_path.c_str());
            bf6_close(context);
            return 1;
        }
        write_json(output, options, stats, rows, items, fake_root,
                   perturbed_root, real_pair_matches,
                   perturbed_signature_matches, rotated_signature_matches,
                   mutated_guid_matches, kind_matches, kind_trials,
                   exact_execution, repeat_execution_trials,
                   repeat_execution_passes, repeat_shuffled_rejected,
                   repeat_radial_rejected, mask_execution_trials,
                   mask_execution_passes, mask_shuffled_rejected,
                   mask_complex_routes,
                   controls_passed, exact, mount_ms, tree_ms);
    }

    std::fprintf(stderr,
        "ui_compositor_gate: rows=%d compositor-elements=%zu controls=%s exact=%s\n",
        got, items.size(), controls_passed ? "PASS" : "FAIL",
        exact ? "YES" : "NO");
    bf6_close(context);
    if (!controls_passed) return 4;
    return (exact || options.allow_partial) ? 0 : 3;
}
