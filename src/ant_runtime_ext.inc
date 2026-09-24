/* ant_runtime_ext.inc - the ANT runtime: controllers that produce poses over
 * time. A translation-unit fragment of bf6_core.cpp.
 *
 * Each controller type is implemented from a spec read out of the analysed
 * executable (the research repo's ANT findings; per-type specs cite the
 * function VAs). Where the spec leaves something OPEN, the behaviour is
 * omitted and named, never approximated:
 *
 *   ClipController (FUN_14081f600 / Extended FUN_1408202f0)
 *     - IMPLEMENTED: tick time base, duration ceil(len/rate), plain = hold
 *       last frame, Extended = start offset / loop (fmodf) / reverse, frame
 *       lookup with the animation's first-frame offset, cyclic wrap.
 *     - OMITTED (OPEN, no effect on the pose shown): interval (motion)
 *       sampling, the optional second animation merge (FUN_1407a74d0), rig
 *       retargeting, the readiness gate.
 *   PhaseController (FUN_1405ce480): child driven to value*length (or
 *     seconds*60), forward by delta, backward by snap.
 *
 * Time is in ticks of 1/60 s throughout; the caller's dt in seconds is
 * multiplied by 60 at the edge. */

#include "ant_graph.h"

namespace bf6ant {

constexpr float kTicksPerSecond = 60.0f;          /* 0x1493197b0 */
constexpr float kNeverEnds      = 1e35f;          /* 0x799a130c */

/* type guids */
constexpr const char* kT_Clip      = "1b4fee39-b54a-bbb3-7341-7843ed8f943f";
constexpr const char* kT_Phase     = "43bd5000-d9d2-fe56-5651-c6f82ac9c3a0";
constexpr const char* kT_DctAnim   = "3a770f61-3d12-3ca2-cdae-b384a29b07cd";

struct Runtime;

/* The controller base: the engine's shared object fields (FUN_1408cb200). */
struct Ctrl {
    const Obj* asset = nullptr;
    float phase = 0.f;        /* +0x18 */
    float time = 0.f;         /* +0x1c ticks */
    float length = 1.f;       /* +0x20 ticks */
    float remaining = kNeverEnds; /* +0x24 */
    float phase_range = 1.f;  /* +0x28 */
    bool  looping = false;    /* +0x2c bit0 */
    bool  wrapped = false;    /* +0x2c bit1 */
    virtual ~Ctrl() = default;
    virtual void update(Runtime& rt, float dt, Pose& out) = 0;
    virtual void set_time(Runtime& rt, float t) = 0;
    virtual void set_phase(Runtime& rt, float p) = 0;
    virtual void restart(Runtime& rt) { set_time(rt, 0.f); }
    /* QueryInterface(0x317458a7): the controller whose tags (and time block)
     * answer branch-window queries. Null = the interface is not answered. */
    virtual Ctrl* tag_leaf() { return nullptr; }
    /* Whether what this controller produces is a DELTA rather than a pose. A
     * clip says so itself (0x5deb11ed); measured over every clip in the rifle
     * locomotion folder, that flag is 1 exactly when the asset name carries the
     * adtv_ or ladtv_ additive prefix, 22 of 22. */
    virtual bool is_additive() const { return false; }
    /* The BranchOutPointTags a tag leaf carries (clip tag set, 0x70b22fd5). */
    virtual const std::vector<const Obj*>* branch_tags() const { return nullptr; }
    float scaled_length() const { return length * phase_range; }   /* slot 0x88 */
    /* FUN_140613ec0 */
    float normalise(float p) const {
        const float s = phase_range;
        if (looping) {
            const float x = p - std::trunc(p / s) * s;
            if (x >= 0.f) return x;
            return std::fmin(s - s * 1.1920929e-7f, std::fmax(s + x, 0.f));
        }
        return std::fmin(std::fmax(p, 0.f), s);
    }
};

struct Runtime {
    bf6_ctx* c = nullptr;
    Graph g;
    States st;
    std::string rig, ske;
    int bones = 0;
    Pose bind;
    std::vector<std::string> notes;                 /* omitted/unknown behaviour met at runtime */
    std::map<std::string, std::unique_ptr<ClipSource>> clips;
    std::map<std::string, bf6_ant_cdb*> cdbs;
    std::map<std::string, int> bone_index;          /* skeleton bone name -> index */
    std::map<std::string, std::unique_ptr<Mask>> masks;   /* blend-mask asset -> built mask */
    bool first_update = true;                        /* the root's first evaluation */
    /* How deep make() is: 1 while the ROOT controller is being built. A node
     * that passes its parent's pose through is only a hole when there is no
     * parent, and that is a property of where the machine sits, not of the
     * node. */
    int make_depth = 0;
    /* THE POSE UNDERNEATH, when this runtime is running a LAYER rather than a
     * whole body. Many machines drive only a part - wep.ads.leftthumb.slc is
     * exactly what its name says - and nested in the real graph the rest of the
     * body arrives from the parent. Run standalone there is no parent, and a
     * layer whose undriven bones come out as bind-or-zero does not look partial,
     * it DESTROYS the pose it was supposed to sit on: measured, the ADS machine
     * wrote Hips at the origin where bind has it at 945 mm, dropping the whole
     * body out of the view. The caller supplies what the layer sits on. */
    Pose parent;
    bool have_parent = false;
    /* POSES HELD IN GAME STATES, keyed by the PoseAssetGS asset that names one.
     *
     * The research repo names type ff206dad PoseAssetGS - a pose GAME STATE, not an
     * output port - and that is what makes the graph's pose sharing readable. A layer
     * whose 0x899b420c names one WRITES its composed pose there; a layer whose
     * controller IS one READS it back. `upperbodyoverride.poseoutput` is written in
     * two containers and read by 1p.oneoffs.slc, and the transition and look-at
     * layers do the same thing with their own slots.
     *
     * Empty means nothing has written it yet, and a reader then contributes nothing,
     * which is the same as the layer being absent. */
    std::map<std::string, Pose> pose_states;
    explicit Runtime(bf6_ctx* cc) : c(cc), g(cc) {}
    ~Runtime() { for (auto& kv : cdbs) if (kv.second) bf6_free(c, kv.second); }
    void note(const std::string& s) {
        for (const auto& n : notes) if (n == s) return;
        notes.push_back(s);
    }
    ClipSource* clip(const std::string& path) {
        auto it = clips.find(path);
        if (it != clips.end()) return it->second.get();
        auto cs = std::make_unique<ClipSource>();
        std::string err;
        if (!cs->open(c, path, rig.c_str(), ske.c_str(), err)) { note("clip: " + err); clips[path] = nullptr; return nullptr; }
        ClipSource* p = cs.get();
        clips[path] = std::move(cs);
        return p;
    }
    std::unique_ptr<Ctrl> make(const Obj* asset);    /* the factory, per type */
};

namespace {
double numv(const bf6::EbxValue* v)
{
    if (!v) return 0.0;
    switch (v->kind) {
    case bf6::EbxValue::Kind::Real: return v->f;
    case bf6::EbxValue::Kind::Int:  return (double)v->i;
    case bf6::EbxValue::Kind::Uint: return (double)v->u;
    case bf6::EbxValue::Kind::Bool: return v->b ? 1.0 : 0.0;
    default: return 0.0;
    }
}
bool boolv(const bf6::EbxValue* v) { return v && v->kind == bf6::EbxValue::Kind::Bool && v->b; }
}  // namespace

constexpr const char* kT_BranchOutPointTag = "dc937b29-db69-2ed4-b629-27d5ddbd86ba";

/* Every tag of a tag-collection set (TagCollectionSet 0x2146a35a -> each
 * collection's 0x605f79af list), in authored order. */
void tag_set_tags(Runtime& rt, const Obj* set, std::vector<const Obj*>& out)
{
    if (!set) return;
    const bf6::EbxValue* cs = set->f(0x2146a35au);
    if (!cs) return;
    for (const auto& cr : cs->items) {
        const Obj* coll = rt.g.resolve(&cr, set);
        const bf6::EbxValue* ts = coll ? coll->f(0x605f79afu) : nullptr;
        if (!ts) continue;
        for (const auto& tr : ts->items) if (const Obj* t = rt.g.resolve(&tr, coll)) out.push_back(t);
    }
}

/* --------------------------------------------------------------- clip */

struct ClipCtrl : Ctrl {
    ClipSource* src = nullptr;
    float rate = 1.f;          /* asset +0x50 */
    float offset = 0.f;        /* asset +0x5c, Extended */
    bool  loop = false;        /* asset +0x63, Extended */
    int   reverse = 0;         /* asset +0x64, Extended */
    bool  additive = false;    /* 0x5deb11ed: the clip is a DELTA, not a pose */
    bool  extended = false;
    float duration = 1.f;      /* +0x68 */
    float anim_first = 0.f;    /* anim +0x30 */
    float anim_len = 0.f;      /* anim +0x34 */
    int   anim_last = 0;       /* anim +0x38 (N) */
    bool  cyclic = false;      /* anim +0x6c */
    std::vector<const Obj*> btags;   /* BranchOutPointTags of the clip's tag set */

    void collect_branch_tags(Runtime& rt, const Obj* a) {
        std::vector<const Obj*> all;
        tag_set_tags(rt, rt.g.resolve(a->f(0x70b22fd5u), a), all);
        for (const Obj* t : all) if (t->type == kT_BranchOutPointTag) btags.push_back(t);
    }
    /* ClipController QI 0x317458a7 -> obj+0x58 (tag queries), spec_clip_phase */
    Ctrl* tag_leaf() override { return this; }
    const std::vector<const Obj*>* branch_tags() const override { return &btags; }
    bool is_additive() const override { return additive; }

    void init(Runtime& rt, const Obj* a) {
        asset = a;
        rate = (float)numv(a->f(0x691d9180u));
        offset = (float)numv(a->f(0x2f2c4cabu));
        loop = boolv(a->f(0xd5593e48u));
        reverse = (int)numv(a->f(0x72e0ac4au));
        additive = numv(a->f(0x5deb11edu)) != 0.0;
        /* factory FUN_14081ffe0 */
        extended = offset > 0.f || loop || reverse == 1;
        looping = extended && loop;
        if (const Obj* anim = rt.g.resolve(a->f(0x26425085u), a)) {
            anim_first = (float)numv(anim->f(0x6c94c3a1u));
            anim_len = (float)numv(anim->f(0x2a910441u));
            anim_last = (int)numv(anim->f(0xd02a1002u));
            cyclic = boolv(anim->f(0x41debdb7u));
        }
        if (a->f(0xf1caef81u) && a->f(0xf1caef81u)->kind != bf6::EbxValue::Kind::Null)
            rt.note("clip second animation (+0x48) merge omitted (operator OPEN): " + a->path);
        src = rt.clip(a->path);
        collect_branch_tags(rt, a);
        /* ctor FUN_14081ef70: ceilf(len / rate) */
        duration = (anim_len > 0.f && rate != 0.f) ? std::ceil(anim_len / rate) : 1.f;
        length = duration;
        time = 0.f;
        remaining = looping ? kNeverEnds : duration - time;
        phase = length > 1e-4f ? time / length : 0.f;
    }

    /* Frame lookup (FUN_1408735d0) is done by the clip sampler; this supplies
     * the sample time in animation frames and whether it wraps. */
    void sample_at(Runtime& rt, float anim_frames, Pose& out) {
        out = rt.bind;
        if (!src) return;
        float ft = anim_frames + anim_first;
        if (!cyclic) ft = std::fmin(std::fmax(ft, 0.f), (float)anim_last);
        src->sample(rt.c, ft, cyclic, out);
    }

    void update(Runtime& rt, float dt, Pose& out) override {
        wrapped = false;
        const float t0 = time;
        const float t1 = t0 + std::fmin(dt, remaining);
        time = t1;
        phase = normalise(length > 1e-4f ? t1 / length : 0.f);
        if (!extended) {
            length = duration;
            remaining = duration - t1;
            sample_at(rt, rate * t1, out);
            return;
        }
        remaining = looping ? kNeverEnds : length - t1;
        /* FUN_1408202f0's segment arithmetic, then FUN_140820780's
         * single-sample choice: seg3 when the step wrapped, else seg1. */
        const float L = length, inv = 1.f / L;
        float a0 = reverse ? L - t0 : t0, a1 = reverse ? L - t1 : t1;
        a0 += offset; a1 += offset;
        float x;
        if (a1 < a0) {                                   /* reverse direction */
            const float base = std::floor(a1 * inv) * L;
            a0 -= base; a1 -= base;
            x = a1;                                      /* seg1 unwrapped, seg3 wrapped: both a1 */
        } else {
            const float base = std::floor(a0 * inv) * L;
            if (a0 == a1) {
                x = (a0 != L) ? a0 - base : a0;
            } else {
                a1 -= base;
                const float e = std::fmin(a1, L);
                const float ex = a1 - e;
                const bool wr = ex != 0.f;
                const float n = std::floor(ex * inv);
                const float r = ex - n * L;
                x = wr ? r : e;
            }
        }
        sample_at(rt, rate * x, out);
        if (!looping) time = std::fmin(time, L);
        else {
            const float w = std::fmod(time, L);
            wrapped = w != time;
            time = w;
        }
    }

    void set_time(Runtime& rt, float t) override {
        (void)rt;
        const float L = length;
        t = (extended && looping) ? std::fmod(t, L) : std::fmin(std::fmax(t, 0.f), L);
        time = t;
        remaining = (extended && looping) ? kNeverEnds : L - t;
        phase = L > 1e-4f ? t / L : 0.f;
    }
    void set_phase(Runtime& rt, float p) override {
        (void)rt;
        p = normalise(p);
        time = length * p;
        remaining = (extended && looping) ? kNeverEnds : length - time;
        phase = p;
    }
};

/* -------------------------------------------------------------- phase */

struct PhaseCtrl : Ctrl {
    std::unique_ptr<Ctrl> child;
    const Obj* value = nullptr;     /* +0x40, phase 0..1 */
    const Obj* seconds = nullptr;   /* +0x48, absolute seconds, overrides */

    /* FUN_1405ce3c0 */
    float target(Runtime& rt) {
        const float len = child ? child->scaled_length() : 0.f;
        float v = 0.f;
        bool ok = false;
        if (seconds) v = rt.st.as_float(rt.g, seconds, ok) * kTicksPerSecond;
        else if (value) v = rt.st.as_float(rt.g, value, ok) * len;
        return std::fmin(std::fmax(v, -1.0f), len);
    }
    void copy_from_child() {
        if (!child) return;
        phase = child->phase; time = child->time; remaining = child->remaining;
    }
    void init(Runtime& rt, const Obj* a) {
        asset = a;
        const Obj* ca = rt.g.resolve(a->f(0x8e60f433u), a);
        child = ca ? rt.make(ca) : nullptr;
        value = rt.g.resolve(a->f(0xeb8d73a3u), a);
        seconds = rt.g.resolve(a->f(0xa5180f9bu), a);
        if (child) {
            child->set_time(rt, target(rt));
            length = child->length;
            copy_from_child();
        }
    }
    void update(Runtime& rt, float dt, Pose& out) override {
        if (!child) { out = rt.bind; return; }
        const float tgt = target(rt);
        if (tgt < 0.f) {
            child->update(rt, dt, out);
            wrapped = child->wrapped;
        } else {
            const float d = tgt - child->time;
            if (d >= 0.f) { wrapped = false; child->update(rt, d, out); }
            else { child->set_time(rt, tgt); child->update(rt, 0.f, out); wrapped = true; }
        }
        length = child->length;
        copy_from_child();
    }
    void set_time(Runtime& rt, float t) override {
        if (child) child->set_phase(rt, length > 0.f ? t / length : 0.f);
        copy_from_child();
    }
    void set_phase(Runtime& rt, float p) override {
        if (child) child->set_phase(rt, p);
        copy_from_child();
    }
    void restart(Runtime& rt) override {
        if (child) child->restart(rt);
        copy_from_child();
    }
    /* QI 0x1405ce680: unanswered interfaces go to the child (+0x60) */
    Ctrl* tag_leaf() override { return child ? child->tag_leaf() : nullptr; }
};

/* --------------------------------------------------------- transitions */

constexpr const char* kT_Bezier     = "dc57fd9d-c987-9d4b-026f-b8fa06240f51";
constexpr const char* kT_Chooser    = "89acb7c0-1ead-f10a-4b34-c2b2400f0691";
constexpr const char* kT_CdbChooser = "534025c7-d78e-a2eb-4a58-144714439738";
constexpr const char* kT_CdbWrapper = "ef37bda2-e5a0-6dfa-1c60-91a033859a7d";
constexpr const char* kT_SigPolicy  = "7f33ed8d-73d6-8951-ba5e-f3e83c50ab23";

constexpr const char* kT_BlendCurve = "7a5304da-cb41-c72b-b137-16a8cef71355";
/* The other two blend kinds the 1P chain ships, named in the research repo. Both inherit
 * BlendAsset, so both carry its Duration; see make_blend for what each adds. */
constexpr const char* kT_AccelBlend     = "92dc7c65-6c8a-737d-1140-af6fa57aa5d4";
constexpr const char* kT_TimeScaleBlend = "2fa821f7-d7bd-846a-7ba6-e83c840e1e59";
constexpr float kEps = 1.52587890625e-05f;        /* 0x149317fc0 */

/* BlendCurve weight tables, 21 floats per type at 0x149b7a710 (FUN_1407a19b0):
 * the OUTGOING pose's weight, 1 -> 0. Type 3 (Linear) is 1 - x. */
constexpr float kBlendCurve[3][21] = {
    { 1.f, .996917307f, .987688363f, .972369909f, .95105654f, .923879504f, .891006529f, .852640152f,
      .809017003f, .760405958f, .707106769f, .649448037f, .587785244f, .522498548f, .453990489f,
      .382683426f, .309017003f, .233445361f, .156434461f, .0784590989f, 0.f },
    { 1.f, .973402977f, .960834265f, .942675829f, .916827321f, .880797088f, .832018375f, .768524766f,
      .689974487f, .598687649f, .5f, .401312351f, .310025513f, .231475219f, .16798161f, .119202919f,
      .0831726938f, .0573241748f, .0391657241f, .0265969932f, 0.f },
    { 1.f, .921540916f, .843565524f, .766554654f, .690982997f, .617316544f, .546009481f, .477501422f,
      .412214756f, .350551963f, .292893231f, .239594027f, .190983012f, .147359833f, .108993478f,
      .076120466f, .0489434823f, .0276300795f, .0123116588f, .00308266608f, 0.f } };

/* A cross-fade between an incoming and an outgoing controller (research
 * spec_stateflow 2b-2c). Durations and elapsed are ticks.
 *
 *   BezierBlend (FUN_1407a0880): the samples 0xf6ec5494 are the INCOMING
 *     weight at x = i/N, 1.0 implied at x = 1; the outgoing side is updated
 *     only while the remaining time before this step exceeds eps, and is not
 *     released by the blend.
 *   BlendCurve (FUN_1407a12b0): the curve is the OUTGOING weight; at or below
 *     0.01 the outgoing side is released. Sync scales both sides' steps so
 *     they cover the same fraction of a length lerped by the weight.
 *
 * Both record the pose job FUN_1407a4b70: the Blend kernel FUN_140845cb0
 * with the new pose first and the old pose second, weighted by the old
 * pose's weight - blend(new, old, w_old), pose_blend's operand order. */
struct TransitionCtrl : Ctrl {
    enum Kind { Bezier, Curve } kind = Bezier;
    std::unique_ptr<Ctrl> target;    /* incoming, owned (choosers) */
    Ctrl* in = nullptr;              /* incoming, owned elsewhere (a state-flow node's child) */
    std::unique_ptr<Ctrl> source;    /* outgoing */
    float duration = 0.f, elapsed = 0.f;
    std::vector<float> samples;      /* Bezier */
    int curve_type = 0;              /* BlendCurve 0x19a14537 */
    bool sync = false;               /* BlendCurve 0xd0007236 */

    Ctrl* inc() const { return target ? target.get() : in; }
    float progress(float e) const {
        if (duration > 0.f) return std::fmin(std::fmax(e / duration, 0.f), 1.f);
        return e < 0.f ? 0.f : 1.f;
    }
    static float curve(int type, float x) {
        if (type == 3) return 1.f - x;
        if (type < 0 || type > 2) return 1.f - x;
        const int i = (int)(x * 20.f);
        if (i >= 20) return 0.f;
        const float f = std::fmin(std::fmax(x * 20.f - (float)i, 0.f), 1.f);
        return (1.f - f) * kBlendCurve[type][i] + f * kBlendCurve[type][i + 1];
    }
    float bezier(float x) const {
        const int N = (int)samples.size();
        if (N == 0) return 1.f;
        const float g = x * (float)N;
        const int i = (int)g;
        if (i >= N) return 1.f;
        const float hi = i < N - 1 ? samples[(size_t)i + 1] : 1.f;
        const float f = std::fmin(std::fmax(g - (float)i, 0.f), 1.f);
        return (1.f - f) * samples[(size_t)i] + f * hi;
    }
    float blend_remaining() const { return duration - elapsed; }   /* vtable +0xb8 */
    void copy_timing() {
        Ctrl* c = inc();
        if (!c) return;
        phase = c->phase; time = c->time; length = c->length; remaining = c->remaining;
        phase_range = c->phase_range; looping = c->looping; wrapped = c->wrapped;
    }
    void update(Runtime& rt, float dt, Pose& out) override {
        Ctrl* c = inc();
        if (!c) { out = rt.bind; return; }
        const float rem0 = duration - elapsed;
        elapsed = std::fmin(std::fmax(elapsed + dt, 0.f), std::fmax(duration, 0.f));
        const float x = progress(elapsed);
        if (kind == Bezier) {
            c->update(rt, dt, out);
            if (rem0 > kEps && source) {
                Pose po;
                source->update(rt, dt, po);
                pose_blend(out, po, 1.f - bezier(x), nullptr);
            }
        } else {
            const float w = curve(curve_type, x);
            if (w <= 0.01f || !source) {
                c->update(rt, dt, out);
                source.reset();
            } else {
                Pose po;
                if (!sync) { c->update(rt, dt, out); source->update(rt, dt, po); }
                else {
                    const float Ln = c->length, Lo = source->length;
                    const float L = (Lo - Ln) * w + Ln;
                    const float s = L > kEps ? dt / L : 0.f;
                    c->update(rt, s * Ln, out);
                    source->update(rt, s * Lo, po);
                }
                pose_blend(out, po, w, nullptr);
            }
        }
        copy_timing();
    }
    void set_time(Runtime& rt, float t) override { if (inc()) inc()->set_time(rt, t); copy_timing(); }
    void set_phase(Runtime& rt, float p) override { if (inc()) inc()->set_phase(rt, p); copy_timing(); }
    Ctrl* tag_leaf() override { return inc() ? inc()->tag_leaf() : nullptr; }
};

/* A blend for `asset`, or null when the kind is not read (named in notes).
 * Duration 0xef693827 (ticks); `override_ticks` >= 0 replaces it. */
std::unique_ptr<TransitionCtrl> make_blend(Runtime& rt, const Obj* blend, float override_ticks = -1.f)
{
    if (!blend) return nullptr;
    auto t = std::make_unique<TransitionCtrl>();
    t->asset = blend;
    t->duration = override_ticks >= 0.f ? override_ticks : (float)numv(blend->f(0xef693827u));
    if (blend->type == kT_Bezier) {
        t->kind = TransitionCtrl::Bezier;
        if (const bf6::EbxValue* s = blend->f(0xf6ec5494u))
            for (const auto& it : s->items) t->samples.push_back((float)numv(&it));
    } else if (blend->type == kT_BlendCurve) {
        t->kind = TransitionCtrl::Curve;
        t->curve_type = (int)numv(blend->f(0x19a14537u));
        t->sync = boolv(blend->f(0xd0007236u));
    } else if (blend->type == kT_AccelBlend || blend->type == kT_TimeScaleBlend) {
        /* THE TWO KINDS THAT USED TO SNAP.
         *
         * Named in the research repo: AccelBlendAsset (92dc7c65) and TimeScaleBlendAsset
         * (2fa821f7). Both inherit BlendAsset, so the Duration read above is theirs, and
         * both were falling through to "switched without blending" - a POP wherever the
         * duration is not zero.
         *
         * They are now blended over that duration with the built-in curve. What each adds
         * on top is NOT read, and is named rather than guessed: the accelerating one
         * carries two bools (0xF4A1EABB, 0xF7A7C5DB, both false on the shipped sample),
         * presumably ease-in and ease-out; the time-scaling one carries a dof set
         * (0x90D0D976) and two further references, which is how it would match the
         * target's timing instead of only cross-fading. Both samples in the 1P chain have
         * a duration of 1 tick and 0, so neither difference shows there - but a snap is
         * wrong at any duration, and a curve is right at zero. */
        t->kind = TransitionCtrl::Curve;
        t->curve_type = 3;
        if (blend->type == kT_AccelBlend
            && (boolv(blend->f(0xf4a1eabbu)) || boolv(blend->f(0xf7a7c5dbu))))
            rt.note("accelerating blend easing flags not read (built-in curve used): "
                    + blend->path);
        if (blend->type == kT_TimeScaleBlend && blend->f(0x90d0d976u)
            && blend->f(0x90d0d976u)->kind != bf6::EbxValue::Kind::Null)
            rt.note("time-scaling blend does not scale time yet (cross-faded instead): "
                    + blend->path);
    } else {
        rt.note("blend kind not implemented, switched without blending: " + blend->type + " (" + blend->path + ")");
        return nullptr;
    }
    if (numv(blend->f(0x0935f04au)) != 0.0)
        rt.note("blend rotation term 0x0935f04a (p2) not implemented: " + blend->path);
    return t;
}

/* Build a transition from `blend` (null = hard switch). Returns `to` itself
 * when there is no blend or it is of a kind not yet read. */
std::unique_ptr<Ctrl> make_transition(Runtime& rt, const Obj* blend, std::unique_ptr<Ctrl> to,
                                      std::unique_ptr<Ctrl> from)
{
    if (!blend || !from) return to;
    auto t = make_blend(rt, blend);
    if (!t) return to;
    t->target = std::move(to);
    t->source = std::move(from);
    t->copy_timing();
    return t;
}

/* A transition whose blend has finished collapses to its target. */
void collapse_finished(std::unique_ptr<Ctrl>& c)
{
    for (;;) {
        auto* t = dynamic_cast<TransitionCtrl*>(c.get());
        if (!t || t->blend_remaining() >= 1.52588e-5f) return;
        c = std::move(t->target);
    }
}

/* FUN_1408081b0: keep at most n levels of in-flight transitions. */
std::unique_ptr<Ctrl> collapse_depth(std::unique_ptr<Ctrl> c, int n)
{
    auto* t = dynamic_cast<TransitionCtrl*>(c.get());
    if (!t) return c;
    if (dynamic_cast<TransitionCtrl*>(t->target.get())) t->target = collapse_depth(std::move(t->target), n - 1);
    if (t->blend_remaining() <= 0.f || n == 0) return std::move(t->target);
    if (dynamic_cast<TransitionCtrl*>(t->source.get())) t->source = collapse_depth(std::move(t->source), n - 1);
    return c;
}

/* ------------------------------------------------------------ choosers */

/* SignalChooserPolicy (FUN_1409bdd70). -1 when nothing matches. `ok` false
 * if an input could not be evaluated. */
/* AN ENUM POLICY, which is how the graph selects by stance and the like.
 *
 * EnumerationChooserPolicy holds the enum TYPE (0xb80210ef) and the game state
 * carrying its current value (0x11d9aa8b). Each entry names one enum VALUE asset
 * in 0x9939f339, whose own value is 0xc82b46dd, and the entry whose value matches
 * the state is the one chosen.
 *
 * READING THE DUMP MISLEADS HERE. Every entry's reference prints as the same
 * partition path - animations/common/mm.currentstance.enum - because an import is
 * shown by partition, not by instance. The instances differ, which is why the
 * three entries of 1p.idle.ec line up with the prone, crouch and stand idles.
 *
 * Unimplemented, this fell through to "chooser input not evaluable" and the
 * chooser took a default entry, so the per-stance idle was never selected. */
constexpr const char* kT_EnumPolicy = "b954fb72-96d8-8129-2ba1-e53d3606ab8e";

int enum_choose(Runtime& rt, const Obj* policy, const std::vector<const Obj*>& entries, bool& ok)
{
    ok = false;
    const Obj* gs = rt.g.resolve(policy->f(0x11d9aa8bu), policy);
    if (!gs) return -1;
    bool vok = false;
    const int cur = rt.st.as_int(rt.g, gs, vok);
    if (!vok) return -1;
    ok = true;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i]) continue;
        const Obj* ev = rt.g.resolve(entries[i]->f(0x9939f339u), entries[i]);
        if (!ev) continue;
        if ((int)numv(ev->f(0xc82b46ddu)) == cur) return (int)i;
    }
    return -1;
}

