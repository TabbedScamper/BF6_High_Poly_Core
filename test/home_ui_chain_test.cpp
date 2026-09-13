#include "bf6_core.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Probe {
    const char* partition;
    const char* expected_list;
};

bool probe(bf6_ctx* ctx, const Probe& value)
{
    bf6_rime_tree_stats stats{};
    const int count = bf6_rime_tree(ctx, value.partition, 6, nullptr, 0,
                                    &stats);
    if (count <= 0) {
        std::printf("partition\t%s\tread=0\n", value.partition);
        return false;
    }
    std::vector<bf6_rime_node> rows(static_cast<size_t>(count));
    if (bf6_rime_tree(ctx, value.partition, 6, rows.data(), count, &stats) !=
        count) {
        std::printf("partition\t%s\tfill=0\n", value.partition);
        return false;
    }

    int exact = 0;
    int xp_root = -1;
    int mark_seen_root = -1;
    if (std::strcmp(value.partition,
                    "common/ui/home/screens/home_screen") == 0)
        for (size_t index = 0; index < rows.size(); ++index)
            if (std::strcmp(rows[index].name,
                            "XPModifiers_EntryWidget") == 0)
            {
                xp_root = static_cast<int>(index);
            }
            else if (std::strcmp(rows[index].name,
                                 "MarkAllSeenButton") == 0 &&
                     std::strcmp(rows[index].partition,
                                 value.partition) == 0)
                mark_seen_root = static_cast<int>(index);
    for (size_t row_index = 0; row_index < rows.size(); ++row_index) {
        const bf6_rime_node& row = rows[row_index];
        if (row.kind == BF6_RIME_WIDGET_REFERENCE) {
            std::printf("widget-ref\troot=%s\towner=%s\tname=%s\t"
                        "reference=%s\tinstance=%d\tparent=%d\n",
                        value.partition, row.partition, row.name,
                        row.reference, row.instance, row.parent);
        }
        if (std::strstr(row.type_name, "List") || row.item_template_count > 0 ||
            row.item_template[0]) {
            std::printf("expanded-list\troot=%s\towner=%s\tname=%s\t"
                        "type=%s\titem=%s\tcandidates=%d\n",
                        value.partition, row.partition, row.name,
                        row.type_name, row.item_template,
                        row.item_template_count);
        }
        if (std::strcmp(value.partition,
                        "common/ui/home/screens/home_screen") == 0 &&
            (std::strstr(row.name, "Event") ||
             std::strstr(row.name, "Beat") ||
             std::strstr(row.name, "Logo") ||
             std::strstr(row.name, "Boost") ||
             std::strstr(row.name, "Timer") ||
             std::strstr(row.name, "XP") ||
             std::strstr(row.reference, "boost") ||
             std::strstr(row.reference, "event"))) {
            std::printf("home-widget\tindex=%zu\tinstance=%d\tparent=%d\tdepth=%d\t"
                        "kind=%d\tvisible=%d\talpha=%.3f\t"
                        "size=%.1fx%.1f\tstack=%d/%d/%d\t"
                        "h=%.2f/%.2f,%.1f/%.1f,%.2f\t"
                        "v=%.2f/%.2f,%.1f/%.1f,%.2f\t"
                        "texture=%d/%d/%d/%d/%d/%d\t"
                        "name=%s\treference=%s\t"
                        "image=%s\ttext=%s\towner=%s\n",
                        row_index, row.instance, row.parent, row.depth, row.kind,
                        row.visible, row.alpha, row.width, row.height,
                        row.stack_orientation, row.stack_size_distribution,
                        row.stack_space_distribution,
                        row.h.anchor_start, row.h.anchor_end,
                        row.h.offset_start, row.h.offset_end, row.h.pivot,
                        row.v.anchor_start, row.v.anchor_end,
                        row.v.offset_start, row.v.offset_end, row.v.pivot,
                        row.image_resize_mode, row.image_h_align,
                        row.image_v_align, row.image_address_v,
                        row.image_address_u, row.image_clip_to_bounds,
                        row.name, row.reference, row.image_asset,
                        row.text_raw[0] ? row.text_raw : row.text_string_id,
                        row.partition);
        }
        if (std::strcmp(value.partition,
                        "common/ui/home/screens/home_screen") == 0 &&
            (std::strstr(row.name, "Gradient") ||
             std::strstr(row.name, "Bottom part") ||
             std::strstr(row.image_asset, "scrimgradient") ||
             std::strstr(row.reference, "readabilityfade")))
            std::printf("home-fade\tindex=%zu\tparent=%d\tdepth=%d\t"
                        "kind=%d\tvisible=%d\talpha=%.3f\t"
                        "size=%.1fx%.1f\tname=%s\treference=%s\timage=%s\n",
                        row_index, row.parent, row.depth, row.kind,
                        row.visible, row.alpha, row.width, row.height,
                        row.name, row.reference, row.image_asset);
        bool xp_descendant = static_cast<int>(row_index) == xp_root;
        int xp_parent = row.parent;
        for (size_t guard = 0;
             !xp_descendant && xp_parent >= 0 &&
             xp_parent < static_cast<int>(rows.size()) &&
             guard < rows.size(); ++guard)
        {
            xp_descendant = xp_parent == xp_root;
            xp_parent = rows[static_cast<size_t>(xp_parent)].parent;
        }
        if (xp_descendant)
            std::printf("xp-subtree\tindex=%zu\tparent=%d\tdepth=%d\t"
                        "kind=%d\tvisible=%d\talpha=%.3f\t"
                        "size=%.1fx%.1f\tname=%s\treference=%s\t"
                        "image=%s\ttext=%s\towner=%s\n",
                        row_index, row.parent, row.depth, row.kind,
                        row.visible, row.alpha, row.width, row.height,
                        row.name, row.reference, row.image_asset,
                        row.text_raw[0] ? row.text_raw : row.text_string_id,
                        row.partition);
        if (std::strcmp(value.partition,
                "common/ui/home/widgets/home_sidebarsection_gamemodecell") ==
            0)
            std::printf("sidebar-cell\tindex=%zu\tparent=%d\tdepth=%d\t"
                        "kind=%d\tvisible=%d\talpha=%.3f\t"
                        "size=%.1fx%.1f\tname=%s\treference=%s\ttext=%s\n",
                        row_index, row.parent, row.depth, row.kind,
                        row.visible, row.alpha, row.width, row.height,
                        row.name, row.reference,
                        row.text_raw[0] ? row.text_raw : row.text_string_id);
        bool mark_seen_descendant = static_cast<int>(row_index) ==
                                    mark_seen_root;
        int mark_seen_parent = row.parent;
        for (size_t guard = 0;
             !mark_seen_descendant && mark_seen_parent >= 0 &&
             mark_seen_parent < static_cast<int>(rows.size()) &&
             guard < rows.size(); ++guard)
        {
            mark_seen_descendant = mark_seen_parent == mark_seen_root;
            mark_seen_parent = rows[static_cast<size_t>(mark_seen_parent)].parent;
        }
        if (mark_seen_descendant)
            std::printf("mark-seen-subtree\tindex=%zu\tparent=%d\tdepth=%d\t"
                        "kind=%d\tvisible=%d\talpha=%.3f\t"
                        "size=%.1fx%.1f\tname=%s\treference=%s\t"
                        "image=%s\ttext=%s\towner=%s\n",
                        row_index, row.parent, row.depth, row.kind,
                        row.visible, row.alpha, row.width, row.height,
                        row.name, row.reference, row.image_asset,
                        row.text_raw[0] ? row.text_raw : row.text_string_id,
                        row.partition);
        if (std::strcmp(row.partition, value.partition) != 0 ||
            std::strcmp(row.name, value.expected_list) != 0)
            continue;
        ++exact;
        std::printf(
            "list\t%s\t%s\ttype=%s\tinstance=%d\tparent=%d\t"
            "item=%s\tcandidates=%d\tspacing=%.3f\tgrid=%.3fx%.3f\t"
            "gap=%.3f,%.3f\tstatic=%d\n",
            value.partition, row.name, row.type_name, row.instance, row.parent,
            row.item_template, row.item_template_count, row.item_spacing,
            row.grid_column_size, row.grid_row_size,
            row.grid_column_spacing, row.grid_row_spacing,
            row.grid_static_segment_item_count);
        for (int index = 0; index < row.item_template_count && index < 8;
             ++index)
            std::printf("candidate\t%s\t%s\t%d\t%s\n", value.partition,
                        row.name, index, row.item_templates[index]);
    }
    std::printf("control\t%s\texact-list=%d\tfake-list=0\n",
                value.partition, exact);
    return exact == 1;
}

