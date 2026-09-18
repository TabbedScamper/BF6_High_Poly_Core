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
};

/* --------------------------------------------------------- transitions */

constexpr const char* kT_Bezier     = "dc57fd9d-c987-9d4b-026f-b8fa06240f51";
constexpr const char* kT_Chooser    = "89acb7c0-1ead-f10a-4b34-c2b2400f0691";
constexpr const char* kT_CdbChooser = "534025c7-d78e-a2eb-4a58-144714439738";
constexpr const char* kT_CdbWrapper = "ef37bda2-e5a0-6dfa-1c60-91a033859a7d";
constexpr const char* kT_SigPolicy  = "7f33ed8d-73d6-8951-ba5e-f3e83c50ab23";

/* A cross-fade between two live controllers. Only BezierBlendAsset is read
 * (research spec: duration +0x28 0xef693827 in ticks, elapsed += dt clamped
 * to duration (FUN_1407a0880), remaining = duration - elapsed
 * (FUN_1407a0800), weight = piecewise-linear lookup of the 0xf6ec5494
 * samples at progress * count, the last segment running to 1.0). Both sides
 * advance by the same dt.
 *
 * The pose operator that mixes the two sides is not yet read from the
 * executable; local-space nlerp/lerp is used and NAMED in the runtime notes. */
struct TransitionCtrl : Ctrl {
    std::unique_ptr<Ctrl> target, source;
    float duration = 0.f, elapsed = 0.f;
    std::vector<float> samples;
    float weight() const {
        if (duration <= 0.f || samples.empty()) return 1.f;
        const float p = std::fmin(elapsed / duration, 1.f);
        const float x = p * (float)samples.size();
        const size_t i = (size_t)std::floor(x);
        if (i >= samples.size()) return 1.f;
        const float a = samples[i];
        const float b = i + 1 < samples.size() ? samples[i + 1] : 1.f;
        return a + (b - a) * (x - (float)i);
    }
    float blend_remaining() const { return duration - elapsed; }   /* vtable +0xb8 */
    void update(Runtime& rt, float dt, Pose& out) override {
        elapsed = std::fmin(elapsed + dt, duration);
        Pose tp;
        source->update(rt, dt, out);
        target->update(rt, dt, tp);
        rt.note("transition pose operator: local-space nlerp (engine operator not yet read)");
        pose_blend(out, tp, weight(), nullptr);
        phase = target->phase; time = target->time; length = target->length; remaining = target->remaining;
    }
    void set_time(Runtime& rt, float t) override { target->set_time(rt, t); }
    void set_phase(Runtime& rt, float p) override { target->set_phase(rt, p); }
};

/* Build a transition from `blend` (null = hard switch). Returns `to` itself
 * when there is no blend or it is of a kind not yet read. */
std::unique_ptr<Ctrl> make_transition(Runtime& rt, const Obj* blend, std::unique_ptr<Ctrl> to,
                                      std::unique_ptr<Ctrl> from)
{
    if (!blend || !from) return to;
    if (blend->type != kT_Bezier) {
        rt.note("blend kind not implemented, switched without blending: " + blend->type + " (" + blend->path + ")");
        return to;
    }
    auto t = std::make_unique<TransitionCtrl>();
    t->duration = (float)numv(blend->f(0xef693827u));
    if (const bf6::EbxValue* s = blend->f(0xf6ec5494u))
        for (const auto& it : s->items) t->samples.push_back((float)numv(&it));
    t->asset = blend;
    t->phase = to->phase; t->time = to->time; t->length = to->length; t->remaining = to->remaining;
    t->target = std::move(to);
    t->source = std::move(from);
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
    return e ? std::string(e) : std::string();
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
