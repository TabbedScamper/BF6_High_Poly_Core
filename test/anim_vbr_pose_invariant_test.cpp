/* VBR pose diagnostic for the BF6 frontend soldier.
 *
 * VBR stores four independently coded quaternion scalar lanes and normalises
 * the reconstructed float4 after IDCT. Consequently a pre-normalisation
 * magnitude near one is deliberately NOT an acceptance criterion. This test
 * instead checks the public output contract and the internal rigid-length
 * consistency of the candidate model-rotation/parent-local-translation path.
 * That length result does not prove the pose-space choice: visual capture is
 * a separate required gate and currently rejects this candidate. */
#include "bf6_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {
struct Vec3 { double x{}, y{}, z{}; };
struct Quat { double x{}, y{}, z{}, w{1.0}; };
struct Affine { double m[12]{}; };

double length(Vec3 v) { return std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z); }

Quat normalized(Quat q)
{
    const double n = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (!(n > 1e-20) || !std::isfinite(n)) return {};
    q.x /= n; q.y /= n; q.z /= n; q.w /= n;
    return q;
}

Quat multiply(Quat a, Quat b)
{
    return normalized({
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z});
}

Quat matrix_quaternion(const float* m)
{
    Quat q;
    const double trace = (double)m[0] + m[4] + m[8];
    if (trace > 0.0) {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        q.w = .25 * s; q.x = (m[7] - m[5]) / s;
        q.y = (m[2] - m[6]) / s; q.z = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const double s = std::sqrt(1.0 + m[0] - m[4] - m[8]) * 2.0;
        q.w = (m[7] - m[5]) / s; q.x = .25 * s;
        q.y = (m[1] + m[3]) / s; q.z = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        const double s = std::sqrt(1.0 + m[4] - m[0] - m[8]) * 2.0;
        q.w = (m[2] - m[6]) / s; q.x = (m[1] + m[3]) / s;
        q.y = .25 * s; q.z = (m[5] + m[7]) / s;
    } else {
        const double s = std::sqrt(1.0 + m[8] - m[0] - m[4]) * 2.0;
        q.w = (m[3] - m[1]) / s; q.x = (m[2] + m[6]) / s;
        q.y = (m[5] + m[7]) / s; q.z = .25 * s;
    }
    q = normalized(q);
    /* The branch above is the conventional column-vector extractor. Stored
     * Frostbite/DirectX rows are its transpose. */
    return {-q.x, -q.y, -q.z, q.w};
}

Affine rotation_affine(Quat q)
{
    q = normalized(q);
    const double xx=q.x*q.x, yy=q.y*q.y, zz=q.z*q.z;
    const double xy=q.x*q.y, xz=q.x*q.z, yz=q.y*q.z;
    const double wx=q.w*q.x, wy=q.w*q.y, wz=q.w*q.z;
    Affine a{};
    /* Frostbite/DirectX row-vector storage: rows are the transformed local
     * Right, Up and Forward axes, followed by Translation. */
    a.m[0]=1-2*(yy+zz); a.m[1]=2*(xy+wz);   a.m[2]=2*(xz-wy);
    a.m[3]=2*(xy-wz);   a.m[4]=1-2*(xx+zz); a.m[5]=2*(yz+wx);
    a.m[6]=2*(xz+wy);   a.m[7]=2*(yz-wx);   a.m[8]=1-2*(xx+yy);
    return a;
}

Affine copy_affine(const float* source)
{
    Affine out{};
    for (int i=0;i<12;++i) out.m[i]=source[i];
    return out;
}

Affine compose(Affine local, Affine parent)
{
    Affine out{};
    for (int row=0;row<4;++row)
    for (int column=0;column<3;++column) {
        for (int k=0;k<3;++k)
            out.m[row*3+column] += local.m[row*3+k]*parent.m[k*3+column];
        if (row==3) out.m[row*3+column] += parent.m[9+column];
    }
    return out;
}

