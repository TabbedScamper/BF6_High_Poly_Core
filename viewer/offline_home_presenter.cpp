#include "offline_home_presenter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <utility>
#include <vector>

namespace offline_home {
namespace {

constexpr const char* kHomeScreen = "common/ui/home/screens/home_screen";
constexpr const char* kSectionTemplate =
    "common/ui/home/widgets/homesectioncell";
constexpr const char* kRowTemplate =
    "common/ui/home/widgets/gamecardrowcell";
constexpr const char* kPageTemplate =
    "common/ui/home/widgets/gamecardpagecell";
constexpr const char* kCardTemplate =
    "common/ui/home/widgets/gamecardcell";

std::string leaf(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool exact_template(const rime::Screen* screen, const char* partition)
{
    return screen && !screen->elements.empty() &&
           screen->partition == partition;
}

int unique_local_element(const rime::Screen& screen, const char* name,
                         int* matches = nullptr)
{
    int found = -1;
    int count = 0;
    for (size_t index = 0; index < screen.elements.size(); ++index) {
        const rime::Element& element = screen.elements[index];
        if (element.partition != screen.partition || element.name != name)
            continue;
        found = static_cast<int>(index);
        ++count;
    }
    if (matches) *matches = count;
    return count == 1 ? found : -1;
}

std::set<uint32_t> upstream_interface_fields(const rime::Screen& screen,
                                             int target_instance)
{
    std::set<uint32_t> result;
    for (const rime::Screen::InterfaceTextGraph& graph :
         screen.interface_text_graphs) {
        if (graph.partition != screen.partition || graph.scope != -1)
            continue;
        std::set<int32_t> interfaces(graph.interfaces.begin(),
                                     graph.interfaces.end());
        std::set<int32_t> visited;
        std::vector<int32_t> pending{target_instance};
        while (!pending.empty()) {
            const int32_t target = pending.back();
            pending.pop_back();
            if (!visited.insert(target).second) continue;
            for (const bf6_rime_connection& connection : graph.connections) {
                if (connection.target != target) continue;
                if (interfaces.count(connection.source))
                    result.insert(connection.source_field);
                else
                    pending.push_back(connection.source);
            }
        }
    }
    return result;
}

uint32_t shuffled(uint32_t value)
{
    return ((value << 8u) | (value >> 24u)) ^ 0xA5A5A5A5u;
}

const Collection* relation(const Snapshot& snapshot, const Record& record,
                           const char* name, PresentationAudit& audit)
{
    const Field* found = nullptr;
    int matches = 0;
    for (const Field& field : record.fields) {
        if (field.name != name) continue;
        found = &field;
        ++matches;
    }
    if (matches != 1 || !found ||
        found->value.kind != ReferenceKind::Collection) {
        if (matches > 1) ++audit.duplicate_relations;
        else ++audit.missing_relations;
        return nullptr;
    }
    const Collection* collection = snapshot.collection(found->value.instance);
    if (!collection) ++audit.missing_relations;
    return collection;
}

std::vector<const Record*> typed_records(const Snapshot& snapshot,
                                         const Collection& collection,
                                         const char* expected_leaf,
                                         PresentationAudit& audit)
{
    std::vector<const Record*> result;
    for (const Reference& item : collection.items) {
        /* Null is an authored collection value, not a fabricated row.  The
         * current PlayMenuDBD.Sections provider ends in one such optional
         * slot.  Preserve its position in the audit while emitting no cell;
         * every non-null value still has to be the exact expected DBD. */
        if (item.kind == ReferenceKind::Null) {
            ++audit.null_collection_items;
            continue;
        }
        if (item.kind != ReferenceKind::Record) {
            ++audit.missing_relations;
            continue;
        }
        const Record* record = snapshot.record(item.instance);
        if (!record || leaf(record->dbd_partition) != expected_leaf) {
            ++audit.missing_relations;
            continue;
        }
        result.push_back(record);
    }
    return result;
}

void bind_record_fields(rime::Screen& screen, const Snapshot& snapshot,
                        const Record& record, PresentationAudit& audit,
                        std::set<int>& visited)
{
    if (!visited.insert(record.instance).second) return;
    const rime_list::Record primitive = snapshot.primitive_record(record);
    for (const rime_list::Entry& entry : primitive.entries()) {
        ++audit.direct_primitive_fields;
        rime_list::Record one;
        if (!one.put(record.contract, entry.field, entry.value)) {
            ++audit.unsupported_fields;
            continue;
        }
        rime::Screen real_screen = screen;
        const rime_list::BindReport real = rime_list::bind_record(
            real_screen, screen.partition.c_str(), record.contract, one);
        rime::Screen control_screen = screen;
        const rime_list::BindReport control =
            rime_list::bind_record_shuffled_control(
                control_screen, screen.partition.c_str(), record.contract,
                one);
        audit.shuffled_targets += control.applied_targets;
        audit.ambiguous_fields += real.ambiguous;
        audit.unsupported_fields += real.unsupported_values;
        if (real.supplied == 1 && real.applied_fields == 1 &&
            real.applied_targets > 0 && real.unrouted == 0 &&
            real.ambiguous == 0 && real.unsupported_values == 0 &&
            control.applied_fields == 0 && control.applied_targets == 0 &&
            control.ambiguous == 0) {
            screen = std::move(real_screen);
            ++audit.bound_fields;
            audit.bound_targets += real.applied_targets;
        } else if (real.supplied == 1 && real.declared == 1 &&
                   real.applied_fields == 0 && real.applied_targets == 0 &&
                   real.unrouted == 1 && real.ambiguous == 0 &&
                   real.unsupported_values == 0 &&
                   control.applied_fields == 0 &&
                   control.applied_targets == 0 &&
                   control.ambiguous == 0) {
            /* The primitive is valid in its direct-read DBD, but this cell's
             * installed property graph declares no route for it.  That is an
             * inapplicable input, not a failed expected binding. */
            ++audit.non_applicable_fields;
        } else {
            ++audit.unrouted_fields;
        }
    }

    /* Compound DBD records (BaseInteractableData, details and tags) are
     * provider inputs to the same cell graph.  Walk exact references; do not
     * flatten their values into the parent contract. */
    for (const Field& field : record.fields) {
        if (field.value.kind == ReferenceKind::Record) {
            const Record* child = snapshot.record(field.value.instance);
            if (child) bind_record_fields(screen, snapshot, *child, audit,
                                          visited);
        } else if (field.value.kind == ReferenceKind::Collection) {
            const Collection* values = snapshot.collection(field.value.instance);
            if (!values) continue;
            for (const Reference& value : values->items) {
                if (value.kind != ReferenceKind::Record) continue;
                const Record* child = snapshot.record(value.instance);
                if (child) bind_record_fields(screen, snapshot, *child, audit,
                                              visited);
            }
        }
    }
}

void bind_record_graph(rime::Screen& screen, const Snapshot& snapshot,
                       const Record& record, PresentationAudit& audit)
{
    std::set<int> visited;
    bind_record_fields(screen, snapshot, record, audit, visited);
}

bool fixed_grid_root_frame(const rime::Screen& item, int& root)
{
    root = -1;
    const rime::Element* frame = nullptr;
    for (size_t index = 0; index < item.elements.size(); ++index) {
        const rime::Element& element = item.elements[index];
        if (element.parent >= 0) continue;
        if (!element.solved || element.kind != rime::Kind::LayerEntity)
            return false;
        if (!frame) {
            frame = &element;
            root = static_cast<int>(index);
            continue;
        }
        /* The two gamecardrowcell roots are sibling presentation planes, not
         * competing cell envelopes: both carry the exact same authored
         * solved frame.  A differing origin or extent is ambiguous and must
         * not be collapsed to whichever root happened to be enumerated
         * first. */
        if (std::fabs(element.x0 - frame->x0) > 0.0001f ||
            std::fabs(element.y0 - frame->y0) > 0.0001f ||
            std::fabs(element.x1 - frame->x1) > 0.0001f ||
            std::fabs(element.y1 - frame->y1) > 0.0001f)
            return false;
    }
    return root >= 0;
}

bool fixed_grid_layout(const rime::Element& grid,
                       const rime::Screen& item,
                       rime::UniformGridLayout& out)
{
    out = {};
    if (!grid.solved || item.elements.empty() ||
        grid.item_template != item.partition ||
        !(grid.grid_column_size > 0.f) ||
        !(grid.grid_row_size > 0.f) ||
        grid.grid_item_fit_content != 0 ||
        grid.grid_segment_distribution < 0 ||
        grid.grid_segment_distribution > 2 ||
        grid.grid_segment_count_mode < 0 ||
        grid.grid_segment_count_mode > 1 ||
        grid.grid_column_flow_direction < 0 ||
        grid.grid_column_flow_direction > 2 ||
        grid.grid_row_flow_direction < 0 ||
        grid.grid_row_flow_direction > 2)
        return false;
    int root = -1;
    if (!fixed_grid_root_frame(item, root)) return false;

    const float content_width =
        grid.x1 - grid.x0 - grid.pad_l - grid.pad_r;
    const float content_height =
        grid.y1 - grid.y0 - grid.pad_t - grid.pad_b;
    if (!(content_width > 0.f) || !(content_height > 0.f) ||
        grid.grid_column_spacing < 0.f || grid.grid_row_spacing < 0.f)
        return false;

    const bool vertical_static = grid.grid_orientation == 1 &&
        grid.grid_segment_count_mode == 0;
    if (vertical_static && grid.grid_segment_distribution != 0)
        return false;
    /* Vertical grids count their static rows. Their perpendicular run uses
     * the viewport extent; edge padding offsets paint but is not a second
     * segment-count inset (gamecardpagecell is the controlled 4-vs-3 case). */
    int columns = vertical_static
        ? static_cast<int>(std::floor(
              ((grid.x1 - grid.x0) + grid.grid_column_spacing) /
              (grid.grid_column_size + grid.grid_column_spacing)))
        : (grid.grid_segment_count_mode == 0
            ? grid.grid_static_segment_item_count
            : static_cast<int>(std::floor(
                  (content_width + grid.grid_column_spacing) /
                  (grid.grid_column_size + grid.grid_column_spacing))));
    if (columns <= 0) return false;
    float cell_width = grid.grid_column_size;
    float column_spacing = grid.grid_column_spacing;
    if (grid.grid_segment_distribution == 1)
        cell_width =
            (content_width - column_spacing * (columns - 1)) / columns;
    else if (grid.grid_segment_distribution == 2 && columns > 1)
        column_spacing =
            (content_width - cell_width * columns) / (columns - 1);
    if (!(cell_width > 0.f) || column_spacing < 0.f) return false;
    const int visible_rows = vertical_static
        ? grid.grid_static_segment_item_count
        : static_cast<int>(std::floor(
              (content_height + grid.grid_row_spacing) /
              (grid.grid_row_size + grid.grid_row_spacing)));
    if (visible_rows <= 0) return false;

    out.columns = columns;
    out.visible_rows = visible_rows;
    out.x0 = grid.x0 + grid.pad_l;
    out.y0 = grid.y0 + grid.pad_t;
    out.cell_w = cell_width;
    out.cell_h = grid.grid_row_size;
    out.column_spacing = column_spacing;
    out.row_spacing = grid.grid_row_spacing;
    out.normalization_root = root;
    return true;
}

bool routed_grid(const rime::Screen& owner, int grid_index,
                 const rime::Screen& item, const char* item_dbd_leaf,
                 rime::UniformGridLayout& layout,
                 PresentationAudit& audit)
{
    if (grid_index < 0 || grid_index >= static_cast<int>(owner.elements.size()))
        return false;
    rime::Element grid = owner.elements[static_cast<size_t>(grid_index)];
    const bool authored = grid.item_template == item.partition ||
        std::find(grid.item_templates.begin(), grid.item_templates.end(),
                  item.partition) != grid.item_templates.end();
    if (!authored) {
        /* The runtime list selects its cell class from the DBD record family.
         * This selection is admitted only for the three exact families in the
         * direct-read Home hierarchy.  A fake family is scored beside it. */
        const auto provider_route = [](const std::string& dbd,
                                       const std::string& partition) {
            return (dbd == "uniformrowdbd" &&
                    partition == kRowTemplate) ||
                   (dbd == "menupagedbd" &&
                    partition == kPageTemplate) ||
                   (dbd == "playinteractabledbd" &&
                    partition == kCardTemplate);
        };
        const bool exact_route = provider_route(item_dbd_leaf,
                                                item.partition);
        audit.fake_template_routes +=
            provider_route("__control_missing", item.partition) ? 1 : 0;
        if (!exact_route) return false;
        grid.item_template = item.partition;
        ++audit.provider_template_routes;
    }
    if (rime::uniform_grid_layout(grid, item, layout)) return true;
    if (!fixed_grid_layout(grid, item, layout)) {
        ++audit.ambiguous_root_frames;
        return false;
    }
    ++audit.authored_root_frame_routes;
    rime::Screen control = item;
    int first_root = -1;
    int second_root = -1;
    for (size_t index = 0; index < control.elements.size(); ++index) {
        if (control.elements[index].parent >= 0) continue;
        if (first_root < 0) first_root = static_cast<int>(index);
        else { second_root = static_cast<int>(index); break; }
    }
    if (second_root >= 0) {
        control.elements[static_cast<size_t>(second_root)].x1 += 1.f;
        rime::UniformGridLayout rejected;
        audit.perturbed_root_frame_matches +=
            fixed_grid_layout(grid, control, rejected) ? 1 : 0;
    }
    return true;
}

bool append_child(const rime::Screen& child,
                  const rime::UniformGridLayout& layout,
                  float offset_x, float offset_y, int owner_index,
                  rime::Screen& host)
{
    if (layout.normalization_root < 0 ||
        layout.normalization_root >= static_cast<int>(child.elements.size()))
        return false;
    const rime::Element& normalization =
        child.elements[static_cast<size_t>(layout.normalization_root)];
    std::vector<rime::Element> moved;
    if (!rime::append_grid_item(child, layout,
                                offset_x + normalization.x0,
                                offset_y + normalization.y0, 0, moved))
        return false;
    const int base = static_cast<int>(host.elements.size());
    const float owner_x = owner_index >= 0 &&
        owner_index < static_cast<int>(host.elements.size())
        ? host.elements[static_cast<size_t>(owner_index)].x0 +
          host.elements[static_cast<size_t>(owner_index)].pad_l : 0.f;
    const float owner_y = owner_index >= 0 &&
        owner_index < static_cast<int>(host.elements.size())
        ? host.elements[static_cast<size_t>(owner_index)].y0 +
          host.elements[static_cast<size_t>(owner_index)].pad_t : 0.f;
    const int depth_base = owner_index >= 0 &&
        owner_index < static_cast<int>(host.elements.size())
        ? host.elements[static_cast<size_t>(owner_index)].depth + 1 : 0;
    for (rime::Element& element : moved) {
        if (element.parent >= 0) {
            element.parent += base;
        } else {
            /* Expanded cell LayerEntity roots ship without axes because they
             * were roots in their own authored canvas.  Reparenting them
             * without establishing a local frame makes the next solve()
             * stretch every root to its owner list (a 352px card became the
             * 1448px page).  Freeze only this newly-created parent edge to
             * the already-solved, grid-derived cell box. Descendants retain
             * their authored anchors and are solved normally inside it. */
            if (!element.solved) return false;
            const float width = element.x1 - element.x0;
            const float height = element.y1 - element.y0;
            if (!(width >= 0.f) || !(height >= 0.f)) return false;
            element.h = rime::Axis{0.f, 0.f, element.x0 - owner_x, 0.f,
                                   0.f, 0.f, true};
            element.v = rime::Axis{0.f, 0.f, element.y0 - owner_y, 0.f,
                                   0.f, 0.f, true};
            element.width = width;
            element.height = height;
            element.fit_w = false;
            element.fit_h = false;
            element.parent = owner_index;
        }
        element.depth += depth_base;
        host.elements.push_back(std::move(element));
    }
    return true;
}

bool prepare_cell(rime::Screen& screen,
                  const rime::UniformGridLayout& parent_layout,
                  float canvas_width, float canvas_height)
{
    if (parent_layout.normalization_root < 0 ||
        parent_layout.normalization_root >=
            static_cast<int>(screen.elements.size()))
        return false;
    /* The normalization root is a coordinate reference, not the painted
     * content envelope.  Solving against the authored grid's cell canvas
     * naturally resizes stretch-anchored roots while preserving the card's
     * authored 248px content height and margins. */
    rime::solve(screen, canvas_width, canvas_height);
    return screen.elements[
        static_cast<size_t>(parent_layout.normalization_root)].solved;
}

bool build_page(const Snapshot& snapshot, const Templates& templates,
                const std::vector<const Record*>& cards, size_t first,
                size_t count,
                const rime::UniformGridLayout& parent_layout,
                rime::Screen& page, PresentationAudit& audit)
{
    page = *templates.page;
    if (!prepare_cell(page, parent_layout, parent_layout.cell_w,
                      parent_layout.cell_h)) {
        std::fprintf(stderr, "offline Home page: page cell solve failed\n");
        ++audit.invalid_layouts;
        return false;
    }
    int matches = 0;
    const int grid_index = unique_local_element(
        page, "DiceUIUniformGridList", &matches);
    if (matches == 0) ++audit.missing_lists;
    else if (matches != 1) ++audit.ambiguous_lists;
    if (grid_index < 0) return false;

    rime::Screen card_measure = *templates.card;
    rime::solve(card_measure, parent_layout.cell_w, parent_layout.cell_h);
    rime::UniformGridLayout card_layout;
    if (!routed_grid(page, grid_index, card_measure,
                     "playinteractabledbd", card_layout, audit)) {
        std::fprintf(stderr, "offline Home page: card grid route failed\n");
        ++audit.invalid_layouts;
        return false;
    }
    const size_t capacity = static_cast<size_t>(card_layout.columns) *
                            static_cast<size_t>(card_layout.visible_rows);
    if (capacity == 0 || count > capacity) {
        std::fprintf(stderr,
            "offline Home page: capacity=%zu requested=%zu columns=%d rows=%d "
            "orientation=%d grid=%.1f..%.1f pad=%.1f/%.1f cell=%.1f gap=%.1f\n",
            capacity, count, card_layout.columns,
            card_layout.visible_rows,
            page.elements[static_cast<size_t>(grid_index)].grid_orientation,
            page.elements[static_cast<size_t>(grid_index)].x0,
            page.elements[static_cast<size_t>(grid_index)].x1,
            page.elements[static_cast<size_t>(grid_index)].pad_l,
            page.elements[static_cast<size_t>(grid_index)].pad_r,
            card_layout.cell_w, card_layout.column_spacing);
        ++audit.invalid_layouts;
        return false;
    }
    for (size_t index = 0; index < count; ++index) {
        rime::Screen card = *templates.card;
        bind_record_graph(card, snapshot, *cards[first + index], audit);
        if (!prepare_cell(card, card_layout, card_layout.cell_w,
                          card_layout.cell_h)) {
            std::fprintf(stderr,
                "offline Home page: card %zu cell solve failed\n", index);
            ++audit.invalid_layouts;
            return false;
        }
        const int column = static_cast<int>(index) % card_layout.columns;
        const int row = static_cast<int>(index) / card_layout.columns;
        const float x = card_layout.x0 + column *
            (card_layout.cell_w + card_layout.column_spacing);
        const float y = card_layout.y0 + row *
            (card_layout.cell_h + card_layout.row_spacing);
        if (!append_child(card, card_layout, x, y, grid_index, page)) {
            std::fprintf(stderr,
                "offline Home page: card %zu append failed at %.1f,%.1f\n",
                index, x, y);
            ++audit.invalid_layouts;
            return false;
        }
        ++audit.visible_cards;
    }
    return true;
}

bool build_row(const Snapshot& snapshot, const Templates& templates,
               const Record& row_record,
               const rime::UniformGridLayout& parent_layout,
               rime::Screen& row, PresentationAudit& audit)
{
    const Collection* card_values = relation(
        snapshot, row_record, "Interactables", audit);
    if (!card_values) return false;
    const std::vector<const Record*> cards = typed_records(
        snapshot, *card_values, "playinteractabledbd", audit);
    if (cards.empty()) { ++audit.missing_relations; return false; }
    audit.cards += static_cast<int>(cards.size());

    row = *templates.row;
    bind_record_graph(row, snapshot, row_record, audit);
    if (!prepare_cell(row, parent_layout, parent_layout.cell_w,
                      parent_layout.cell_h)) {
        ++audit.invalid_layouts;
        return false;
    }
    int matches = 0;
    const int grid_index = unique_local_element(
        row, "DiceUIUniformGridList", &matches);
    if (matches == 0) ++audit.missing_lists;
    else if (matches != 1) ++audit.ambiguous_lists;
    if (grid_index < 0) return false;

    rime::Screen page_measure = *templates.page;
    rime::solve(page_measure, parent_layout.cell_w, parent_layout.cell_h);
    rime::UniformGridLayout page_layout;
    if (!routed_grid(row, grid_index, page_measure, "menupagedbd",
                     page_layout, audit)) {
        ++audit.invalid_layouts;
        return false;
    }

    /* The page size is not a constant.  It is the exact current page cell's
     * own visible card-grid capacity. */
    int page_grid_matches = 0;
    const int page_grid_index = unique_local_element(
        page_measure, "DiceUIUniformGridList", &page_grid_matches);
    if (page_grid_matches != 1) {
        if (page_grid_matches == 0) ++audit.missing_lists;
        else ++audit.ambiguous_lists;
        return false;
    }
    rime::Screen card_measure = *templates.card;
    rime::solve(card_measure, parent_layout.cell_w, parent_layout.cell_h);
    rime::UniformGridLayout card_layout;
    if (!routed_grid(page_measure, page_grid_index, card_measure,
                     "playinteractabledbd", card_layout, audit)) {
        ++audit.invalid_layouts;
        return false;
    }
    const size_t cards_per_page =
        static_cast<size_t>(card_layout.columns) *
        static_cast<size_t>(card_layout.visible_rows);
    if (cards_per_page == 0) { ++audit.invalid_layouts; return false; }
    const size_t page_count =
        (cards.size() + cards_per_page - 1) / cards_per_page;
    audit.pages += static_cast<int>(page_count);
    const size_t visible_page_capacity =
        static_cast<size_t>(page_layout.columns) *
        static_cast<size_t>(page_layout.visible_rows);
    const size_t visible_pages = (std::min)(page_count, visible_page_capacity);
    audit.truncated_items += static_cast<int>(page_count - visible_pages);
    for (size_t index = 0; index < visible_pages; ++index) {
        const size_t first = index * cards_per_page;
        const size_t count = (std::min)(cards_per_page,
                                        cards.size() - first);
        rime::Screen page;
        if (!build_page(snapshot, templates, cards, first, count,
                        page_layout, page, audit))
            return false;
        const int column = static_cast<int>(index) % page_layout.columns;
        const int page_row = static_cast<int>(index) / page_layout.columns;
        const float x = page_layout.x0 + column *
            (page_layout.cell_w + page_layout.column_spacing);
        const float y = page_layout.y0 + page_row *
            (page_layout.cell_h + page_layout.row_spacing);
        if (!append_child(page, page_layout, x, y, grid_index, row)) {
            ++audit.invalid_layouts;
            return false;
        }
        ++audit.visible_pages;
    }
    return true;
}

bool build_section(const Snapshot& snapshot, const Templates& templates,
                   const Record& section_record,
                   float canvas_width, float canvas_height,
                   rime::Screen& section, PresentationAudit& audit)
{
    const Collection* row_values = relation(
        snapshot, section_record, "Rows", audit);
    if (!row_values) return false;
    const std::vector<const Record*> rows = typed_records(
        snapshot, *row_values, "uniformrowdbd", audit);
    if (rows.empty()) { ++audit.missing_relations; return false; }
    audit.rows += static_cast<int>(rows.size());

    section = *templates.section;
    bind_record_graph(section, snapshot, section_record, audit);
    rime::solve(section, canvas_width, canvas_height);
    int matches = 0;
    const int grid_index = unique_local_element(
        section, "DiceUIUniformGridList", &matches);
    if (matches == 0) ++audit.missing_lists;
    else if (matches != 1) ++audit.ambiguous_lists;
    if (grid_index < 0) return false;

    rime::Screen row_measure = *templates.row;
    rime::solve(row_measure, canvas_width, canvas_height);
    rime::UniformGridLayout row_layout;
    if (!routed_grid(section, grid_index, row_measure, "uniformrowdbd",
                     row_layout, audit)) {
        ++audit.invalid_layouts;
        return false;
    }
    const size_t capacity = static_cast<size_t>(row_layout.columns) *
                            static_cast<size_t>(row_layout.visible_rows);
    const size_t visible_rows = (std::min)(rows.size(), capacity);
    audit.truncated_items += static_cast<int>(rows.size() - visible_rows);
    for (size_t index = 0; index < visible_rows; ++index) {
        rime::Screen row;
        if (!build_row(snapshot, templates, *rows[index], row_layout,
                       row, audit))
            return false;
        const int column = static_cast<int>(index) % row_layout.columns;
        const int row_number = static_cast<int>(index) / row_layout.columns;
        const float x = row_layout.x0 + column *
            (row_layout.cell_w + row_layout.column_spacing);
        const float y = row_layout.y0 + row_number *
            (row_layout.cell_h + row_layout.row_spacing);
        if (!append_child(row, row_layout, x, y, grid_index, section)) {
            ++audit.invalid_layouts;
            return false;
        }
        ++audit.visible_rows;
    }
    return true;
}

bool data_list_axis(const rime::Element& list,
                    const rime::Element& item_envelope,
                    bool& horizontal)
{
    if (!list.solved || !item_envelope.solved) return false;
    /* The current Home contract proves the repeated section against the
     * DataList viewport: 304 equals the list's solved 304 cross extent even
     * though the list separately authors a 20px bottom pad.  Treating that
     * stack-style padding as part of axis discovery changes the raw cross
     * extent to 284 and rejects the authored section. */
    const float list_width = list.x1 - list.x0;
    const float list_height = list.y1 - list.y0;
    const float item_width = item_envelope.x1 - item_envelope.x0;
    const float item_height = item_envelope.y1 - item_envelope.y0;
    if (!(list_width > 0.f) || !(list_height > 0.f) ||
        !(item_width > 0.f) || !(item_height > 0.f))
        return false;
    const bool width_is_cross = std::fabs(item_width - list_width) <= 0.01f;
    const bool height_is_cross = std::fabs(item_height - list_height) <= 0.01f;
    /* A DataList FlowDirection is ordering, never the missing axis enum.  The
     * shipped Home data makes the axis unambiguous instead: its 1508x304
     * section envelope matches exactly one 1592x304 list cross-axis. */
    if (width_is_cross == height_is_cross) return false;
    horizontal = height_is_cross;
    return true;
}

bool append_sections(rime::Screen& home, int main_list,
                     std::vector<rime::Screen>& sections,
                     PresentationAudit& audit)
{
    if (main_list < 0 || main_list >= static_cast<int>(home.elements.size()))
        return false;
    const rime::Element& list = home.elements[static_cast<size_t>(main_list)];
    const bool authored_section_route =
        list.item_template == kSectionTemplate ||
        std::find(list.item_templates.begin(), list.item_templates.end(),
                  kSectionTemplate) != list.item_templates.end();
    if (!list.solved ||
        list.type_name != "DiceUIDataListElementData" ||
        list.data_list_size_distribution != 0 ||
        list.data_list_flow_direction != 0 ||
        list.data_list_space_distribution != 0 ||
        list.data_list_preserve_fit_content != 1 ||
        list.item_spacing < 0.f || !authored_section_route || sections.empty())
        return false;
    std::vector<int> envelopes;
    bool horizontal = false;
    float item_width = 0.f;
    float item_height = 0.f;
    for (const rime::Screen& section : sections) {
        int matches = 0;
        const int envelope = unique_local_element(
            section, "DiceUIUniformGridList", &matches);
        if (matches != 1 || envelope < 0)
            return false;
        const rime::Element& item =
            section.elements[static_cast<size_t>(envelope)];
        bool section_horizontal = false;
        if (!data_list_axis(list, item, section_horizontal) ||
            (!envelopes.empty() && section_horizontal != horizontal))
            return false;
        if (envelopes.empty()) {
            horizontal = section_horizontal;
            item_width = item.x1 - item.x0;
            item_height = item.y1 - item.y0;
        } else if (std::fabs((item.x1 - item.x0) - item_width) > 0.01f ||
                   std::fabs((item.y1 - item.y0) - item_height) > 0.01f) {
            return false;
        }
        envelopes.push_back(envelope);
    }

    rime::Element perturbed =
        sections.front().elements[static_cast<size_t>(envelopes.front())];
    if (horizontal) perturbed.y1 += 1.f;
    else perturbed.x1 += 1.f;
    bool control_axis = false;
    audit.perturbed_data_list_axis_matches +=
        data_list_axis(list, perturbed, control_axis) ? 1 : 0;
    ++audit.authored_data_list_routes;

    float cursor = horizontal ? list.x0 + list.pad_l
                              : list.y0 + list.pad_t;
    const float cross = horizontal ? list.y0 + list.pad_t
                                   : list.x0 + list.pad_l;
    /* append_child grows home.elements and may reallocate it.  `list` points
     * into that vector, so snapshot the last value consumed by the loop
     * before the first append.  Reading list.item_spacing afterwards caused
     * the intermittent Release-only startup access violation. */
    const float item_spacing = list.item_spacing;
    for (size_t index = 0; index < sections.size(); ++index) {
        rime::Screen& section = sections[index];
        rime::UniformGridLayout translation;
        translation.normalization_root = envelopes[index];
        const float x = horizontal ? cursor : cross;
        const float y = horizontal ? cross : cursor;
        if (!append_child(section, translation, x, y, main_list, home))
            return false;
        ++audit.visible_sections;
        cursor += (horizontal ? item_width : item_height) + item_spacing;
    }
    return true;
}

bool apply_loaded_spinner_state(rime::Screen& home, PresentationAudit& audit)
{
    const auto route = [&](const char* name) -> int {
        int matches = 0;
        const int index = unique_local_element(home, name, &matches);
        if (matches != 1 || index < 0) return 0;
        const rime::Element& element = home.elements[(size_t)index];
        if (element.kind != rime::Kind::Flipbook ||
            element.image_asset !=
                "common/ui/assets/images/spinners/spinner01_4k")
            return 0;
        const std::set<uint32_t> inputs = upstream_interface_fields(
            home, element.instance);
        return inputs.count(rime::property_hash("IsLoading")) == 1 ? 1 : 0;
    };
    audit.fake_loaded_spinner_routes = route("__control_PAC");
    audit.loaded_spinner_routes = route("PAC");
    if (audit.loaded_spinner_routes != 1 ||
        audit.fake_loaded_spinner_routes != 0)
        return false;

    /* A completed, control-passing Snapshot is the Home provider's terminal
     * loaded state. The installed root graph routes IsLoading through its OR
     * node to this exact PAC.Visible target. One other producer is a timeline
     * output that the static graph evaluator intentionally leaves unresolved,
     * so commit the host terminal on the graph-proven target rather than
     * leaking the asset's editor-visible value after content has arrived. */
    int matches = 0;
    const int pac = unique_local_element(home, "PAC", &matches);
    if (matches != 1 || pac < 0) return false;
    home.elements[(size_t)pac].visible = false;
    home.elements[(size_t)pac].runtime_visibility_unresolved = false;
    return true;
}

} // namespace

bool materialize_home(const Snapshot& snapshot, const Templates& templates,
                      const rime::Screen& authored_home,
                      float canvas_width, float canvas_height,
                      rime::Screen& out, PresentationAudit& audit,
                      std::string& error)
{
    audit = {};
    error.clear();
    if (!snapshot.audit().passed()) {
        error = "offline Home provider snapshot did not pass its read controls";
        return false;
    }
    if (authored_home.partition != kHomeScreen ||
        authored_home.elements.empty()) {
        error = "Home presenter requires the exact installed home_screen";
        return false;
    }
    if (!(canvas_width > 0.f) || !(canvas_height > 0.f)) {
        error = "Home presenter requires a positive authored canvas";
        return false;
    }
    const bool templates_ok =
        exact_template(templates.section, kSectionTemplate) &&
        exact_template(templates.row, kRowTemplate) &&
        exact_template(templates.page, kPageTemplate) &&
        exact_template(templates.card, kCardTemplate);
    if (!templates_ok) {
        audit.missing_templates =
            (!exact_template(templates.section, kSectionTemplate) ? 1 : 0) +
            (!exact_template(templates.row, kRowTemplate) ? 1 : 0) +
            (!exact_template(templates.page, kPageTemplate) ? 1 : 0) +
            (!exact_template(templates.card, kCardTemplate) ? 1 : 0);
        error = "one or more exact Home cell templates are unavailable";
        return false;
    }

    rime::Screen working = authored_home;
    rime::solve(working, canvas_width, canvas_height);
    int main_matches = 0;
    const int main_list = unique_local_element(working, "MainList",
                                               &main_matches);
    audit.main_lists = main_matches;
    audit.fake_root_matches =
        unique_local_element(working, "__control_missing") >= 0 ? 1 : 0;
    if (main_matches == 0) ++audit.missing_lists;
    else if (main_matches != 1) ++audit.ambiguous_lists;
    if (main_list < 0) {
        error = "Home MainList is missing or ambiguous";
        return false;
    }

    const int main_instance =
        working.elements[static_cast<size_t>(main_list)].instance;
    const std::set<uint32_t> main_provider_fields =
        upstream_interface_fields(working, main_instance);
    audit.main_list_provider_fields =
        static_cast<int>(main_provider_fields.size());

    const Record* play_menu = nullptr;
    for (const PublicValue& value : snapshot.public_values()) {
        if (value.value.kind != ReferenceKind::Record) continue;
        const Record* candidate = snapshot.record(value.value.instance);
        if (!candidate || leaf(candidate->dbd_partition) != "playmenudbd")
            continue;
        ++audit.public_play_menu_candidates;
        /* A matching leaf is insufficient: homescreenmockdata publishes two
         * PlayMenuDBD-shaped records.  Only the public pin whose authored
         * InterfaceDescriptor TypeRef imports this exact DBD partition is a
         * typed root.  The other public record is retained as a negative
         * candidate instead of being silently accepted by shape.  Do not
         * equate the mock's public-field hash with homescreenlistprovider's
         * input hash: the shipped partitions expose different interface
         * contracts, while the DBD TypeRef is the exact cross-partition type
         * evidence. */
        if (value.declared_partition != candidate->dbd_partition) {
            ++audit.rejected_play_menu_declarations;
            continue;
        }
        play_menu = candidate;
        ++audit.public_play_menus;
    }
    for (uint32_t field : main_provider_fields)
        for (const PublicValue& value : snapshot.public_values())
            if (value.field == shuffled(field))
                ++audit.fake_provider_field_matches;
    for (const PublicValue& value : snapshot.public_values()) {
        if (value.value.kind != ReferenceKind::Record) continue;
        const Record* candidate = snapshot.record(value.value.instance);
        if (candidate &&
            value.declared_partition ==
                (candidate->dbd_partition + "/__control_missing"))
            ++audit.fake_declared_partition_matches;
    }
    for (const PublicValue& value : snapshot.public_values()) {
        if (value.value.kind != ReferenceKind::Record) continue;
        const Record* candidate = snapshot.record(value.value.instance);
        if (candidate && leaf(candidate->dbd_partition) ==
                         "__control_missing")
            ++audit.fake_root_matches;
    }
    if (audit.public_play_menus != 1 || !play_menu) {
        error = "offline Home public PlayMenuDBD root is missing or ambiguous";
        return false;
    }
    audit.fake_relation_matches =
        snapshot.field(*play_menu, "__control_missing") ? 1 : 0;
    const Collection* section_values = relation(
        snapshot, *play_menu, "Sections", audit);
    if (!section_values) {
        error = "PlayMenuDBD.Sections is unavailable";
        return false;
    }
    const std::vector<const Record*> section_records = typed_records(
        snapshot, *section_values, "menusectiondbd", audit);
    audit.sections = static_cast<int>(section_records.size());
    if (section_records.empty()) {
        error = "PlayMenuDBD.Sections contains no MenuSectionDBD rows";
        return false;
    }

    std::vector<rime::Screen> sections;
    sections.reserve(section_records.size());
    for (const Record* section_record : section_records) {
        rime::Screen section;
        if (!build_section(snapshot, templates, *section_record,
                           canvas_width, canvas_height, section, audit)) {
            error = "Home section/row/page/card hierarchy failed closed";
            return false;
        }
        sections.push_back(std::move(section));
    }
    if (!append_sections(working, main_list, sections, audit)) {
        ++audit.invalid_layouts;
        error = "Home MainList layout is unsupported or invalid";
        return false;
    }
    if (!apply_loaded_spinner_state(working, audit)) {
        error = "Home loaded-state spinner route is missing or ambiguous";
        return false;
    }
    audit.materialized_elements =
        static_cast<int>(working.elements.size() -
                         authored_home.elements.size());
    if (!audit.passed()) {
        error = "Home presenter controls or structural audit did not close";
        return false;
    }
    out = std::move(working);
    return true;
}

} // namespace offline_home
