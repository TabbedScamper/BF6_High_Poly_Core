#include "rime.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace rime {

Kind kind_of(const std::string& t)
{
    if (t.rfind("RimeWidgetReference", 0) == 0)        return Kind::WidgetReference;
    if (t == "RimeStackContainerElementData")          return Kind::StackContainer;
    if (t == "RimeContainerElementData")               return Kind::Container;
    if (t == "RimeLabelElementData")                   return Kind::Label;
    if (t == "RimeLayerEntityData")                    return Kind::LayerEntity;
    if (t == "RimeRepeatShapeElementData")             return Kind::RepeatShape;
    if (t == "RimeVectorShapeElementData")             return Kind::VectorShape;
    if (t == "RimeFillElementData")                    return Kind::Fill;
    if (t == "RimeSvgElementData")                     return Kind::Svg;
    if (t == "RimeTextureElementData")                 return Kind::Texture;
    if (t == "RimeLineElementData")                    return Kind::Line;
    if (t == "RimeMovieElementData")                   return Kind::Movie;
    if (t == "RimeBorderElementData")                  return Kind::Border;
    if (t == "RimeBlurElementData")                    return Kind::Blur;
    if (t == "RimeProgressBarElementData")             return Kind::Progress;
    if (t == "RimeArcProgressBarElementData")          return Kind::ArcProgress;
    if (t == "RimeFlipbookTextureElementData")         return Kind::Flipbook;
    if (t == "RimeTextureBlendContainerElementData")   return Kind::TextureBlend;
    if (t == "DiceUIInputBehaviorElementData" ||
        t == "BFUIMouseInputElementData")              return Kind::InputBehavior;
    if (t == "751ec8e6-27dc-bc4f-6c40-7c076ef6eb20") return Kind::LayeredIconBinding;
    if (t == "ab871b92-e85b-4ba1-c980-7aca88024128") return Kind::HardwareIconBinding;
    if (t == "9792d266-dad3-ed84-7a55-de19ee7190fa") return Kind::RemoteWidgetPresenter;
    // Stretch containers hold the edge gradient fills; they lay out and their
    // Fill children draw.
    if (t == "RimeViewportStretchContainerElementData") return Kind::Container;
    // The widget-reference element ships as a bare type guid in retail.
    if (t.rfind("ca959de9", 0) == 0)                    return Kind::WidgetReference;
    return Kind::Unknown;
}

bool from_live(const bf6_rime_node* rows, int count, Screen& out,
               std::string& err)
{
    out = Screen{};
    err.clear();
    if (!rows || count <= 0) { err = "live Rime tree has no rows"; return false; }
    out.partition = rows[0].partition;
    out.elements.reserve((size_t)count);
    for (int i = 0; i < count; i++)
    {
        const bf6_rime_node& r = rows[i];
        Element e;
        e.type_name = r.type_name;
        switch (r.kind)
        {
        case BF6_RIME_WIDGET_REFERENCE: e.kind = Kind::WidgetReference; break;
        case BF6_RIME_CONTAINER: e.kind = Kind::Container; break;
        case BF6_RIME_STACK_CONTAINER: e.kind = Kind::StackContainer; break;
        case BF6_RIME_LABEL: e.kind = Kind::Label; break;
        case BF6_RIME_LAYER: e.kind = Kind::LayerEntity; break;
        case BF6_RIME_REPEAT_SHAPE: e.kind = Kind::RepeatShape; break;
        case BF6_RIME_VECTOR_SHAPE: e.kind = Kind::VectorShape; break;
        case BF6_RIME_FILL: e.kind = Kind::Fill; break;
        case BF6_RIME_SVG: e.kind = Kind::Svg; break;
        case BF6_RIME_TEXTURE: e.kind = Kind::Texture; break;
        case BF6_RIME_LINE: e.kind = Kind::Line; break;
        case BF6_RIME_MOVIE: e.kind = Kind::Movie; break;
        case BF6_RIME_BORDER: e.kind = Kind::Border; break;
        case BF6_RIME_BLUR: e.kind = Kind::Blur; break;
        case BF6_RIME_PROGRESS: e.kind = Kind::Progress; break;
        case BF6_RIME_ARC_PROGRESS: e.kind = Kind::ArcProgress; break;
        case BF6_RIME_FLIPBOOK: e.kind = Kind::Flipbook; break;
        case BF6_RIME_TEXTURE_BLEND: e.kind = Kind::TextureBlend; break;
        case BF6_RIME_INPUT_BEHAVIOR: e.kind = Kind::InputBehavior; break;
        case BF6_RIME_LAYERED_ICON_BINDING: e.kind = Kind::LayeredIconBinding; break;
        case BF6_RIME_HARDWARE_ICON_BINDING: e.kind = Kind::HardwareIconBinding; break;
        case BF6_RIME_REMOTE_WIDGET_PRESENTER: e.kind = Kind::RemoteWidgetPresenter; break;
        default: e.kind = Kind::Unknown; break;
        }
        e.name = r.name;
        e.name_hash = r.name_hash;
        e.depth = r.depth;
        e.parent = r.parent;
        e.h = Axis{ r.h.anchor_start, r.h.anchor_end, r.h.offset_start,
                    r.h.offset_end, r.h.pivot, r.h.weight,
                    r.kind != BF6_RIME_LAYER };
        e.v = Axis{ r.v.anchor_start, r.v.anchor_end, r.v.offset_start,
                    r.v.offset_end, r.v.pivot, r.v.weight,
                    r.kind != BF6_RIME_LAYER };
        e.width = r.width; e.height = r.height; e.alpha = r.alpha;
        e.visible = r.visible != 0; e.fit_w = r.fit_w != 0; e.fit_h = r.fit_h != 0;
        e.pad_l = r.pad_l; e.pad_t = r.pad_t; e.pad_r = r.pad_r; e.pad_b = r.pad_b;
        e.item_spacing = r.item_spacing;
        e.stack_orientation = r.stack_orientation;
        e.container_flow_direction = r.container_flow_direction;
        e.stack_wrap_spacing = r.stack_wrap_spacing;
        e.stack_overflow_mode = r.stack_overflow_mode;
        e.stack_size_distribution = r.stack_size_distribution;
        e.stack_space_distribution = r.stack_space_distribution;
        e.stack_preserve_fit_content = r.stack_preserve_fit_content;
        e.data_list_size_distribution = r.data_list_size_distribution;
        e.data_list_flow_direction = r.data_list_flow_direction;
        e.data_list_space_distribution = r.data_list_space_distribution;
        e.data_list_preserve_fit_content =
            r.data_list_preserve_fit_content;
        e.widget_use_width = r.widget_use_width;
        e.widget_use_height = r.widget_use_height;
        e.references_widget = r.reference;
        e.color_rgb = r.color_rgb;
        e.color_name = r.color_name;
        e.color_source = r.color_source;
        e.repeat_instances = r.repeat_instances;
        e.repeat_distribution = r.repeat_distribution;
        e.instance = r.instance;
        e.partition = r.partition;
        e.fill_kind = r.fill_kind;
        e.fill_direction = r.fill_direction;
        e.fill_alpha_start = r.fill_alpha_start;
        e.fill_alpha_end = r.fill_alpha_end;
        e.blend_mode = r.blend_mode;
        e.image_asset = r.image_asset;
        for (int j = 0; j < 4; ++j) e.image_uv[j] = r.image_uv[j];
        e.image_resize_mode = r.image_resize_mode;
        e.image_h_align = r.image_h_align;
        e.image_v_align = r.image_v_align;
        e.image_address_v = r.image_address_v;
        e.image_address_u = r.image_address_u;
        e.image_clip_to_bounds = r.image_clip_to_bounds;
        e.svg_raster_size[0] = r.svg_raster_size[0];
        e.svg_raster_size[1] = r.svg_raster_size[1];
        e.svg_raster_size_mode = r.svg_raster_size_mode;
        e.flipbook_progress = r.flipbook_progress;
        e.flipbook_auto_play = r.flipbook_auto_play;
        e.flipbook_loop = r.flipbook_loop;
        e.flipbook_random_frames = r.flipbook_random_frames;
        e.vector_glow_size = r.vector_glow_size;
        e.vector_scale_line_widths = r.vector_scale_line_widths;
        e.blend_gradient_start = r.blend_gradient_start;
        e.blend_gradient_end = r.blend_gradient_end;
        e.blend_mask_threshold = r.blend_mask_threshold;
        e.blend_invert_mask = r.blend_invert_mask;
        e.font_style = r.font_style;
        e.text_string_id = r.text_string_id;
        e.text_raw = r.text_raw;
        e.text_capitalization = r.text_capitalization;
        e.point_size = r.point_size;
        e.line_height = r.line_height;
        e.text_valign = r.text_valign;
        e.text_halign = r.text_halign;
        e.text_overflow_mode = r.text_overflow_mode;
        e.text_parse_markup = r.text_parse_markup;
        e.text_word_wrap = r.text_word_wrap;
        e.text_spacing = r.text_spacing;
        e.has_attach = r.has_attach != 0;
        e.attach_x = r.attach_x;
        e.attach_y = r.attach_y;
        e.item_template = r.item_template;
        e.item_templates.clear();
        for (int j = 0; j < r.item_template_count && j < 8; ++j)
            if (r.item_templates[j][0])
                e.item_templates.emplace_back(r.item_templates[j]);
        e.grid_orientation = r.grid_orientation;
        e.grid_static_segment_item_count = r.grid_static_segment_item_count;
        e.grid_column_size = r.grid_column_size;
        e.grid_row_size = r.grid_row_size;
        e.grid_row_spacing = r.grid_row_spacing;
        e.grid_column_spacing = r.grid_column_spacing;
        e.grid_segment_distribution = r.grid_segment_distribution;
        e.grid_segment_count_mode = r.grid_segment_count_mode;
        e.grid_column_flow_direction = r.grid_column_flow_direction;
        e.grid_row_flow_direction = r.grid_row_flow_direction;
        e.grid_item_fit_content = r.grid_item_fit_content;
        e.viewport_flow_direction = r.viewport_flow_direction;
        e.viewport_extend_top = r.viewport_extend_top;
        e.viewport_extend_bottom = r.viewport_extend_bottom;
        e.viewport_extend_left = r.viewport_extend_left;
        e.viewport_extend_right = r.viewport_extend_right;
        e.progress = r.progress;
        e.progress_segment_gap = r.progress_segment_gap;
        e.progress_segment_count = r.progress_segment_count;
        e.progress_orientation = r.progress_orientation;
        e.border_thickness = r.border_thickness;
        e.border_start_alpha = r.border_start_alpha;
        e.border_end_alpha = r.border_end_alpha;
        e.border_alignment = r.border_alignment;
        e.border_gradient_direction = r.border_gradient_direction;
        e.masking_mode = r.masking_mode;
        e.invert_mask = r.invert_mask;
        e.mask_count = r.mask_count;
        e.mask_owner = r.mask_owner;
        e.mask_gradient_end = r.mask_gradient_end;
        e.mask_gradient_start = r.mask_gradient_start;
        e.mask_threshold = r.mask_threshold;
        for (int j = 0; j < 4; ++j)
            e.mask_target_instances[j] = r.mask_target_instances[j];
        out.elements.push_back(std::move(e));
    }
    return true;
}

float mask_coverage(float mask_alpha, float gradient_start,
                    float gradient_end, bool inverted)
{
    const float span = gradient_end - gradient_start;
    float coverage = 0.f;
    if (span == 0.f)
        coverage = mask_alpha > gradient_start ? 1.f : 0.f;
    else
    {
        coverage = (mask_alpha - gradient_start) / span;
        coverage = (std::max)(0.f, (std::min)(1.f, coverage));
    }
    return inverted ? 1.f - coverage : coverage;
}

int simple_inverse_fill_mask(const std::vector<Element>& tree, int owner,
                             const std::vector<unsigned char>& effective_visible,
                             const std::vector<float>& effective_alpha)
{
    if (owner < 0 || (size_t)owner >= tree.size() ||
        effective_visible.size() != tree.size() ||
        effective_alpha.size() != tree.size())
        return -1;
    const Element& mask = tree[(size_t)owner];
    if (mask.mask_owner >= 0 || mask.masking_mode != 0 ||
        mask.invert_mask != 1 || mask.mask_count != 1 ||
        std::fabs(mask.mask_gradient_start) > 1e-6f ||
        std::fabs(mask.mask_gradient_end - 1.f) > 1e-6f)
        return -1;

    int target = -1;
    int fill = -1;
    for (size_t i = 0; i < tree.size(); ++i)
    {
        const Element& row = tree[i];
        if (row.mask_owner != owner) continue;
        if (row.parent == owner &&
            row.instance == mask.mask_target_instances[0])
        {
            if (target >= 0) return -1;
            target = (int)i;
        }
        if (row.kind == Kind::Container || row.kind == Kind::StackContainer ||
            row.kind == Kind::LayerEntity)
            continue;
        if (row.kind != Kind::Fill || row.fill_kind != BF6_RIME_FILL_FLAT ||
            fill >= 0 || !row.solved || !effective_visible[i] ||
            std::fabs(effective_alpha[i] - 1.f) > 1e-6f ||
            row.runtime_visibility_unresolved || row.runtime_alpha_unresolved)
            return -1;
        fill = (int)i;
    }
    return target >= 0 && fill >= 0 ? fill : -1;
}

bool uniform_inverse_fill_mask(const std::vector<Element>& tree, int owner,
                               UniformInverseMask& out)
{
    out = UniformInverseMask{};
    if (owner < 0 || (size_t)owner >= tree.size()) return false;
    const Element& mask = tree[(size_t)owner];
    if (mask.mask_owner >= 0 || mask.masking_mode != 0 ||
        mask.invert_mask != 1 || mask.mask_count != 1)
        return false;

    int target = -1;
    int finalPaint = -1;
    int paintCount = 0;
    for (size_t i = 0; i < tree.size(); ++i)
    {
        const Element& row = tree[i];
        if (row.mask_owner != owner) continue;
        if (row.parent == owner &&
            row.instance == mask.mask_target_instances[0])
        {
            if (target >= 0) return false;
            target = (int)i;
        }
        const bool paints = row.kind == Kind::Fill ||
            row.kind == Kind::Label || row.kind == Kind::VectorShape ||
            row.kind == Kind::RepeatShape || row.kind == Kind::Svg ||
            row.kind == Kind::Texture || row.kind == Kind::Line ||
            row.kind == Kind::Movie || row.kind == Kind::Border ||
            row.kind == Kind::Progress || row.kind == Kind::ArcProgress ||
            row.kind == Kind::Flipbook || row.kind == Kind::TextureBlend;
        if (paints) { finalPaint = (int)i; ++paintCount; }
    }
    if (target < 0 || finalPaint < 0) return false;
    for (size_t i = 0; i < tree.size(); ++i)
    {
        if (tree[i].mask_owner != owner) continue;
        int cursor = (int)i;
        for (size_t guard = 0;
             cursor != target && cursor != owner && guard <= tree.size();
             ++guard)
        {
            if (cursor < 0 || (size_t)cursor >= tree.size()) return false;
            cursor = tree[(size_t)cursor].parent;
        }
        if (cursor != target) return false;
    }
    const Element& fill = tree[(size_t)finalPaint];
    if (fill.kind != Kind::Fill || fill.fill_kind != BF6_RIME_FILL_FLAT ||
        fill.blend_mode != 0 || !fill.solved || !fill.visible ||
        fill.runtime_visibility_unresolved || fill.runtime_alpha_unresolved)
        return false;

    // Mask rows are an isolated off-screen surface.  Their alpha chain stops
    // at the mask owner: multiplying the owner's content opacity here would
    // multiply it a second time in psRimeMaskedInverted.
    float localAlpha = 1.f;
    bool localVisible = true;
    int cursor = finalPaint;
    for (size_t guard = 0; cursor != owner && guard <= tree.size(); ++guard)
    {
        if (cursor < 0 || (size_t)cursor >= tree.size()) return false;
        const Element& row = tree[(size_t)cursor];
        if (row.mask_owner != owner) return false;
        localAlpha *= row.alpha;
        localVisible = localVisible && row.visible;
        cursor = row.parent;
    }
    if (cursor != owner || !localVisible || !std::isfinite(localAlpha))
        return false;

    if (paintCount > 1)
    {
        // Earlier mask paint is irrelevant only when the final Normal source
        // is fully opaque and covers exactly the selected target surface.
        const Element& root = tree[(size_t)target];
        if (!root.solved || std::fabs(localAlpha - 1.f) > 1e-6f ||
            std::fabs(fill.x0 - root.x0) > 1e-4f ||
            std::fabs(fill.y0 - root.y0) > 1e-4f ||
            std::fabs(fill.x1 - root.x1) > 1e-4f ||
            std::fabs(fill.y1 - root.y1) > 1e-4f)
            return false;
    }

    out.fill = finalPaint;
    out.inside_factor = mask_coverage(localAlpha,
        mask.mask_gradient_start, mask.mask_gradient_end, true);
    return true;
}

bool reference_frame(float viewport_w, float viewport_h,
                     float canvas_w, float canvas_h,
                     ReferenceFrame& out)
{
    out = ReferenceFrame{};
    if (!(viewport_w > 0.f && viewport_h > 0.f &&
          canvas_w > 0.f && canvas_h > 0.f))
        return false;
    const float sx = viewport_w / canvas_w;
    const float sy = viewport_h / canvas_h;
    out.scale = (std::min)(sx, sy);
    const float w = canvas_w * out.scale;
    const float h = canvas_h * out.scale;
    out.x0 = (viewport_w - w) * 0.5f;
    out.y0 = (viewport_h - h) * 0.5f;
    out.x1 = out.x0 + w;
    out.y1 = out.y0 + h;
    return true;
}

bool viewport_rect(const Element& element, const ReferenceFrame& frame,
                   float viewport_w, float viewport_h, ViewportRect& out)
{
    out = ViewportRect{};
    if (!element.solved || !(frame.scale > 0.f) ||
        !(viewport_w > 0.f && viewport_h > 0.f))
        return false;
    out.x0 = frame.x0 + element.x0 * frame.scale;
    out.y0 = frame.y0 + element.y0 * frame.scale;
    out.x1 = frame.x0 + element.x1 * frame.scale;
    out.y1 = frame.y0 + element.y1 * frame.scale;
    if (element.viewport_extend_left == 1) out.x0 = 0.f;
    if (element.viewport_extend_top == 1) out.y0 = 0.f;
    if (element.viewport_extend_right == 1) out.x1 = viewport_w;
    if (element.viewport_extend_bottom == 1) out.y1 = viewport_h;
    return true;
}

