#include "bf6_core.h"

#include <cstdint>
#include <cstdio>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: rime_start_logo_color_live_test <game-dir>\n");
        return 2;
    }
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, sizeof(error));
    if (!context || !bf6_mount_frontend(context, error, sizeof(error))) {
        std::fprintf(stderr, "%s\n", error);
        if (context) bf6_close(context);
        return 2;
    }

    const char* image =
        "common/ui/bootflow/assets/images/t_ui_startscreen_logo_dallas";
    char resource[512]{};
    const int resolved = bf6_rime_image_resource(
        context, image, resource, static_cast<int>(sizeof(resource)));
    const int texture_id = bf6_rime_texture_id(context, image);
    const bf6_texture* texture = texture_id >= 0
        ? bf6_texture_at_max_dim(context, texture_id, 512) : nullptr;

    constexpr int kSide = 256;
    std::vector<uint8_t> rgba(static_cast<size_t>(kSide) * kSide * 4u);
    const int decoded = resolved > 0
        ? bf6_layer_sheet(context, resource, kSide, rgba.data(), error,
                          static_cast<int>(sizeof(error)))
        : 0;
    int red_dominant = 0;
    int blue_dominant = 0;
    uint64_t red_energy = 0;
    uint64_t blue_energy = 0;
    if (decoded) {
        for (size_t i = 0; i < rgba.size(); i += 4) {
            const int r = rgba[i + 0];
            const int b = rgba[i + 2];
            const int a = rgba[i + 3];
            if (a < 32 || (r < 48 && b < 48)) continue;
            red_energy += static_cast<uint64_t>(r) * a;
            blue_energy += static_cast<uint64_t>(b) * a;
            red_dominant += r > b + 32;
            blue_dominant += b > r + 32;
        }
    }

    const int fake = bf6_rime_texture_id(
        context, "common/ui/bootflow/assets/images/not_a_real_logo");
    std::printf(
        "resource=%s texture=%d %dx%d fmt=%d srgb=%d decoded=%d "
        "red_pixels=%d blue_pixels=%d red_energy=%llu blue_energy=%llu fake=%d\n",
        resolved > 0 ? resource : "<unresolved>", texture_id,
        texture ? texture->width : 0, texture ? texture->height : 0,
        texture ? static_cast<int>(texture->format) : -1,
        texture ? texture->srgb : -1, decoded, red_dominant, blue_dominant,
        static_cast<unsigned long long>(red_energy),
        static_cast<unsigned long long>(blue_energy), fake);
    bf6_close(context);

    // This gate proves only that the exact authored ResourceId resolves and
    // that the native BC payload decodes. The channel census is diagnostic:
    // it deliberately does not bake a screenshot-derived colour expectation
    // into the reader.
    return resolved > 0 && texture && decoded && fake == -1 ? 0 : 1;
}
