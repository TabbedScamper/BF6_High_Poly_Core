/* ant_graph_ext.inc - ANT asset graph, game states and pose sampling.
 * A translation-unit fragment of bf6_core.cpp (it needs bf6_ctx internals).
 * See ant_graph.h for what each piece is for. */

#include "ant_graph.h"

namespace bf6ant {

/* ------------------------------------------------------------------ graph */

Part* Graph::part(const std::string& path)
{
    auto it = parts_.find(path);
    if (it != parts_.end()) return it->second.get();
    std::string terr;
    if (!ensure_types(c_, terr)) { err_ = "no type schema: " + terr; return nullptr; }
    std::string e2;
    std::vector<uint8_t> raw = c_->src.get_ebx(path + ".ebx", e2);
    if (raw.empty()) raw = c_->src.get_ebx(path, e2);
    if (raw.empty()) { err_ = "not in the mount: " + path; parts_[path] = nullptr; return nullptr; }
    bf6::Ebx e(*c_->types);
    e.set_guid_index(&c_->src.partition_index());
    if (!e.parse(std::move(raw), e2)) { err_ = "parse " + path + ": " + e2; parts_[path] = nullptr; return nullptr; }
    auto p = std::make_unique<Part>();
    p->path = path;
    for (size_t i = 0; i < e.instance_count(); ++i) {
        auto o = std::make_unique<Obj>();
        o->path = path;
        o->index = (int)i;
        o->type = bf6::TypeDb::guid_str(e.instance_type(i));
        o->v = e.read_instance(i);
        const std::string g = e.instance_guid(i);
        if (!g.empty()) p->by_guid[g] = (int)i;
        p->objs.push_back(std::move(o));
    }
    Part* out = p.get();
    parts_[path] = std::move(p);
    return out;
}

const Obj* Graph::root(const std::string& path)
{
    Part* p = part(path);
    return (p && !p->objs.empty()) ? p->objs[0].get() : nullptr;
}

const Obj* Graph::resolve(const bf6::EbxValue* ref, const Obj* from)
{
    if (!ref) return nullptr;
    if (ref->kind == bf6::EbxValue::Kind::InstanceRef) {
        if (!from || ref->instance < 0) return nullptr;
        Part* p = part(from->path);
        if (!p || ref->instance >= (int)p->objs.size()) return nullptr;
        return p->objs[(size_t)ref->instance].get();
    }
    if (ref->kind != bf6::EbxValue::Kind::ImportRef) return nullptr;
    std::string path = ref->import_path;
    if (path.empty() || path == "<not indexed>") { err_ = "unresolved import"; return nullptr; }
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".ebx") == 0) path.resize(path.size() - 4);
    Part* p = part(path);
    if (!p || p->objs.empty()) return nullptr;
    if (!ref->import_instance.empty()) {
        auto it = p->by_guid.find(ref->import_instance);
        if (it != p->by_guid.end()) return p->objs[(size_t)it->second].get();
    }
    return p->objs[0].get();
}

/* ------------------------------------------------------------ game states */