bool effective_paint_state(const std::vector<Element>& elements,
                           std::vector<unsigned char>& visible,
                           std::vector<float>& alpha)
{
    visible.assign(elements.size(), 0);
    alpha.assign(elements.size(), 0.f);
    bool valid = true;
    for (size_t index = 0; index < elements.size(); ++index)
    {
        const Element& element = elements[index];
        bool parentVisible = true;
        float parentAlpha = 1.f;
        if (element.parent >= 0)
        {
            if (element.parent >= (int)index)
            {
                valid = false;
                continue;
            }
            parentVisible = visible[(size_t)element.parent] != 0;
            parentAlpha = alpha[(size_t)element.parent];
        }
        visible[index] = element.visible && parentVisible ? 1 : 0;
        alpha[index] = parentAlpha * element.alpha;
    }
    return valid;
}

bool repeat_shape_cells(const Element& repeat, std::vector<RepeatCell>& out)
{
    out.clear();
    if (repeat.kind != Kind::RepeatShape || !repeat.solved ||
        repeat.repeat_instances <= 0 || repeat.repeat_instances >= 4096 ||
        !(repeat.x1 > repeat.x0) || !(repeat.y1 > repeat.y0))
        return false;
    const bool horizontal =
        repeat.repeat_distribution == BF6_RIME_DIST_HORIZONTAL;
    const bool vertical =
        repeat.repeat_distribution == BF6_RIME_DIST_VERTICAL;
    if (!horizontal && !vertical) return false;
    out.reserve((size_t)repeat.repeat_instances);
    for (int index = 0; index < repeat.repeat_instances; ++index)
    {
        RepeatCell cell;
        cell.x0 = horizontal
            ? repeat.x0 + (repeat.x1 - repeat.x0) * (float)index /
                (float)repeat.repeat_instances
            : repeat.x0;
        cell.x1 = horizontal
            ? repeat.x0 + (repeat.x1 - repeat.x0) * (float)(index + 1) /
                (float)repeat.repeat_instances
            : repeat.x1;
        cell.y0 = vertical
            ? repeat.y0 + (repeat.y1 - repeat.y0) * (float)index /
                (float)repeat.repeat_instances
            : repeat.y0;
        cell.y1 = vertical
            ? repeat.y0 + (repeat.y1 - repeat.y0) * (float)(index + 1) /
                (float)repeat.repeat_instances
            : repeat.y1;
        out.push_back(cell);
    }
    return true;
}

void load_shapes(bf6_ctx* c, Screen& s)
{
    if (!c) return;
    for (Element& e : s.elements)
    {
        if (e.kind == Kind::Line)
        {
            if (e.partition.empty() || e.instance < 0) continue;
            bf6_rime_line_info li{};
            const int count = bf6_rime_line(c, e.partition.c_str(), e.instance,
                                             &li, nullptr, 0);
            if (count < 0 || count != li.point_count) continue;
            e.line_width = li.width;
            e.line_glow_size = li.glow_size;
            e.line_start_progress = li.start_progress;
            e.line_end_progress = li.end_progress;
            e.line_cap = li.cap_type;
            e.line_relative = li.relative_coordinates != 0;
            e.line_closed = li.close_line_shape != 0;
            e.line_single_pixel = li.single_pixel != 0;
            if (count > 0)
            {
                std::vector<bf6_rime_line_point> points((size_t)count);
                if (bf6_rime_line(c, e.partition.c_str(), e.instance, &li,
                                  points.data(), count) != count)
                    continue;
                e.line_points.resize(points.size());
                for (size_t i = 0; i < points.size(); ++i)
                    e.line_points[i] = Element::LinePoint{points[i].x, points[i].y};
            }
            continue;
        }
        if (e.kind != Kind::VectorShape && e.kind != Kind::RepeatShape) continue;
        if (e.partition.empty() || e.instance < 0) continue;

        bf6_rime_shape_info si{};
        if (bf6_rime_shape(c, e.partition.c_str(), e.instance, &si,
                           nullptr, 0, nullptr, 0, nullptr, 0) <= 0) continue;
        e.shape_style = si.draw_style;
        e.shape_thickness = si.thickness;
        e.shape_size[0] = si.size[0];
        e.shape_size[1] = si.size[1];

        if (si.vertex_count > 0 && si.index_count > 0)
        {
            std::vector<bf6_rime_shape_vertex> vs((size_t)si.vertex_count);
            std::vector<unsigned short> is((size_t)si.index_count);
            bf6_rime_shape(c, e.partition.c_str(), e.instance, &si,
                           vs.data(), si.vertex_count, is.data(), si.index_count,
                           nullptr, 0);
            e.shape_verts.resize(vs.size());
            for (size_t i = 0; i < vs.size(); i++)
                e.shape_verts[i] = Element::ShapePoint{ vs[i].anchor[0], vs[i].anchor[1],
                                                        vs[i].offset[0], vs[i].offset[1] };
            e.shape_indices = is;
        }
        if (si.corner_count > 0)
        {
            std::vector<bf6_rime_shape_corner> ks((size_t)si.corner_count);
            bf6_rime_shape(c, e.partition.c_str(), e.instance, &si, nullptr, 0,
                           nullptr, 0, ks.data(), si.corner_count);
            e.shape_corners.resize(ks.size());
            for (size_t i = 0; i < ks.size(); i++)
                e.shape_corners[i] = Element::ShapePoint{ ks[i].anchor[0], ks[i].anchor[1],
                                                          ks[i].offset[0], ks[i].offset[1] };
        }
    }
}

void load_images(bf6_ctx* c, Screen& s)
{
    if (!c) return;
    for (Element& e : s.elements)
    {
        if (e.image_asset.empty()) continue;
        if (e.kind == Kind::Texture)
        {
            e.texture_id = bf6_rime_texture_id(c, e.image_asset.c_str());
            e.image_decode_status = e.texture_id >= 0 ? 1 : -1;
            continue;
        }
        if (e.kind == Kind::Flipbook)
        {
            bf6_rime_flipbook_info fi{};
            const int count = bf6_rime_flipbook(
                c, e.image_asset.c_str(), &fi, nullptr, 0);
            if (count < 0)
            {
                e.image_decode_status = count;
                continue;
            }
            std::vector<bf6_rime_flipbook_frame> frames((size_t)count);
            const int got = bf6_rime_flipbook(
                c, e.image_asset.c_str(), &fi, frames.data(), count);
            if (got != count || fi.frame_count != count || fi.texture_id < 0)
            {
                e.image_decode_status = -2;
                continue;
            }
            e.flipbook_atlas_asset = fi.atlas_asset;
            e.flipbook_frame_rate = fi.frame_rate;
            e.flipbook_frames.resize(frames.size());
            for (size_t i = 0; i < frames.size(); ++i)
                for (int component = 0; component < 4; ++component)
                    e.flipbook_frames[i].uv[component] =
                        frames[i].uv[component];
            e.texture_id = fi.texture_id;
            e.image_decode_status = 1;
            continue;
        }
        if (e.kind != Kind::Svg) continue;

        bf6_rime_svg_info si{};
        const int count = bf6_rime_svg(c, e.image_asset.c_str(), &si,
                                       nullptr, 0, nullptr, 0);
        if (count < 0)
        {
            e.image_decode_status = count;
            continue;
        }
        std::vector<bf6_rime_svg_contour> contours((size_t)si.contour_count);
        std::vector<bf6_rime_svg_point> points((size_t)si.point_count);
        const int got = bf6_rime_svg(c, e.image_asset.c_str(), &si,
                                     contours.data(), (int)contours.size(),
                                     points.data(), (int)points.size());
        if (got != count)
        {
            e.image_decode_status = -2;
            continue;
        }
        e.svg_canvas[0] = si.canvas[0];
        e.svg_canvas[1] = si.canvas[1];
        e.svg_shape_count = si.shape_count;
        e.svg_contours.resize(contours.size());
        for (size_t i = 0; i < contours.size(); ++i)
        {
            const bf6_rime_svg_contour& src = contours[i];
            Element::SvgContour& dst = e.svg_contours[i];
            dst.shape = src.shape;
            dst.point_first = src.point_first;
            dst.point_count = src.point_count;
            dst.flag = src.flag;
            for (int k = 0; k < 4; ++k) dst.bounds[k] = src.bounds[k];
        }
        e.svg_points.resize(points.size());
        for (size_t i = 0; i < points.size(); ++i)
            e.svg_points[i] = Element::SvgPoint{ points[i].x, points[i].y };
        e.image_decode_status = 1;
    }
}

int flipbook_frame_index(const Element& element, double seconds)
{
    const int count = (int)element.flipbook_frames.size();
    if (count <= 0) return -1;
    if (element.flipbook_auto_play != 1)
    {
        const double progress = (std::max)(0.0,
            (std::min)(1.0, (double)element.flipbook_progress));
        return (std::min)(count - 1, (int)(progress * count));
    }

    if (!(seconds > 0.0)) seconds = 0.0;
    const double frame_rate = element.flipbook_frame_rate > 0.f
        ? element.flipbook_frame_rate : 30.0;
    const uint64_t tick = (uint64_t)std::floor(seconds * frame_rate);
    if (element.flipbook_random_frames == 1)
    {
        // RandomFrames is a presentation choice, not nondeterministic state.
        // Hash the authored animation tick so every repaint within one tick
        // chooses the same cell and capture/test runs remain reproducible.
        uint64_t mixed = tick + 0x9E3779B97F4A7C15ull + element.name_hash;
        mixed ^= mixed >> 30;
        mixed *= 0xBF58476D1CE4E5B9ull;
        mixed ^= mixed >> 27;
        mixed *= 0x94D049BB133111EBull;
        mixed ^= mixed >> 31;
        return (int)(mixed % (uint64_t)count);
    }
    if (element.flipbook_loop == 1)
        return (int)(tick % (uint64_t)count);
    return (int)(std::min)(tick, (uint64_t)(count - 1));
}

int load_text_bindings(bf6_ctx* c, Screen& s, int* ambiguous)
{
    if (ambiguous) *ambiguous = 0;
    if (!c) return 0;

    std::set<std::string> partitions;
    for (const Element& e : s.elements)
        if (!e.partition.empty()) partitions.insert(e.partition);

    int bound = 0;
    for (const std::string& partition : partitions)
    {
        const int nc = bf6_rime_connections(c, partition.c_str(), nullptr, 0);
        if (nc <= 0) continue;
        std::vector<bf6_rime_connection> connections((size_t)nc);
        bf6_rime_connections(c, partition.c_str(), connections.data(), nc);

        std::map<int, std::string> value;
        std::set<int> conflict;
        auto seed = [&](int instance) {
            if (instance < 0 || value.count(instance) || conflict.count(instance)) return;
            const uint32_t sid = bf6_rime_string_entity(c, partition.c_str(), instance);
            if (!sid) return;
            const char* localized = bf6_localized_string(c, sid);
            if (localized && *localized) value.emplace(instance, localized);
        };
        for (const bf6_rime_connection& cn : connections)
        {
            seed(cn.source);
            seed(cn.target);
        }

        bool changed = true;
        for (int pass = 0; changed && pass <= nc; ++pass)
        {
            changed = false;
            for (const bf6_rime_connection& cn : connections)
            {
                const auto src = value.find(cn.source);
                if (src == value.end() || conflict.count(cn.source) || conflict.count(cn.target))
                    continue;
                const auto dst = value.find(cn.target);
                if (dst == value.end())
                {
                    value.emplace(cn.target, src->second);
                    changed = true;
                }
                else if (dst->second != src->second)
                {
                    value.erase(dst);
                    conflict.insert(cn.target);
                    changed = true;
                }
            }
        }

        for (Element& e : s.elements)
        {
            if (e.partition != partition || e.kind != Kind::Label || e.instance < 0)
                continue;
            if (conflict.count(e.instance))
            {
                if (ambiguous) ++*ambiguous;
                continue;
            }
            const auto it = value.find(e.instance);
            if (it == value.end()) continue;
            e.text = it->second;
            ++bound;
        }
    }

    /* Labels no connection feeds may still carry their own authored text:
     * RawText verbatim, or a symbolic StringId that the executable's
     * localisation service resolves by hashing the id (bf6_string_id_hash)
     * into the same table the numeric route uses.  An id the table lacks is
     * left empty on purpose - printing "ID_..." is not what the game does. */
    for (Element& e : s.elements)
    {
        if (e.kind != Kind::Label || !e.text.empty()) continue;
        if (!e.text_raw.empty()) { e.text = e.text_raw; ++bound; continue; }
        if (e.text_string_id.empty()) continue;
        const char* localized = bf6_localized_string_by_id(c, e.text_string_id.c_str());
        if (localized && *localized) { e.text = localized; ++bound; }
    }
    return bound;
}

int load_conditional_float_bindings(bf6_ctx* c, Screen& s)
{
    if (!c) return 0;
    s.conditional_float_bindings.clear();

    std::set<std::string> partitions;
    for (const Element& e : s.elements)
        if (!e.partition.empty()) partitions.insert(e.partition);

    static const uint32_t kCondition = 0x6b535a76u;
    static const uint32_t kOutput = 0xac996e7au;
    int compiled = 0;
    for (const std::string& partition : partitions)
    {
        const int nn = bf6_rime_conditional_floats(c, partition.c_str(), nullptr, 0);
        const int nc = bf6_rime_connections(c, partition.c_str(), nullptr, 0);
        if (nn <= 0 || nc <= 0) continue;

        std::vector<bf6_rime_conditional_float> nodes((size_t)nn);
        std::vector<bf6_rime_connection> connections((size_t)nc);
        bf6_rime_conditional_floats(c, partition.c_str(), nodes.data(), nn);
        bf6_rime_connections(c, partition.c_str(), connections.data(), nc);

        for (const bf6_rime_conditional_float& node : nodes)
        {
            int condition_instance = -1;
            uint32_t condition_field = 0;
            bool condition_conflict = false;
            for (const bf6_rime_connection& cn : connections)
                if (cn.target == node.instance && cn.target_field == kCondition)
                {
                    if (condition_instance >= 0 &&
                        (condition_instance != cn.source ||
                         condition_field != cn.source_field))
                        condition_conflict = true;
                    condition_instance = cn.source;
                    condition_field = cn.source_field;
                }
            if (condition_conflict) continue;

            for (const bf6_rime_connection& cn : connections)
            {
                if (cn.source != node.instance || cn.source_field != kOutput)
                    continue;
                Screen::ConditionalFloatBinding binding;
                binding.partition = partition;
                binding.condition_instance = condition_instance;
                binding.condition_field = condition_field;
                binding.target_instance = cn.target;
                binding.target_field = cn.target_field;
                binding.value_if_true = node.value_if_true;
                binding.value_if_false = node.value_if_false;
                binding.authored_condition = node.authored_condition != 0;
                s.conditional_float_bindings.push_back(std::move(binding));
                ++compiled;
            }
        }
    }
    return compiled;
}

int apply_conditional_float_bindings(Screen& s)
{
    static const uint32_t kIsFocused = 0xcbec4452u;
    static const uint32_t kWidth = 0x0d877543u;
    static const uint32_t kHeight = 0xb60957dau;
    int applied = 0;
    for (const Screen::ConditionalFloatBinding& binding :
         s.conditional_float_bindings)
    {
        bool condition = binding.authored_condition;
        bool condition_known = binding.condition_instance < 0;
        if (binding.condition_instance >= 0 &&
            binding.condition_field == kIsFocused)
        {
            for (const Element& source : s.elements)
                if (source.partition == binding.partition &&
                    source.instance == binding.condition_instance)
                {
                    condition = source.runtime_state >= 2;
                    condition_known = true;
                    break;
                }
        }
        if (!condition_known) continue;

        const float value = condition ? binding.value_if_true
                                      : binding.value_if_false;
        for (Element& target : s.elements)
        {
            if (target.partition != binding.partition ||
                target.instance != binding.target_instance)
                continue;
            if (binding.target_field == kWidth)
            {
                target.width = value;
                if (target.kind == Kind::VectorShape) target.shape_size[0] = value;
                ++applied;
            }
            else if (binding.target_field == kHeight)
            {
                target.height = value;
                if (target.kind == Kind::VectorShape) target.shape_size[1] = value;
                ++applied;
            }
        }
    }
    return applied;
}

uint32_t property_hash(const char* property_name)
{
    uint32_t hash = 5381u;
    if (!property_name) return hash;
    for (const unsigned char* p =
             reinterpret_cast<const unsigned char*>(property_name); *p; ++p)
        hash = hash * 33u ^ *p;
    return hash;
}