bool dump_xp_reminder_cell(bf6_ctx* ctx)
{
    constexpr const char* partition =
        "common/ui/xpmodifiers/widgets/xpmodifiers_remindericoncellwidget";
    bf6_rime_tree_stats stats{};
    const int count = bf6_rime_tree(ctx, partition, 6, nullptr, 0, &stats);
    if (count <= 0) {
        std::printf("xp-cell\tread=0\n");
        return false;
    }
    std::vector<bf6_rime_node> rows(static_cast<size_t>(count));
    if (bf6_rime_tree(ctx, partition, 6, rows.data(), count, &stats) != count)
        return false;
    int category_icon = 0;
    int duration_labels = 0;
    for (size_t index = 0; index < rows.size(); ++index) {
        const bf6_rime_node& row = rows[index];
        if (std::strcmp(row.name, "Booster Category Icon") == 0 &&
            std::strcmp(row.image_asset,
                "common/ui/assets/images/consumables/boosters/"
                "t_ui_xpboost_sm_icn") == 0)
            ++category_icon;
        if (std::strcmp(row.name, "Days") == 0 ||
            std::strcmp(row.name, "Hours") == 0 ||
            std::strcmp(row.name, "Minutes") == 0)
            ++duration_labels;
        std::printf("xp-cell\tindex=%zu\tparent=%d\tdepth=%d\tkind=%d\t"
                    "visible=%d\talpha=%.3f\tsize=%.1fx%.1f\tname=%s\t"
                    "reference=%s\timage=%s\ttext=%s\towner=%s\n",
                    index, row.parent, row.depth, row.kind, row.visible,
                    row.alpha, row.width, row.height, row.name, row.reference,
                    row.image_asset,
                    row.text_raw[0] ? row.text_raw : row.text_string_id,
                    row.partition);
    }
    std::printf("xp-cell-control\ticon=%d\tduration-labels=%d\n",
                category_icon, duration_labels);
    return category_icon == 1 && duration_labels == 3;
}