int signal_choose(Runtime& rt, const Obj* policy, const std::vector<const Obj*>& entries, int current, bool& ok)
{
    ok = true;
    if (policy && policy->type == kT_EnumPolicy) return enum_choose(rt, policy, entries, ok);
    if (!policy || policy->type != kT_SigPolicy) { ok = false; return -1; }
    const bool sticky = boolv(policy->f(0x0961c844u));
    if (sticky && current >= 0 && current < (int)entries.size() && entries[(size_t)current]) {
        const Obj* e = entries[(size_t)current];
        const bf6::EbxValue* a = e->f(0xb6eb75d4u);
        const bf6::EbxValue* b = e->f(0xdf9f1fbau);
        const size_t conds = (a ? a->items.size() : 0) + (b ? b->items.size() : 0);
        bool pok = true;
        if (conds > 0 && rt.st.signal_pass(rt.g, e, pok) && pok) return current;
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        bool pok = true;
        const bool pass = rt.st.signal_pass(rt.g, entries[i], pok);
        if (!pok) { ok = false; return -1; }
        if (pass) return (int)i;
    }
    return -1;
}

/* Values buffer for a ContextDatabase, from game states (FUN_1408a9e80 /
 * FUN_1408a9fb0 and its four writers - documented in bf6_core.h). */
bool cdb_values(Runtime& rt, const AntCdbHandle& h, std::vector<uint8_t>& v)
{
    v.assign((size_t)std::max(0, h.info.value_bytes) + 16, 0);
    auto put32 = [&](uint64_t off, uint32_t x) { if (off + 4 <= v.size()) std::memcpy(&v[off], &x, 4); };
    auto putf  = [&](uint64_t off, float x)    { if (off + 4 <= v.size()) std::memcpy(&v[off], &x, 4); };
    bool all_ok = true;
    for (int r = 0; r < (int)h.asset_rows.size(); ++r) {
        const auto& row = h.asset_rows[(size_t)r];
        if (row.descriptor < 0 || row.descriptor >= (int)h.keys.size()) continue;
        bool skip = false;
        for (int k = h.skip_lo; k < h.skip_hi && k < (int)h.keys.size(); ++k)
            if (h.keys[(size_t)k].key_index == r) skip = true;
        if (skip) { rt.note("context database row filled elsewhere (skipped): " + h.name); continue; }
        if (!row.mirror.empty()) rt.note("context database mirror remap not applied: " + h.name);
        const bf6_ant_cdb_key& d = h.keys[(size_t)row.descriptor];
        const Obj* s = row.state.empty() ? nullptr : rt.g.root(row.state);
        bool ok = false;
        switch (d.key_class) {
        case 0: { const bool b = rt.st.as_bool(rt.g, s, ok); if (d.value_offset < v.size()) v[d.value_offset] = b ? 1 : 0; break; }
        case 1: case 2: {
            int32_t i = rt.st.as_int(rt.g, s, ok);
            if (!ok) { const bool b = rt.st.as_bool(rt.g, s, ok); i = b ? 1 : 0; }
            put32(d.value_offset, (uint32_t)i);
            break;
        }
        case 3: {
            /* A bitmap key: a true bitmap state (interface 0x1d52f677) is not
             * implemented; an integer state with the row's flag stores
             * 1 << value (-1 when negative), per FUN_140616450. */
            const int32_t i = rt.st.as_int(rt.g, s, ok);
            if (ok && row.bitflag) put32(d.value_offset, (uint32_t)(i >= 0 ? (1 << (i & 31)) : -1));
            else { ok = false; rt.note("bitmap-state key not packed in " + h.name); }
            break;
        }
        case 4: case 6: {
            const float f = rt.st.as_float(rt.g, s, ok);
            putf(d.value_offset, f); putf(d.second, f);                 /* scalar: both ends */
            break;
        }
        case 5: case 7: {
            const float f = rt.st.as_float(rt.g, s, ok);
            putf(d.value_offset, f);
            if (d.second != 0xFFFFFFFFull) putf(d.second, 0.f);         /* scalar state: weight 0 */
            break;
        }
        default:
            rt.note("context database key class not packed: " + std::to_string(d.key_class) + " in " + h.name);
            ok = false;
        }
        if (!ok) all_ok = false;
    }
    return all_ok;
}

struct Runtime;
/* Look up a ContextDatabase for the current game states. Returns the entry
 * asset path or "" for no match. */
std::string cdb_lookup(Runtime& rt, const Obj* cdb_asset);

/* Generic ChooserController (FUN_1409be160 / FUN_1409be450). */
struct ChooserCtrl : Ctrl {
    std::unique_ptr<Ctrl> child;
    int index = -1;
    bool first = true;
    std::vector<const Obj*> entries;       /* entry objects */
    const Obj* policy = nullptr;
    const Obj* blend = nullptr;
    bool every = false, on_wrap = false, keep_phase = false, restart_on_wrap = false, first_update = false, in_blendout = false;

    void init(Runtime& rt, const Obj* a) {
        asset = a;
        policy = rt.g.resolve(a->f(0xb888be6fu), a);
        blend = rt.g.resolve(a->f(0x9bffb09eu), a);
        if (const bf6::EbxValue* es = a->f(0xc7166a0cu))
            for (const auto& e : es->items) entries.push_back(rt.g.resolve(&e, a));
        every = boolv(a->f(0xa0f5b40bu));
        first_update = boolv(a->f(0xe25a9213u));
        looping = boolv(a->f(0x7e3f6d2eu));
        on_wrap = boolv(a->f(0xade01964u));
        keep_phase = boolv(a->f(0x0d6765feu));
        restart_on_wrap = boolv(a->f(0xeb09a8f7u));
        in_blendout = boolv(a->f(0x7b6c1109u));
        choose(rt, true);
    }
    void copy_timing() {
        if (!child) { time = length = remaining = 0.f; phase_range = 1.f; return; }
        time = child->time; remaining = child->remaining; length = child->length; phase_range = child->phase_range; phase = child->phase;
        if (looping) remaining = kNeverEnds;
    }
    void choose(Runtime& rt, bool force) {
        Ctrl* old = child.get();
        float ph = phase;
        if (!force && child) { if (wrapped) child->set_phase(rt, 0.f); collapse_finished(child); copy_timing(); return; }
        bool ok = true;
        int idx = entries.empty() ? -1 : signal_choose(rt, policy, entries, index, ok);
        if (!ok) rt.note("chooser input not evaluable: " + asset->path);
        if (!entries.empty() && idx == -1) idx = (int)entries.size() - 1;
        if (child) ph = child->phase;
        if (idx >= 0 && idx != index) {
            const Obj* ce = entries[(size_t)idx] ? rt.g.resolve(entries[(size_t)idx]->f(0x81309ef7u), entries[(size_t)idx]) : nullptr;
            if (!ce) { idx = -1; child.reset(); }
            else {
                auto nc = rt.make(ce);
                if (!child) child = std::move(nc);
                else if (!blend) child = std::move(nc);
                else {
                    if (dynamic_cast<TransitionCtrl*>(child.get()))
                        rt.note("chooser switched mid-blend: engine snapshots the old blend (SnapshotController, not read)");
                    child = make_transition(rt, blend, std::move(nc), std::move(child));
                }
            }
            index = idx;
        }
        if (!wrapped) { if (child.get() != old && keep_phase && ph > 0.f && child) child->set_phase(rt, ph); }
        else if (child.get() != old) {
            if (restart_on_wrap) if (auto* t = dynamic_cast<TransitionCtrl*>(child.get())) t->target->set_phase(rt, 0.f);
        } else if (child) child->set_phase(rt, 0.f);
        collapse_finished(child);
        copy_timing();
    }
    void update(Runtime& rt, float dt, Pose& out) override {
        if (!policy) { out = rt.bind; return; }
        wrapped = false;
        bool rechoose_wrap = false;
        if (child && looping && child->remaining <= dt + 1e-4f) {
            wrapped = true;
            Pose pre;
            child->update(rt, child->remaining, pre);
            dt = std::fmax(dt - child->remaining, 0.f);
            rechoose_wrap = on_wrap;
            if (!on_wrap || restart_on_wrap) child->set_phase(rt, 0.f);
            /* THE POSE AT THE END OF THE STEP IS THE POST-WRAP POSE, and that is not an
             * approximation: a step that crosses a loop boundary ends after the wrap, so
             * the pose to show is the one after it. What the engine's join adds is
             * TRAJECTORY accumulation - carrying the root's motion across the wrap so a
             * body keeps travelling instead of snapping back to where the cycle started,
             * the same accumulation FUN_140846480 does for a wrap count.
             *
             * A first-person viewmodel has no trajectory to accumulate: the walker places
             * the skeleton and the animation never moves the root. So this is reported for
             * the third-person case, where it would matter, and is not a defect here. */
            rt.note("loop-wrap trajectory accumulation not read; the pose after the wrap is "
                    "the right one and only root motion would differ (1P has none)");
        }
        choose(rt, rechoose_wrap || every || (first_update && first));
        if (child) child->update(rt, dt, out); else out = rt.bind;
        copy_timing();
        first = false;
    }
    void set_time(Runtime& rt, float t) override { if (child) child->set_time(rt, t); copy_timing(); }
    void set_phase(Runtime& rt, float p) override { if (child) child->set_phase(rt, p); copy_timing(); }
    /* QI 0x1409bde40: to the chosen child (+0x60); a blend answers with its
     * incoming side */
    Ctrl* tag_leaf() override { return child ? child->tag_leaf() : nullptr; }
};

/* ContextDatabaseChooserController (FUN_1408ab830 / FUN_1408abb70). */
struct CdbChooserCtrl : Ctrl {
    std::unique_ptr<Ctrl> child;
    std::string chosen;
    const Obj* cdb = nullptr;
    const Obj* blend = nullptr;
    int depth = 3;
    bool on_enter = false, every = false, on_wrap = false, keep_phase = false;
    void init(Runtime& rt, const Obj* a) {
        asset = a;
        cdb = rt.g.resolve(a->f(0x37ee8be9u), a);
        blend = rt.g.resolve(a->f(0x4ffcff88u), a);
        depth = (int)numv(a->f(0xd67715b1u));
        on_enter = boolv(a->f(0x66fdcb25u));
        every = boolv(a->f(0xa0f5b40bu));
        looping = boolv(a->f(0x7e3f6d2eu));
        on_wrap = boolv(a->f(0xade01964u));
        keep_phase = boolv(a->f(0x0d6765feu));
        choose(rt, true);
    }
    void copy_timing() {
        if (!child) { time = length = remaining = 0.f; phase_range = 1.f; return; }
        time = child->time; remaining = child->remaining; length = child->length; phase_range = child->phase_range; phase = child->phase;
        if (looping) remaining = kNeverEnds;
    }
    void choose(Runtime& rt, bool force) {
        Ctrl* old = child.get();
        float ph = phase;
        if (!force && child) { if (wrapped) child->set_phase(rt, 0.f); collapse_finished(child); copy_timing(); return; }
        const std::string e = cdb_lookup(rt, cdb);
        if (child) ph = child->phase;
        if (!child || e != chosen) {
            chosen = e;
            if (e.empty()) child.reset();
            else {
                const Obj* ea = rt.g.root(e);
                auto nc = rt.make(ea);
                if (!child || !blend) child = std::move(nc);
                else {
                    auto t = make_transition(rt, blend, std::move(nc), std::move(child));
                    child = depth < 0 ? std::move(t) : collapse_depth(std::move(t), depth);
                }
            }
        }
        if (child) {
            if (wrapped) child->set_phase(rt, 0.f);
            else if (child.get() != old && keep_phase && ph > 0.f) child->set_phase(rt, ph);
        }
        collapse_finished(child);
        copy_timing();
    }
    void update(Runtime& rt, float dt, Pose& out) override {
        wrapped = false;
        bool rechoose = false;
        if (child && looping && child->remaining <= dt) {
            wrapped = true;
            Pose pre;
            child->update(rt, child->remaining, pre);
            dt = std::fmax(dt - child->remaining, 0.f);
            rechoose = on_wrap;
            child->set_phase(rt, 0.f);
            /* THE POSE AT THE END OF THE STEP IS THE POST-WRAP POSE, and that is not an
             * approximation: a step that crosses a loop boundary ends after the wrap, so
             * the pose to show is the one after it. What the engine's join adds is
             * TRAJECTORY accumulation - carrying the root's motion across the wrap so a
             * body keeps travelling instead of snapping back to where the cycle started,
             * the same accumulation FUN_140846480 does for a wrap count.
             *
             * A first-person viewmodel has no trajectory to accumulate: the walker places
             * the skeleton and the animation never moves the root. So this is reported for
             * the third-person case, where it would matter, and is not a defect here. */
            rt.note("loop-wrap trajectory accumulation not read; the pose after the wrap is "
                    "the right one and only root motion would differ (1P has none)");
        }
        choose(rt, rechoose || every);
        if (child) child->update(rt, dt, out); else out = rt.bind;
        copy_timing();
    }
    void set_time(Runtime& rt, float t) override { if (child) child->set_time(rt, t); copy_timing(); }
    void set_phase(Runtime& rt, float p) override { if (child) child->set_phase(rt, p); copy_timing(); }
    /* QI 0x1408ab6d0: to the chosen child (+0x60); a blend answers with its
     * incoming side */
    Ctrl* tag_leaf() override { return child ? child->tag_leaf() : nullptr; }
};

std::string cdb_lookup(Runtime& rt, const Obj* cdb_asset)
{
    if (!cdb_asset) return std::string();
    bf6_ant_cdb*& h = rt.cdbs[cdb_asset->path];
    if (!h) {
        char why[512] = {0};
        h = bf6_ant_cdb_open(rt.c, cdb_asset->path.c_str(), why, (int)sizeof(why));
        if (!h) { rt.note(std::string("context database refused: ") + why); return std::string(); }
    }
    const AntCdbHandle* x = reinterpret_cast<const AntCdbHandle*>(h);
    std::vector<uint8_t> v;
    if (!cdb_values(rt, *x, v)) rt.note("context database input not evaluable: " + cdb_asset->path);
    const int pick = bf6_ant_cdb_match(h, v.data(), (int)v.size(), nullptr, 0, nullptr, 0);
    const char* e = pick >= 0 ? bf6_ant_cdb_entry(h, pick) : nullptr;
    static const bool trace = std::getenv("BF6_ANT_TRACE") != nullptr;
    if (trace) {
        std::string hx; for (uint8_t b : v) { char t[4]; std::snprintf(t, sizeof t, "%02x", b); hx += t; }
        std::fprintf(stderr, "ant lookup %s -> %d %s  values %s\n", cdb_asset->path.c_str(), pick,
                     e ? e : "(none)", hx.c_str());
    }
    return e ? std::string(e) : std::string();
}

/* ---------------------------------------------------------- blend masks */

constexpr const char* kT_BlendMaskList = "04045519-ccc7-012b-da52-9d83e2511499";
constexpr const char* kT_CdbAsset      = "6fa5a6cc-ab97-ef80-9a98-59b5bd2edbda";

/* A DofSetAsset's records: (bone name, component 'q'/'t'/...) in record order,
 * from the packed blob in 0xf71b7fd3 (research: dof-names-and-pose-offsets-
 * ship-in-the-ds-payload). */
std::vector<std::pair<std::string, char>> ds_records(const Obj* ds)
{
    std::vector<std::pair<std::string, char>> out;
    if (!ds) return out;
    const bf6::EbxValue* f = ds->f(0xf71b7fd3u);
    if (!f) return out;
    std::vector<uint8_t> b;
    for (const auto& it : f->items) {
        const uint64_t u = it.kind == bf6::EbxValue::Kind::Int ? (uint64_t)it.i : it.u;
        for (int k = 0; k < 8; ++k) b.push_back((uint8_t)(u >> (8 * k)));
    }
    if (b.size() < 32) return out;
    uint32_t h[8];
    std::memcpy(h, b.data(), 32);
    const uint32_t n = h[1] + 1;
    const size_t blob = 32 + (size_t)n * 32;
    for (uint32_t i = 0; i < n && 32 + (size_t)i * 32 + 32 <= b.size(); ++i) {
        uint32_t r[8];
        std::memcpy(r, b.data() + 32 + (size_t)i * 32, 32);
        const size_t no = blob + r[3];
        if (no >= b.size()) { out.emplace_back(std::string(), 0); continue; }
        const std::string s((const char*)b.data() + no, strnlen((const char*)b.data() + no, b.size() - no));
        const size_t dot = s.rfind('.');
        out.emplace_back(dot == std::string::npos ? s : s.substr(0, dot),
                         dot == std::string::npos ? 0 : s[dot + 1]);
    }
    return out;
}

/* IS THERE ACTUALLY A SOURCE IN HERE?
 *
 * A weight-source array is a PARALLEL of the weights: one slot per set or per
 * DOF, and in shipped data every slot is null. Testing the array for non-empty
 * therefore reports "not evaluated" for every mask that carries weights at all,
 * which is all of them. Testing for a non-null ITEM is the question that was
 * meant. */
static bool has_nonnull(const bf6::EbxValue* a)
{
    if (!a) return false;
    for (const auto& i : a->items)
        if (i.kind != bf6::EbxValue::Kind::Null) return true;
    return false;
}

/* FUN_14079af40: build a BlendMaskList's per-DOF weights. A set named by a
 * BlendMaskAsset takes that asset's per-record weights; otherwise a set in the
 * list takes its whole-set weight (missing = 0); every other DOF is 0. Sets
 * are matched by identity - the same DofSetAsset - which is what the engine's
 * hdr[7] comparison identifies. Per-DOF and per-set weight SOURCES (dynamic)
 * are named in notes and their constants used. */
const Mask* build_mask(Runtime& rt, const Obj* list)
{
    if (!list) return nullptr;
    /* NOT A MASK LIST AT ALL, and saying so beats masking everything out.
     *
     * A layer may name its mask through a chooser rather than outright, and the
     * caller is expected to resolve that first. Handed something else - a context
     * database, a wrapper - neither of a mask list's array fields is present, every
     * weight stays 0, and a zero mask contributes NOTHING while looking like a
     * perfectly healthy layer. That silence cost the whole upper body: see
     * layer_mask below. */
    if (list->type != kT_BlendMaskList && !list->f(0x7512e8c8u) && !list->f(0x4c23c8ceu)) {
        rt.note("blend mask reference is not a mask list, so no mask is applied: "
                + list->path);
        return nullptr;
    }
    auto it = rt.masks.find(list->path);
    if (it != rt.masks.end()) return it->second.get();
    auto m = std::make_unique<Mask>();
    m->rot.assign((size_t)rt.bones, 0.f);
    m->tr.assign((size_t)rt.bones, 0.f);
    auto put = [&](const std::pair<std::string, char>& rec, float w) {
        auto b = rt.bone_index.find(rec.first);
        if (b == rt.bone_index.end()) return;
        if (rec.second == 'q') m->rot[(size_t)b->second] = w;
        else if (rec.second == 't') m->tr[(size_t)b->second] = w;
    };
    std::map<std::string, bool> done;
    if (const bf6::EbxValue* ms = list->f(0x7512e8c8u))
        for (const auto& mr : ms->items) {
            const Obj* bm = rt.g.resolve(&mr, list);
            if (!bm) continue;
            const Obj* ds = rt.g.resolve(bm->f(0xe91906a2u), bm);
            if (!ds) continue;
            const auto recs = ds_records(ds);
            const bf6::EbxValue* ws = bm->f(0x8cf424e7u);
            /* ONLY WHEN A SOURCE IS ACTUALLY THERE.
             *
             * This used to fire on the array being non-EMPTY, and the array is a
             * parallel of the weights - one slot per DOF, every slot null. So it
             * fired for every mask that had any weights at all, which is all of
             * them, and said "not evaluated" about something that was not there to
             * evaluate.
             *
             * Measured across all 242 blend masks in the install: 0 carry a
             * non-null per-DOF weight source, and 0 carry a non-null per-set one.
             * The feature is unused in shipped data, so using the constants is not
             * an approximation - it is the whole answer.
             *
             * That mattered: this note was the largest entry in the gap census by a
             * wide margin (153 and 115 reports over 79 and 63 machines), and it is
             * the note that sent an earlier investigation after the blend mask as
             * the cause of 7 mm of hand slide. It was never the cause. A diagnostic
             * that cries wolf on every mask costs more than one that says nothing. */
            if (has_nonnull(bm->f(0x202e54f7u)))
                rt.note("blend mask per-DOF weight sources not evaluated (constants used): " + bm->path);
            for (size_t i = 0; i < recs.size(); ++i)
                put(recs[i], ws && i < ws->items.size() ? (float)numv(&ws->items[i]) : 0.f);
            done[ds->path] = true;
        }
    const bf6::EbxValue* sets = list->f(0x4c23c8ceu);
    const bf6::EbxValue* sw = list->f(0x42c8b257u);
    if (has_nonnull(list->f(0x6d77fca0u)))
        rt.note("blend mask per-set weight sources not evaluated (constants used): " + list->path);
    if (sets) for (size_t j = 0; j < sets->items.size(); ++j) {
        const Obj* ds = rt.g.resolve(&sets->items[j], list);
        if (!ds || done.count(ds->path)) continue;
        const float w = sw && j < sw->items.size() ? (float)numv(&sw->items[j]) : 0.f;
        for (const auto& rec : ds_records(ds)) put(rec, w);
    }
    /* A MASK OF ALL ZEROS IS SILENCE, and it looks exactly like a healthy layer.
     *
     * Every weight staying 0 means none of the mask's bones matched this skeleton: a mask
     * authored against joints this rig does not carry, or a dof set that decoded to
     * nothing. The layer then runs perfectly and contributes nothing, which is the most
     * expensive failure shape in this file - it is what hid the whole upper body until the
     * chooser behind its mask was resolved. Say it once, naming the asset. */
    bool any = false;
    for (float w : m->rot) if (w != 0.f) { any = true; break; }
    if (!any) for (float w : m->tr) if (w != 0.f) { any = true; break; }
    if (!any)
        rt.note("blend mask built with every weight zero, so any layer using it is silent "
                "(its bones are not in this skeleton): " + list->path);
    const Mask* out = m.get();
    rt.masks[list->path] = std::move(m);
    return out;
}

