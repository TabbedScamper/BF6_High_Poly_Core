/* THE FIRST-PERSON SOLDIER: arms and a weapon, anchored on the eye.
 *
 *   soldier_1p_view_test <game_dir>
 *
 * "view":"1p" builds what a player sees of themselves instead of the whole
 * soldier seen from outside: the character's own arms on ske_soldier_1p, posed
 * by a first-person clip, holding their configured weapon, with the origin at
 * CameraJoint so an engine parents the record straight onto its camera.
 *
 * Every way this goes wrong is quiet:
 *
 *   - the body creeps in, and the player is standing inside their own head;
 *   - the arms come back at bind pose, which is a T-pose, not a hold;
 *   - the 3P rig gets paired with the 1P clip, which binds completely and puts
 *     every channel on a plausible wrong bone;
 *   - the origin stays on the floor, so the arms hang at ankle height.
 *
 * So this checks the SHAPE of the result - what is in it, where it sits, how
 * big it is - rather than that a record came back.
 */
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int fails = 0;
void bad(const char* what) { std::printf("  FAIL %s\n", what); ++fails; }

double num_after(const std::string& s, const char* key, size_t from = 0)
{
    const std::string k = std::string("\"") + key + "\":";
    const size_t at = s.find(k, from);
    return at == std::string::npos ? -1.0 : std::atof(s.c_str() + at + k.size());
}

struct Record {
    std::string json;
    std::vector<float> body;
    bool ok = false;
};

Record build(bf6_ctx* c, const char* view)
{
    Record r;
    std::string req = std::string("{\"character\":\"cha0001wisp\",\"outfit\":\"001\","
                                  "\"faction\":\"alliance\",\"role\":\"assault\",\"view\":\"")
                    + view + "\"}";
    uint8_t* blob = nullptr;
    const int64_t n = bf6_loadout_soldier(c, req.c_str(), "", &blob);
    if (n < 12 || !blob) return r;
    uint32_t jl = 0;
    std::memcpy(&jl, blob + 8, 4);
    r.json.assign((const char*)blob + 12, jl);
    const float* f = (const float*)(blob + 12 + jl);
    const size_t floats = (size_t)((n - 12 - jl) / 4);
    r.body.assign(f, f + floats);
    bf6_blob_free(blob);
    r.ok = true;
    return r;
}