int load_interface_text_graphs(bf6_ctx* c, Screen& s)
{
    s.interface_text_graphs.clear();
    if (!c) return 0;

    auto normalized = [](std::string value) {
        for (char& ch : value)
            if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        if (value.size() > 4 &&
            value.compare(value.size() - 4, 4, ".ebx") == 0)
            value.resize(value.size() - 4);
        return value;
    };

    /* A partition can occur many times in one expanded screen (list cells are
     * the obvious case).  Frostbite instance ids restart in every partition,
     * so (partition, instance) is not enough to identify a live element.  The
     * nearest ancestor WidgetReference whose target is this partition is the
     * exact authored occurrence scope. */
    std::map<std::string, std::set<int>> partitionScopes;
    for (size_t i = 0; i < s.elements.size(); ++i)
    {
        Element& element = s.elements[i];
        element.scope = -1;
        if (element.partition.empty()) continue;
        const std::string partition = normalized(element.partition);
        int parent = element.parent;
        std::set<int> visited;
        while (parent >= 0 && (size_t)parent < s.elements.size() &&
               visited.insert(parent).second)
        {
            const Element& ancestor = s.elements[(size_t)parent];
            if (ancestor.kind == Kind::WidgetReference &&
                !ancestor.references_widget.empty() &&
                normalized(ancestor.references_widget) == partition)
            {
                element.scope = parent;
                break;
            }
            parent = ancestor.parent;
        }
        partitionScopes[element.partition].insert(element.scope);
    }

    int compiled = 0;
    for (const auto& partitionEntry : partitionScopes)
    {
        const std::string& partition = partitionEntry.first;
        const int ni = bf6_rime_interface_descriptors(
            c, partition.c_str(), nullptr, 0);
        const int nc = bf6_rime_connections(c, partition.c_str(), nullptr, 0);
        /* DBD-fed cells can have provider connections without exposing a
         * public InterfaceDescriptor of their own.  Those graphs are still
         * executable: the provider source instance is selected from the
         * exact connection endpoint. */
        if (nc <= 0) continue;

        Screen::InterfaceTextGraph baseGraph;
        baseGraph.partition = partition;
        if (ni > 0) baseGraph.interfaces.resize((size_t)ni);
        baseGraph.connections.resize((size_t)nc);
        if (ni > 0)
            bf6_rime_interface_descriptors(c, partition.c_str(),
                                           baseGraph.interfaces.data(), ni);
        bf6_rime_connections(c, partition.c_str(),
                             baseGraph.connections.data(), nc);

        std::map<int32_t, std::vector<uint32_t>> stringArgumentFields;
        const int nsa = bf6_rime_string_arguments(
            c, partition.c_str(), nullptr, 0);
        if (nsa > 0)
        {
            std::vector<bf6_rime_string_argument> rows((size_t)nsa);
            const int got = bf6_rime_string_arguments(
                c, partition.c_str(), rows.data(), nsa);
            for (int i = 0; i < got; ++i)
            {
                const bf6_rime_string_argument& row = rows[(size_t)i];
                if (row.index < 0 || !row.field_id) continue;
                std::vector<uint32_t>& fields =
                    stringArgumentFields[row.instance];
                if (fields.size() <= (size_t)row.index)
                    fields.resize((size_t)row.index + 1);
                fields[(size_t)row.index] = row.field_id;
            }
        }

        /* LocalizedStringEntity is an authored graph source, including when
         * its destination is a WidgetReference rather than a local label.
         * Seed the exact observed output field here so the ordinary prefab
         * bridge can carry strings such as the boot-flow input prompt through
         * the referenced input-action widget.  The earlier label-only walker
         * stopped at this boundary and therefore reported an empty prompt
         * even though the current install contained the selected string. */
        std::set<std::pair<int32_t, uint32_t>> localizedSeeds;
        for (const bf6_rime_connection& connection : baseGraph.connections)
        {
            const std::pair<int32_t, uint32_t> key{
                connection.source, connection.source_field};
            if (!localizedSeeds.insert(key).second) continue;
            const uint32_t sid = bf6_rime_string_entity(
                c, partition.c_str(), connection.source);
            if (!sid) continue;
            const char* localized = bf6_localized_string(c, sid);
            if (!localized || !*localized) continue;
            const auto argumentFields =
                stringArgumentFields.find(connection.source);
            if (argumentFields != stringArgumentFields.end() &&
                !argumentFields->second.empty() &&
                std::all_of(argumentFields->second.begin(),
                            argumentFields->second.end(),
                            [](uint32_t field) { return field != 0; }))
            {
                Screen::InterfaceTextGraph::LocalizedFormat format;
                format.instance = connection.source;
                format.output_field = connection.source_field;
                format.pattern = localized;
                format.argument_fields = argumentFields->second;
                baseGraph.localized_formats.push_back(std::move(format));
                continue;
            }
            Screen::InterfaceTextGraph::Input input;
            input.source_instance = connection.source;
            input.field = connection.source_field;
            input.kind = BF6_RIME_VALUE_STRING;
            input.string = localized;
            baseGraph.inputs.push_back(std::move(input));
        }

        const int nd = bf6_rime_interface_fields(
            c, partition.c_str(), nullptr, 0);
        if (nd > 0)
        {
            baseGraph.defaults.resize((size_t)nd);
            bf6_rime_interface_fields(c, partition.c_str(),
                                      baseGraph.defaults.data(), nd);
        }

        const int np = bf6_rime_conditional_properties(
            c, partition.c_str(), nullptr, 0);
        if (np > 0)
        {
            baseGraph.conditionals.resize((size_t)np);
            bf6_rime_conditional_properties(c, partition.c_str(),
                                             baseGraph.conditionals.data(), np);
        }
        const int nf = bf6_rime_conditional_floats(
            c, partition.c_str(), nullptr, 0);
        if (nf > 0)
        {
            baseGraph.float_conditionals.resize((size_t)nf);
            bf6_rime_conditional_floats(c, partition.c_str(),
                                        baseGraph.float_conditionals.data(), nf);
        }
        const int nfi = bf6_rime_float_interpolators(
            c, partition.c_str(), nullptr, 0);
        if (nfi > 0)
        {
            baseGraph.float_interpolators.resize((size_t)nfi);
            bf6_rime_float_interpolators(
                c, partition.c_str(), baseGraph.float_interpolators.data(), nfi);
        }
        const int nl = bf6_rime_logic_operations(
            c, partition.c_str(), nullptr, 0);
        if (nl > 0)
        {
            baseGraph.logic_operations.resize((size_t)nl);
            bf6_rime_logic_operations(c, partition.c_str(),
                                      baseGraph.logic_operations.data(), nl);
        }
        const int nps = bf6_rime_property_statuses(
            c, partition.c_str(), nullptr, 0);
        if (nps > 0)
        {
            baseGraph.property_statuses.resize((size_t)nps);
            bf6_rime_property_statuses(
                c, partition.c_str(), baseGraph.property_statuses.data(), nps);
        }
        const int nmi = bf6_rime_math_instructions(
            c, partition.c_str(), nullptr, 0);
        if (nmi > 0)
        {
            baseGraph.math_instructions.resize((size_t)nmi);
            bf6_rime_math_instructions(
                c, partition.c_str(), baseGraph.math_instructions.data(), nmi);
        }
        const int nmo = bf6_rime_math_operations(
            c, partition.c_str(), nullptr, 0);
        if (nmo > 0)
        {
            baseGraph.math_operations.resize((size_t)nmo);
            bf6_rime_math_operations(
                c, partition.c_str(), baseGraph.math_operations.data(), nmo);
        }
        const int nro = bf6_rime_roundings(
            c, partition.c_str(), nullptr, 0);
        if (nro > 0)
        {
            baseGraph.roundings.resize((size_t)nro);
            bf6_rime_roundings(
                c, partition.c_str(), baseGraph.roundings.data(), nro);
        }
        const int npc = bf6_rime_property_casts(
            c, partition.c_str(), nullptr, 0);
        if (npc > 0)
        {
            baseGraph.property_casts.resize((size_t)npc);
            bf6_rime_property_casts(
                c, partition.c_str(), baseGraph.property_casts.data(), npc);
        }
        const int ncf = bf6_rime_compare_floats(
            c, partition.c_str(), nullptr, 0);
        if (ncf > 0)
        {
            baseGraph.compare_floats.resize((size_t)ncf);
            bf6_rime_compare_floats(
                c, partition.c_str(), baseGraph.compare_floats.data(), ncf);
        }
        const int na = bf6_rime_array_elements(
            c, partition.c_str(), nullptr, 0);
        if (na > 0)
        {
            baseGraph.array_elements.resize((size_t)na);
            bf6_rime_array_elements(c, partition.c_str(),
                                    baseGraph.array_elements.data(), na);
        }
        const int nr = bf6_rime_logic_references(
            c, partition.c_str(), nullptr, 0);
        if (nr > 0)
        {
            baseGraph.logic_references.resize((size_t)nr);
            bf6_rime_logic_references(c, partition.c_str(),
                                      baseGraph.logic_references.data(), nr);
        }
        compiled += nc;
        for (int scope : partitionEntry.second)
        {
            Screen::InterfaceTextGraph graph = baseGraph;
            graph.scope = scope;
            s.interface_text_graphs.push_back(std::move(graph));
        }
    }
    return compiled;
}

namespace {

struct RuntimePropertyValue {
    int kind = BF6_RIME_VALUE_NULL;
    bool boolean = false;
    int64_t integer = 0;
    uint64_t unsigned_integer = 0;
    double real = 0.0;
    std::string string;
    std::vector<std::string> string_array;
    std::vector<int64_t> text_format_integers;
    std::vector<TextFormatArgument> text_format_arguments;

