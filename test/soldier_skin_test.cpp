/* THE SKINNED SOLDIER RECORD, checked against the pose it replaces.
 *
 *   soldier_skin_test <game_dir>
 *
 * "skinned":"1" makes bf6_loadout_soldier hand back BIND-pose vertices, the
 * per-vertex skin binding and the composed rig, so an engine can pose the mesh
 * itself and therefore animate it. Everything downstream - a Skeleton3D, an
 * animation, first-person eventually - rests on that record being right, and a
 * wrong rig does not fail: it produces a soldier that looks almost correct and
 * drifts on exactly the bones that are hardest to see.
 *
 * TWO CHECKS, because the first one alone is a trap.
 *
 * 1. THE BIND IDENTITY, which bf6_core.h prescribes: "At bind time pose ==
 *    model and the inverse cancels it, which is the cheapest way to check a rig
 *    is wired correctly: the mesh must come back unchanged." Compose each
 *    bone's model pose from the BIND locals, build inverse[b] * model[b], skin
 *    with it, and require the bind vertex back.
 *
 *    That check passes on things that are badly wrong. The palette is the
 *    IDENTITY there, so it cannot see a rig whose transforms are all identity,
 *    and it cannot see geometry that has been translated out of rig space -
 *    the identity happily carries the translation along. Both of those produce
 *    a soldier that renders perfectly until something poses it.
 *
 * 2. THE POSED EQUALITY, which is the real gate: compose the palette from
 *    `posed` instead, skin the BIND vertices with it, shift by `root`, and
 *    require the result to equal the DEFAULT record - the static posed soldier
 *    this mode is meant to replace. That is the whole claim in one number: same
 *    picture, different mechanism. It fails on an identity rig, on translated
 *    geometry, and on a mis-shifted renderbone, none of which check 1 sees.
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

/* Row-vector 3x4: p' = p.x*R + p.y*U + p.z*F + T, the convention bf6_bone uses. */
void mul43(const float a[12], const float b[12], float out[12])
{
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col)
            out[r * 3 + col] = a[r * 3 + 0] * b[0 + col] + a[r * 3 + 1] * b[3 + col]
                             + a[r * 3 + 2] * b[6 + col];
    for (int col = 0; col < 3; ++col)
        out[9 + col] = a[9 + 0] * b[0 + col] + a[9 + 1] * b[3 + col]
                     + a[9 + 2] * b[6 + col] + b[9 + col];
}

void point43(const float m[12], const float p[3], float out[3])
{
    for (int k = 0; k < 3; ++k)
        out[k] = p[0] * m[0 + k] + p[1] * m[3 + k] + p[2] * m[6 + k] + m[9 + k];
}

/* A very small JSON reach-in. The record's JSON is machine written and flat
 * enough that a full parser would be more code than the test. */
std::string slice(const std::string& s, size_t& at, char open, char close)
{
    const size_t a = s.find(open, at);
    if (a == std::string::npos) return std::string();
    int depth = 0;
    for (size_t i = a; i < s.size(); ++i) {
        if (s[i] == open) ++depth;
        else if (s[i] == close && --depth == 0) { at = i + 1; return s.substr(a, i - a + 1); }
    }
    return std::string();
}

double num_after(const std::string& s, const std::string& key, size_t from = 0)
{
    const size_t at = s.find("\"" + key + "\":", from);
    if (at == std::string::npos) return -1;
    return std::atof(s.c_str() + at + key.size() + 3);
}

typedef std::vector<float> Mat;

/* The 12 floats of one named array inside one JSON object. */
Mat mat_field(const std::string& obj, const char* key)
{
    Mat out(12, 0.f);
    size_t at = obj.find(std::string("\"") + key + "\":");
    if (at == std::string::npos) return out;
    const std::string arr = slice(obj, at, '[', ']');
    const char* p = arr.c_str() + 1;
    for (int k = 0; k < 12; ++k) {
        out[(size_t)k] = (float)std::atof(p);
        p = std::strchr(p, ',');
        if (p) ++p; else break;
    }
    return out;
}