namespace {
constexpr const char* kBoolAsset         = "e82b3d00-c9ef-2a3b-f610-b71fb6e323c5";
constexpr const char* kBoolChannel       = "ecc32e44-f41c-69a0-8229-9a2e647699ae";
constexpr const char* kFloatAsset        = "3d4cf414-7569-67e9-2c1b-1885a58ef54a";
constexpr const char* kFloatChannel      = "15069cf8-f758-fa4c-23be-778b6939ac47";
constexpr const char* kEnumAsset         = "a5cab29b-aabb-e8f1-8ecb-a48ad0b941e9";
constexpr const char* kEnumChannel       = "8308d4de-c89c-ebda-5629-55b5ab2e55f9";
constexpr const char* kEnumItem          = "37948790-7933-b4e3-f96d-ab1e63ef9383";
constexpr const char* kFloatComparison   = "57ef39ec-4fd6-988d-f799-23c09a6addbc";
constexpr const char* kBoolChooser       = "48155699-ed9c-adcf-4e0a-ccd29461119c";
constexpr const char* kEnumPair          = "19b1bfd2-07ad-e287-a6be-7ef64ee5e12b";
constexpr const char* kSignalPolicy      = "7f33ed8d-73d6-8951-ba5e-f3e83c50ab23";
constexpr const char* kSignalEntry       = "e430c3f8-f311-79c7-b0c4-d2a9a200ddd4";
constexpr uint32_t kDefault   = 0x97ea1523u;   /* the authored default value, every state type */
constexpr uint32_t kItemEnum  = 0xffc4267fu;   /* enum item: which enumeration state */
constexpr uint32_t kItemValue = 0x7daad29fu;   /* enum item: the value it names */
constexpr uint32_t kCmpInput  = 0x63545f5eu;   /* FloatComparison: float input */
constexpr uint32_t kCmpInput2 = 0xea11b64au;   /* FloatComparison: second input (null in every sample seen) */
constexpr uint32_t kCmpOp     = 0xf2ca26dbu;   /* GameStateCompareOp */
constexpr uint32_t kCmpValue  = 0x8350c0b8u;   /* threshold */

double num(const bf6::EbxValue* v)
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
}  // namespace

/* SignalChooserPolicy's entry test (FUN_1409bdc80): every bool in +0x38
 * (0xdf9f1fba) must read TRUE and every bool in +0x30 (0xb6eb75d4) FALSE.
 * Items that are null or have no bool face are IGNORED by the engine (it
 * queries the bool interface and skips when absent); here an item whose type
 * is not one we can evaluate sets ok=false instead, because "we could not
 * read it" is not the same as "it has no bool face". Only SignalChooserEntry
 * can pass. */
bool States::signal_pass(Graph& g, const Obj* e, bool& ok)
{
    ok = true;
    if (!e || e->type != kSignalEntry) return false;
    auto all = [&](uint32_t h, bool want) -> bool {
        const bf6::EbxValue* l = e->f(h);
        if (!l) return true;
        for (const auto& it : l->items) {
            const Obj* s = g.resolve(&it, e);
            if (!s) continue;
            bool sok = false;
            const bool v = as_bool(g, s, sok);
            if (!sok) { ok = false; return false; }
            if (v != want) return false;
        }
        return true;
    };
    if (!all(0xdf9f1fbau, true)) return false;
    if (!all(0xb6eb75d4u, false)) return false;
    return true;
}

void States::note_unknown(const Obj* o)
{
    const std::string s = o ? (o->path + " (" + o->type + ")") : std::string("(null state)");
    for (const auto& u : unknown_) if (u == s) return;
    unknown_.push_back(s);
}