/* Every section's mesh name, and the bounding box over every vertex. */
void survey(const Record& r, std::vector<std::string>& meshes, float lo[3], float hi[3],
            int& vertices)
{
    for (int k = 0; k < 3; ++k) { lo[k] = 1e30f; hi[k] = -1e30f; }
    vertices = 0;
    size_t at = 0;
    while (true) {
        at = r.json.find("\"mesh\":\"", at);
        if (at == std::string::npos) break;
        const size_t end = r.json.find('"', at + 8);
        if (end == std::string::npos) break;
        meshes.push_back(r.json.substr(at + 8, end - at - 8));
        const int vc = (int)num_after(r.json, "vertex_count", end);
        const long pos = (long)num_after(r.json, "positions", end);
        if (vc > 0 && pos >= 0 && (size_t)pos + (size_t)vc * 3 <= r.body.size()) {
            for (int v = 0; v < vc; ++v)
                for (int k = 0; k < 3; ++k) {
                    const float x = r.body[(size_t)pos + (size_t)v * 3 + (size_t)k];
                    if (x < lo[k]) lo[k] = x;
                    if (x > hi[k]) hi[k] = x;
                }
            vertices += vc;
        }
        at = end;
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: soldier_1p_view_test <game>\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }

    const Record one = build(c, "1p");
    const Record three = build(c, "3p");
    if (!one.ok) { std::printf("FAIL the first-person soldier could not be built\n"); return 1; }
    if (!three.ok) { std::printf("FAIL the third-person control could not be built\n"); return 1; }
    const std::string e1 = one.json.substr(0, 200);
    if (one.json.find("\"error\":\"\"") == std::string::npos)
        { std::printf("FAIL first person: %s\n", e1.c_str()); return 1; }

    std::vector<std::string> m1, m3;
    float lo1[3], hi1[3], lo3[3], hi3[3];
    int v1 = 0, v3 = 0;
    survey(one, m1, lo1, hi1, v1);
    survey(three, m3, lo3, hi3, v3);

    std::printf("first person : %zu section(s), %d vertices\n", m1.size(), v1);
    std::printf("third person : %zu section(s), %d vertices\n", m3.size(), v3);

    /* ---- ARMS, BODY, AND THE WEAPON --------------------------------------
     * A HEAD or a backpack in here would mean the player is inside their own
     * character. The BODY is not an intruder: `_1p_mesh` is the first-person
     * legs and lower torso (bones 11-36, Hips, both legs, Spine..Spine2, no
     * head), shipped 1:1 beside `_1parms_mesh` for all 323 characters, and it
     * is what the player sees when they look down.
     *
     * This test used to call it an intruder and fail. That was the belief the
     * 1P branch was written to - "their own arms, and nothing else" - and it
     * made the check agree with the code rather than with the game. The tell is
     * that `_1p_mesh` was excluded from the 3P sweep too: an asset that no path
     * loads, shipped per outfit for every character, is not an asset nobody
     * meant to draw.
     *
     * Weapon sections come from the armory and are not character meshes, so
     * they are identified by NOT being under the character root. */
    int arms = 0, body = 0, weapon = 0, intruders = 0;
    for (const std::string& m : m1) {
        /* THE WEAPON TEST COMES FIRST, because weapon parts are named
         * `_1p_mesh` too - ob_wep_carbine_m4a1_base_1p_mesh and friends. Asking
         * about the suffix before asking where the mesh lives counted all
         * twelve weapon sections as body and then failed for having no weapon.
         * "Under the character root" is the question that actually separates
         * them; the suffix only distinguishes body from arms once it does. */
        if (m.find("_1parms") != std::string::npos) ++arms;
        else if (m.find("/characters/") == std::string::npos) ++weapon;
        else if (m.find("_1p_mesh") != std::string::npos) ++body;
        else { ++intruders; std::printf("   INTRUDER %s\n", m.c_str()); }
    }
    std::printf("   arms sections %d, body sections %d, weapon sections %d, other %d\n",
                arms, body, weapon, intruders);
    if (arms == 0) bad("the first-person build has no arms");
    if (body == 0) bad("the first-person build has no body to look down at");
    if (weapon == 0) bad("the first-person build has no weapon");
    if (intruders > 0) bad("the first-person build contains character meshes that are neither arms nor the 1P body");

    /* ---- ANCHORED ON THE EYE ---------------------------------------------
     * The origin is CameraJoint, so the arms hang BELOW it and reach forward.
     * If the record were still placed feet-on-floor, everything would sit a
     * metre and a half up instead. */
    std::printf("   bounds x %.3f..%.3f  y %.3f..%.3f  z %.3f..%.3f\n",
                lo1[0], hi1[0], lo1[1], hi1[1], lo1[2], hi1[2]);
    if (lo1[1] > -0.1)
        bad("nothing sits below the origin - the arms are not anchored on the eye");
    if (hi1[1] > 0.6)
        bad("geometry reaches well above the eye - this looks placed on the floor");
    /* NOW AS TALL AS A STANDING SOLDIER, and that is the correct answer once
     * the body is included: the origin is the eye, the feet are on the floor,
     * so the record reaches about eye height DOWNWARDS. A camera at 1.56 m
     * means roughly -1.56 at the feet.
     *
     * The bound that still means something is the other direction - geometry
     * far ABOVE the eye would say the record was placed feet-on-floor after
     * all, which hi1[1] > 0.6 above already catches. So this checks the feet
     * land near the floor rather than that the build is short. */
    const float height = hi1[1] - lo1[1];
    if (height > 2.2f)
        bad("the first-person build is taller than a standing soldier");
    if (lo1[1] > -1.2f)
        bad("nothing reaches down to the feet - the 1P body is missing or misplaced");

    /* ---- A HOLD, NOT A BIND POSE -----------------------------------------
     * Arms at bind pose are straight out to the sides. Posed, they come
     * forward. Comparing the span across X against the reach along Z separates
     * the two without needing to know what the pose looks like. */
    const float span_x = hi1[0] - lo1[0];
    const float reach_z = hi1[2] - lo1[2];
    std::printf("   span across %.3f m, reach forward %.3f m\n", span_x, reach_z);
    if (span_x > 1.6f)
        bad("the arms are spread wide - this is a bind pose, not a hold");

    /* ---- the control ------------------------------------------------------
     * The third-person soldier must still be a whole soldier, or "1p only has
     * arms" would be true because everything broke. */
    if (m3.size() <= m1.size())
        bad("the third-person soldier has no more sections than the first-person one");
    if (hi3[1] - lo3[1] < 1.5f)
        bad("the third-person control is not soldier-sized any more");
    std::printf("third person control: %.3f m tall over %zu sections\n",
                hi3[1] - lo3[1], m3.size());

    bf6_close(c);
    std::printf("\n%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