struct Bones {
    std::vector<Mat> local, inverse, posed;
    std::vector<int> parent;
};

/* Every {...} of a bone array, in order. */
Bones read_bones(const std::string& arr)
{
    Bones b;
    size_t cur = 0;
    while (true) {
        const size_t open = arr.find('{', cur);
        if (open == std::string::npos) break;
        size_t scan = open;
        const std::string one = slice(arr, scan, '{', '}');
        cur = scan;
        if (one.empty()) break;
        b.parent.push_back((int)num_after(one, "parent"));
        b.local.push_back(mat_field(one, "local"));
        b.inverse.push_back(mat_field(one, "inverse"));
        b.posed.push_back(mat_field(one, "posed"));
    }
    return b;
}

/* Model poses from parent-relative transforms. Bones are topologically
 * ordered, so one forward pass is enough. `base` is the already-composed rig
 * that a renderbone's parent may point into; empty when composing the rig
 * itself. */
std::vector<Mat> compose(const std::vector<Mat>& rel, const std::vector<int>& parent,
                         const std::vector<Mat>& base, bool* order_ok)
{
    std::vector<Mat> model(rel.size(), Mat(12, 0.f));
    for (size_t i = 0; i < rel.size(); ++i) {
        const int par = parent[i];
        if (par < 0) { model[i] = rel[i]; continue; }
        if (base.empty()) {
            if (par >= (int)i) { if (order_ok) *order_ok = false; model[i] = rel[i]; continue; }
            mul43(rel[i].data(), model[(size_t)par].data(), model[i].data());
        } else if (par < (int)base.size()) {
            mul43(rel[i].data(), base[(size_t)par].data(), model[i].data());
        } else if ((size_t)(par - (int)base.size()) < i) {
            mul43(rel[i].data(), model[(size_t)(par - (int)base.size())].data(), model[i].data());
        } else {
            if (order_ok) *order_ok = false;
            model[i] = rel[i];
        }
    }
    return model;
}

/* One section's geometry offsets, in record order. */
struct SecRef {
    int vertex_count = 0, influences = 0, attach_bone = -1;
    long positions = -1, skin_bones = -1, skin_weights = -1;
    std::string json;
};