/* ---------------------------------------------------------- super-layers */

constexpr const char* kT_SuperLayers = "2719b155-2f8d-4f8e-f516-4fad3b362bbe";

/* The built-in layer weight curve, 21 floats at 0x149b7a764 (FUN_14071e400). */
constexpr float kLayerCurve[21] = {
    1.0f, .973403f, .960834f, .942676f, .916827f, .880797f, .832018f, .768525f, .689974f, .598688f, .5f,
    .401312f, .310026f, .231475f, .167982f, .119203f, .083173f, .057324f, .039166f, .026597f, 0.0f };
constexpr float kLayerEps = 1.52587890625e-05f;   /* 2^-16, DAT_14931f850 */

/* A controller asset's effective controller: a ContextDatabaseAsset (or a
 * wrapper) used where a controller is expected resolves to its chosen entry
 * for the current game states - how SuperLayers resolves its base and layer
 * assets through the chooser interfaces (FUN_140720470). */
const Obj* resolve_ctrl_asset(Runtime& rt, const Obj* a)
{
    for (int guard = 0; a && guard < 8; ++guard) {
        if (a->type == kT_CdbAsset) {
            const std::string e = cdb_lookup(rt, a);
            a = e.empty() ? nullptr : rt.g.root(e);
            continue;
        }
        if (a->type == kT_CdbWrapper) {
            const std::string e = cdb_lookup(rt, rt.g.resolve(a->f(0x37ee8be9u), a));
            a = e.empty() ? nullptr : rt.g.root(e);
            continue;
        }
        return a;
    }
    return a;
}

/* A LAYER'S BLEND MASK, resolved the way the layer's controller is.
 *
 * Field 0x795e0190 may name a BlendMaskList outright, or it may name a context
 * database that CHOOSES one from the current game states. 1p.gameplay.slc's
 * upper-body layer does the latter: 1p.upperbody.blendmask.cdb is keyed on
 * fb.ads.bool, wep.weaponinspectionactive.bool and inmeleebash.bool, because which
 * bones the upper body owns depends on what it is doing.
 *
 * Passing the CDB straight to build_mask produced a mask of all zeros, and a zero
 * mask contributes nothing. That is why every upper-body action - reload, melee,
 * fire-mode, the abilities - said nothing when driven through the top machine while
 * answering immediately when its own machine was opened directly: measured,
 * wep.reloadstart.bool moves 1p.upperbodyoverride.slc 529 mm and moved
 * 1p.gameplay.slc 0.0. */
/* A MASK CHOSEN BY A BOOL CHOOSER, the third way a layer can name one.
 *
 * .boolbmc assets are type ff6ac7fe: a policy plus signal entries, each entry a
 * SignalEntry (e430c3f8) whose 0x81309ef7 names the mask list to use when its
 * signals pass. 1p.fidgets.boolbmc chooses fingersonly.bml while the sprint is
 * exiting, for instance. Five of them are in the first-person chain.
 *
 * The structure is the one signal_pass already reads for bool choosers, so this is
 * the same walk: first entry whose signals pass wins, and nothing means no mask. */
constexpr const char* kT_BoolMaskChooser = "ff6ac7fe-d00b-21fb-c8d5-d92743ba049b";

const Obj* chosen_mask_asset(Runtime& rt, const Obj* a)
{
    if (!a || a->type != kT_BoolMaskChooser) return a;
    const bf6::EbxValue* es = a->f(0xc7166a0cu);
    if (!es) return nullptr;
    for (const auto& er : es->items) {
        const Obj* e = rt.g.resolve(&er, a);
        bool ok = true;
        if (e && rt.st.signal_pass(rt.g, e, ok) && ok)
            return rt.g.resolve(e->f(0x81309ef7u), e);
    }
    return nullptr;
}

/* A LAYER'S BLEND MASK, resolved every way a layer can name one.
 *
 * Field 0x795e0190 may name a BlendMaskList outright, a context database that
 * CHOOSES one from the current game states, or a bool chooser that picks between
 * two. 1p.gameplay.slc's upper-body layer is the second kind:
 * 1p.upperbody.blendmask.cdb is keyed on fb.ads.bool,
 * wep.weaponinspectionactive.bool and inmeleebash.bool, because which bones the
 * upper body owns depends on what it is doing.
 *
 * Passing the CDB straight to build_mask produced a mask of all zeros, and a zero
 * mask contributes nothing. That is why every upper-body action - reload, melee,
 * fire-mode, the abilities - said nothing when driven through the top machine while
 * answering immediately when its own machine was opened directly: measured,
 * wep.reloadstart.bool moves 1p.upperbodyoverride.slc 529 mm and moved
 * 1p.gameplay.slc 0.0.
 *
 * WHEN IT STILL DOES NOT RESOLVE, the layer is skipped rather than applied. A
 * missing mask means "no mask", and a layer with no mask, weight 1 and blend mode 0
 * replaces the WHOLE body - so treating an unresolved reference as "no mask" does
 * not lose a detail, it wipes the base pose and with it the stance. An all-zero mask
 * is the safe reading: the layer is silent, which is what it was before the
 * reference was understood at all. */
const Mask* layer_mask(Runtime& rt, const Obj* e, bool& resolved)
{
    resolved = true;
    const Obj* ref = rt.g.resolve(e->f(0x795e0190u), e);
    if (!ref) return nullptr;
    const Obj* mk = chosen_mask_asset(rt, resolve_ctrl_asset(rt, ref));
    if (mk) {
        /* WHICH ONE IT CHOSE, when the reference was a chooser rather than a name.
         * A mask decides which bones a layer owns, so "the reload only moved the
         * weapon root" and "the chooser picked onlywep.noik" are the same sentence,
         * and without this the first has to be guessed back to the second. */
        if (mk != ref) rt.note("layer mask chosen: " + ref->path + " -> " + mk->path);
        if (const Mask* m = build_mask(rt, mk)) return m;
    }
    resolved = false;
    return nullptr;
}

struct SuperLayersCtrl : Ctrl {
    struct Inst { std::unique_ptr<Ctrl> ctrl; const Obj* src = nullptr; float w = 0.f, T = 0.f, delay = 0.f; };
    struct Layer {
        const Obj* e = nullptr;
        std::vector<Inst> inst;        /* newest first */
        float r = 0.f, remain = 0.f, W = 1.f, last_w = 1.f;
        bool dir_out = false, prev_trig = false;
        const Mask* mask = nullptr;
        /* A mask this layer NAMES but that could not be built. Distinct from having
         * no mask field at all, which legitimately means the whole body. */
        bool mask_lost = false;
        /* The PoseAssetGS this layer publishes its composed pose into, if any. */
        std::string publish;
    };
    std::unique_ptr<Ctrl> base;
    const Obj* cur_base = nullptr;
    std::vector<Layer> layers;
    int sync = -1;
    bool first = true;

    static float curve(float r) {
        const float t = (1.f - r) * 20.f;
        const int i = (int)std::trunc(t);
        if (i >= 20) return 0.f;
        if (i < 0) return 1.f;
        const float f = std::fmin(std::fmax(t - (float)i, 0.f), 1.f);
        return (1.f - f) * kLayerCurve[i] + f * kLayerCurve[i + 1];
    }
    float srcf(Runtime& rt, const Obj* e, uint32_t src_hash, uint32_t const_hash) {
        if (const Obj* s = rt.g.resolve(e->f(src_hash), e)) { bool ok = false; const float v = rt.st.as_float(rt.g, s, ok); if (ok) return v; }
        return (float)numv(e->f(const_hash));
    }
    bool srcb(Runtime& rt, const Obj* e, uint32_t src_hash, uint32_t const_hash) {
        if (const Obj* s = rt.g.resolve(e->f(src_hash), e)) { bool ok = false; const bool v = rt.st.as_bool(rt.g, s, ok); if (ok) return v; }
        return boolv(e->f(const_hash));
    }

    void init(Runtime& rt, const Obj* a) {
        asset = a;
        sync = (int)numv(a->f(0x1b6ad390u));
        if (const bf6::EbxValue* es = a->f(0xcf111f58u))
            for (const auto& er : es->items) { Layer L; L.e = rt.g.resolve(&er, a); layers.push_back(std::move(L)); }
        refresh_base(rt);
        /* NAME EVERY LAYER, with its blend mode.
         *
         * Asking "which additive layer moves when the player moves" by driving
         * game states does not work while the transition machinery is
         * unimplemented: a sweep of all 259 movement-related states moved
         * 1p.gameplay.slc by nothing at all, because the transitions and
         * additive-capture layers that would react are themselves missing. An
         * inventory answers the question directly instead. */
        for (size_t li = 0; li < layers.size(); ++li) {
            const Obj* e = layers[li].e;
            if (!e) continue;
            const int bm = (int)numv(e->f(0x787a258eu));
            const Obj* lc = rt.g.resolve(e->f(0x81309ef7u), e);
            rt.note("  layer " + std::to_string(li)
                    + (bm == 1 ? " ADDITIVE" : (bm == 4 ? " SUBTRACTIVE" : " blend"))
                    + ": " + (lc ? lc->path : std::string("empty")));
        }
        for (auto& L : layers) if (L.e) {
            if ((int)numv(L.e->f(0x787a258eu)) >= 2 && (int)numv(L.e->f(0x787a258eu)) <= 3)
                rt.note("ModelSpace layer blend (never ships) not implemented: " + a->path);
            /* WHERE THIS LAYER'S POSE IS PUBLISHED, if it publishes one. The target is
             * a PoseAssetGS, a pose held in a game state, so another layer can name it
             * as its controller and use this layer's result. */
            if (const Obj* ps = rt.g.resolve(L.e->f(0x899b420cu), L.e))
                L.publish = ps->path;
        }
    }
    void refresh_base(Runtime& rt) {
        const Obj* b = resolve_ctrl_asset(rt, rt.g.resolve(asset->f(0x046783f8u), asset));
        if (!b || b == cur_base) return;
        auto nc = rt.make(b);
        if (!nc) return;
        if (!base) base = std::move(nc);
        else {
            rt.note("super-layers base crossfade curve not read (linear over +0x78 ticks)");
            auto t = std::make_unique<TransitionCtrl>();
            t->kind = TransitionCtrl::Curve; t->curve_type = 3;
            t->duration = srcf(rt, asset, 0xc2ad7611u, 0x26b662f5u);
            t->target = std::move(nc); t->source = std::move(base);
            base = std::move(t);
        }
        cur_base = b;
    }

    void update(Runtime& rt, float dt, Pose& out) override {
        refresh_base(rt);
        if (base) {
            /* A BASE THAT IS A DELTA IS ADDED, NOT WRITTEN.
             *
             * wep.ads.leftthumb.slc is the case that exposed this: its base is
             * adtv_oma_noanimation_01, a clip that means "no change", and one
             * additive layer masked to the left thumb. Written as a pose that
             * base is all zeros, so the composite replaced the whole body with
             * the origin - measured, Hips at 0 where bind has it at 945 mm,
             * which put the arms and the gun a metre and a half out of view.
             * Added instead, the composite is what it was authored as: the pose
             * underneath plus a thumb. */
            if (base->is_additive()) {
                Pose d;
                base->update(rt, dt, d);
                if (rt.have_parent && rt.parent.q.size() == rt.bind.q.size()) out = rt.parent;
                else {
                    out = rt.bind;
                    std::fill(out.vq.begin(), out.vq.end(), 0);
                    std::fill(out.vt.begin(), out.vt.end(), 0);
                }
                pose_add(out, d, 1.f, nullptr);
            } else {
                base->update(rt, dt, out);
            }
            collapse_finished(base);
        }
        else out = rt.bind;
        for (auto& L : layers) {
            if (!L.e) continue;
            const Obj* e = L.e;
            bool enabled = srcb(rt, e, 0x6e8c0b93u, 0x18daea78u);
            const float weight = srcf(rt, e, 0x71f01f90u, 0x89b14df8u);
            /* A LAYER HELD OFF BY ITS ENABLE, said once, with the state that held it.
             *
             * "The feature does not run" and "its Enable read false" are the same
             * sentence, and without this the first has to be chased back to the second
             * by hand - which is how the sprint pose cost an afternoon. Only reported
             * when the weight says somebody wants the layer, so a layer that is simply
             * idle stays quiet. */
            if (!enabled && weight > 0.f) {
                const Obj* en = rt.g.resolve(e->f(0x6e8c0b93u), e);
                if (en) rt.note("layer held off by its Enable: " + en->path);
            }
            const float tIn = srcf(rt, e, 0x6705f3e0u, 0xa26fe9d9u);
            const float tOut = srcf(rt, e, 0x859432c8u, 0x2312f573u);
            const float tX = srcf(rt, e, 0xb7d0341du, 0xcbfae21cu);
            const int upd = (int)numv(e->f(0xe94c4a4eu));
            if ((upd == 1 || upd == 2) && weight <= 0.f) enabled = false;
            L.last_w = weight;
            const Obj* trig = rt.g.resolve(e->f(0x437be494u), e);
            bool restart = false;
            if (enabled) {
                if (trig) {
                    bool ok = false; const bool v = rt.st.as_bool(rt.g, trig, ok);
                    restart = !L.prev_trig && v;
                    L.prev_trig = v;
                }
                const Obj* ca = resolve_ctrl_asset(rt, rt.g.resolve(e->f(0x81309ef7u), e));
                const bool fresh = (!trig && (L.inst.empty() || L.inst.front().src != ca)) || restart;
                if (ca && fresh) {
                    Inst n;
                    n.ctrl = rt.make(ca);
                    n.src = ca;
                    if (L.inst.empty()) {
                        L.remain = tIn; L.r = first ? 1.f : 0.f; L.W = L.r; L.dir_out = false;
                        bool got = true;
                        L.mask = layer_mask(rt, e, got);
                        L.mask_lost = !got;
                    } else L.inst.front().T = tX;
                    const float off = srcf(rt, e, 0x3f3bf41au, 0xdb29a4dcu);
                    if (off > 0.f && n.ctrl) n.ctrl->set_time(rt, n.ctrl->time + off);
                    n.w = (first || L.inst.empty()) ? 1.f : 0.f;
                    n.T = tX;
                    n.delay = std::fmin(off, 0.f);
                    if (n.ctrl) L.inst.insert(L.inst.begin(), std::move(n));
                }
                /* EVERY FRAME, not once when the layer starts.
                 *
                 * A mask chosen from a context database is chosen by game state, and
                 * the upper body's is keyed on fb.ads.bool among others - so it
                 * changes the moment the player aims. Built once at instance time it
                 * would be frozen at whatever the states were then. build_mask caches
                 * per resolved asset, so asking again costs a map lookup. */
                bool got = true;
                L.mask = layer_mask(rt, e, got);
                L.mask_lost = !got;
            }
            if (L.inst.empty()) continue;
            const bool finished = trig ? (tOut >= L.inst.front().ctrl->remaining)
                                       : (!resolve_ctrl_asset(rt, rt.g.resolve(e->f(0x81309ef7u), e)) && upd == 0);
            if (!enabled || finished) {
                if (!L.dir_out) L.remain = tOut;
                const float k = dt < L.remain ? dt / L.remain : 1.f;
                L.r -= L.r * k; L.remain = std::fmax(L.remain - dt, 0.f); L.dir_out = true;
                if (finished && std::fabs(L.r) <= kLayerEps) { L.inst.clear(); continue; }
            } else {
                if (L.dir_out) L.remain = tX;
                const float k = dt < L.remain ? dt / L.remain : 1.f;
                L.r += (1.f - L.r) * k; L.remain = std::fmax(L.remain - dt, 0.f); L.dir_out = false;
            }
            if (e->f(0x5f5ba288u) && !e->f(0x5f5ba288u)->items.empty())
                rt.note("custom layer blend curve not implemented (built-in S-curve used): " + asset->path);
            L.W = curve(L.r) * weight;
            /* FUN_14071e500: instances of one layer crossfade, newest first. */
            const int mode = (int)numv(e->f(0x787a258eu));
            Pose lp; bool have = false;
            for (size_t k = 0; k < L.inst.size();) {
                Inst& n = L.inst[k];
                const float kk = dt < n.T ? dt / n.T : 1.f;
                if (k == 0) n.w += (1.f - n.w) * kk;
                else { n.w -= n.w * kk; if (std::fabs(n.w) <= 1.1920929e-7f) { L.inst.erase(L.inst.begin() + (long)k); continue; } }
                const float cw = curve(n.w);
                float ddt = dt;
                if (n.delay < 0.f) { n.delay += dt; ddt = std::fmax(n.delay, 0.f); if (n.delay >= 0.f) n.delay = 0.f; }
                Pose p;
                const bool run = upd == 0 || L.last_w > 0.f;
                if (run) n.ctrl->update(rt, ddt, p); else p = rt.bind;
                if (std::fabs(cw) > kLayerEps) {
                    if (!have) { lp = p; have = true; }
                    else if (mode == 0) pose_blend(lp, p, cw, nullptr);
                    else pose_add(lp, p, mode == 4 ? -cw : cw, nullptr);
                }
                ++k;
            }
            /* PUBLISH BEFORE MASKING AND WEIGHTING. What a reader wants is this
             * layer's own result, not the composite it happens to contribute to, so the
             * pose stored is `lp` as the layer produced it. A layer that produced
             * nothing this frame leaves whatever was there. */
            if (!L.publish.empty() && have) rt.pose_states[L.publish] = lp;
            if (!have || std::fabs(L.W) <= kLayerEps) continue;
            /* A NAMED MASK THAT COULD NOT BE BUILT MEANS SILENCE, not the whole body.
             * Applying it unmasked replaces every bone the base posed, and measured
             * that is what wiped the stance out of 1p.gameplay.slc: 581 mm of crouch
             * at 1p.poselayer.slc became 0.0 one level up. */
            if (L.mask_lost) continue;
            if (mode == 0) pose_blend(out, lp, L.W, L.mask);
            else if (mode == 1 || mode == 4) pose_add(out, lp, mode == 4 ? -L.W : L.W, L.mask);
        }
        /* timing from the sync layer's newest instance, else the base */
        Ctrl* src = base.get();
        if (sync >= 0 && sync < (int)layers.size() && !layers[(size_t)sync].inst.empty())
            src = layers[(size_t)sync].inst.front().ctrl.get();
        /* FUN_140720810 tail: +0x18..+0x24 only; the flags word is not copied */
        if (src) { phase = src->phase; time = src->time; length = src->length; remaining = src->remaining; }
        first = false;
    }
    void set_time(Runtime& rt, float t) override { if (base) base->set_time(rt, t); }
    void set_phase(Runtime& rt, float p) override { if (base) base->set_phase(rt, p); }
    /* FUN_140720f80: an interface SuperLayers does not answer itself is
     * forwarded to the sync layer's newest instance (layer asset +0x7c index,
     * only while that layer has instances), else to the base. */
    Ctrl* tag_leaf() override {
        if (sync >= 0 && sync < (int)layers.size() && !layers[(size_t)sync].inst.empty())
            return layers[(size_t)sync].inst.front().ctrl->tag_leaf();
        return base ? base->tag_leaf() : nullptr;
    }
};

/* ---------------------------------------------------------- state flow */

constexpr const char* kT_StateFlow   = "7947dd2a-67a1-5f67-fae7-fcc39ba32551";
constexpr const char* kT_SFNode      = "3725ff4d-d4ae-8a0b-da12-8017ab38da5c";
constexpr const char* kT_BoolGSTag   = "5b52f4b1-7679-5f5a-7f45-2b67ee1c7f84";
/* FloatGameStateTagAsset, named in the research repo (da2980a1). Structurally the SAME
 * tag as the bool one - it inherits GameStateTagAsset, so the state is 0x6B01E323, the
 * value 0x8CF424E7, ResetOnLeave 0xFEAC4B79 and the mode 0xC755B26C - and only the value's
 * type and the mode enum differ. FloatMode is the game's own: Disabled 0, Set 1,
 * Multiply 2, Blend 3, Add 4, Subtract 5. */
constexpr const char* kT_FloatGSTag  = "5c1a1e07-cace-7afd-6783-3c465c98eece";
constexpr float kHuge = 3.4028235e38f;   /* DAT_149319c64, the "never ends" tag window end */

/* A tag set evaluated against a running time (research spec_stateflow 3:
 * DefaultTagCollection, per-tag state FUN_1408cdff0, APPLY FUN_1408ccac0,
 * LEAVE FUN_1408cc210). Node tag sets run on node time with length BIG.
 * BoolGameStateTagAsset is the tag kind implemented (Save FUN_1407e3760,
 * Apply FUN_1407e37d0, Reset FUN_1407e3870); others are named in notes. */
struct TagSet {
    struct T { const Obj* a = nullptr; uint8_t s = 0; int16_t c = 0; bool store = false;
               bool ever = false; bool is_float = false; float fstore = 0.f; };
    std::vector<T> tags;
    float prev = 0.f, now = 0.f, length = kNeverEnds;
    bool jumped = true;          /* Restart (FUN_1408cc170) sets it; the first evaluation sees it */
    bool looped = false;