    bool operator<(const RuntimePropertyValue& rhs) const
    {
        if (kind != rhs.kind) return kind < rhs.kind;
        if (boolean != rhs.boolean) return boolean < rhs.boolean;
        if (integer != rhs.integer) return integer < rhs.integer;
        if (unsigned_integer != rhs.unsigned_integer)
            return unsigned_integer < rhs.unsigned_integer;
        if (real != rhs.real) return real < rhs.real;
        if (string != rhs.string) return string < rhs.string;
        if (string_array != rhs.string_array)
            return string_array < rhs.string_array;
        if (text_format_integers != rhs.text_format_integers)
            return text_format_integers < rhs.text_format_integers;
        return text_format_arguments < rhs.text_format_arguments;
    }
};

RuntimePropertyValue runtime_value(const bf6_rime_interface_field& field)
{
    RuntimePropertyValue value;
    value.kind = field.value_kind;
    value.boolean = field.bool_value != 0;
    value.integer = field.int_value;
    value.unsigned_integer = field.uint_value;
    value.real = field.real_value;
    value.string = field.string_value;
    return value;
}

RuntimePropertyValue runtime_value(const Screen::InterfaceTextGraph::Input& input)
{
    RuntimePropertyValue value;
    value.kind = input.kind;
    value.boolean = input.boolean;
    value.integer = input.integer;
    value.unsigned_integer = input.unsigned_integer;
    value.real = input.real;
    value.string = input.string;
    value.string_array = input.string_array;
    value.text_format_integers = input.text_format_integers;
    value.text_format_arguments = input.text_format_arguments;
    return value;
}

bool runtime_bool(const RuntimePropertyValue& value, bool& out)
{
    if (value.kind == BF6_RIME_VALUE_BOOL) { out = value.boolean; return true; }
    if (value.kind == BF6_RIME_VALUE_INT) { out = value.integer != 0; return true; }
    if (value.kind == BF6_RIME_VALUE_UINT)
    { out = value.unsigned_integer != 0; return true; }
    if (value.kind == BF6_RIME_VALUE_REAL) { out = value.real != 0.0; return true; }
    return false;
}

bool runtime_real(const RuntimePropertyValue& value, float& out)
{
    if (value.kind == BF6_RIME_VALUE_REAL) { out = (float)value.real; return true; }
    if (value.kind == BF6_RIME_VALUE_INT) { out = (float)value.integer; return true; }
    if (value.kind == BF6_RIME_VALUE_UINT)
    { out = (float)value.unsigned_integer; return true; }
    return false;
}

using RuntimeKey = std::pair<int32_t, uint32_t>;
using ResolvedRuntimeValue = std::pair<RuntimeKey, RuntimePropertyValue>;

bool checked_math_add(int64_t a, int64_t b, int64_t& out)
{
    if ((b > 0 && a > (std::numeric_limits<int64_t>::max)() - b) ||
        (b < 0 && a < (std::numeric_limits<int64_t>::min)() - b))
        return false;
    out = a + b;
    return true;
}

bool checked_math_sub(int64_t a, int64_t b, int64_t& out)
{
    if ((b < 0 && a > (std::numeric_limits<int64_t>::max)() + b) ||
        (b > 0 && a < (std::numeric_limits<int64_t>::min)() + b))
        return false;
    out = a - b;
    return true;
}

bool checked_math_mul(int64_t a, int64_t b, int64_t& out)
{
    if (!a || !b) { out = 0; return true; }
    if ((a == -1 && b == (std::numeric_limits<int64_t>::min)()) ||
        (b == -1 && a == (std::numeric_limits<int64_t>::min)()))
        return false;
    if (a > 0)
    {
        if ((b > 0 && a > (std::numeric_limits<int64_t>::max)() / b) ||
            (b < 0 && b < (std::numeric_limits<int64_t>::min)() / a))
            return false;
    }
    else
    {
        if ((b > 0 && a < (std::numeric_limits<int64_t>::min)() / b) ||
            (b < 0 && a < (std::numeric_limits<int64_t>::max)() / b))
            return false;
    }
    out = a * b;
    return true;
}

/* Execute one authored MathEntityData program.  Register files are separate,
 * exactly as the opcode suffixes specify.  Unsupported vector/transform/Func
 * instructions decline the complete node; no partial result is published. */
bool evaluate_math_entity(
    const Screen::InterfaceTextGraph& graph,
    const std::map<RuntimeKey, std::set<RuntimePropertyValue>>& candidates,
    std::vector<bf6_rime_math_instruction>::const_iterator first,
    std::vector<bf6_rime_math_instruction>::const_iterator last,
    RuntimePropertyValue& returned)
{
    if (first == last) return false;
    const int32_t instance = first->instance;
    std::map<int32_t, bool> bools;
    std::map<int32_t, int64_t> integers;
    std::map<int32_t, double> reals;
    std::map<int32_t, RuntimePropertyValue> produced;

    auto input = [&](uint32_t field, RuntimePropertyValue& value) {
        bool wired = false;
        bool everyProducerKnown = true;
        std::set<RuntimePropertyValue> runtimeValues;
        auto isRuntimeSource = [&](int32_t source, uint32_t sourceField) {
            for (const Screen::InterfaceTextGraph::Input& runtime :
                 graph.inputs)
            {
                if (runtime.field != sourceField) continue;
                if (runtime.source_instance == source) return true;
                if (runtime.source_instance < 0 &&
                    std::find(graph.interfaces.begin(), graph.interfaces.end(),
                              source) != graph.interfaces.end())
                    return true;
            }
            return false;
        };
        for (const bf6_rime_connection& connection : graph.connections)
        {
            if (connection.target != instance ||
                connection.target_field != field)
                continue;
            wired = true;
            const auto producer = candidates.find(RuntimeKey{
                connection.source, connection.source_field});
            if (producer == candidates.end() || producer->second.size() != 1)
            {
                everyProducerKnown = false;
                continue;
            }
            if (isRuntimeSource(connection.source, connection.source_field))
                runtimeValues.insert(*producer->second.begin());
        }
        if (!wired) return false;
        /* Property connections are update transactions.  The interface's
         * authored zero initializer reaches these pins during construction;
         * a supplied runtime field is a later writer and replaces it.  When
         * more than one runtime endpoint writes a different value the result
         * remains ambiguous and the complete math node is declined. */
        if (!runtimeValues.empty())
        {
            if (runtimeValues.size() != 1) return false;
            value = *runtimeValues.begin();
            return true;
        }
        if (!everyProducerKnown) return false;
        const auto found = candidates.find(RuntimeKey{instance, field});
        if (found == candidates.end() || found->second.size() != 1)
            return false;
        value = *found->second.begin();
        return true;
    };
    auto write_bool = [&](int32_t instruction, int32_t slot, bool value) {
        bools[slot] = value;
        RuntimePropertyValue runtime;
        runtime.kind = BF6_RIME_VALUE_BOOL;
        runtime.boolean = value;
        produced[instruction] = runtime;
    };
    auto write_int = [&](int32_t instruction, int32_t slot, int64_t value) {
        integers[slot] = value;
        RuntimePropertyValue runtime;
        runtime.kind = BF6_RIME_VALUE_INT;
        runtime.integer = value;
        produced[instruction] = runtime;
    };
    auto write_real = [&](int32_t instruction, int32_t slot, double value) {
        if (!std::isfinite(value)) return false;
        reals[slot] = value;
        RuntimePropertyValue runtime;
        runtime.kind = BF6_RIME_VALUE_REAL;
        runtime.real = value;
        produced[instruction] = runtime;
        return true;
    };

    int32_t expected = 0;
    for (auto it = first; it != last; ++it, ++expected)
    {
        const bf6_rime_math_instruction& instruction = *it;
        if (instruction.instance != instance ||
            instruction.instruction_index != expected ||
            instruction.result < 0)
            return false;
        const int32_t p1 = instruction.param1;
        const int32_t p2 = instruction.param2;
        const int32_t result = instruction.result;
        const int32_t index = instruction.instruction_index;
        RuntimePropertyValue pin;
        int64_t ia = 0, ib = 0, ir = 0;
        double fa = 0.0, fb = 0.0;
        bool ba = false, bb = false;

        switch (instruction.code)
        {
        case 0: write_bool(index, result, p1 != 0); break;       // ConstB
        case 1: write_int(index, result, p1); break;             // ConstI
        case 2:                                                     // ConstF
        {
            const uint32_t bits = (uint32_t)p1;
            float value = 0.f;
            std::memcpy(&value, &bits, sizeof(value));
            if (!write_real(index, result, value)) return false;
            break;
        }
        case 3:                                                     // InputB
            if (!input((uint32_t)p1, pin) ||
                pin.kind != BF6_RIME_VALUE_BOOL)
                return false;
            write_bool(index, result, pin.boolean);
            break;
        case 4:                                                     // InputI
            if (!input((uint32_t)p1, pin) ||
                pin.kind != BF6_RIME_VALUE_INT)
                return false;
            write_int(index, result, pin.integer);
            break;
        case 5:                                                     // InputF
            if (!input((uint32_t)p1, pin) ||
                pin.kind != BF6_RIME_VALUE_REAL ||
                !write_real(index, result, pin.real))
                return false;
            break;
        case 10: case 11:                                          // OrB/AndB
            if (!bools.count(p1) || !bools.count(p2)) return false;
            write_bool(index, result, instruction.code == 10
                ? bools[p1] || bools[p2] : bools[p1] && bools[p2]);
            break;
        case 12: case 14: case 16: case 18: case 20: case 23:
            if (!integers.count(p1) || !integers.count(p2)) return false;
            ia = integers[p1]; ib = integers[p2];
            if (instruction.code == 12) ba = ia > ib;
            else if (instruction.code == 14) ba = ia >= ib;
            else if (instruction.code == 16) ba = ia < ib;
            else if (instruction.code == 18) ba = ia <= ib;
            else if (instruction.code == 20) ba = ia != ib;
            else ba = ia == ib;
            write_bool(index, result, ba);
            break;
        case 13: case 15: case 17: case 19: case 21: case 24:
            if (!reals.count(p1) || !reals.count(p2)) return false;
            fa = reals[p1]; fb = reals[p2];
            if (instruction.code == 13) ba = fa > fb;
            else if (instruction.code == 15) ba = fa >= fb;
            else if (instruction.code == 17) ba = fa < fb;
            else if (instruction.code == 19) ba = fa <= fb;
            else if (instruction.code == 21) ba = fa != fb;
            else ba = fa == fb;
            write_bool(index, result, ba);
            break;
        case 22: case 25:                                          // NotEqB/EqB
            if (!bools.count(p1) || !bools.count(p2)) return false;
            ba = instruction.code == 22
                ? bools[p1] != bools[p2] : bools[p1] == bools[p2];
            write_bool(index, result, ba);
            break;
        case 26: case 31: case 37:                                 // integer arithmetic
            if (!integers.count(p1) || !integers.count(p2)) return false;
            ia = integers[p1]; ib = integers[p2];
            if (instruction.code == 26 && !checked_math_add(ia, ib, ir))
                return false;
            if (instruction.code == 31 && !checked_math_sub(ia, ib, ir))
                return false;
            if (instruction.code == 37 && !checked_math_mul(ia, ib, ir))
                return false;
            write_int(index, result, ir);
            break;
        case 27: case 32: case 36:                                 // float arithmetic
            if (!reals.count(p1) || !reals.count(p2)) return false;
            fa = reals[p1]; fb = reals[p2];
            if (!write_real(index, result,
                    instruction.code == 27 ? fa + fb :
                    instruction.code == 32 ? fa - fb : fa * fb))
                return false;
            break;
        case 45:                                                    // DivI
            if (!integers.count(p1) || !integers.count(p2)) return false;
            ia = integers[p1]; ib = integers[p2];
            if (!ib || (ia == (std::numeric_limits<int64_t>::min)() &&
                        ib == -1))
                return false;
            write_int(index, result, ia / ib);
            break;
        case 46:                                                    // DivF
            if (!reals.count(p1) || !reals.count(p2) || reals[p2] == 0.0 ||
                !write_real(index, result, reals[p1] / reals[p2]))
                return false;
            break;
        case 53:                                                    // ModI
            if (!integers.count(p1) || !integers.count(p2)) return false;
            ia = integers[p1]; ib = integers[p2];
            if (!ib || (ia == (std::numeric_limits<int64_t>::min)() &&
                        ib == -1))
                return false;
            write_int(index, result, ia % ib);
            break;
        case 54:                                                    // ModF
            if (!reals.count(p1) || !reals.count(p2) || reals[p2] == 0.0 ||
                !write_real(index, result, std::fmod(reals[p1], reals[p2])))
                return false;
            break;
        case 55:                                                    // NegI
            if (!integers.count(p1) ||
                integers[p1] == (std::numeric_limits<int64_t>::min)())
                return false;
            write_int(index, result, -integers[p1]);
            break;
        case 56:                                                    // NegF
            if (!reals.count(p1) ||
                !write_real(index, result, -reals[p1]))
                return false;
            break;
        case 60:                                                    // NotB
            if (!bools.count(p1)) return false;
            write_bool(index, result, !bools[p1]);
            break;
        case 61:                                                    // PowI
            if (!integers.count(p1) || !integers.count(p2) ||
                integers[p2] < 0 || integers[p2] > 63)
                return false;
            ia = integers[p1]; ib = integers[p2]; ir = 1;
            while (ib-- > 0)
                if (!checked_math_mul(ir, ia, ir)) return false;
            write_int(index, result, ir);
            break;
        case 62:                                                    // PowF
            if (!reals.count(p1) || !reals.count(p2) ||
                !write_real(index, result,
                            std::pow(reals[p1], reals[p2])))
                return false;
            break;
        case 68:                                                    // Return
        {
            const auto value = produced.find(p1);
            if (value == produced.end()) return false;
            returned = value->second;
            return true;
        }
        default:
            return false;
        }
    }
    return false;
}

int apply_interface_graph(Screen& s, Screen::InterfaceTextGraph& graph,
                          int* ambiguous,
                          std::vector<ResolvedRuntimeValue>* resolved = nullptr,
                          bool include_all_resolved = false)
{
    if (ambiguous) *ambiguous = 0;
    static const uint32_t kCondition = property_hash("Condition");
    static const uint32_t kOutput = property_hash("Output");
    static const uint32_t kVisible = property_hash("Visible");
    static const uint32_t kAlpha = property_hash("Alpha");
    static const uint32_t kWidth = property_hash("Width");
    static const uint32_t kHeight = property_hash("Height");
    static const uint32_t kProgress = property_hash("Progress");
    static const uint32_t kStartProgress = property_hash("StartProgress");
    static const uint32_t kEndProgress = property_hash("EndProgress");
    static const uint32_t kGlowSize = property_hash("GlowSize");
    static const uint32_t kLocalizedText = property_hash("LocalizedText");
    static const uint32_t kRawText = property_hash("RawText");
    static const uint32_t kStringId = property_hash("StringId");
    static const uint32_t kText = property_hash("Text");
    static const uint32_t kInValue = 0x62da2189u;
    static const uint32_t kDefaultValue = 0x7b256865u;
    static const uint32_t kInPins[17] = {
        0x0b87d372u, 0x0b87d373u, 0x0b87d370u, 0x0b87d371u,
        0x0b87d376u, 0x0b87d377u, 0x0b87d374u, 0x0b87d375u,
        0x0b87d37au, 0x0b87d37bu, 0x7c8241e3u, 0x7c8241e2u,
        0x7c8241e1u, 0x7c8241e0u, 0x7c8241e7u, 0x7c8241e6u,
        0x7c8241e5u
    };
    static const uint32_t kOut = 0x0b87df4bu;
    static const uint32_t kFloatIn = 0x7f39a652u;
    static const uint32_t kIntOut = 0xa5f78658u;
    static const uint32_t kFloatOut = 0x666e571bu;
    static const uint32_t kCastInputs[] = {
        0x0056c23du, 0xba31493eu, 0x0a7e28e0u,
        0xef81c8e8u, 0x4bb0cd9bu, 0xaeabcb6au
    };
    static const uint32_t kCastToInt = 0x0ea01568u;
    static const uint32_t kCastToUint = 0xe2a032ddu;
    static const uint32_t kCastToFloat = 0x37d7840bu;
    static const uint32_t kCastToDouble = 0x401a44ceu;
    static const uint32_t kCastToBool = 0xe2a75c55u;
    static const uint32_t kCastToString = 0x0885f34eu;
    static const uint32_t kCompareA = 0x0002b5e4u;
    static const uint32_t kCompareB = 0x0002b5e7u;
    static const uint32_t kCompareEq = 0x0b87bd3bu;
    static const uint32_t kCompareNotEq = 0x7c7f159au;
    static const uint32_t kCompareGreaterEq = 0x7c7f72c5u;
    static const uint32_t kCompareLessEq = 0x7c7f6a47u;
    static const uint32_t kCompareGreater = 0x0b87bdd8u;
    static const uint32_t kCompareLess = 0x0b87bd1au;

    std::map<RuntimeKey, std::set<RuntimePropertyValue>> candidates;
    std::map<RuntimeKey, std::set<RuntimePropertyValue>> runtimeInputs;
    auto add = [&](const RuntimeKey& key, const RuntimePropertyValue& value) {
        return candidates[key].insert(value).second;
    };
    auto has_runtime_input = [&](int32_t instance, uint32_t field) {
        for (const Screen::InterfaceTextGraph::Input& input : graph.inputs)
        {
            if (input.field != field) continue;
            if (input.source_instance == instance) return true;
            if (input.source_instance < 0 &&
                std::find(graph.interfaces.begin(), graph.interfaces.end(),
                          instance) != graph.interfaces.end())
                return true;
        }
        return false;
    };
    for (const bf6_rime_interface_field& field : graph.defaults)
    {
        if (field.value_kind == BF6_RIME_VALUE_NULL ||
            field.value_kind == BF6_RIME_VALUE_UNRESOLVED ||
            field.value_kind == BF6_RIME_VALUE_STRUCT ||
            field.value_kind == BF6_RIME_VALUE_ARRAY ||
            /* A DataField.BoxedValue is the value used only when no runtime
             * input is present. Keeping both values in the candidate set
             * made a perfectly valid false override of cl_tag's authored
             * true IconVisibility look ambiguous and rolled the transaction
             * back. */
            has_runtime_input(field.interface_instance, field.field_id))
            continue;
        add(RuntimeKey{field.interface_instance, field.field_id},
            runtime_value(field));
    }
    for (const Screen::InterfaceTextGraph::Input& input : graph.inputs)
    {
        if (input.source_instance >= 0)
        {
            const RuntimeKey key{input.source_instance, input.field};
            const RuntimePropertyValue value = runtime_value(input);
            add(key, value);
            runtimeInputs[key].insert(value);
        }
        else
            for (int32_t interfaceInstance : graph.interfaces)
            {
                const RuntimeKey key{interfaceInstance, input.field};
                const RuntimePropertyValue value = runtime_value(input);
                add(key, value);
                runtimeInputs[key].insert(value);
            }
    }

    /* Rime cells commonly author an InterfaceDescriptor fallback and connect
     * a DBD provider to the same WidgetReference pin.  The provider is a
     * runtime assignment, so it replaces that fallback; treating both as
     * simultaneous writers makes valid supplied values ambiguous.  Preserve
     * all distinct runtime writers so genuinely conflicting providers still
     * fail closed. */
    std::map<RuntimeKey, std::set<RuntimePropertyValue>> runtimeOverrides;
    for (const bf6_rime_connection& connection : graph.connections)
    {
        const auto runtime = runtimeInputs.find(
            RuntimeKey{connection.source, connection.source_field});
        if (runtime == runtimeInputs.end()) continue;
        std::set<RuntimePropertyValue>& target = runtimeOverrides[
            RuntimeKey{connection.target, connection.target_field}];
        target.insert(runtime->second.begin(), runtime->second.end());
    }
    for (const auto& overrideValue : runtimeOverrides)
        candidates[overrideValue.first] = overrideValue.second;

    const int passLimit = (int)graph.connections.size() +
                          (int)graph.float_conditionals.size() +
                          (int)graph.float_interpolators.size() +
                          (int)graph.conditionals.size() +
                          (int)graph.logic_operations.size() +
                          (int)graph.property_statuses.size() +
                          (int)graph.math_instructions.size() +
                          (int)graph.math_operations.size() +
                          (int)graph.roundings.size() +
                          (int)graph.property_casts.size() +
                          (int)graph.compare_floats.size() +
                          (int)graph.localized_formats.size() +
                          (int)graph.array_elements.size() + 2;
    for (int pass = 0; pass < passLimit; ++pass)
    {
        bool changed = false;
        for (const bf6_rime_connection& connection : graph.connections)
        {
            const RuntimeKey targetKey{connection.target,
                                       connection.target_field};
            if (runtimeOverrides.count(targetKey)) continue;
            const auto source = candidates.find(
                RuntimeKey{connection.source, connection.source_field});
            if (source == candidates.end()) continue;
            for (const RuntimePropertyValue& value : source->second)
                changed |= add(targetKey, value);
        }
        for (const bf6_rime_property_status& node : graph.property_statuses)
        {
            bool known = false;
            bool hasValue = false;
            for (const bf6_rime_connection& connection : graph.connections)
            {
                if (connection.target != node.instance ||
                    connection.target_field != node.input_field)
                    continue;
                const RuntimeKey sourceKey{connection.source,
                                           connection.source_field};
                const auto source = candidates.find(sourceKey);
                if (source != candidates.end() && source->second.size() == 1)
                {
                    known = true;
                    const int kind = source->second.begin()->kind;
                    hasValue |= kind != BF6_RIME_VALUE_NULL &&
                                kind != BF6_RIME_VALUE_UNRESOLVED;
                    continue;
                }
                for (const bf6_rime_interface_field& field : graph.defaults)
                    if (field.interface_instance == connection.source &&
                        field.field_id == connection.source_field)
                    {
                        known = true;
                        hasValue |= field.value_kind != BF6_RIME_VALUE_NULL &&
                                    field.value_kind !=
                                        BF6_RIME_VALUE_UNRESOLVED;
                        break;
                    }
            }
            if (!known) continue;
            RuntimePropertyValue output;
            output.kind = BF6_RIME_VALUE_BOOL;
            output.boolean = hasValue;
            for (const bf6_rime_connection& connection : graph.connections)
                if (connection.source == node.instance)
                    changed |= add(RuntimeKey{node.instance,
                                               connection.source_field},
                                   output);
        }
        for (auto first = graph.math_instructions.cbegin();
             first != graph.math_instructions.cend();)
        {
            auto last = first + 1;
            while (last != graph.math_instructions.cend() &&
                   last->instance == first->instance)
                ++last;
            RuntimePropertyValue output;
            if (evaluate_math_entity(graph, candidates, first, last, output))
                for (const bf6_rime_connection& connection : graph.connections)
                    if (connection.source == first->instance)
                        changed |= add(RuntimeKey{first->instance,
                                                   connection.source_field},
                                       output);
            first = last;
        }
        for (const bf6_rime_math_operation& node : graph.math_operations)
        {
            if (node.operator_count <= 0 || node.operator_count > 16)
                continue;
            double accumulator = 0.0;
            bool complete = true;
            for (int input = 0; input <= node.operator_count; ++input)
            {
                const auto found = candidates.find(
                    RuntimeKey{node.instance, kInPins[input]});
                if (found == candidates.end() || found->second.size() != 1)
                { complete = false; break; }
                float scalar = 0.f;
                if (!runtime_real(*found->second.begin(), scalar))
                { complete = false; break; }
                const double value = scalar;
                if (!input) { accumulator = value; continue; }
                switch (node.operators[input - 1])
                {
                case 0: accumulator += value; break;
                case 1: accumulator -= value; break;
                case 2: accumulator *= value; break;
                case 3:
                    if (value == 0.0) complete = false;
                    else accumulator /= value;
                    break;
                case 4: accumulator = (std::min)(accumulator, value); break;
                case 5: accumulator = (std::max)(accumulator, value); break;
                case 6:
                    if (value == 0.0) complete = false;
                    else accumulator = std::fmod(accumulator, value);
                    break;
                case 7: accumulator = std::pow(accumulator, value); break;
                default: complete = false; break;
                }
                if (!complete || !std::isfinite(accumulator))
                { complete = false; break; }
            }
            if (!complete) continue;
            RuntimePropertyValue output;
            output.kind = BF6_RIME_VALUE_REAL;
            output.real = accumulator;
            changed |= add(RuntimeKey{node.instance, kOut}, output);
        }
        for (const bf6_rime_rounding& node : graph.roundings)
        {
            const auto found = candidates.find(
                RuntimeKey{node.instance, kFloatIn});
            if (found == candidates.end() || found->second.size() != 1)
                continue;
            float source = 0.f;
            if (!runtime_real(*found->second.begin(), source) ||
                !std::isfinite(source))
                continue;
            double rounded = 0.0;
            if (node.rounding_type == 1) rounded = std::ceil(source);
            else if (node.rounding_type == 2) rounded = std::floor(source);
            else rounded = std::round(source);
            if (rounded < (double)(std::numeric_limits<int64_t>::min)() ||
                rounded > (double)(std::numeric_limits<int64_t>::max)())
                continue;
            RuntimePropertyValue integer;
            integer.kind = BF6_RIME_VALUE_INT;
            integer.integer = (int64_t)rounded;
            changed |= add(RuntimeKey{node.instance, kIntOut}, integer);
            RuntimePropertyValue real;
            real.kind = BF6_RIME_VALUE_REAL;
            real.real = rounded;
            changed |= add(RuntimeKey{node.instance, kFloatOut}, real);
        }
        for (const bf6_rime_property_cast& node : graph.property_casts)
        {
            std::set<RuntimePropertyValue> sources;
            for (uint32_t field : kCastInputs)
            {
                const auto found = candidates.find(
                    RuntimeKey{node.instance, field});
                if (found != candidates.end() && found->second.size() == 1)
                    sources.insert(*found->second.begin());
            }
            if (sources.size() != 1) continue;
            const RuntimePropertyValue& source = *sources.begin();
            auto numeric = [&](long double& value) {
                if (source.kind == BF6_RIME_VALUE_INT)
                    value = (long double)source.integer;
                else if (source.kind == BF6_RIME_VALUE_UINT)
                    value = (long double)source.unsigned_integer;
                else if (source.kind == BF6_RIME_VALUE_REAL &&
                         std::isfinite(source.real))
                    value = (long double)source.real;
                else if (source.kind == BF6_RIME_VALUE_BOOL)
                    value = source.boolean ? 1.0L : 0.0L;
                else return false;
                return true;
            };
            std::set<uint32_t> requested;
            for (const bf6_rime_connection& connection : graph.connections)
                if (connection.source == node.instance)
                    requested.insert(connection.source_field);
            long double scalar = 0.0L;
            for (uint32_t field : requested)
            {
                RuntimePropertyValue output;
                bool valid = false;
                if (field == kCastToString)
                {
                    output.kind = BF6_RIME_VALUE_STRING;
                    if (source.kind == BF6_RIME_VALUE_STRING)
                        output.string = source.string;
                    else if (source.kind == BF6_RIME_VALUE_INT)
                        output.string = std::to_string(source.integer);
                    else if (source.kind == BF6_RIME_VALUE_UINT)
                        output.string = std::to_string(source.unsigned_integer);
                    else if (source.kind == BF6_RIME_VALUE_BOOL)
                        output.string = source.boolean ? "True" : "False";
                    else if (source.kind == BF6_RIME_VALUE_REAL &&
                             std::isfinite(source.real))
                    {
                        char buffer[64]{};
                        std::snprintf(buffer, sizeof(buffer), "%.9g", source.real);
                        output.string = buffer;
                    }
                    valid = source.kind == BF6_RIME_VALUE_STRING ||
                            source.kind == BF6_RIME_VALUE_INT ||
                            source.kind == BF6_RIME_VALUE_UINT ||
                            source.kind == BF6_RIME_VALUE_BOOL ||
                            source.kind == BF6_RIME_VALUE_REAL;
                }
                else if (field == kCastToBool)
                {
                    bool value = false;
                    valid = runtime_bool(source, value);
                    output.kind = BF6_RIME_VALUE_BOOL;
                    output.boolean = value;
                }
                else if ((field == kCastToFloat ||
                          field == kCastToDouble) && numeric(scalar))
                {
                    output.kind = BF6_RIME_VALUE_REAL;
                    output.real = (double)scalar;
                    valid = std::isfinite(output.real);
                }
                else if (field == kCastToInt && numeric(scalar) &&
                         scalar >= (long double)(std::numeric_limits<int64_t>::min)() &&
                         scalar <= (long double)(std::numeric_limits<int64_t>::max)())
                {
                    output.kind = BF6_RIME_VALUE_INT;
                    output.integer = (int64_t)std::llround((double)scalar);
                    valid = true;
                }
                else if (field == kCastToUint && numeric(scalar) && scalar >= 0.0L &&
                         scalar <= (long double)(std::numeric_limits<uint64_t>::max)())
                {
                    output.kind = BF6_RIME_VALUE_UINT;
                    output.unsigned_integer = (uint64_t)std::llround((double)scalar);
                    valid = true;
                }
                if (valid) changed |= add(RuntimeKey{node.instance, field}, output);
            }
        }
        for (const bf6_rime_compare_float& node : graph.compare_floats)
        {
            auto operand = [&](uint32_t field, double fallback, double& out) {
                const auto found = candidates.find(RuntimeKey{node.instance, field});
                if (found == candidates.end()) { out = fallback; return true; }
                if (found->second.size() != 1) return false;
                float value = 0.f;
                if (!runtime_real(*found->second.begin(), value)) return false;
                out = value;
                return std::isfinite(out);
            };
            double a = 0.0, b = 0.0;
            if (!operand(kCompareA, node.authored_a, a) ||
                !operand(kCompareB, node.authored_b, b))
                continue;
            const std::pair<uint32_t, bool> relations[] = {
                {kCompareEq, a == b}, {kCompareNotEq, a != b},
                {kCompareGreaterEq, a >= b}, {kCompareLessEq, a <= b},
                {kCompareGreater, a > b}, {kCompareLess, a < b}
            };
            for (const auto& relation : relations)
            {
                RuntimePropertyValue output;
                output.kind = BF6_RIME_VALUE_BOOL;
                output.boolean = relation.second;
                changed |= add(RuntimeKey{node.instance, relation.first}, output);
            }
        }
        for (const Screen::InterfaceTextGraph::LocalizedFormat& node :
             graph.localized_formats)
        {
            RuntimePropertyValue output;
            output.kind = BF6_RIME_VALUE_STRING;
            output.string = node.pattern;
            bool complete = true;
            for (uint32_t field : node.argument_fields)
            {
                const auto argument = candidates.find(
                    RuntimeKey{node.instance, field});
                if (argument == candidates.end() ||
                    argument->second.size() != 1)
                { complete = false; break; }
                const RuntimePropertyValue& value = *argument->second.begin();
                TextFormatArgument retained;
                retained.kind = value.kind;
                retained.integer = value.integer;
                retained.unsigned_integer = value.unsigned_integer;
                retained.real = value.real;
                retained.string = value.string;
                if (value.kind == BF6_RIME_VALUE_INT)
                {
                    output.text_format_integers.push_back(value.integer);
                    output.text_format_arguments.push_back(std::move(retained));
                }
                else if (value.kind == BF6_RIME_VALUE_UINT &&
                         value.unsigned_integer <=
                             (uint64_t)(std::numeric_limits<int64_t>::max)())
                {
                    output.text_format_integers.push_back(
                        (int64_t)value.unsigned_integer);
                    output.text_format_arguments.push_back(std::move(retained));
                }
                else if (value.kind == BF6_RIME_VALUE_REAL &&
                         std::isfinite(value.real))
                    output.text_format_arguments.push_back(std::move(retained));
                else if (value.kind == BF6_RIME_VALUE_STRING)
                    output.text_format_arguments.push_back(std::move(retained));
                else
                { complete = false; break; }
            }
            if (complete)
                changed |= add(RuntimeKey{node.instance, node.output_field},
                               output);
        }
        for (const bf6_rime_conditional_float& node :
             graph.float_conditionals)
        {
            std::set<bool> conditions;
            const auto dynamic = candidates.find(
                RuntimeKey{node.instance, kCondition});
            if (dynamic == candidates.end())
                conditions.insert(node.authored_condition != 0);
            else
                for (const RuntimePropertyValue& value : dynamic->second)
                {
                    bool condition = false;
                    if (runtime_bool(value, condition))
                        conditions.insert(condition);
                }
            for (bool condition : conditions)
            {
                RuntimePropertyValue output;
                output.kind = BF6_RIME_VALUE_REAL;
                output.real = condition ? node.value_if_true
                                        : node.value_if_false;
                changed |= add(RuntimeKey{node.instance, kOutput}, output);
            }
        }
        /* A newly instantiated Frostbite interpolator publishes its authored
         * DefaultValue before the first time step.  This initial transaction
         * is exact and independent of the still-unexecuted interpolation
         * curve.  It is what makes authored fade helpers start from their
         * shipped state instead of leaking the target element's editor-cache
         * value into frame zero. */
        for (const bf6_rime_float_interpolator& node :
             graph.float_interpolators)
        {
            RuntimePropertyValue output;
            output.kind = BF6_RIME_VALUE_REAL;
            output.real = node.default_value;
            changed |= add(RuntimeKey{node.instance, node.output_field}, output);
        }
        for (const bf6_rime_conditional_property& node : graph.conditionals)
        {
            std::set<bool> conditions;
            const auto dynamic = candidates.find(RuntimeKey{node.instance, kCondition});
            if (dynamic == candidates.end())
                conditions.insert(node.authored_condition != 0);
            else
                for (const RuntimePropertyValue& value : dynamic->second)
                {
                    bool condition = false;
                    if (runtime_bool(value, condition)) conditions.insert(condition);
                }
            for (bool condition : conditions)
            {
                const uint32_t selected = condition
                    ? node.value_if_true_property_hash
                    : node.value_if_false_property_hash;
                const auto source = candidates.find(RuntimeKey{node.instance, selected});
                if (source == candidates.end()) continue;
                for (const RuntimePropertyValue& value : source->second)
                    changed |= add(RuntimeKey{node.instance, node.out_hash}, value);
            }
        }
        for (const bf6_rime_logic_operation& node : graph.logic_operations)
        {
            std::vector<bool> inputs;
            bool allWiredKnown = false;
            bool invalid = false;
            std::set<uint32_t> inputFields;
            std::set<uint32_t> outputFields;
            for (const bf6_rime_connection& connection : graph.connections)
            {
                if (connection.target == node.instance)
                    inputFields.insert(connection.target_field);
                if (connection.source == node.instance)
                    outputFields.insert(connection.source_field);
            }
            if (outputFields.empty()) continue;
            if (node.operation == BF6_RIME_LOGIC_PROPERTY_DEFAULT)
            {
                const RuntimePropertyValue* selected = nullptr;
                const auto input = candidates.find(
                    RuntimeKey{node.instance, kInValue});
                if (input != candidates.end() && input->second.size() == 1)
                    selected = &*input->second.begin();
                else
                {
                    const auto fallback = candidates.find(
                        RuntimeKey{node.instance, kDefaultValue});
                    if (fallback != candidates.end() &&
                        fallback->second.size() == 1)
                        selected = &*fallback->second.begin();
                }
                if (!selected) continue;
                for (uint32_t field : outputFields)
                    changed |= add(RuntimeKey{node.instance, field}, *selected);
                continue;
            }

            allWiredKnown = !inputFields.empty();
            for (uint32_t field : inputFields)
            {
                /* A dynamic Rime input pin may have more than one authored
                 * producer.  Seeing a value copied from one writer does not
                 * make the pin known while another writer is unresolved.
                 * Treating that partial value as final hid the authored start
                 * movie: a false InterfaceDescriptor default reached an OR
                 * pin before the movie element's Loop/AutoStart producers had
                 * been decoded.  Require every incoming source endpoint to be
                 * singular before executing the operator. */
                bool everyProducerKnown = true;
                for (const bf6_rime_connection& connection : graph.connections)
                {
                    if (connection.target != node.instance ||
                        connection.target_field != field)
                        continue;
                    const auto producer = candidates.find(RuntimeKey{
                        connection.source, connection.source_field});
                    if (producer == candidates.end() ||
                        producer->second.size() != 1)
                    {
                        everyProducerKnown = false;
                        break;
                    }
                }
                if (!everyProducerKnown)
                {
                    allWiredKnown = false;
                    break;
                }
                const auto found = candidates.find(
                    RuntimeKey{node.instance, field});
                if (found == candidates.end() || found->second.size() != 1)
                {
                    allWiredKnown = false;
                    break;
                }
                bool value = false;
                if (!runtime_bool(*found->second.begin(), value))
                {
                    invalid = true;
                    break;
                }
                inputs.push_back(value);
            }
            if (!allWiredKnown || invalid) continue;
            bool result = false;
            if (node.operation == BF6_RIME_LOGIC_NOT)
            {
                if (inputs.size() != 1) continue;
                result = !inputs[0];
            }
            else if (node.operation == BF6_RIME_LOGIC_AND)
            {
                result = true;
                for (bool value : inputs) result = result && value;
            }
            else if (node.operation == BF6_RIME_LOGIC_OR)
            {
                for (bool value : inputs) result = result || value;
            }
            else continue;

            RuntimePropertyValue output;
            output.kind = BF6_RIME_VALUE_BOOL;
            output.boolean = result;
            for (uint32_t field : outputFields)
                changed |= add(RuntimeKey{node.instance, field}, output);
        }
        for (const bf6_rime_array_element& node : graph.array_elements)
        {
            static const uint32_t kArray = property_hash("Array");
            static const uint32_t kIndex = property_hash("Index");
            static const uint32_t kElement = property_hash("Element");
            const auto array = candidates.find(
                RuntimeKey{node.instance, kArray});
            const auto index = candidates.find(
                RuntimeKey{node.instance, kIndex});
            if (array == candidates.end() || array->second.size() != 1 ||
                index == candidates.end() || index->second.size() != 1)
                continue;
            const RuntimePropertyValue& source = *array->second.begin();
            const RuntimePropertyValue& selector = *index->second.begin();
            if (source.kind != BF6_RIME_VALUE_ARRAY) continue;
            int64_t selected = -1;
            if (selector.kind == BF6_RIME_VALUE_INT)
                selected = selector.integer;
            else if (selector.kind == BF6_RIME_VALUE_UINT &&
                     selector.unsigned_integer <=
                         (uint64_t)(std::numeric_limits<int64_t>::max)())
                selected = (int64_t)selector.unsigned_integer;
            if (selected < 0 ||
                (size_t)selected >= source.string_array.size())
                continue;
            RuntimePropertyValue output;
            output.kind = BF6_RIME_VALUE_STRING;
            output.string = source.string_array[(size_t)selected];
            changed |= add(RuntimeKey{node.instance, kElement}, output);
        }
        if (!changed) break;
    }

    int applied = 0;
    for (Element& element : s.elements)
    {
        if (element.partition != graph.partition ||
            element.scope != graph.scope || element.instance < 0)
            continue;
        auto unique = [&](uint32_t field) -> const RuntimePropertyValue* {
            const auto it = candidates.find(RuntimeKey{element.instance, field});
            if (it == candidates.end()) return nullptr;
            if (it->second.size() != 1)
            {
                if (ambiguous) ++*ambiguous;
                return nullptr;
            }
            return &*it->second.begin();
        };
        if (const RuntimePropertyValue* value = unique(kVisible))
        {
            bool visible = false;
            if (runtime_bool(*value, visible))
            {
                element.visible = visible;
                element.runtime_visibility_unresolved = false;
                if (resolved)
                    resolved->push_back({RuntimeKey{element.instance, kVisible},
                                         *value});
                ++applied;
            }
        }
        if (const RuntimePropertyValue* value = unique(kAlpha))
        {
            float alpha = 0.f;
            if (runtime_real(*value, alpha))
            {
                element.alpha = alpha;
                element.runtime_alpha_unresolved = false;
                if (resolved)
                    resolved->push_back({RuntimeKey{element.instance, kAlpha},
                                         *value});
                ++applied;
            }
        }
        if (const RuntimePropertyValue* value = unique(kWidth))
        {
            float width = 0.f;
            if (runtime_real(*value, width)) {
                element.width = width;
                if (resolved)
                    resolved->push_back({RuntimeKey{element.instance, kWidth},
                                         *value});
                ++applied;
            }
        }
        if (const RuntimePropertyValue* value = unique(kHeight))
        {
            float height = 0.f;
            if (runtime_real(*value, height)) {
                element.height = height;
                if (resolved)
                    resolved->push_back({RuntimeKey{element.instance, kHeight},
                                         *value});
                ++applied;
            }
        }
        if (element.kind == Kind::Progress)
        {
            if (const RuntimePropertyValue* value = unique(kProgress))
            {
                float progress = 0.f;
                if (runtime_real(*value, progress))
                {
                    element.progress = progress;
                    if (resolved)
                        resolved->push_back(
                            {RuntimeKey{element.instance, kProgress},
                             *value});
                    ++applied;
                }
            }
        }
        /* These are ordinary Rime properties, not special effects invented
         * by the host.  cl_border_focus drives all three through its shipped
         * property graph.  Dropping them here left the decoded animated
         * outline frozen at its editor defaults even when the graph had
         * produced an unambiguous value. */
        if (element.kind == Kind::Line)
        {
            if (const RuntimePropertyValue* value = unique(kStartProgress))
            {
                float progress = 0.f;
                if (runtime_real(*value, progress))
                {
                    element.line_start_progress = progress;
                    if (resolved)
                        resolved->push_back(
                            {RuntimeKey{element.instance, kStartProgress},
                             *value});
                    ++applied;
                }
            }
            if (const RuntimePropertyValue* value = unique(kEndProgress))
            {
                float progress = 0.f;
                if (runtime_real(*value, progress))
                {
                    element.line_end_progress = progress;
                    if (resolved)
                        resolved->push_back(
                            {RuntimeKey{element.instance, kEndProgress},
                             *value});
                    ++applied;
                }
            }
            if (const RuntimePropertyValue* value = unique(kGlowSize))
            {
                float glow = 0.f;
                if (runtime_real(*value, glow))
                {
                    element.line_glow_size = glow;
                    if (resolved)
                        resolved->push_back(
                            {RuntimeKey{element.instance, kGlowSize}, *value});
                    ++applied;
                }
            }
        }
        if (element.kind != Kind::Label) continue;
        const uint32_t fields[] = { kLocalizedText, kRawText, kStringId, kText };
        for (uint32_t field : fields)
        {
            const RuntimePropertyValue* value = unique(field);
            if (!value) continue;
            if (value->kind == BF6_RIME_VALUE_STRING)
            {
                element.text = value->string;
                element.text_format_integers =
                    value->text_format_integers;
                element.text_format_arguments =
                    value->text_format_arguments;
                if (resolved)
                    resolved->push_back({RuntimeKey{element.instance, field},
                                         *value});
                ++applied;
            }
            break;
        }
    }
    /* Callers that bridge nested WidgetReferences need the complete set of
     * unique graph results, including public-interface outputs and values on
     * WidgetReference pins.  Returning only properties that happened to paint
     * a local element made child-produced state (IsFocused/IsHovered/etc.)
     * stop at the first prefab boundary. */
    if (resolved && include_all_resolved)
    {
        resolved->clear();
        for (const auto& entry : candidates)
            if (entry.second.size() == 1)
                resolved->push_back({entry.first, *entry.second.begin()});
    }
    return applied;
}

Screen::InterfaceTextGraph::Input interface_input(
    const RuntimePropertyValue& value)
{
    Screen::InterfaceTextGraph::Input input;
    input.kind = value.kind;
    input.boolean = value.boolean;
    input.integer = value.integer;
    input.unsigned_integer = value.unsigned_integer;
    input.real = value.real;
    input.string = value.string;
    input.string_array = value.string_array;
    input.text_format_integers = value.text_format_integers;
    input.text_format_arguments = value.text_format_arguments;
    return input;
}

Screen::InterfaceTextGraph* interface_graph_field_scoped(
    Screen& s, const char* partition, uint32_t field, int scope)
{
    if (!partition || !*partition || !field) return nullptr;
    Screen::InterfaceTextGraph* selected = nullptr;
    for (Screen::InterfaceTextGraph& graph : s.interface_text_graphs)
        if (graph.partition == partition && graph.scope == scope)
        { selected = &graph; break; }
    if (!selected) return nullptr;
    for (const bf6_rime_connection& connection : selected->connections)
        for (int32_t interfaceInstance : selected->interfaces)
            if (connection.source == interfaceInstance &&
                connection.source_field == field)
                return selected;
    return nullptr;
}

Screen::InterfaceTextGraph* interface_graph_field(
    Screen& s, const char* partition, uint32_t field)
{
    if (Screen::InterfaceTextGraph* root =
            interface_graph_field_scoped(s, partition, field, -1))
        return root;
    Screen::InterfaceTextGraph* unique = nullptr;
    for (Screen::InterfaceTextGraph& graph : s.interface_text_graphs)
    {
        if (graph.partition != partition) continue;
        bool exposes = false;
        for (const bf6_rime_connection& connection : graph.connections)
            for (int32_t interfaceInstance : graph.interfaces)
                if (connection.source == interfaceInstance &&
                    connection.source_field == field)
                    exposes = true;
        if (!exposes) continue;
        if (unique) return nullptr;
        unique = &graph;
    }
    return unique;
}

Screen::InterfaceTextGraph* interface_graph(
    Screen& s, const char* partition, const char* property_name,
    uint32_t& field)
{
    if (!property_name || !*property_name) return nullptr;
    field = property_hash(property_name);
    return interface_graph_field(s, partition, field);
}

std::string normalized_widget_partition(std::string target)
{
    for (char& ch : target)
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
    if (target.size() > 4 &&
        target.compare(target.size() - 4, 4, ".ebx") == 0)
        target.resize(target.size() - 4);
    return target;
}

int set_interface_value_field_recursive(
    Screen& s, const char* partition, uint32_t field,
    const Screen::InterfaceTextGraph::Input& value, int* ambiguous,
    int scope, std::set<std::tuple<std::string, uint32_t, int>>& visiting)
{
    if (ambiguous) *ambiguous = 0;
    if (!partition || !*partition || !field) return 0;
    const std::tuple<std::string, uint32_t, int> key{partition, field, scope};
    if (!visiting.insert(key).second) return 0;
    Screen::InterfaceTextGraph* graph =
        interface_graph_field_scoped(s, partition, field, scope);
    if (!graph)
    {
        visiting.erase(key);
        return 0;
    }
    bool replaced = false;
    for (Screen::InterfaceTextGraph::Input& input : graph->inputs)
        if (input.source_instance < 0 && input.field == field)
        {
            input = value;
            input.source_instance = -1;
            input.field = field;
            replaced = true;
            break;
        }
    if (!replaced)
    {
        Screen::InterfaceTextGraph::Input input = value;
        input.source_instance = -1;
        input.field = field;
        graph->inputs.push_back(std::move(input));
    }
    int outerAmbiguous = 0;
    std::vector<ResolvedRuntimeValue> resolved;
    int applied = apply_interface_graph(s, *graph, &outerAmbiguous, &resolved);

    std::set<int32_t> interfaces(graph->interfaces.begin(),
                                 graph->interfaces.end());
    for (const bf6_rime_connection& connection : graph->connections)
    {
        if (!interfaces.count(connection.source) ||
            connection.source_field != field)
            continue;
        for (const Element& element : s.elements)
        {
            if (element.partition != graph->partition ||
                element.scope != graph->scope ||
                element.instance != connection.target ||
                element.kind != Kind::WidgetReference ||
                element.references_widget.empty())
                continue;
            const std::string target = normalized_widget_partition(
                element.references_widget);
            int childAmbiguous = 0;
            applied += set_interface_value_field_recursive(
                s, target.c_str(), connection.target_field, value,
                &childAmbiguous, (int)(&element - s.elements.data()), visiting);
            outerAmbiguous += childAmbiguous;
        }
    }
    /* Operators can produce a WidgetReference input that is not a direct
     * edge from the public interface (for example ConditionalFloat -> Alpha).
     * Carry that resolved typed value across the authored widget boundary as
     * well.  This is graph execution, not a control-specific alias. */
    for (const ResolvedRuntimeValue& output : resolved)
        for (const Element& element : s.elements)
        {
            if (element.partition != graph->partition ||
                element.scope != graph->scope ||
                element.instance != output.first.first ||
                element.kind != Kind::WidgetReference ||
                element.references_widget.empty())
                continue;
            const std::string target = normalized_widget_partition(
                element.references_widget);
            int childAmbiguous = 0;
            applied += set_interface_value_field_recursive(
                s, target.c_str(), output.first.second,
                interface_input(output.second), &childAmbiguous,
                (int)(&element - s.elements.data()), visiting);
            outerAmbiguous += childAmbiguous;
        }
    visiting.erase(key);
    if (ambiguous) *ambiguous = outerAmbiguous;
    return applied;
}

int set_interface_value_field(
    Screen& s, const char* partition, uint32_t field,
    const Screen::InterfaceTextGraph::Input& value, int* ambiguous)
{
    Screen::InterfaceTextGraph* graph = interface_graph_field(s, partition, field);
    if (!graph) { if (ambiguous) *ambiguous = 0; return 0; }
    std::set<std::tuple<std::string, uint32_t, int>> visiting;
    return set_interface_value_field_recursive(
        s, partition, field, value, ambiguous, graph->scope, visiting);
}

using ExactSourceVisitKey =
    std::tuple<std::string, int, int32_t, uint32_t>;

std::set<RuntimeKey> graph_reachable_from(
    const Screen::InterfaceTextGraph& graph, const RuntimeKey& seed)
{
    std::set<RuntimeKey> reachable{seed};
    const int limit = static_cast<int>(graph.connections.size()) + 2;
    for (int pass = 0; pass < limit; ++pass)
    {
        bool changed = false;
        for (const bf6_rime_connection& connection : graph.connections)
        {
            const RuntimeKey source{connection.source,
                                    connection.source_field};
            const RuntimeKey target{connection.target,
                                    connection.target_field};
            if (reachable.count(source)) changed |= reachable.insert(target).second;
        }

        /* A graph operator transforms any of its input pins into one or more
         * authored output pins.  Connection metadata identifies those pins
         * without naming or special-casing the operator. */
        std::set<int32_t> taintedOperators;
        for (const bf6_rime_connection& connection : graph.connections)
            if (reachable.count(
                    RuntimeKey{connection.target, connection.target_field}))
                taintedOperators.insert(connection.target);
        for (const bf6_rime_connection& connection : graph.connections)
            if (taintedOperators.count(connection.source))
                changed |= reachable.insert(
                    RuntimeKey{connection.source,
                               connection.source_field}).second;
        if (!changed) break;
    }
    return reachable;
}

bool is_public_output(const Screen::InterfaceTextGraph& graph,
                      int32_t interface_instance, uint32_t field)
{
    for (const bf6_rime_interface_field& declaration : graph.defaults)
        if (declaration.interface_instance == interface_instance &&
            declaration.field_id == field)
            return declaration.access_type == 0;
    return false;
}

int set_exact_source_value_recursive(
    Screen& s, const std::string& partition, int scope,
    int32_t source_instance, uint32_t field,
    const Screen::InterfaceTextGraph::Input& value, int* ambiguous,
    std::set<ExactSourceVisitKey>& visiting)
{
    if (ambiguous) *ambiguous = 0;
    const ExactSourceVisitKey key{partition, scope, source_instance, field};
    if (!field || !visiting.insert(key).second) return 0;

    Screen::InterfaceTextGraph* graph = nullptr;
    for (Screen::InterfaceTextGraph& candidate : s.interface_text_graphs)
        if (candidate.partition == partition && candidate.scope == scope)
        { graph = &candidate; break; }
    if (!graph) { visiting.erase(key); return 0; }

    bool replaced = false;
    for (Screen::InterfaceTextGraph::Input& input : graph->inputs)
        if (input.source_instance == source_instance && input.field == field)
        {
            input = value;
            input.source_instance = source_instance;
            input.field = field;
            replaced = true;
            break;
        }
    if (!replaced)
    {
        Screen::InterfaceTextGraph::Input input = value;
        input.source_instance = source_instance;
        input.field = field;
        graph->inputs.push_back(std::move(input));
    }

    int outerAmbiguous = 0;
    std::vector<ResolvedRuntimeValue> resolved;
    int applied = apply_interface_graph(
        s, *graph, &outerAmbiguous, &resolved, true);
    const std::set<RuntimeKey> reachable = graph_reachable_from(
        *graph, RuntimeKey{source_instance, field});

    /* Values arriving at a WidgetReference are public inputs of the exact
     * child occurrence.  Preserve the authored field id across that boundary. */
    for (const ResolvedRuntimeValue& output : resolved)
    {
        if (!reachable.count(output.first)) continue;
        for (const Element& element : s.elements)
        {
            if (element.partition != graph->partition ||
                element.scope != graph->scope ||
                element.instance != output.first.first ||
                element.kind != Kind::WidgetReference ||
                element.references_widget.empty())
                continue;
            const std::string target = normalized_widget_partition(
                element.references_widget);
            int childAmbiguous = 0;
            std::set<std::tuple<std::string, uint32_t, int>> childVisiting;
            applied += set_interface_value_field_recursive(
                s, target.c_str(), output.first.second,
                interface_input(output.second), &childAmbiguous,
                static_cast<int>(&element - s.elements.data()), childVisiting);
            outerAmbiguous += childAmbiguous;
        }
    }

    /* An InterfaceDescriptor output belongs on the WidgetReference that
     * instantiated this occurrence.  Feed it into the exact parent graph;
     * this is the reverse half of the prefab bridge and is what carries an
     * input behavior's focus state back to the button and border prefabs. */
    if (graph->scope >= 0 &&
        static_cast<size_t>(graph->scope) < s.elements.size())
    {
        const Element& owner = s.elements[static_cast<size_t>(graph->scope)];
        const std::set<int32_t> interfaces(graph->interfaces.begin(),
                                           graph->interfaces.end());
        for (const ResolvedRuntimeValue& output : resolved)
        {
            if (!reachable.count(output.first)) continue;
            if (!interfaces.count(output.first.first)) continue;
            if (!is_public_output(*graph, output.first.first,
                                  output.first.second))
                continue;
            int parentAmbiguous = 0;
            applied += set_exact_source_value_recursive(
                s, owner.partition, owner.scope, owner.instance,
                output.first.second, interface_input(output.second),
                &parentAmbiguous, visiting);
            outerAmbiguous += parentAmbiguous;
        }
    }

    visiting.erase(key);
    if (ambiguous) *ambiguous = outerAmbiguous;
    return applied;
}

int set_interface_value(Screen& s, const char* partition,
                        const char* property_name,
                        const Screen::InterfaceTextGraph::Input& value,
                        int* ambiguous)
{
    if (ambiguous) *ambiguous = 0;
    uint32_t field = 0;
    if (!interface_graph(s, partition, property_name, field)) return 0;
    return set_interface_value_field(s, partition, field, value, ambiguous);
}

Screen::InterfaceTextGraph* provider_graph(
    Screen& s, const char* partition, uint32_t provider_field,
    int32_t& source_instance)
{
    if (!partition || !*partition) return nullptr;
    Screen::InterfaceTextGraph* selected = nullptr;
    for (Screen::InterfaceTextGraph& graph : s.interface_text_graphs)
        if (graph.partition == partition && graph.scope == -1)
        { selected = &graph; break; }
    if (!selected)
        for (Screen::InterfaceTextGraph& graph : s.interface_text_graphs)
            if (graph.partition == partition)
            {
                if (selected) return nullptr;
                selected = &graph;
            }
    if (!selected) return nullptr;

    std::set<int32_t> sources;
    for (const bf6_rime_connection& connection : selected->connections)
        if (connection.source_field == provider_field)
            sources.insert(connection.source);
    if (sources.size() != 1) return nullptr;
    source_instance = *sources.begin();
    return selected;
}

int set_provider_value(Screen& s, const char* partition,
                       uint32_t provider_field,
                       const Screen::InterfaceTextGraph::Input& value,
                       int* ambiguous)
{
    if (ambiguous) *ambiguous = 0;
    int32_t source_instance = -1;
    Screen::InterfaceTextGraph* graph = provider_graph(
        s, partition, provider_field, source_instance);
    if (!graph) return 0;
    std::set<ExactSourceVisitKey> visiting;
    return set_exact_source_value_recursive(
        s, graph->partition, graph->scope, source_instance, provider_field,
        value, ambiguous, visiting);
}

} // namespace

