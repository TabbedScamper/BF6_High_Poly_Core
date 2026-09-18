/* THE FIRST-PERSON CLIP, exported on the first-person rig.
 *
 *   soldier_1p_clip_test <game>
 *
 * bf6_loadout_soldier_clip could only ever export the THIRD-person soldier: it
 * called resolve_idle_clip and composed kSkeleton, both 3P, so the arms the
 * player actually looks at stayed a still photograph no matter what the caller
 * asked for. With "view":"1p" it now takes the first-person rig, skeleton and
 * hold.
 *
 * WHAT THIS CHECKS, and why each one is here rather than a bare "it returned
 * something":
 *
 *   - the 1P export must differ from the 3P export. If a wiring mistake left
 *     it on the 3P path, every other check here would still pass - same shape,
 *     same field names, plausible numbers - and the arms would be a soldier
 *     seen from outside. The bone counts are the tell: ske_soldier_1p has 203
 *     bones against ske_soldier_3p's 291.
 *   - every frame's rotation basis must be ORTHONORMAL. The body holds 3x4
 *     matrices, not quaternions, so "is it unit" becomes "are the three rows
 *     unit and mutually perpendicular" - which is the thing a wrong channel
 *     stride or a wrong quat/vec3 split actually breaks. Reading the right
 *     count of floats from the wrong place still yields a full matrix.
 *   - the frames must not all be the same frame. This is the check the rest
 *     cannot make: a still POSE held for 41 frames has the right frame count,
 *     the right bone count, perfectly orthonormal rotations and differs from
 *     the 3P export. It passes everything else while the arms sit frozen,
 *     which is the exact failure this whole task exists to end.
 *   - track bone indices must be distinct and non-negative. Two tracks on one
 *     bone means the last writer silently wins.
 */
#include "bf6_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int fails = 0;
void bad(const char* what) { std::printf("  FAIL %s\n", what); ++fails; }

/* The record is a JSON head followed by a float body; this only needs the
 * head's scalars, so a full parser would be more machinery than the question
 * deserves. */
long find_int(const std::string& head, const char* key, long missing = -1)
{
    const std::string pat = std::string("\"") + key + "\":";
    const size_t at = head.find(pat);
    if (at == std::string::npos) return missing;
    return std::strtol(head.c_str() + at + pat.size(), nullptr, 10);
}

std::string head_of(const uint8_t* blob, int64_t n)
{
    if (!blob || n < 12) return std::string();
    uint32_t json_len = 0; std::memcpy(&json_len, blob + 8, 4);
    if ((int64_t)json_len + 12 > n) return std::string();
    return std::string((const char*)blob + 12, json_len);
}

struct Export {
    long frames = -1, bones = -1; int64_t bytes = 0; bool ok = false;
    std::vector<int> bone_index;      /* per track, the skeleton bone it drives */
    std::vector<float> body;          /* bone-major, frames * 12 per track */
};

Export run(bf6_ctx* c, const char* request)
{
    Export e{};
    uint8_t* blob = nullptr;
    const int64_t n = bf6_loadout_soldier_clip(c, request, &blob);
    if (n <= 0 || !blob) { if (blob) bf6_free(c, blob); return e; }
    const std::string head = head_of(blob, n);
    e.frames = find_int(head, "frames");
    e.bones  = find_int(head, "bones");
    e.bytes  = n;
    e.ok     = head.find("\"error\":\"\"") != std::string::npos ||
               head.find("\"error\"") == std::string::npos;
    if (!e.ok) {
        const size_t at = head.find("\"error\"");
        std::printf("  record error: %.120s\n", head.c_str() + at);
    }
    /* Every `"bone":N` in the tracks array, in order. The track offsets are
     * bone-major and regular - track t starts at t * frames * 12 - so only the
     * bone indices need lifting out. */
    for (size_t at = head.find("\"bone\":"); at != std::string::npos;
         at = head.find("\"bone\":", at + 1))
        e.bone_index.push_back((int)std::strtol(head.c_str() + at + 7, nullptr, 10));

    const size_t body_at = 12 + head.size();
    if (e.frames > 0 && e.bones > 0 && (int64_t)body_at < n) {
        const size_t floats = (size_t)e.bones * (size_t)e.frames * 12;
        if (body_at + floats * 4 <= (size_t)n) {
            e.body.resize(floats);
            std::memcpy(e.body.data(), blob + body_at, floats * 4);
        }
    }
    bf6_free(c, blob);
    return e;
}

