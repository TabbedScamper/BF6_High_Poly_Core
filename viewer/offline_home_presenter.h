#pragma once

#include "offline_home_provider.h"
#include "rime.h"

#include <string>

namespace offline_home {

/* All four inputs are expanded, install-authored Rime trees.  The presenter
 * never opens an exported layout, substitutes text, or constructs chrome. */
struct Templates {
    const rime::Screen* section = nullptr;
    const rime::Screen* row = nullptr;
    const rime::Screen* page = nullptr;
    const rime::Screen* card = nullptr;
};

struct PresentationAudit {
    int main_lists = 0;
    int main_list_provider_fields = 0;
    int public_play_menu_candidates = 0;
    int public_play_menus = 0;
    int rejected_play_menu_declarations = 0;
    int sections = 0;
    int rows = 0;
    int pages = 0;
    int cards = 0;
    int visible_sections = 0;
    int visible_rows = 0;
    int visible_pages = 0;
    int visible_cards = 0;
    int provider_template_routes = 0;
    int authored_root_frame_routes = 0;
    int authored_data_list_routes = 0;
    int direct_primitive_fields = 0;
    int bound_fields = 0;
    int bound_targets = 0;
    int non_applicable_fields = 0;
    int unrouted_fields = 0;
    int ambiguous_fields = 0;
    int unsupported_fields = 0;
    int shuffled_targets = 0;
    int missing_templates = 0;
    int missing_lists = 0;
    int ambiguous_lists = 0;
    int missing_relations = 0;
    int duplicate_relations = 0;
    int null_collection_items = 0;
    int invalid_layouts = 0;
    int ambiguous_root_frames = 0;
    int truncated_items = 0;
    int fake_root_matches = 0;
    int fake_provider_field_matches = 0;
    int fake_declared_partition_matches = 0;
    int fake_relation_matches = 0;
    int fake_template_routes = 0;
    int perturbed_root_frame_matches = 0;
    int perturbed_data_list_axis_matches = 0;
    int loaded_spinner_routes = 0;
    int fake_loaded_spinner_routes = 0;
    int materialized_elements = 0;

    bool passed() const {
        return main_lists == 1 && public_play_menu_candidates == 2 &&
               public_play_menus == 1 &&
               rejected_play_menu_declarations == 1 && sections > 0 &&
               rows > 0 && cards > 0 && visible_sections > 0 &&
               visible_rows > 0 && visible_pages > 0 && visible_cards > 0 &&
               bound_fields > 0 && bound_targets > 0 &&
               shuffled_targets == 0 && missing_templates == 0 &&
               unrouted_fields == 0 &&
               missing_lists == 0 && ambiguous_lists == 0 &&
               missing_relations == 0 && duplicate_relations == 0 &&
               invalid_layouts == 0 && ambiguous_root_frames == 0 &&
               fake_root_matches == 0 &&
               main_list_provider_fields > 0 &&
               fake_provider_field_matches == 0 &&
               fake_declared_partition_matches == 0 &&
               fake_relation_matches == 0 && fake_template_routes == 0 &&
               perturbed_root_frame_matches == 0 &&
               perturbed_data_list_axis_matches == 0 &&
               loaded_spinner_routes == 1 &&
               fake_loaded_spinner_routes == 0 &&
               materialized_elements > 0;
    }
};

/* Materialize BF6's shipped developer Home provider into BF6's shipped Home
 * screen and cell templates.  canvas_width/height are the authored Rime
 * canvas (normally the caller's decoded 1920x1080 reference frame), not the
 * OS window dimensions.
 *
 * The hierarchy is admitted only when the current-install DBD records prove
 * PlayMenuDBD.Sections -> MenuSectionDBD.Rows ->
 * UniformRowDBD.Interactables -> PlayInteractableDBD.  The page cell is the
 * authored pagination envelope; its capacity is derived from its own uniform
 * grid.  Additional pages/rows/sections are retained in audit.truncated_items
 * when the first-frame viewports cannot display them.
 *
 * On any structural ambiguity the function leaves `out` unchanged.  Primitive
 * fields are applied one at a time only when the real provider pin reaches an
 * authored target and the shuffled-pin control reaches none. */
bool materialize_home(const Snapshot& snapshot, const Templates& templates,
                      const rime::Screen& authored_home,
                      float canvas_width, float canvas_height,
                      rime::Screen& out, PresentationAudit& audit,
                      std::string& error);

} // namespace offline_home
