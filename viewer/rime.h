// Rime: draw the game's OWN screen, rather than an ImGui panel dressed up to
// look like it.
//
// The distinction matters and it is the whole point of this file. Putting the
// game's fonts and palette on ImGui widgets gets you something BF6-flavoured
// with none of its actual structure - no background plates, no button chrome,
// no element tree. The screen is authored data: a tree of Rime elements with
// anchors, offsets, pivots, weights and stack rules. So interpret it.
//
// ImGui is still here, but only as a QUAD BATCHER. Every rect and glyph goes
// through ImDrawList at absolute pixel coordinates; none of ImGui's layout or
// widget code participates. That reuse is worth a great deal of time and costs
// nothing in fidelity, because a draw list is just a vertex buffer.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "bf6_core.h"

namespace rime {

// The element vocabulary the armory screens actually use, by frequency over
// the 237 elements of the 20 armory partitions.
enum class Kind {
    WidgetReference,   // 67  instantiates another partition
    Container,         // 52  layout only
    StackContainer,    // 30  layout, one axis, spacing + padding
    Label,             // 22  text
    LayerEntity,       // 21  a screen layer root
    RepeatShape,       // 14  the point-cost pip strip
    VectorShape,       //  9  plates and chrome
    Fill,              //  5  solid background
    Svg,               //  2
    Texture,           // image/atlas element
    Line,              // authored polyline/rule
    Movie,
    Border,
    Blur,
    Progress,
    ArcProgress,
    Flipbook,
    TextureBlend,
    InputBehavior,
    LayeredIconBinding,
    HardwareIconBinding,
    RemoteWidgetPresenter,
    Unknown
};

// One axis of the box model. Anchors are fractions of the parent; offsets are
// in authored pixels. Pivot and weight are carried through but their exact
// semantics are still being established - see solve() before trusting them.
struct Axis {
    float anchor_start = 0.f, anchor_end = 0.f;
    float offset_start = 0.f, offset_end = 0.f;
    float pivot = 0.f, weight = 0.f;
    bool  present = false;
};

/* One positional argument retained with the installed localized brace
 * pattern. Kind uses BF6_RIME_VALUE_* so graph evaluation and the painter
 * share the same typed value without pre-formatting or string guessing. */
struct TextFormatArgument {
    int kind = BF6_RIME_VALUE_NULL;
    int64_t integer = 0;
    uint64_t unsigned_integer = 0;
    double real = 0.0;
    std::string string;

    bool operator==(const TextFormatArgument& rhs) const
    {
        return kind == rhs.kind && integer == rhs.integer &&
               unsigned_integer == rhs.unsigned_integer &&
               real == rhs.real && string == rhs.string;
    }

    bool operator<(const TextFormatArgument& rhs) const
    {
        if (kind != rhs.kind) return kind < rhs.kind;
        if (integer != rhs.integer) return integer < rhs.integer;
        if (unsigned_integer != rhs.unsigned_integer)
            return unsigned_integer < rhs.unsigned_integer;
        if (real != rhs.real) return real < rhs.real;
        return string < rhs.string;
    }
};

struct Element {
    Kind        kind = Kind::Unknown;
    std::string type_name;
    std::string name;
    uint32_t    name_hash = 0;
    int         depth = 0;
    Axis        h, v;
    int         stack_orientation = -1;   // 0/1 when a stack, -1 otherwise
    int         container_flow_direction = -1; // Default/Reverse/TextDirection
    float       item_spacing = 0.f;
    float       stack_wrap_spacing = 0.f;
    int         stack_overflow_mode = -1;
    int         stack_size_distribution = -1;
    int         stack_space_distribution = -1;
    int         stack_preserve_fit_content = -1;
    /* DiceUIDataListElementData's shipped, concrete repetition policy.
     * FlowDirection is ordering (Default/Reverse/TextDirection), not an
     * orientation; keep it separate from stack_orientation. */
    int         data_list_size_distribution = -1;
    int         data_list_flow_direction = -1;
    int         data_list_space_distribution = -1;
    int         data_list_preserve_fit_content = -1;
    int         widget_use_width = -1;
    int         widget_use_height = -1;
    float       pad_l = 0.f, pad_t = 0.f, pad_r = 0.f, pad_b = 0.f;
    float       width = 0.f, height = 0.f;
    float       alpha = 1.f;
    bool        visible = true, fit_w = false, fit_h = false;
    std::string references_widget;

