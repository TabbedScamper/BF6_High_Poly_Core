#include "bf6_core.h"
#include "offline_home_presenter.h"
#include "rime.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

bool load_screen(bf6_ctx* context, const char* partition,
                 rime::Screen& screen)
{
    bf6_rime_tree_stats stats{};
    const int count = bf6_rime_tree(context, partition, 6, nullptr, 0,
                                    &stats);
    if (count <= 0) return false;
    std::vector<bf6_rime_node> rows(static_cast<size_t>(count));
    if (bf6_rime_tree(context, partition, 6, rows.data(), count, &stats) !=
        count)
        return false;
    std::string error;
    if (!rime::from_live(rows.data(), count, screen, error)) return false;
    rime::load_shapes(context, screen);
    rime::load_images(context, screen);
    rime::load_text_bindings(context, screen);
    rime::load_interface_text_graphs(context, screen);
    rime::load_conditional_float_bindings(context, screen);
    rime::apply_conditional_float_bindings(screen);
    rime::apply_interface_defaults(screen);
    return true;
}

int local_element(const rime::Screen& screen, const char* name)
{
    int result = -1;
    int matches = 0;
    for (size_t index = 0; index < screen.elements.size(); ++index) {
        const rime::Element& element = screen.elements[index];
        if (element.partition != screen.partition || element.name != name)
            continue;
        result = static_cast<int>(index);
        ++matches;
    }
    return matches == 1 ? result : -1;
}

void dump_measurement(const char* label, rime::Screen row,
                      rime::Screen section)
{
    rime::solve(section, 1920.f, 1080.f);
    rime::solve(row, 1920.f, 1080.f);
    const int grid_index = local_element(section, "DiceUIUniformGridList");
    if (grid_index < 0) {
        std::printf("probe %s owner-grid missing\n", label);
        return;
    }
    rime::Element grid = section.elements[static_cast<size_t>(grid_index)];
    grid.item_template = row.partition;
    rime::UniformGridLayout layout;
    const bool laid_out = rime::uniform_grid_layout(grid, row, layout);
    std::printf("probe %s grid solved=%d box=%.1f,%.1f..%.1f,%.1f "
                "size=%.1fx%.1f fit=%d count-mode=%d distribution=%d "
                "item-fit=%d flow=%d/%d row-elements=%zu layout=%d\n",
                label, grid.solved ? 1 : 0, grid.x0, grid.y0, grid.x1, grid.y1,
                grid.grid_column_size, grid.grid_row_size,
                grid.grid_static_segment_item_count,
                grid.grid_segment_count_mode,
                grid.grid_segment_distribution, grid.grid_item_fit_content,
                grid.grid_column_flow_direction,
                grid.grid_row_flow_direction,
                row.elements.size(), laid_out ? 1 : 0);
    int roots = 0;
    for (size_t index = 0; index < row.elements.size(); ++index) {
        const rime::Element& element = row.elements[index];
        if (element.parent >= 0) continue;
        ++roots;
        std::printf("probe %s root index=%zu name=%s kind=%d solved=%d "
                    "box=%.1f,%.1f..%.1f,%.1f\n", label, index,
                    element.name.c_str(), static_cast<int>(element.kind),
                    element.solved ? 1 : 0, element.x0, element.y0,
                    element.x1, element.y1);
        for (size_t child_index = 0; child_index < row.elements.size();
             ++child_index) {
            const rime::Element& child = row.elements[child_index];
            if (child.parent != static_cast<int>(index)) continue;
            std::printf("probe %s child index=%zu name=%s kind=%d solved=%d "
                        "w=%.1f h=%.1f anchor=%.3f/%.3f,%.3f/%.3f "
                        "box=%.1f,%.1f..%.1f,%.1f ref=%s\n",
                        label, child_index, child.name.c_str(),
                        static_cast<int>(child.kind), child.solved ? 1 : 0,
                        child.width, child.height,
                        child.h.anchor_start, child.h.anchor_end,
                        child.v.anchor_start, child.v.anchor_end,
                        child.x0, child.y0, child.x1, child.y1,
                        child.references_widget.c_str());
        }
    }
    std::printf("probe %s roots=%d\n", label, roots);
}

} // namespace