int apply_interface_defaults(Screen& s)
{
    int applied = 0;
    std::vector<std::vector<ResolvedRuntimeValue>> graphResolved(
        s.interface_text_graphs.size());
    for (size_t graphIndex = 0;
         graphIndex < s.interface_text_graphs.size(); ++graphIndex)
        applied += apply_interface_graph(
            s, s.interface_text_graphs[graphIndex], nullptr,
            &graphResolved[graphIndex], true);

    /* Any uniquely resolved property on a WidgetReference is the exact
     * public input of that child occurrence.  Bridge all such values, not
     * only InterfaceDescriptor defaults: authored string entities and graph
     * operator outputs can terminate at the same reference pins. */
    for (size_t graphIndex = 0;
         graphIndex < s.interface_text_graphs.size(); ++graphIndex)
    {
        const Screen::InterfaceTextGraph& graph =
            s.interface_text_graphs[graphIndex];
        for (const ResolvedRuntimeValue& output : graphResolved[graphIndex])
            for (const Element& element : s.elements)
            {
                if (element.partition != graph.partition ||
                    element.scope != graph.scope ||
                    element.instance != output.first.first ||
                    element.kind != Kind::WidgetReference ||
                    element.references_widget.empty())
                    continue;
                const std::string target = normalized_widget_partition(
                    element.references_widget);
                std::set<std::tuple<std::string, uint32_t, int>> visiting;
                applied += set_interface_value_field_recursive(
                    s, target.c_str(), output.first.second,
                    interface_input(output.second), nullptr,
                    static_cast<int>(&element - s.elements.data()), visiting);
            }
    }

    /* A default on a parent widget's public InterfaceDescriptor can target a
     * child WidgetReference.  It is runtime input to the referenced graph,
     * not merely a property on the empty reference rectangle.  Follow that
     * authored edge by exact instance/field just as provider inputs do. */
    for (size_t graphIndex = 0;
         graphIndex < s.interface_text_graphs.size(); ++graphIndex)
    {
        const Screen::InterfaceTextGraph& graph =
            s.interface_text_graphs[graphIndex];
        const std::set<int32_t> interfaces(graph.interfaces.begin(),
                                           graph.interfaces.end());
        for (const bf6_rime_interface_field& field : graph.defaults)
        {
            if (!interfaces.count(field.interface_instance) ||
                field.value_kind == BF6_RIME_VALUE_NULL ||
                field.value_kind == BF6_RIME_VALUE_UNRESOLVED ||
                field.value_kind == BF6_RIME_VALUE_STRUCT ||
                field.value_kind == BF6_RIME_VALUE_ARRAY)
                continue;
            Screen::InterfaceTextGraph::Input input;
            input.kind = field.value_kind;
            input.boolean = field.bool_value != 0;
            input.integer = field.int_value;
            input.unsigned_integer = field.uint_value;
            input.real = field.real_value;
            input.string = field.string_value;
            for (const bf6_rime_connection& connection : graph.connections)
            {
                if (connection.source != field.interface_instance ||
                    connection.source_field != field.field_id)
                    continue;
                for (const Element& element : s.elements)
                {
                    if (element.partition != graph.partition ||
                        element.scope != graph.scope ||
                        element.instance != connection.target ||
                        element.kind != Kind::WidgetReference ||
                        element.references_widget.empty())
                        continue;
                    const std::string target = normalized_widget_partition(
                        element.references_widget);
                    std::set<std::tuple<std::string, uint32_t, int>> visiting;
                    applied += set_interface_value_field_recursive(
                        s, target.c_str(), connection.target_field, input,
                        nullptr, (int)(&element - s.elements.data()), visiting);
                }
            }
        }
    }
    return applied;
}

