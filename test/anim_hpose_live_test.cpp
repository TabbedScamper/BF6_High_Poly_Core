#include "bf6_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static void multiply_affine(const float a[12], const float b[12], float out[12])
{
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            out[row * 3 + column] =
                a[row * 3 + 0] * b[0 * 3 + column] +
                a[row * 3 + 1] * b[1 * 3 + column] +
                a[row * 3 + 2] * b[2 * 3 + column];
    for (int column = 0; column < 3; ++column)
        out[9 + column] = a[9] * b[column] +
            a[10] * b[3 + column] + a[11] * b[6 + column] + b[9 + column];
}

static void set_rotation(float out[12], const float value[4])
{
    float x = value[0], y = value[1], z = value[2], w = value[3];
    const float length = std::sqrt(x*x + y*y + z*z + w*w);
    if (length > 1e-12f) { x/=length; y/=length; z/=length; w/=length; }
    out[0]=1.f-2.f*(y*y+z*z); out[1]=2.f*(x*y+z*w); out[2]=2.f*(x*z-y*w);
    out[3]=2.f*(x*y-z*w); out[4]=1.f-2.f*(x*x+z*z); out[5]=2.f*(y*z+x*w);
    out[6]=2.f*(x*z+y*w); out[7]=2.f*(y*z-x*w); out[8]=1.f-2.f*(x*x+y*y);
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: anim_hpose_live_test <game>\n"); return 2; }
    char error[1024]{};
    bf6_ctx* context = bf6_open(argv[1], error, (int)sizeof(error));
    if (!context) { std::fprintf(stderr, "%s\n", error); return 2; }
    const char* clipPath =
        "animations/glacier/assets/3p/weapons/rifles/m4a1/hposes_m4a1";
    const char* rigPath = "animations/glacier/global/rigging/soldier_3p.rig";
    const char* skeletonPath = "common/characters/_soldier/ske_soldier_3p";
    bf6_anim_binding_stats stats{};
    const int bindingCount = bf6_anim_bindings(context, clipPath, rigPath,
        skeletonPath, nullptr, 0, &stats);
    bf6_anim_clip* clip = bf6_anim_clip_open(context, clipPath);
    bf6_skeleton* skeleton = bf6_skeleton_compose(context, skeletonPath, nullptr);
    if (bindingCount <= 0 || !clip || !skeleton) {
        std::fprintf(stderr, "hpose graph unavailable\n");
        if (clip) bf6_free(context, clip);
        if (skeleton) bf6_free(context, skeleton);
        bf6_close(context); return 1;
    }
    std::vector<bf6_anim_binding> bindings((size_t)bindingCount);
    if (bf6_anim_bindings(context, clipPath, rigPath, skeletonPath,
            bindings.data(), bindingCount, &stats) != bindingCount) return 1;
    int left=-1, right=-1, align=-1, leftIk=-1, rightIk=-1;
    int leftShoulder=-1, leftArm=-1, leftForeArm=-1;
    int rightShoulder=-1, rightArm=-1, rightForeArm=-1;
    for (int bone=0; bone<skeleton->bone_count; ++bone) {
        const char* name=skeleton->bones[bone].name;
        if (!name) continue;
        if (!std::strcmp(name,"LeftHand")) left=bone;
        else if (!std::strcmp(name,"RightHand")) right=bone;
        else if (!std::strcmp(name,"LeftShoulder")) leftShoulder=bone;
        else if (!std::strcmp(name,"LeftArm")) leftArm=bone;
        else if (!std::strcmp(name,"LeftForeArm")) leftForeArm=bone;
        else if (!std::strcmp(name,"RightShoulder")) rightShoulder=bone;
        else if (!std::strcmp(name,"RightArm")) rightArm=bone;
        else if (!std::strcmp(name,"RightForeArm")) rightForeArm=bone;
        else if (!std::strcmp(name,"Wep_Align")) align=bone;
        else if (!std::strcmp(name,"Wep_IK_LeftHand")) leftIk=bone;
        else if (!std::strcmp(name,"Wep_IK_RightHand")) rightIk=bone;
    }
    const int weaponBoneCount = bf6_weapon_bones(context, nullptr, 0);
    std::vector<bf6_weapon_bone_info> weaponBones(
        weaponBoneCount > 0 ? (size_t)weaponBoneCount : 0);
    const int weaponBonesRead = weaponBoneCount > 0
        ? bf6_weapon_bones(context, weaponBones.data(), weaponBoneCount) : -1;
    int packedScalarBindings = 0;
    bool packedScalarLanes[2] = { false, false };
    for (const bf6_anim_binding& binding : bindings)
        if (binding.component == BF6_ANIM_DOF_SCALAR &&
            binding.channel == 239 && binding.lane >= 0 && binding.lane < 2)
        {
            ++packedScalarBindings;
            packedScalarLanes[binding.lane] = true;
        }
    bool ok = bindingCount == 241 && clip->key_time_count == 36 &&
        clip->framing == BF6_ANIM_DCT && clip->channel_count == 240 &&
        clip->quat_count == 145 && clip->vec3_count == 94 &&
        clip->group_count == 1 && left>=0 && right>=0 && align>=0 &&
        leftIk>=0 && rightIk>=0 &&
        leftShoulder>=0 && leftArm>=0 && leftForeArm>=0 &&
        rightShoulder>=0 && rightArm>=0 && rightForeArm>=0 &&
        skeleton->bones[leftArm].parent == leftShoulder &&
        skeleton->bones[leftForeArm].parent == leftArm &&
        skeleton->bones[left].parent == leftForeArm &&
        skeleton->bones[rightArm].parent == rightShoulder &&
        skeleton->bones[rightForeArm].parent == rightArm &&
        skeleton->bones[right].parent == rightForeArm &&
        stats.map_keys == 241 &&
        stats.resolved_rig == 241 && stats.resolved_storage == 241 &&
        stats.resolved_bones == 239 && stats.quaternion_bones == 145 &&
        stats.vector_bones == 94 && stats.scalar_bones == 0 &&
        packedScalarBindings == 2 && packedScalarLanes[0] &&
        packedScalarLanes[1] && weaponBoneCount == 66 &&
        weaponBonesRead == 66 && weaponBones[3].index == 3 &&
        std::strcmp(weaponBones[3].name, "Wep_Align") == 0;
    std::vector<float> sample((size_t)clip->channel_count*4);
    std::vector<bf6_bone_xform> local((size_t)skeleton->bone_count);
    std::vector<bf6_bone_xform> model((size_t)skeleton->bone_count);
    std::printf("hpose frames=%d bindings=%d decoded=%d resolved=%d q/v/s=%d/%d/%d\n",
        clip->key_time_count,bindingCount,clip->channel_count,stats.resolved_bones,
        stats.quaternion_bones,stats.vector_bones,stats.scalar_bones);
    const int armBones[] = { leftShoulder, leftArm, leftForeArm, left,
                             rightShoulder, rightArm, rightForeArm, right };
    for (int bone : armBones)
        if (bone >= 0)
            std::printf("arm hierarchy: [%d] %s parent=[%d] %s\n", bone,
                skeleton->bones[bone].name, skeleton->bones[bone].parent,
                skeleton->bones[bone].parent >= 0
                    ? skeleton->bones[skeleton->bones[bone].parent].name : "<root>");
    for (int frame=0; frame<clip->key_time_count && ok; ++frame) {
        ok = bf6_anim_clip_sample(context,clip,frame,sample.data(),nullptr)!=0;
        for (int bone=0; bone<skeleton->bone_count; ++bone)
            std::memcpy(local[(size_t)bone].m,skeleton->bones[bone].local,48);
        for (const bf6_anim_binding& binding:bindings) {
            if (binding.bone<0 || binding.bone>=skeleton->bone_count ||
                binding.component==BF6_ANIM_DOF_SCALAR) continue;
            const float* value=sample.data()+(size_t)binding.channel*4;
            float* destination=local[(size_t)binding.bone].m;
            if (binding.component==BF6_ANIM_DOF_QUATERNION) set_rotation(destination,value);
            else { destination[9]=value[0]; destination[10]=value[1]; destination[11]=value[2]; }
        }
        for (int bone=0; bone<skeleton->bone_count; ++bone) {
            const int parent=skeleton->bones[bone].parent;
            if (parent<0) std::memcpy(model[(size_t)bone].m,local[(size_t)bone].m,48);
            else multiply_affine(local[(size_t)bone].m,model[(size_t)parent].m,
                                 model[(size_t)bone].m);
        }
        const float* l=model[(size_t)left].m; const float* r=model[(size_t)right].m;
        const float dx=l[9]-r[9],dy=l[10]-r[10],dz=l[11]-r[11];
        const float separation=std::sqrt(dx*dx+dy*dy+dz*dz);
        std::printf("frame=%02d hands_mid=(% .4f % .4f % .4f) sep=%.4f "
                    "left=(% .4f % .4f % .4f) right=(% .4f % .4f % .4f)\n",
            frame,(l[9]+r[9])*.5f,(l[10]+r[10])*.5f,(l[11]+r[11])*.5f,separation,
            l[9],l[10],l[11],r[9],r[10],r[11]);
    }
    std::printf("RESULT=%s\n",ok?"PASS":"FAIL");
    bf6_free(context,skeleton); bf6_free(context,clip); bf6_close(context);
    return ok?0:1;
}
