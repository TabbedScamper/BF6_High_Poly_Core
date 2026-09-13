#include "bf6_core.h"
#include "rime.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* label)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
}

bool near(float a, float b, float tolerance = 0.00001f)
{
    return std::fabs(a - b) <= tolerance;
}

struct Expected {
    const char* asset;
    int frames;
    float rate;
};

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: rime_flipbook_live_test <game-dir>\n");
        return 2;
    }
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, (int)sizeof(error));
    if (!context || !bf6_mount_frontend(context, error, (int)sizeof(error)))
    {
        std::fprintf(stderr, "open/mount failed: %s\n", error);
        if (context) bf6_close(context);
        return 1;
    }

    const Expected expected[] = {
        {"common/ui/assets/images/spinners/loader_loading", 11, 15.f},
        {"common/ui/assets/images/spinners/spinner01_4k", 36, 24.f},
        {"common/ui/notification/assets/t_ui_flipbook_boxmosaic_large", 20, 60.f},
        {"common/ui/assets/images/logos/animatedlogotype/bfmonogramloop", 0, 0.f},
    };
    for (const Expected& item : expected)
    {
        bf6_rime_flipbook_info info{};
        const int count = bf6_rime_flipbook(
            context, item.asset, &info, nullptr, 0);
        std::vector<bf6_rime_flipbook_frame> frames(
            count > 0 ? (size_t)count : 0u);
        const int filled = count > 0
            ? bf6_rime_flipbook(context, item.asset, &info,
                                frames.data(), count)
            : count;
        std::printf("flipbook %s frames=%d rate=%.3f atlas=%s texture=%d\n",
                    item.asset, count, info.frame_rate, info.atlas_asset,
                    info.texture_id);
        check(count > 0 && filled == count && info.frame_count == count,
              "real flipbook returns a stable count/fill UV table");
        check(info.texture_id >= 0 &&
              std::string(info.atlas_asset).find("win32") != std::string::npos,
              "authored atlas import resolves to a native texture");
        if (item.frames > 0)
            check(count == item.frames && near(info.frame_rate, item.rate),
                  "measured frame count and rate match installed asset");
        bool valid = !frames.empty();
        for (const auto& frame : frames)
            valid = valid && frame.uv[0] >= 0.f && frame.uv[1] >= 0.f &&
                    frame.uv[2] <= 1.f && frame.uv[3] <= 1.f &&
                    frame.uv[2] > frame.uv[0] && frame.uv[3] > frame.uv[1];
        check(valid, "every authored frame is a valid ordered UV rectangle");
        if (std::strstr(item.asset, "bfmonogramloop") && !frames.empty())
        {
            bool found_inset = false;
            for (const auto& frame : frames)
                found_inset = found_inset ||
                    (near(frame.uv[0], 0.75255f, 0.0001f) &&
                     near(frame.uv[1], 0.75379f, 0.0001f) &&
                     near(frame.uv[2], 0.99745f, 0.0001f) &&
                     near(frame.uv[3], 0.99621f, 0.0001f));
            check(found_inset,
                  "BF monogram keeps its irregular inset authored UV cell");
        }
    }

    bf6_rime_flipbook_info control{};
    check(bf6_rime_flipbook(context,
          "common/ui/assets/images/spinners/__bf6_missing_flipbook__",
          &control, nullptr, 0) == -1,
          "fabricated asset fails closed as not mounted");
    check(bf6_rime_flipbook(context,
          "common/ui/assets/images/misc/t_ui_player_bot_icn",
          &control, nullptr, 0) == -1,
          "non-flipbook Rime image fails the concrete type gate");

    rime::Screen screen;
    rime::Element element;
    element.kind = rime::Kind::Flipbook;
    element.image_asset = expected[0].asset;
    element.flipbook_progress = 0.5f;
    screen.elements.push_back(element);
    rime::load_images(context, screen);
    const rime::Element& loaded = screen.elements[0];
    check(loaded.image_decode_status == 1 && loaded.texture_id >= 0 &&
          loaded.flipbook_frames.size() == 11 &&
          near(loaded.flipbook_frame_rate, 15.f),
          "viewer adapter retains atlas, frames, rate and decode status");
    check(rime::flipbook_frame_index(loaded, 99.0) == 5,
          "non-autoplay selection uses clamped authored Progress");

    rime::Element clocked = loaded;
    clocked.flipbook_auto_play = 1;
    clocked.flipbook_loop = 1;
    check(rime::flipbook_frame_index(clocked, 1.0) == 4,
          "autoplay loop advances at the authored frame rate");
    clocked.flipbook_loop = 0;
    check(rime::flipbook_frame_index(clocked, 99.0) == 10,
          "non-looping autoplay holds the final authored frame");
    clocked.flipbook_random_frames = 1;
    clocked.name_hash = 0x12345678u;
    check(rime::flipbook_frame_index(clocked, 2.0) ==
          rime::flipbook_frame_index(clocked, 2.0),
          "RandomFrames selection is stable within an authored animation tick");

    bf6_close(context);
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