    void build(Runtime& rt, const Obj* set) {
        std::vector<const Obj*> all;
        tag_set_tags(rt, set, all);
        for (const Obj* a : all) {
            if (a->type == kT_BranchOutPointTag) continue;          /* windows, read by the transition test */
            if (a->type != kT_BoolGSTag && a->type != kT_FloatGSTag) {
                rt.note("tag kind not implemented: " + a->type + " (" + a->path + ")");
                continue;
            }
            T t; t.a = a; t.is_float = a->type == kT_FloatGSTag; tags.push_back(t);
        }
    }
    static float start_of(const Obj* a) {                      /* getter vtbl+0x60 */
        const int f = (int)numv(a->f(0x9becce6eu));
        return (f & 0x80) ? 0.f : (float)numv(a->f(0x46e2e98au));
    }
    /* FUN_1408cdff0 for tag i: s = previous state, returns the new state. */
    uint8_t step(Runtime& rt, T& t) {
        const Obj* a = t.a;
        const int f = (int)numv(a->f(0x9becce6eu));
        const uint8_t s = t.s;
        uint8_t n = 0;
        bool cond = true;
        if (const Obj* cs = rt.g.resolve(a->f(0xe1084d98u), a)) { bool ok = false; cond = rt.st.as_bool(rt.g, cs, ok); }
        if (!cond) { if ((s & 6) == 2) n = 4; return n; }
        if (f & 0x80) { if ((s & 3) == 0) n |= 1; n |= 2; return n; }
        const bool norm = (f & 0x40) != 0;
        const float L = norm ? 1.f : length;
        const float T0 = norm ? (length > 0.f ? now / length : 0.f) : now;
        const float Tp = norm ? (length > 0.f ? prev / length : 0.f) : prev;
        float S;
        if ((f & 7) == 1) { rt.note("tag start mode 1 (collection start time) not implemented: " + a->path); S = 0.f; }
        else if ((f & 7) == 2) return 0;
        else S = start_of(a);
        if (L < S) return 0;
        const float E = ((f & 0x38) == 0x10) ? kHuge : S + (float)numv(a->f(0x7e4474c2u));
        if (E < 0.f) return 0;
        const int d = (int)numv(a->f(0xdcc360f4u));
        auto check = [&]() -> uint8_t {
            const bool near = std::fabs(S - T0) <= kEps && std::fabs(E - S) <= kEps;
            const bool inside = (S <= T0 || ((s & 2) && (f & 0x38) == 0x10)) && T0 <= E;
            if (inside || near) {
                if (!(s & 2) && !(n & 1) && d != 0) { t.c = (int16_t)d; return n; }
                n |= 2; if ((s & 3) == 0) n |= 1;
            } else if ((s & 2) && !(s & 4) && !(n & 2) && !(n & 1)) n |= 4;
            return n;
        };
        if (t.c > 0) { --t.c; if (t.c == 0) { n |= 3; return n; } return n; }
        bool to_check;
        if ((s & 3) != 0) to_check = true;
        else if (jumped) to_check = S < Tp ? true : T0 <= E;
        else if ((f & 7) == 1) to_check = true;
        else if (!looped) to_check = S <= Tp ? true : T0 <= E;
        else if (S <= Tp) to_check = T0 <= E;
        else to_check = false;
        if (to_check) return check();
        if (d != 0) { t.c = (int16_t)d; return n; }            /* window jumped over: begins after the delay */
        n |= 3; return n;
    }
    void save(Runtime& rt, T& t) {
        const int mode = (int)numv(t.a->f(0xc755b26cu));
        if (mode == 0) return;                                  /* Disabled */
        const Obj* gs = rt.g.resolve(t.a->f(0x6b01e323u), t.a);
        bool ok = false;
        if (t.is_float) t.fstore = gs ? rt.st.as_float(rt.g, gs, ok) : 0.f;
        else t.store = gs ? rt.st.as_bool(rt.g, gs, ok) : false;
    }
    void apply(Runtime& rt, T& t) {
        const int mode = (int)numv(t.a->f(0xc755b26cu));
        const Obj* gs = rt.g.resolve(t.a->f(0x6b01e323u), t.a);
        if (!gs) return;
        if (t.is_float) {
            /* FloatMode: Set 1, Multiply 2, Blend 3, Add 4, Subtract 5. Blend needs a
             * factor the asset does not carry, so it is reported rather than invented. */
            const float v = (float)numv(t.a->f(0x8cf424e7u));
            switch (mode) {
            case 1: rt.st.set_float(gs->path, v); break;
            case 2: rt.st.set_float(gs->path, t.fstore * v); break;
            case 4: rt.st.set_float(gs->path, t.fstore + v); break;
            case 5: rt.st.set_float(gs->path, t.fstore - v); break;
            case 3: rt.note("float tag Blend mode carries no factor, left alone: " + t.a->path); break;
            default: break;
            }
            return;
        }
        if (mode == 1) rt.st.set_bool(gs->path, boolv(t.a->f(0x8cf424e7u)));
        else if (mode == 2) rt.st.set_bool(gs->path, !t.store);
    }
    void reset(Runtime& rt, T& t) {
        const int mode = (int)numv(t.a->f(0xc755b26cu));
        const int rs = (int)numv(t.a->f(0xfeac4b79u));
        const Obj* gs = rt.g.resolve(t.a->f(0x6b01e323u), t.a);
        if (mode == 0 || rs == 0 || !gs) return;
        if (rs == 1) rt.st.clear(gs->path);                     /* the state's authored default */
        else if (t.is_float) rt.st.set_float(gs->path, t.fstore);
        else rt.st.set_bool(gs->path, t.store);
    }
    void evaluate(Runtime& rt, float time) {
        prev = now; now = time;
        for (auto& t : tags) t.s = step(rt, t);
        jumped = false;
        /* APPLY: every ENDED tag resets, then BEGAN saves and ACTIVE applies */
        for (auto& t : tags) if (t.s & 4) reset(rt, t);
        for (auto& t : tags) {
            if (t.s & 1) save(rt, t);
            if (t.s & 2) { apply(rt, t); t.ever = true; }
        }
    }
    void leave(Runtime& rt) {
        for (auto& t : tags) {
            if (t.s & 2) reset(rt, t);
            else if (!t.ever && ((int)numv(t.a->f(0x9becce6eu)) & 7) == 2) { save(rt, t); apply(rt, t); }
            t.s = 0;
        }
    }
};

/* The node's child when the node is a transparent/exit node (kind 2): the
 * engine's TransparentController passes the PARENT's pose through. A root
 * state flow has no parent pose, so this outputs an all-invalid pose (a
 * blend into it keeps the other side). */
struct TransparentCtrl : Ctrl {
    /* The parent's pose passes through. With a parent supplied that is literally
     * it; without one, nothing is written, which is what lets a blend leave the
     * slot alone rather than lerp against a value the graph never chose. */
    void update(Runtime& rt, float, Pose& out) override {
        if (rt.have_parent && rt.parent.q.size() == rt.bind.q.size()) { out = rt.parent; return; }
        out = rt.bind;
        std::fill(out.vq.begin(), out.vq.end(), 0);
        std::fill(out.vt.begin(), out.vt.end(), 0);
    }
    void set_time(Runtime&, float t) override { time = t; }
    void set_phase(Runtime&, float p) override { phase = p; }
};

struct StateFlowCtrl;

/* One branch slot of the transition evaluator (SFC+0x138 + n*16): the
 * earliest matching window's tag, its time-until, and whether the branch
 * applies to this state flow. */
struct BranchSlot { const Obj* tag = nullptr; float win = kNeverEnds; int applicable = 2; bool computed = false; };

/* StateFlowNodeController (ctor FUN_14099d680 / FUN_14099d870, init
 * FUN_14099dcd0, update FUN_14099e8f0, enter FUN_1409a0cf0, exit
 * FUN_1409a0bf0) with the transition test that runs on it (FUN_14099f0f0 ->
 * FUN_140999ab0 / FUN_140999d00 / FUN_14099a0a0 / FUN_14099aef0 /
 * FUN_14099a7a0). */
struct NodeCtrl : Ctrl {
    struct Entry {
        const Obj* to = nullptr;          /* 0x88b009f9 */
        const Obj* tr = nullptr;          /* 0x7159d693 TransitionAsset */
        std::vector<int> must_true;       /* 0x00e4aefc */
        std::vector<int> must_false;      /* 0x9336b9d9 */
        bool gate_list = false;           /* 0xf7ac97ae */
        int gate = 0xff, gate_next = 0xff, cont = 0xff, next = 0xff, timing = 0;
    };
    StateFlowCtrl* sf = nullptr;
    int kind = 0;                         /* 0xcaad5086 */
    std::unique_ptr<Ctrl> child;
    std::unique_ptr<TransitionCtrl> fade; /* node+0x98: in = child, source = the old node */
    TagSet tags;                          /* 0x70b22fd5, on node time */
    std::unique_ptr<TagSet> fade_tags;    /* the transition's tag set, live with the fade */
    float node_time = 0.f;                /* +0xa8 */
    bool first_pending = true;            /* +0xb2 */
    bool active = false;                  /* +0xb3 / +0xb4 */
    std::vector<Entry> entries;           /* 0xfa9c5e77 */
    std::vector<const Obj*> watched;      /* 0x438ebff1 */
    std::vector<const Obj*> branches;     /* 0x8e477845 */
    std::vector<uint8_t> entry_state;     /* SFC+0xe20 per entry, zeroed on enter */
    /* result of the last test */
    int chosen = 0xff;
    const Obj* out_tag = nullptr;         /* the BranchOutPointTag whose window opened it */

    void init(Runtime& rt, StateFlowCtrl* owner, const Obj* a);
    void enter(Runtime& rt);
    void exit(Runtime& rt);
    /* Whether one entry's own conditions hold right now. The pool it indexes is
     * `watched`, the node's 0x438ebff1. */
    bool entry_conds(Runtime& rt, const Entry& e) const;
    /* The first entry in a child chain whose conditions hold, 0xff for none. A child
     * list is a CHOICE - which reload, which ability, which field upgrade - and the
     * last child usually carries no conditions and is the group's default. */
    int matching_child(Runtime& rt, int start) const;
    float time_to_transition(Runtime& rt, float dt);
    bool timing(Runtime& rt, int i, Ctrl* leaf, float tNow, float rem, float len, float dt,
                std::vector<float>& window, BranchSlot* slots);
    void copy_timing() {
        if (!child) return;
        phase = child->phase; time = child->time; length = child->length; remaining = child->remaining;
        phase_range = child->phase_range; looping = child->looping; wrapped = child->wrapped;
    }
    void update(Runtime& rt, float dt, Pose& out) override;
    void set_time(Runtime& rt, float t) override { if (child) child->set_time(rt, t); copy_timing(); }
    void set_phase(Runtime& rt, float p) override { if (child) child->set_phase(rt, p); copy_timing(); }
    /* QI 0x14099e860: unanswered interfaces go to the node's child (+0x70) */
    Ctrl* tag_leaf() override { return child ? child->tag_leaf() : nullptr; }
};


struct StateFlowCtrl : Ctrl {
    std::unique_ptr<NodeCtrl> node;
    const Obj* default_blend = nullptr;   /* 0x7c8aa9d8 */
    int depth = 3;                        /* 0xc15edaf2 */
    uint32_t id = 0;                      /* 0xde6f63b3 */
    bool stopped = false;                 /* +0xa1 */
    bool start_pending = false;           /* +0xa8 */
    bool evaluates = true;                /* +0xa6: false inside a fading-out subtree */
    bool finished = false;                /* reported to the parent: current node kind 2, no fade */
    /* Whether this machine IS the root. Nodes are also built when a transition
     * fires, long after make() has returned, so the depth has to be captured
     * here rather than read when a node is created. */
    bool is_root = false;

    void init(Runtime& rt, const Obj* a) {
        asset = a;
        is_root = rt.make_depth <= 1;
        default_blend = rt.g.resolve(a->f(0x7c8aa9d8u), a);
        depth = (int)numv(a->f(0xc15edaf2u));
        id = (uint32_t)numv(a->f(0xde6f63b3u));
        const bf6::EbxValue* mi = a->f(0x0360f8dau);
        if (mi && mi->kind != bf6::EbxValue::Kind::Null) rt.note("state-flow mirror input not implemented: " + a->path);
        const bf6::EbxValue* mod = a->f(0x857c9b4du);
        if (mod && mod->kind != bf6::EbxValue::Kind::Null) rt.note("state-flow modifier not implemented: " + a->path);
        const bf6::EbxValue* rs = a->f(0x8e44821au);
        if (rs && !rs->items.empty()) rt.note("state-flow resume table not implemented: " + a->path);
        if (boolv(a->f(0x8089c8e1u))) rt.note("state-flow gate/PTM manager not implemented: " + a->path);
        restart(rt);
    }
    /* FUN_14099c300 */
    void restart(Runtime& rt) override {
        if (node) node->exit(rt);
        const Obj* start = rt.g.resolve(asset->f(0x5fa260eeu), asset);
        if (!start) { stopped = true; node.reset(); return; }
        stopped = false;
        node = std::make_unique<NodeCtrl>();
        node->init(rt, this, start);
        start_pending = true;
        copy_timing();
    }
    void copy_timing() {
        if (!node) return;
        phase = node->phase; time = node->time; length = node->length; remaining = node->remaining;
        phase_range = node->phase_range; looping = node->looping; wrapped = node->wrapped;
        const bf6::EbxValue* ts = node->asset ? node->asset->f(0x1750ec21u) : nullptr;
        if (ts && !ts->items.empty()) remaining = kNeverEnds;
    }
    void fire(Runtime& rt);
    void update(Runtime& rt, float dt, Pose& out) override {
        if (start_pending && !stopped) {
            start_pending = false;
            if (node && node->kind == 1) {
                const float t = node->time_to_transition(rt, 0.f);
                if (t > kEps) stopped = true; else fire(rt);
            }
        }
        if (stopped || !node) {
            rt.note("stopped state flow outputs the reference pose (FUN_1407a7e20, meaning OPEN): " + asset->path);
            out = rt.bind;
            return;
        }
        int fires = 0;
        while (dt >= 0.f) {
            const float t = node->time_to_transition(rt, dt);
            if (dt <= t) break;
            if (t > 0.f && std::fabs(t) > kEps) { Pose tmp; node->update(rt, t, tmp); dt -= t; }
            fire(rt);
            if (++fires >= 26) {
                rt.note("Suspiciously high number of transitions per frame (engine stops the state flow): " + asset->path);
                stopped = true;
                break;
            }
        }
        node->update(rt, dt, out);
        copy_timing();
        finished = node->kind == 2 && !node->fade;
    }
    void set_time(Runtime& rt, float t) override { if (node) node->set_time(rt, t); copy_timing(); }
    void set_phase(Runtime& rt, float p) override { if (node) node->set_phase(rt, p); copy_timing(); }
    /* QI 0x14099b600: unanswered interfaces go to the current node (+0x78) */
    Ctrl* tag_leaf() override { return node ? node->tag_leaf() : nullptr; }
};

void NodeCtrl::init(Runtime& rt, StateFlowCtrl* owner, const Obj* a)
{
    asset = a; sf = owner;
    kind = (int)numv(a->f(0xcaad5086u));
    if (const bf6::EbxValue* ws = a->f(0x438ebff1u)) for (const auto& w : ws->items) watched.push_back(rt.g.resolve(&w, a));
    if (const bf6::EbxValue* bs = a->f(0x8e477845u)) for (const auto& b : bs->items) branches.push_back(rt.g.resolve(&b, a));
    if (const bf6::EbxValue* cs = a->f(0xfa9c5e77u))
        for (const auto& c : cs->items) {
            Entry e;
            e.to = rt.g.resolve(c.field(0x88b009f9u), a);
            e.tr = rt.g.resolve(c.field(0x7159d693u), a);
            if (const bf6::EbxValue* v = c.field(0x00e4aefcu)) for (const auto& x : v->items) e.must_true.push_back((int)numv(&x));
            if (const bf6::EbxValue* v = c.field(0x9336b9d9u)) for (const auto& x : v->items) e.must_false.push_back((int)numv(&x));
            e.gate_list = boolv(c.field(0xf7ac97aeu));
            e.gate = (int)numv(c.field(0xafddb84du));
            e.gate_next = (int)numv(c.field(0xb4bd1b16u));
            e.cont = (int)numv(c.field(0xabe772afu));
            e.next = (int)numv(c.field(0x6b9ac316u));
            e.timing = (int)numv(c.field(0xc09fdc38u));
            entries.push_back(e);
        }
    const bf6::EbxValue* mod = a->f(0x8f198e8eu);
    if (mod && mod->kind != bf6::EbxValue::Kind::Null) rt.note("state-flow node modifier not implemented: " + a->path);
    const bf6::EbxValue* gi = a->f(0x8e36d24au);
    if (gi && gi->kind != bf6::EbxValue::Kind::Null) rt.note("state-flow gate transitions info not implemented: " + a->path);
    /* FUN_14099dcd0: time 0, +0xb2 set, child created (FUN_1409a1180) */
    node_time = 0.f; first_pending = true; active = false;
    if (kind == 2) {
        child = std::make_unique<TransparentCtrl>();
        /* Nested, this is already right: the node writes nothing, and a blend
         * leaves a slot its source never wrote alone, so the parent's pose is
         * what comes out. Only a machine run AS the root has no parent pose. */
        if (sf && sf->is_root)
            rt.note("transparent node in a machine run AS a root, so there is no parent pose "
                    "to pass through and it shows bind; nested, the pass-through is correct: "
                    + a->path);
    } else if (const Obj* ca = rt.g.resolve(a->f(0x0171c26du), a)) {
        child = rt.make(ca);
    }
    if (!child) { auto m = std::make_unique<TransparentCtrl>(); child = std::move(m); }
    tags.build(rt, rt.g.resolve(a->f(0x70b22fd5u), a));
    copy_timing();
    if (owner && owner->evaluates) enter(rt);
}

/* FUN_1409a0cf0 */
void NodeCtrl::enter(Runtime& rt)
{
    active = true;
    const bf6::EbxValue* set = asset->f(0xb80c39a4u);
    const bf6::EbxValue* cnt = asset->f(0x4a482187u);
    const size_t n = cnt ? cnt->items.size() : 0;
    for (size_t i = 0; set && i < n && i < set->items.size(); ++i)
        if (const Obj* b = rt.g.resolve(&set->items[i], asset)) rt.st.set_bool(b->path, true);
    entry_state.assign(entries.size(), 0);
    if (kind == 3) rt.note("gate (PTM) node registration not implemented: " + asset->path);
}

/* FUN_1409a0bf0 */
void NodeCtrl::exit(Runtime& rt)
{
    if (!active) return;
    active = false;
    const bf6::EbxValue* set = asset->f(0xb80c39a4u);
    const bf6::EbxValue* cnt = asset->f(0x4a482187u);
    const size_t n = cnt ? cnt->items.size() : 0;
    for (size_t i = 0; set && i < n && i < set->items.size(); ++i)
        if (const Obj* b = rt.g.resolve(&set->items[i], asset)) rt.st.set_bool(b->path, false);
    /* LEAVE (FUN_1408cc210): its call site on node exit is not traced
     * (spec OPEN 1); the node's tags are left here, when the node stops
     * evaluating them. */
    tags.leave(rt);
    if (auto* nested = dynamic_cast<StateFlowCtrl*>(child.get())) { if (nested->node) nested->node->exit(rt); }
}

/* FUN_14099a7a0: the time until entry i may fire, into window[i]; true when
 * it fires inside this frame's dt. */
bool NodeCtrl::timing(Runtime& rt, int i, Ctrl* leaf, float tNow, float rem, float len, float dt,
                      std::vector<float>& window, BranchSlot* slots)
{
    const Entry& e = entries[(size_t)i];
    const int m = e.timing;
    if (m == 1) {
        window[(size_t)i] = rem == kNeverEnds ? kNeverEnds : rem - (float)(int32_t)numv(e.tr ? e.tr->f(0xa27bf0d8u) : nullptr);
    } else if (m == 2) {
        window[(size_t)i] = first_pending ? kNeverEnds : 0.f;
    } else if (m >= 0 && m < (int)branches.size() + 1 && m < 64) {
        BranchSlot& bs = slots[m];
        if (!bs.computed) {
            bs.computed = true;
            if (bs.applicable == 2) {
                const Obj* br = m < (int)branches.size() ? branches[(size_t)m] : nullptr;
                if (!br) { bs.applicable = 0; rt.note("branch slot with no branch asset: " + asset->path); }
                else {
                    const int32_t bid = (int32_t)numv(br->f(0x4b3c2c4bu));
                    const bool any = boolv(br->f(0xe666946bu));
                    const bool nested = dynamic_cast<StateFlowCtrl*>(child.get()) != nullptr;
                    bs.applicable = (!any && bid != (int32_t)sf->id && (bid != 0 || nested)) ? 0 : 1;
                }
            }
            if (bs.applicable == 1) {
                float best = kNeverEnds;
                const Obj* best_tag = nullptr;
                const std::vector<const Obj*>* bt = leaf ? leaf->branch_tags() : nullptr;
                if (!leaf) rt.note("branch window needs the child's tag query (0x317458a7), not answered: " + asset->path);
                if (bt) for (const Obj* tg : *bt) {
                    const Obj* tb = rt.g.resolve(tg->f(0xd2cd9e3fu), tg);
                    if (!tb || m >= (int)branches.size() || tb != branches[(size_t)m]) continue;
                    if (entry_state[(size_t)i] == 0) entry_state[(size_t)i] = 1;
                    const int f = (int)numv(tg->f(0x9becce6eu));
                    if (numv(tg->f(0x571974adu)) != 0.0 || numv(tg->f(0x9dacc63bu)) != 0.0 || numv(tg->f(0x4f9d1a6cu)) != 0.0)
                        rt.note("branch-tag window adjustments (weighted by the branch value) not applied: " + tg->path);
                    if (wrapped) { rt.note("branch window on a wrapped child (node +0xa4 path) not implemented: " + asset->path); continue; }
                    float S = TagSet::start_of(tg), W = (float)numv(tg->f(0x7e4474c2u));
                    if (f & 0x40) { S *= len; W *= len; }
                    float w;
                    const float d = S - tNow;
                    if (d <= 0.f) w = ((W + S) - tNow < 0.f) ? kNeverEnds : 0.f;
                    else w = d;
                    if (w < best) { best = w; best_tag = tg; }
                }
                bs.win = best; bs.tag = best_tag;
            }
        }
        window[(size_t)i] = bs.win;
    } else {
        rt.note("transition timing index outside the branch list: " + asset->path);
        window[(size_t)i] = kNeverEnds;
    }
    const float w = window[(size_t)i];
    return w < kNeverEnds && w <= dt;
}

/* FUN_14099f0f0 -> FUN_140999ab0 -> FUN_140999d00 (top level): the time
 * until the first transition fires, kNeverEnds for none; `chosen` and
 * `out_tag` describe it. */
float NodeCtrl::time_to_transition(Runtime& rt, float dt)
{
    chosen = 0xff; out_tag = nullptr;
    if (!sf || !(sf->evaluates || kind == 1) || sf->stopped || entries.empty()) return kNeverEnds;
    Ctrl* leaf = child ? child->tag_leaf() : nullptr;
    const Ctrl* tb = leaf ? leaf : static_cast<const Ctrl*>(this);
    const float tNow = tb->time;
    float rem = remaining;
    if (rem != kNeverEnds) rem = tb->remaining;
    const float len = tb->length;
    std::vector<int8_t> cond(watched.size(), 2);
    std::vector<float> window(entries.size(), kNeverEnds);
    BranchSlot slots[64];
    slots[0].win = 0.f; slots[0].computed = true;            /* SFC+0x228 = 0, +0x22d = 1 */
    auto conds = [&](const std::vector<int>& idx, bool want) {
        for (int k : idx) {
            if (k < 0 || k >= (int)cond.size()) return false;
            if (cond[(size_t)k] == 2) {
                bool ok = false;
                const bool v = rt.st.as_bool(rt.g, watched[(size_t)k], ok);
                cond[(size_t)k] = v ? 1 : 0;
            }
            if ((cond[(size_t)k] != 0) != want) return false;
        }
        return true;
    };
    /* A CHILD LIST IS A CHOICE, AND AN EMPTY ONE IS NOT A YES.
     *
     * Transitions form a tree, not a list: `next` (0x6b9ac316) is the sibling to try
     * and `cont` (0xabe772af) a child list to choose from. A parent with a child list
     * and no conditions of its own is a GROUP HEADER, and the group is entered only
     * if one of its children matches. 1p.ubo.default.node has three of them:
     *
     *   entry  0  1p.ubo.abilitiesenter.node   no conditions, children 1..18
     *   entry 23  1p.ubo.reloadenter.node      wep.reloadstart.bool, children 24..32
     *   entry 43  1p.ubo.fieldupgrade.enter    no conditions, children 44..46
     *
     * Treating "no conditions" as a pass fired entry 0 or entry 43 on the first frame
     * and parked the upper body in a field-upgrade or an ability from which the
     * reload transition does not exist - measured, the machine sat in
     * 1p.ubo.fieldupgrade.enter.node and wep.reloadstart.bool did nothing at all.
     *
     * A child with no conditions is the group's DEFAULT and does pass, which is what
     * entry 32 is for: once reloadstart is true, one of the reload variants must be
     * chosen, and the last one takes it if none of the specific ones match. That also
     * keeps default_node5 working, where entry 1's child 2 carries no conditions. */
    auto child_matches = [&](int start, auto&& self) -> bool {
        for (int j = start, guard = 0;
             j != 0xff && j >= 0 && j < (int)entries.size() && guard <= (int)entries.size();
             j = entries[(size_t)j].next, ++guard) {
            const Entry& c = entries[(size_t)j];
            if (!(conds(c.must_true, true) && conds(c.must_false, false))) continue;
            if (c.cont == 0xff) return true;
            if (self(c.cont, self)) return true;
        }
        return false;
    };
    float best = kNeverEnds;
    int result = 0xff;
    for (int i = 0; i < (int)entries.size() && i != 0xff; i = entries[(size_t)i].next) {
        const Entry& e = entries[(size_t)i];
        {
            static const char* why_node = std::getenv("BF6_ANT_WHY_NODE");
            if (why_node && *why_node) (void)entry_conds(rt, e);   /* prints when the node matches */
        }
        if (entry_state.size() != entries.size()) entry_state.assign(entries.size(), 0);
        bool pass = true;
        if (entry_state[(size_t)i] != 4) pass = conds(e.must_true, true) && conds(e.must_false, false);
        /* Its own conditions are not enough when it heads a choice: see child_matches. */
        if (pass && e.cont != 0xff && !child_matches(e.cont, child_matches)) pass = false;
        int r = 0xff;
        if (pass) {
            /* A CONTINUATION IS NOT A REFUSAL, which is what this used to treat it
             * as. `cont` names another entry to carry on to because THIS entry's
             * target is a pass-through node with no pose of its own; the transition
             * itself is perfectly valid and its blend comes from here. Resolving
             * where it lands is fire()'s job, so selection no longer stops at it.
             *
             * Read out of .1p.soldier.top.sf's default_node5, whose entry 1 is:
             *
             *   to = animations/alive.node   tr = transition_default_node_to_alive
             *   must_true = []  must_false = []  gate = 255  cont = 2  next = 3
             *
             * and entry 2, which the `next` chain SKIPS (0 -> 1 -> 3 -> 4):
             *
             *   to = animations/loco.node    tr = null   cont = 255  next = 255
             *
             * So alive.node is passed through and loco.node is the real
             * destination. That it is loco.node is corroborated from the other
             * side: 1p.fullbodyactions.sf already rests there. Refusing this entry
             * left the whole soldier graph parked in default_node5 forever, and
             * with it the 68 machines that wait on a transition. */
            if (e.gate != 0xff) rt.note("gated transition (gate manager) not implemented: " + asset->path);
            else { timing(rt, i, leaf, tNow, rem, len, dt, window, slots); r = i; }
        }
        if (r != 0xff && window[(size_t)i] < best) {
            best = window[(size_t)i]; result = r;
            if (std::fabs(best) <= kEps) break;
        }
        if (e.next == 0xff || e.next >= (int)entries.size()) break;
    }
    if (result == 0xff) return kNeverEnds;
    chosen = result;
    const int m = entries[(size_t)result].timing;
    if (m > 2 && m < 64) out_tag = slots[m].tag;
    return best;
}

