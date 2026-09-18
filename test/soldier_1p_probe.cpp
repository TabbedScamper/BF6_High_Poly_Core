/* THE FIRST-PERSON CHAIN: skeleton, arms, rig, clips.
 *
 *   soldier_1p_probe <game_dir> [character] [outfit]
 *
 * Walk mode currently puts the camera at eye height and parents the weapon to
 * it at an offset I authored. That is a stand-in. The real thing is the 1P
 * skeleton with the character's own arms skinned to it and the weapon's own 1P
 * animations driving both - which is how the game holds a rifle, and why the
 * gun moves when you sprint.
 *
 * Everything the 3P soldier needed has a 1P counterpart, and this checks each
 * one exists and joins up rather than assuming the symmetry:
 *
 *   render skeleton   common/characters/_soldier/ske_soldier_1p      (3p: 291 bones)
 *   animation rig     animations/glacier/global/rigging/soldier_1p.rig
 *   arms mesh         <character>/set/set_<outfit>/<character>_set_<outfit>_1parms_mesh
 *   clips             animations/glacier/assets/1p/common/<weapon class>/loco/...
 *
 * A binding count of zero against the 1P rig would mean the 3P pairing does not
 * transfer, and that is the one thing worth knowing before building anything on
 * top of it.
 */
#include "bf6_core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int fails = 0;
void bad(const char* what) { std::printf("  FAIL %s\n", what); ++fails; }

const char* const k1pSkeleton = "common/characters/_soldier/ske_soldier_1p";
const char* const k3pSkeleton = "common/characters/_soldier/ske_soldier_3p";
const char* const k1pRig = "animations/glacier/global/rigging/soldier_1p.rig";
const char* const k3pRig = "animations/glacier/global/rigging/soldier_3p.rig";

void describe_skeleton(bf6_ctx* c, const char* name)
{
    bf6_skeleton* s = bf6_skeleton_read(c, name);
    if (!s) { std::printf("%-46s UNREADABLE\n", name); bad("a soldier skeleton could not be read"); return; }
    std::printf("%-46s %d bone(s)\n", name, s->bone_count);
    /* The first few and any that name a hand or a weapon, since those are the
     * ones a first-person view actually hangs things off. */
    int shown = 0;
    for (int i = 0; i < s->bone_count && shown < 6; ++i) {
        const char* n = s->bones[i].name;
        if (!n) continue;
        std::printf("      [%3d] %-28s parent %d\n", i, n, s->bones[i].parent);
        ++shown;
    }
    for (int i = 0; i < s->bone_count; ++i) {
        const char* n = s->bones[i].name;
        if (!n) continue;
        if (std::strstr(n, "Wep_Align") || std::strcmp(n, "RightHand") == 0
            || std::strcmp(n, "Camera") == 0 || std::strstr(n, "Wep_IK_RightHand"))
            std::printf("      [%3d] %-28s parent %d   <- an attachment point\n",
                        i, n, s->bones[i].parent);
    }
    bf6_free(c, s);
}

/* Bindings for one clip against a rig/skeleton pair. 0 means the pair does not
 * resolve, which is the answer this probe exists to get. */