    // PAINT, resolved by the reader against the game's own 111-entry palette.
    // color_rgb is already sRGB-encoded and `alpha` above modulates it. An
    // element that named no colour and inherited none arrives white, which is
    // what the game does with it too - so this is a value to use, not a
    // sentinel to test.
    uint32_t    color_rgb = 0xFFFFFFu;
    std::string color_name;
    int         color_source = 0;          // BF6_RIME_COLOR_*
    bool        runtime_color_unresolved = false;
    bool        runtime_visibility_unresolved = false;
    bool        runtime_alpha_unresolved = false;
    int         repeat_instances = 0;      // repeat shapes: the authored count
    int         repeat_distribution = -1;  // BF6_RIME_DIST_*

    // SHAPE GEOMETRY, as authored. Vertices are (anchor, offset) pairs and are
    // solved against THIS element's box at draw time - anchor * box + offset -
    // which is how one 12x12 corner bracket serves boxes of every size. Stored
    // unsolved for exactly that reason.
    struct ShapePoint { float ax = 0, ay = 0, ox = 0, oy = 0; };
    std::vector<ShapePoint>     shape_verts;    // filled shapes
    std::vector<unsigned short> shape_indices;
    std::vector<ShapePoint>     shape_corners;  // stroked shapes: the polyline
    int   shape_style = 0;                      // BF6_RIME_DRAW_*
    float shape_thickness = 0.f;
    float shape_size[2] = { 0.f, 0.f };         // asset design box; fit-content input

    // LINE GEOMETRY. Unlike vector shapes these points live directly on the
    // RimeLineElementData record. Relative points are fractions of this
    // element's solved box; absolute points are authored canvas pixels within
    // the box. Values are populated only through bf6_rime_line's gated read.
    struct LinePoint { float x = 0.f, y = 0.f; };
    std::vector<LinePoint> line_points;
    float line_width = 0.f;
    float line_glow_size = 0.f;
    float line_start_progress = 0.f, line_end_progress = 1.f;
    int   line_cap = 0;
    bool  line_relative = false;
    bool  line_closed = false, line_single_pixel = false;

    float progress = 0.f;
    float progress_segment_gap = 0.f;
    int   progress_segment_count = 0;
    int   progress_orientation = -1;
    float border_thickness = 0.f;
    float border_start_alpha = 1.f, border_end_alpha = 1.f;
    int   border_alignment = -1;
    int   border_gradient_direction = -1;

    // FILL. A gradient layer is an alpha RAMP over the element colour, not a
    // colour gradient - both its ends are white in the shipped data. Drawing it
    // as a colour gradient turns the full-screen scrim white-on-white and it
    // disappears, which is the specific mistake this comment exists to prevent.
    int   fill_kind = 0;                        // BF6_RIME_FILL_*
    int   fill_direction = 0;                   // BF6_RIME_GRADIENT_*
    float fill_alpha_start = 1.f, fill_alpha_end = 1.f;
    int   blend_mode = -1;

