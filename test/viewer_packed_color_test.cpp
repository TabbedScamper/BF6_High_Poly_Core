#include "packed_color.h"

#include <cstdio>

int main()
{
    const ImU32 charcoal = IM_COL32(0x22, 0x31, 0x3C, 0xFF);
    const ImU32 focus = IM_COL32(0xBF, 0xCA, 0xD1, 0xFF);
    const ImU32 mixed = bf6_viewer::composite_packed_color(
        charcoal, focus, 0.08f);

    const int red = static_cast<int>(
        (mixed >> IM_COL32_R_SHIFT) & 0xFFu);
    const int green = static_cast<int>(
        (mixed >> IM_COL32_G_SHIFT) & 0xFFu);
    const int blue = static_cast<int>(
        (mixed >> IM_COL32_B_SHIFT) & 0xFFu);
    const ImU32 focusRgb = bf6_viewer::packed_color_to_rgb24(focus);
    std::printf("packed=%08X rgb=%02X%02X%02X focus-rgb=%06X "
                "shifts=%d/%d/%d\n",
                mixed, red, green, blue, focusRgb,
                IM_COL32_R_SHIFT, IM_COL32_G_SHIFT, IM_COL32_B_SHIFT);

    // 0x22/0x31/0x3C composited toward 0xBF/0xCA/0xD1 at 0.08.
    // The stock DX11 backend consumes the first packed byte as R through its
    // DXGI_FORMAT_R8G8B8A8_UNORM vertex declaration.  This raw-lane check
    // prevents IMGUI_USE_BGRA_PACKED_COLOR from silently returning and
    // turning the installed gold UI accents cyan.
    const bool stock_dx11_lanes =
        (focus & 0xFFu) == 0xBFu && ((focus >> 16) & 0xFFu) == 0xD1u;
    return red == 0x2F && green == 0x3D && blue == 0x48 &&
        focusRgb == 0xBFCAD1u && stock_dx11_lanes ? 0 : 1;
}