/* THE ROTATION BASIS. The record's 12 floats are three rows of the 3x3
 * rotation followed by the translation, so a valid rotation has all three rows
 * unit length and mutually perpendicular. Checking only length would accept a
 * shear; checking only one row would accept a read that went wrong two
 * components in. */
void check_orthonormal(const Export& e, const char* who)
{
    if (e.body.empty()) { bad("no float body to check"); return; }
    double worst_len = 0.0, worst_dot = 0.0;
    long checked = 0;
    for (long t = 0; t < e.bones; ++t) {
        for (long f = 0; f < e.frames; ++f) {
            const float* m = &e.body[((size_t)t * (size_t)e.frames + (size_t)f) * 12];
            for (int r = 0; r < 3; ++r) {
                const double len = std::sqrt((double)m[r*3+0]*m[r*3+0] +
                                             (double)m[r*3+1]*m[r*3+1] +
                                             (double)m[r*3+2]*m[r*3+2]);
                worst_len = std::fmax(worst_len, std::fabs(len - 1.0));
            }
            for (int a = 0; a < 3; ++a) {
                const int b = (a + 1) % 3;
                const double dot = (double)m[a*3+0]*m[b*3+0] +
                                   (double)m[a*3+1]*m[b*3+1] +
                                   (double)m[a*3+2]*m[b*3+2];
                worst_dot = std::fmax(worst_dot, std::fabs(dot));
            }
            ++checked;
        }
    }
    std::printf("  %s: %ld matrices, worst row length error %.5f, worst row dot %.5f\n",
                who, checked, worst_len, worst_dot);
    /* Half-float channel payloads and a float32 round trip put the floor a good
     * way above epsilon; a wrong stride misses by whole tenths, not thousandths. */
    if (worst_len > 0.01) bad("rotation rows are not unit length");
    if (worst_dot > 0.01) bad("rotation rows are not perpendicular");
}

/* THE POSE TRAP. Frame 0 against every other frame: if the clip is a still
 * hold, this is identically zero and nothing else in the test would notice. */