bool States::as_bool(Graph& g, const Obj* o, bool& ok)
{
    ok = false;
    if (!o) { note_unknown(o); return false; }
    if (o->type == kBoolAsset || o->type == kBoolChannel) {
        ok = true;
        if (const Value* v = get(o->path)) return v->b;
        return num(o->f(kDefault)) != 0.0;
    }
    if (o->type == kEnumItem) {
        const Obj* e = g.resolve(o->f(kItemEnum), o);
        bool eok = false;
        const int32_t cur = as_int(g, e, eok);
        if (!eok) return false;
        ok = true;
        return cur == (int32_t)num(o->f(kItemValue));
    }
    if (o->type == kEnumPair) {
        /* EnumerationEnumeratorPair, bool face Get 0x1408287a0: the enum game
         * state (+0x30, 0xc2b95529) equals the referenced EnumerationValueAsset's
         * Value (+0x38 -> 0xc82b46dd). Either reference missing reads false. */
        const Obj* e = g.resolve(o->f(0xc2b95529u), o);
        const Obj* ev = g.resolve(o->f(0xf951f98eu), o);
        if (!e || !ev) { ok = true; return false; }
        bool eok = false;
        const int32_t cur = as_int(g, e, eok);
        if (!eok) return false;
        ok = true;
        return cur == (int32_t)num(ev->f(0xc82b46ddu));
    }
    if (o->type == kBoolChooser) {
        /* BoolGameStateChooserAsset (FUN_1409c0350 / FUN_1409c03e0): the
         * policy picks an entry with current index -1 and NO last-entry
         * fallback; the value is that entry's bool, false when none matches.
         * Only SignalChooserPolicy is implemented - the only policy any
         * shipped BoolGameStateChooser uses. */
        const Obj* pol = g.resolve(o->f(0x0ff9bae2u), o);
        const bf6::EbxValue* es = o->f(0xc7166a0cu);
        if (!pol || pol->type != kSignalPolicy) { note_unknown(o); return false; }
        ok = true;
        if (!es) return false;
        for (const auto& er : es->items) {
            const Obj* e = g.resolve(&er, o);
            bool pok = true;
            if (e && signal_pass(g, e, pok) && pok) {
                bool bok = false;
                const bool v = as_bool(g, g.resolve(e->f(0x81309ef7u), e), bok);
                if (!bok) { ok = false; return false; }
                return v;
            }
            if (!pok) { ok = false; return false; }
        }
        return false;
    }
    if (o->type == kFloatComparison) {
        /* GameStateCompareOp - decoded (research: 1p-animation-is-one-authored-
         * ant-graph): 0 EqualTo 1 NotEqualTo 2 LessThan 3 LessThanOrEqualTo
         * 4 GreaterThan 5 GreaterThanOrEqualTo. A second input is refused
         * until a sample shows what it means. */
        if (o->f(kCmpInput2) && o->f(kCmpInput2)->kind != bf6::EbxValue::Kind::Null) { note_unknown(o); return false; }
        bool fok = false;
        const float x = as_float(g, g.resolve(o->f(kCmpInput), o), fok);
        if (!fok) return false;
        const float t = (float)num(o->f(kCmpValue));
        ok = true;
        switch ((int)num(o->f(kCmpOp))) {
        case 0: return x == t;
        case 1: return x != t;
        case 2: return x < t;
        case 3: return x <= t;
        case 4: return x > t;
        case 5: return x >= t;
        default: ok = false; note_unknown(o); return false;
        }
    }
    note_unknown(o);
    return false;
}

float States::as_float(Graph& g, const Obj* o, bool& ok)
{
    (void)g;
    ok = false;
    if (!o) { note_unknown(o); return 0.f; }
    if (o->type == kFloatAsset || o->type == kFloatChannel) {
        ok = true;
        if (const Value* v = get(o->path)) return v->f;
        return (float)num(o->f(kDefault));
    }
    note_unknown(o);
    return 0.f;
}

int32_t States::as_int(Graph& g, const Obj* o, bool& ok)
{
    (void)g;
    ok = false;
    if (!o) { note_unknown(o); return 0; }
    if (o->type == kEnumAsset || o->type == kEnumChannel) {
        ok = true;
        if (const Value* v = get(o->path)) return v->i;
        return (int32_t)num(o->f(kDefault));
    }
    note_unknown(o);
    return 0;
}

/* ------------------------------------------------------------------- pose */

void rows_from_quat(const float* q, float* m9)
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    const float len = std::sqrt(x * x + y * y + z * z + w * w);
    if (len > 1e-8f) { x /= len; y /= len; z /= len; w /= len; }
    const float x2 = x + x, y2 = y + y, z2 = z + z;
    const float xx = x * x2, xy = x * y2, xz = x * z2, yy = y * y2, yz = y * z2, zz = z * z2;
    const float wx = w * x2, wy = w * y2, wz = w * z2;
    m9[0] = 1 - (yy + zz); m9[1] = xy + wz;       m9[2] = xz - wy;
    m9[3] = xy - wz;       m9[4] = 1 - (xx + zz); m9[5] = yz + wx;
    m9[6] = xz + wy;       m9[7] = yz - wx;       m9[8] = 1 - (xx + yy);
}