bool dump_xp_booster_contract(bf6_ctx* ctx)
{
    constexpr const char* partition =
        "common/ui/xpmodifiers/databindings/"
        "xpmodifiers_inventoryboosteritemdbd";
    char data_name[256]{};
    const int count = bf6_rime_dbd_fields(
        ctx, partition, data_name, sizeof(data_name), nullptr, 0);
    if (count <= 0) {
        std::printf("xp-dbd\tread=0\n");
        return false;
    }
    std::vector<bf6_rime_dbd_field> fields(static_cast<size_t>(count));
    const int filled = bf6_rime_dbd_fields(
        ctx, partition, data_name, sizeof(data_name), fields.data(), count);
    std::printf("xp-dbd\tdata=%s\tcount=%d\n", data_name, filled);
    for (const bf6_rime_dbd_field& field : fields)
        std::printf("xp-dbd-field\tname=%s\ttype=%016llX\n", field.name,
                    static_cast<unsigned long long>(field.type_signature));
    const auto has_field = [&fields](const char* name) {
        return std::any_of(fields.begin(), fields.end(),
            [name](const bf6_rime_dbd_field& field) {
                return std::strcmp(field.name, name) == 0;
            });
    };
    const int fake = bf6_rime_dbd_fields(
        ctx,
        "common/ui/xpmodifiers/databindings/"
        "xpmodifiers_inventoryboosteritemdbd__control__",
        nullptr, 0, nullptr, 0);
    std::printf("xp-dbd-control\treal=%d\tfake=%d\n", filled, fake);
    return filled == count && fake < 0 && has_field("Icon") &&
           has_field("Duration") && has_field("Level") &&
           has_field("Color") && has_field("Alpha");
}