double identity_error(Affine a)
{
    double worst=0.0;
    for (int row=0;row<4;++row)
    for (int column=0;column<3;++column) {
        const double expected = row==column ? 1.0 : 0.0;
        worst=std::max(worst,std::fabs(a.m[row*3+column]-expected));
    }
    return worst;
}

double normalized_basis_error(Affine candidate, const float* target)
{
    double worst=0.0;
    for (int row=0;row<3;++row) {
        const double n=std::sqrt((double)target[row*3]*target[row*3] +
            (double)target[row*3+1]*target[row*3+1] +
            (double)target[row*3+2]*target[row*3+2]);
        if (!(n>1e-20)) continue;
        for (int column=0;column<3;++column)
            worst=std::max(worst,std::fabs(candidate.m[row*3+column]-
                target[row*3+column]/n));
    }
    return worst;
}

Vec3 transform_position(Affine a, Vec3 p)
{
    return {p.x*a.m[0]+p.y*a.m[3]+p.z*a.m[6]+a.m[9],
            p.x*a.m[1]+p.y*a.m[4]+p.z*a.m[7]+a.m[10],
            p.x*a.m[2]+p.y*a.m[5]+p.z*a.m[8]+a.m[11]};
}

double distance(Vec3 a, Vec3 b)
{
    return length({a.x-b.x,a.y-b.y,a.z-b.z});
}

/* Unit-quaternion vector rotation with the same row-vector/DirectX convention
 * as rotation_affine. */
