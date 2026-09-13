#include "bf6_core.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
struct Quat { double v[4]{}; };

Quat normalized(Quat value)
{
    double length = 0.0;
    for (double lane : value.v) length += lane * lane;
    if (length <= 1e-20) return {};
    length = std::sqrt(length);
    for (double& lane : value.v) lane /= length;
    return value;
}

Quat conjugated(Quat value)
{
    value.v[0] = -value.v[0];
    value.v[1] = -value.v[1];
    value.v[2] = -value.v[2];
    return value;
}

Quat multiplied(Quat a, Quat b)
{
    return normalized({{
        a.v[3] * b.v[0] + a.v[0] * b.v[3] + a.v[1] * b.v[2] - a.v[2] * b.v[1],
        a.v[3] * b.v[1] - a.v[0] * b.v[2] + a.v[1] * b.v[3] + a.v[2] * b.v[0],
        a.v[3] * b.v[2] + a.v[0] * b.v[1] - a.v[1] * b.v[0] + a.v[2] * b.v[3],
        a.v[3] * b.v[3] - a.v[0] * b.v[0] - a.v[1] * b.v[1] - a.v[2] * b.v[2]}});
}

double abs_dot(Quat a, Quat b)
{
    double value = 0.0;
    for (int lane = 0; lane < 4; ++lane) value += a.v[lane] * b.v[lane];
    return std::fabs(value);
}