int bindings_for(bf6_ctx* c, const char* clip, const char* rig, const char* skeleton)
{
    bf6_anim_binding_stats st{};
    const int n = bf6_anim_bindings(c, clip, rig, skeleton, nullptr, 0, &st);
    return n > 0 ? n : 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: soldier_1p_probe <game> [character] [outfit]\n"); return 2; }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string character = argc > 2 ? argv[2] : "cha0001wisp";
    const std::string outfit = argc > 3 ? argv[3] : "001";

    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_all(c, 1, err, (int)sizeof(err));

    std::printf("=== the two render skeletons ===\n");
    describe_skeleton(c, k3pSkeleton);
    std::printf("\n");
    describe_skeleton(c, k1pSkeleton);

    /* ---- the arms ------------------------------------------------------- */
    std::printf("\n=== this character's first-person arms ===\n");
    const std::string arms = "common/characters/mp/main/" + character + "/set/set_" + outfit
                           + "/" + character + "_set_" + outfit + "_1parms_mesh";
    const int have = bf6_list_ebx(c, arms.c_str(), nullptr, 0);
    std::printf("%s\n   %s\n", arms.c_str(), have > 0 ? "present" : "ABSENT");
    if (have <= 0) bad("the character has no first-person arms mesh at the 3P naming's counterpart");

    /* How many characters ship arms at all, so "absent" can be told apart from
     * "this one is unusual". */
    std::printf("\n%d partition(s) match \"_1parms_mesh\" across the install\n",
                bf6_list_ebx(c, "_1parms_mesh", nullptr, 0));

    /* ---- the clips ------------------------------------------------------ */
    std::printf("\n=== first-person animation clips ===\n");
    const int total = bf6_list_ebx(c, "animations/glacier/assets/1p/", nullptr, 0);
    std::printf("%d partition(s) under animations/glacier/assets/1p/\n", total);
    std::vector<std::string> sample;
    if (total > 0) {
        const int want = total < 4000 ? total : 4000;
        std::vector<bf6_asset> rows((size_t)want);
        const int got = bf6_list_ebx(c, "animations/glacier/assets/1p/", rows.data(), want);
        /* ONE OF EACH NAMING CONVENTION, not six of whichever is listed first.
         * The folder mixes p_ (pose), t_ (transition), c_ (cycle), a_ and
         * tadtv_ prefixes with dotted blend-space names, and a binding check
         * that only ever saw p_ would report "first person does not bind" when
         * the truth is that poses are not clips. */
        static const char* const kPrefixes[] = {"/c_1p_", "/t_1p_", "/a_1p_",
                                                "/p_1p_", "/tadtv_1p_"};
        for (const char* prefix : kPrefixes)
            for (int i = 0; i < got; ++i) {
                const char* n = rows[(size_t)i].name;
                if (n && std::strstr(n, prefix)) { sample.push_back(n); break; }
            }
        /* And a dotted one, which is a different kind of asset again. */
        for (int i = 0; i < got; ++i) {
            const char* n = rows[(size_t)i].name;
            const char* tail = n ? std::strrchr(n, '/') : nullptr;
            if (tail && std::strchr(tail, '.')) { sample.push_back(n); break; }
        }
    }
    if (sample.empty()) bad("no first-person clip could be listed");

    std::printf("\n%-78s %8s %8s\n", "clip", "1P rig", "3P rig");
    for (const std::string& clip : sample) {
        const int one = bindings_for(c, clip.c_str(), k1pRig, k1pSkeleton);
        const int three = bindings_for(c, clip.c_str(), k3pRig, k3pSkeleton);
        const char* tail = std::strrchr(clip.c_str(), '/');
        std::printf("%-78s %8d %8d\n", tail ? tail + 1 : clip.c_str(), one, three);
    }

    /* The claim worth testing: the 1P pair resolves at least one clip. If it
     * does not, the 3P pairing does not transfer and nothing above matters. */
    int resolved = 0;
    std::string bound;
    for (const std::string& clip : sample)
        if (bindings_for(c, clip.c_str(), k1pRig, k1pSkeleton) > 0) {
            ++resolved;
            if (bound.empty()) bound = clip;
        }
    std::printf("\n%d of %zu sampled clips bind against the 1P rig and skeleton\n",
                resolved, sample.size());
    if (resolved == 0)
        bad("no first-person clip binds against soldier_1p.rig + ske_soldier_1p");

    /* BOTH RIGS RETURN THE SAME BINDING COUNT, and a count is not a mapping.
     * Two skeletons of different sizes resolving the same number of channels
     * would read as "either rig works", which would be a comfortable and wrong
     * conclusion - the same shape of mistake as reading equal maxima as equal
     * clips. So compare where the channels actually LAND. */
    if (!bound.empty()) {
        std::printf("\n=== does the rig choice change anything? %s ===\n",
                    std::strrchr(bound.c_str(), '/') + 1);
        bf6_anim_binding_stats s1{}, s3{};
        const int n1 = bf6_anim_bindings(c, bound.c_str(), k1pRig, k1pSkeleton, nullptr, 0, &s1);
        const int n3 = bf6_anim_bindings(c, bound.c_str(), k3pRig, k3pSkeleton, nullptr, 0, &s3);
        std::vector<bf6_anim_binding> b1((size_t)(n1 > 0 ? n1 : 0));
        std::vector<bf6_anim_binding> b3((size_t)(n3 > 0 ? n3 : 0));
        if (n1 > 0) bf6_anim_bindings(c, bound.c_str(), k1pRig, k1pSkeleton, b1.data(), n1, &s1);
        if (n3 > 0) bf6_anim_bindings(c, bound.c_str(), k3pRig, k3pSkeleton, b3.data(), n3, &s3);
        int differing = 0, only1p = 0, only3p = 0, highest1p = -1, highest3p = -1;
        const size_t n = b1.size() < b3.size() ? b1.size() : b3.size();
        for (size_t i = 0; i < n; ++i) {
            if (b1[i].bone != b3[i].bone) ++differing;
            if (b1[i].bone >= 0 && b3[i].bone < 0) ++only1p;
            if (b3[i].bone >= 0 && b1[i].bone < 0) ++only3p;
            if (b1[i].bone > highest1p) highest1p = b1[i].bone;
            if (b3[i].bone > highest3p) highest3p = b3[i].bone;
        }
        std::printf("   channels %zu; %d resolve to a DIFFERENT bone under the two rigs\n",
                    n, differing);
        std::printf("   bound only under 1P: %d, only under 3P: %d\n", only1p, only3p);
        std::printf("   highest bone index: 1P %d (of 203), 3P %d (of 291)\n",
                    highest1p, highest3p);
        if (differing == 0)
            bad("the 1P and 3P rigs resolve every channel identically - the pairing "
                "may not matter, or one of them is not being applied");
    }

    /* ---- which STANDING clip can a first-person view actually use? --------
     * A 1P view wants a neutral standing hold. The obvious candidate,
     * p_1p_rifle_stand_idle_01, is a one-frame clip authored at 60 fps - the
     * right thing - but bf6_anim_bindings reports it unavailable. So sweep
     * every rifle-loco partition mentioning "stand" and report which resolve,
     * rather than guessing at a name that sounds standing. */
    std::printf("\n=== rifle loco clips mentioning \"stand\" ===\n");
    const char* const kLoco = "animations/glacier/assets/1p/common/rifle/loco/";
    const int loco = bf6_list_ebx(c, kLoco, nullptr, 0);
    int usable = 0;
    if (loco > 0) {
        std::vector<bf6_asset> rows((size_t)loco);
        const int got = bf6_list_ebx(c, kLoco, rows.data(), loco);
        for (int i = 0; i < got; ++i) {
            const char* n = rows[(size_t)i].name;
            if (!n || !std::strstr(n, "stand")) continue;
            const int b = bindings_for(c, n, k1pRig, k1pSkeleton);
            int frames = 0;
            if (b > 0) {
                bf6_anim_clip* clip = bf6_anim_clip_open(c, n);
                if (clip) { frames = clip->key_time_count; bf6_free(c, clip); }
            }
            if (b > 0) {
                ++usable;
                std::printf("   %-62s %4d bind %5d fr\n", std::strrchr(n, '/') + 1, b, frames);
            }
        }
    }
    std::printf("%d standing rifle clip(s) resolve against the 1P pair\n", usable);
    if (usable == 0)
        bad("no standing first-person rifle clip resolves - nothing to hold a pose with");

    /* ---- bindings against channels ---------------------------------------
     * The soldier build refuses a pose unless the clip's channel_count equals
     * its binding count. For the front-end 3P idles those agree (505 = 505).
     * If they do not agree for 1P, that equality is a 3P coincidence rather
     * than a rule, and the build is rejecting a perfectly good clip. */
    std::printf("\n=== bindings vs channels ===\n");
    for (const char* clip : {
            "animations/glacier/assets/1p/common/rifle/loco/t_1p_rifle_stand_idle_turn_inplace_left_01",
            "animations/glacier/assets/frontend/mainmenu/loadout/ui_frontend_standing_idle_assault_02"}) {
        const int b = bindings_for(c, clip, std::strstr(clip, "/1p/") ? k1pRig : k3pRig,
                                   std::strstr(clip, "/1p/") ? k1pSkeleton : k3pSkeleton);
        bf6_anim_clip* h = bf6_anim_clip_open(c, clip);
        const char* tail = std::strrchr(clip, '/') + 1;
        if (!h) { std::printf("   %-58s bindings %d, CLIP WILL NOT OPEN\n", tail, b); continue; }
        std::printf("   %-58s bindings %d, channels %d, frames %d%s\n",
                    tail, b, h->channel_count, h->key_time_count,
                    b == h->channel_count ? "" : "   <- they DISAGREE");
        bf6_free(c, h);
    }

    bf6_close(c);
    std::printf("\n%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