Vec3 rotate(Quat q, Vec3 v)
{
    const Vec3 u{q.x, q.y, q.z};
    const double tx = 2.0 * (u.y*v.z - u.z*v.y);
    const double ty = 2.0 * (u.z*v.x - u.x*v.z);
    const double tz = 2.0 * (u.x*v.y - u.y*v.x);
    return {v.x + q.w*tx + (u.y*tz - u.z*ty),
            v.y + q.w*ty + (u.z*tx - u.x*tz),
            v.z + q.w*tz + (u.x*ty - u.y*tx)};
}
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: anim_vbr_pose_invariant_test <game-dir>\n");
        return 2;
    }
    static const char* clip_path =
        "animations/glacier/assets/frontend/mainmenu/loadout/"
        "ui_frontend_standing_idle_assault_01";
    static const char* rig_path =
        "animations/glacier/global/rigging/soldier_3p.rig";
    static const char* skeleton_path =
        "common/characters/_soldier/ske_soldier_3p";

    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, sizeof(error));
    if (!context || !bf6_mount_all(context, 0, error, sizeof(error))) {
        std::fprintf(stderr, "mount: %s\n", error);
        if (context) bf6_close(context);
        return 1;
    }
    bf6_anim_clip* clip = bf6_anim_clip_open(context, clip_path);
    bf6_skeleton* skeleton = bf6_skeleton_read(context, skeleton_path);
    bf6_anim_binding_stats stats{};
    const int binding_count = bf6_anim_bindings(context, clip_path, rig_path,
        skeleton_path, nullptr, 0, &stats);
    std::vector<bf6_anim_binding> bindings(
        binding_count > 0 ? (size_t)binding_count : 0);
    const bool ready = clip && skeleton && binding_count == clip->channel_count &&
        bf6_anim_bindings(context, clip_path, rig_path, skeleton_path,
            bindings.data(), binding_count, &stats) == binding_count;
    if (!ready) {
        std::fprintf(stderr, "frontend VBR pose route unavailable\n");
        if (skeleton) bf6_free(context, skeleton);
        if (clip) bf6_free(context, clip);
        bf6_close(context);
        return 1;
    }

    std::vector<int> q_channel((size_t)skeleton->bone_count, -1);
    std::vector<int> t_channel((size_t)skeleton->bone_count, -1);
    for (const bf6_anim_binding& binding : bindings) {
        if (binding.bone < 0 || binding.bone >= skeleton->bone_count) continue;
        if (binding.component == BF6_ANIM_DOF_QUATERNION)
            q_channel[(size_t)binding.bone] = binding.channel;
        else if (binding.component == BF6_ANIM_DOF_VECTOR3)
            t_channel[(size_t)binding.bone] = binding.channel;
    }

    const int frames[] = {0, 1, 7, 8, 569, 1138};
    std::vector<float> sample((size_t)clip->channel_count * 4);
    std::vector<float> raw_magnitude((size_t)clip->quat_count);
    std::vector<Quat> model_rotation((size_t)skeleton->bone_count);
    std::vector<Vec3> model_position((size_t)skeleton->bone_count);
    double raw_min = std::numeric_limits<double>::infinity(), raw_max = 0.0;
    double raw_sum = 0.0, worst_unit_error = 0.0, worst_length_error = 0.0;
    int raw_count = 0, raw_far = 0, length_checks = 0;
    bool ok = clip->framing == BF6_ANIM_VBR;

    /* The old basis probe extracted a column-vector quaternion from the
     * skeleton's row-vector matrix, which made the matching Rig quaternion
     * appear conjugated. Reconstructing matrices in their actual storage pins
     * the direction without that ambiguity. */
    int default_raw_exact=0, default_conjugate_exact=0, default_q=0;
    double default_raw_worst=0.0, default_conjugate_worst=0.0;
    int driven_scaled_local=0, driven_scaled_model=0;
    double local_scale_min=std::numeric_limits<double>::infinity(), local_scale_max=0.0;
    double model_scale_min=std::numeric_limits<double>::infinity(), model_scale_max=0.0;
    for (const bf6_anim_binding& binding : bindings) {
        if (binding.component != BF6_ANIM_DOF_QUATERNION ||
            binding.bone < 0 || !binding.has_default_value) continue;
        const Quat q{binding.default_value[0], binding.default_value[1],
                     binding.default_value[2], binding.default_value[3]};
        const double raw_error=normalized_basis_error(rotation_affine(q),
            skeleton->bones[binding.bone].local);
        const double conjugate_error=normalized_basis_error(
            rotation_affine({-q.x,-q.y,-q.z,q.w}),
            skeleton->bones[binding.bone].local);
        default_raw_worst=std::max(default_raw_worst,raw_error);
        default_conjugate_worst=std::max(default_conjugate_worst,conjugate_error);
        default_raw_exact += raw_error < 1e-4;
        default_conjugate_exact += conjugate_error < 1e-4;
        bool local_scaled=false, model_scaled=false;
        for (int row=0;row<3;++row) {
            const float* local=skeleton->bones[binding.bone].local+row*3;
            const float* model=skeleton->bones[binding.bone].model+row*3;
            const double ls=std::sqrt((double)local[0]*local[0]+(double)local[1]*local[1]+(double)local[2]*local[2]);
            const double ms=std::sqrt((double)model[0]*model[0]+(double)model[1]*model[1]+(double)model[2]*model[2]);
            local_scale_min=std::min(local_scale_min,ls); local_scale_max=std::max(local_scale_max,ls);
            model_scale_min=std::min(model_scale_min,ms); model_scale_max=std::max(model_scale_max,ms);
            local_scaled = local_scaled || std::fabs(ls-1.0)>1e-4;
            model_scaled = model_scaled || std::fabs(ms-1.0)>1e-4;
        }
        driven_scaled_local += local_scaled;
        driven_scaled_model += model_scaled;
        ++default_q;
    }

    /* Storage-order oracle independent of animation: row-vector skinning is
     * inverseBind * modelPose. Reversing it is a real control and must fail on
     * rotated/translated bones. */
    int bind_identity=0;
    double bind_worst=0.0;
    for (int bone=0;bone<skeleton->bone_count;++bone) {
        const Affine inverse=copy_affine(skeleton->bones[bone].inverse);
        const Affine model_bind=copy_affine(skeleton->bones[bone].model);
        const double good=identity_error(compose(inverse,model_bind));
        bind_worst=std::max(bind_worst,good);
        bind_identity += good < 1e-4;
    }
    int skin_origin_exact=0, reverse_origin_exact=0, skin_origin_checks=0;
    double skin_origin_worst=0.0, reverse_origin_worst=0.0;

    for (int frame : frames) {
        ok = ok && bf6_anim_clip_sample(context, clip, frame, sample.data(),
                                         raw_magnitude.data());
        if (frame == 0) {
            std::printf("frame0 vector channels displaced from bind local:\n");
            for (const bf6_anim_binding& binding : bindings) {
                if (binding.component != BF6_ANIM_DOF_VECTOR3 ||
                    binding.bone < 0) continue;
                const float* value=sample.data()+(size_t)binding.channel*4;
                const float* local=skeleton->bones[binding.bone].local+9;
                const double d=length({value[0]-local[0],value[1]-local[1],value[2]-local[2]});
                if (d < .05) continue;
                const int parent=skeleton->bones[binding.bone].parent;
                std::printf("  bone=%d parent=%d parentName=%s delta=%.6f value=(%.6f %.6f %.6f) local=(%.6f %.6f %.6f) %s\n",
                    binding.bone,parent,parent>=0?skeleton->bones[parent].name:"-",d,
                    value[0],value[1],value[2],local[0],local[1],local[2],binding.dof_name);
            }
        }
        for (int q = 0; q < clip->quat_count; ++q) {
            const float* value = sample.data() + (size_t)q * 4;
            const double n = std::sqrt((double)value[0]*value[0] +
                (double)value[1]*value[1] + (double)value[2]*value[2] +
                (double)value[3]*value[3]);
            worst_unit_error = std::max(worst_unit_error, std::fabs(n - 1.0));
            const double raw = raw_magnitude[(size_t)q];
            ok = ok && std::isfinite(raw) && raw > 1e-20;
            raw_min = std::min(raw_min, raw); raw_max = std::max(raw_max, raw);
            raw_sum += raw; ++raw_count;
            if (std::fabs(raw - 1.0) > .1) ++raw_far;
        }

        for (int bone = 0; bone < skeleton->bone_count; ++bone) {
            const int qc = q_channel[(size_t)bone];
            const int parent = skeleton->bones[bone].parent;
            if (qc >= 0) {
                const float* q = sample.data() + (size_t)qc * 4;
                /* DirectX's row-vector matrix consumes the ANT lanes directly.
                 * Conjugating here would transpose the authored rotation. */
                model_rotation[(size_t)bone] = normalized({q[0], q[1], q[2], q[3]});
            } else if (parent >= 0) {
                /* An undriven bone retains its bind-local orientation relative
                 * to the parent's animated model orientation. */
                model_rotation[(size_t)bone] = multiply(
                    model_rotation[(size_t)parent],
                    matrix_quaternion(skeleton->bones[bone].local));
            } else {
                model_rotation[(size_t)bone] =
                    matrix_quaternion(skeleton->bones[bone].model);
            }
            Vec3 local{(double)skeleton->bones[bone].local[9],
                       (double)skeleton->bones[bone].local[10],
                       (double)skeleton->bones[bone].local[11]};
            const int tc = t_channel[(size_t)bone];
            if (tc >= 0) {
                const float* t = sample.data() + (size_t)tc * 4;
                local = {t[0], t[1], t[2]};
            }
            if (parent < 0) {
                model_position[(size_t)bone] = local;
            } else {
                const Vec3 rotated = rotate(model_rotation[(size_t)parent], local);
                const Vec3 parent_position = model_position[(size_t)parent];
                model_position[(size_t)bone] = {parent_position.x + rotated.x,
                    parent_position.y + rotated.y, parent_position.z + rotated.z};
                const Vec3 difference{model_position[(size_t)bone].x-parent_position.x,
                    model_position[(size_t)bone].y-parent_position.y,
                    model_position[(size_t)bone].z-parent_position.z};
                worst_length_error = std::max(worst_length_error,
                    std::fabs(length(difference) - length(local)));
                ++length_checks;
            }
        }
        for (int bone=0;bone<skeleton->bone_count;++bone) {
            Affine animated=rotation_affine(model_rotation[(size_t)bone]);
            animated.m[9]=model_position[(size_t)bone].x;
            animated.m[10]=model_position[(size_t)bone].y;
            animated.m[11]=model_position[(size_t)bone].z;
            const Affine inverse=copy_affine(skeleton->bones[bone].inverse);
            /* In row-vector storage, semantic modelPose * inverseBind is
             * represented by inverseBind * modelPose. */
            const Affine skin=compose(inverse,animated);
            const Affine reverse=compose(animated,inverse);
            const Vec3 bind_origin{(double)skeleton->bones[bone].model[9],
                (double)skeleton->bones[bone].model[10],
                (double)skeleton->bones[bone].model[11]};
            const Vec3 expected=model_position[(size_t)bone];
            const double skin_error=distance(transform_position(skin,bind_origin),expected);
            const double reverse_error=distance(transform_position(reverse,bind_origin),expected);
            skin_origin_worst=std::max(skin_origin_worst,skin_error);
            reverse_origin_worst=std::max(reverse_origin_worst,reverse_error);
            skin_origin_exact += skin_error < 1e-4;
            reverse_origin_exact += reverse_error < 1e-4;
            ++skin_origin_checks;
        }
    }

    /* The online decoder's engine-confirmed corpus observes raw VBR float4s
     * far from unit. Require this specimen to retain that discriminating
     * control while the normalized output and candidate's internally rigid
     * lengths remain precise. The latter is not a visual/semantic pose oracle. */
    ok = ok && worst_unit_error < 1e-5 && worst_length_error < 1e-5 &&
         raw_far > raw_count / 10 && stats.resolved_bones > 200 &&
         default_raw_exact > default_conjugate_exact &&
         bind_identity == skeleton->bone_count &&
         skin_origin_exact == skin_origin_checks &&
         reverse_origin_exact < skin_origin_exact;
    std::printf("defaultQ rawExact=%d/%d rawWorst=%.9g "
                "conjugateExact=%d/%d conjugateWorst=%.9g\n",
        default_raw_exact,default_q,default_raw_worst,
        default_conjugate_exact,default_q,default_conjugate_worst);
    std::printf("drivenScale localNonUnit=%d/%d range=%.9g..%.9g "
                "modelNonUnit=%d/%d range=%.9g..%.9g\n",
        driven_scaled_local,default_q,local_scale_min,local_scale_max,
        driven_scaled_model,default_q,model_scale_min,model_scale_max);
    std::printf("bindSkin inverse*model identity=%d/%d worst=%.9g\n",
        bind_identity,skeleton->bone_count,bind_worst);
    std::printf("animated bind-origin -> posed-origin inverse*model="
                "%d/%d worst=%.9g control model*inverse=%d/%d worst=%.9g\n",
        skin_origin_exact,skin_origin_checks,skin_origin_worst,
        reverse_origin_exact,skin_origin_checks,reverse_origin_worst);
    std::printf("frames=%zu bindings=%d resolvedBones=%d rawQuat="
                "min=%.6f mean=%.6f max=%.6f far=%d/%d "
                "postUnitMaxError=%.9g lengthChecks=%d lengthMaxError=%.9g\n",
        sizeof(frames)/sizeof(frames[0]), binding_count, stats.resolved_bones,
        raw_min, raw_count ? raw_sum/raw_count : 0.0, raw_max, raw_far,
        raw_count, worst_unit_error, length_checks, worst_length_error);
    std::printf("RESULT=%s\n", ok ? "PASS" : "FAIL");

    bf6_free(context, skeleton);
    bf6_free(context, clip);
    bf6_close(context);
    return ok ? 0 : 1;
}