/* FUN_14099e8f0 */
void NodeCtrl::update(Runtime& rt, float dt, Pose& out)
{
    first_pending = false;
    node_time += dt;
    if (!fade) {
        child->update(rt, dt, out);
    } else {
        const float r = fade->blend_remaining();
        if (dt < r) {
            fade->update(rt, dt, out);
            if (fade_tags && active) fade_tags->evaluate(rt, node_time);
        } else {
            fade->update(rt, r, out);
            if (fade_tags) {
                if (active) fade_tags->evaluate(rt, node_time - (dt - r));
                /* dropped with the blend; LEAVE's call site is not traced (spec OPEN 1) */
                fade_tags->leave(rt);
                fade_tags.reset();
            }
            fade.reset();                                       /* releases the old node chain (FUN_14099f020) */
            if (dt - r >= kEps) child->update(rt, dt - r, out);
        }
    }
    copy_timing();
    if (active) tags.evaluate(rt, node_time);
}

/* FUN_14099f400 + FUN_14099d870 + FUN_1409a04e0 */
bool NodeCtrl::entry_conds(Runtime& rt, const Entry& e) const
{
    auto all = [&](const std::vector<int>& idx, bool want) {
        for (int k : idx) {
            if (k < 0 || k >= (int)watched.size()) return false;
            bool ok = false;
            if (rt.st.as_bool(rt.g, watched[(size_t)k], ok) != want) return false;
        }
        return true;
    };
    /* BF6_ANT_WHY_NODE=<path substring>: for a matching node, each exit tested, where it
     * leads, and every condition with the value read now - which state holds a machine
     * in its pass-through node when the layer around it is already enabled. */
    static const char* why = std::getenv("BF6_ANT_WHY_NODE");
    if (why && *why && asset && asset->path.find(why) != std::string::npos) {
        std::string line = "node " + asset->path + " -> " + (e.to ? e.to->path : std::string("(none)"));
        auto dump = [&](const std::vector<int>& idx, const char* need) {
            for (int k : idx) {
                if (k < 0 || k >= (int)watched.size()) { line += std::string("  ") + need + " (bad index)"; continue; }
                bool ok = false;
                const bool v = rt.st.as_bool(rt.g, watched[(size_t)k], ok);
                line += std::string("  ") + need + " " + (watched[(size_t)k] ? watched[(size_t)k]->path : "null")
                        + "=" + (ok ? (v ? "1" : "0") : "?");
            }
        };
        dump(e.must_true, "T");
        dump(e.must_false, "F");
        std::fprintf(stderr, "%s\n", line.c_str());
    }
    return all(e.must_true, true) && all(e.must_false, false);
}

int NodeCtrl::matching_child(Runtime& rt, int start) const
{
    for (int j = start, guard = 0;
         j != 0xff && j >= 0 && j < (int)entries.size() && guard <= (int)entries.size();
         j = entries[(size_t)j].next, ++guard)
        if (entry_conds(rt, entries[(size_t)j])) return j;
    return 0xff;
}

void StateFlowCtrl::fire(Runtime& rt)
{
    if (!node || node->chosen == 0xff) return;
    NodeCtrl::Entry e = node->entries[(size_t)node->chosen];
    const Obj* T = e.tr;
    /* FOLLOW THE CONTINUATION to the node that actually has a pose.
     *
     * An entry whose `cont` is set points at a pass-through node; the entry it
     * names carries the real destination, and typically no transition asset of its
     * own because the blend belongs to the first entry. Walk to the end of that
     * chain and land there.
     *
     * BOUNDED BY THE ENTRY COUNT, not by trust: `cont` is an index out of authored
     * data and a cycle would hang the update. Each step must also move forward
     * into a real entry, so a self-reference stops rather than spins.
     *
     * A CONTINUATION IS A CHOICE WHEN ITS CHILDREN CARRY CONDITIONS, and stopping at
     * the first conditioned child - which this used to do - lands on the pass-through
     * and poses nothing. The reload is the case that showed it:
     *
     *   1p.ubo.default.node entry 23  -> 1p.ubo.reloadenter.node  on wep.reloadstart
     *                       children 24..32 -> 1p.ubo.reload.node, one per variant
     *
     * Nine children, eight with a condition and the last with none. reloadenter has
     * no controller and no transitions of its own, so landing there is landing on
     * nothing: measured, the machine transitioned correctly and the pose moved 0.0 mm
     * while the selector flick beside it moved 708.9. Choosing the matching child -
     * the default when no specific one matches - lands on the node that has the
     * reload. default_node5 still works: its entry 2 carries no conditions, so it is
     * the child that matches. */
    {
        int at = node->chosen;
        for (size_t hop = 0; hop < node->entries.size(); ++hop) {
            const int nx = node->entries[(size_t)at].cont;
            if (nx == 0xff || nx < 0 || nx >= (int)node->entries.size() || nx == at) break;
            const int pick = node->matching_child(rt, nx);
            if (pick == 0xff || pick == at) break;
            at = pick;
            if (node->entries[(size_t)at].to) e.to = node->entries[(size_t)at].to;
        }
    }
    const Obj* to = e.to ? e.to : (T ? rt.g.resolve(T->f(0x7decae88u), T) : nullptr);
    if (!to) { rt.note("transition with no target node: " + node->asset->path); node->chosen = 0xff; return; }
    /* CONSUME THE TRIGGERS THIS TRANSITION FIRED ON.
     *
     * Each record in 0xa80bcb57 names a game state and carries a bool, and taking
     * the transition writes that bool to that state. Without it a trigger stays
     * where it was and the transition re-fires on the next update forever, which is
     * what "consumption" means: the trigger is spent by being taken.
     *
     * READ OUT OF THE DATA, not assumed. A record has five fields - four object
     * references and one bool - and across 60 transitions that carry one, all four
     * references name the SAME state every time (0 of 60 differ), so their separate
     * roles cannot be told apart and do not need to be: there is one state per
     * record. The bool is NOT a constant reset; it is true in 39 of 60 and false in
     * 21, so it is the value to leave behind rather than "set to false".
     *
     * The write goes through the same rt.st.set_bool that the state-flow tags use
     * (mode 1 there writes an authored bool to a resolved state), so this is the
     * established path in this file and not a new one. */
    if (T) {
        if (const bf6::EbxValue* c = T->f(0xa80bcb57u)) {
            for (const auto& rec : c->items) {
                const Obj* gs = rt.g.resolve(rec.field(0xcb2eaef9u), T);
                if (!gs) continue;
                rt.st.set_bool(gs->path, boolv(rec.field(0x4380d37eu)));
            }
        }
    }
    /* blend selection, FUN_1409998c0 */
    const Obj* blend = T ? rt.g.resolve(T->f(0x4ffcff88u), T) : nullptr;
    if (!blend && node->out_tag) blend = rt.g.resolve(node->out_tag->f(0x9bffb09eu), node->out_tag);
    if (!blend) blend = default_blend;
    const Obj* out_tag = node->out_tag;

    std::unique_ptr<NodeCtrl> old = std::move(node);
    old->exit(rt);
    auto nn = std::make_unique<NodeCtrl>();
    nn->init(rt, this, to);
    /* the cross-fade, FUN_1409a14a0 */
    std::unique_ptr<TransitionCtrl> f;
    if (blend) f = make_blend(rt, blend);
    else {
        f = std::make_unique<TransitionCtrl>();       /* no blend asset: BlendCurve type 0, 8 ticks */
        f->kind = TransitionCtrl::Curve; f->curve_type = 0; f->duration = 8.f;
    }
    const float src_phase = old->phase;
    const bool old_loops = old->looping;
    if (f) {
        f->in = nn->child.get();
        f->source = std::move(old);
        f->copy_timing();
        nn->fade = std::move(f);
        if (T) if (const Obj* ts = rt.g.resolve(T->f(0x70b22fd5u), T)) {
            nn->fade_tags = std::make_unique<TagSet>();
            nn->fade_tags->build(rt, ts);
        }
        /* FUN_1409a0a10: keep at most `depth` nested in-flight fades */
        NodeCtrl* n = nn.get();
        for (int k = 0; n && n->fade && k < depth; ++k) n = dynamic_cast<NodeCtrl*>(n->fade->source.get());
        if (n && n->fade) { n->fade.reset(); n->fade_tags.reset(); }
    }
    /* start position, FUN_1409a04e0 */
    if (T) {
        const bf6::EbxValue* tob = T->f(0xb8f33128u);
        const bool any = ((int)numv(T->f(0x5645e663u)) & 2) != 0;
        if ((tob && tob->kind != bf6::EbxValue::Kind::Null) || any)
            rt.note("transition in-point entry (to-branch 0xb8f33128) not implemented: " + T->path);
        float v = (float)numv(T->f(0x263be7e5u));
        if (const Obj* va = rt.g.resolve(T->f(0x514691f6u), T)) { bool ok = false; v = rt.st.as_float(rt.g, va, ok); }
        const int m = (int)numv(T->f(0x882f89cbu));
        const float ot = out_tag ? (float)numv(out_tag->f(0x263be7e5u)) : 0.f;
        nn->copy_timing();
        switch (m) {
        case 0:
            if (v <= kEps && (!out_tag || ot <= kEps)) { if (nn->looping && old_loops) nn->set_phase(rt, nn->normalise(src_phase)); }
            else nn->set_phase(rt, nn->normalise(v > kEps ? v : ot));
            break;
        case 1: nn->set_phase(rt, nn->normalise(v + src_phase)); break;
        case 2: nn->set_phase(rt, nn->normalise(v)); break;
        case 6: if (out_tag) nn->set_phase(rt, nn->normalise(ot)); break;
        case 7: {
            const float L = nn->length > 0.f ? nn->length : 1.f;
            nn->set_phase(rt, nn->normalise(std::fmax(0.f, (float)numv(T->f(0x7b3c1705u))) / L));
            break;
        }
        case 5: break;
        default: rt.note("transition start mode " + std::to_string(m) + " not implemented: " + T->path);
        }
    }
    const bf6::EbxValue* ew = to->f(0xe15fd79du);
    if (ew && numv(ew) != 0.0) rt.note("node entry enum write (0x903c52d5) not implemented: " + to->path);
    node = std::move(nn);
    if (auto* fw = node->fade.get()) fw->copy_timing();
}

/* THE MOTION MACHINE, named but not run.
 *
 * MotionMachineLayersControllerAsset is where BF6 keeps locomotion, and it is
 * a CONTAINER: field 0xcf111f58 is a list of MotionMachineLayersEntryAsset,
 * each holding its own controller at 0x81309ef7 (the same field a chooser
 * entry uses). Its module, AnimationMotionMachine, declares about sixty types -
 * a stack of physics layers (acceleration from input, friction, gravity,
 * ground follow, rotation, buoyancy), pose matching, clip warping, path
 * following and a channel/tag system.
 *
 * So "implement the motion machine" is not one controller. Until it exists,
 * the useful thing is to say exactly WHICH layers a given asset asks for,
 * because that is what decides whether the work is four controllers or sixty.
 * A bare "controller type not implemented" hides that entirely. */
constexpr const char* kT_MMLayers = "a6304eea-e9ba-3218-4be6-86338eb3380c";
/* SignalDrivenBlendSpaceControllerAsset: signals in, a 2D blend position out. */
constexpr const char* kT_SignalBlendSpace = "3e08d128-2e68-d4f7-f8b2-7fa5a7261984";

/* THE LAYER STACK ITSELF, read from the executable.
 *
 *   FUN_140669d30  factory, allocates 0x98 "MotionMachineLayersController"
 *   FUN_140657de0  constructor
 *   FUN_140658100  entry array, 0x68 per "...::Entry"
 *   FUN_1406583e0  layer instantiation: the controller is the entry asset's
 *                  +0x28 (0x81309ef7), stored at Entry+0x08
 *   FUN_140658630  the update, decoded below
 *
 * The update walks the entries IN ORDER, accumulating onto a running pose:
 *
 *   skip unless Entry+0x5c bit 0 (enabled)
 *   skip unless Entry+0x14 > 1/65536, EXCEPT index 0 which always contributes
 *   evaluate the layer (its own vtable +0x58, the same slot this function is)
 *   BlendMode 0: FUN_1407a4b70 unmasked / FUN_1407a5010 masked
 *   BlendMode 1: FUN_1407a93d0 unmasked / FUN_1407a9810 masked
 *   nothing produced a pose at all -> FUN_1407a6190, the bind pass-through
 *
 * ALL FOUR BLEND CALLS PASS 1.0f. The per-layer float is a GATE, not a blend
 * factor; the actual weighting is the per-DOF mask. That is not what a reader
 * would assume from a field called weight, so it is worth stating.
 *
 * BlendMode is field 0x787a258e - THE SAME field and the same enum
 * SuperLayersCtrl already reads, where 0 blends, 1 adds, 4 adds negated and
 * 2/3 are ModelSpace and never ship. The exe decode and the existing
 * implementation agree on 0 and 1 independently, which is why this reuses
 * pose_blend/pose_add rather than inventing an operation. */
struct MMLayersCtrl : Ctrl {
    struct Layer {
        const Obj* e = nullptr;
        std::unique_ptr<Ctrl> ctrl;
        const Mask* mask = nullptr;
        int mode = 0;
    };
    std::vector<Layer> layers;

    /* Entry+0x5c bit 0. Authored as a source with a constant fallback, the
     * same pair SuperLayers uses on the same two field hashes. */
    static bool enabled_of(Runtime& rt, const Obj* e) {
        if (const Obj* s = rt.g.resolve(e->f(0x6e8c0b93u), e)) {
            bool ok = false;
            const bool v = rt.st.as_bool(rt.g, s, ok);
            if (ok) return v;
        }
        return boolv(e->f(0x18daea78u));
    }

    void init(Runtime& rt, const Obj* a) {
        asset = a;
        const bf6::EbxValue* es = a->f(0xcf111f58u);
        if (es) for (const auto& er : es->items) {
            Layer L;
            L.e = rt.g.resolve(&er, a);
            if (L.e) {
                L.ctrl = rt.make(resolve_ctrl_asset(rt, rt.g.resolve(L.e->f(0x81309ef7u), L.e)));
                L.mode = (int)numv(L.e->f(0x787a258eu));
                if (L.mode >= 2 && L.mode <= 3)
                    rt.note("ModelSpace motion machine layer blend (never ships) not implemented: " + a->path);
                else if (L.mode != 0 && L.mode != 1 && L.mode != 4)
                    rt.note("motion machine blend mode " + std::to_string(L.mode)
                            + " not implemented, blended: " + a->path);
                /* THE MASK IS A DIFFERENT KIND HERE. SuperLayers masks are a
                 * BlendMaskList; a motion machine entry's 0xb441d8d7 resolves
                 * to a boolean blend-mask CHANNEL on the shipped assets.
                 * build_mask would answer an all-zero mask for that, which
                 * reads as "this layer contributes nothing" - silently wrong.
                 * So only a real list is used, and anything else is named. */
                if (const Obj* mk = rt.g.resolve(L.e->f(0xb441d8d7u), L.e)) {
                    if (mk->type == kT_BlendMaskList) L.mask = build_mask(rt, mk);
                    else rt.note("motion machine layer mask kind not implemented ("
                                 + mk->type + "), layer blended unmasked: " + mk->path);
                }
            }
            layers.push_back(std::move(L));
        }
        if (const bf6::EbxValue* b = a->f(0x7172f1b5u))
            if (b->kind != bf6::EbxValue::Kind::Null)
                rt.note("motion machine controller +0x78 (0x7172f1b5) not read: " + a->path);
    }

    void update(Runtime& rt, float dt, Pose& out) override {
        bool have = false;
        Ctrl* timing = nullptr;
        for (size_t i = 0; i < layers.size(); ++i) {
            Layer& L = layers[i];
            if (!L.ctrl || !L.e) continue;
            /* Index 0 runs whatever the gate says; the rest need it. The
             * runtime weight at Entry+0x14 is not modelled, so an enabled
             * layer counts as fully on. */
            if (i != 0 && !enabled_of(rt, L.e)) continue;
            Pose p;
            L.ctrl->update(rt, dt, p);
            if (!have) { out = p; have = true; timing = L.ctrl.get(); continue; }
            if (L.mode == 1 || L.mode == 4) pose_add(out, p, L.mode == 4 ? -1.f : 1.f, L.mask);
            else pose_blend(out, p, 1.f, L.mask);
        }
        if (!have) out = rt.bind;
        /* The exe copies timing from a member at +0x70 that is not identified
         * yet; the first contributing layer stands in for it. */
        if (timing) { phase = timing->phase; time = timing->time; length = timing->length; remaining = timing->remaining; }
    }
    void set_time(Runtime& rt, float t) override {
        time = t;
        for (auto& L : layers) if (L.ctrl) { L.ctrl->set_time(rt, t); break; }
    }
    void set_phase(Runtime& rt, float p) override {
        phase = p;
        for (auto& L : layers) if (L.ctrl) { L.ctrl->set_phase(rt, p); break; }
    }
    Ctrl* tag_leaf() override {
        for (auto& L : layers) if (L.ctrl) return L.ctrl->tag_leaf();
        return nullptr;
    }
};

/* --------------------------------------------------------- blend space */

constexpr const char* kT_BlendSpaceAsset = "2bf0b88a-bb40-0e00-e186-be5a71a320fc";

/* A TRIANGULATED 2D BLEND SPACE (BlendSpaceAsset 0x207247a8, AnimationSbu).
 *
 * The space is field 0x08d13ba1, a 0x70-byte struct whose two arrays matter:
 *
 *   0x2c95d7b7  the POINTS: a Vec2Simd position (0x2455acb7, x 0x3901db14,
 *               y 0x42fc0f5e) and the clip played there (0x73e9a894)
 *   0x9c8d5f05  the TRIANGLES, triangulated and shipped; the three point
 *               indices are 0x2077a2ca, 0x9cd8d699, 0x79b6faf3
 *
 * So evaluation is: find the triangle holding the blend position, take
 * barycentric weights, blend those three clips.
 *
 * SYNC. Blended cycles must keep step or the result is mush. The game's sync
 * is a piecewise-linear time remap between two marker tables (FUN_1409aba50),
 * and FUN_1409ab3c0 turns it into a per-clip scale warped/reference with
 * offset reference-warped. WHERE A CLIP CARRIES NO AUTHORED MARKERS that
 * remap is the identity and the scale is simply clipLength/referenceLength -
 * which is exactly driving every clip at one shared NORMALISED phase, and is
 * what this does. The degenerate case is the game's own behaviour, not an
 * approximation of it.
 *
 * NOT IMPLEMENTED, named rather than faked: authored marker tables (a clip
 * that really carries sync points is matched proportionally instead of marker
 * to marker), and the authored clamp/clip modes outside the hull, where this
 * falls back to the nearest point. */
struct BlendSpaceCtrl : Ctrl {
    struct Pt { float x = 0.f, y = 0.f; std::unique_ptr<Ctrl> ctrl; };
    struct Tri { int a = 0, b = 0, c = 0; };
    std::vector<Pt> pts;
    std::vector<Tri> tris;
    float bx = 0.f, by = 0.f;          /* the blend position, set by the driver */
    bool warned_outside = false;
    /* The convex hull the asset ships, 0x82c94243: one directed edge per
     * boundary segment, with the triangle it bounds. */
    struct Edge { float ax = 0.f, ay = 0.f, bx = 0.f, by = 0.f; int tri = -1; };
    std::vector<Edge> edges;
    bool pts_flagged = false;

    static float axis(const bf6::EbxValue* v, uint32_t h) {
        if (!v) return 0.f;
        const bf6::EbxValue* c = v->field(h);
        return c ? (float)numv(c) : 0.f;
    }

    void init(Runtime& rt, const Obj* a) {
        asset = a;
        looping = true;
        remaining = kNeverEnds;
        const bf6::EbxValue* sp = a->f(0x08d13ba1u);
        if (!sp) { rt.note("blend space has no space struct: " + a->path); return; }
        if (const bf6::EbxValue* ps = sp->field(0x2c95d7b7u))
            for (const auto& p : ps->items) {
                Pt q;
                const bf6::EbxValue* pos = p.field(0x2455acb7u);
                q.x = axis(pos, 0x3901db14u);
                q.y = axis(pos, 0x42fc0f5eu);
                q.ctrl = rt.make(resolve_ctrl_asset(rt, rt.g.resolve(p.field(0x73e9a894u), a)));
                if (boolv(p.field(0xea7c7a5bu)) || boolv(p.field(0x68dcdb81u))) pts_flagged = true;
                pts.push_back(std::move(q));
            }
        if (const bf6::EbxValue* ts = sp->field(0x9c8d5f05u))
            for (const auto& t : ts->items) {
                Tri T;
                T.a = (int)numv(t.field(0x2077a2cau));
                T.b = (int)numv(t.field(0x9cd8d699u));
                T.c = (int)numv(t.field(0x79b6faf3u));
                tris.push_back(T);
            }
        if (const bf6::EbxValue* es = sp->field(0x82c94243u))
            for (const auto& e : es->items) {
                Edge E;
                E.ax = axis(e.field(0xb2b3d85au), 0x3901db14u);
                E.ay = axis(e.field(0xb2b3d85au), 0x42fc0f5eu);
                E.bx = axis(e.field(0xe1c074ffu), 0x3901db14u);
                E.by = axis(e.field(0xe1c074ffu), 0x42fc0f5eu);
                E.tri = (int)numv(e.field(0xf0a8b02fu));
                edges.push_back(E);
            }
        if (pts.empty()) rt.note("blend space has no points: " + a->path);
        else if (tris.empty())
            rt.note("blend space has no triangles, nearest point used: " + a->path);
        /* WHAT IS ACTUALLY LEFT HERE, having gone looking for it.
         *
         * This used to report unread "sync-point marker tables". There are
         * none. A blend space ships exactly two arrays besides its points and
         * triangles: 0x82c94243, the convex hull as directed edges, now read
         * and used to clamp a position that lands outside it, and 0xdcc6cb80,
         * a grid of cells each listing the triangles that overlap it - a
         * lookup accelerator whose only effect is how fast the containing
         * triangle is found, not which one. The clips themselves carry only
         * game-state tags, no markers.
         *
         * Clips are matched by one shared normalised phase against a
         * weighted reference length, which is what keeps a walk and a run
         * cycle on the same foot. What remains unread is two flags per point,
         * 0xea7c7a5b and 0x68dcdb81, both the same value on every point of
         * every space measured, so there is nothing to tell from the data
         * about what they select. */
        else if (pts_flagged)
            rt.note("blend space per-point flags 0xea7c7a5b/0x68dcdb81 not read: " + a->path);
    }

