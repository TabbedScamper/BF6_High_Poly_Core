#ifndef BF6_VIEWER_ARMORY_CAMERA_COMPOSITION_H
#define BF6_VIEWER_ARMORY_CAMERA_COMPOSITION_H

#include "bf6_core.h"

namespace armory_camera {

struct Vec3 { float x = 0.f, y = 0.f, z = 0.f; };

/* Orthonormal world transform.  The caller supplies the fitted weapon bone
 * transforms; this layer does not substitute mesh bounds for them. */
struct Transform {
    Vec3 right{1.f, 0.f, 0.f};
    Vec3 up{0.f, 1.f, 0.f};
    Vec3 forward{0.f, 0.f, 1.f};
    Vec3 position{};
};

enum class SensorFit {
    /* Preserve the physical vertical gate and let horizontal FOV follow the
     * framebuffer aspect. */
    Vertical,
    /* Preserve the physical horizontal gate and let vertical FOV follow the
     * framebuffer aspect. */
    Horizontal
};

enum class PostOffsetLaw {
    /* Reproduction-step interpretation: OffsetCameraControllerData moves the
     * camera after LookAt and retains the orientation established by LookAt.
     * Engine execution has not yet distinguished this from the control below. */
    CameraDolly,
    /* Competing interpretation retained as an explicit control until engine
     * execution distinguishes it: the post offset moves the look-at target. */
    LookAtTarget
};

enum class PostOffsetSpace {
    CameraLocal,
    /* Some menu camera graphs serialize their offset in the shared
     * schematic/prefab frame rather than the camera basis. */
    World
};

struct Input {
    bf6_armory_camera_mode mode{};
    Transform anchor{};
    Transform look_at{};
    Vec3 weapon_pre_offset{};
    Vec3 weapon_post_offset{};
    float sensor_width_mm = 0.f;
    float sensor_height_mm = 0.f;
    float viewport_width = 0.f;
    float viewport_height = 0.f;
    float near_plane = 0.005f;
    float far_plane = 200.f;
    SensorFit sensor_fit = SensorFit::Vertical;
    PostOffsetLaw post_law = PostOffsetLaw::CameraDolly;
    PostOffsetSpace post_space = PostOffsetSpace::CameraLocal;
};

struct Output {
    Vec3 eye{};
    Vec3 right{};
    Vec3 up{};
    Vec3 forward{};
    Vec3 focus_target{};
    float horizontal_fov_radians = 0.f;
    float vertical_fov_radians = 0.f;
    float autofocus_distance = 0.f;
    /* Row-major, left-handed matrices. */
    float view[16]{};
    float projection[16]{};
};

/* Copy one exact fitted model-definition bone into composition space.  This
 * calls the live libbf6 pose reader; it never substitutes a mesh bound or a
 * viewer-authored attachment offset. */
bool read_weapon_bone(bf6_ctx* context, const char* model_definition,
                      const char* exact_bone_name, Transform& output);

/* Select the graph slots consumed by a shipped camera mode.  Unrecognised
 * modes deliberately return false instead of silently applying a nearby slot.
 * Overview modes are recognised and return zero corrections. */
bool correction_for_mode(const bf6_armory_camera_weapon_correction& weapon,
                         const char* mode, Vec3& pre, Vec3& post);

/* Map the live attachment-category label to the authored camera graph that
 * owns that slot family.  The returned name is only a lookup key: all camera
 * values still come from bf6_armory_camera_modes() at runtime.  Null is the
 * deliberate control for an unknown category. */
const char* attachment_mode_for_category(const char* category_name);

bool compose(const Input& input, Output& output);
bool project(const Output& camera, const Vec3& world,
             float viewport_x, float viewport_y,
             float viewport_width, float viewport_height,
             Vec3& screen);

} // namespace armory_camera

#endif