/* The exact inverse of rows_from_quat (and of loadout.cpp's quat_rows): the
 * off-diagonal pairs there are m1-m3 = 4wz, m6-m2 = 4wy, m5-m7 = 4wx. */
void quat_from_rows(const float* m, float* q)
{
    const float tr = m[0] + m[4] + m[8];
    if (tr > 0.f) {
        const float s = std::sqrt(tr + 1.f) * 2.f;              /* 4w */
        q[3] = 0.25f * s;
        q[0] = (m[5] - m[7]) / s;
        q[1] = (m[6] - m[2]) / s;
        q[2] = (m[1] - m[3]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const float s = std::sqrt(1.f + m[0] - m[4] - m[8]) * 2.f;  /* 4x */
        q[0] = 0.25f * s;
        q[3] = (m[5] - m[7]) / s;
        q[1] = (m[1] + m[3]) / s;
        q[2] = (m[6] + m[2]) / s;
    } else if (m[4] > m[8]) {
        const float s = std::sqrt(1.f + m[4] - m[0] - m[8]) * 2.f;  /* 4y */
        q[1] = 0.25f * s;
        q[3] = (m[6] - m[2]) / s;
        q[0] = (m[1] + m[3]) / s;
        q[2] = (m[5] + m[7]) / s;
    } else {
        const float s = std::sqrt(1.f + m[8] - m[0] - m[4]) * 2.f;  /* 4z */
        q[2] = 0.25f * s;
        q[3] = (m[1] - m[3]) / s;
        q[0] = (m[6] + m[2]) / s;
        q[1] = (m[5] + m[7]) / s;
    }
}

namespace {
void qnorm(float* q)
{
    const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l > 1e-12f) for (int k = 0; k < 4; ++k) q[k] /= l;
}
void qmul(const float* a, const float* b, float* o)   /* Hamilton product a*b */
{
    const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    const float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    const float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    const float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
}  // namespace

namespace {
/* nlerp(a,b,t) = normalize(a + (hemi(a,b) - a) * t), hemi flips b to a's side. */
void nlerp(const float* a, const float* b, float t, float* o)
{
    const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    const float s = d > 0.f ? 1.f : -1.f;
    for (int c = 0; c < 4; ++c) o[c] = a[c] + (s * b[c] - a[c]) * t;
    qnorm(o);
}
}  // namespace

void pose_blend(Pose& dst, const Pose& src, float w, const Mask* mask)
{
    for (size_t i = 0; i < dst.q.size() && i < src.q.size(); ++i) {
        const float wr = w * (mask && i < mask->rot.size() ? mask->rot[i] : 1.f);
        const float wt = w * (mask && i < mask->tr.size() ? mask->tr[i] : 1.f);
        if (src.vq[i]) {
            if (!dst.vq[i]) { dst.q[i] = src.q[i]; dst.vq[i] = 1; }
            else if (wr != 0.f) { float o[4]; nlerp(dst.q[i].data(), src.q[i].data(), wr, o); for (int c = 0; c < 4; ++c) dst.q[i][(size_t)c] = o[c]; }
        }
        if (src.vt[i]) {
            if (!dst.vt[i]) { dst.t[i] = src.t[i]; dst.vt[i] = 1; }
            else if (wt != 0.f) for (int c = 0; c < 3; ++c) dst.t[i][(size_t)c] = dst.t[i][(size_t)c] * (1.f - wt) + src.t[i][(size_t)c] * wt;
        }
    }
}

/* AN ADDITIVE LAYER ADDS TO SOMETHING. Where the pose underneath never wrote
 * a slot, there is nothing to add to: the engine's base always fills the joint
 * block from a clip over the whole rig, so its kernel never meets the case,
 * and adding to the BIND value here would invent a pose for a bone the graph
 * never drove - which is what put an unposed arm in its bind stance. The slot
 * is left invalid instead, and the caller keeps whatever it already had. */
void pose_add(Pose& dst, const Pose& add, float w, const Mask* mask)
{
    static const float I[4] = {0, 0, 0, 1};
    for (size_t i = 0; i < dst.q.size() && i < add.q.size(); ++i) {
        if (add.vq[i] && dst.vq[i]) {
            float a[4] = { add.q[i][0], add.q[i][1], add.q[i][2], add.q[i][3] };
            float k;
            if (mask) k = w * (i < mask->rot.size() ? mask->rot[i] : 1.f);
            else { k = std::fabs(w); if (w < 0.f) { a[0] = -a[0]; a[1] = -a[1]; a[2] = -a[2]; } }
            if (k != 0.f) {
                float d[4], r[4];
                nlerp(I, a, k, d);
                qmul(dst.q[i].data(), d, r);
                qnorm(r);
                for (int c = 0; c < 4; ++c) dst.q[i][(size_t)c] = r[c];
                dst.vq[i] = 1;
            }
        }
        if (add.vt[i] && dst.vt[i]) {
            const float k = w * (mask && i < mask->tr.size() ? mask->tr[i] : 1.f);
            for (int c = 0; c < 3; ++c) dst.t[i][(size_t)c] += add.t[i][(size_t)c] * k;
        }
    }
}

/* ------------------------------------------------------------ clip source */

bool ClipSource::open(bf6_ctx* c, const std::string& clip, const char* rig, const char* skeleton,
                      std::string& err)
{
    owner_ = c;
    bf6_anim_binding_stats st{};
    const int n = bf6_anim_bindings(c, clip.c_str(), rig, skeleton, nullptr, 0, &st);
    if (n < 1 || n > 8192) { err = "no binding for " + clip; return false; }
    std::vector<bf6_anim_binding> b((size_t)n);
    if (bf6_anim_bindings(c, clip.c_str(), rig, skeleton, b.data(), n, &st) != n) { err = "binding read"; return false; }
    clip_ = bf6_anim_clip_open(c, clip.c_str());
    if (!clip_) { err = "clip does not open: " + clip; return false; }
    channels_ = clip_->channel_count;
    frames_ = clip_->key_time_count;
    for (const auto& x : b) {
        if (x.bone < 0) continue;
        if (x.component != BF6_ANIM_DOF_QUATERNION && x.component != BF6_ANIM_DOF_VECTOR3) continue;
        if (x.channel < 0 || x.channel >= channels_) continue;
        ch_.push_back(x.channel); comp_.push_back(x.component); bone_.push_back(x.bone);
    }
    return true;
}

ClipSource::~ClipSource()
{
    if (clip_ && owner_) bf6_free(owner_, clip_);
}

bool ClipSource::sample(bf6_ctx* c, float frames, bool loop, Pose& out) const
{
    if (!clip_) return false;
    std::vector<float> ch((size_t)channels_ * 4, 0.f);
    if (!bf6_anim_clip_sample_time(c, clip_, frames, loop ? 1 : 0, ch.data())) return false;
    for (size_t k = 0; k < ch_.size(); ++k) {
        const size_t b = (size_t)bone_[k];
        if (b >= out.q.size()) continue;
        const float* v = ch.data() + (size_t)ch_[k] * 4;
        if (comp_[k] == BF6_ANIM_DOF_QUATERNION) { out.q[b] = { v[0], v[1], v[2], v[3] }; qnorm(out.q[b].data()); out.vq[b] = 1; }
        else { out.t[b] = { v[0], v[1], v[2] }; out.vt[b] = 1; }
    }
    return true;
}

}  // namespace bf6ant
