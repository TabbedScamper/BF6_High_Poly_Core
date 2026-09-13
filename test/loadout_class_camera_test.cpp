#include "bf6_core.h"

#include <cmath>
#include <cstdio>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: loadout_class_camera_test <game>\n");
        return 2;
    }
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, (int)sizeof(error));
    if (!context || !bf6_mount_frontend(context, error, (int)sizeof(error)))
    {
        std::fprintf(stderr, "open: %s\n", error);
        if (context) bf6_close(context);
        return 1;
    }
    bf6_loadout_class_camera camera{}, fake{};
    bf6_loadout_overview_camera overview{};
    const int ok = bf6_loadout_class_camera_read(context, &camera);
    const int null_control = bf6_loadout_class_camera_read(context, nullptr);
    std::printf("camera=%d classes=%d directions=%d selectors=%d/%d lens=%d "
                "focal=%.3f aperture=%.3f focus=%.3f shutter=%.3f null=%d\n",
        ok, camera.class_count, camera.direction_count,
        camera.position_selector_candidates,
        camera.look_at_selector_candidates, camera.lens_candidates,
        camera.focal_length_mm, camera.aperture, camera.focus_distance,
        camera.shutter_speed, null_control);
    for (int cls = 0; cls < camera.class_count; ++cls)
        std::printf("class=%d camera=(%.6f %.6f %.6f) "
                    "ltr=(%.6f %.6f %.6f) rtl=(%.6f %.6f %.6f)\n",
            cls, camera.camera_offset[cls][0], camera.camera_offset[cls][1],
            camera.camera_offset[cls][2],
            camera.look_at_offset[cls][0][0],
            camera.look_at_offset[cls][0][1],
            camera.look_at_offset[cls][0][2],
            camera.look_at_offset[cls][1][0],
            camera.look_at_offset[cls][1][1],
            camera.look_at_offset[cls][1][2]);
    const int overview_ok =
        bf6_loadout_overview_camera_read(context, &overview);
    std::printf("overview=%d position=(%.6f %.6f %.6f) "
                "position-rotation=(%.6f %.6f %.6f) "
                "ltr=(%.6f %.6f %.6f) rtl=(%.6f %.6f %.6f) "
                "ltr-rotation=(%.6f %.6f %.6f) "
                "rtl-rotation=(%.6f %.6f %.6f) "
                "directions=%d candidates=%d/%d lens=%d "
                "focal=%.3f aperture=%.3f focus=%.3f shutter=%.3f\n",
        overview_ok, overview.position[0], overview.position[1],
        overview.position[2], overview.position_rotation_degrees[0],
        overview.position_rotation_degrees[1],
        overview.position_rotation_degrees[2], overview.look_at[0][0],
        overview.look_at[0][1], overview.look_at[0][2],
        overview.look_at[1][0], overview.look_at[1][1],
        overview.look_at[1][2],
        overview.look_at_rotation_degrees[0][0],
        overview.look_at_rotation_degrees[0][1],
        overview.look_at_rotation_degrees[0][2],
        overview.look_at_rotation_degrees[1][0],
        overview.look_at_rotation_degrees[1][1],
        overview.look_at_rotation_degrees[1][2],
        overview.direction_count,
        overview.position_candidates, overview.look_at_selector_candidates,
        overview.lens_candidates, overview.focal_length_mm,
        overview.aperture, overview.focus_distance, overview.shutter_speed);
    const bool shape = ok && camera.class_count == 4 &&
        camera.direction_count == 2 &&
        camera.position_selector_candidates == 1 &&
        camera.look_at_selector_candidates == 1 &&
        camera.lens_candidates == 1 && null_control == 0 &&
        std::fabs(camera.focal_length_mm - 68.f) < .001f && overview_ok &&
        overview.direction_count == 2 &&
        overview.position_candidates == 1 &&
        overview.look_at_selector_candidates == 1 &&
        overview.lens_candidates == 1 &&
        std::fabs(overview.focal_length_mm - 70.f) < .001f &&
        std::fabs(overview.aperture - 1.f) < .001f &&
        std::fabs(overview.focus_distance - 2.f) < .001f &&
        std::fabs(overview.look_at_rotation_degrees[0][1] - 2.064684f) <
            .001f &&
        std::fabs(overview.look_at_rotation_degrees[1][1] + 15.58475f) <
            .001f;
    bf6_close(context);
    return shape ? 0 : 1;
}
