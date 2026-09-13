#include "rime_image_layout.h"

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char* label)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
}

bool near(float a, float b)
{
    return std::fabs(a - b) < 0.001f;
}

} // namespace

int main()
{
    using namespace rime_image;
    const Rect square{0.f, 0.f, 100.f, 100.f};
    const Uv full{};

    const Placement stretch = place(square, full, 200, 100, 0, -1, -1, -1);
    check(stretch.applied && near(stretch.destination.x1, 100.f) &&
          near(stretch.destination.y1, 100.f),
          "Stretch occupies the solved box without alignment inputs");

    const Placement fit = place(square, full, 200, 100, 1, 1, 1, 1);
    check(fit.applied && near(fit.destination.x0, 0.f) &&
          near(fit.destination.y0, 25.f) && near(fit.destination.x1, 100.f) &&
          near(fit.destination.y1, 75.f),
          "Fit preserves aspect and centers the contained image");

    const Placement fill = place(square, full, 200, 100, 2, 1, 1, 1);
    check(fill.applied && near(fill.destination.x0, 0.f) &&
          near(fill.destination.x1, 100.f) && near(fill.uv.u0, 0.25f) &&
          near(fill.uv.u1, 0.75f),
          "Fill center-crops UVs when ClipToBounds is authored");

    const Placement fill_top = place(square, full, 100, 200, 2, 0, 0, 1);
    check(fill_top.applied && near(fill_top.uv.v0, 0.f) &&
          near(fill_top.uv.v1, 0.5f),
          "Fill honors Top alignment while cropping");

    const Placement none_right = place(
        square, full, 20, 40, 3, 2, 2, 0);
    check(none_right.applied && near(none_right.destination.x0, 80.f) &&
          near(none_right.destination.y0, 60.f),
          "None keeps native pixel size and honors Right/Bottom alignment");

    const Placement subrect = place(
        square, Uv{0.25f, 0.f, 0.75f, 1.f}, 400, 100, 1, 1, 1, 1);
    check(subrect.applied && near(subrect.destination.y0, 25.f),
          "aspect calculation uses the authored UV sub-rectangle");

    const Placement reversed = place(
        square, Uv{1.f, 0.f, 0.f, 1.f}, 200, 100, 2, 1, 1, 1);
    check(reversed.applied && near(reversed.uv.u0, 0.75f) &&
          near(reversed.uv.u1, 0.25f),
          "cropping preserves reversed authored UV orientation");

    const Placement custom = place(square, full, 200, 100, 4, 1, 1, 1);
    check(!custom.applied && near(custom.destination.x1, 100.f),
          "Custom remains unchanged until its parameter schema is decoded");

    return failures ? 1 : 0;
}