    bool inside(const Tri& T, float x, float y, float* w) const {
        const int n = (int)pts.size();
        if (T.a < 0 || T.b < 0 || T.c < 0 || T.a >= n || T.b >= n || T.c >= n) return false;
        const Pt& A = pts[(size_t)T.a];
        const Pt& B = pts[(size_t)T.b];
        const Pt& C = pts[(size_t)T.c];
        const float d = (B.y - C.y) * (A.x - C.x) + (C.x - B.x) * (A.y - C.y);
        if (std::fabs(d) < 1e-12f) return false;
        w[0] = ((B.y - C.y) * (x - C.x) + (C.x - B.x) * (y - C.y)) / d;
        w[1] = ((C.y - A.y) * (x - C.x) + (A.x - C.x) * (y - C.y)) / d;
        w[2] = 1.f - w[0] - w[1];
        return w[0] >= -1e-4f && w[1] >= -1e-4f && w[2] >= -1e-4f;
    }

    void update(Runtime& rt, float dt, Pose& out) override {
        int idx[3] = {-1, -1, -1};
        float w[3] = {0.f, 0.f, 0.f};
        bool found = false;
        for (const Tri& T : tris) {
            float bw[3];
            if (!inside(T, bx, by, bw)) continue;
            idx[0] = T.a; idx[1] = T.b; idx[2] = T.c;
            w[0] = bw[0]; w[1] = bw[1]; w[2] = bw[2];
            found = true;
            break;
        }
        /* OUTSIDE THE HULL: clamp onto the boundary the asset ships.
         *
         * 0x82c94243 is the convex hull as directed edges, each carrying its
         * start and end, a unit direction, the outward normal, its length and
         * the triangle it belongs to - everything needed to project a position
         * back onto the boundary. Snapping to the nearest POINT instead throws
         * away the edge the position is next to, which on a locomotion space
         * means a diagonal input collapses onto one pure direction. */
        if (!found && !edges.empty()) {
            float bd = 0.f, cx = 0.f, cy = 0.f;
            int btri = -1;
            for (const Edge& e : edges) {
                const float ex = e.bx - e.ax, ey = e.by - e.ay;
                const float l2 = ex * ex + ey * ey;
                float t = l2 > 1e-12f ? ((bx - e.ax) * ex + (by - e.ay) * ey) / l2 : 0.f;
                t = std::fmin(std::fmax(t, 0.f), 1.f);
                const float px = e.ax + ex * t, py = e.ay + ey * t;
                const float dx = px - bx, dy = py - by;
                const float d2 = dx * dx + dy * dy;
                if (btri < 0 || d2 < bd) { bd = d2; cx = px; cy = py; btri = e.tri; }
            }
            if (btri >= 0 && btri < (int)tris.size()) {
                const Tri& T = tris[(size_t)btri];
                float bw[3] = {0.f, 0.f, 0.f};
                inside(T, cx, cy, bw);            /* weights, valid or not */
                float s = 0.f;
                for (int k = 0; k < 3; ++k) { bw[k] = std::fmax(bw[k], 0.f); s += bw[k]; }
                if (s > 1e-6f) {
                    idx[0] = T.a; idx[1] = T.b; idx[2] = T.c;
                    for (int k = 0; k < 3; ++k) w[k] = bw[k] / s;
                    found = true;
                }
            }
        }
        if (!found && !pts.empty()) {
            int best = -1;
            float bd = 0.f;
            for (size_t i = 0; i < pts.size(); ++i) {
                const float dx = pts[i].x - bx, dy = pts[i].y - by;
                const float d2 = dx * dx + dy * dy;
                if (best < 0 || d2 < bd) { best = (int)i; bd = d2; }
            }
            idx[0] = best;
            w[0] = 1.f;
            if (!warned_outside && !tris.empty() && asset) {
                warned_outside = true;
                rt.note("blend space position outside the hull and the asset ships no hull "
                        "edge list, nearest point used: " + asset->path);
            }
        }
        /* The reference length sync scales onto, weighted like the pose is. */
        float ref = 0.f, tw = 0.f;
        for (int k = 0; k < 3; ++k)
            if (idx[k] >= 0 && w[k] > 0.f && pts[(size_t)idx[k]].ctrl) {
                ref += pts[(size_t)idx[k]].ctrl->length * w[k];
                tw += w[k];
            }
        length = (tw > 0.f && ref > 1e-4f) ? ref / tw : 1.f;
        time += dt;
        if (length > 1e-4f) time = time - std::trunc(time / length) * length;
        phase = length > 1e-4f ? time / length : 0.f;
        remaining = kNeverEnds;

        bool have = false;
        float acc = 0.f;
        for (int k = 0; k < 3; ++k) {
            if (idx[k] < 0 || w[k] <= 1e-5f) continue;
            Ctrl* c = pts[(size_t)idx[k]].ctrl.get();
            if (!c) continue;
            c->set_phase(rt, phase);          /* one shared normalised phase */
            Pose p;
            c->update(rt, 0.f, p);
            if (!have) { out = p; have = true; acc = w[k]; }
            else { acc += w[k]; if (acc > 0.f) pose_blend(out, p, w[k] / acc, nullptr); }
        }
        if (!have) out = rt.bind;
    }
    void set_time(Runtime& rt, float t) override {
        (void)rt; time = t; phase = length > 1e-4f ? t / length : 0.f;
    }
    void set_phase(Runtime& rt, float p) override {
        (void)rt; phase = normalise(p); time = length * phase;
    }
};

/* One axis of a signal-driven blend space, FUN_1409cc470 ported whole.
 *
 * It is a rate limiter with an acceleration limit and an optional wrap, and it
 * carries a velocity across frames. That last part is why the raw signal steps
 * where the game ramps: without the stored velocity there is nothing for the
 * acceleration limit to act on.
 *
 * `cur` is last frame's smoothed value and `want` the raw signal. `dt` is in
 * ticks, like every other time in this runtime, so the rates are per tick.
 * `rate_pos`/`rate_neg` limit a rising and a falling value; `signed_speed`
 * reinterprets them as away-from-centre and back-to-centre instead, which is
 * what an aim-yaw axis wants. `wrap` takes the short way round a cyclic axis.
 * `accel_max` limits the change in velocity EXCEPT when that change opposes
 * the velocity already there: braking is never acceleration-limited.
 *
 * The engine keeps `cur` and the velocity on the controller (+0x88/+0x8c and
 * +0x98/+0x9c), zero-initialised, so the first frame ramps from 0 clamped into
 * the axis range - for an aim axis that is the centre, and for a speed axis
 * that is standing still. */
static float axis_smooth(float cur, float want, float dt, float rate_pos, float rate_neg,
                         float* vel, float accel_max, bool wrap, bool signed_speed,
                         float lo, float hi)
{
    constexpr float kSmoothEps = 1.1920929e-07f;
    if (cur <= lo) cur = lo;
    if (want <= lo) want = lo;
    if (hi <= cur) cur = hi;
    if (hi <= want) want = hi;
    /* Which limit applies in which direction. With signed_speed the axis is
     * signed around a centre and BOTH limits become one of the two: rate_neg
     * while the value heads back toward zero, rate_pos while it leaves. */
    float up = rate_pos, down = rate_neg;
    if (signed_speed) {
        bool toward_zero = false;
        if (cur > kSmoothEps || cur < -kSmoothEps)
            toward_zero = ((cur < 0.f) != (want < 0.f)) || std::fabs(want) < std::fabs(cur);
        up = down = toward_zero ? rate_neg : rate_pos;
    }
    const float inv = std::fabs(dt) >= 1.5258789e-05f ? 1.f / dt : 1000.f;
    float d = want - cur;
    const float range = hi - lo;
    if (wrap && range > 1e-6f && std::fabs(d) > range * 0.5f) {
        float a = std::fabs(d), r = range;
        if (d < 0.f) { r = -range; a = -a; }
        d = a - r;
    }
    float step = d < 0.f ? std::fmax(d, -(dt * down)) : std::fmin(d, dt * up);
    /* A reversal drops the stored velocity rather than decelerating through it. */
    float pv = *vel;
    if ((step < 0.f ? -1.f : 1.f) != (pv < 0.f ? -1.f : 1.f)) pv = 0.f;
    float nv = step * inv;
    const float accel = (nv - pv) * inv;
    if (accel_max < std::fabs(accel)) {
        bool braking = false;
        if (pv > kSmoothEps || pv < -kSmoothEps)
            braking = (accel < 0.f ? -1.f : 1.f) != (pv < 0.f ? -1.f : 1.f);
        if (!braking && (step > kSmoothEps || step < -kSmoothEps)) {
            nv = (accel < 0.f ? -accel_max : accel_max) * dt + pv;
            step = nv * dt;
        }
    }
    float outv = cur + step;
    if (wrap && range > 1e-6f) {
        while (outv < lo) outv += range;
        while (outv > hi) outv -= range;
    }
    *vel = nv;
    return outv;
}

/* SignalDrivenBlendSpaceControllerAsset: two signals in, a 2D blend position
 * out. FUN_1409cbd10 reads the two signals through interface 0x67745308,
 * smooths each with FUN_1409cc470, writes the pair into the inner blend space
 * and echoes the smoothed values back through the asset's own two output
 * signals when they are set.
 *
 * The asset's field order IS its memory order, which is how the smoothing
 * parameters were identified: the eight references land on 0x38..0x70, so the
 * eleven floats that follow start at 0x78 and the six bools at 0xa4 - exactly
 * where FUN_1409cbd10 reads them. The values agree independently: on
 * 1p.loco.unarmed.dragging.crouch.cycles.bs the X axis runs -1.309..1.309
 * (+-75 degrees of aim yaw) and the Y axis 0..2.1 (metres per second). */
struct SignalBlendSpaceCtrl : Ctrl {
    std::unique_ptr<Ctrl> inner;
    BlendSpaceCtrl* space = nullptr;
    const Obj* sx = nullptr;          /* +0x38, +0x40: the driving signals */
    const Obj* sy = nullptr;
    const Obj* ox = nullptr;          /* +0x48, +0x50: the smoothed value echoed back */
    const Obj* oy = nullptr;
    struct Axis {
        float lo = 0.f, hi = 0.f;     /* +0x78/+0x7c, +0x90/+0x94 */
        float rate_pos = 0.f;         /* +0x80, +0x88 */
        float rate_neg = 0.f;         /* +0x84, +0x8c */
        float accel = 0.f;            /* +0x98, +0x9c */
        bool wrap = false;            /* +0xa7, +0xa8 */
        bool signed_speed = false;    /* +0xa5, +0xa6 */
        float cur = 0.f, vel = 0.f;   /* controller +0x88/+0x8c, +0x98/+0x9c */
    } ax, ay;

    void init(Runtime& rt, const Obj* a) {
        asset = a;
        sx = rt.g.resolve(a->f(0x265245deu), a);
        sy = rt.g.resolve(a->f(0x5d02d6edu), a);
        ox = rt.g.resolve(a->f(0x4e59ce9fu), a);
        oy = rt.g.resolve(a->f(0x92980bd8u), a);
        ax.lo = (float)numv(a->f(0x4ed99195u));  ay.lo = (float)numv(a->f(0x1083436bu));
        ax.hi = (float)numv(a->f(0x53852281u));  ay.hi = (float)numv(a->f(0x0a32433au));
        ax.rate_pos = (float)numv(a->f(0xeb403e86u));
        ax.rate_neg = (float)numv(a->f(0x9d182dd3u));
        ay.rate_pos = (float)numv(a->f(0x250e14e6u));
        ay.rate_neg = (float)numv(a->f(0xdbe08598u));
        ax.accel = (float)numv(a->f(0x91360232u));
        ay.accel = (float)numv(a->f(0xd5b36263u));
        ax.signed_speed = boolv(a->f(0x10dad74eu));
        ay.signed_speed = boolv(a->f(0x1b2cd32fu));
        ax.wrap = boolv(a->f(0x4d7430feu));
        ay.wrap = boolv(a->f(0x2acad9ddu));
        ax.cur = std::fmin(std::fmax(0.f, ax.lo), ax.hi);
        ay.cur = std::fmin(std::fmax(0.f, ay.lo), ay.hi);
        inner = rt.make(resolve_ctrl_asset(rt, rt.g.resolve(a->f(0x776fb08cu), a)));
        space = dynamic_cast<BlendSpaceCtrl*>(inner.get());
        if (ax.hi <= ax.lo || ay.hi <= ay.lo)
            rt.note("signal blend space axis has an empty range, smoothing clamps to it: " + a->path);
    }
    void update(Runtime& rt, float dt, Pose& out) override {
        float wx = ax.cur, wy = ay.cur;
        bool ok = false;
        if (sx) { const float v = rt.st.as_float(rt.g, sx, ok); if (ok) wx = v; }
        ok = false;
        if (sy) { const float v = rt.st.as_float(rt.g, sy, ok); if (ok) wy = v; }
        ax.cur = axis_smooth(ax.cur, wx, dt, ax.rate_pos, ax.rate_neg, &ax.vel,
                             ax.accel, ax.wrap, ax.signed_speed, ax.lo, ax.hi);
        ay.cur = axis_smooth(ay.cur, wy, dt, ay.rate_pos, ay.rate_neg, &ay.vel,
                             ay.accel, ay.wrap, ay.signed_speed, ay.lo, ay.hi);
        if (space) { space->bx = ax.cur; space->by = ay.cur; }
        /* The smoothed value is published for whatever else reads that signal. */
        if (ox) rt.st.set_float(ox->path, ax.cur);
        if (oy) rt.st.set_float(oy->path, ay.cur);
        if (!inner) { out = rt.bind; return; }
        inner->update(rt, dt, out);
        phase = inner->phase; time = inner->time;
        length = inner->length; remaining = inner->remaining; looping = inner->looping;
    }
    void set_time(Runtime& rt, float t) override { if (inner) inner->set_time(rt, t); time = t; }
    void set_phase(Runtime& rt, float p) override { if (inner) inner->set_phase(rt, p); phase = p; }
    Ctrl* tag_leaf() override { return inner ? inner->tag_leaf() : nullptr; }
};

/* --------------------------------------------------------- sequence */

/* SequenceContainerAsset (189b3989) and ActorControllerAsset (039e9aa4).
 *
 * A scripted move - a vault, a ladder mount, a zipline grab - is authored ONCE
 * for every actor it involves, and the 1P and 3P halves of the local soldier
 * are two of those actors. The container holds one ActorControllerAsset per
 * actor in 0x5aae4e39 and an enum game state in 0x62f7dd2e (mm.1por3p) that
 * picks between them: the value indexes the array, and measured across every
 * multi-actor sequence in the install the order is fixed - index 0 is the actor
 * named Player0 and carries the 3P clips, index 1 is Player1 and carries the 1P
 * clips. The instance numbering behind the references is NOT in that order, so
 * the array position is what decides, not the instance id.
 *
 * An actor holds tracks (0xf8c3f7c5); each track is a DofSet (0x645b319c) over
 * a list of entries (0xdc5ce195). An entry places a ClipController (0x73e9a894)
 * on the actor's timeline at 0xa0548731 for 0x4e05720f ticks, and the actor's
 * own 0xfc88f64b is the length of the whole sequence. That is why a 109-tick
 * clip starting at -25 lands on an 84-tick actor: the first 25 ticks are
 * pre-roll that the sequence never shows. 0xde60aca6 carries the same 25 as a
 * clip in-point, so the two readings agree on every shipped file; they are
 * cross-checked below and a disagreement is reported rather than guessed at. */
constexpr const char* kT_Sequence = "189b3989-b657-17b1-f2e5-fe1a60630061";
constexpr const char* kT_Actor    = "039e9aa4-d525-7327-315e-6fdd3f7c04f4";

/* A DofSetList (3dbe09b4) names the DOFs a sequence track drives. It carries
 * both the leaf DofSets it resolves to, ALREADY FLATTENED, in 0x4c23c8ce, and
 * the sub-lists it was authored from in 0xf162c58d; the flattened list is the
 * one to read, and the sub-lists are only walked if it is empty.
 *
 * build_mask cannot be reused for this even though it reads the same field: it
 * takes each set's weight from a BlendMaskList's parallel 0x42c8b257 array and
 * treats a missing weight as 0. A DofSetList has no weights, so every set would
 * come out at zero and the mask would DELETE the track it is meant to mask. */
constexpr const char* kT_DofSetList = "3dbe09b4-7661-0e3b-032f-e8fc2db45b7a";

const Mask* dofset_mask(Runtime& rt, const Obj* ds)
{
    if (!ds) return nullptr;
    const std::string key = "dofset:" + ds->path;
    auto it = rt.masks.find(key);
    if (it != rt.masks.end()) return it->second.get();
    auto m = std::make_unique<Mask>();
    m->rot.assign((size_t)rt.bones, 0.f);
    m->tr.assign((size_t)rt.bones, 0.f);
    int hit = 0;
    auto add = [&](const Obj* set) {
        if (!set) return;
        for (const auto& rec : ds_records(set)) {
            auto b = rt.bone_index.find(rec.first);
            if (b == rt.bone_index.end()) continue;
            if (rec.second == 'q') { m->rot[(size_t)b->second] = 1.f; ++hit; }
            else if (rec.second == 't') { m->tr[(size_t)b->second] = 1.f; ++hit; }
        }
    };
    add(ds);                                  /* named a leaf DofSetAsset directly */
    if (const bf6::EbxValue* flat = ds->f(0x4c23c8ceu))
        for (const auto& r : flat->items) add(rt.g.resolve(&r, ds));
    if (hit == 0)
        if (const bf6::EbxValue* subs = ds->f(0xf162c58du))
            for (const auto& r : subs->items)
                if (const Obj* sub = rt.g.resolve(&r, ds))
                    if (const bf6::EbxValue* f2 = sub->f(0x4c23c8ceu))
                        for (const auto& r2 : f2->items) add(rt.g.resolve(&r2, sub));
    /* An all-zero mask silently deletes the track. If nothing matched, the set
     * is for another rig (or did not parse) and NO mask is the honest answer. */
    if (hit == 0) {
        rt.note("sequence track dofset matched no bone of this rig, track unmasked: " + ds->path);
        return nullptr;
    }
    const Mask* out = m.get();
    rt.masks[key] = std::move(m);
    return out;
}

/* ONE ACTOR'S TRACKS. An ActorControllerAsset is what both a sequence and a
 * sequence flow are made of, so the playback lives here once.
 *
 * Tracks (0xf8c3f7c5) each mask a DofSet (0x645b319c) over a list of entries
 * (0xdc5ce195). An entry places a ClipController (0x73e9a894) on the actor's
 * timeline at 0xa0548731 for 0x4e05720f ticks, and the actor's own 0xfc88f64b
 * is the length of the whole thing. A 109-tick clip starting at -25 lands on an
 * 84-tick actor: the first 25 ticks are pre-roll the actor never shows. That
 * reading is confirmed on every shipped asset checked - 0xde60aca6 carries the
 * same offset as a clip in-point, and start + length equals the actor length
 * (-58 + 238 = 180, -232 + 290 = 58) - and a disagreement is reported rather
 * than guessed at. */
struct ActorCtrl : Ctrl {
    struct Entry {
        std::unique_ptr<Ctrl> ctrl;
        float start = 0.f;      /* 0xa0548731, ticks on the actor timeline */
        float len = 0.f;        /* 0x4e05720f, ticks */
    };
    struct Track {
        std::vector<Entry> entries;
        const Mask* mask = nullptr;
    };
    std::vector<Track> tracks;

    /* `owner` is the partition the references resolve against, which is the
     * sequence or flow asset, not necessarily the actor itself. */
    void init(Runtime& rt, const Obj* actor, const Obj* owner) {
        asset = actor;
        looping = false;
        if (!actor) return;
        if (actor->type != kT_Actor)
            rt.note("sequence member is not an actor controller (" + actor->type + "): " + owner->path);
        float span = 0.f;
        const bf6::EbxValue* ts = actor->f(0xf8c3f7c5u);
        static const std::vector<bf6::EbxValue> kNone;
        for (const auto& tr : ts ? ts->items : kNone) {
            Track T;
            T.mask = dofset_mask(rt, rt.g.resolve(tr.field(0x645b319cu), actor));
            if (const bf6::EbxValue* x = tr.field(0xbc5b17fau))
                if (!x->items.empty())
                    rt.note("sequence track second list (0xbc5b17fa) not implemented: " + owner->path);
            const bf6::EbxValue* es = tr.field(0xdc5ce195u);
            for (const auto& er : es ? es->items : kNone) {
                Entry E;
                E.start = (float)numv(er.field(0xa0548731u));
                E.len = (float)numv(er.field(0x4e05720fu));
                const float inpoint = (float)numv(er.field(0xde60aca6u));
                if (std::fabs(E.start + inpoint) > 1e-3f)
                    rt.note("sequence entry start and clip in-point disagree, start used: " + owner->path);
                const float w = (float)numv(er.field(0x533f6740u));
                if (std::fabs(w - 1.f) > 1e-3f)
                    rt.note("sequence entry 0x533f6740 is not 1 and its meaning is not "
                            "established, ignored: " + owner->path);
                E.ctrl = rt.make(resolve_ctrl_asset(rt, rt.g.resolve(er.field(0x73e9a894u), actor)));
                if (!E.ctrl) { rt.note("sequence entry has no clip: " + owner->path); continue; }
                span = std::fmax(span, E.start + E.len);
                T.entries.push_back(std::move(E));
            }
            /* Entries that overlap in time would have to crossfade through the
             * entry blend curve 0x9bffb09e; every shipped track holds one. */
            for (size_t i = 1; i < T.entries.size(); ++i)
                if (T.entries[i].start < T.entries[i - 1].start + T.entries[i - 1].len) {
                    rt.note("sequence entries overlap, crossfade not implemented: " + owner->path);
                    break;
                }
            if (!T.entries.empty()) tracks.push_back(std::move(T));
        }
        if (tracks.empty()) { rt.note("sequence actor has no clip: " + owner->path); return; }
        length = (float)numv(actor->f(0xfc88f64bu));
        if (length <= 0.f) length = span > 0.f ? span : 1.f;
        time = 0.f;
        phase = 0.f;
        remaining = length;
    }
    bool empty() const { return tracks.empty(); }

    /* Entries sit on an absolute timeline, so they are DRIVEN to the time they
     * should be showing rather than advanced by dt: an entry starting at -25
     * has to be 25 ticks in on the actor's very first frame. */
    void update(Runtime& rt, float dt, Pose& out) override {
        wrapped = false;
        time = std::fmin(time + dt, length);
        phase = normalise(length > 1e-4f ? time / length : 0.f);
        remaining = std::fmax(length - time, 0.f);
        write(rt, out);
    }
    void write(Runtime& rt, Pose& out) {
        out = rt.bind;
        std::fill(out.vq.begin(), out.vq.end(), 0);
        std::fill(out.vt.begin(), out.vt.end(), 0);
        for (Track& T : tracks) {
            Entry* act = nullptr;
            for (Entry& E : T.entries) {
                if (time < E.start) continue;
                if (time >= E.start + E.len && &E != &T.entries.back()) continue;
                act = &E;
            }
            if (!act) act = &T.entries.front();
            Pose p;
            act->ctrl->set_time(rt, time - act->start);
            act->ctrl->update(rt, 0.f, p);
            pose_blend(out, p, 1.f, T.mask);
        }
    }
    Entry* active() {
        for (Track& T : tracks)
            for (Entry& E : T.entries)
                if (time >= E.start && time < E.start + E.len) return &E;
        return tracks.empty() || tracks.front().entries.empty()
            ? nullptr : &tracks.front().entries.front();
    }
    void set_time(Runtime& rt, float t) override {
        (void)rt;
        time = std::fmin(std::fmax(t, 0.f), length);
        remaining = std::fmax(length - time, 0.f);
        phase = length > 1e-4f ? time / length : 0.f;
    }
    void set_phase(Runtime& rt, float p) override { set_time(rt, length * normalise(p)); }
    Ctrl* tag_leaf() override { Entry* e = active(); return e && e->ctrl ? e->ctrl->tag_leaf() : nullptr; }
};

/* Which member of a sequence or a flow is US. Both types hold one member per
 * actor in 0x5aae4e39 and name an enum game state in 0x62f7dd2e; the value
 * indexes the array. Measured across every multi-actor sequence in the install
 * the order is fixed: index 0 is the member named Player0 and carries the 3P
 * clips, index 1 is Player1 and the 1P clips, whatever order the instances
 * behind the references happen to be in. mm.1por3p.enumgs defaults to 1, so a
 * runtime that sets nothing gets the first-person half. */