int set_interface_text(Screen& s, const char* partition,
                       const char* property_name, const std::string& value,
                       int* ambiguous)
{
    Screen::InterfaceTextGraph::Input input;
    input.kind = BF6_RIME_VALUE_STRING;
    input.string = value;
    return set_interface_value(s, partition, property_name, input, ambiguous);
}

int set_interface_bool(Screen& s, const char* partition,
                       const char* property_name, bool value, int* ambiguous)
{
    Screen::InterfaceTextGraph::Input input;
    input.kind = BF6_RIME_VALUE_BOOL;
    input.boolean = value;
    return set_interface_value(s, partition, property_name, input, ambiguous);
}

int set_interface_int(Screen& s, const char* partition,
                      const char* property_name, int64_t value, int* ambiguous)
{
    Screen::InterfaceTextGraph::Input input;
    input.kind = BF6_RIME_VALUE_INT;
    input.integer = value;
    return set_interface_value(s, partition, property_name, input, ambiguous);
}

int set_interface_uint(Screen& s, const char* partition,
                       const char* property_name, uint64_t value, int* ambiguous)
{
    Screen::InterfaceTextGraph::Input input;
    input.kind = BF6_RIME_VALUE_UINT;
    input.unsigned_integer = value;
    return set_interface_value(s, partition, property_name, input, ambiguous);
}

int set_interface_real(Screen& s, const char* partition,
                       const char* property_name, double value, int* ambiguous)
{
    Screen::InterfaceTextGraph::Input input;
    input.kind = BF6_RIME_VALUE_REAL;
    input.real = value;
    return set_interface_value(s, partition, property_name, input, ambiguous);
}

int set_interface_text_field(Screen& s, const char* partition, uint32_t field,
                             const std::string& value, int* ambiguous)
{
    Screen::InterfaceTextGraph::Input input;
    input.kind = BF6_RIME_VALUE_STRING;
    input.string = value;
    return set_interface_value_field(s, partition, field, input, ambiguous);
}

int set_interface_string_array_field(
    Screen& s, const char* partition, uint32_t field,
    const std::vector<std::string>& value, int* ambiguous)
{
    Screen::InterfaceTextGraph::Input input;
    input.kind = BF6_RIME_VALUE_ARRAY;
    input.string_array = value;
    return set_interface_value_field(s, partition, field, input, ambiguous);
}

int set_interface_bool_field(Screen& s, const char* partition, uint32_t field,
                             bool value, int* ambiguous)
{
    Screen::InterfaceTextGraph::Input input;
    input.kind = BF6_RIME_VALUE_BOOL;
    input.boolean = value;
    return set_interface_value_field(s, partition, field, input, ambiguous);
}

int set_interface_int_field(Screen& s, const char* partition, uint32_t field,
                            int64_t value, int* ambiguous)
{
    Screen::InterfaceTextGraph::Input input;
    input.kind = BF6_RIME_VALUE_INT;
    input.integer = value;
    return set_interface_value_field(s, partition, field, input, ambiguous);
}

int set_interface_uint_field(Screen& s, const char* partition, uint32_t field,
                             uint64_t value, int* ambiguous)
{
    Screen::InterfaceTextGraph::Input input;
    input.kind = BF6_RIME_VALUE_UINT;
    input.unsigned_integer = value;
    return set_interface_value_field(s, partition, field, input, ambiguous);
}

int set_interface_real_field(Screen& s, const char* partition, uint32_t field,
                             double value, int* ambiguous)
{
    Screen::InterfaceTextGraph::Input input;
    input.kind = BF6_RIME_VALUE_REAL;
    input.real = value;
    return set_interface_value_field(s, partition, field, input, ambiguous);
}

int set_interface_bool_field_scoped(Screen& s, const char* partition,
                                    int scope, uint32_t field, bool value,
                                    int* ambiguous)
{
    Screen::InterfaceTextGraph::Input input;
    input.kind = BF6_RIME_VALUE_BOOL;
    input.boolean = value;
    std::set<std::tuple<std::string, uint32_t, int>> visiting;
    return set_interface_value_field_recursive(
        s, partition, field, input, ambiguous, scope, visiting);
}

int set_interface_int_field_scoped(Screen& s, const char* partition,
                                   int scope, uint32_t field, int64_t value,
                                   int* ambiguous)
{
    Screen::InterfaceTextGraph::Input input;
    input.kind = BF6_RIME_VALUE_INT;
    input.integer = value;
    std::set<std::tuple<std::string, uint32_t, int>> visiting;
    return set_interface_value_field_recursive(
        s, partition, field, input, ambiguous, scope, visiting);
}

int set_provider_text(Screen& s, const char* partition,
                      uint32_t provider_field, const std::string& value,
                      int* ambiguous)
{
    Screen::InterfaceTextGraph::Input input;
    input.kind = BF6_RIME_VALUE_STRING;
    input.string = value;
    return set_provider_value(s, partition, provider_field, input, ambiguous);
}

int set_provider_string_array(Screen& s, const char* partition,
                              uint32_t provider_field,
                              const std::vector<std::string>& value,
                              int* ambiguous)
{
    Screen::InterfaceTextGraph::Input input;
    input.kind = BF6_RIME_VALUE_ARRAY;
    input.string_array = value;
    return set_provider_value(s, partition, provider_field, input, ambiguous);
}

int set_provider_bool(Screen& s, const char* partition,
                      uint32_t provider_field, bool value, int* ambiguous)
{
    Screen::InterfaceTextGraph::Input input;
    input.kind = BF6_RIME_VALUE_BOOL;
    input.boolean = value;
    return set_provider_value(s, partition, provider_field, input, ambiguous);
}

int set_provider_int(Screen& s, const char* partition,
                     uint32_t provider_field, int64_t value, int* ambiguous)
{
    Screen::InterfaceTextGraph::Input input;
    input.kind = BF6_RIME_VALUE_INT;
    input.integer = value;
    return set_provider_value(s, partition, provider_field, input, ambiguous);
}

int set_provider_uint(Screen& s, const char* partition,
                      uint32_t provider_field, uint64_t value, int* ambiguous)
{
    Screen::InterfaceTextGraph::Input input;
    input.kind = BF6_RIME_VALUE_UINT;
    input.unsigned_integer = value;
    return set_provider_value(s, partition, provider_field, input, ambiguous);
}

int set_provider_real(Screen& s, const char* partition,
                      uint32_t provider_field, double value, int* ambiguous)
{
    Screen::InterfaceTextGraph::Input input;
    input.kind = BF6_RIME_VALUE_REAL;
    input.real = value;
    return set_provider_value(s, partition, provider_field, input, ambiguous);
}

int mark_unresolved_interface_colors(bf6_ctx* c, Screen& s)
{
    if (!c) return 0;
    static const uint32_t kColor = 0x0ca8c5f8u;
    std::set<std::string> partitions;
    for (const Element& e : s.elements)
        if (!e.partition.empty()) partitions.insert(e.partition);

    int marked = 0;
    for (const std::string& partition : partitions)
    {
        const int ni = bf6_rime_interface_descriptors(c, partition.c_str(),
                                                       nullptr, 0);
        const int nc = bf6_rime_connections(c, partition.c_str(), nullptr, 0);
        if (ni <= 0 || nc <= 0) continue;
        std::vector<int32_t> interfaces((size_t)ni);
        std::vector<bf6_rime_connection> connections((size_t)nc);
        bf6_rime_interface_descriptors(c, partition.c_str(),
                                       interfaces.data(), ni);
        bf6_rime_connections(c, partition.c_str(), connections.data(), nc);
        const std::set<int32_t> interfaceSet(interfaces.begin(), interfaces.end());
        std::set<int32_t> dynamicTargets;
        for (const bf6_rime_connection& connection : connections)
            if (interfaceSet.count(connection.source) &&
                connection.target_field == kColor)
                dynamicTargets.insert(connection.target);

        for (Element& e : s.elements)
            if (e.partition == partition && dynamicTargets.count(e.instance) &&
                !e.runtime_color_unresolved)
            {
                e.runtime_color_unresolved = true;
                ++marked;
            }
    }
    return marked;
}

