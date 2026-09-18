/* WHAT A RAW-FRAMED ANIMATION PAYLOAD IS MADE OF.
 *
 *   anim_raw_probe <game> [res-name ...]
 *
 * WHY. bf6_anim_clip_open reads DCT (header 32) and VBR (header 48) and refuses
 * everything else, so 1,020 of the 2,801 animation resources under
 * animations/glacier/assets/1p/ do not open at all. That population is not a
 * long tail: it is the whole `p_` (pose), `tadtv_` and `ladtv_` (additive
 * transition) families, which between them carry every per-weapon idle, and the
 * reload, ADS and traversal takes we need. The framing is documented in
 * bf6_core.h as BF6_ANIM_RAW with five regions and nothing reads it.
 *
 * This prints the container so the codec can be written against what is there
 * rather than against a guess: the header size, every region's count, offset
 * and flags, the gaps between them, and the first bytes of each region read
 * three ways (u32, float, u16) because which one is right is exactly the
 * question.
 *
 * A DCT clip is printed alongside as a CONTROL. The two framings share a
 * container, so a field that looks meaningful in RAW but reads identically in
 * DCT is a property of the container and not of the codec.
 */
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char* const kDefaults[] = {
    /* RAW: the rest pose the 1P view currently fakes with a turn-in-place clip */
    "animations/glacier/assets/1p/common/rifle/loco/p_1p_rifle_stand_idle_01",
    /* RAW: a per-weapon idle, the shape every weapon has one of */
    "animations/glacier/assets/1p/common/rifle/loco/p_1p_rifle_stand_idle_phaseaim_yaw_01",
    /* RAW: an additive transition, the family that carries sway and lean */
    "animations/glacier/assets/1p/common/rifle/loco/tadtv_1p_rifle_pronetostand_02",
    /* CONTROL, DCT: this one already opens, so anything that reads the same in
     * both is the container speaking rather than the codec. */
    "animations/glacier/assets/1p/common/rifle/loco/t_1p_rifle_stand_idle_turn_inplace_left_01",
};

const char* framing_name(bf6_anim_framing f)
{
    switch (f) {
        case BF6_ANIM_DCT: return "DCT";
        case BF6_ANIM_VBR: return "VBR";
        case BF6_ANIM_RAW: return "RAW";
        default:           return "UNKNOWN";
    }
}

void dump_head(const uint8_t* d, int64_t n, uint32_t off, const char* label)
{
    if ((int64_t)off >= n) { std::printf("      %-12s past the end\n", label); return; }
    const int64_t left = n - (int64_t)off;
    const int64_t show = left < 8 ? left : 8;

    std::printf("      %-12s u32:", label);
    for (int64_t i = 0; i + 4 <= show; i += 4) {
        uint32_t v; std::memcpy(&v, d + off + i, 4);
        std::printf(" %u", v);
    }
    std::printf("   f32:");
    for (int64_t i = 0; i + 4 <= show; i += 4) {
        float v; std::memcpy(&v, d + off + i, 4);
        std::printf(" %.4f", (double)v);
    }
    std::printf("   u16:");
    for (int64_t i = 0; i + 2 <= show; i += 2) {
        uint16_t v; std::memcpy(&v, d + off + i, 2);
        std::printf(" %u", v);
    }
    std::printf("\n");
}