static const Obj* pick_member(Runtime& rt, const Obj* a, const char* what)
{
    const bf6::EbxValue* ms = a->f(0x5aae4e39u);
    if (!ms || ms->items.empty()) { rt.note(std::string(what) + " has no members: " + a->path); return nullptr; }
    int idx = 0;
    if (const Obj* gs = rt.g.resolve(a->f(0x62f7dd2eu), a)) {
        bool ok = false;
        const int v = rt.st.as_int(rt.g, gs, ok);
        if (ok) idx = v;
        else if (ms->items.size() > 1)
            rt.note(std::string(what) + " member enum not evaluable, first used: " + a->path);
    } else if (ms->items.size() > 1) {
        rt.note(std::string(what) + " has several members and no selector enum: " + a->path);
    }
    if (idx < 0 || idx >= (int)ms->items.size()) idx = 0;
    const Obj* m = rt.g.resolve(&ms->items[(size_t)idx], a);
    if (!m) rt.note(std::string(what) + " member missing: " + a->path);
    return m;
}

struct SequenceCtrl : Ctrl {
    ActorCtrl actor;

    void init(Runtime& rt, const Obj* a) {
        asset = a;
        looping = false;
        actor.init(rt, pick_member(rt, a, "sequence"), a);
        length = actor.length;
        remaining = actor.remaining;
    }
    void update(Runtime& rt, float dt, Pose& out) override {
        if (actor.empty()) { out = rt.bind; return; }
        actor.update(rt, dt, out);
        time = actor.time; phase = actor.phase;
        length = actor.length; remaining = actor.remaining;
    }
    void set_time(Runtime& rt, float t) override { actor.set_time(rt, t); time = actor.time; }
    void set_phase(Runtime& rt, float p) override { actor.set_phase(rt, p); phase = actor.phase; }
    Ctrl* tag_leaf() override { return actor.empty() ? nullptr : actor.tag_leaf(); }
};

/* SequenceFlowContainerAsset (87cfb3d1) and its per-actor flow (100c61a0).
 *
 * A flow holds STATES (0xf8c3f7c5, type b434df78), each naming an
 * ActorControllerAsset in 0x564beb5f with its own length in 0xed10e827 and a
 * flag in 0x7e3f6d2e. An action authored this way is an enter, a middle and an
 * exit: adtv.1p.rifle.stand.run.suppression.seqflow has 63, 180 and 58 ticks,
 * and it is the 180 that carries the flag.
 *
 * NOTHING CONNECTS THE STATES IN SHIPPED DATA. The container's two transition
 * arrays (0xa562b576, 0xcb73fd98) and the flow's (0x21a22ce4) are empty on
 * every seqflow checked, every state starts at 0, and 0xa5bb4d94 is 1 whether
 * the flow has two states or seven, so it is not a start index. So the states
 * are played in array order, back to back, which is what their differing
 * lengths are for. What the flag on a state means is NOT established, so it is
 * reported rather than acted on: reading it as "loop forever" would stop the
 * exit state ever playing, which is a worse guess than playing each state once.
 * If an asset ever does ship a transition array, that is reported too. */
constexpr const char* kT_SeqFlow = "87cfb3d1-8b54-ca3a-b2c3-b7fbe1d9bb38";
constexpr const char* kT_Flow    = "100c61a0-b263-94e2-c056-eb61bb254044";

/* ---------------------------------------------------- pose game states */

/* A POSE HELD IN A GAME STATE, read where a controller is expected.
 *
 * PoseAssetGS (type ff206dad, named in the research repo) carries no pose of its own -
 * its only fields are a flag and its name - because it is a SLOT. One layer writes its
 * composed pose into it through 0x899b420c and another layer names it as its
 * controller to read it back. Before the type was named this was two notes:
 * "additive-capture layer (+0xa8) not implemented" on 1p.poselayer.slc and
 * 1p.gameplay.slc, and a layer whose controller would not build.
 *
 * Nothing written yet means nothing to contribute, which is indistinguishable from the
 * layer being absent - so the pose comes back as the parent, or bind when there is no
 * parent, rather than as zeros. */
constexpr const char* kT_PoseState = "ff206dad-a7bf-1ebb-3d60-75b5096baddf";

struct PoseStateCtrl : Ctrl {
    std::string slot;
    bool warned = false;

    void init(Runtime& rt, const Obj* a) {
        asset = a;
        slot = a->path;
        (void)rt;
    }
    void update(Runtime& rt, float dt, Pose& out) override {
        (void)dt;
        auto it = rt.pose_states.find(slot);
        if (it != rt.pose_states.end() && it->second.q.size() == rt.bind.q.size()) {
            out = it->second;
            return;
        }
        if (!warned) {
            warned = true;
            rt.note("pose game state read before anything wrote it: " + slot);
        }
        out = (rt.have_parent && rt.parent.q.size() == rt.bind.q.size()) ? rt.parent : rt.bind;
    }
    void set_time(Runtime&, float) override {}
    void set_phase(Runtime&, float) override {}
};

/* -------------------------------------------------- signal blend array */

/* SignalBlendArrayControllerAsset, which is what the ladder is.
 *
 * Named in the research repo (41c01e0d), and its shape comes from the repo's field rows
 * plus the one asset the first-person chain uses:
 *
 *   0x0C50E436  the primary clip
 *   0x7CCB650C  the SIGNAL that positions the blend
 *   0xDFBBFC76  a BlendArrayAsset, whose 0x2C95D7B7 is an array of
 *               BlendArrayItemStruct { clip 0xAE2FFC58, ClipInitParams 0xDB6F3DFC,
 *                                      X 0x3901DB14, length 0xA39ED53F }
 *   0x42C8B257  the signal's default
 *
 * 1p.adtv.ladder.enter.ba reads: three items at X = 0, 0.5 and 1 -
 * ladder_enter_bottom_to_idle_LEFT, _RIGHT and _LEFT again, 37 frames each - positioned by
 * common/climb.phase.float. So the ladder entry picks which foot leads from how far up the
 * climb is, and blends between the two.
 *
 * This is a blend space in one dimension, so it is built the same way: one controller per
 * item, the signal clamped to the item range, and the two items either side of it blended
 * by where it falls between them. Before this the node the ladder machine correctly
 * reached posed nothing at all, because the type had no controller. */
constexpr const char* kT_SignalBlendArray = "5e166389-6882-ce71-01cd-cc2fb49be744";
constexpr const char* kT_BlendArray       = "3890e550-e591-a397-0486-599c38961b7c";

struct SignalBlendArrayCtrl : Ctrl {
    struct Item { float x = 0.f; std::unique_ptr<Ctrl> ctrl; };
    std::vector<Item> items;          /* sorted by x */
    const Obj* signal = nullptr;      /* 0x7CCB650C */
    float fallback = 0.f;             /* 0x42C8B257, the signal's default */
    std::unique_ptr<Ctrl> primary;    /* 0x0C50E436, used when the array is empty */

    void init(Runtime& rt, const Obj* a) {
        asset = a;
        signal = rt.g.resolve(a->f(0x7ccb650cu), a);
        fallback = (float)numv(a->f(0x42c8b257u));
        /* The items live on a BlendArrayAsset, which this controller points at - or on
         * this asset itself when the array IS the controller. */
        const Obj* arr = a->f(0x2c95d7b7u) ? a : rt.g.resolve(a->f(0xdfbbfc76u), a);
        if (arr) {
            if (const bf6::EbxValue* is = arr->f(0x2c95d7b7u))
                for (const auto& it : is->items) {
                    Item q;
                    q.x = (float)numv(it.field(0x3901db14u));
                    q.ctrl = rt.make(resolve_ctrl_asset(rt,
                        rt.g.resolve(it.field(0xae2ffc58u), arr)));
                    if (q.ctrl) items.push_back(std::move(q));
                }
        }
        std::stable_sort(items.begin(), items.end(),
                         [](const Item& p, const Item& q) { return p.x < q.x; });
        if (items.empty()) {
            if (const Obj* c = resolve_ctrl_asset(rt, rt.g.resolve(a->f(0x0c50e436u), a)))
                primary = rt.make(c);
            if (!primary) rt.note("signal blend array has no items: " + a->path);
        }
        if (!signal && !items.empty())
            rt.note("signal blend array has no signal, its first item is used: " + a->path);
    }
    bool is_additive() const override {
        if (!items.empty()) return items.front().ctrl && items.front().ctrl->is_additive();
        return primary && primary->is_additive();
    }

    void update(Runtime& rt, float dt, Pose& out) override {
        if (items.empty()) {
            if (primary) { primary->update(rt, dt, out); copy_from(primary.get()); }
            else out = rt.bind;
            return;
        }
        float s = fallback;
        if (signal) {
            bool ok = false;
            const float v = rt.st.as_float(rt.g, signal, ok);
            if (ok) s = v;
        }
        s = std::fmin(std::fmax(s, items.front().x), items.back().x);
        size_t hi = 0;
        while (hi + 1 < items.size() && items[hi].x < s) ++hi;
        const size_t lo = hi == 0 ? 0 : hi - 1;
        const float span = items[hi].x - items[lo].x;
        const float t = span > 1e-6f ? (s - items[lo].x) / span : 0.f;
        /* Every item is advanced, not only the two in play, so a blend that moves does not
         * jump to a clip that has been standing still - the same reason a blend space keeps
         * all of its points running. */
        Pose p;
        for (size_t i = 0; i < items.size(); ++i) {
            Pose q;
            items[i].ctrl->update(rt, dt, q);
            if (i == lo) p = q;
            else if (i == hi && t > 0.f) pose_blend(p, q, t, nullptr);
        }
        out = p;
        copy_from(items[lo].ctrl.get());
    }
    void copy_from(const Ctrl* c) {
        if (!c) return;
        phase = c->phase; time = c->time; length = c->length; remaining = c->remaining;
        looping = c->looping;
    }
    void set_time(Runtime& rt, float t) override {
        for (auto& i : items) i.ctrl->set_time(rt, t);
        if (primary) primary->set_time(rt, t);
    }
    void set_phase(Runtime& rt, float p) override {
        for (auto& i : items) i.ctrl->set_phase(rt, p);
        if (primary) primary->set_phase(rt, p);
    }
    Ctrl* tag_leaf() override {
        if (!items.empty()) return items.front().ctrl->tag_leaf();
        return primary ? primary->tag_leaf() : nullptr;
    }
};

/* -------------------------------------------------------- speed scale */

/* SpeedScaleControllerAsset, the turn-in-place additive's clock.
 *
 * Named in the research repo (fcb6e19e), six fields, and the one asset the 1P chain uses
 * reads: child 0x0171C26D = 1p.additive.turninplace.cdb, speed 0x6A0747F4 =
 * 1p.additive.turninplace.speed.float, then 1, 0 and a false.
 *
 * It is the PlaybackRate controller's smaller sibling: advance the child at a rate a game
 * state gives. The two floats are read as the same bracket PlaybackRate's MinScale and
 * MaxScale are, which for a normalised turn speed of 0 to 1 behaves identically to reading
 * them as a scale and an offset - the shipped values are exactly 1 and 0. */
constexpr const char* kT_SpeedScale = "fcb6e19e-b186-2e52-43f5-18e40f164854";

struct SpeedScaleCtrl : Ctrl {
    std::unique_ptr<Ctrl> child;
    const Obj* speed = nullptr;
    float hi = 1.f, lo = 0.f;

    void init(Runtime& rt, const Obj* a) {
        asset = a;
        child = rt.make(resolve_ctrl_asset(rt, rt.g.resolve(a->f(0x0171c26du), a)));
        speed = rt.g.resolve(a->f(0x6a0747f4u), a);
        hi = (float)numv(a->f(0xb6798148u));
        lo = (float)numv(a->f(0x0d10ce55u));
        if (boolv(a->f(0xc570266eu)))
            rt.note("speed scale flag 0xC570266E set and not acted on: " + a->path);
    }
    bool is_additive() const override { return child && child->is_additive(); }
    void update(Runtime& rt, float dt, Pose& out) override {
        if (!child) { out = rt.bind; return; }
        float s = hi;
        if (speed) {
            bool ok = false;
            const float v = rt.st.as_float(rt.g, speed, ok);
            if (ok) s = v;
        }
        if (hi > lo) s = std::fmin(std::fmax(s, lo), hi);
        child->update(rt, dt * s, out);
        phase = child->phase; time = child->time;
        length = child->length; remaining = child->remaining;
    }
    void set_time(Runtime& rt, float t) override { if (child) child->set_time(rt, t); }
    void set_phase(Runtime& rt, float p) override { if (child) child->set_phase(rt, p); }
    Ctrl* tag_leaf() override { return child ? child->tag_leaf() : nullptr; }
};

/* --------------------------------------------------------- joint copy */

/* JointCopyControllerAsset, which is what aiming down sights IS.
 *
 * Named in the research repo (a4000c65), and its entries are JointAdjustEntryAsset
 * (381b1888, 23 fields, 0xC0 bytes). The repo's field rows give every offset AND every
 * field's own type, so nothing here is inferred from values:
 *
 *   0x30  0x8CF424E7  Vec4                      the translation operand
 *   0x40  0xBCE46C70  Quat                      the rotation operand
 *   0x60  0xEF68847E  ref                       a joint map template
 *   0x70  0x668F29AC  ref                       a dof set
 *   0x88  0x71F01F90  ref     Weight source
 *   0x98  0xF23F2474  JointAdjustOperation      Copy 0 Add 1 Set 2 Multiply 3
 *                                               Transform 4 Damp 5
 *   0x9C  0xBC485B17  JointAdjustType           Translation 0 Rotation 1 Scale 2
 *   0xA0  0x757A244F  JointAdjustSpace          Local 0 Model 1
 *   0xA4  0xDD98CBC4  JointCopyTranslationType  TrajectorySpace 0 ReferenceSpace 1
 *   0xAC  0x89B14DF8  Weight                    the constant
 *   0xB3  0x6E8C0B93  Enable
 *
 * The enumerator names come from the repo's enum value table, so the two assets the
 * first-person chain uses read straight off:
 *
 *   1p.weproot.zoompose.jointadjust    two entries over weaponroot.ds, both Set, Local:
 *                                      Rotation to the identity quaternion and
 *                                      Translation to the origin. It NEUTRALISES the
 *                                      weapon root, which is exactly what puts the sight
 *                                      where the camera is looking. This is ADS.
 *   1p.weproot.aimupdown.jointadjust   one entry, Add, Translation, Local: a 2 cm local
 *                                      offset (0, -0.02, -0.02) scaled by
 *                                      1p.loco.weproot.aimupdown.weight.float.
 *
 * WHICH JOINTS comes from the dof set, whose records already say whether each one is a
 * rotation or a translation slot. An entry that selects its joints through a joint map
 * TEMPLATE instead is reported rather than guessed: the template carries numeric ids and
 * nothing in the install names them, so picking bones for it would be invention. */
constexpr const char* kT_JointCopy = "a4000c65-7211-b2db-9e30-7c859b36b912";

/* A Vec4 or Quat operand, by the field names the repo confirms: X, Y, Z, W. */
void read_xyzw(const bf6::EbxValue* v, float* out)
{
    if (!v) return;
    out[0] = (float)numv(v->field(0x3901db14u));
    out[1] = (float)numv(v->field(0x42fc0f5eu));
    out[2] = (float)numv(v->field(0x32a99b9cu));
    out[3] = (float)numv(v->field(0x7c8062f2u));
}

struct JointCopyCtrl : Ctrl {
    enum { OpCopy = 0, OpAdd = 1, OpSet = 2, OpMultiply = 3, OpTransform = 4, OpDamp = 5 };
    enum { KindTranslation = 0, KindRotation = 1, KindScale = 2 };
    struct Entry {
        const Obj* e = nullptr;
        float v[4] = {0, 0, 0, 0};
        float q[4] = {0, 0, 0, 1};
        const Obj* weight_src = nullptr;
        float weight_const = 1.f;
        const Obj* enable_src = nullptr;
        bool enable_const = true;
        int op = OpCopy, kind = KindTranslation, space = 0;
        std::vector<int> bones;      /* the dof set's slots for this entry's kind */
        bool unresolved = false;     /* joints named only through a map template */
        bool warned_add = false;
    };
    std::unique_ptr<Ctrl> base;
    std::vector<Entry> entries;

    void init(Runtime& rt, const Obj* a) {
        asset = a;
        if (const Obj* b = resolve_ctrl_asset(rt, rt.g.resolve(a->f(0x81309ef7u), a)))
            base = rt.make(b);
        const bf6::EbxValue* es = a->f(0x1466a487u);
        if (!es) return;
        for (const auto& er : es->items) {
            const Obj* eo = rt.g.resolve(&er, a);
            if (!eo) continue;
            Entry en;
            en.e = eo;
            read_xyzw(eo->f(0x8cf424e7u), en.v);
            read_xyzw(eo->f(0xbce46c70u), en.q);
            en.weight_src = rt.g.resolve(eo->f(0x71f01f90u), eo);
            en.weight_const = (float)numv(eo->f(0x89b14df8u));
            en.enable_src = rt.g.resolve(eo->f(0x6e8c0b93u), eo);
            en.enable_const = boolv(eo->f(0x6e8c0b93u));
            en.op = (int)numv(eo->f(0xf23f2474u));
            en.kind = (int)numv(eo->f(0xbc485b17u));
            en.space = (int)numv(eo->f(0x757a244fu));
            const Obj* ds = rt.g.resolve(eo->f(0x668f29acu), eo);
            if (ds) {
                const char want = en.kind == KindRotation ? 'q' : 't';
                for (const auto& rec : ds_records(ds)) {
                    if (rec.second != want) continue;
                    auto it = rt.bone_index.find(rec.first);
                    if (it != rt.bone_index.end()) en.bones.push_back(it->second);
                }
            }
            if (en.bones.empty()) {
                en.unresolved = true;
                const Obj* jm = rt.g.resolve(eo->f(0xef68847eu), eo);
                rt.note("joint copy selects joints through a map template, which nothing "
                        "names, so the entry is skipped: "
                        + (jm ? jm->path : a->path));
            }
            if (en.space != 0)
                rt.note("joint copy in model space not implemented (local applied): " + a->path);
            if (en.kind == KindScale)
                rt.note("joint copy of scale not implemented (the pose carries none): " + a->path);
            if (en.op == OpMultiply || en.op == OpTransform || en.op == OpDamp)
                rt.note("joint copy operation " + std::to_string(en.op)
                        + " not implemented: " + a->path);
            entries.push_back(std::move(en));
        }
    }
    bool is_additive() const override { return base && base->is_additive(); }

    void update(Runtime& rt, float dt, Pose& out) override {
        /* THE BASE IS THE SOURCE TO COPY FROM, NEVER THE OUTPUT.
         *
         * 1p.weproot.aimupdown.jointadjust's base is l_3p_rifle_stand_idle_high
         * intensity_01 - a THIRD-PERSON idle clip. Writing that out as the layer's pose
         * put a 3P idle over the first-person body at full weight and flattened the
         * stance: measured, crouch went from 531.9 mm against its control to 5.2. A
         * JointCopy copies named joints out of that source into the pose it sits on, so
         * the output starts CLAIMING NOTHING and only the joints an entry writes become
         * valid. pose_blend skips the rest, which is how a layer contributes a few
         * joints without touching the body. */
        out.resize(rt.bind.q.size());
        Pose src;
        bool have_src = false;
        if (base) { base->update(rt, dt, src); have_src = src.q.size() == out.q.size(); }
        for (auto& en : entries) {
            if (en.unresolved || en.bones.empty()) continue;
            bool enabled = en.enable_const;
            if (en.enable_src) {
                bool ok = false;
                const bool b = rt.st.as_bool(rt.g, en.enable_src, ok);
                if (ok) enabled = b;
            }
            if (!enabled) continue;
            float w = en.weight_const;
            if (en.weight_src) {
                bool ok = false;
                const float f = rt.st.as_float(rt.g, en.weight_src, ok);
                if (ok) w = f;
            }
            if (w <= 0.f) continue;
            if (w > 1.f) w = 1.f;
            /* ADD NEEDS WHAT IS UNDERNEATH, and a layer's pose does not carry it: the
             * value it would add to lives in the composite this layer has not seen. So
             * an Add entry is reported rather than approximated against bind, which
             * would invent a pose. The two assets the 1P chain uses are unaffected -
             * zoompose Sets, and aimupdown's joints come from a map template that is
             * already skipped. */
            if (en.op == OpAdd) {
                if (!en.warned_add) {
                    en.warned_add = true;
                    rt.note("joint copy Add needs the pose underneath, which a layer does "
                            "not carry, so the entry is skipped: " + asset->path);
                }
                continue;
            }
            for (int b : en.bones) {
                if (b < 0 || (size_t)b >= out.q.size()) continue;
                /* Copy takes the joint out of the source; Set takes the operand. */
                const bool from_src = en.op == OpCopy && have_src;
                if (en.kind == KindRotation) {
                    if (from_src && !src.vq[(size_t)b]) continue;
                    const float* v = from_src ? src.q[(size_t)b].data() : en.q;
                    for (int c = 0; c < 4; ++c) out.q[(size_t)b][c] = v[c];
                    out.vq[(size_t)b] = 1;
                } else if (en.kind == KindTranslation) {
                    if (from_src && !src.vt[(size_t)b]) continue;
                    for (int c = 0; c < 3; ++c)
                        out.t[(size_t)b][c] = from_src ? src.t[(size_t)b][(size_t)c] : en.v[c];
                    out.vt[(size_t)b] = 1;
                }
            }
        }
    }
    void set_time(Runtime& rt, float t) override { if (base) base->set_time(rt, t); }
    void set_phase(Runtime& rt, float p) override { if (base) base->set_phase(rt, p); }
    Ctrl* tag_leaf() override { return base ? base->tag_leaf() : nullptr; }
};

/* ------------------------------------------------------- playback rate */

/* A CONTROLLER PLAYED AT A RATE THE GAME OWNS, and the reload's whole timeline.
 *
 * .prc assets, and the _(pbrate) wrappers, are this one type. The field meanings
 * are read off the shipped assets rather than guessed, because three of them differ
 * in exactly the interesting places:
 *
 *   1p.wep.reloadversion.prc  child wep.reloadversion.cdbchooser1
 *                             rate  fb.wep.reloadspeed.float
 *                             tick  wep.reloadcurrenttick.float
 *                             mode 0   range 0 .. 100
 *   1p.swim.loco.prc          rate  mm.swim.playbackrate.float
 *                             mode 0   range 0.75 .. 1.25
 *   1p.wep.deploy.prc         duration fb.wep.deploytime.float.sectic
 *                             mode 1   range 0 .. 100
 *
 * So 0x4e0c85a5 picks between a RATE (0) and a DURATION (1): a duration says how
 * long the child must take, which is a rate of its own length over that duration.
 * 0x1a4a3ee3 and 0x41fe1b9a bracket the result - a swim cycle may only run between
 * 0.75x and 1.25x - and 0xed10e827 is 60 on every one of them, the tick rate the
 * duration is expressed in.
 *
 * 0x717e6c9b IS THE GAME DRIVING THE CLOCK. wep.reloadcurrenttick.float is the
 * reload's own tick, owned by the weapon rather than the animation, and when it is
 * present the child is SEEKED to it instead of advanced. That is what makes a reload
 * finish exactly when the magazine is in, however long the clip is.
 *
 * WHY THIS ONE MATTERS MOST of the five missing controller types: it is the base of
 * 1p.wep.reload.slc, so without it the reload node has no pose at all. Measured, the
 * upper body transitioned to 1p.ubo.reload.node correctly and moved 0.0 mm.
 *
 * A RATE OF ZERO FREEZES THE CHILD, and every one of these rate states is authored
 * to default 0, so the driver has to supply them exactly as the game does. Inventing
 * a fallback of 1 here would hide a state nobody is driving. */
constexpr const char* kT_PlaybackRate = "4ac8e55b-83b5-afac-c2f9-6b35f5c2390c";

struct PlaybackRateCtrl : Ctrl {
    std::unique_ptr<Ctrl> child;
    const Obj* rate_src = nullptr;    /* 0x65d592db */
    const Obj* dur_src = nullptr;     /* 0x0d513bb6, in the mode that uses it */
    const Obj* tick_src = nullptr;    /* 0x717e6c9b, the game's own clock */
    int mode = 0;                     /* 0x4e0c85a5: 0 rate, 1 duration */
    float lo = 0.f, hi = 0.f;         /* MinScale 0x1a4a3ee3 .. MaxScale 0x41fe1b9a */
    float scale = 1.f;                /* TimeScale 0x691d9180 */
    float tick_rate = 60.f;           /* 0xed10e827, 60 on every shipped asset */

