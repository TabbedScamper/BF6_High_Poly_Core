/* EVERY ROLE STANDS IN ITS OWN POSE, FROM THE SAME TAKE.
 *
 *   role_pose_test <game_dir>
 *
 * Reported as "the poses for recon and support are wrong". They were. The clip
 * resolver took the first suffix that resolved, and the front-end mount carries
 * assault's "_01" but not engineer's, support's or recon's - so assault played
 * the _01 take and the other three silently played _02, a different performance
 * of a different length. Nothing errored, because each of them had found a clip.
 *
 * Two things have to hold, and only both together mean anything:
 *
 *   SAME TAKE. All four resolve to ui_frontend_standing_idle_<role>_01. A test
 *   that only checked "a clip was found" is what let this through.
 *
 *   DIFFERENT POSES. The four are genuinely distinct, so "they all look the
 *   same now" would be a failure rather than a fix. Frame 0 of each is compared
 *   against every other.
 */
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int fails = 0;
void bad(const char* what) { std::printf("  FAIL %s\n", what); ++fails; }

const char* const kRoles[4] = {"assault", "engineer", "support", "recon"};

/* The number after "key": in a machine-written record. */
double num_after(const std::string& s, const char* key)
{
    const std::string k = std::string("\"") + key + "\":";
    const size_t at = s.find(k);
    return at == std::string::npos ? -1.0 : std::atof(s.c_str() + at + k.size());
}

/* The record's rig, posed. Frame 0 of the role's clip, as the soldier record
 * itself reports it - so this measures what the preview will draw, not what a
 * clip file contains in isolation. */