double motion_of(const Export& e)
{
    if (e.body.empty() || e.frames < 2) return 0.0;
    double worst = 0.0;
    for (long t = 0; t < e.bones; ++t) {
        const float* first = &e.body[(size_t)t * (size_t)e.frames * 12];
        for (long f = 1; f < e.frames; ++f) {
            const float* m = &e.body[((size_t)t * (size_t)e.frames + (size_t)f) * 12];
            for (int k = 0; k < 12; ++k)
                worst = std::fmax(worst, std::fabs((double)m[k] - first[k]));
        }
    }
    return worst;
}

} /* namespace */

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: soldier_1p_clip_test <game>\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }

    const char* const REQ_3P =
        "{\"character\":\"cha0001wisp\",\"outfit\":\"001\",\"faction\":\"alliance\","
        "\"role\":\"assault\",\"item\":\"carbine/m4a1\"}";
    const char* const REQ_1P =
        "{\"character\":\"cha0001wisp\",\"outfit\":\"001\",\"faction\":\"alliance\","
        "\"role\":\"assault\",\"item\":\"carbine/m4a1\",\"view\":\"1p\"}";

    const Export third = run(c, REQ_3P);
    const Export first = run(c, REQ_1P);

    std::printf("third person: frames %ld bones %ld bytes %lld\n",
                third.frames, third.bones, (long long)third.bytes);
    std::printf("first person: frames %ld bones %ld bytes %lld\n",
                first.frames, first.bones, (long long)first.bytes);

    if (third.frames < 1 || third.bones < 1)
        bad("the third-person clip did not export - the control failed, so nothing "
            "below is evidence about the 1P path");
    if (first.frames < 1) bad("the first-person clip exported no frames");
    if (first.bones < 1)  bad("the first-person clip drives no bones");

    /* THE CHECK THAT CATCHES A WIRING MISTAKE. Identical bone counts would mean
     * the 1P request quietly took the 3P path. */
    if (first.bones == third.bones && first.frames == third.frames)
        bad("the 1P export is identical to the 3P one - the view switch did nothing");

    if ((long)first.bone_index.size() != first.bones)
        bad("the tracks array does not have one entry per bone");
    {
        std::vector<int> seen = first.bone_index;
        std::sort(seen.begin(), seen.end());
        if (!seen.empty() && seen.front() < 0) bad("a track names a negative bone");
        if (std::adjacent_find(seen.begin(), seen.end()) != seen.end())
            bad("two tracks drive the same bone - the last writer silently wins");
    }

    check_orthonormal(first, "first person");

    const double moved = motion_of(first);
    std::printf("  first person: largest change from frame 0 is %.5f\n", moved);
    if (moved < 1e-4)
        bad("every frame of the 1P clip is frame 0 - this is a still pose, not motion");

    /* THE JOIN. The clip and the soldier are two separate calls, and the tracks
     * address bones by INDEX. An engine builds its Skeleton3D from the skinned
     * record's rig and then plays these tracks against it, so if the two
     * disagree about what bone 87 is, every check above still passes and the
     * arms animate from the wrong joints. Nothing downstream can detect that;
     * it has to be checked here, where both numbers are in one process.
     *
     * A skinned 1P record is also the precondition for animating at all - an
     * unskinned record is a baked mesh with no skeleton to drive. */
    {
        const char* const REQ_1P_SKINNED =
            "{\"character\":\"cha0001wisp\",\"outfit\":\"001\",\"faction\":\"alliance\","
            "\"role\":\"assault\",\"item\":\"carbine/m4a1\",\"view\":\"1p\",\"skinned\":\"1\"}";
        uint8_t* blob = nullptr;
        const int64_t n = bf6_loadout_soldier(c, REQ_1P_SKINNED, nullptr, &blob);
        if (n <= 0 || !blob) {
            bad("the skinned 1P soldier record could not be built - nothing to animate");
        } else {
            const std::string head = head_of(blob, n);
            /* One "name" per rig entry is the bone count; the record writes the
             * rig as an array of objects and this only needs how many. */
            long rig_bones = 0;
            const size_t rig_at = head.find("\"rig\":[");
            if (rig_at != std::string::npos)
                for (size_t a = head.find("\"name\":", rig_at); a != std::string::npos;
                     a = head.find("\"name\":", a + 1))
                    ++rig_bones;
            std::printf("  skinned 1P record: rig carries %ld bone(s)\n", rig_bones);
            if (rig_bones < 1)
                bad("the skinned 1P record carries no rig - Godot would build a "
                    "static mesh with no skeleton to animate");
            long worst = -1;
            for (int b : first.bone_index) if (b > worst) worst = b;
            std::printf("  clip's highest track bone index is %ld\n", worst);
            if (rig_bones > 0 && worst >= rig_bones)
                bad("a clip track addresses a bone the skinned record's rig does "
                    "not have - the two are built on different skeletons");
            bf6_free(c, blob);
        }
    }

    std::printf("\n%s\n", fails == 0 ? "PASS: 0 failure(s)"
                                     : "FAILED");
    bf6_close(c);
    return fails ? 1 : 0;
}