    // IMAGE/TEXT. These values already come from the live partition walk.
    // Keeping them in the adapter is important even before every painter is
    // implemented: dropping them here made a successfully decoded SVG,
    // texture or font indistinguishable from an absent binding downstream.
    std::string image_asset;
    float       image_uv[4] = { 0.f, 0.f, 1.f, 1.f };
    int         image_resize_mode = -1;
    int         image_h_align = -1;
    int         image_v_align = -1;
    int         image_address_v = -1;
    int         image_address_u = -1;
    int         image_clip_to_bounds = -1;
    float       svg_raster_size[2] = { 0.f, 0.f };
    int         svg_raster_size_mode = -1;
    float       flipbook_progress = 0.f;
    int         flipbook_auto_play = -1;
    int         flipbook_loop = -1;
    int         flipbook_random_frames = -1;
    float       vector_glow_size = 0.f;
    int         vector_scale_line_widths = -1;
    float       blend_gradient_start = 0.f;
    float       blend_gradient_end = 1.f;
    float       blend_mask_threshold = .5f;
    int         blend_invert_mask = -1;
    // Resolved on the one libbf6 worker.  The render thread consumes these
    // immutable values and never calls back into the single-threaded reader.
    int         texture_id = -1;
    int         image_decode_status = 0; // 1 decoded, -1 absent, -2 unsupported
    struct FlipbookFrame {
        float uv[4] = { 0.f, 0.f, 1.f, 1.f };
    };
    std::string flipbook_atlas_asset;
    float       flipbook_frame_rate = 0.f;
    std::vector<FlipbookFrame> flipbook_frames;
    struct SvgPoint { float x = 0.f, y = 0.f; };
    struct SvgContour {
        int shape = 0, point_first = 0, point_count = 0, flag = 0;
        float bounds[4] = { 0.f, 0.f, 0.f, 0.f };
    };
    float       svg_canvas[2] = { 0.f, 0.f };
    int         svg_shape_count = 0;
    std::vector<SvgContour> svg_contours;
    std::vector<SvgPoint>   svg_points;
    std::string font_style;
    // Authored localized text resolved through this partition's property
    // connections, or - when no connection feeds the label - from the
    // label's own authored StringId through the executable's symbolic hash.
    // Empty means the runtime view model owns the value.
    std::string text;
    // Typed positional inputs supplied by the host/provider for authored
    // brace templates such as "{0:d}". Keeping them separate preserves the
    // installed pattern for both measurement and drawing.
    std::vector<int64_t> text_format_integers;
    std::vector<TextFormatArgument> text_format_arguments;
    // The label's authored symbolic key ("ID_...") and RawText, verbatim.
    std::string text_string_id;
    std::string text_raw;
    // The style's RimeTextCapitalization: 0 as written, 1 UPPERCASE.  Apply
    // it when measuring or drawing `text`; never store the transformed text.
    int         text_capitalization = 0;
    float       point_size = 0.f;
    float       line_height = 0.f;
    int         text_valign = -1;
    int         text_halign = -1;
    int         text_overflow_mode = -1;
    int         text_parse_markup = -1;
    int         text_word_wrap = -1;
    float       text_spacing = 0.f;

    // Anchor containers expose the authored point from which customization
    // leader lines leave the tile. (0,0) is valid, hence the separate flag.
    bool        has_attach = false;
    float       attach_x = 0.f, attach_y = 0.f;

    // Authored repetition data for DiceUI runtime-populated lists.
    std::string item_template;
    std::vector<std::string> item_templates;
    int         grid_orientation = -1;
    int         grid_static_segment_item_count = -1;
    float       grid_column_size = 0.f, grid_row_size = 0.f;
    float       grid_row_spacing = 0.f, grid_column_spacing = 0.f;
    int         grid_segment_distribution = -1;
    int         grid_segment_count_mode = -1;
    int         grid_column_flow_direction = -1;
    int         grid_row_flow_direction = -1;
    int         grid_item_fit_content = -1;

    // RimeViewportStretchContainerElementData's shipped old-schema tail.
    // These edge bits selectively expand the solved 1920x1080 reference-box
    // rect to the real render viewport. They remain -1 on every other type.
    int         viewport_flow_direction = -1;
    int         viewport_extend_top = -1;
    int         viewport_extend_bottom = -1;
    int         viewport_extend_left = -1;
    int         viewport_extend_right = -1;

    // Runtime state selected by the decoded list/view-model layer. Authored
    // cells contain all visual variants; 0 normal, 1 hover, 2 focused.
    int         runtime_state = 0;

    // Mask containers retain their exact separate Masks array. Auxiliary mask
    // rows are solved like children but never painted as content.
    int         masking_mode = -1;
    int         invert_mask = -1;
    int         mask_count = 0;
    int         mask_owner = -1;
    float       mask_gradient_end = 1.f;
    float       mask_gradient_start = 0.f;
    float       mask_threshold = 0.5f;
    int         mask_target_instances[4] = { -1, -1, -1, -1 };