bool posed_of(bf6_ctx* c, const char* role, std::vector<float>& out, std::string& clip)
{
    out.clear();
    clip.clear();
    std::string req = std::string("{\"character\":\"cha0001wisp\",\"outfit\":\"001\","
                                  "\"faction\":\"alliance\",\"skinned\":\"1\",\"role\":\"") + role + "\"}";
    uint8_t* blob = nullptr;
    const int64_t n = bf6_loadout_soldier(c, req.c_str(), "", &blob);
    if (n < 12 || !blob) return false;
    uint32_t jl = 0;
    std::memcpy(&jl, blob + 8, 4);
    const std::string js((const char*)blob + 12, jl);
    /* Every bone's posed transform, concatenated in rig order. */
    size_t at = 0;
    while (true) {
        at = js.find("\"posed\":[", at);
        if (at == std::string::npos) break;
        const char* p = js.c_str() + at + 9;
        for (int k = 0; k < 12; ++k) {
            out.push_back((float)std::atof(p));
            p = std::strchr(p, ',');
            if (!p) break;
            ++p;
        }
        at += 9;
    }
    bf6_blob_free(blob);

    /* And which clip it came from, which the clip record names outright. */
    uint8_t* anim = nullptr;
    std::string areq = std::string("{\"role\":\"") + role + "\"}";
    const int64_t an = bf6_loadout_soldier_clip(c, areq.c_str(), &anim);
    if (an >= 12 && anim) {
        uint32_t aj = 0;
        std::memcpy(&aj, anim + 8, 4);
        const std::string ajs((const char*)anim + 12, aj);
        const size_t a = ajs.find("\"clip\":\"");
        if (a != std::string::npos) {
            const size_t b = ajs.find('"', a + 8);
            if (b != std::string::npos) clip = ajs.substr(a + 8, b - a - 8);
        }
        bf6_blob_free(anim);
    }
    return !out.empty();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: role_pose_test <game>\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }

    std::vector<std::vector<float>> posed(4);
    std::string clips[4];
    for (int r = 0; r < 4; ++r) {
        if (!posed_of(c, kRoles[r], posed[(size_t)r], clips[r])) {
            bad("a role could not be posed at all");
            std::printf("       %s\n", kRoles[r]);
            continue;
        }
        const char* tail = std::strrchr(clips[r].c_str(), '/');
        std::printf("%-9s %5zu floats   %s\n", kRoles[r], posed[(size_t)r].size(),
                    tail ? tail + 1 : clips[r].c_str());
    }

    /* SAME TAKE - and specifically the _02 one, which is what the loadout
     * controllers actually reference (Class.Loadout.Node -> classloadout.cbd ->
     * ui.loadout.idcl.<class>.rc, whose unit-weight base entry is
     * idle[_cl]_<class>_02). The _01 clips are a different surface's, and
     * preferring them was this test's first wrong answer. */
    for (int r = 0; r < 4; ++r) {
        const std::string want = std::string("ui_frontend_standing_idle_") + kRoles[r] + "_02";
        if (clips[r].size() < want.size()
            || clips[r].compare(clips[r].size() - want.size(), want.size(), want) != 0) {
            bad("a role does not use the loadout screen's _02 take");
            std::printf("       %s -> %s\n", kRoles[r], clips[r].c_str());
        }
    }

    /* DIFFERENT POSES. */
    std::printf("\npairwise, over the whole posed rig:\n");
    for (int a = 0; a < 4; ++a)
        for (int b = a + 1; b < 4; ++b) {
            if (posed[(size_t)a].empty() || posed[(size_t)b].empty()) continue;
            const size_t n = posed[(size_t)a].size() < posed[(size_t)b].size()
                           ? posed[(size_t)a].size() : posed[(size_t)b].size();
            double worst = 0.0;
            int differing = 0;
            for (size_t k = 0; k < n; ++k) {
                const double d = std::fabs((double)posed[(size_t)a][k] - (double)posed[(size_t)b][k]);
                if (d > 1e-6) ++differing;
                if (d > worst) worst = d;
            }
            std::printf("   %-9s vs %-9s worst %.6g, %d of %zu differ\n",
                        kRoles[a], kRoles[b], worst, differing, n);
            if (differing == 0)
                bad("two roles stand in exactly the same pose");
        }

    /* ---- eye height -------------------------------------------------------
     * Walk mode stood every soldier's camera at a documented 1.72 m default.
     * The rig knows better, per character and per pose, and now reports it.
     * What is checked is that the number is PLAUSIBLE AND VARIES: a constant
     * would mean the measurement is not really happening, and anything outside
     * human range would mean it is measuring the wrong joint. */
    std::printf("\neye height, measured from the ground the soldier is placed on:\n");
    const char* const kChars[4] = {"cha0001wisp", "cha0002know", "cha0004jazz", "cha0007dust"};
    double lowest = 1e9, highest = -1e9;
    int measured = 0;
    for (int i = 0; i < 4; ++i) {
        for (int r = 0; r < 2; ++r) {
            std::string req = std::string("{\"character\":\"") + kChars[i]
                            + "\",\"outfit\":\"001\",\"faction\":\"alliance\",\"role\":\""
                            + kRoles[r] + "\"}";
            uint8_t* blob = nullptr;
            const int64_t n = bf6_loadout_soldier(c, req.c_str(), "", &blob);
            if (n < 12 || !blob) continue;
            uint32_t jl = 0;
            std::memcpy(&jl, blob + 8, 4);
            const std::string js((const char*)blob + 12, jl);
            const double eye = num_after(js, "eye");
            bf6_blob_free(blob);
            if (eye <= 0) continue;
            std::printf("   %-14s %-9s %.4f m\n", kChars[i], kRoles[r], eye);
            lowest = eye < lowest ? eye : lowest;
            highest = eye > highest ? eye : highest;
            ++measured;
        }
    }
    if (measured == 0) {
        bad("no soldier reported an eye height");
    } else {
        std::printf("   range %.4f .. %.4f m over %d soldier(s)\n", lowest, highest, measured);
        if (lowest < 1.2 || highest > 2.1)
            bad("an eye height is outside any plausible human range - wrong joint?");
        if (highest - lowest < 1e-4)
            bad("every soldier has exactly the same eye height - it is not being measured");
    }

    bf6_close(c);
    std::printf("\n%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