bool dump_event_widget_connections(bf6_ctx* ctx)
{
    constexpr const char* partition =
        "common/ui/home/widgets/bulletin/um_beatlogolimitedeventinfo";
    const int count = bf6_rime_connections(ctx, partition, nullptr, 0);
    if (count < 0) return false;
    std::vector<bf6_rime_connection> rows(static_cast<size_t>(count));
    const int filled = bf6_rime_connections(
        ctx, partition, rows.data(), count);
    for (const bf6_rime_connection& row : rows)
        if (row.source == 2 || row.source == 3 || row.source == 5 ||
            row.source == 7 || row.source == 9 || row.target == 2 ||
            row.target == 3 || row.target == 5 || row.target == 7 ||
            row.target == 9)
            std::printf("event-wire\tsource=%d\tfield=%08X\t"
                        "target=%d\tfield=%08X\tmode=%d\n",
                        row.source, row.source_field, row.target,
                        row.target_field, row.mode);
    const int fake = bf6_rime_connections(
        ctx, "common/ui/home/widgets/bulletin/"
             "um_beatlogolimitedeventinfo__control__", nullptr, 0);
    std::printf("event-wire-control\treal=%d\tfake=%d\n", filled, fake);
    return filled == count && fake < 0;
}

bool dump_bulletin_pagination(bf6_ctx* ctx)
{
    constexpr const char* partition =
        "common/ui/home/widgets/bulletin/bulletinsectioncell";
    const int count = bf6_rime_tree(ctx, partition, 6, nullptr, 0, nullptr);
    if (count <= 0) return false;
    std::vector<bf6_rime_node> rows(static_cast<size_t>(count));
    if (bf6_rime_tree(ctx, partition, 6, rows.data(), count, nullptr) != count)
        return false;
    std::vector<int> roots;
    std::vector<int> cta_roots;
    for (size_t index = 0; index < rows.size(); ++index)
    {
        if (std::strstr(rows[index].name, "PaginationButton") != nullptr)
            roots.push_back(static_cast<int>(index));
        if (std::strcmp(rows[index].name, "CTA_Button_01") == 0)
            cta_roots.push_back(static_cast<int>(index));
    }
    for (size_t index = 0; index < rows.size(); ++index)
    {
        int owner = static_cast<int>(index);
        bool descendant = false;
        for (int guard = 0; owner >= 0 &&
             owner < static_cast<int>(rows.size()) && guard < 64; ++guard)
        {
            if (std::find(roots.begin(), roots.end(), owner) != roots.end())
            {
                descendant = true;
                break;
            }
            owner = rows[static_cast<size_t>(owner)].parent;
        }
        if (!descendant) continue;
        const bf6_rime_node& row = rows[index];
        std::printf("bulletin-pagination\tindex=%zu\tparent=%d\tkind=%d\t"
                    "visible=%d\tsize=%.1fx%.1f\tname=%s\timage=%s\ttext=%s\n",
                    index, row.parent, row.kind, row.visible,
                    row.width, row.height, row.name, row.image_asset,
                    row.text_raw[0] ? row.text_raw : row.text_string_id);
    }
    for (size_t index = 0; index < rows.size(); ++index)
    {
        int owner = static_cast<int>(index);
        bool descendant = false;
        for (int guard = 0; owner >= 0 &&
             owner < static_cast<int>(rows.size()) && guard < 64; ++guard)
        {
            if (std::find(cta_roots.begin(), cta_roots.end(), owner) !=
                cta_roots.end())
            {
                descendant = true;
                break;
            }
            owner = rows[static_cast<size_t>(owner)].parent;
        }
        if (!descendant) continue;
        const bf6_rime_node& row = rows[index];
        std::printf("bulletin-cta\tindex=%zu\tparent=%d\tkind=%d\t"
                    "visible=%d\tsize=%.1fx%.1f\tname=%s\timage=%s\ttext=%s\n",
                    index, row.parent, row.kind, row.visible,
                    row.width, row.height, row.name, row.image_asset,
                    row.text_raw[0] ? row.text_raw : row.text_string_id);
    }
    std::printf("bulletin-pagination-control\troots=%zu\tfake=0\n",
                roots.size());
    return roots.size() == 2 && cta_roots.size() == 1;
}