void probe(bf6_ctx* c, const std::string& name)
{
    std::printf("\n=== %s\n", name.c_str());
    bf6_anim_reloc* r = bf6_anim_reloc_read(c, name.c_str());
    if (!r) {
        /* NOT A RELOC PAYLOAD, which is not the same as unreadable: the flat
         * pose blob lives here, so ask the clip codec anyway rather than
         * reporting an absence the reader never tested for. */
        std::printf("    bf6_anim_reloc_read refused it - not the reloc container\n");
        bf6_anim_clip* flat = bf6_anim_clip_open(c, name.c_str());
        std::printf("    bf6_anim_clip_open: %s\n", flat ? "OPENS (flat pose blob)" : "REFUSED");
        if (flat) {
            std::printf("      channels %d  quat %d  vec3 %d  scalars %d  frames %d\n",
                        flat->channel_count, flat->quat_count, flat->vec3_count,
                        flat->scalar_count, flat->key_time_count);
            std::vector<float> out((size_t)flat->channel_count * 4u + 4u, 0.0f);
            float mag = 0.0f;
            if (bf6_anim_clip_sample(c, flat, 0, out.data(), &mag)) {
                int unit_q = 0, unit_rest = 0;
                for (int i = 0; i < flat->channel_count; i++) {
                    const float* v = &out[(size_t)i * 4u];
                    const double m = std::sqrt((double)v[0]*v[0] + (double)v[1]*v[1]
                                             + (double)v[2]*v[2] + (double)v[3]*v[3]);
                    const bool u = m > 0.98 && m < 1.02;
                    if (i < flat->quat_count) { if (u) ++unit_q; } else if (u) ++unit_rest;
                }
                std::printf("      sampled: %d/%d quats unit, %d non-quats unit (want 0)\n",
                            unit_q, flat->quat_count, unit_rest);
            }
            bf6_free(c, flat);
        }
        return;
    }
    std::printf("    framing %-8s header %d   payload %lld bytes   %d region(s)\n",
                framing_name(r->framing), r->header_size,
                (long long)r->size, r->region_count);

    /* THE HEADER ITSELF, byte for byte. 5 regions x 12 = 60 of the 64, so four
     * bytes are unaccounted for, and the channel-kind split (how many
     * quaternions, how many vector3s, how many scalar groups) has to be stated
     * somewhere - the DCT framing puts it in region 1's first 8 bytes. Printed
     * as u32 and as u16 pairs because the counts are small. */
    {
        const int hn = r->header_size < (int)r->size ? r->header_size : (int)r->size;
        std::printf("      header u32:");
        for (int o = 0; o + 4 <= hn; o += 4) {
            uint32_t v; std::memcpy(&v, r->data + o, 4);
            std::printf(" %u", v);
            if ((o + 4) % 12 == 0) std::printf(" |");
        }
        std::printf("\n      header u16:");
        for (int o = 0; o + 2 <= hn; o += 2) {
            uint16_t v; std::memcpy(&v, r->data + o, 2);
            std::printf(" %u", v);
        }
        std::printf("\n");
    }

    /* Does the clip codec take it today? The whole point of the exercise. */
    bf6_anim_clip* clip = bf6_anim_clip_open(c, name.c_str());
    std::printf("    bf6_anim_clip_open: %s\n", clip ? "OPENS" : "REFUSED");
    if (clip) {
        std::printf("      channels %d  quat %d  vec3 %d  frames %d\n",
                    clip->channel_count, clip->quat_count, clip->vec3_count,
                    clip->key_time_count);
        /* THE VALUES, not just the open. A codec that hands back the right
         * COUNT of wrong numbers passes every structural check there is, so
         * this samples the pose and asks whether the quaternions are unit and
         * the vectors are not - the same split the reader derived, verified
         * here from the other side. */
        std::vector<float> out((size_t)clip->channel_count * 4u + 4u, 0.0f);
        float mag = 0.0f;
        if (bf6_anim_clip_sample(c, clip, 0, out.data(), &mag)) {
            int unit_in_quats = 0, unit_in_vecs = 0;
            for (int i = 0; i < clip->channel_count; i++) {
                const float* v = &out[(size_t)i * 4u];
                const double m = std::sqrt((double)v[0]*v[0] + (double)v[1]*v[1]
                                         + (double)v[2]*v[2] + (double)v[3]*v[3]);
                const bool unit = m > 0.98 && m < 1.02;
                if (i < clip->quat_count) { if (unit) ++unit_in_quats; }
                else if (unit) ++unit_in_vecs;
            }
            std::printf("      sampled: first magnitude %.6f, %d/%d quats unit, "
                        "%d non-quats unit (want 0)\n",
                        (double)mag, unit_in_quats, clip->quat_count, unit_in_vecs);
            const float* v0 = out.data();
            std::printf("      channel 0 %.4f %.4f %.4f %.4f   channel %d %.4f %.4f %.4f %.4f\n",
                        (double)v0[0], (double)v0[1], (double)v0[2], (double)v0[3],
                        clip->quat_count,
                        (double)out[(size_t)clip->quat_count * 4u + 0],
                        (double)out[(size_t)clip->quat_count * 4u + 1],
                        (double)out[(size_t)clip->quat_count * 4u + 2],
                        (double)out[(size_t)clip->quat_count * 4u + 3]);
        } else {
            std::printf("      sample FAILED\n");
        }
        bf6_free(c, clip);   /* handles are released through bf6_free */
    }

    for (int i = 0; i < r->region_count; i++) {
        const bf6_anim_region& g = r->regions[i];
        /* The span to the next region's offset, which is what says how wide an
         * element is when a count is known: bytes / count. */
        uint32_t next = (uint32_t)r->size;
        for (int j = 0; j < r->region_count; j++)
            if (r->regions[j].offset > g.offset && r->regions[j].offset < next)
                next = r->regions[j].offset;
        const uint32_t span = next > g.offset ? next - g.offset : 0u;
        char per[32] = "-";
        if (g.count) std::snprintf(per, sizeof(per), "%.2f", (double)span / (double)g.count);
        std::printf("    region %d  count %-8u offset %-8u flags 0x%08x  span %-8u bytes/count %s\n",
                    i, g.count, g.offset, g.flags, span, per);
        char lbl[16];
        std::snprintf(lbl, sizeof(lbl), "r%d", i);
        dump_head(r->data, r->size, g.offset, lbl);

        /* IS THE DATA CHANNEL-MAJOR OR FRAME-MAJOR?
         *
         * r1 is a permutation whose first A entries are the animated channels,
         * so A = channels - constants is known. What is not known is how the
         * float4 records are ordered, and the two possibilities predict
         * opposite things about neighbours:
         *
         *   channel-major  record i and i+1 are consecutive FRAMES of one
         *                  channel, so they are nearly identical - animation
         *                  is smooth - while i and i+A are unrelated bones.
         *   frame-major    record i and i+1 are different BONES in one frame,
         *                  so they differ, while i and i+A are the same bone
         *                  one frame later and are nearly identical.
         *
         * Measured as mean absolute difference over the first few hundred
         * records. The smaller mean says which stride follows a single bone. */
        if (i == 3 && g.count > 8 && (uint32_t)r->region_count > 4) {
            /* THE ANIMATED RUN, COUNTED FROM THE PERMUTATION ITSELF.
             *
             * r1 is slot -> channel with the animated channels first, so the
             * run ends at the first descent. Counting it beats deriving it
             * from r1.count - r4.count, which disagrees with the measured
             * frame stride on some clips and not others - and a count that is
             * right three times out of four is the most dangerous kind. */
            uint32_t run = 0;
            const bf6_anim_region& idx = r->regions[1];
            if (idx.count >= 2 &&
                (size_t)idx.offset + (size_t)idx.count * 2 <= (size_t)r->size) {
                uint16_t prev = 0; std::memcpy(&prev, r->data + idx.offset, 2);
                run = 1;
                for (uint32_t k = 1; k < idx.count; k++) {
                    uint16_t v; std::memcpy(&v, r->data + idx.offset + (size_t)k * 2, 2);
                    if (v < prev) break;
                    prev = v; ++run;
                }
            }
            std::printf("      permutation: animated run %u, r1.count %u, const %u, "
                        "r1-const %u, r1-1-const %u\n",
                        run, r->regions[1].count, r->regions[4].count,
                        r->regions[1].count - r->regions[4].count,
                        r->regions[1].count - 1 - r->regions[4].count);
            const uint32_t A = r->regions[1].count - r->regions[4].count;
            if (A >= 2 && g.count > A * 2) {
                auto rec = [&](uint32_t k, int lane) {
                    float v; std::memcpy(&v, r->data + g.offset + ((size_t)k * 4 + lane) * 4, 4);
                    return (double)v;
                };
                auto meandiff = [&](uint32_t stride) {
                    double acc = 0.0; uint32_t n = 0;
                    for (uint32_t k = 0; k + stride < g.count && n < 400; k++, n++)
                        for (int l = 0; l < 4; l++)
                            acc += std::fabs(rec(k, l) - rec(k + stride, l));
                    return n ? acc / (double)(n * 4) : 0.0;
                };
                std::printf("      ordering: A=%u  mean|d| stride1 %.6f  strideA %.6f  -> %s\n",
                            A, meandiff(1), meandiff(A),
                            meandiff(1) < meandiff(A) ? "CHANNEL-major" : "FRAME-major");

                /* DO NOT ASSUME A. The stride that follows one bone through
                 * consecutive frames is the one where neighbouring records are
                 * nearly identical, and that can be MEASURED over every
                 * candidate rather than derived from a channel count that may
                 * itself be wrong. Scanning finds the true frame stride even
                 * when channels - constants is off, and a scan whose winner is
                 * A confirms A independently. */
                double best = 1e30; uint32_t best_s = 0;
                const uint32_t cap = g.count / 4 < 400u ? g.count / 4 : 400u;
                for (uint32_t s = 2; s <= cap; s++) {
                    const double d = meandiff(s);
                    if (d < best) { best = d; best_s = s; }
                }
                if (best_s) {
                    const double ratio = meandiff(1) > 0 ? best / meandiff(1) : 0.0;
                    std::printf("      scan: best stride %u (mean|d| %.6f, %.1f%% of stride1)"
                                "  data/stride = %.3f%s\n",
                                best_s, best, 100.0 * ratio,
                                (double)g.count / (double)best_s,
                                g.count % best_s == 0 ? "  EXACT" : "");
                }
            }
        }

        /* THE INDEX REGIONS IN FULL. Regions 1 and 2 are u16 arrays the same
         * length as the channel count, and which is which decides everything:
         * the clip struct needs the quaternion / vector3 / scalar-group split
         * and nothing in the 64-byte header states it. Eight values told us
         * they ascend; all of them will say whether one is a kind table, a
         * bone map, or a run of offsets. */
        if ((i == 1 || i == 2) && g.count && g.count <= 512 &&
            (size_t)g.offset + (size_t)g.count * 2 <= (size_t)r->size) {
            std::printf("      r%d full :", i);
            for (uint32_t k = 0; k < g.count; k++) {
                uint16_t v; std::memcpy(&v, r->data + g.offset + (size_t)k * 2, 2);
                std::printf(" %u", v);
            }
            std::printf("\n");
        }
    }
    bf6_free(c, r);
}

} /* namespace */