Quat matrix_quaternion(const float* matrix)
{
    const double m00 = matrix[0], m01 = matrix[1], m02 = matrix[2];
    const double m10 = matrix[3], m11 = matrix[4], m12 = matrix[5];
    const double m20 = matrix[6], m21 = matrix[7], m22 = matrix[8];
    Quat q;
    const double trace = m00 + m11 + m22;
    if (trace > 0.0) {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        q.v[3] = 0.25 * s;
        q.v[0] = (m21 - m12) / s;
        q.v[1] = (m02 - m20) / s;
        q.v[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        q.v[3] = (m21 - m12) / s;
        q.v[0] = 0.25 * s;
        q.v[1] = (m01 + m10) / s;
        q.v[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        q.v[3] = (m02 - m20) / s;
        q.v[0] = (m01 + m10) / s;
        q.v[1] = 0.25 * s;
        q.v[2] = (m12 + m21) / s;
    } else {
        const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        q.v[3] = (m10 - m01) / s;
        q.v[0] = (m02 + m20) / s;
        q.v[1] = (m12 + m21) / s;
        q.v[2] = 0.25 * s;
    }
    return normalized(q);
}

struct Candidate {
    std::array<int, 4> permutation{};
    int signs = 0;
    bool transpose = false;
    double mean_dot = 0.0;
    int exact = 0;
};

struct VectorCandidate {
    std::array<int, 3> permutation{};
    int signs = 0;
    double mean_error = 0.0;
    double max_error = 0.0;
    int exact = 0;
};
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: anim_pose_basis_test <game-dir>\n");
        return 2;
    }
    static const char* clip_path =
        "animations/glacier/assets/frontend/mainmenu/loadout/"
        "ui_frontend_standing_idle_assault_01";
    const char* rig_path = argc > 2 ? argv[2]
        : "animations/kingston/global/rigging/soldier.rig";
    static const char* skeleton_path =
        "common/characters/_soldier/ske_soldier_3p";
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, sizeof(error));
    if (!context) { std::fprintf(stderr, "open: %s\n", error); return 1; }
    if (!bf6_mount_all(context, 0, error, sizeof(error))) {
        std::fprintf(stderr, "mount: %s\n", error);
        bf6_close(context);
        return 1;
    }
    bf6_anim_binding_stats stats{};
    const int count = bf6_anim_bindings(context, clip_path, rig_path,
                                         skeleton_path, nullptr, 0, &stats);
    std::vector<bf6_anim_binding> bindings(count > 0 ? (size_t)count : 0);
    bf6_skeleton* skeleton = bf6_skeleton_read(context, skeleton_path);
    if (count <= 0 || !skeleton || bf6_anim_bindings(
            context, clip_path, rig_path, skeleton_path, bindings.data(),
            count, &stats) != count) {
        std::fprintf(stderr, "binding/skeleton unavailable\n");
        if (skeleton) bf6_free(context, skeleton);
        bf6_close(context);
        return 1;
    }

    std::vector<std::pair<Quat, Quat>> pairs;
    struct VectorPair { double source[3]; double target[3]; };
    std::vector<VectorPair> vector_pairs;
    for (const bf6_anim_binding& binding : bindings) {
        if (binding.component != BF6_ANIM_DOF_QUATERNION ||
            binding.bone < 0 || binding.bone >= skeleton->bone_count ||
            !binding.has_default_value)
            continue;
        Quat source;
        for (int lane = 0; lane < 4; ++lane)
            source.v[lane] = binding.default_value[lane];
        pairs.push_back({normalized(source), matrix_quaternion(
            skeleton->bones[binding.bone].local)});
    }
    for (const bf6_anim_binding& binding : bindings) {
        if (binding.component != BF6_ANIM_DOF_VECTOR3 ||
            binding.bone < 0 || binding.bone >= skeleton->bone_count ||
            !binding.has_default_value)
            continue;
        VectorPair pair{};
        for (int lane = 0; lane < 3; ++lane) {
            pair.source[lane] = binding.default_value[lane];
            pair.target[lane] = skeleton->bones[binding.bone].local[9 + lane];
        }
        vector_pairs.push_back(pair);
    }

    bf6_anim_clip* clip = bf6_anim_clip_open(context, clip_path);
    std::vector<float> sample(clip ? (size_t)clip->channel_count * 4 : 0);
    std::vector<float> quaternion_magnitude(clip ? (size_t)clip->quat_count : 0);
    if (!clip || clip->channel_count != count ||
        !bf6_anim_clip_sample(context, clip, 0, sample.data(),
            quaternion_magnitude.data())) {
        std::fprintf(stderr, "frame-zero sample unavailable\n");
        if (clip) bf6_free(context, clip);
        bf6_free(context, skeleton);
        bf6_close(context);
        return 1;
    }
    if (clip->framing == BF6_ANIM_VBR && clip->constant_channels) {
        std::vector<std::array<float, 4>> animated_q, constant_q;
        for (int channel = 0; channel < clip->quat_count; ++channel) {
            std::array<float, 4> q{};
            std::copy_n(sample.data() + (size_t)channel * 4, 4, q.begin());
            (clip->constant_channels[channel] ? constant_q : animated_q).push_back(q);
        }
        auto mean_default_dot = [&](bool constants_first) {
            double sum = 0.0; int rows = 0;
            for (const bf6_anim_binding& binding : bindings) {
                if (binding.component != BF6_ANIM_DOF_QUATERNION ||
                    !binding.has_default_value || binding.channel >= clip->quat_count)
                    continue;
                const int channel = binding.channel;
                const std::array<float, 4>* q = nullptr;
                if (!constants_first) {
                    if (channel < (int)animated_q.size()) q = &animated_q[(size_t)channel];
                    else q = &constant_q[(size_t)(channel - (int)animated_q.size())];
                } else {
                    if (channel < (int)constant_q.size()) q = &constant_q[(size_t)channel];
                    else q = &animated_q[(size_t)(channel - (int)constant_q.size())];
                }
                double dot = 0.0, qn = 0.0, dn = 0.0;
                for (int lane = 0; lane < 4; ++lane) {
                    dot += (*q)[lane] * binding.default_value[lane];
                    qn += (*q)[lane] * (*q)[lane];
                    dn += binding.default_value[lane] * binding.default_value[lane];
                }
                if (qn > 1e-20 && dn > 1e-20) {
                    sum += std::fabs(dot / std::sqrt(qn * dn)); ++rows;
                }
            }
            return rows ? sum / rows : 0.0;
        };
        std::printf("frame0 q/default map-order hypotheses logical=reported "
                    "animated-first=%.9f constant-first=%.9f (anim=%zu const=%zu)\n",
            mean_default_dot(false), mean_default_dot(true),
            animated_q.size(), constant_q.size());

        double own_constant_dot = 0.0;
        double own_animated_dot = 0.0;
        double animated_unrange_dot = 0.0;
        double animated_half_range_bias_dot = 0.0;
        double animated_raw_magnitude_sum = 0.0;
        double animated_raw_magnitude_min = 1e30;
        double animated_raw_magnitude_max = 0.0;
        double best_constant_dot = 0.0;
        int own_constant_exact = 0;
        int best_constant_exact = 0;
        int own_constant_rows = 0;
        int own_animated_rows = 0;
        int best_constant_same_channel = 0;
        for (const bf6_anim_binding& binding : bindings) {
            if (binding.component != BF6_ANIM_DOF_QUATERNION ||
                !binding.has_default_value || binding.channel < 0 ||
                binding.channel >= clip->quat_count)
                continue;
            const float* value = sample.data() + (size_t)binding.channel * 4;
            Quat decoded, own_default;
            for (int lane = 0; lane < 4; ++lane) {
                decoded.v[lane] = value[lane];
                own_default.v[lane] = binding.default_value[lane];
            }
            const double own_dot = abs_dot(normalized(decoded), normalized(own_default));
            if (!clip->constant_channels[binding.channel]) {
                own_animated_dot += own_dot;
                const double magnitude = quaternion_magnitude[(size_t)binding.channel];
                Quat raw, unranged, half_range_bias;
                for (int lane = 0; lane < 4; ++lane) {
                    raw.v[lane] = decoded.v[lane] * magnitude;
                    unranged.v[lane] = raw.v[lane] - clip->quat_min;
                    half_range_bias.v[lane] = raw.v[lane] +
                        (clip->quat_max - clip->quat_min) * 0.5;
                }
                animated_unrange_dot += abs_dot(normalized(unranged), normalized(own_default));
                animated_half_range_bias_dot += abs_dot(
                    normalized(half_range_bias), normalized(own_default));
                animated_raw_magnitude_sum += magnitude;
                animated_raw_magnitude_min = std::min(
                    animated_raw_magnitude_min, magnitude);
                animated_raw_magnitude_max = std::max(
                    animated_raw_magnitude_max, magnitude);
                ++own_animated_rows;
                continue;
            }
            own_constant_dot += own_dot;
            ++own_constant_rows;
            if (own_dot > 0.999999) ++own_constant_exact;

            double best_dot = -1.0;
            int best_channel = -1;
            for (const bf6_anim_binding& candidate : bindings) {
                if (candidate.component != BF6_ANIM_DOF_QUATERNION ||
                    !candidate.has_default_value || candidate.channel < 0 ||
                    candidate.channel >= clip->quat_count ||
                    !clip->constant_channels[candidate.channel])
                    continue;
                Quat candidate_default;
                for (int lane = 0; lane < 4; ++lane)
                    candidate_default.v[lane] = candidate.default_value[lane];
                const double dot = abs_dot(normalized(decoded), normalized(candidate_default));
                if (dot > best_dot) {
                    best_dot = dot;
                    best_channel = candidate.channel;
                }
            }
            best_constant_dot += best_dot;
            if (best_dot > 0.999999) ++best_constant_exact;
            if (best_channel == binding.channel) ++best_constant_same_channel;
        }
        std::printf("frame0 q/default split constant mean=%.9f exact=%d/%d; "
                    "animated mean=%.9f rows=%d\n",
            own_constant_rows ? own_constant_dot / own_constant_rows : 0.0,
            own_constant_exact, own_constant_rows,
            own_animated_rows ? own_animated_dot / own_animated_rows : 0.0,
            own_animated_rows);
        std::printf("frame0 animated-q domain oracle unrange=%.9f half-range-bias=%.9f "
                    "raw-magnitude mean/min/max=%.9f/%.9f/%.9f range=%.9f..%.9f\n",
            own_animated_rows ? animated_unrange_dot / own_animated_rows : 0.0,
            own_animated_rows ? animated_half_range_bias_dot / own_animated_rows : 0.0,
            own_animated_rows ? animated_raw_magnitude_sum / own_animated_rows : 0.0,
            own_animated_rows ? animated_raw_magnitude_min : 0.0,
            animated_raw_magnitude_max, clip->quat_min, clip->quat_max);
        std::printf("frame0 constant-q best-default oracle mean=%.9f exact=%d/%d "
                    "same-channel=%d/%d\n",
            own_constant_rows ? best_constant_dot / own_constant_rows : 0.0,
            best_constant_exact, own_constant_rows,
            best_constant_same_channel, own_constant_rows);

        double own_constant_vec_error = 0.0;
        double own_animated_vec_error = 0.0;
        int own_constant_vec_exact = 0;
        int own_constant_vec_rows = 0;
        int own_animated_vec_rows = 0;
        for (const bf6_anim_binding& binding : bindings) {
            if (binding.component != BF6_ANIM_DOF_VECTOR3 ||
                !binding.has_default_value || binding.channel < clip->quat_count ||
                binding.channel >= clip->quat_count + clip->vec3_count)
                continue;
            const float* value = sample.data() + (size_t)binding.channel * 4;
            double squared = 0.0;
            for (int lane = 0; lane < 3; ++lane) {
                const double delta = value[lane] - binding.default_value[lane];
                squared += delta * delta;
            }
            const double error = std::sqrt(squared);
            if (clip->constant_channels[binding.channel]) {
                own_constant_vec_error += error;
                ++own_constant_vec_rows;
                if (error < 1e-6) ++own_constant_vec_exact;
            } else {
                own_animated_vec_error += error;
                ++own_animated_vec_rows;
            }
        }
        std::printf("frame0 v/default split constant mean_error=%.9f exact=%d/%d; "
                    "animated mean_error=%.9f rows=%d\n",
            own_constant_vec_rows ? own_constant_vec_error / own_constant_vec_rows : 0.0,
            own_constant_vec_exact, own_constant_vec_rows,
            own_animated_vec_rows ? own_animated_vec_error / own_animated_vec_rows : 0.0,
            own_animated_vec_rows);
    }
    std::vector<Quat> sampled_quaternion((size_t)skeleton->bone_count);
    std::vector<unsigned char> has_sampled_quaternion((size_t)skeleton->bone_count, 0);
    std::vector<std::array<double, 3>> sampled_translation((size_t)skeleton->bone_count);
    std::vector<unsigned char> has_sampled_translation((size_t)skeleton->bone_count, 0);
    for (const bf6_anim_binding& binding : bindings) {
        if (binding.bone < 0 || binding.bone >= skeleton->bone_count) continue;
        const float* value = sample.data() + (size_t)binding.channel * 4;
        if (binding.component == BF6_ANIM_DOF_QUATERNION) {
            Quat q{{value[0], value[1], value[2], value[3]}};
            sampled_quaternion[(size_t)binding.bone] = normalized(q);
            has_sampled_quaternion[(size_t)binding.bone] = 1;
        } else if (binding.component == BF6_ANIM_DOF_VECTOR3) {
            sampled_translation[(size_t)binding.bone] = {value[0], value[1], value[2]};
            has_sampled_translation[(size_t)binding.bone] = 1;
        }
    }

    double direct_local = 0.0, direct_model = 0.0;
    double relative_ab = 0.0, relative_ba = 0.0;
    int direct_rows = 0, relative_rows = 0;
    double sampled_length_error = 0.0, sampled_length_worst = 0.0;
    int sampled_length_rows = 0;
    for (int bone = 0; bone < skeleton->bone_count; ++bone) {
        if (has_sampled_quaternion[(size_t)bone]) {
            const Quat q = sampled_quaternion[(size_t)bone];
            direct_local += abs_dot(q, matrix_quaternion(skeleton->bones[bone].local));
            direct_model += abs_dot(q, matrix_quaternion(skeleton->bones[bone].model));
            ++direct_rows;
            const int parent = skeleton->bones[bone].parent;
            if (parent >= 0 && parent < skeleton->bone_count &&
                has_sampled_quaternion[(size_t)parent]) {
                const Quat p = sampled_quaternion[(size_t)parent];
                const Quat target = matrix_quaternion(skeleton->bones[bone].local);
                relative_ab += abs_dot(multiplied(conjugated(p), q), target);
                relative_ba += abs_dot(multiplied(q, conjugated(p)), target);
                ++relative_rows;
            }
        }
        const int parent = skeleton->bones[bone].parent;
        if (parent >= 0 && parent < skeleton->bone_count &&
            has_sampled_translation[(size_t)bone] &&
            has_sampled_translation[(size_t)parent]) {
            double sample_squared = 0.0;
            for (int lane = 0; lane < 3; ++lane) {
                const double delta = sampled_translation[(size_t)bone][lane] -
                    sampled_translation[(size_t)parent][lane];
                sample_squared += delta * delta;
            }
            const float* local_t = skeleton->bones[bone].local + 9;
            const double bind_length = std::sqrt((double)local_t[0] * local_t[0] +
                (double)local_t[1] * local_t[1] + (double)local_t[2] * local_t[2]);
            const double error = std::fabs(std::sqrt(sample_squared) - bind_length);
            sampled_length_error += error;
            sampled_length_worst = std::max(sampled_length_worst, error);
            ++sampled_length_rows;
        }
    }
    std::printf("sample quaternion mean dot local/model=%.9f/%.9f rows=%d\n",
        direct_rows ? direct_local / direct_rows : 0.0,
        direct_rows ? direct_model / direct_rows : 0.0, direct_rows);
    std::printf("sample model-relative mean dot inv(parent)*child/child*inv(parent)=%.9f/%.9f rows=%d\n",
        relative_rows ? relative_ab / relative_rows : 0.0,
        relative_rows ? relative_ba / relative_rows : 0.0, relative_rows);
    std::printf("sample translation parent-distance mean/worst error=%.9f/%.9f rows=%d\n",
        sampled_length_rows ? sampled_length_error / sampled_length_rows : 0.0,
        sampled_length_worst, sampled_length_rows);
    double quaternion_dot_sum = 0.0;
    double quaternion_dot_min = 1.0;
    double quaternion_model_dot_sum = 0.0;
    double quaternion_model_dot_min = 1.0;
    int quaternion_samples = 0;
    double vector_local_error_sum = 0.0;
    double vector_model_error_sum = 0.0;
    int vector_sample_count = 0;
    struct VectorDelta { double magnitude; std::string name; double value[3]; double rest[3]; };
    std::vector<VectorDelta> vector_deltas;
    for (const bf6_anim_binding& binding : bindings) {
        if (binding.bone < 0 || !binding.has_default_value) continue;
        const float* value = sample.data() + (size_t)binding.channel * 4;
        if (binding.component == BF6_ANIM_DOF_QUATERNION) {
            double dot = 0.0, sample_length = 0.0, rest_length = 0.0;
            for (int lane = 0; lane < 4; ++lane) {
                dot += value[lane] * binding.default_value[lane];
                sample_length += value[lane] * value[lane];
                rest_length += binding.default_value[lane] * binding.default_value[lane];
            }
            if (sample_length > 1e-20 && rest_length > 1e-20) {
                dot = std::fabs(dot / std::sqrt(sample_length * rest_length));
                quaternion_dot_sum += dot;
                quaternion_dot_min = std::min(quaternion_dot_min, dot);
                const Quat model_quaternion = matrix_quaternion(
                    skeleton->bones[binding.bone].model);
                double model_dot = 0.0;
                for (int lane = 0; lane < 4; ++lane)
                    model_dot += value[lane] * model_quaternion.v[lane];
                model_dot = std::fabs(model_dot / std::sqrt(sample_length));
                quaternion_model_dot_sum += model_dot;
                quaternion_model_dot_min = std::min(
                    quaternion_model_dot_min, model_dot);
                ++quaternion_samples;
            }
        } else if (binding.component == BF6_ANIM_DOF_VECTOR3) {
            VectorDelta delta{};
            delta.name = binding.dof_name;
            for (int lane = 0; lane < 3; ++lane) {
                delta.value[lane] = value[lane];
                delta.rest[lane] = binding.default_value[lane];
                const double difference = delta.value[lane] - delta.rest[lane];
                delta.magnitude += difference * difference;
            }
            delta.magnitude = std::sqrt(delta.magnitude);
            vector_deltas.push_back(delta);
            double local_squared = 0.0, model_squared = 0.0;
            for (int lane = 0; lane < 3; ++lane) {
                const double local_delta = value[lane] -
                    skeleton->bones[binding.bone].local[9 + lane];
                const double model_delta = value[lane] -
                    skeleton->bones[binding.bone].model[9 + lane];
                local_squared += local_delta * local_delta;
                model_squared += model_delta * model_delta;
            }
            vector_local_error_sum += std::sqrt(local_squared);
            vector_model_error_sum += std::sqrt(model_squared);
            ++vector_sample_count;
        }
    }
    std::sort(vector_deltas.begin(), vector_deltas.end(),
        [](const VectorDelta& a, const VectorDelta& b) {
            return a.magnitude > b.magnitude;
        });
    std::printf("frame0 q/default mean_abs_dot=%.9f min=%.9f rows=%d\n",
        quaternion_samples ? quaternion_dot_sum / quaternion_samples : 0.0,
        quaternion_dot_min, quaternion_samples);
    std::vector<Candidate> sample_lane_candidates;
    std::array<int, 4> sample_permutation{0, 1, 2, 3};
    do {
        for (int signs = 0; signs < 8; ++signs) {
            Candidate candidate{sample_permutation, signs, false};
            int rows = 0;
            for (const bf6_anim_binding& binding : bindings) {
                if (binding.component != BF6_ANIM_DOF_QUATERNION ||
                    !binding.has_default_value) continue;
                const float* value = sample.data() + (size_t)binding.channel * 4;
                Quat converted, target;
                for (int lane = 0; lane < 4; ++lane) {
                    converted.v[lane] = value[sample_permutation[lane]] *
                        ((signs & (1 << lane)) ? -1.0 : 1.0);
                    target.v[lane] = binding.default_value[lane];
                }
                candidate.mean_dot += abs_dot(normalized(converted), normalized(target));
                ++rows;
            }
            if (rows) candidate.mean_dot /= rows;
            sample_lane_candidates.push_back(candidate);
        }
    } while (std::next_permutation(sample_permutation.begin(), sample_permutation.end()));
    std::sort(sample_lane_candidates.begin(), sample_lane_candidates.end(),
        [](const Candidate& a, const Candidate& b) { return a.mean_dot > b.mean_dot; });
    for (size_t i = 0; i < std::min<size_t>(8, sample_lane_candidates.size()); ++i) {
        const Candidate& c = sample_lane_candidates[i];
        std::printf("  sample-lane rank=%zu map=%d%d%d%d signs=%X mean_abs_dot=%.9f\n",
            i + 1, c.permutation[0], c.permutation[1], c.permutation[2],
            c.permutation[3], c.signs, c.mean_dot);
    }
    std::printf("frame0 q/bind-model mean_abs_dot=%.9f min=%.9f rows=%d\n",
        quaternion_samples ? quaternion_model_dot_sum / quaternion_samples : 0.0,
        quaternion_model_dot_min, quaternion_samples);
    std::printf("frame0 vector mean error local/model=%.9f/%.9f rows=%d\n",
        vector_sample_count ? vector_local_error_sum / vector_sample_count : 0.0,
        vector_sample_count ? vector_model_error_sum / vector_sample_count : 0.0,
        vector_sample_count);
    std::printf("frame0 largest render-vector deltas (%zu rows):\n",
                vector_deltas.size());
    for (size_t index = 0; index < std::min<size_t>(16, vector_deltas.size());
         ++index) {
        const VectorDelta& delta = vector_deltas[index];
        std::printf("  %.6f value=(%.6f %.6f %.6f) default=(%.6f %.6f %.6f) %s\n",
            delta.magnitude, delta.value[0], delta.value[1], delta.value[2],
            delta.rest[0], delta.rest[1], delta.rest[2], delta.name.c_str());
    }
    static const char* inspect_names[] = {
        "Reference.q", "Reference.t", "AITrajectory.q", "AITrajectory.t",
        "Trajectory.q", "Trajectory.t", "Connect.q", "Connect.t",
        "Hips.q", "Hips.t", "Spine.q", "Spine.t", "Spine1.q", "Spine1.t",
        "LeftShoulder.q", "LeftShoulder.t", "LeftArm.q", "LeftArm.t",
        "RightShoulder.q", "RightShoulder.t", "RightArm.q", "RightArm.t"
    };
    std::printf("frame0 selected pose channels:\n");
    for (const char* wanted : inspect_names) {
        for (const bf6_anim_binding& binding : bindings) {
            if (std::strcmp(binding.dof_name, wanted) != 0) continue;
            const float* value = sample.data() + (size_t)binding.channel * 4;
            std::printf("  ch=%-3d storage=%-3d bone=%-3d %-20s "
                        "value=(% .6f % .6f % .6f % .6f) "
                        "default=(% .6f % .6f % .6f % .6f)\n",
                binding.channel, binding.storage_index, binding.bone,
                binding.dof_name, value[0], value[1], value[2], value[3],
                binding.default_value[0], binding.default_value[1],
                binding.default_value[2], binding.default_value[3]);
        }
    }

    std::vector<Candidate> candidates;
    std::array<int, 4> permutation{0, 1, 2, 3};
    do {
        // Quaternion global sign is immaterial under abs(dot), and conjugating
        // the target is identical to conjugating the converted source.  The
        // old 16-sign x transpose enumeration therefore emitted every score
        // four times and made the separation gate impossible to pass.  Keep
        // w positive and express conjugation through the xyz sign mask: these
        // are the eight distinct candidates for each lane permutation.
        for (int signs = 0; signs < 8; ++signs) {
                Candidate candidate{permutation, signs, false};
                for (const auto& pair : pairs) {
                    Quat converted;
                    for (int lane = 0; lane < 4; ++lane)
                        converted.v[lane] = pair.first.v[permutation[lane]] *
                            ((signs & (1 << lane)) ? -1.0 : 1.0);
                    converted = normalized(converted);
                    double dot = 0.0;
                    for (int lane = 0; lane < 4; ++lane)
                        dot += converted.v[lane] * pair.second.v[lane];
                    dot = std::fabs(dot);
                    candidate.mean_dot += dot;
                    candidate.exact += dot > 0.9999;
                }
                if (!pairs.empty()) candidate.mean_dot /= pairs.size();
                candidates.push_back(candidate);
        }
    } while (std::next_permutation(permutation.begin(), permutation.end()));
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) {
            return a.mean_dot > b.mean_dot;
        });
    std::printf("quaternion default/render pairs=%zu bindings=%d bones=%d\n",
                pairs.size(), count, stats.resolved_bones);
    for (size_t index = 0; index < std::min<size_t>(12, candidates.size());
         ++index) {
        const Candidate& candidate = candidates[index];
        std::printf("  rank=%zu map=%d%d%d%d signs=%X transpose=%d "
                    "mean_abs_dot=%.9f exact=%d/%zu\n",
            index + 1, candidate.permutation[0], candidate.permutation[1],
            candidate.permutation[2], candidate.permutation[3],
            candidate.signs, candidate.transpose ? 1 : 0,
            candidate.mean_dot, candidate.exact, pairs.size());
    }
    const bool separated = candidates.size() > 1 && !pairs.empty() &&
        candidates[0].mean_dot > candidates[1].mean_dot + 1e-6;
    std::printf("control_separated=%d delta=%.9f\n", separated ? 1 : 0,
        candidates.size() > 1
            ? candidates[0].mean_dot - candidates[1].mean_dot : 0.0);

    std::vector<VectorCandidate> vector_candidates;
    std::array<int, 3> vector_permutation{0, 1, 2};
    do {
        for (int signs = 0; signs < 8; ++signs) {
            VectorCandidate candidate{vector_permutation, signs};
            for (const VectorPair& pair : vector_pairs) {
                double squared = 0.0;
                for (int lane = 0; lane < 3; ++lane) {
                    const double converted = pair.source[vector_permutation[lane]] *
                        ((signs & (1 << lane)) ? -1.0 : 1.0);
                    const double delta = converted - pair.target[lane];
                    squared += delta * delta;
                }
                const double error = std::sqrt(squared);
                candidate.mean_error += error;
                candidate.max_error = std::max(candidate.max_error, error);
                candidate.exact += error < 1e-5;
            }
            if (!vector_pairs.empty()) candidate.mean_error /= vector_pairs.size();
            vector_candidates.push_back(candidate);
        }
    } while (std::next_permutation(vector_permutation.begin(), vector_permutation.end()));
    std::sort(vector_candidates.begin(), vector_candidates.end(),
        [](const VectorCandidate& a, const VectorCandidate& b) {
            return a.mean_error < b.mean_error;
        });
    std::printf("vector default/render pairs=%zu\n", vector_pairs.size());
    for (size_t index = 0; index < std::min<size_t>(12, vector_candidates.size());
         ++index) {
        const VectorCandidate& candidate = vector_candidates[index];
        std::printf("  rank=%zu map=%d%d%d signs=%X mean_error=%.9f max=%.9f exact=%d/%zu\n",
            index + 1, candidate.permutation[0], candidate.permutation[1],
            candidate.permutation[2], candidate.signs, candidate.mean_error,
            candidate.max_error, candidate.exact, vector_pairs.size());
    }
    bf6_free(context, clip);
    bf6_free(context, skeleton);
    bf6_close(context);
    return pairs.size() >= 100 ? 0 : 1;
}
