#include "armory_camera_composition.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace armory_camera {
namespace {

Vec3 add(Vec3 a, Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Vec3 mul(Vec3 a, float s) { return {a.x*s, a.y*s, a.z*s}; }
float dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
float length(Vec3 a) { return std::sqrt(dot(a,a)); }
bool normalize(Vec3 a, Vec3& out) {
    const float n = length(a);
    if (!(n > 1e-7f) || !std::isfinite(n)) return false;
    out = mul(a, 1.f/n); return true;
}
Vec3 transform_direction(const Transform& t, Vec3 p) {
    return add(add(mul(t.right,p.x),mul(t.up,p.y)),mul(t.forward,p.z));
}
Vec3 rotate_xyz(Vec3 v, const float degrees[3]) {
    const float k = 3.14159265358979323846f / 180.f;
    const float cx=std::cos(degrees[0]*k), sx=std::sin(degrees[0]*k);
    const float cy=std::cos(degrees[1]*k), sy=std::sin(degrees[1]*k);
    const float cz=std::cos(degrees[2]*k), sz=std::sin(degrees[2]*k);
    Vec3 q{v.x, v.y*cx-v.z*sx, v.y*sx+v.z*cx};
    v = {q.x*cy+q.z*sy, q.y, -q.x*sy+q.z*cy};
    return {v.x*cz-v.y*sz, v.x*sz+v.y*cz, v.z};
}
void identity(float m[16]) { std::memset(m,0,16*sizeof(float)); m[0]=m[5]=m[10]=m[15]=1.f; }

} // namespace

bool read_weapon_bone(bf6_ctx* context, const char* model_definition,
                      const char* exact_bone_name, Transform& out)
{
    out = Transform{};
    bf6_bone_xform bone{};
    if (!bf6_weapon_bone_transform(context, model_definition, exact_bone_name, &bone))
        return false;
    out.right   = {bone.m[0], bone.m[1], bone.m[2]};
    out.up      = {bone.m[3], bone.m[4], bone.m[5]};
    out.forward = {bone.m[6], bone.m[7], bone.m[8]};
    out.position= {bone.m[9], bone.m[10], bone.m[11]};
    return std::isfinite(out.position.x) && std::isfinite(out.position.y) &&
           std::isfinite(out.position.z);
}

bool correction_for_mode(const bf6_armory_camera_weapon_correction& weapon,
                         const char* mode_name, Vec3& pre, Vec3& post)
{
    pre = {}; post = {};
    if (!mode_name || !*mode_name) return false;
    std::string mode(mode_name);
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : (char)c;
    });
    auto slot = [&](int i) {
        return Vec3{weapon.correction[i][0], weapon.correction[i][1],
                    weapon.correction[i][2]};
    };
    if (mode == "weaponsightbehavior") {
        pre = slot(BF6_ARMORY_CAMERA_SIGHT_POSITION);
        post = slot(BF6_ARMORY_CAMERA_SIGHT_LOOK_AT);
    } else if (mode == "weaponlazerbehavior")
        post = slot(BF6_ARMORY_CAMERA_TOP_RAIL_LOOK_AT);
    else if (mode == "weaponflashlightbehavior")
        post = slot(BF6_ARMORY_CAMERA_RIGHT_RAIL_LOOK_AT);
    else if (mode == "weaponleftrailbehavior")
        post = slot(BF6_ARMORY_CAMERA_LEFT_RAIL_LOOK_AT);
    else if (mode == "weaponmuzzlebehavior")
        post = slot(BF6_ARMORY_CAMERA_MUZZLE_LOOK_AT);
    else if (mode == "weaponunderbarrelbehavior")
        post = slot(BF6_ARMORY_CAMERA_UNDERBARREL_LOOK_AT);
    else if (mode == "weaponmagazinebehavior" || mode == "weaponergonomicbehavior")
        post = slot(BF6_ARMORY_CAMERA_MAGAZINE_LOOK_AT);
    else if (mode == "weaponbehavior" || mode == "weapononbenchbehavior" ||
             mode == "weaponrangefinderbehavior" || mode == "weaponcharmbehavior" ||
             mode == "weapondecalbehavior" || mode == "weapondecaloverviewbehavior" ||
             mode == "weaponskinbehavior" || mode == "weaponpackagebehavior" ||
             mode == "weaponcollectionbehavior") {
        /* These modes consume no member of the eight-slot graph. */
    } else return false;
    return true;
}

const char* attachment_mode_for_category(const char* category_name)
{
    if (!category_name || !*category_name) return nullptr;
    std::string category;
    for (const unsigned char ch : std::string(category_name))
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
            category.push_back((char)ch);
        else if (ch >= 'A' && ch <= 'Z')
            category.push_back((char)(ch + ('a' - 'A')));

    /* The direct rows are the one-to-one camera-anchor/bone families proven
     * by the shipped graphs.  The aliases are categories that share that same
     * physical mount: secondary optics use Sight, Barrel uses the front-end
     * Muzzle view, and ammunition/launcher use the magazine-well views. */
    if (category == "sight" || category == "opticaccessory")
        return "weaponsightbehavior";
    if (category == "muzzle" || category == "barrel")
        return "weaponmuzzlebehavior";
    if (category == "magazine" || category == "ammunition")
        return "weaponmagazinebehavior";
    if (category == "underbarrel")
        return "weaponunderbarrelbehavior";
    if (category == "laser" || category == "toprail")
        return "weaponlazerbehavior";
    if (category == "flashlight" || category == "rightrail")
        return "weaponflashlightbehavior";
    if (category == "leftrail")
        return "weaponleftrailbehavior";
    if (category == "rangefinder")
        return "weaponrangefinderbehavior";
    if (category == "launcher" || category == "ergonomic")
        return "weaponergonomicbehavior";
    return nullptr;
}

