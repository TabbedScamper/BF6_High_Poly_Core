#pragma once

#include <algorithm>
#include <cmath>

namespace rime_image {

struct Rect
{
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
};

struct Uv
{
    float u0 = 0.f, v0 = 0.f, u1 = 1.f, v1 = 1.f;
};

struct Placement
{
    Rect destination;
    Uv uv;
    bool visible = true;
    bool applied = false;
};

inline float aligned_origin(float low, float high, float extent, int alignment)
{
    if (alignment == 0) return low;             // Left / Top
    if (alignment == 1) return (low + high - extent) * 0.5f; // Center
    return high - extent;                       // Right / Bottom
}

inline Placement place(Rect bounds, Uv uv, int texture_width,
                       int texture_height, int resize_mode,
                       int horizontal_alignment, int vertical_alignment,
                       int clip_to_bounds)
{
    Placement result{bounds, uv, true, false};
    const float bounds_width = bounds.x1 - bounds.x0;
    const float bounds_height = bounds.y1 - bounds.y0;
    if (!(bounds_width > 0.f && bounds_height > 0.f))
    {
        result.visible = false;
        return result;
    }

    // Stretch is independent of source size and alignment. Unknown and
    // Custom retain the legacy solved-box presentation until Custom's extra
    // parameters have a decoded schema.
    if (resize_mode == 0)
    {
        result.applied = true;
        return result;
    }
    if (resize_mode < 1 || resize_mode > 3 ||
        texture_width <= 0 || texture_height <= 0 ||
        horizontal_alignment < 0 || horizontal_alignment > 2 ||
        vertical_alignment < 0 || vertical_alignment > 2)
        return result;

    const float source_width = texture_width * std::fabs(uv.u1 - uv.u0);
    const float source_height = texture_height * std::fabs(uv.v1 - uv.v0);
    if (!(source_width > 0.f && source_height > 0.f))
    {
        result.visible = false;
        return result;
    }

    float scale = 1.f;
    if (resize_mode == 1) // Fit
        scale = (std::min)(bounds_width / source_width,
                           bounds_height / source_height);
    else if (resize_mode == 2) // Fill
        scale = (std::max)(bounds_width / source_width,
                           bounds_height / source_height);
    // mode 3 is None: one source pixel occupies one reference-canvas unit.

    const float width = source_width * scale;
    const float height = source_height * scale;
    result.destination.x0 = aligned_origin(
        bounds.x0, bounds.x1, width, horizontal_alignment);
    result.destination.y0 = aligned_origin(
        bounds.y0, bounds.y1, height, vertical_alignment);
    result.destination.x1 = result.destination.x0 + width;
    result.destination.y1 = result.destination.y0 + height;
    result.applied = true;

    if (clip_to_bounds != 1) return result;

    const Rect unclipped = result.destination;
    result.destination.x0 = (std::max)(unclipped.x0, bounds.x0);
    result.destination.y0 = (std::max)(unclipped.y0, bounds.y0);
    result.destination.x1 = (std::min)(unclipped.x1, bounds.x1);
    result.destination.y1 = (std::min)(unclipped.y1, bounds.y1);
    if (!(result.destination.x1 > result.destination.x0 &&
          result.destination.y1 > result.destination.y0))
    {
        result.visible = false;
        return result;
    }

    const float du = uv.u1 - uv.u0;
    const float dv = uv.v1 - uv.v0;
    const float inv_width = 1.f / width;
    const float inv_height = 1.f / height;
    result.uv.u0 = uv.u0 +
        (result.destination.x0 - unclipped.x0) * inv_width * du;
    result.uv.u1 = uv.u0 +
        (result.destination.x1 - unclipped.x0) * inv_width * du;
    result.uv.v0 = uv.v0 +
        (result.destination.y0 - unclipped.y0) * inv_height * dv;
    result.uv.v1 = uv.v0 +
        (result.destination.y1 - unclipped.y0) * inv_height * dv;
    return result;
}

} // namespace rime_image