    // Filled by solve(): the absolute rect in authored pixels.
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
    bool  solved = false;
    int   parent = -1;

    // Where this element came from, so its shape geometry can be fetched.
    std::string partition;
    int         instance = -1;
    // Index of the WidgetReference occurrence that instantiated this
    // partition, or -1 for the root partition.  Instance ids are only unique
    // inside a partition; the occurrence is required to keep repeated cells
    // from sharing runtime state.
    int         scope = -1;
};

struct Screen {
    std::string partition;
    std::vector<Element> elements;        // depth-ordered, as authored

    struct ConditionalFloatBinding {
        std::string partition;
        int condition_instance = -1;
        uint32_t condition_field = 0;
        int target_instance = -1;
        uint32_t target_field = 0;
        float value_if_true = 0.f;
        float value_if_false = 0.f;
        bool authored_condition = false;
    };
    std::vector<ConditionalFloatBinding> conditional_float_bindings;

    /* Immutable runtime-facing property graphs compiled on the libbf6
     * worker.  Values are supplied later by the render-thread view model, but
     * the routes, selectors and field ids all remain those shipped by the
     * game. */
    struct InterfaceTextGraph {
        std::string partition;
        int scope = -1;
        std::vector<int32_t> interfaces;
        std::vector<bf6_rime_connection> connections;
        std::vector<bf6_rime_conditional_float> float_conditionals;
        std::vector<bf6_rime_float_interpolator> float_interpolators;
        std::vector<bf6_rime_conditional_property> conditionals;
        std::vector<bf6_rime_logic_operation> logic_operations;
        std::vector<bf6_rime_property_status> property_statuses;
        std::vector<bf6_rime_math_instruction> math_instructions;
        std::vector<bf6_rime_math_operation> math_operations;
        std::vector<bf6_rime_rounding> roundings;
        std::vector<bf6_rime_property_cast> property_casts;
        std::vector<bf6_rime_compare_float> compare_floats;
        std::vector<bf6_rime_array_element> array_elements;
        std::vector<bf6_rime_logic_reference> logic_references;
        std::vector<bf6_rime_interface_field> defaults;
        struct LocalizedFormat {
            int32_t instance = -1;
            uint32_t output_field = 0;
            std::string pattern;
            std::vector<uint32_t> argument_fields;
        };
        std::vector<LocalizedFormat> localized_formats;
        struct Input {
            // -1 means a public InterfaceDescriptor input and is fanned out
            // to this graph's exact interface instances.  A non-negative
            // value is an exact authored source entity selected from a live
            // connection endpoint (for example a DBD provider instance).
            int32_t source_instance = -1;
            uint32_t field = 0;
            int kind = BF6_RIME_VALUE_NULL;
            bool boolean = false;
            int64_t integer = 0;
            uint64_t unsigned_integer = 0;
            double real = 0.0;
            std::string string;
            std::vector<std::string> string_array;
            std::vector<int64_t> text_format_integers;
            std::vector<TextFormatArgument> text_format_arguments;
        };
        std::vector<Input> inputs;
    };
    std::vector<InterfaceTextGraph> interface_text_graphs;
};

// Evidence-oracle loaders. The native viewer does not call these and they are
// not packaged beside it; tests may use them to diff a live read against a
// recorded research result.
bool load_tree_tsv(const char* path, std::vector<Screen>& out, std::string& err);

// Load the SOLVED layout: every element with its absolute rect already
// computed and verified, references instantiated, 408 rows for the weapon
// screen with zero negative sizes.
//
// Preferred over solving here, because the box law is subtle enough that our
// first reading got it wrong in three separate ways. The shipped element base
// stores VerticalLayout BEFORE HorizontalLayout - the reverse of what was
// first recorded - so a consumer reading them in declaration order silently
// transposes every axis. offset_end is an INWARD inset, not a same-direction
// offset: on all 14,077 point-anchored axes in common/ui, offset_start ==
// -offset_end exactly, which only makes sense if both edges are being put on
// the same line and the box is then SELF-SIZED from its own Width/Height and
// placed by the pivot. Stacks sequence their source Elements in REVERSE array
// order (40/40 horizontal, 13/13 vertical). The live bf6_rime_tree ABI already
// emits that presentation order, so the renderer must not reverse its rows a
// second time.
bool load_solved_tsv(const char* path, std::vector<Screen>& out, std::string& err);

// Convert the live C ABI rows returned by bf6_rime_tree into the renderer's
// small value types. No game reads happen here and no derived file is opened.
bool from_live(const bf6_rime_node* rows, int count, Screen& out,
               std::string& err);

// Exact scalar coverage law recovered from psRimeMasked/MaskedInverted in the
// current game's DXIL. This is deliberately independent of rasterization.
float mask_coverage(float mask_alpha, float gradient_start,
                    float gradient_end, bool inverted);

/* Return the auxiliary flat-fill row for the one mask form that can be
 * executed exactly without an off-screen raster target: an opaque, inverted,
 * single-target stencil whose complete mask subtree is one flat Fill under
 * layout-only containers. -1 means the route needs the general compositor. */
int simple_inverse_fill_mask(const std::vector<Element>& tree, int owner,
                             const std::vector<unsigned char>& effective_visible,
                             const std::vector<float>& effective_alpha);

// Exact, renderer-independent reduction of the selected armory masks to a
// uniform rectangular coverage surface.  `inside_factor` is the installed
// MaskedInverted pixel law evaluated over that rectangle; outside is 1.
// This accepts either a single flat Fill or a mask subtree whose final opaque
// Normal Fill completely overwrites the target rectangle.  It fails closed
// for every other compositor shape.
struct UniformInverseMask {
    int fill = -1;
    float inside_factor = 1.f;
};
bool uniform_inverse_fill_mask(const std::vector<Element>& tree, int owner,
                               UniformInverseMask& out);

// Pull each shape element's authored geometry in. Separate from from_live
// because it costs one partition read per shape and only wants doing when the
// tree changes, not per frame.
void load_shapes(bf6_ctx* c, Screen& s);

// Resolve authored Rime image EBX assets to the exact installed-game native
// texture id or compiled cubic SVG geometry.  Must run on the libbf6 worker.
void load_images(bf6_ctx* c, Screen& s);

// Select the authored flipbook frame at one wall-clock time. Non-autoplay
// elements use their Progress property; autoplay elements obey the asset rate
// and the element's Loop/RandomFrames controls. Returns -1 when undecoded.
int flipbook_frame_index(const Element& element, double seconds);

// Follow authored localized-string entities through directed property wires.
// Conflicting targets stay empty rather than selecting a graph-order winner.
int load_text_bindings(bf6_ctx* c, Screen& s, int* ambiguous = nullptr);

// Compile the shipped ConditionalFloatEntityData wires into direct element
// property bindings. Constants and topology are read live; only runtime state
// (currently IsFocused on an element) is supplied by the viewer.
int load_conditional_float_bindings(bf6_ctx* c, Screen& s);

// Apply the compiled bindings before solve(). Unknown condition sources and
// unknown target fields are deliberately left at their authored defaults.
int apply_conditional_float_bindings(Screen& s);

// Copy the live InterfaceDescriptor/property graph into the renderer value
// object while the single-threaded libbf6 context is available.
int load_interface_text_graphs(bf6_ctx* c, Screen& s);

// Execute non-null typed InterfaceDescriptor defaults through the authored
// property graph and apply supported element properties. Null remains an
// unresolved provider input. Returns the number of element properties set.
int apply_interface_defaults(Screen& s);

// Set one runtime string on one exact widget partition and execute its shipped
// property graph. `property_name` is case-sensitive because dynamic Rime field
// ids are case-sensitive djb2-xor hashes. Returns the number of label targets
// receiving an unambiguous value; a nonexistent property returns zero.
int set_interface_text(Screen& s, const char* partition,
                       const char* property_name, const std::string& value,
                       int* ambiguous = nullptr);

// Typed counterparts to set_interface_text. They use the same exact public
// property id and shipped connection graph, and update every supported target
// (visibility, alpha, size and labels) before the caller solves layout.
int set_interface_bool(Screen& s, const char* partition,
                       const char* property_name, bool value,
                       int* ambiguous = nullptr);
int set_interface_int(Screen& s, const char* partition,
                      const char* property_name, int64_t value,
                      int* ambiguous = nullptr);
int set_interface_uint(Screen& s, const char* partition,
                       const char* property_name, uint64_t value,
                       int* ambiguous = nullptr);
int set_interface_real(Screen& s, const char* partition,
                       const char* property_name, double value,
                       int* ambiguous = nullptr);

// Exact-hash counterparts used when a parent WidgetReference connection
// supplies the child's public field id directly.  They execute that shipped
// field through every authored WidgetReference boundary and stop on a graph
// cycle; no hash-to-name dictionary or route-specific alias participates.
int set_interface_text_field(Screen& s, const char* partition, uint32_t field,
                             const std::string& value,
                             int* ambiguous = nullptr);
int set_interface_string_array_field(
    Screen& s, const char* partition, uint32_t field,
    const std::vector<std::string>& value,
    int* ambiguous = nullptr);
int set_interface_bool_field(Screen& s, const char* partition, uint32_t field,
                             bool value, int* ambiguous = nullptr);
int set_interface_int_field(Screen& s, const char* partition, uint32_t field,
                            int64_t value, int* ambiguous = nullptr);
int set_interface_uint_field(Screen& s, const char* partition, uint32_t field,
                             uint64_t value, int* ambiguous = nullptr);
int set_interface_real_field(Screen& s, const char* partition, uint32_t field,
                             double value, int* ambiguous = nullptr);

// Exact-occurrence variants for a repeated child widget. `scope` is the
// WidgetReference element index that owns that compiled graph occurrence.
// They are intentionally not allowed to choose a first matching copy.
int set_interface_bool_field_scoped(Screen& s, const char* partition,
                                    int scope, uint32_t field, bool value,
                                    int* ambiguous = nullptr);
int set_interface_int_field_scoped(Screen& s, const char* partition,
                                   int scope, uint32_t field, int64_t value,
                                   int* ambiguous = nullptr);

// Inject one value at an exact provider-side source pin.  Unlike the public
// InterfaceDescriptor helpers above, these functions do not hash a short
// property name or fan a value across interfaces.  They require
// `provider_field` to occur on the source side of the shipped connection graph
// and reject it when more than one source entity owns that pin.  This is the
// runtime route used by DBD-backed list cells.
int set_provider_text(Screen& s, const char* partition,
                      uint32_t provider_field, const std::string& value,
                      int* ambiguous = nullptr);
int set_provider_string_array(Screen& s, const char* partition,
                              uint32_t provider_field,
                              const std::vector<std::string>& value,
                              int* ambiguous = nullptr);
int set_provider_bool(Screen& s, const char* partition,
                      uint32_t provider_field, bool value,
                      int* ambiguous = nullptr);
int set_provider_int(Screen& s, const char* partition,
                     uint32_t provider_field, int64_t value,
                     int* ambiguous = nullptr);
int set_provider_uint(Screen& s, const char* partition,
                      uint32_t provider_field, uint64_t value,
                      int* ambiguous = nullptr);
int set_provider_real(Screen& s, const char* partition,
                      uint32_t provider_field, double value,
                      int* ambiguous = nullptr);

uint32_t property_hash(const char* property_name);

// Mark element colours whose source is a live InterfaceDescriptor input. The
// authored/editor colour is only a fallback in that case; until the caller's
// view-model value is decoded, painting it would fabricate visible UI.
int mark_unresolved_interface_colors(bf6_ctx* c, Screen& s);
// Mark paint-state fields sourced directly from a blueprint's public runtime
// interface. The EBX value on those targets is an editor cache, not a safe
// production fallback. Returns the number of newly marked element fields.
int mark_unresolved_interface_states(bf6_ctx* c, Screen& s);

// Resolve every element to an absolute rect inside a canvas. Live screens
// retain bf6_rime_tree's explicit parent graph; older evidence rows without
// parents fall back to traversal depth.
//
// The measured box law: end offsets are inward insets; a point anchor is
// self-sized from Width/Height and positioned by SizingPivot. Fit-to-content
// currently falls back to the authored size because shaping/child measurement
// is a separate renderer concern.
void solve(Screen& s, float canvas_w, float canvas_h);

// Rime's authored 1920x1080 tree is an aspect-preserved reference frame, not
// an independently scaled full-client canvas.  The otherwise-zero-width
// ViewportStretch elements prove why: only their explicit Extend* edges fill
// the letter/pillar region outside this centered frame.
struct ReferenceFrame {
    float scale = 0.f;
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
};

struct ViewportRect {
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
};

bool reference_frame(float viewport_w, float viewport_h,
                     float canvas_w, float canvas_h,
                     ReferenceFrame& out);

// Map one already-solved authored element through the reference frame and
// apply its exact viewport-stretch edge selectors. No presentation constants
// or screenshots enter this operation.
bool viewport_rect(const Element& element, const ReferenceFrame& frame,
                   float viewport_w, float viewport_h, ViewportRect& out);

// Compose the paint state carried by the authored parent tree. Rime alpha is
// multiplicative: fading a WidgetReference/container fades every descendant,
// even when the child partition's local Alpha remains 1. Visibility follows
// the same ancestry. Invalid/forward parent links fail closed instead of
// turning an untrusted row into a visible root.
bool effective_paint_state(const std::vector<Element>& elements,
                           std::vector<unsigned char>& visible,
                           std::vector<float>& alpha);

// Exact per-instance cells for the implemented linear repeat-shape route.
// The referenced shape's anchors/offsets are solved inside each returned cell.
// Radial and grid distributions fail closed until their runtime materializer
// has been reproduced.
struct RepeatCell {
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
};
bool repeat_shape_cells(const Element& repeat, std::vector<RepeatCell>& out);

// Runtime geometry for a shipped DiceUI uniform-grid list.  `grid` is the
// live 6cc27f98... element and `item` is the partition named by its
// ItemTemplate pointer.  No recorded table or presentation constant enters
// this calculation.
struct UniformGridLayout {
    int columns = 0;
    int visible_rows = 0;
    float x0 = 0.f, y0 = 0.f;
    float cell_w = 0.f, cell_h = 0.f;
    float column_spacing = 0.f, row_spacing = 0.f;
    // Coordinate reference only.  It is not necessarily the painted card
    // root and its authored size is never expanded to the grid slot.
    int normalization_root = -1;
};

// Resolve a uniform grid's content box, dynamic/static segment count and cell
// geometry from the live element fields. Returns false for a non-grid,
// missing/ambiguous normalization root, invalid enum, or non-positive
// geometry.
bool uniform_grid_layout(const Element& grid, const Screen& item,
                         UniformGridLayout& out);

// Copy one already-solved item template into an authored grid cell. Every
// solved descendant is first made local to the selected normalization root and only
// then translated to the cell. This is deliberately separate from the grid
// sizing law: it prevents reference children from inheriting the template's
// screen-space origin (the old ~150 px decoration displacement).
bool append_grid_item(const Screen& item, const UniformGridLayout& layout,
                      float cell_x, float cell_y, int runtime_state,
                      std::vector<Element>& out);

Kind kind_of(const std::string& type_name);

// Expand every RimeWidgetReference into the tree of the partition it names.
//
// A screen is not one partition. menuweaponscreen is 8 elements, of which 6
// are references; the actual grid, info panel, point cost and stat bars each
// live in their own partition and are INSTANTIATED into the parent's box.
// Without this a consumer draws six empty rectangles and concludes the screen
// is nearly empty, which is what it looked like.
//
// Returns a flattened tree with the referenced children inlined at the
// reference's depth. `unresolved` receives the count of references whose
// partition is not in `all` - they stay as leaves, because a widget we do not
// have is a gap to show, not a thing to invent.
Screen instantiate(const std::vector<Screen>& all, size_t root, int max_depth,
                   int* unresolved);

}  // namespace rime