std::vector<SecRef> read_sections(const std::string& js)
{
    std::vector<SecRef> out;
    size_t at = js.find("\"sections\":");
    if (at == std::string::npos) return out;
    const std::string sections = slice(js, at, '[', ']');
    size_t cur = 0;
    while (true) {
        const size_t open = sections.find('{', cur);
        if (open == std::string::npos) break;
        size_t scan = open;
        const std::string sec = slice(sections, scan, '{', '}');
        cur = scan;
        if (sec.empty()) break;
        SecRef r;
        r.vertex_count = (int)num_after(sec, "vertex_count");
        r.influences = (int)num_after(sec, "influences");
        r.attach_bone = (int)num_after(sec, "attach_bone");
        r.positions = (long)num_after(sec, "positions");
        r.skin_bones = (long)num_after(sec, "skin_bones");
        r.skin_weights = (long)num_after(sec, "skin_weights");
        r.json = sec;
        out.push_back(r);
    }
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: soldier_skin_test <game>\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }

    const char* kReq = "{\"character\":\"cha0001wisp\",\"outfit\":\"001\","
                       "\"faction\":\"alliance\",\"role\":\"assault\",\"skinned\":\"1\"}";
    uint8_t* blob = nullptr;
    const int64_t n = bf6_loadout_soldier(c, kReq, "", &blob);
    if (n < 12 || !blob) { std::printf("FAIL no skinned record (%lld)\n", (long long)n); bf6_close(c); return 1; }
    if (std::memcmp(blob, "BLWP", 4) != 0) { std::printf("FAIL not a BLWP record\n"); return 1; }
    uint32_t json_len = 0;
    std::memcpy(&json_len, blob + 8, 4);
    const std::string js((const char*)blob + 12, json_len);
    const float* body = (const float*)(blob + 12 + json_len);

    /* ---- the rig ---------------------------------------------------------- */
    size_t at = js.find("\"rig\":");
    if (at == std::string::npos) { std::printf("FAIL the record carries no rig\n"); return 1; }
    const Bones rig = read_bones(slice(js, at, '[', ']'));
    std::printf("rig: %zu bone(s)\n", rig.local.size());
    if (rig.local.size() < 100) bad("the rig is implausibly small for a 291-bone soldier");

    bool order_ok = true;
    const std::vector<Mat> model  = compose(rig.local, rig.parent, {}, &order_ok);
    const std::vector<Mat> pmodel = compose(rig.posed, rig.parent, {}, &order_ok);
    if (!order_ok) bad("a bone's parent index is not lower than its own");

    /* palette = inverse * model, which at bind time must be the identity. */
    double worst_identity = 0.0;
    for (size_t i = 0; i < rig.local.size(); ++i) {
        float pal[12];
        mul43(rig.inverse[i].data(), model[i].data(), pal);
        static const float I43[12] = {1,0,0, 0,1,0, 0,0,1, 0,0,0};
        for (int k = 0; k < 12; ++k)
            worst_identity = std::max(worst_identity, (double)std::fabs(pal[k] - I43[k]));
    }
    std::printf("bind palette vs identity: worst element %.6g\n", worst_identity);
    if (worst_identity > 1e-3) bad("inverse * model is not the identity at bind pose - the rig is wrong");

    /* The posed palette must NOT be the identity, or check 2 below would be
     * comparing the bind mesh against itself and calling that a pass. */
    double posed_spread = 0.0;
    for (size_t i = 0; i < rig.local.size(); ++i) {
        float pal[12];
        mul43(rig.inverse[i].data(), pmodel[i].data(), pal);
        static const float I43[12] = {1,0,0, 0,1,0, 0,0,1, 0,0,0};
        for (int k = 0; k < 12; ++k)
            posed_spread = std::max(posed_spread, (double)std::fabs(pal[k] - I43[k]));
    }
    std::printf("posed palette vs identity: worst element %.4g\n", posed_spread);
    if (posed_spread < 1e-3) bad("the posed palette IS the identity - `posed` carries no pose");

    /* ---- the control record ----------------------------------------------- */
    const char* kPosed = "{\"character\":\"cha0001wisp\",\"outfit\":\"001\","
                         "\"faction\":\"alliance\",\"role\":\"assault\"}";
    uint8_t* posed_blob = nullptr;
    const int64_t pn = bf6_loadout_soldier(c, kPosed, "", &posed_blob);
    if (pn < 12 || !posed_blob) { std::printf("FAIL the posed control record could not be built\n"); return 1; }
    uint32_t pj = 0;
    std::memcpy(&pj, posed_blob + 8, 4);
    const std::string pjs((const char*)posed_blob + 12, pj);
    const float* pbody = (const float*)(posed_blob + 12 + pj);
    const std::vector<SecRef> psecs = read_sections(pjs);

    /* Where the engine is told to stand the skeleton, since skinned geometry
     * is left in rig space. */
    float root[3] = {0, 0, 0};
    size_t rat = js.find("\"root\":");
    if (rat == std::string::npos) {
        bad("the skinned record carries no root offset - the soldier cannot be placed");
    } else {
        const std::string ra = slice(js, rat, '[', ']');
        const char* p = ra.c_str() + 1;
        for (int k = 0; k < 3; ++k) { root[k] = (float)std::atof(p); p = std::strchr(p, ','); if (p) ++p; else break; }
    }
    std::printf("root offset: %.4f %.4f %.4f\n", root[0], root[1], root[2]);

    /* ---- the vertices ----------------------------------------------------- */
    const std::vector<SecRef> secs = read_sections(js);
    if (secs.size() != psecs.size())
        bad("the skinned and posed records disagree on how many sections the soldier has");
    int checked = 0, skinned_sections = 0, outside = 0, worst_index = -1, attached = 0;
    double worst_vertex = 0.0, worst_posed = 0.0;

    for (size_t si = 0; si < secs.size(); ++si) {
        const SecRef& s = secs[si];
        if (s.attach_bone >= 0) ++attached;
        if (s.vertex_count <= 0 || s.influences <= 0 || s.skin_bones < 0 || s.skin_weights < 0) continue;
        ++skinned_sections;
        /* THIS SECTION'S OWN RENDERBONES, appended above the shared base rig.
         * Resolving without them is what produced "index 297 in a 291-bone
         * rig", and it is exactly the step a consumer has to get right. */
        Bones rb;
        size_t rb_at = s.json.find("\"renderbones\":");
        if (rb_at != std::string::npos) {
            size_t scan = rb_at;
            rb = read_bones(slice(s.json, scan, '[', ']'));
        }
        bool rb_ok = true;
        const std::vector<Mat> rb_model  = compose(rb.local, rb.parent, model, &rb_ok);
        const std::vector<Mat> rb_pmodel = compose(rb.posed, rb.parent, pmodel, &rb_ok);
        if (!rb_ok) bad("a renderbone's parent is not resolvable");

        const float* pos = body + s.positions;
        const uint32_t* sb = (const uint32_t*)(body + s.skin_bones);
        const float* sw = body + s.skin_weights;
        const bool comparable = si < psecs.size()
                             && psecs[si].vertex_count == s.vertex_count
                             && psecs[si].positions >= 0;
        /* Every 97th vertex: this is a correctness check, not a benchmark, and
         * a wrong rig is wrong everywhere rather than in one corner. */
        for (int v = 0; v < s.vertex_count; v += 97) {
            float bind_acc[3] = {0, 0, 0}, posed_acc[3] = {0, 0, 0};
            float total = 0.f;
            bool broke = false;
            for (int lane = 0; lane < s.influences; ++lane) {
                const uint32_t b = sb[(size_t)v * s.influences + lane];
                const float w = sw[(size_t)v * s.influences + lane];
                if (w <= 0.f) continue;
                float pal[12], ppal[12], tp[3];
                if (b < rig.local.size()) {
                    mul43(rig.inverse[b].data(), model[b].data(), pal);
                    mul43(rig.inverse[b].data(), pmodel[b].data(), ppal);
                } else {
                    const size_t k2 = (size_t)b - rig.local.size();
                    if (k2 >= rb.local.size()) {
                        if ((int)b > worst_index) worst_index = (int)b;
                        ++outside;
                        broke = true;
                        break;
                    }
                    mul43(rb.inverse[k2].data(), rb_model[k2].data(), pal);
                    mul43(rb.inverse[k2].data(), rb_pmodel[k2].data(), ppal);
                }
                point43(pal, pos + (size_t)v * 3, tp);
                for (int k = 0; k < 3; ++k) bind_acc[k] += tp[k] * w;
                point43(ppal, pos + (size_t)v * 3, tp);
                for (int k = 0; k < 3; ++k) posed_acc[k] += tp[k] * w;
                total += w;
            }
            if (broke || total <= 1e-6f) continue;
            for (int k = 0; k < 3; ++k)
                worst_vertex = std::max(worst_vertex,
                    (double)std::fabs(bind_acc[k] / total - pos[(size_t)v * 3 + k]));
            if (comparable) {
                const float* want = pbody + psecs[si].positions + (size_t)v * 3;
                for (int k = 0; k < 3; ++k)
                    worst_posed = std::max(worst_posed,
                        (double)std::fabs(posed_acc[k] / total + root[k] - want[k]));
            }
            ++checked;
        }
    }
    std::printf("skinned sections %d, vertices sampled %d, worst bind drift %.6g m\n",
                skinned_sections, checked, worst_vertex);
    std::printf("skin indices outside the %zu-bone rig: %d (highest seen %d)\n",
                rig.local.size(), outside, worst_index);
    std::printf("POSED vs the static soldier: worst drift %.6g m over %d vertices\n",
                worst_posed, checked);
    std::printf("sections attached to a bone (the weapon): %d\n", attached);

    if (outside > 0) bad("skin indices exceed the base rig PLUS the section's renderbones");
    if (skinned_sections == 0) bad("the record carries no skinned section");
    if (checked == 0) bad("no vertex could be skinned at all");
    if (worst_vertex > 1e-3) bad("skinning at bind pose does NOT reproduce the mesh");
    /* A millimetre. The two paths differ only in float ordering, so anything
     * larger is a real disagreement about where the soldier's surface is. */
    if (worst_posed > 1e-3) bad("skinning by `posed` does NOT reproduce the static soldier");
    if (attached == 0) bad("no section names an attach bone - the weapon will not follow the hand");

    /* ---- the clip --------------------------------------------------------- */
    /* bf6_loadout_soldier_clip is meant to be the SAME pose, extended over
     * time: frame 0 of it must equal the rig's `posed` exactly. If the two
     * drift, a soldier built from the record and then driven by the animation
     * snaps on the first frame - visible, but only for a moment, and easy to
     * blame on blending instead of on the two paths disagreeing. */
    uint8_t* anim = nullptr;
    const int64_t an = bf6_loadout_soldier_clip(c, "{\"role\":\"assault\"}", &anim);
    if (an < 12 || !anim) {
        bad("the idle clip could not be exported");
    } else if (std::memcmp(anim, "BLWA", 4) != 0) {
        bad("the clip record is not a BLWA record");
    } else {
        uint32_t aj = 0;
        std::memcpy(&aj, anim + 8, 4);
        const std::string ajs((const char*)anim + 12, aj);
        const float* abody = (const float*)(anim + 12 + aj);
        const int frames = (int)num_after(ajs, "frames");
        const int nbones = (int)num_after(ajs, "bones");
        std::printf("clip: %d frame(s) over %d driven bone(s)\n", frames, nbones);
        if (frames < 2) bad("the clip has fewer than two frames - nothing can animate");
        if (nbones < 10) bad("the clip drives implausibly few bones");

        size_t tat = ajs.find("\"tracks\":");
        const std::string tracks = slice(ajs, tat, '[', ']');
        double worst_frame0 = 0.0, moved = 0.0;
        int compared = 0;
        size_t tc = 0;
        while (true) {
            const size_t open = tracks.find('{', tc);
            if (open == std::string::npos) break;
            size_t scan = open;
            const std::string one = slice(tracks, scan, '{', '}');
            tc = scan;
            if (one.empty()) break;
            const int bone = (int)num_after(one, "bone");
            const long off = (long)num_after(one, "track");
            if (bone < 0 || bone >= (int)rig.posed.size() || off < 0) continue;
            for (int k = 0; k < 12; ++k)
                worst_frame0 = std::max(worst_frame0,
                    (double)std::fabs(abody[off + k] - rig.posed[(size_t)bone][(size_t)k]));
            /* And the clip must actually go somewhere: a track that is frame 0
             * repeated is a decode that produced one pose, not an animation. */
            if (frames > 1)
                for (int f = 1; f < frames; ++f)
                    for (int k = 0; k < 12; ++k)
                        moved = std::max(moved,
                            (double)std::fabs(abody[off + (long)f * 12 + k] - abody[off + k]));
            ++compared;
        }
        std::printf("clip frame 0 vs the record's `posed`: worst %.6g over %d track(s)\n",
                    worst_frame0, compared);
        std::printf("largest change from frame 0 anywhere in the clip: %.4g\n", moved);
        if (compared == 0) bad("no clip track could be matched to a rig bone");
        if (worst_frame0 > 1e-6) bad("clip frame 0 is NOT the pose the soldier record ships");
        if (moved <= 0.0) bad("every frame of the clip is identical - this is a pose, not an animation");
        bf6_blob_free(anim);
    }

    bf6_blob_free(blob);
    bf6_blob_free(posed_blob);
    bf6_close(c);
    std::printf("\n%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
