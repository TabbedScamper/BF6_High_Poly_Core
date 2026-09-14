#pragma once

#include "imgui.h"

namespace bf6_viewer {

inline ImU32 composite_packed_color(ImU32 base, ImU32 over, float t)
{
    const auto mix = [&](int shift) {
        const int b = static_cast<int>((base >> shift) & 0xFFu);
        const int o = static_cast<int>((over >> shift) & 0xFFu);
        const int v = static_cast<int>(b + (o - b) * t + 0.5f);
        return static_cast<ImU32>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    return IM_COL32(mix(IM_COL32_R_SHIFT),
                    mix(IM_COL32_G_SHIFT),
                    mix(IM_COL32_B_SHIFT), 255);
}

inline ImU32 packed_color_to_rgb24(ImU32 color)
{
    return (((color >> IM_COL32_R_SHIFT) & 0xFFu) << 16) |
           (((color >> IM_COL32_G_SHIFT) & 0xFFu) << 8) |
           ((color >> IM_COL32_B_SHIFT) & 0xFFu);
}

} // namespace bf6_viewer