/* THE RELATIONSHIP BETWEEN THE REGIONS, over many clips at once.
 *
 * One clip is not enough to fix the layout: the first RAW payload read factors
 * as 7,581 = 21 x 361 with 129 - 108 = 21 channels left over after the constant
 * region, which is a clean "animated channels x frames" story - and the second
 * does not fit it at all. Either the model is wrong or one of the counts means
 * something else, and the way to tell is a population rather than an argument.
 *
 * Prints, per clip: the channel count, the data and const counts, and whether
 * data divides by (channels - const). A column of exact divisions is the model
 * holding; a scatter of remainders is it failing.
 */
void census(bf6_ctx* c, const std::vector<std::string>& names)
{
    std::printf("\n%-62s %5s %5s %6s %8s %7s\n",
                "clip", "chan", "const", "anim", "data", "data/anim");
    int fits = 0, total = 0;
    for (const std::string& n : names) {
        bf6_anim_reloc* r = bf6_anim_reloc_read(c, n.c_str());
        if (!r) continue;
        if (r->framing != BF6_ANIM_RAW || r->region_count < 5) { bf6_free(c, r); continue; }
        const uint32_t chan  = r->regions[1].count;
        const uint32_t konst = r->regions[4].count;
        const uint32_t data  = r->regions[3].count;
        const long     anim  = (long)chan - (long)konst;
        ++total;
        char ratio[32] = "-";
        if (anim > 0) {
            if (data % (uint32_t)anim == 0) { std::snprintf(ratio, sizeof(ratio), "%u EXACT", data / (uint32_t)anim); ++fits; }
            else std::snprintf(ratio, sizeof(ratio), "%.2f", (double)data / (double)anim);
        }
        std::string leaf = n.substr(n.find_last_of('/') + 1);
        /* THE MEASURED STRIDE against the permutation's run, over the whole
         * population. One clip agreeing is a coincidence; a column of
         * run-minus-one with a handful of run is a rule with an exception
         * worth naming. */
        char strideCol[48] = "-";
        if (data > 8 && anim >= 2) {
            auto rec = [&](uint32_t k, int lane) {
                float v; std::memcpy(&v, r->data + r->regions[3].offset
                                     + ((size_t)k * 4 + lane) * 4, 4);
                return (double)v;
            };
            /* CHEAP ON PURPOSE. Scanning every stride over 200 records for
             * every clip in the tree does not finish; 40 records is plenty to
             * separate "same bone, next frame" from "different bone", and the
             * search only has to cover strides near the run. */
            auto meandiff = [&](uint32_t stride) {
                double acc = 0.0; uint32_t m = 0;
                for (uint32_t k = 0; k + stride < data && m < 40; k++, m++)
                    for (int l = 0; l < 4; l++) acc += std::fabs(rec(k, l) - rec(k + stride, l));
                return m ? acc / (double)(m * 4) : 1e30;
            };
            double best = 1e30; uint32_t best_s = 0;
            const uint32_t lo = anim > 6 ? (uint32_t)anim - 5u : 2u;
            const uint32_t hi_raw = (uint32_t)anim + 5u;
            const uint32_t cap = hi_raw < data / 2 ? hi_raw : data / 2;
            for (uint32_t s = lo; s <= cap; s++) {
                const double d = meandiff(s);
                if (d < best) { best = d; best_s = s; }
            }
            const long delta = (long)anim - (long)best_s;
            std::snprintf(strideCol, sizeof(strideCol), "%u %s run%+ld",
                          best_s, data % best_s == 0 ? "exact" : "RAGGED", -delta);
        }
        std::printf("%-52s %5u %5u %6ld %8u %8s  %s\n",
                    leaf.c_str(), chan, konst, anim, data, ratio, strideCol);
        bf6_free(c, r);
    }
    std::printf("\n%d of %d RAW clip(s) divide exactly\n", fits, total);

    /* COVERAGE, which is the number this work exists to move. Before the RAW
     * pose codec every RAW payload was refused outright. */
    int opens = 0, refused = 0, raw_pose = 0, raw_anim = 0;
    for (const std::string& n : names) {
        bf6_anim_reloc* r = bf6_anim_reloc_read(c, n.c_str());
        if (!r) continue;
        const bool is_raw = r->framing == BF6_ANIM_RAW;
        const bool empty_data = is_raw && r->region_count >= 5 && r->regions[3].count == 0;
        bf6_free(c, r);
        bf6_anim_clip* cl = bf6_anim_clip_open(c, n.c_str());
        if (cl) { ++opens; if (is_raw) ++raw_pose; bf6_free(c, cl); }
        else { ++refused; if (is_raw && !empty_data) ++raw_anim; }
    }
    std::printf("bf6_anim_clip_open: %d open, %d refused  "
                "(%d RAW poses now open, %d animated RAW still refused)\n",
                opens, refused, raw_pose, raw_anim);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: anim_raw_probe <game> [res-name ...]\n");
        return 2;
    }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }

    std::vector<std::string> names;
    for (int i = 2; i < argc; i++) names.push_back(argv[i]);
    if (names.empty())
        for (const char* d : kDefaults) names.push_back(d);

    for (const std::string& n : names) probe(c, n);

    /* A POPULATION, not the four hand-picked ones above. Every animation
     * resource under the 1P tree, so the model is tested against what ships
     * rather than against the examples that suggested it. */
    std::vector<std::string> all;
    {
        std::vector<bf6_cat_entry> ents((size_t)200000);
        const int n = bf6_catalogue(c, "animations/glacier/assets/1p/",
                                    ents.data(), (int)ents.size());
        for (int i = 0; i < n && i < (int)ents.size(); i++)
            if (ents[(size_t)i].res_name) all.push_back(ents[(size_t)i].res_name);
        std::printf("\n%d resource(s) under animations/glacier/assets/1p/\n", n);
    }
    if (!all.empty()) census(c, all);

    bf6_close(c);
    return 0;
}