bool compose(const Input& in, Output& out)
{
    out = Output{};
    if (!(in.mode.focal_length_mm > 0.f) || !(in.sensor_width_mm > 0.f) ||
        !(in.sensor_height_mm > 0.f) || !(in.viewport_width > 0.f) ||
        !(in.viewport_height > 0.f) || !(in.near_plane > 0.f) ||
        !(in.far_plane > in.near_plane)) return false;

    Vec3 ar, au, af;
    if (!normalize(in.anchor.right,ar) || !normalize(in.anchor.up,au) ||
        !normalize(in.anchor.forward,af)) return false;
    Transform anchor{ar,au,af,in.anchor.position};
    const Vec3 pre = add({in.mode.pre_offset[0],in.mode.pre_offset[1],in.mode.pre_offset[2]},
                         in.weapon_pre_offset);
    Vec3 eye = add(anchor.position, transform_direction(anchor,pre));

    /* LookAt uses the authored target transform, not a mesh AABB centre.  The
     * pre-controller rotation survives as the reference-up/roll input even
     * though LookAt establishes the final forward direction. */
    Vec3 reference_up = rotate_xyz(anchor.up, in.mode.pre_rotation_degrees);
    Vec3 target = in.look_at.position;
    Vec3 forward;
    if (!normalize(sub(target,eye),forward)) return false;
    /* Frostbite camera transforms are right-handed: local +X is their right
     * basis and local +Z is camera-backward.  `forward` below deliberately
     * remains the DirectX/view direction (eye -> target), because projection
     * depth is positive in front of the camera.  Building right from forward
     * x up, and applying local Z against forward, is the exact bridge between
     * the two conventions. */
    Vec3 right;
    if (!normalize(cross(forward,reference_up),right)) return false;
    Vec3 up;
    if (!normalize(cross(right,forward),up)) return false;

    const Vec3 post_local = add({in.mode.post_offset[0],in.mode.post_offset[1],in.mode.post_offset[2]},
                                in.weapon_post_offset);
    const Vec3 post_world = in.post_space == PostOffsetSpace::World
        ? post_local
        : add(add(mul(right,post_local.x),mul(up,post_local.y)),
              mul(forward,-post_local.z));
    if (in.post_law == PostOffsetLaw::CameraDolly)
        eye = add(eye,post_world);
    else {
        target = add(target,post_world);
        if (!normalize(sub(target,eye),forward)) return false;
        if (!normalize(cross(forward,reference_up),right)) return false;
        if (!normalize(cross(right,forward),up)) return false;
    }

    right = rotate_xyz(right,in.mode.post_rotation_degrees);
    up = rotate_xyz(up,in.mode.post_rotation_degrees);
    forward = rotate_xyz(forward,in.mode.post_rotation_degrees);
    if (!normalize(right,right) || !normalize(up,up) || !normalize(forward,forward)) return false;

    const float sensor_hfov = 2.f*std::atan(in.sensor_width_mm/(2.f*in.mode.focal_length_mm));
    const float sensor_vfov = 2.f*std::atan(in.sensor_height_mm/(2.f*in.mode.focal_length_mm));
    const float aspect = in.viewport_width/in.viewport_height;
    float hfov=sensor_hfov, vfov=sensor_vfov;
    if (in.sensor_fit == SensorFit::Vertical)
        hfov = 2.f*std::atan(std::tan(vfov*0.5f)*aspect);
    else
        vfov = 2.f*std::atan(std::tan(hfov*0.5f)/aspect);

    out.eye=eye; out.right=right; out.up=up; out.forward=forward;
    out.focus_target=target; out.horizontal_fov_radians=hfov;
    out.vertical_fov_radians=vfov;
    out.autofocus_distance=(std::max)(in.near_plane,dot(sub(target,eye),forward));

    identity(out.view);
    out.view[0]=right.x; out.view[1]=up.x; out.view[2]=forward.x;
    out.view[4]=right.y; out.view[5]=up.y; out.view[6]=forward.y;
    out.view[8]=right.z; out.view[9]=up.z; out.view[10]=forward.z;
    out.view[12]=-dot(right,eye); out.view[13]=-dot(up,eye); out.view[14]=-dot(forward,eye);

    std::memset(out.projection,0,sizeof(out.projection));
    const float ys=1.f/std::tan(vfov*0.5f), xs=ys/aspect;
    out.projection[0]=xs; out.projection[5]=ys;
    out.projection[10]=in.far_plane/(in.far_plane-in.near_plane);
    out.projection[11]=1.f;
    out.projection[14]=-in.near_plane*in.far_plane/(in.far_plane-in.near_plane);
    return true;
}

bool project(const Output& c, const Vec3& world, float vx, float vy,
             float vw, float vh, Vec3& screen)
{
    if (!(vw>0.f) || !(vh>0.f)) return false;
    const Vec3 rel=sub(world,c.eye);
    const float x=dot(rel,c.right), y=dot(rel,c.up), z=dot(rel,c.forward);
    if (!(z>0.f)) return false;
    const float ndcx=(x/z)/std::tan(c.horizontal_fov_radians*0.5f);
    const float ndcy=(y/z)/std::tan(c.vertical_fov_radians*0.5f);
    screen={vx+(ndcx+1.f)*0.5f*vw, vy+(1.f-ndcy)*0.5f*vh, z};
    return std::isfinite(screen.x)&&std::isfinite(screen.y)&&std::isfinite(screen.z);
}

} // namespace armory_camera