int mark_unresolved_interface_states(bf6_ctx* c, Screen& s)
{
    if (!c) return 0;
    static const uint32_t kVisible = property_hash("Visible");
    static const uint32_t kAlpha = property_hash("Alpha");
    std::set<std::string> partitions;
    for (const Element& e : s.elements)
        if (!e.partition.empty()) partitions.insert(e.partition);

    int marked = 0;
    for (const std::string& partition : partitions)
    {
        const int ni = bf6_rime_interface_descriptors(c, partition.c_str(),
                                                       nullptr, 0);
        const int nc = bf6_rime_connections(c, partition.c_str(), nullptr, 0);
        if (ni <= 0 || nc <= 0) continue;
        std::vector<int32_t> interfaces((size_t)ni);
        std::vector<bf6_rime_connection> connections((size_t)nc);
        bf6_rime_interface_descriptors(c, partition.c_str(),
                                       interfaces.data(), ni);
        bf6_rime_connections(c, partition.c_str(), connections.data(), nc);
        const std::set<int32_t> interfaceSet(interfaces.begin(), interfaces.end());
        std::set<int32_t> visibilityTargets, alphaTargets;
        for (const bf6_rime_connection& connection : connections)
        {
            if (!interfaceSet.count(connection.source)) continue;
            if (connection.target_field == kVisible)
                visibilityTargets.insert(connection.target);
            else if (connection.target_field == kAlpha)
                alphaTargets.insert(connection.target);
        }

        for (Element& e : s.elements)
        {
            if (e.partition != partition) continue;
            if (visibilityTargets.count(e.instance) &&
                !e.runtime_visibility_unresolved)
            {
                e.runtime_visibility_unresolved = true;
                ++marked;
            }
            if (alphaTargets.count(e.instance) && !e.runtime_alpha_unresolved)
            {
                e.runtime_alpha_unresolved = true;
                ++marked;
            }
        }
    }
    return marked;
}

namespace {

// An empty cell is not a zero: "no anchor authored" and "anchor at 0.0" mean
// different things, and collapsing them puts full-bleed layers at zero size.
bool cell(const std::string& s, float& out)
{
    if (s.empty()) return false;
    out = (float)atof(s.c_str());
    return true;
}

std::vector<std::string> split_tab(const std::string& line)
{
    std::vector<std::string> f;
    size_t a = 0;
    for (;;)
    {
        const size_t b = line.find('\t', a);
        if (b == std::string::npos) { f.push_back(line.substr(a)); break; }
        f.push_back(line.substr(a, b - a));
        a = b + 1;
    }
    // Authored names carry trailing padding spaces in the partition; strip so
    // a name compares equal to the one a widget reference asks for.
    for (std::string& s : f)
    {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.pop_back();
        while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    }
    return f;
}

}  // namespace

bool load_tree_tsv(const char* path, std::vector<Screen>& out, std::string& err)
{
    FILE* f = fopen(path, "rb");
    if (!f) { err = std::string("cannot open ") + path; return false; }

    std::string all;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
    fclose(f);

    std::vector<std::string> lines;
    for (size_t a = 0; a < all.size();)
    {
        size_t b = all.find('\n', a);
        if (b == std::string::npos) b = all.size();
        lines.push_back(all.substr(a, b - a));
        a = b + 1;
    }
    if (lines.size() < 2) { err = "tree file has no rows"; return false; }

    std::map<std::string, int> col;
    {
        const std::vector<std::string> h = split_tab(lines[0]);
        for (size_t i = 0; i < h.size(); i++) col[h[i]] = (int)i;
    }
    auto need = [&](const char* c) { return col.count(c) ? col[c] : -1; };
    const int cPart = need("partition"), cDepth = need("depth");
    const int cType = need("element_type"), cName = need("element_name");
    if (cPart < 0 || cDepth < 0 || cType < 0) { err = "tree file missing columns"; return false; }

    std::map<std::string, size_t> index;
    for (size_t li = 1; li < lines.size(); li++)
    {
        if (lines[li].empty()) continue;
        const std::vector<std::string> r = split_tab(lines[li]);
        if ((int)r.size() <= cType) continue;

        const std::string part = r[(size_t)cPart];
        if (index.find(part) == index.end())
        {
            index[part] = out.size();
            out.push_back(Screen{ part, {} });
        }
        Screen& s = out[index[part]];

        Element e;
        e.type_name = r[(size_t)cType];
        e.kind = kind_of(e.type_name);
        e.depth = atoi(r[(size_t)cDepth].c_str());
        if (cName >= 0 && (int)r.size() > cName) e.name = r[(size_t)cName];

        auto ax = [&](const char* a, const char* b, const char* c, const char* d,
                      const char* p, const char* w, Axis& out_axis)
        {
            const int ia = need(a), ib = need(b), ic = need(c), id = need(d);
            const int ip = need(p), iw = need(w);
            bool any = false;
            if (ia >= 0 && (int)r.size() > ia) any |= cell(r[(size_t)ia], out_axis.anchor_start);
            if (ib >= 0 && (int)r.size() > ib) any |= cell(r[(size_t)ib], out_axis.anchor_end);
            if (ic >= 0 && (int)r.size() > ic) any |= cell(r[(size_t)ic], out_axis.offset_start);
            if (id >= 0 && (int)r.size() > id) any |= cell(r[(size_t)id], out_axis.offset_end);
            if (ip >= 0 && (int)r.size() > ip) cell(r[(size_t)ip], out_axis.pivot);
            if (iw >= 0 && (int)r.size() > iw) cell(r[(size_t)iw], out_axis.weight);
            out_axis.present = any;
        };
        ax("h_anchor_start", "h_anchor_end", "h_offset_start", "h_offset_end",
           "h_pivot", "h_weight", e.h);
        ax("v_anchor_start", "v_anchor_end", "v_offset_start", "v_offset_end",
           "v_pivot", "v_weight", e.v);

        const int cSt = need("stack_orientation"), cSp = need("item_spacing");
        if (cSt >= 0 && (int)r.size() > cSt && !r[(size_t)cSt].empty())
            e.stack_orientation = atoi(r[(size_t)cSt].c_str());
        if (cSp >= 0 && (int)r.size() > cSp && !r[(size_t)cSp].empty())
            e.item_spacing = (float)atof(r[(size_t)cSp].c_str());
        const char* pads[4] = { "pad_l", "pad_t", "pad_r", "pad_b" };
        float* dst[4] = { &e.pad_l, &e.pad_t, &e.pad_r, &e.pad_b };
        for (int i = 0; i < 4; i++)
        {
            const int ci = need(pads[i]);
            if (ci >= 0 && (int)r.size() > ci) cell(r[(size_t)ci], *dst[i]);
        }
        const int cRef = need("references_widget");
        if (cRef >= 0 && (int)r.size() > cRef) e.references_widget = r[(size_t)cRef];

        s.elements.push_back(std::move(e));
    }
    return true;
}

namespace {

// "common/ui/.../foo.ebx" -> "common/ui/.../foo", so a reference compares
// equal to the partition key it names.
std::string strip_ebx(std::string p)
{
    for (char& ch : p) ch = (char)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
    if (p.size() > 4 && p.compare(p.size() - 4, 4, ".ebx") == 0) p.resize(p.size() - 4);
    return p;
}

void inline_tree(const std::vector<Screen>& all,
                 const std::map<std::string, size_t>& index,
                 size_t src, int base_depth, int depth_left,
                 std::vector<std::string>& open,
                 Screen& out, int* unresolved)
{
    const Screen& s = all[src];
    for (const Element& e : s.elements)
    {
        Element c = e;
        c.depth = base_depth + e.depth;
        out.elements.push_back(c);

        if (e.kind != Kind::WidgetReference || e.references_widget.empty()) continue;
        const std::string key = strip_ebx(e.references_widget);

        auto it = index.find(key);
        if (it == index.end()) { if (unresolved) (*unresolved)++; continue; }
        if (depth_left <= 0) continue;

        // A widget that reaches itself would recurse forever. Cheap guard: a
        // partition already open on this path is not entered again.
        bool cycle = false;
        for (const std::string& o : open) if (o == key) { cycle = true; break; }
        if (cycle) continue;

        open.push_back(key);
        inline_tree(all, index, it->second, c.depth + 1, depth_left - 1, open, out, unresolved);
        open.pop_back();
    }
}

}  // namespace

Screen instantiate(const std::vector<Screen>& all, size_t root, int max_depth,
                   int* unresolved)
{
    Screen out;
    if (root >= all.size()) return out;
    out.partition = all[root].partition;
    out.conditional_float_bindings = all[root].conditional_float_bindings;
    out.interface_text_graphs = all[root].interface_text_graphs;
    if (unresolved) *unresolved = 0;

    std::map<std::string, size_t> index;
    for (size_t i = 0; i < all.size(); i++) index[strip_ebx(all[i].partition)] = i;

    std::vector<std::string> open;
    open.push_back(strip_ebx(all[root].partition));
    inline_tree(all, index, root, 0, max_depth, open, out, unresolved);
    return out;
}

void solve(Screen& s, float canvas_w, float canvas_h)
{
    // Live rows already carry the exact parent selected by bf6_rime_tree.
    // This matters at expanded widget references: the referenced layer itself
    // is not emitted, so rebuilding parents from depth can attach its content
    // to a preceding sibling and silently drop the reference's translation.
    // Evidence TSVs predate the parent column and still need depth fallback.
    bool explicit_parents = false;
    bool parent_graph_valid = true;
    for (size_t i = 0; i < s.elements.size(); ++i)
    {
        const int p = s.elements[i].parent;
        if (p < 0) continue;
        explicit_parents = true;
        if ((size_t)p >= i || s.elements[(size_t)p].depth >= s.elements[i].depth)
        {
            parent_graph_valid = false;
            break;
        }
    }
    if (!explicit_parents || !parent_graph_valid)
    {
        std::vector<int> stack;
        for (size_t i = 0; i < s.elements.size(); i++)
        {
            Element& e = s.elements[i];
            while (!stack.empty() && s.elements[(size_t)stack.back()].depth >= e.depth)
                stack.pop_back();
            e.parent = stack.empty() ? -1 : stack.back();
            stack.push_back((int)i);
        }
    }

    auto axis = [](const Axis& a, float parent_start, float parent_size,
                   bool fit, float authored_size, float& start, float& size)
    {
        if (!a.present) { start = parent_start; size = parent_size; return; }
        const float slot_start = parent_start + a.anchor_start * parent_size + a.offset_start;
        const float slot_end   = parent_start + a.anchor_end   * parent_size - a.offset_end;
        // Content measurement is not yet part of this layout-only renderer;
        // use the authored cache for fit axes and expose the limitation in UI.
        if (fit || a.anchor_start == a.anchor_end) size = authored_size;
        else size = slot_end - slot_start;
        start = slot_start + (slot_end - slot_start - size) * a.pivot;
    };

    std::vector<std::vector<int>> children(s.elements.size());
    std::vector<int> roots;
    for (size_t i = 0; i < s.elements.size(); ++i)
    {
        const int p = s.elements[i].parent;
        if (p >= 0 && p < (int)s.elements.size()) children[(size_t)p].push_back((int)i);
        else roots.push_back((int)i);
    }

    // Fit-to-content is not the element's cached editor Width/Height. Widget
    // references commonly retain the 1920px design canvas in Width even when
    // their actual content is a 600px row. That exact case is the armory's
    // cosmetic footer: three 192px cells with two 12px gaps. Using the cached
    // width made the footer 1920px wide and displaced the dots beside it.
    //
    // Measure descendants before assigning boxes. Only axes explicitly marked
    // FitToContent consume the measured value; ordinary and point-anchored
    // elements keep the authored width/height, so this cannot silently resize
    // the rest of the screen. Rime stacks sequence in reverse for placement,
    // but reversal does not change their measured extent.
    struct Extent {
        float w = 0.f, h = 0.f;
        /* Actual box size and desired-content contribution are different in
         * Rime.  A fixed 256x256 fill has a real box once its parent is
         * solved, but it must not make a FitToContent overlay 256px tall.
         * Conversely a fit label or a visible-overflow stack does advertise
         * its measured content to its parent. */
        bool desired_w = false, desired_h = false;
        bool done = false;
    };
    std::vector<Extent> measured(s.elements.size());
    std::function<Extent(int)> measure = [&](int index) -> Extent
    {
        Extent& cached = measured[(size_t)index];
        if (cached.done) return cached;
        const Element& e = s.elements[(size_t)index];
        cached.w = e.kind == Kind::VectorShape && e.shape_size[0] > 0.f
                     ? e.shape_size[0] : e.width;
        cached.h = e.kind == Kind::VectorShape && e.shape_size[1] > 0.f
                     ? e.shape_size[1] : e.height;

        const std::vector<int>& kids = children[(size_t)index];
        if (!kids.empty())
        {
            float content_w = 0.f, content_h = 0.f;
            int visible = 0;
            bool desired_w = false, desired_h = false;
            if (e.kind == Kind::StackContainer && e.stack_orientation == 0)
            {
                for (int child : kids)
                {
                    if (!s.elements[(size_t)child].visible) continue;
                    const Extent m = measure(child);
                    /* Stack sequencing consumes each immediate child's real
                     * box.  Desired flags only control whether that finished
                     * stack is allowed to escape through another fit
                     * container. */
                    content_w += m.w;
                    content_h = std::max(content_h, m.h);
                    visible++;
                }
                if (visible > 1) content_w += e.item_spacing * (visible - 1);
            }
            else if (e.kind == Kind::StackContainer && e.stack_orientation == 1)
            {
                for (int child : kids)
                {
                    if (!s.elements[(size_t)child].visible) continue;
                    const Extent m = measure(child);
                    content_w = std::max(content_w, m.w);
                    content_h += m.h;
                    visible++;
                }
                if (visible > 1) content_h += e.item_spacing * (visible - 1);
            }
            else
            {
                /* Multiple children of an ordinary container are overlays.
                 * Only a child which explicitly has desired content may size
                 * a fit overlay.  Using every child's actual authored box is
                 * the source of the 256x256 editor-placeholder leak in
                 * CL_Tag and several full-screen Rime composites. */
                for (int child : kids)
                {
                    const Element& childElement =
                        s.elements[(size_t)child];
                    if (!childElement.visible) continue;
                    const Extent m = measure(child);
                    ++visible;
                    /* A child stretched across an ordinary overlay axis is
                     * a consumer of the parent's slot, not a measurement of
                     * that slot.  Treating the armory Header reference's
                     * stretched 158px cache as desired content shrank its
                     * authored 350px Package Info envelope and pushed every
                     * header to the bottom.  Point-anchored children and text
                     * retain intrinsic measurement; a stretched control is
                     * deliberately excluded on that axis. */
                    const bool intrinsicWidth =
                        childElement.kind == Kind::Label ||
                        (childElement.h.present &&
                         std::fabs(childElement.h.anchor_start -
                                   childElement.h.anchor_end) < 0.0001f);
                    const bool intrinsicHeight =
                        childElement.kind == Kind::Label ||
                        childElement.kind == Kind::StackContainer ||
                        (childElement.v.present &&
                         std::fabs(childElement.v.anchor_start -
                                   childElement.v.anchor_end) < 0.0001f);
                    if (m.desired_w && intrinsicWidth)
                    {
                        content_w = std::max(content_w, m.w);
                        desired_w = true;
                    }
                    if (m.desired_h && intrinsicHeight)
                    {
                        content_h = std::max(content_h, m.h);
                        desired_h = true;
                    }
                }
            }
            /* A stack's desired extent on its MAIN axis is the run of its
             * immediate visible children.  This is distinct from recursively
             * propagating overflow through every fixed container: the latter
             * was disproved by the armory reference because Title's authored
             * 824px box leaked through a fixed 600px vertical stack.  Here the
             * 256px Horizontal Icon Container advertises its one visible
             * immediate child (Headers, 600px) to the fit-width parent, while
             * Headers still advertises its own fixed 600px cross-axis size.
             * The stack's ACTUAL box remains authored below unless its axis is
             * itself FitToContent. Wrap/compress need their separate laws. */
            if (e.kind == Kind::StackContainer &&
                e.stack_overflow_mode == 0 &&
                e.stack_orientation == 0 && content_w > 0.f)
            {
                /* Children consume the stack's padded content box, so its
                 * fitted outer main-axis extent includes those same insets.
                 * The installed UM_CollapseButton ContentContainer proves
                 * the real case: 12 px left + 12 px right. Omitting them
                 * starts each following class tab inside the previous text. */
                cached.w = (std::max)(
                    0.f, content_w + e.pad_l + e.pad_r);
                cached.desired_w = true;
            }
            else if (e.fit_w)
            {
                if (e.kind == Kind::WidgetReference &&
                    e.widget_use_width == 0)
                    cached.desired_w = true;
                else if (e.kind == Kind::StackContainer || desired_w)
                {
                    cached.w = content_w;
                    cached.desired_w = true;
                }
                else if (visible == 0)
                {
                    cached.w = 0.f;
                    cached.desired_w = true;
                }
            }
            if (e.kind == Kind::StackContainer &&
                e.stack_overflow_mode == 0 &&
                e.stack_orientation == 1 && content_h > 0.f)
            {
                cached.h = content_h;
                cached.desired_h = true;
            }
            else if (e.fit_h)
            {
                if (e.kind == Kind::WidgetReference &&
                    e.widget_use_height == 0)
                    cached.desired_h = true;
                else if (e.kind == Kind::StackContainer || desired_h)
                {
                    cached.h = content_h;
                    cached.desired_h = true;
                }
                else if (visible == 0)
                {
                    cached.h = 0.f;
                    cached.desired_h = true;
                }
            }
        }
        else
        {
            /* Leaf labels/vectors and runtime-populated list placeholders
             * carry their measured size in Width/Height. */
            cached.desired_w = e.fit_w;
            cached.desired_h = e.fit_h;
        }
        /* A point-anchored axis has an intrinsic box; a stretched axis is a
         * backdrop/layout result and cannot size its own parent. Labels are
         * the exception on the cross axis: their authored wrap width is the
         * desired width of a fit text container (the weapon description is
         * the shipped 600px proof case). */
        if (!e.fit_w &&
            (e.kind == Kind::Label ||
             (e.h.present && e.h.anchor_start == e.h.anchor_end)))
            cached.desired_w = true;
        if (!e.fit_h &&
            (e.kind == Kind::Label ||
             (e.v.present && e.v.anchor_start == e.v.anchor_end)))
            cached.desired_h = true;
        cached.done = true;
        return cached;
    };
    for (size_t i = 0; i < s.elements.size(); ++i) measure((int)i);

    auto solve_box = [&](int index, Element& e, float px0, float py0, float px1, float py1,
                         bool parent_visible)
    {
        const float pw = px1 - px0, ph = py1 - py0;
        float w = 0.f, h = 0.f;
        // Filled/stroked vector assets ship their intrinsic design box. For a
        // fit-to-content vector this is the content size; 256 is merely the
        // untouched editor default and produced the giant input-icon panels
        // seen in the first live capture.
        const bool effective_fit_w =
            e.fit_w && measured[(size_t)index].desired_w;
        const bool effective_fit_h =
            e.fit_h && measured[(size_t)index].desired_h;
        const float intrinsic_w = effective_fit_w ? measured[(size_t)index].w :
            (e.kind == Kind::VectorShape && e.shape_size[0] > 0.f
                ? e.shape_size[0] : e.width);
        const float intrinsic_h = effective_fit_h ? measured[(size_t)index].h :
            (e.kind == Kind::VectorShape && e.shape_size[1] > 0.f
                ? e.shape_size[1] : e.height);
        /* A fit axis with no desired child is a stretch-through overlay, not
         * a request to resurrect its 256px editor cache. Point anchors remain
         * self-sized through axis()'s ordinary point-anchor rule. */
        axis(e.h, px0, pw, effective_fit_w, intrinsic_w, e.x0, w);
        axis(e.v, py0, ph, effective_fit_h, intrinsic_h, e.y0, h);
        e.x1 = e.x0 + w; e.y1 = e.y0 + h;
        // Visible is local state. Expanded widget references retain their
        // descendants' authored local flags, so a visible child beneath an
        // invisible state/reference is still effectively hidden. Treating
        // every row independently painted all of the inactive input-icon
        // variants at once (including the 256px TextIcon template slab).
        e.solved = parent_visible && e.visible && w >= 0.f && h >= 0.f;
    };

    std::function<void(int)> descend = [&](int index)
    {
        Element& p = s.elements[(size_t)index];
        const float px0 = p.x0 + p.pad_l, py0 = p.y0 + p.pad_t;
        const float px1 = p.x1 - p.pad_r, py1 = p.y1 - p.pad_b;
        std::vector<int>& kids = children[(size_t)index];

        // First determine each child's authored/self-sized box. A stack owns
        // only the main-axis position; its cross axis is still the ordinary
        // anchor law. bf6_rime_tree deliberately emits stack children in
        // Rime's reverse-Elements presentation order (rime_ext.inc passes
        // ti->stack to walk_array). Reversing them here a second time put the
        // armory dots on the left and its cosmetic footer on the right.
        for (int child : kids)
            solve_box(child, s.elements[(size_t)child], px0, py0, px1, py1, p.solved);

        if (p.kind == Kind::StackContainer && !kids.empty() &&
            (p.stack_orientation == 0 || p.stack_orientation == 1))
        {
            std::vector<int> visibleKids;
            for (int child : kids)
            {
                Element& e = s.elements[(size_t)child];
                // A Rime stack contains all authored state variants, but only
                // locally visible children participate in its layout. Moving
                // the cursor for hidden alternatives inserted their cached
                // 256px design boxes ahead of the selected armory labels.
                if (!e.visible) continue;
                visibleKids.push_back(child);
            }
            /* RimeFlowDirection is Default=0, Reverse=1,
             * TextDirection=2 in the installed runtime.  The tree walker
             * already converts the serialized Elements array to its normal
             * presentation order; Reverse is an additional authored layout
             * operation.  weaponattributes uses it to put the mastery badge
             * above, and the compact stat panel below, in a vertical stack. */
            if (p.container_flow_direction == 1)
                std::reverse(visibleKids.begin(), visibleKids.end());

            const bool horizontal = p.stack_orientation == 0;
            const float contentStart = horizontal ? px0 : py0;
            const float contentEnd = horizontal ? px1 : py1;
            const float contentSize = (std::max)(0.f, contentEnd - contentStart);
            const size_t count = visibleKids.size();

            /* These decoded Rime fields are authored layout inputs.  The old
             * renderer read them but always packed at the content origin,
             * collapsing whole action rows, navigation groups and headers.
             * SizeDistribution is None=0, Equally=1, Proportionally=2,
             * Static=3.  In proportional mode zero-weight children keep
             * their intrinsic size and positive weights share what remains. */
            if (count > 0 &&
                (p.stack_size_distribution == 1 ||
                 p.stack_size_distribution == 2))
            {
                const float gaps = count > 1
                    ? p.item_spacing * (float)(count - 1) : 0.f;
                float fixed = 0.f;
                float totalWeight = 0.f;
                for (int child : visibleKids)
                {
                    Element& e = s.elements[(size_t)child];
                    const float weight = horizontal ? e.h.weight : e.v.weight;
                    if (p.stack_size_distribution == 1)
                        totalWeight += 1.f;
                    else if (weight > 0.f)
                        totalWeight += weight;
                    else
                        fixed += horizontal ? e.x1 - e.x0 : e.y1 - e.y0;
                }
                const float shareable =
                    (std::max)(0.f, contentSize - gaps - fixed);
                if (totalWeight > 0.f)
                    for (int child : visibleKids)
                    {
                        Element& e = s.elements[(size_t)child];
                        const float weight = p.stack_size_distribution == 1
                            ? 1.f : (horizontal ? e.h.weight : e.v.weight);
                        if (weight <= 0.f) continue;
                        const float size = shareable * weight / totalWeight;
                        if (horizontal) e.x1 = e.x0 + size;
                        else e.y1 = e.y0 + size;
                    }
            }

            float occupied = count > 1
                ? p.item_spacing * (float)(count - 1) : 0.f;
            for (int child : visibleKids)
            {
                const Element& e = s.elements[(size_t)child];
                occupied += horizontal ? e.x1 - e.x0 : e.y1 - e.y0;
            }
            const float unused = (std::max)(0.f, contentSize - occupied);
            float lead = 0.f;
            float distributedGap = 0.f;
            /* SpaceDistribution is Packed=0, SpaceBetween=1, PadSides=2,
             * Evenly=3.  ItemSpacing remains the authored minimum gap. */
            if (p.stack_space_distribution == 1 && count > 1)
                distributedGap = unused / (float)(count - 1);
            else if (p.stack_space_distribution == 2)
                lead = unused * 0.5f;
            else if (p.stack_space_distribution == 3)
            {
                distributedGap = unused / (float)(count + 1);
                lead = distributedGap;
            }

            float cursor = contentStart + lead;
            bool placedAny = false;
            for (int child : visibleKids)
            {
                Element& e = s.elements[(size_t)child];
                if (placedAny) cursor += p.item_spacing + distributedGap;
                if (horizontal)
                {
                    const float size = e.x1 - e.x0;
                    e.x0 = cursor; e.x1 = cursor + size;
                    cursor += size;
                }
                else
                {
                    const float size = e.y1 - e.y0;
                    e.y0 = cursor; e.y1 = cursor + size;
                    cursor += size;
                }
                placedAny = true;
            }
        }
        for (int child : kids) descend(child);
    };

    for (int root : roots)
    {
        solve_box(root, s.elements[(size_t)root], 0.f, 0.f, canvas_w, canvas_h, true);
        descend(root);
    }
}