int main(int argc, char** argv)
{
    const char* game = argc > 1 ? argv[1]
        : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Battlefield 6";
    char buffer[512]{};
    bf6_ctx* context = bf6_open(game, buffer, sizeof(buffer));
    if (!context || !bf6_mount_frontend(context, buffer, sizeof(buffer))) {
        std::fprintf(stderr, "open/mount: %s\n", buffer);
        if (context) bf6_close(context);
        return 2;
    }
    rime::Screen home, section, row, page, card;
    const bool screens =
        load_screen(context, "common/ui/home/screens/home_screen", home) &&
        load_screen(context, "common/ui/home/widgets/homesectioncell", section) &&
        load_screen(context, "common/ui/home/widgets/gamecardrowcell", row) &&
        load_screen(context, "common/ui/home/widgets/gamecardpagecell", page) &&
        load_screen(context, "common/ui/home/widgets/gamecardcell", card);
    rime::solve(home, 1920.f, 1080.f);
    const int main_list = local_element(home, "MainList");
    if (main_list >= 0) {
        const rime::Element& list = home.elements[static_cast<size_t>(main_list)];
        std::printf("probe main-list type=%s kind=%d parent=%d box=%.1f,%.1f..%.1f,%.1f "
                    "stack=%d/%d/%d datalist=%d/%d/%d/%d spacing=%.1f pad=%.1f/%.1f/%.1f/%.1f template=%s candidates=%zu\n",
                    list.type_name.c_str(), static_cast<int>(list.kind), list.parent,
                    list.x0, list.y0, list.x1, list.y1,
                    list.stack_orientation, list.stack_size_distribution,
                    list.stack_space_distribution,
                    list.data_list_size_distribution,
                    list.data_list_flow_direction,
                    list.data_list_space_distribution,
                    list.data_list_preserve_fit_content, list.item_spacing,
                    list.pad_l, list.pad_t, list.pad_r, list.pad_b,
                    list.item_template.c_str(), list.item_templates.size());
        if (list.parent >= 0) {
            const rime::Element& parent = home.elements[static_cast<size_t>(list.parent)];
            std::printf("probe main-list-parent name=%s type=%s kind=%d stack=%d box=%.1f,%.1f..%.1f,%.1f\n",
                        parent.name.c_str(), parent.type_name.c_str(),
                        static_cast<int>(parent.kind), parent.stack_orientation,
                        parent.x0, parent.y0, parent.x1, parent.y1);
        }
    }
    dump_measurement("section-row", row, section);
    for (size_t index = 0; index < section.elements.size(); ++index) {
        const rime::Element& element = section.elements[index];
        if (element.parent < 0 || element.parent == 0)
            std::printf("probe section-frame index=%zu parent=%d name=%s type=%s kind=%d box=%.1f,%.1f..%.1f,%.1f anchor=%.3f/%.3f,%.3f/%.3f fit=%d/%d\n",
                        index, element.parent, element.name.c_str(),
                        element.type_name.c_str(), static_cast<int>(element.kind),
                        element.x0, element.y0, element.x1, element.y1,
                        element.h.anchor_start, element.h.anchor_end,
                        element.v.anchor_start, element.v.anchor_end,
                        element.fit_w ? 1 : 0, element.fit_h ? 1 : 0);
    }
    dump_measurement("row-page", page, row);
    dump_measurement("page-card", card, page);

    offline_home::Snapshot snapshot;
    std::string error;
    const bool provider = snapshot.load(context, error);
    offline_home::PresentationAudit audit;
    rime::Screen output;
    const offline_home::Templates templates{&section, &row, &page, &card};
    const bool result = screens && provider && offline_home::materialize_home(
        snapshot, templates, home, 1920.f, 1080.f, output, audit, error);
    bool resolve_stable = result;
    bool absent_axis_control_rejected = false;
    bool loaded_spinner_hidden = false;
    if (result) {
        for (const rime::Element& element : output.elements)
            if (element.partition == output.partition &&
                element.name == "PAC" &&
                element.kind == rime::Kind::Flipbook && !element.visible)
                loaded_spinner_hidden = true;
        int first_card_root = -1;
        for (size_t index = 0; index < output.elements.size(); ++index) {
            const rime::Element& element = output.elements[index];
            if (element.partition == card.partition &&
                element.name == "ParentContainer") {
                first_card_root = static_cast<int>(index);
                break;
            }
        }
        if (first_card_root >= 0) {
            for (size_t index = 0; index < output.elements.size(); ++index) {
                const rime::Element& element = output.elements[index];
                int ancestor = static_cast<int>(index);
                bool belongs = false;
                for (int guard = 0; ancestor >= 0 &&
                     ancestor < static_cast<int>(output.elements.size()) &&
                     guard < 96; ++guard) {
                    if (ancestor == first_card_root) {
                        belongs = true;
                        break;
                    }
                    ancestor = output.elements[static_cast<size_t>(ancestor)].parent;
                }
                if (!belongs) continue;
                std::printf("probe first-card index=%zu parent=%d depth=%d "
                            "kind=%d visible=%d name=%s text=%s image=%s "
                            "box=%.1f,%.1f..%.1f,%.1f\n",
                            index, element.parent, element.depth,
                            static_cast<int>(element.kind),
                            element.visible ? 1 : 0, element.name.c_str(),
                            element.text.c_str(), element.image_asset.c_str(),
                            element.x0, element.y0, element.x1, element.y1);
            }
        }
        for (size_t index = 0; index < output.elements.size(); ++index) {
            const rime::Element& element = output.elements[index];
            const float width = element.x1 - element.x0;
            const float height = element.y1 - element.y0;
            if (!element.visible || !element.solved ||
                !((width >= 20.f && width <= 50.f && height >= 100.f) ||
                  element.text.find("MULTI") != std::string::npos))
                continue;
            std::printf("probe home-artifact index=%zu parent=%d kind=%d "
                        "name=%s text=%s partition=%s "
                        "box=%.1f,%.1f..%.1f,%.1f\n",
                        index, element.parent, static_cast<int>(element.kind),
                        element.name.c_str(), element.text.c_str(),
                        element.partition.c_str(), element.x0, element.y0,
                        element.x1, element.y1);
        }
        std::vector<float> card_widths;
        for (size_t index = 0; index < output.elements.size(); ++index) {
            const rime::Element& element = output.elements[index];
            if (element.partition != card.partition ||
                (element.name != "Black" &&
                 element.name != "ParentContainer" &&
                 element.parent >= 0))
                continue;
            std::printf("probe materialized-card index=%zu parent=%d name=%s kind=%d box=%.1f,%.1f..%.1f,%.1f size=%.1fx%.1f\n",
                        index, element.parent, element.name.c_str(),
                        static_cast<int>(element.kind), element.x0, element.y0,
                        element.x1, element.y1, element.x1 - element.x0,
                        element.y1 - element.y0);
            if (element.name == "ParentContainer")
                card_widths.push_back(element.x1 - element.x0);
        }
        for (size_t index = 0; index < output.elements.size(); ++index) {
            const rime::Element& element = output.elements[index];
            if (element.name != "Black") continue;
            std::printf("probe black index=%zu parent=%d partition=%s box=%.1f,%.1f..%.1f,%.1f size=%.1fx%.1f\n",
                        index, element.parent, element.partition.c_str(),
                        element.x0, element.y0, element.x1, element.y1,
                        element.x1 - element.x0, element.y1 - element.y0);
        }

        rime::Screen second_solve = output;
        rime::solve(second_solve, 1920.f, 1080.f);
        size_t card_index = 0;
        for (const rime::Element& element : second_solve.elements) {
            if (element.partition != card.partition ||
                element.name != "ParentContainer")
                continue;
            if (card_index >= card_widths.size() ||
                std::fabs((element.x1 - element.x0) -
                          card_widths[card_index]) > 0.01f)
                resolve_stable = false;
            ++card_index;
        }
        resolve_stable = resolve_stable && card_index == card_widths.size() &&
                         !card_widths.empty();

        rime::Screen control = output;
        int controlled_root = -1;
        for (size_t index = 0; index < control.elements.size(); ++index) {
            rime::Element& element = control.elements[index];
            if (element.partition == card.partition &&
                element.kind == rime::Kind::LayerEntity &&
                element.parent >= 0) {
                controlled_root = static_cast<int>(index);
                element.h.present = false;
                element.v.present = false;
                break;
            }
        }
        if (controlled_root >= 0) {
            rime::solve(control, 1920.f, 1080.f);
            for (const rime::Element& element : control.elements) {
                if (element.partition != card.partition ||
                    element.name != "ParentContainer")
                    continue;
                int ancestor = element.parent;
                while (ancestor >= 0 && ancestor != controlled_root)
                    ancestor = control.elements[
                        static_cast<size_t>(ancestor)].parent;
                if (ancestor == controlled_root &&
                    std::fabs((element.x1 - element.x0) -
                              card_widths.front()) > 0.01f) {
                    absent_axis_control_rejected = true;
                    break;
                }
            }
        }
        std::printf("probe second-solve-stable=%d absent-axis-control-rejected=%d\n",
                    resolve_stable ? 1 : 0,
                    absent_axis_control_rejected ? 1 : 0);
    }
    std::printf("probe materialized=%d candidates=%d accepted=%d rejected=%d "
                "sections=%d/%d rows=%d/%d pages=%d/%d cards=%d/%d "
                "null=%d missing=%d invalid-layout=%d bound=%d/%d "
                "non-applicable=%d unrouted=%d root-routes=%d list-routes=%d "
                "root-control=%d list-control=%d spinner=%d/%d hidden=%d "
                "passed=%d error=%s\n",
                result ? 1 : 0, audit.public_play_menu_candidates,
                audit.public_play_menus,
                audit.rejected_play_menu_declarations,
                audit.visible_sections, audit.sections,
                audit.visible_rows, audit.rows,
                audit.visible_pages, audit.pages,
                audit.visible_cards, audit.cards,
                audit.null_collection_items, audit.missing_relations,
                audit.invalid_layouts, audit.bound_fields,
                audit.bound_targets, audit.non_applicable_fields,
                audit.unrouted_fields, audit.authored_root_frame_routes,
                audit.authored_data_list_routes,
                audit.perturbed_root_frame_matches,
                audit.perturbed_data_list_axis_matches,
                audit.loaded_spinner_routes,
                audit.fake_loaded_spinner_routes,
                loaded_spinner_hidden ? 1 : 0,
                audit.passed() ? 1 : 0,
                error.c_str());
    bf6_close(context);
    return result && resolve_stable && absent_axis_control_rejected &&
           loaded_spinner_hidden ? 0 : 1;
}