bool dump_home_sidebar_cell(bf6_ctx* ctx)
{
    constexpr const char* partition =
        "common/ui/home/widgets/home_sidebarsection_gamemodecell";
    const int count = bf6_rime_tree(ctx, partition, 6, nullptr, 0, nullptr);
    if (count <= 0) return false;
    std::vector<bf6_rime_node> rows(static_cast<size_t>(count));
    if (bf6_rime_tree(ctx, partition, 6, rows.data(), count, nullptr) != count)
        return false;
    for (size_t index = 0; index < rows.size(); ++index)
    {
        const bf6_rime_node& row = rows[index];
        std::printf("sidebar-cell\tindex=%zu\tparent=%d\tdepth=%d\t"
                    "kind=%d\tvisible=%d\talpha=%.3f\t"
                    "size=%.1fx%.1f\tname=%s\treference=%s\ttext=%s\n",
                    index, row.parent, row.depth, row.kind, row.visible,
                    row.alpha, row.width, row.height, row.name, row.reference,
                    row.text_raw[0] ? row.text_raw : row.text_string_id);
    }
    return true;
}

bool dump_chrome_widget(bf6_ctx* ctx, const char* partition)
{
    const int count = bf6_rime_tree(ctx, partition, 6, nullptr, 0, nullptr);
    if (count <= 0) return false;
    std::vector<bf6_rime_node> rows(static_cast<size_t>(count));
    if (bf6_rime_tree(ctx, partition, 6, rows.data(), count, nullptr) != count)
        return false;
    for (size_t index = 0; index < rows.size(); ++index)
    {
        const bf6_rime_node& row = rows[index];
        std::printf("chrome-widget\troot=%s\tindex=%zu\tparent=%d\t"
                    "kind=%d\tvisible=%d\talpha=%.3f\tsize=%.1fx%.1f\t"
                    "name=%s\treference=%s\ttext=%s\n",
                    partition, index, row.parent, row.kind, row.visible,
                    row.alpha, row.width, row.height, row.name, row.reference,
                    row.text_raw[0] ? row.text_raw : row.text_string_id);
    }
    return true;
}