bool uniform_grid_layout(const Element& grid, const Screen& item,
                         UniformGridLayout& out)
{
    out = UniformGridLayout{};
    if (!grid.solved || grid.item_template.empty() || item.elements.empty() ||
        (!item.partition.empty() && item.partition != grid.item_template))
        return false;

    // These are the complete shipped enum domains.  Treating an unknown as a
    // familiar default makes a patched grid look convincing while following
    // a different layout rule.
    if (grid.grid_segment_distribution < 0 ||
        grid.grid_segment_distribution > 2 ||
        grid.grid_segment_count_mode < 0 ||
        grid.grid_segment_count_mode > 1 ||
        grid.grid_column_flow_direction < 0 ||
        grid.grid_column_flow_direction > 2 ||
        grid.grid_row_flow_direction < 0 ||
        grid.grid_row_flow_direction > 2)
        return false;

    const float contentW = grid.x1 - grid.x0 - grid.pad_l - grid.pad_r;
    const float contentH = grid.y1 - grid.y0 - grid.pad_t - grid.pad_b;
    if (!(contentW > 0.f) || !(contentH > 0.f) ||
        grid.grid_column_spacing < 0.f || grid.grid_row_spacing < 0.f)
        return false;

    int layer = -1;
    for (size_t i = 0; i < item.elements.size(); ++i)
        if (item.elements[i].parent < 0)
        {
            if (layer >= 0) return false;
            layer = (int)i;
        }
    if (layer < 0) return false;

    // The cell partition has several depth-one binding siblings.  The one
    // point-anchored on both axes is a stable coordinate reference, but its
    // 152x152 cached size is NOT assumed to be the card or slot size.
    int normalizationRoot = -1;
    int depthOneRoot = -1;
    int depthOneCount = 0;
    float preferredW = 0.f, preferredH = 0.f;
    for (size_t i = 0; i < item.elements.size(); ++i)
    {
        const Element& e = item.elements[i];
        if (e.parent != layer) continue;
        depthOneRoot = (int)i;
        ++depthOneCount;
        preferredW = (std::max)(preferredW, e.width);
        preferredH = (std::max)(preferredH, e.height);
        const bool pointH = e.h.present &&
            std::fabs(e.h.anchor_start - e.h.anchor_end) < 0.0001f;
        const bool pointV = e.v.present &&
            std::fabs(e.v.anchor_start - e.v.anchor_end) < 0.0001f;
        const bool centred = pointH && pointV &&
            std::fabs(e.h.anchor_start - 0.5f) < 0.0001f &&
            std::fabs(e.v.anchor_start - 0.5f) < 0.0001f;
        if (e.kind == Kind::WidgetReference && centred)
        {
            if (normalizationRoot >= 0) return false;
            normalizationRoot = (int)i;
        }
    }
    /* Some shipped list cells (the attachment-selection cell in the current
     * build) have one unambiguous depth-one content envelope rather than the
     * grid-item cell's several siblings plus centred reference.  Its sole
     * envelope is the only possible coordinate root; multiple non-centred
     * siblings remain rejected instead of selecting one by name/order. */
    if (normalizationRoot < 0 && depthOneCount == 1)
    {
        normalizationRoot = depthOneRoot;

        /* The attachment-selection cell is a full-parent flow envelope whose
         * own 256x256 Width/Height are untouched Rime defaults.  Its measured
         * content is the one centred widget reference immediately inside the
         * envelope (CL Grid Item: 166x142 in the current install).  Taking the
         * envelope's defaults produced four 256px cards; the game's authored
         * DynamicCount law yields six 166px cards in the 1101px grid.
         *
         * Require one and only one centred referenced child.  This is a
         * structural content-measurement path, not a partition/name special
         * case, and ambiguous envelopes continue to fail closed. */
        int measuredContent = -1;
        for (size_t i = 0; i < item.elements.size(); ++i)
        {
            const Element& e = item.elements[i];
            if (e.parent != depthOneRoot || e.references_widget.empty())
                continue;
            const bool pointH = e.h.present &&
                std::fabs(e.h.anchor_start - e.h.anchor_end) < 0.0001f;
            const bool pointV = e.v.present &&
                std::fabs(e.v.anchor_start - e.v.anchor_end) < 0.0001f;
            const bool centred = pointH && pointV &&
                std::fabs(e.h.anchor_start - 0.5f) < 0.0001f &&
                std::fabs(e.v.anchor_start - 0.5f) < 0.0001f;
            if (!centred) continue;
            if (measuredContent >= 0) return false;
            measuredContent = (int)i;
        }
        if (measuredContent >= 0)
        {
            preferredW = item.elements[(size_t)measuredContent].width;
            preferredH = item.elements[(size_t)measuredContent].height;
        }
    }
    if (normalizationRoot < 0) return false;

    // A zero authored segment size delegates the preferred size to the item
    // only when ItemFitContent is set.  The shipped weapon grid takes this
    // route: the depth-one binding envelope is 256x152.  No child is resized.
    if (grid.grid_column_size > 0.f) preferredW = grid.grid_column_size;
    else if (grid.grid_item_fit_content != 1) return false;
    if (grid.grid_row_size > 0.f) preferredH = grid.grid_row_size;
    else if (grid.grid_item_fit_content != 1) return false;
    if (!(preferredW > 0.f) || !(preferredH > 0.f)) return false;

    const bool verticalStatic = grid.grid_orientation == 1 &&
        grid.grid_segment_count_mode == 0;
    int columns = 0;
    if (verticalStatic)
    {
        /* RimeOrientation::Vertical makes StaticSegmentItemCount the row
         * count.  The perpendicular axis is filled from the authored cell
         * width.  gamecardpagecell proves 1 row and four 352px columns in its
         * 1448px viewport; applying the count as columns showed only 1/4. */
        if (grid.grid_segment_distribution != 0) return false;
        /* The perpendicular run is the grid viewport.  Padding offsets the
         * first painted item but does not redefine the segment count: the
         * shipped 1448px page carries 12px edge padding yet authors four
         * 352px cells at 358px pitch. Subtracting both edges yields a false
         * three-card capacity. */
        const float perpendicularExtent = grid.x1 - grid.x0;
        columns = (int)std::floor((perpendicularExtent +
                                   grid.grid_column_spacing) /
                                  (preferredW + grid.grid_column_spacing));
    }
    else if (grid.grid_segment_count_mode == 0)
        columns = grid.grid_static_segment_item_count;
    else
        columns = (int)std::floor((contentW + grid.grid_column_spacing) /
                                  (preferredW + grid.grid_column_spacing));
    if (columns <= 0) return false;

    float cellW = preferredW;
    float columnSpacing = grid.grid_column_spacing;
    if (grid.grid_segment_distribution == 1)
        cellW = (contentW - columnSpacing * (columns - 1)) / columns;
    else if (grid.grid_segment_distribution == 2 && columns > 1)
        columnSpacing = (contentW - cellW * columns) / (columns - 1);
    if (!(cellW > 0.f) || columnSpacing < 0.f) return false;

    const int visibleRows = verticalStatic
        ? grid.grid_static_segment_item_count
        : (int)std::floor((contentH + grid.grid_row_spacing) /
                          (preferredH + grid.grid_row_spacing));
    if (visibleRows <= 0) return false;

    out.columns = columns;
    out.visible_rows = visibleRows;
    out.x0 = grid.x0 + grid.pad_l;
    out.y0 = grid.y0 + grid.pad_t;
    out.cell_w = cellW;
    out.cell_h = preferredH;
    out.column_spacing = columnSpacing;
    out.row_spacing = grid.grid_row_spacing;
    out.normalization_root = normalizationRoot;
    return true;
}

bool append_grid_item(const Screen& item, const UniformGridLayout& layout,
                      float cell_x, float cell_y, int runtime_state,
                      std::vector<Element>& out)
{
    if (layout.normalization_root < 0 ||
        layout.normalization_root >= (int)item.elements.size())
        return false;
    const Element& root = item.elements[(size_t)layout.normalization_root];
    if (!root.solved) return false;

    const float dx = cell_x - root.x0;
    const float dy = cell_y - root.y0;
    const int base = (int)out.size();
    out.reserve(out.size() + item.elements.size());
    for (const Element& source : item.elements)
    {
        Element e = source;
        if (e.parent >= 0) e.parent += base;
        if (e.solved)
        {
            e.x0 += dx; e.x1 += dx;
            e.y0 += dy; e.y1 += dy;
        }
        e.runtime_state = runtime_state;
        out.push_back(std::move(e));
    }
    return true;
}


bool load_solved_tsv(const char* path, std::vector<Screen>& out, std::string& err)
{
    FILE* f = fopen(path, "rb");
    if (!f) { err = std::string("cannot open ") + path; return false; }
    std::string all;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
    fclose(f);

    std::vector<std::string> lines;
    for (size_t a = 0; a < all.size();)
    {
        size_t b = all.find('\n', a);
        if (b == std::string::npos) b = all.size();
        lines.push_back(all.substr(a, b - a));
        a = b + 1;
    }
    if (lines.size() < 2) { err = "solved file has no rows"; return false; }

    std::map<std::string, int> col;
    {
        const std::vector<std::string> hh = split_tab(lines[0]);
        for (size_t i = 0; i < hh.size(); i++) col[hh[i]] = (int)i;
    }
    auto ix = [&](const char* c2) { return col.count(c2) ? col[c2] : -1; };
    const int cScr = ix("screen"), cDep = ix("depth"), cTyp = ix("element_type");
    const int cNam = ix("element_name"), cX = ix("x"), cY = ix("y");
    const int cW = ix("w"), cH = ix("h"), cVis = ix("visible");
    if (cTyp < 0 || cX < 0 || cY < 0 || cW < 0 || cH < 0)
    { err = "solved file missing x/y/w/h"; return false; }

    std::map<std::string, size_t> index;
    for (size_t li = 1; li < lines.size(); li++)
    {
        if (lines[li].empty()) continue;
        const std::vector<std::string> r = split_tab(lines[li]);
        if ((int)r.size() <= cH) continue;

        const std::string scr = cScr >= 0 ? r[(size_t)cScr] : std::string("screen");
        if (index.find(scr) == index.end())
        { index[scr] = out.size(); out.push_back(Screen{ scr, {} }); }
        Screen& s2 = out[index[scr]];

        Element e;
        e.type_name = r[(size_t)cTyp];
        e.kind = kind_of(e.type_name);
        if (cDep >= 0) e.depth = atoi(r[(size_t)cDep].c_str());
        if (cNam >= 0 && (int)r.size() > cNam) e.name = r[(size_t)cNam];

        const float x = (float)atof(r[(size_t)cX].c_str());
        const float y = (float)atof(r[(size_t)cY].c_str());
        const float w = (float)atof(r[(size_t)cW].c_str());
        const float hgt = (float)atof(r[(size_t)cH].c_str());
        e.x0 = x; e.y0 = y; e.x1 = x + w; e.y1 = y + hgt;

        // An element the game hides is not drawn. Skipping it here rather than
        // at draw time keeps the count honest.
        bool vis = true;
        if (cVis >= 0 && (int)r.size() > cVis)
        {
            const std::string v = r[(size_t)cVis];
            vis = !(v == "0" || v == "False" || v == "false");
        }
        e.solved = vis && w > 0.f && hgt > 0.f;
        s2.elements.push_back(std::move(e));
    }
    return true;
}


}  // namespace rime