    void init(Runtime& rt, const Obj* a) {
        asset = a;
        child = rt.make(resolve_ctrl_asset(rt, rt.g.resolve(a->f(0x7cdb5360u), a)));
        rate_src = rt.g.resolve(a->f(0x65d592dbu), a);
        dur_src = rt.g.resolve(a->f(0x0d513bb6u), a);
        tick_src = rt.g.resolve(a->f(0x717e6c9bu), a);
        mode = (int)numv(a->f(0x4e0c85a5u));
        lo = (float)numv(a->f(0x1a4a3ee3u));
        hi = (float)numv(a->f(0x41fe1b9au));
        /* TIMESCALE, named in the research repo's field table rather than guessed - it
         * reads 1 on every shipped .prc, which is why it looked like a flag. MinScale
         * and MaxScale are named there too, and they are exactly the bracket this
         * clamps to, so the shape read off the three sample assets was right. */
        scale = (float)numv(a->f(0x691d9180u));
        if (scale == 0.f) scale = 1.f;
        tick_rate = (float)numv(a->f(0xed10e827u));
        if (tick_rate <= 0.f) tick_rate = 60.f;
        if (a->f(0x050440a3u) && a->f(0x050440a3u)->kind != bf6::EbxValue::Kind::Null)
            rt.note("playback rate second output not read: " + a->path);
        if ((int)numv(a->f(0x38e4f670u)) != 0)
            rt.note("playback rate flag 0x38e4f670 set and not acted on: " + a->path);
    }
    bool is_additive() const override { return child && child->is_additive(); }

    float read(Runtime& rt, const Obj* s, bool& ok) const {
        ok = false;
        if (!s) return 0.f;
        return rt.st.as_float(rt.g, s, ok);
    }

    void update(Runtime& rt, float dt, Pose& out) override {
        if (!child) { out = rt.bind; return; }
        float rate = 1.f;
        bool ok = false;
        if (mode == 1) {
            /* A duration: the child has to cover its own length in that long. */
            const float d = read(rt, dur_src, ok);
            if (ok && d > 0.f && child->length > 0.f) rate = child->length / d;
        } else {
            const float r = read(rt, rate_src, ok);
            if (ok) rate = r;
        }
        rate *= scale;
        if (hi > lo) rate = std::fmin(std::fmax(rate, lo), hi);
        if (tick_src) {
            bool tok = false;
            const float t = read(rt, tick_src, tok);
            if (tok) {
                child->set_time(rt, t);
                child->update(rt, 0.f, out);
                phase = child->phase; time = child->time;
                length = child->length; remaining = child->remaining;
                return;
            }
        }
        child->update(rt, dt * rate, out);
        phase = child->phase; time = child->time;
        length = child->length; remaining = child->remaining;
    }
    void set_time(Runtime& rt, float t) override { if (child) child->set_time(rt, t); }
    void set_phase(Runtime& rt, float p) override { if (child) child->set_phase(rt, p); }
    Ctrl* tag_leaf() override { return child ? child->tag_leaf() : nullptr; }
};

struct SeqFlowCtrl : Ctrl {
    struct State {
        std::unique_ptr<ActorCtrl> actor;
        float start = 0.f;       /* where it lands once the states are laid end to end */
        float len = 0.f;         /* 0xed10e827 */
        bool hold = false;       /* 0x7e3f6d2e */
    };
    std::vector<State> states;

    void init(Runtime& rt, const Obj* a) {
        asset = a;
        looping = false;
        if (has_nonnull(a->f(0xa562b576u)) || has_nonnull(a->f(0xcb73fd98u)))
            rt.note("sequence flow container transitions not implemented, states played "
                    "in order: " + a->path);
        const Obj* flow = pick_member(rt, a, "sequence flow");
        if (!flow) return;
        if (flow->type != kT_Flow)
            rt.note("sequence flow member is not a flow (" + flow->type + "): " + a->path);
        if (has_nonnull(flow->f(0x21a22ce4u)))
            rt.note("sequence flow transitions not implemented, states played in order: " + a->path);
        const bf6::EbxValue* ss = flow->f(0xf8c3f7c5u);
        static const std::vector<bf6::EbxValue> kNone;
        float at = 0.f;
        for (const auto& sr : ss ? ss->items : kNone) {
            const Obj* s = rt.g.resolve(&sr, flow);
            if (!s) continue;
            State S;
            S.len = (float)numv(s->f(0xed10e827u));
            S.hold = boolv(s->f(0x7e3f6d2eu));
            /* WHAT THE TRACK ACTUALLY SAYS, now that the type is named.
             *
             * A state is a FlowTrackControllerAsset (b434df78): 0x7E3F6D2E is a bool at
             * offset 0x78 whose name the tables still do not carry, and its neighbours DO
             * - 0xF59CF04D is SequenceFlowTrackType (SingleActor 0, MultiActor 1,
             * PartialBody 2) and 0xFDFD38C6 is StartOnTypes (FirstEnable 0, EveryEnable 1,
             * TimeZero 2).
             *
             * On adtv.1p.rifle.stand.suppression.seqflow all three tracks are SingleActor
             * and FirstEnable, of 120, 63 and 52 ticks, and only the FIRST carries the
             * bool. That is consistent with either "loop this one" or "hold its last
             * frame", and the data cannot separate them - so it is reported with
             * everything known rather than guessed at, and the tracks are played in
             * order. */
            if (S.hold)
                rt.note("sequence flow track bool 0x7E3F6D2E (FlowTrackControllerAsset "
                        "+0x78, unnamed; loop or hold) not acted on, tracks played in "
                        "order: " + a->path);
            if ((int)numv(s->f(0xfdfd38c6u)) != 0)
                rt.note("sequence flow track StartOnTypes "
                        + std::to_string((int)numv(s->f(0xfdfd38c6u)))
                        + " not acted on (FirstEnable assumed): " + a->path);
            if ((int)numv(s->f(0xf59cf04du)) != 0)
                rt.note("sequence flow track type "
                        + std::to_string((int)numv(s->f(0xf59cf04du)))
                        + " (MultiActor or PartialBody) not implemented: " + a->path);
            const int mode = (int)numv(s->f(0x787a258eu));
            if (mode != 0)
                rt.note("sequence flow state blend mode " + std::to_string(mode) +
                        " not implemented, blended: " + a->path);
            S.actor = std::make_unique<ActorCtrl>();
            S.actor->init(rt, rt.g.resolve(s->f(0x564beb5fu), s), a);
            if (S.actor->empty()) continue;
            if (S.len <= 0.f) S.len = S.actor->length;
            S.start = at;
            at += S.len;
            states.push_back(std::move(S));
        }
        if (states.empty()) { rt.note("sequence flow has no state with a clip: " + a->path); return; }
        length = at > 0.f ? at : 1.f;
        time = 0.f;
        phase = 0.f;
        remaining = length;
    }
    void update(Runtime& rt, float dt, Pose& out) override {
        if (states.empty()) { out = rt.bind; return; }
        wrapped = false;
        time = std::fmin(time + dt, length);
        phase = normalise(length > 1e-4f ? time / length : 0.f);
        remaining = std::fmax(length - time, 0.f);
        State* cur = &states.front();
        for (State& S : states)
            if (time >= S.start) cur = &S;
        /* A state stays on its last frame rather than running past its end. */
        const float local = std::fmin(time - cur->start, cur->len);
        cur->actor->set_time(rt, local);
        cur->actor->write(rt, out);
    }
    void set_time(Runtime& rt, float t) override {
        (void)rt;
        time = std::fmin(std::fmax(t, 0.f), length);
        remaining = std::fmax(length - time, 0.f);
        phase = length > 1e-4f ? time / length : 0.f;
    }
    void set_phase(Runtime& rt, float p) override { set_time(rt, length * normalise(p)); }
    Ctrl* tag_leaf() override {
        if (states.empty()) return nullptr;
        State* cur = &states.front();
        for (State& S : states)
            if (time >= S.start) cur = &S;
        return cur->actor->tag_leaf();
    }
};

/* Other controller types are added here as their specs land. Until then an
 * unknown type is NAMED in notes and yields the bind pose, so a partially
 * implemented graph shows what it is missing instead of pretending. */
struct MissingCtrl : Ctrl {
    /* A controller we cannot evaluate should leave the pose it sits on alone
     * rather than replace it with bind. */
    void update(Runtime& rt, float, Pose& out) override {
        if (rt.have_parent && rt.parent.q.size() == rt.bind.q.size()) out = rt.parent;
        else out = rt.bind;
    }
    void set_time(Runtime&, float t) override { time = t; }
    void set_phase(Runtime&, float p) override { phase = p; }
};

std::unique_ptr<Ctrl> Runtime::make(const Obj* a)
{
    struct DepthGuard { int& d; explicit DepthGuard(int& x) : d(x) { ++d; } ~DepthGuard() { --d; } } guard(make_depth);
    if (!a) return nullptr;
    if (a->type == kT_Clip) { auto c = std::make_unique<ClipCtrl>(); c->init(*this, a); return c; }
    if (a->type == kT_Phase) { auto c = std::make_unique<PhaseCtrl>(); c->init(*this, a); return c; }
    if (a->type == kT_SuperLayers) { auto c = std::make_unique<SuperLayersCtrl>(); c->init(*this, a); return c; }
    if (a->type == kT_StateFlow) { auto c = std::make_unique<StateFlowCtrl>(); c->init(*this, a); return c; }
    if (a->type == kT_CdbAsset) {
        /* a ContextDatabaseAsset where a controller is expected: its entry */
        const Obj* e = resolve_ctrl_asset(*this, a);
        return e ? make(e) : nullptr;
    }
    if (a->type == kT_Chooser) {
        /* factory FUN_1409bece0: no dynamic flag = static choice at creation */
        const bool dyn = boolv(a->f(0x66fdcb25u)) || boolv(a->f(0xa0f5b40bu)) || boolv(a->f(0xe25a9213u)) || boolv(a->f(0x7e3f6d2eu));
        if (dyn) { auto c = std::make_unique<ChooserCtrl>(); c->init(*this, a); return c; }
        std::vector<const Obj*> es;
        if (const bf6::EbxValue* l = a->f(0xc7166a0cu)) for (const auto& e : l->items) es.push_back(g.resolve(&e, a));
        if (es.empty()) { note("empty chooser: " + a->path); auto m = std::make_unique<MissingCtrl>(); m->asset = a; return m; }
        bool ok = true;
        int idx = signal_choose(*this, g.resolve(a->f(0xb888be6fu), a), es, -1, ok);
        if (!ok) note("chooser input not evaluable: " + a->path);
        if (idx < 0 || idx >= (int)es.size()) idx = (int)es.size() - 1;
        return make(es[(size_t)idx] ? g.resolve(es[(size_t)idx]->f(0x81309ef7u), es[(size_t)idx]) : nullptr);
    }
    if (a->type == kT_CdbChooser) {
        /* factory FUN_1408ac130: no dynamic flag = static lookup at creation */
        const bool dyn = boolv(a->f(0x66fdcb25u)) || boolv(a->f(0xa0f5b40bu)) || boolv(a->f(0x7e3f6d2eu));
        if (dyn) { auto c = std::make_unique<CdbChooserCtrl>(); c->init(*this, a); return c; }
        const std::string e = cdb_lookup(*this, g.resolve(a->f(0x37ee8be9u), a));
        return e.empty() ? nullptr : make(g.root(e));
    }
    if (a->type == kT_CdbWrapper) {
        /* FUN_1408ac770: a one-shot lookup at creation. Its four optional
         * inputs/outputs are null in all 266 shipped wrappers. */
        const std::string e = cdb_lookup(*this, g.resolve(a->f(0x37ee8be9u), a));
        return e.empty() ? nullptr : make(g.root(e));
    }
    if (a->type == kT_MMLayers) {
        auto c = std::make_unique<MMLayersCtrl>();
        c->init(*this, a);
        return c;
    }
    if (a->type == kT_SignalBlendSpace) {
        auto c = std::make_unique<SignalBlendSpaceCtrl>();
        c->init(*this, a);
        return c;
    }
    if (a->type == kT_Sequence) {
        auto c = std::make_unique<SequenceCtrl>();
        c->init(*this, a);
        return c;
    }
    if (a->type == kT_SeqFlow) {
        auto c = std::make_unique<SeqFlowCtrl>();
        c->init(*this, a);
        return c;
    }
    if (a->type == kT_BlendSpaceAsset) {
        auto c = std::make_unique<BlendSpaceCtrl>();
        c->init(*this, a);
        return c;
    }
    if (a->type == kT_PlaybackRate) {
        auto c = std::make_unique<PlaybackRateCtrl>();
        c->init(*this, a);
        return c;
    }
    if (a->type == kT_PoseState) {
        auto c = std::make_unique<PoseStateCtrl>();
        c->init(*this, a);
        return c;
    }
    if (a->type == kT_JointCopy) {
        auto c = std::make_unique<JointCopyCtrl>();
        c->init(*this, a);
        return c;
    }
    if (a->type == kT_SpeedScale) {
        auto c = std::make_unique<SpeedScaleCtrl>();
        c->init(*this, a);
        return c;
    }
    if (a->type == kT_SignalBlendArray) {
        auto c = std::make_unique<SignalBlendArrayCtrl>();
        c->init(*this, a);
        return c;
    }
    if (a->type == kT_BlendArray) {
        /* The array on its own, used where a controller is expected: the same walk with
         * no signal, so its first item plays. */
        auto c = std::make_unique<SignalBlendArrayCtrl>();
        c->init(*this, a);
        return c;
    }
    /* SAY WHAT IT IS, now that the research repo names them. A raw GUID in a note costs a
     * lookup every time it is read, and two of these are deliberate rather than pending:
     *
     *   BoneAlignChainAsset      an IK chain: 40 fields, two Vec3s, a Vec2Simd, twenty
     *                            references and fourteen floats. A solver cannot be
     *                            written honestly from field types alone, so it stays a
     *                            pass-through until its function is read out of the exe.
     *                            Used by the prone crawl and the aim-down pose layer.
     *   NetworkProxyControllerAsset  replays another player's animation over the network.
     *                            Nothing for a local preview to do.
     */
    std::string what = a->type;
    if (a->type == "324ea9e1-2aa3-e686-8dc1-250f5fc9298f")
        what = "BoneAlignChainAsset (an IK chain; deliberately a pass-through)";
    else if (a->type == "c9426437-1cd8-9dda-9bf3-0f271e25bfff")
        what = "NetworkProxyControllerAsset (network replay; nothing to do locally)";
    note("controller type not implemented: " + what + " (" + a->path + ")");
    auto m = std::make_unique<MissingCtrl>();
    m->asset = a;
    return m;
}

}  // namespace bf6ant

/* ------------------------------------------------------------ C API --- */

struct AntRuntimeHandle {
    std::unique_ptr<bf6ant::Runtime> rt;
    std::unique_ptr<bf6ant::Ctrl> root;
    const bf6ant::Obj* root_obj = nullptr;   /* built into `root` on the first update */
    bf6ant::Pose pose;
    std::string root_path;
};

void bf6__antrt_delete(void* p) { delete (AntRuntimeHandle*)p; }

bf6_ant_runtime* bf6_ant_runtime_create(bf6_ctx* c, const char* root_asset, const char* rig,
                                        const char* skeleton, char* err, int err_len)
{
    auto fail = [&](const std::string& why) -> bf6_ant_runtime* {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", why.c_str());
        return nullptr;
    };
    if (!c || !root_asset || !rig || !skeleton) return fail("missing argument");
    auto h = std::make_unique<AntRuntimeHandle>();
    h->rt = std::make_unique<bf6ant::Runtime>(c);
    bf6ant::Runtime& rt = *h->rt;
    rt.rig = rig; rt.ske = skeleton;
    bf6_skeleton* s = bf6_skeleton_compose(c, skeleton, nullptr);
    if (!s || s->bone_count < 1) { if (s) bf6_free(c, s); return fail(std::string("skeleton: ") + skeleton); }
    rt.bones = s->bone_count;
    rt.bind.resize((size_t)s->bone_count);
    for (int i = 0; i < s->bone_count; ++i) {
        float q[4];
        bf6ant::quat_from_rows(s->bones[i].local, q);
        rt.bind.q[(size_t)i] = { q[0], q[1], q[2], q[3] };
        rt.bind.t[(size_t)i] = { s->bones[i].local[9], s->bones[i].local[10], s->bones[i].local[11] };
        if (s->bones[i].name) rt.bone_index[s->bones[i].name] = i;
    }
    bf6_free(c, s);
    const bf6ant::Obj* root = rt.g.root(root_asset);
    if (!root) return fail("root asset: " + rt.g.error());
    /* THE GRAPH IS BUILT ON THE FIRST UPDATE, not here. Building a controller
     * runs its lookups (which stance, which weapon pose), and the game builds
     * a soldier's graph with the soldier's states already live. Built here, a
     * caller's weapon would arrive one step late: every lookup would first pick
     * for the authored defaults - a grenade - and then crossfade away. */
    h->root_obj = root;
    h->root_path = root_asset;
    h->pose = rt.bind;
    AntRuntimeHandle* out = h.release();
    c->handles[out] = bf6_ctx::HK_ANTRT;
    return reinterpret_cast<bf6_ant_runtime*>(out);
}

void bf6_ant_runtime_set_bool(bf6_ant_runtime* r, const char* s, int v)
{ if (r && s) reinterpret_cast<AntRuntimeHandle*>(r)->rt->st.set_bool(s, v != 0); }
void bf6_ant_runtime_set_float(bf6_ant_runtime* r, const char* s, float v)
{ if (r && s) reinterpret_cast<AntRuntimeHandle*>(r)->rt->st.set_float(s, v); }
void bf6_ant_runtime_set_int(bf6_ant_runtime* r, const char* s, int32_t v)
{ if (r && s) reinterpret_cast<AntRuntimeHandle*>(r)->rt->st.set_int(s, v); }

/* THE POSE A LAYER SITS ON, 7 floats per bone: quaternion x,y,z,w then the
 * translation. A quaternion rather than the 3x4 rows the pose OUTPUT uses, so a
 * caller on the other side of the boundary has no matrix convention to get wrong.
 * Null or a bone count of 0 clears it. */
void bf6_ant_runtime_set_parent_pose(bf6_ant_runtime* r, const float* qt, int bones)
{
    auto* h = reinterpret_cast<AntRuntimeHandle*>(r);
    if (!h || !h->rt) return;
    if (!qt || bones <= 0) { h->rt->have_parent = false; return; }
    h->rt->parent = h->rt->bind;
    const int n = (int)h->rt->bind.q.size();
    for (int i = 0; i < n && i < bones; ++i) {
        const float* p = qt + (size_t)i * 7;
        h->rt->parent.q[(size_t)i] = {p[0], p[1], p[2], p[3]};
        h->rt->parent.t[(size_t)i] = {p[4], p[5], p[6]};
        h->rt->parent.vq[(size_t)i] = 1;
        h->rt->parent.vt[(size_t)i] = 1;
    }
    h->rt->have_parent = true;
}

void bf6_ant_runtime_set_weapon(bf6_ant_runtime* r, int32_t specific_weapon, int32_t weapon_type)
{
    if (!r) return;
    bf6ant::States& st = reinterpret_cast<AntRuntimeHandle*>(r)->rt->st;
    st.set_int("animations/glacier/global/gamestates/fb.wep.specificweapon.enumgs", specific_weapon);
    st.set_bool("animations/glacier/global/gamestates/fb.wep.isdualwielding.bool", false);
    /* not dual-wielding: the program's else-branch copies the live values */
    st.set_int("animations/glacier/global/gamestates/dualwield.specificweapon.cached.enumgs", specific_weapon);
    if (weapon_type >= 0) {
        st.set_int("animations/common/fb.weapontype.enumgs", weapon_type);
        st.set_int("animations/glacier/global/gamestates/dualwield.weapontype.cached.enumgs", weapon_type);
    }
}

int bf6_ant_resolve_for_weapon(bf6_ctx* c, const char* asset, int32_t specific_weapon,
                               int32_t weapon_type, char* out, int out_len)
{
    std::string s;
    if (c && asset) {
        AntRuntimeHandle h;
        h.rt = std::make_unique<bf6ant::Runtime>(c);
        bf6_ant_runtime_set_weapon(reinterpret_cast<bf6_ant_runtime*>(&h), specific_weapon, weapon_type);
        if (const bf6ant::Obj* a = h.rt->g.root(asset))
            if (const bf6ant::Obj* leaf = bf6ant::resolve_ctrl_asset(*h.rt, a)) s = leaf->path;
    }
    if (out && out_len > 0) std::snprintf(out, (size_t)out_len, "%s", s.c_str());
    return (int)s.size() + 1;
}

int bf6_ant_runtime_update(bf6_ant_runtime* r, float seconds)
{
    auto* h = reinterpret_cast<AntRuntimeHandle*>(r);
    if (!h) return 0;
    if (!h->root && h->root_obj) {
        h->root = h->rt->make(h->root_obj);
        h->root_obj = nullptr;
        if (!h->root) h->rt->note("root asset has no controller: " + h->root_path);
    }
    if (!h->root) return 0;
    bf6ant::Pose p;
    h->root->update(*h->rt, seconds * bf6ant::kTicksPerSecond, p);
    if (p.q.size() != h->rt->bind.q.size()) p = h->rt->bind;
    h->pose = p;
    h->rt->first_update = false;
    return 1;
}

int bf6_ant_runtime_pose(const bf6_ant_runtime* r, float* out, int bone_max)
{
    const auto* h = reinterpret_cast<const AntRuntimeHandle*>(r);
    if (!h) return 0;
    const int n = (int)h->pose.q.size();
    if (out)
        for (int i = 0; i < n && i < bone_max; ++i) {
            float* m = out + (size_t)i * 12;
            bf6ant::rows_from_quat(h->pose.q[(size_t)i].data(), m);
            m[9] = h->pose.t[(size_t)i][0]; m[10] = h->pose.t[(size_t)i][1]; m[11] = h->pose.t[(size_t)i][2];
        }
    return n;
}

int bf6_ant_runtime_pose_valid(const bf6_ant_runtime* r, uint8_t* rot, uint8_t* tr, int bone_max)
{
    const auto* h = reinterpret_cast<const AntRuntimeHandle*>(r);
    if (!h) return 0;
    const int n = (int)h->pose.q.size();
    for (int i = 0; i < n && i < bone_max; ++i) {
        if (rot) rot[i] = i < (int)h->pose.vq.size() ? h->pose.vq[(size_t)i] : 0;
        if (tr) tr[i] = i < (int)h->pose.vt.size() ? h->pose.vt[(size_t)i] : 0;
    }
    return n;
}

int bf6_ant_runtime_notes(const bf6_ant_runtime* r, char* out, int out_len)
{
    const auto* h = reinterpret_cast<const AntRuntimeHandle*>(r);
    if (!h) return 0;
    std::string s;
    for (const auto& n : h->rt->notes) s += n + "\n";
    for (const auto& u : h->rt->st.unknown()) s += "game state not evaluable: " + u + "\n";
    if (const auto* sf = dynamic_cast<const bf6ant::StateFlowCtrl*>(h->root.get()))
        s += std::string("state: ") + (sf->node && sf->node->asset ? sf->node->asset->path : std::string("(none)")) + "\n";
    if (out && out_len > 0) std::snprintf(out, (size_t)out_len, "%s", s.c_str());
    return (int)s.size() + 1;
}

int bf6_ant_runtime_get_bool(const bf6_ant_runtime* r, const char* state, int* ok)
{
    if (ok) *ok = 0;
    const auto* h = reinterpret_cast<const AntRuntimeHandle*>(r);
    if (!h || !state) return 0;
    bf6ant::Runtime& rt = *h->rt;
    bool k = false;
    const bool v = rt.st.as_bool(rt.g, rt.g.root(state), k);
    if (ok) *ok = k ? 1 : 0;
    return v ? 1 : 0;
}

int bf6_ant_runtime_node(const bf6_ant_runtime* r, char* out, int out_len)
{
    const auto* h = reinterpret_cast<const AntRuntimeHandle*>(r);
    std::string s;
    if (h) if (const auto* sf = dynamic_cast<const bf6ant::StateFlowCtrl*>(h->root.get()))
        if (sf->node && sf->node->asset) s = sf->node->asset->path;
    if (out && out_len > 0) std::snprintf(out, (size_t)out_len, "%s", s.c_str());
    return (int)s.size() + 1;
}

int bf6_ant_runtime_finished(const bf6_ant_runtime* r)
{
    const auto* h = reinterpret_cast<const AntRuntimeHandle*>(r);
    if (!h) return 0;
    const auto* sf = dynamic_cast<const bf6ant::StateFlowCtrl*>(h->root.get());
    return sf && sf->finished ? 1 : 0;
}