bool home_background_image_contract(bf6_ctx* ctx)
{
    constexpr const char* partition =
        "common/ui/home/screens/home_screen_bg";
    const int count = bf6_rime_tree(ctx, partition, 6, nullptr, 0, nullptr);
    if (count <= 0) return false;
    std::vector<bf6_rime_node> rows(static_cast<size_t>(count));
    if (bf6_rime_tree(ctx, partition, 6, rows.data(), count, nullptr) != count)
        return false;
    int exact = 0;
    int wrong = 0;
    for (const bf6_rime_node& row : rows)
    {
        std::printf("home-bg-row\tparent=%d\tdepth=%d\tkind=%d\t"
                    "visible=%d\talpha=%.3f\tsize=%.1fx%.1f\t"
                    "layout=%.2f/%.2f,%.1f/%.1f;%.2f/%.2f,%.1f/%.1f\t"
                    "texture=%d/%d/%d/%d/%d/%d\t"
                    "name=%s\treference=%s\timage=%s\n",
                    row.parent, row.depth, row.kind, row.visible, row.alpha,
                    row.width, row.height,
                    row.h.anchor_start, row.h.anchor_end,
                    row.h.offset_start, row.h.offset_end,
                    row.v.anchor_start, row.v.anchor_end,
                    row.v.offset_start, row.v.offset_end,
                    row.image_resize_mode, row.image_h_align,
                    row.image_v_align, row.image_address_v,
                    row.image_address_u, row.image_clip_to_bounds,
                    row.name, row.reference,
                    row.image_asset);
        if (std::strcmp(row.name, "PreviousImage") != 0 &&
            std::strcmp(row.name, "FadeInImage") != 0)
            continue;
        ++exact;
        if (row.kind != BF6_RIME_TEXTURE || row.image_resize_mode != 2 ||
            row.image_h_align != 1 || row.image_v_align != 1)
            ++wrong;
        std::printf("home-bg-image\tname=%s\tresize=%d\th=%d\tv=%d\n",
                    row.name, row.image_resize_mode, row.image_h_align,
                    row.image_v_align);
    }
    const int fake = bf6_rime_tree(
        ctx, "common/ui/home/screens/home_screen_bg__control__", 6,
        nullptr, 0, nullptr);
    std::printf("home-bg-image-control\texact=%d\twrong=%d\tfake=%d\n",
                exact, wrong, fake);
    return exact == 2 && wrong == 0 && fake < 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: home_ui_chain_test <game-dir>\n");
        return 2;
    }
    char error[512]{};
    bf6_ctx* ctx = bf6_open(argv[1], error, static_cast<int>(sizeof(error)));
    if (!ctx) {
        std::fprintf(stderr, "open failed: %s\n", error);
        return 2;
    }
    if (!bf6_mount_frontend(ctx, error, static_cast<int>(sizeof(error)))) {
        std::fprintf(stderr, "mount failed: %s\n", error);
        bf6_close(ctx);
        return 2;
    }

    const Probe probes[] = {
        {"common/ui/home/screens/home_screen", "MainList"},
        {"common/ui/home/widgets/homesectioncell", "DiceUIUniformGridList"},
        {"common/ui/home/widgets/gamecardrowcell", "DiceUIUniformGridList"},
        {"common/ui/home/widgets/gamecardpagecell", "DiceUIUniformGridList"},
        {"common/ui/home/widgets/bulletin/bulletinsectioncell",
         "DiceUIUniformGridList"},
        {"common/ui/home/widgets/__control_missing", "DiceUIUniformGridList"},
    };
    bool ok = true;
    for (size_t index = 0; index + 1 < std::size(probes); ++index)
        ok = probe(ctx, probes[index]) && ok;
    ok = dump_xp_reminder_cell(ctx) && ok;
    ok = dump_xp_booster_contract(ctx) && ok;
    ok = dump_event_widget_connections(ctx) && ok;
    ok = dump_bulletin_pagination(ctx) && ok;
    ok = dump_home_sidebar_cell(ctx) && ok;
    ok = dump_chrome_widget(ctx,
        "common/ui/universalmenu/widgets/visual/um_inboxbutton") && ok;
    ok = dump_chrome_widget(ctx,
        "common/ui/universalmenu/widgets/visual/um_socialbutton") && ok;
    ok = home_background_image_contract(ctx) && ok;

    bf6_rime_tree_stats fake_stats{};
    const int fake = bf6_rime_tree(ctx, probes[std::size(probes) - 1].partition,
                                   6, nullptr, 0, &fake_stats);
    std::printf("fake-partition\tcount=%d\n", fake);
    ok = ok && fake < 0;
    bf6_close(ctx);
    return ok ? 0 : 1;
}
