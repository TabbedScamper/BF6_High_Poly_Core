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

    void init(Runtime& rt, const Obj* a) {
        asset = a;
        rate = (float)numv(a->f(0x691d9180u));
        offset = (float)numv(a->f(0x2f2c4cabu));
        loop = boolv(a->f(0xd5593e48u));
        reverse = (int)numv(a->f(0x72e0ac4au));
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
int signal_choose(Runtime& rt, const Obj* policy, const std::vector<const Obj*>& entries, int current, bool& ok)
{
    ok = true;
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
            rt.note("loop-wrap pose join (FUN_1407a5e40) not read: post-wrap pose used");
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
            rt.note("loop-wrap pose join (FUN_1407a5e40) not read: post-wrap pose used");
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

/* FUN_14079af40: build a BlendMaskList's per-DOF weights. A set named by a
 * BlendMaskAsset takes that asset's per-record weights; otherwise a set in the
 * list takes its whole-set weight (missing = 0); every other DOF is 0. Sets
 * are matched by identity - the same DofSetAsset - which is what the engine's
 * hdr[7] comparison identifies. Per-DOF and per-set weight SOURCES (dynamic)
 * are named in notes and their constants used. */
const Mask* build_mask(Runtime& rt, const Obj* list)
{
    if (!list) return nullptr;
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
            if (bm->f(0x202e54f7u) && !bm->f(0x202e54f7u)->items.empty())
                rt.note("blend mask per-DOF weight sources not evaluated (constants used): " + bm->path);
            for (size_t i = 0; i < recs.size(); ++i)
                put(recs[i], ws && i < ws->items.size() ? (float)numv(&ws->items[i]) : 0.f);
            done[ds->path] = true;
        }
    const bf6::EbxValue* sets = list->f(0x4c23c8ceu);
    const bf6::EbxValue* sw = list->f(0x42c8b257u);
    if (list->f(0x6d77fca0u) && !list->f(0x6d77fca0u)->items.empty())
        rt.note("blend mask per-set weight sources not evaluated (constants used): " + list->path);
    if (sets) for (size_t j = 0; j < sets->items.size(); ++j) {
        const Obj* ds = rt.g.resolve(&sets->items[j], list);
        if (!ds || done.count(ds->path)) continue;
        const float w = sw && j < sw->items.size() ? (float)numv(&sw->items[j]) : 0.f;
        for (const auto& rec : ds_records(ds)) put(rec, w);
    }
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

struct SuperLayersCtrl : Ctrl {
    struct Inst { std::unique_ptr<Ctrl> ctrl; const Obj* src = nullptr; float w = 0.f, T = 0.f, delay = 0.f; };
    struct Layer {
        const Obj* e = nullptr;
        std::vector<Inst> inst;        /* newest first */
        float r = 0.f, remain = 0.f, W = 1.f, last_w = 1.f;
        bool dir_out = false, prev_trig = false;
        const Mask* mask = nullptr;
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
        for (auto& L : layers) if (L.e) {
            if ((int)numv(L.e->f(0x787a258eu)) >= 2 && (int)numv(L.e->f(0x787a258eu)) <= 3)
                rt.note("ModelSpace layer blend (never ships) not implemented: " + a->path);
            if (L.e->f(0x899b420cu) && L.e->f(0x899b420cu)->kind != bf6::EbxValue::Kind::Null)
                rt.note("additive-capture layer (+0xa8) not implemented: " + a->path);
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
        if (base) { base->update(rt, dt, out); collapse_finished(base); }
        else out = rt.bind;
        for (auto& L : layers) {
            if (!L.e) continue;
            const Obj* e = L.e;
            bool enabled = srcb(rt, e, 0x6e8c0b93u, 0x18daea78u);
            const float weight = srcf(rt, e, 0x71f01f90u, 0x89b14df8u);
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
                        L.mask = build_mask(rt, rt.g.resolve(e->f(0x795e0190u), e));
                    } else L.inst.front().T = tX;
                    const float off = srcf(rt, e, 0x3f3bf41au, 0xdb29a4dcu);
                    if (off > 0.f && n.ctrl) n.ctrl->set_time(rt, n.ctrl->time + off);
                    n.w = (first || L.inst.empty()) ? 1.f : 0.f;
                    n.T = tX;
                    n.delay = std::fmin(off, 0.f);
                    if (n.ctrl) L.inst.insert(L.inst.begin(), std::move(n));
                }
                if (!L.mask) L.mask = build_mask(rt, rt.g.resolve(e->f(0x795e0190u), e));
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
            if (!have || std::fabs(L.W) <= kLayerEps) continue;
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
constexpr float kHuge = 3.4028235e38f;   /* DAT_149319c64, the "never ends" tag window end */

/* A tag set evaluated against a running time (research spec_stateflow 3:
 * DefaultTagCollection, per-tag state FUN_1408cdff0, APPLY FUN_1408ccac0,
 * LEAVE FUN_1408cc210). Node tag sets run on node time with length BIG.
 * BoolGameStateTagAsset is the tag kind implemented (Save FUN_1407e3760,
 * Apply FUN_1407e37d0, Reset FUN_1407e3870); others are named in notes. */
struct TagSet {
    struct T { const Obj* a = nullptr; uint8_t s = 0; int16_t c = 0; bool store = false; bool ever = false; };
    std::vector<T> tags;
    float prev = 0.f, now = 0.f, length = kNeverEnds;
    bool jumped = true;          /* Restart (FUN_1408cc170) sets it; the first evaluation sees it */
    bool looped = false;

    void build(Runtime& rt, const Obj* set) {
        std::vector<const Obj*> all;
        tag_set_tags(rt, set, all);
        for (const Obj* a : all) {
            if (a->type == kT_BranchOutPointTag) continue;          /* windows, read by the transition test */
            if (a->type != kT_BoolGSTag) { rt.note("tag kind not implemented: " + a->type + " (" + a->path + ")"); continue; }
            T t; t.a = a; tags.push_back(t);
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
        if (mode == 0) return;
        const Obj* gs = rt.g.resolve(t.a->f(0x6b01e323u), t.a);
        bool ok = false; t.store = gs ? rt.st.as_bool(rt.g, gs, ok) : false;
    }
    void apply(Runtime& rt, T& t) {
        const int mode = (int)numv(t.a->f(0xc755b26cu));
        const Obj* gs = rt.g.resolve(t.a->f(0x6b01e323u), t.a);
        if (!gs) return;
        if (mode == 1) rt.st.set_bool(gs->path, boolv(t.a->f(0x8cf424e7u)));
        else if (mode == 2) rt.st.set_bool(gs->path, !t.store);
    }
    void reset(Runtime& rt, T& t) {
        const int mode = (int)numv(t.a->f(0xc755b26cu));
        const int rs = (int)numv(t.a->f(0xfeac4b79u));
        const Obj* gs = rt.g.resolve(t.a->f(0x6b01e323u), t.a);
        if (mode == 0 || rs == 0 || !gs) return;
        if (rs == 1) rt.st.clear(gs->path);                     /* the state's authored default */
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
    void update(Runtime& rt, float, Pose& out) override { out = rt.bind; std::fill(out.vq.begin(), out.vq.end(), 0); std::fill(out.vt.begin(), out.vt.end(), 0); }
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

    void init(Runtime& rt, const Obj* a) {
        asset = a;
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
        rt.note("transparent node: the engine passes the parent's pose through; a root state flow has none");
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
    float best = kNeverEnds;
    int result = 0xff;
    for (int i = 0; i < (int)entries.size() && i != 0xff; i = entries[(size_t)i].next) {
        const Entry& e = entries[(size_t)i];
        if (entry_state.size() != entries.size()) entry_state.assign(entries.size(), 0);
        bool pass = true;
        if (entry_state[(size_t)i] != 4) pass = conds(e.must_true, true) && conds(e.must_false, false);
        int r = 0xff;
        if (pass) {
            if (e.gate != 0xff) rt.note("gated transition (gate manager) not implemented: " + asset->path);
            else if (e.cont != 0xff) rt.note("transition continuation through a transparent node not implemented: " + asset->path);
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
void StateFlowCtrl::fire(Runtime& rt)
{
    if (!node || node->chosen == 0xff) return;
    NodeCtrl::Entry e = node->entries[(size_t)node->chosen];
    const Obj* T = e.tr;
    const Obj* to = e.to ? e.to : (T ? rt.g.resolve(T->f(0x7decae88u), T) : nullptr);
    if (!to) { rt.note("transition with no target node: " + node->asset->path); node->chosen = 0xff; return; }
    if (T) {
        const bf6::EbxValue* c = T->f(0xa80bcb57u);
        if (c && !c->items.empty()) rt.note("transition trigger consumption (interface 0x28185be6) not implemented: " + T->path);
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

/* Other controller types are added here as their specs land. Until then an
 * unknown type is NAMED in notes and yields the bind pose, so a partially
 * implemented graph shows what it is missing instead of pretending. */
struct MissingCtrl : Ctrl {
    void update(Runtime& rt, float, Pose& out) override { out = rt.bind; }
    void set_time(Runtime&, float t) override { time = t; }
    void set_phase(Runtime&, float p) override { phase = p; }
};

std::unique_ptr<Ctrl> Runtime::make(const Obj* a)
{
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
    note("controller type not implemented: " + a->type + " (" + a->path + ")");
    auto m = std::make_unique<MissingCtrl>();
    m->asset = a;
    return m;
}

}  // namespace bf6ant

/* ------------------------------------------------------------ C API --- */

struct AntRuntimeHandle {
    std::unique_ptr<bf6ant::Runtime> rt;
    std::unique_ptr<bf6ant::Ctrl> root;
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
    h->root = rt.make(root);
    if (!h->root) return fail("root asset has no controller");
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

int bf6_ant_runtime_update(bf6_ant_runtime* r, float seconds)
{
    auto* h = reinterpret_cast<AntRuntimeHandle*>(r);
    if (!h || !h->root) return 0;
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
