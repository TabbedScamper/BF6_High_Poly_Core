#include "ant_graph.h"
#include "bf6_core.h"
#include "ex_vm.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

// Render-bone EX host. Resource bytecode and all tuning come from the install.
// VerletChain: 145321cb0 / 14531eff0, integration 14531efa0,
// constraints 14531ec30. This is a separate host, not vehicle dynamics.
namespace rb {
using E = bf6::EbxValue;
using V = std::array<float, 3>;
using Q = std::array<float, 4>;
using M = std::array<float, 16>;
constexpr float eps = 1.1920929e-7f; // Executable comparison threshold.
constexpr float tick = 0.033333335f; // Executable VerletChain fixed step.
float num(const E *e) {
    return !e                         ? 0.f
           : e->kind == E::Kind::Real ? (float)e->f
           : e->kind == E::Kind::Int  ? (float)e->i
                                      : (float)e->u;
}
uint32_t integer(const E *e) {
    return !e ? 0 : e->kind == E::Kind::Int ? (uint32_t)e->i : (uint32_t)e->u;
}
const std::vector<E> &rows(const E *e) {
    static const std::vector<E> empty;
    return e ? e->items : empty;
}
V add(V a, V b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V sub(V a, V b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V mul(V a, float b) { return {a[0] * b, a[1] * b, a[2] * b}; }
float dot(V a, V b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V cross(V a, V b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
float len(V a) { return std::sqrt(dot(a, a)); }
V unit(V a) { return mul(a, 1.f / len(a)); }
V xyz(const float *a) { return {a[0], a[1], a[2]}; }
V pos(const M &m) { return xyz(m.data() + 12); }
Q norm(Q q) {
    float s = 0;
    for (float f : q)
        s += f * f;
    s = 1 / std::sqrt(s);
    for (float &f : q)
        f *= s;
    return q;
}
Q qm(Q a, Q b) {
    V av = xyz(a.data()), bv = xyz(b.data());
    V v = add(add(mul(bv, a[3]), mul(av, b[3])), cross(av, bv));
    return {v[0], v[1], v[2], a[3] * b[3] - dot(av, bv)};
}
Q inv(Q q) { return {-q[0], -q[1], -q[2], q[3]}; }
V rotate(Q q, V v) {
    V a = xyz(q.data());
    return add(v, mul(cross(a, add(cross(a, v), mul(v, q[3]))), 2));
}
Q between(V a, V b) {
    V c = cross(a, b);
    return norm({c[0], c[1], c[2], dot(a, b) + 1});
}
Q blend(Q a, Q b, float t) {
    float d = 0;
    for (int i = 0; i < 4; ++i)
        d += a[i] * b[i];
    for (int i = 0; i < 4; ++i)
        a[i] += ((d > 0 ? b[i] : -b[i]) - a[i]) * t;
    return norm(a);
}
M identity() {
    M m{};
    m[0] = m[5] = m[10] = m[15] = 1;
    return m;
}
M matrix(const E *e) {
    M m = identity();
    uint32_t rs[] = {0xc478cc3b, 0xbf151ef9, 0x695d12a4, 0xbc4b07b4};
    uint32_t cs[] = {0x3901db14, 0x42fc0f5e, 0x32a99b9c};
    if (e)
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c) {
                auto v = e->field(rs[r]);
                m[r * 4 + c] = num(v ? v->field(cs[c]) : nullptr);
            }
    return m;
}
M compose(Q q, V t) {
    M m = identity();
    for (int i = 0; i < 3; ++i) {
        V v{};
        v[i] = 1;
        v = rotate(q, v);
        std::copy(v.begin(), v.end(), m.begin() + 4 * i);
    }
    std::copy(t.begin(), t.end(), m.begin() + 12);
    return m;
}
M product(M a, M b) {
    M m{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k)
                m[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
    return m;
}
Q quat(M m) {
    for (int r = 0; r < 3; ++r) {
        float l = len(xyz(m.data() + 4 * r));
        for (int k = 0; k < 3; ++k)
            m[4 * r + k] /= l;
    }
    float tr = m[0] + m[5] + m[10];
    Q q;
    if (tr > 0) {
        float s = std::sqrt(tr + 1), h = .5f / s;
        q = {(m[6] - m[9]) * h, (m[8] - m[2]) * h, (m[1] - m[4]) * h, s * .5f};
    } else {
        int i = m[0] > m[5] ? (m[0] > m[10] ? 0 : 2) : (m[5] > m[10] ? 1 : 2);
        int j = (i + 1) % 3, k = (i + 2) % 3;
        float s = std::sqrt(1 + m[5 * i] - m[5 * j] - m[5 * k]), h = .5f / s;
        q = {0, 0, 0, 0};
        q[i] = s * .5f;
        q[j] = (m[4 * i + j] + m[4 * j + i]) * h;
        q[k] = (m[4 * i + k] + m[4 * k + i]) * h;
        q[3] = (m[4 * j + k] - m[4 * k + j]) * h;
    }
    return norm(q);
}
struct Slot {
    uint32_t a = 0, b = 0, type = 0;
};
struct Binding {
    Slot slot;
    int bone = -1;
};
struct Link {
    V current{}, previous{}, local_previous{};
    Q rotation{0, 0, 0, 1}, last_rotation{0, 0, 0, 1};
    float length = 0, last_length = 0;
};
struct Expression {
    bf6ex::Program p;
    bf6ex::Instance vm;
    std::map<uint32_t, Slot> slots;
    std::vector<Binding> input, output;
    std::vector<uint32_t> custom;
    std::vector<Link> links;
    V old_root{}, old_collider{};
    Q old_collider_q{0, 0, 0, 1}, old_q{0, 0, 0, 1};
    float remainder = 0;
    bool initialized = false;
    std::string asset;
    uint8_t *at(Slot s) { return vm.addr(s.a, s.b); }
};
struct Session {
    std::vector<std::unique_ptr<Expression>> expressions;
    std::vector<M> local;
    std::vector<int> parents;
    int rig = 0;
    std::string error;
};
std::mutex mutex;
std::map<uint64_t, std::unique_ptr<Session>> sessions;
uint64_t serial = 0;
thread_local std::string error;
uint32_t word(const uint8_t *p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
Slot operand(const uint8_t *p) { return {word(p), word(p + 4), 0}; }
bool finite(const M &m) {
    for (float f : m)
        if (!std::isfinite(f))
            return false;
    return true;
}

// VerletChain has six matrix inputs and twelve outputs. The same kernel
// services the authored one-, two- and six-link assets. The forward constraint
// pass shares corrections with the preceding endpoint for links after zero.
bool chain(Expression &ex, const uint8_t *code, float dt, std::string &err) {
    std::vector<uint8_t *> in, out;
    const uint8_t *p = code + 12;
    for (auto *list : {&in, &out}) {
        uint32_t n = word(p);
        p += 4;
        for (uint32_t i = 0; i < n; ++i, p += 8)
            list->push_back(ex.at(operand(p)));
    }
    if (in.size() != 35 || out.size() != 12) {
        err = "unknown VerletChain operand layout";
        return false;
    }
    auto f = [&](int i) {
        float v;
        std::memcpy(&v, in[i], 4);
        return v;
    };
    auto m = [&](int i) {
        M v;
        std::memcpy(v.data(), in[i], 64);
        return v;
    };
    const int count = (int)word(in[0]);
    if (count < 1 || count > 6) {
        err = "invalid VerletChain link count";
        return false;
    }
    for (int i = 30; i < 35; ++i)
        if (word(in[i]) || word(in[i] + 4)) {
            err = "unknown: external collision object";
            return false;
        }
    const M root = m(7), collider = m(19);
    const Q root_q = quat(root), collider_q = quat(collider);
    const V root_t = pos(root), collider_t = pos(collider);
    const float mass = f(12);
    const int collision = (int)word(in[18]);
    if (mass == 0 || !finite(root)) {
        err = "degenerate VerletChain root or mass";
        return false;
    }
    if (collision != 1 && collision != 2) {
        err = "unknown VerletChain collision mode";
        return false;
    }
    const V force = xyz((float *)in[10]), gravity = xyz((float *)in[11]);
    std::vector<M> rest;
    std::vector<Q> rest_q;
    std::vector<V> offset, direction;
    std::vector<float> lengths;
    for (int i = 0; i < count; ++i) {
        rest.push_back(m(i + 1));
        rest_q.push_back(quat(rest.back()));
        offset.push_back(pos(rest.back()));
        float length = len(offset.back());
        if (length <= eps || !finite(rest.back())) {
            err = "degenerate VerletChain rest";
            return false;
        }
        lengths.push_back(length);
        direction.push_back(unit(offset.back()));
    }
    if (!ex.initialized) {
        ex.links.resize(count);
        M parent = root;
        for (int i = 0; i < count; ++i) {
            auto &l = ex.links[i];
            parent = product(rest[i], parent);
            l.current = l.previous = pos(parent);
            l.length = l.last_length = lengths[i];
            l.rotation = l.last_rotation = rest_q[i];
            l.local_previous = offset[i];
        }
        ex.old_root = root_t;
        ex.old_q = root_q;
        ex.old_collider = collider_t;
        ex.old_collider_q = collider_q;
        ex.initialized = true;
    }
    ex.remainder += dt;
    const int steps = (int)std::floor(ex.remainder * 29.999998f);
    // Ours: reject pathological caller dt instead of dropping simulation time.
    if (steps > 120) {
        err = "jiggle step exceeds preview work limit; reset after a teleport";
        return false;
    }
    const float rem = std::fmod(ex.remainder, tick);
    for (int step = 0; step < steps; ++step) {
        const float t = ((dt - rem) - step * tick) / dt;
        const V anchor = add(ex.old_root, mul(sub(root_t, ex.old_root), t));
        const Q anchor_q = blend(ex.old_q, root_q, t);
        for (auto &l : ex.links) {
            V old = l.current;
            l.current = add(add(l.current, mul(sub(l.current, l.previous), 1.f - f(15))),
                            mul(add(gravity, mul(force, 1.f / mass)), 0.0011111113f));
            l.previous = old;
        }
        V parent = anchor;
        Q parent_q = anchor_q;
        for (int i = 0; i < count; ++i) {
            auto &l = ex.links[i];
            V local = rotate(inv(parent_q), sub(l.current, parent));
            // 14531ec30: rest-distance limit, retained local motion, length and angle.
            V d = sub(local, offset[i]);
            float distance = len(d);
            if (distance > eps)
                local = add(offset[i], mul(d, std::min(distance, lengths[i] * f(26)) / distance));
            float retention = f(16);
            if (count > 1)
                retention -= i * (f(16) - f(17)) / (count - 1);
            local = add(l.local_previous, mul(sub(local, l.local_previous), 1.f - retention));
            distance = len(local);
            V back{};
            if (distance >= eps) {
                Q correction = blend({0, 0, 0, 1}, between(unit(local), direction[i]), f(14));
                V wanted =
                    rotate(correction,
                           mul(local, ((lengths[i] - distance) * f(13) + distance) / distance));
                float weight = i == 0 ? 1.f : (std::max(0.f, std::min(1.f, f(25))) + 1.f) * .5f;
                V change = sub(local, wanted);
                local = sub(local, mul(change, weight));
                back = mul(change, 1.f - weight);
            }
            if (*in[27]) {
                V lo = xyz((float *)in[28]), hi = xyz((float *)in[29]);
                for (int k = 0; k < 3; ++k)
                    local[k] = std::max(lo[k], std::min(hi[k], local[k]));
            }
            l.current = add(parent, rotate(parent_q, local));
            if (i > 0)
                ex.links[i - 1].current = add(parent, rotate(parent_q, back));
            parent = l.current;
            if (len(local) > eps)
                parent_q = qm(parent_q, qm(between(direction[i], unit(local)), rest_q[i]));
        }
        // 14531e720 capsule and 14531e5b0 plane. Both use collider X.
        const V ct = add(ex.old_collider, mul(sub(collider_t, ex.old_collider), t));
        const Q cq = blend(ex.old_collider_q, collider_q, t);
        const V axis = rotate(cq, {1, 0, 0});
        const float friction = *in[23] ? 1.f - f(22) : f(22);
        for (auto &l : ex.links) {
            V normal = axis;
            float penetration = 0;
            if (collision == 1 && f(20) > 0) {
                float radius = len(xyz(collider.data() + 8)) * f(20),
                      height = len(xyz(collider.data() + 4)) * f(21);
                float extension = *in[24] ? radius : 0.f;
                float along = std::max(-extension,
                                       std::min(height + extension, dot(sub(l.current, ct), axis)));
                V radial = sub(l.current, add(ct, mul(axis, along)));
                float distance = len(radial);
                if (distance >= eps && distance <= radius) {
                    normal = mul(radial, 1.f / distance);
                    penetration = radius - distance;
                }
            } else if (collision == 2 && len(sub(l.current, ct)) >= eps)
                penetration = std::max(0.f, -dot(sub(l.current, ct), axis));
            if (penetration > 0) {
                l.current = add(l.current, mul(normal, penetration));
                V velocity = sub(l.current, l.previous),
                  tangent = sub(velocity, mul(normal, dot(velocity, normal)));
                float speed = len(tangent);
                if (friction > 0 && speed > eps)
                    l.current = sub(l.current,
                                    mul(tangent, std::min(speed, penetration * friction) / speed));
            }
        }
        parent = anchor;
        parent_q = anchor_q;
        for (int i = 0; i < count; ++i) {
            auto &l = ex.links[i];
            l.last_length = l.length;
            l.last_rotation = l.rotation;
            V local = rotate(inv(parent_q), sub(l.current, parent));
            l.length = len(local);
            l.local_previous = local;
            if (l.length > eps)
                l.rotation = qm(between(direction[i], unit(local)), rest_q[i]);
            parent = l.current;
            parent_q = qm(parent_q, l.rotation);
        }
    }
    ex.remainder -= steps * tick;
    ex.old_root = root_t;
    ex.old_q = root_q;
    ex.old_collider = collider_t;
    ex.old_collider_q = collider_q;
    const float alpha = ex.remainder * 29.999998f;
    M parent = root;
    for (int i = 0; i < count; ++i) {
        auto &l = ex.links[i];
        const Q rotation = blend(l.last_rotation, l.rotation, alpha);
        const float length = l.last_length + (l.length - l.last_length) * alpha;
        const Q delta = qm(rotation, inv(rest_q[i]));
        M local = compose(rotation, rotate(delta, mul(direction[i], length)));
        M result = product(local, parent);
        if (!finite(result)) {
            err = "nonfinite VerletChain result";
            return false;
        }
        std::memcpy(out[i], result.data(), 64);
        std::memcpy(out[i + 6], local.data(), 64);
        parent = result;
    }
    return true;
}

bool setup(bf6_ctx *ctx, Session &session, const bf6ant::Obj *data, bf6ant::Graph &assets) {
    const auto &bones = rows(data->f(0x0fb091a9));
    for (const E &b : bones) {
        session.local.push_back(matrix(b.field(0x0b50d1d9)));
        session.parents.push_back((int)integer(b.field(0x7538578a)));
    }
    for (const E &row : rows(data->f(0x54d2bcd1))) {
        auto ex = std::make_unique<Expression>();
        auto asset = assets.resolve(row.field(0xa05c1f85), data);
        if (!asset) {
            session.error = "missing render expression";
            return false;
        }
        ex->asset = asset->path;
        auto definition = assets.resolve(asset->f(0x2094d4d6), asset);
        if (!definition) {
            session.error = "missing render expression definition";
            return false;
        }
        const E *rid = definition->f(0xaa4fa860);
        char resource[1024] = {};
        if (!rid || !bf6_res_by_rid(ctx, rid->u, resource, sizeof resource)) {
            session.error = "missing render expression resource";
            return false;
        }
        const uint8_t *raw = nullptr;
        int64_t size = bf6_read_raw(ctx, BF6_RAW_RES, resource, &raw);
        if (size <= 0) {
            session.error = "unreadable render expression resource";
            return false;
        }
        ex->p.image.assign(raw, raw + size);
        ex->p.path = asset->path;
        if (!ex->p.open_image(session.error) ||
            !ex->vm.init(ex->p, nullptr, nullptr, session.error))
            return false;
        for (const E &r : rows(definition->f(0xfa116802))) {
            auto a = r.field(0xebfee2ee);
            if (!a)
                continue;
            ex->slots[integer(r.field(0x0f885030))] = {integer(a->field(0xbdf81e34)),
                                                       integer(a->field(0x4ff541e4)),
                                                       integer(r.field(0x9bda9997))};
        }
        for (const E &r : rows(row.field(0x0089cf26))) {
            auto it = ex->slots.find(integer(r.field(0xe73c7944)));
            if (it == ex->slots.end()) {
                session.error = "unknown render parameter binding";
                return false;
            }
            Slot s = it->second;
            uint32_t kind = integer(r.field(0x80543d4f));
            if (kind == 1) {
                float v = num(r.field(0xc324528b));
                std::memcpy(ex->at(s), &v, 4);
            } else if (kind == 3) {
                const E *v = r.field(0x935388bc);
                *ex->at(s) = v && v->b;
            } else if (kind == 4)
                ex->input.push_back({s, (int)integer(r.field(0xc82b46dd))});
            else {
                session.error = "unknown render parameter kind";
                return false;
            }
        }
        const auto &ids = rows(row.field(0xc5d39c69));
        const auto &inputs = rows(row.field(0x1b57df53));
        const auto &outputs = rows(row.field(0x7d7c92f1));
        if (ids.size() != inputs.size() || ids.size() != outputs.size()) {
            session.error = "mismatched render bone bindings";
            return false;
        }
        for (size_t i = 0; i < ids.size(); ++i) {
            int b = (int)integer(&ids[i]);
            if (b < 0 || b >= (int)bones.size()) {
                session.error = "render bone binding out of range";
                return false;
            }
            auto si = ex->slots.find(integer(&inputs[i])),
                 so = ex->slots.find(integer(&outputs[i]));
            if ((integer(&inputs[i]) != 0 && si == ex->slots.end()) || so == ex->slots.end()) {
                session.error = "missing render matrix slot";
                return false;
            }
            M rest = matrix(bones[b].field(0xe7062026));
            if (si != ex->slots.end())
                std::memcpy(ex->at(si->second), rest.data(), 64);
            ex->output.push_back({so->second, b});
        }
        // Native reflected side-effect hooks are the executable's 14735f160 RET.
        // The custom chain kernel is handled between EX segments without altering
        // the shared EX interpreter or any soldier/vehicle host.
        for (auto site : ex->p.call_sites) {
            if (site.second == 0x2714ef35u) {
                ex->custom.push_back(site.first);
                uint32_t halt = 0x2c;
                std::memcpy(ex->vm.konst.data() + ex->p.code + site.first, &halt, 4);
            } else if (site.second == 0x7e1afbcbu || site.second == 0x1dc28f8bu) {
                auto at = ex->vm.konst.data() + ex->p.code + site.first;
                uint32_t w = word(at);
                w = (w & ~255u) | 0x2d;
                std::memcpy(at, &w, 4);
            }
        }
        // Check unknown kernels before any output is admitted.
        for (auto name : ex->p.missing_kernels())
            if (name != "2714ef35" && name != "7e1afbcb" && name != "1dc28f8b") {
                session.error = "unknown render kernel " + name;
                return false;
            }
        // Keep the unmodified resource in Program for the custom operand decoder.
        session.expressions.push_back(std::move(ex));
    }
    return true;
}
} // namespace rb
extern "C" {
uint64_t bf6_renderbone_sim_open(bf6_ctx *ctx, const char *path) {
    std::lock_guard<std::mutex> guard(rb::mutex);
    rb::error.clear();
    if (!ctx || !path)
        return 0;
    auto s = std::make_unique<rb::Session>();
    bf6ant::Graph g(ctx);
    auto data = g.root(path);
    if (data && data->f(0xa38bc860))
        data = g.resolve(data->f(0xa38bc860), data);
    if (!data || !data->f(0x0fb091a9)) {
        rb::error = "no RenderBonesData";
        return 0;
    }
    if (!rb::setup(ctx, *s, data, g)) {
        rb::error = s->error;
        return 0;
    }
    const uint64_t id = ++rb::serial;
    rb::sessions[id] = std::move(s);
    return id;
}
int bf6_renderbone_sim_count(uint64_t id) {
    std::lock_guard<std::mutex> guard(rb::mutex);
    auto it = rb::sessions.find(id);
    return it == rb::sessions.end() ? 0 : (int)it->second->local.size();
}
const char *bf6_renderbone_sim_error() { return rb::error.c_str(); }
void bf6_renderbone_sim_close(uint64_t id) {
    std::lock_guard<std::mutex> guard(rb::mutex);
    rb::sessions.erase(id);
}
int bf6_renderbone_sim_step(uint64_t id, const float *world, int count, float dt, float *out,
                            int capacity) {
    std::lock_guard<std::mutex> guard(rb::mutex);
    rb::error.clear();
    auto it = rb::sessions.find(id);
    if (it == rb::sessions.end() || !world || !out || !std::isfinite(dt) || dt <= 0) {
        rb::error = "invalid jiggle step";
        return -1;
    }
    auto &s = *it->second;
    s.rig = count;
    int n = (int)s.local.size();
    if (capacity < n * 16) {
        rb::error = "jiggle matrix count mismatch";
        return -1;
    }
    std::vector<rb::M> bones(count + n);
    for (int i = 0; i < count; ++i)
        std::memcpy(bones[i].data(), world + i * 16, 64);
    for (int i = 0; i < n; ++i) {
        int parent = s.parents[i];
        if (parent < 0 || parent >= s.rig + i) {
            rb::error = "invalid render parent";
            return -1;
        }
        bones[s.rig + i] = rb::product(s.local[i], bones[parent]);
    }
    for (auto &ptr : s.expressions) {
        auto &ex = *ptr;
        ex.vm.notes.clear();
        for (auto input : ex.input) {
            if (input.bone < 0 || input.bone >= s.rig + n) {
                rb::error = "invalid expression bone input";
                return -1;
            }
            std::memcpy(ex.at(input.slot), bones[input.bone].data(), 64);
        }
        uint32_t entry = 0;
        for (uint32_t pc : ex.custom) {
            const uint8_t *original = ex.p.image.data() + ex.p.code + pc;
            const uint8_t *inputs = original + 16;
            rb::Slot clock = rb::operand(inputs + 8 * 8);
            std::memcpy(ex.at(clock), &dt, 4);
            rb::Slot enabled = rb::operand(inputs + 9 * 8);
            *ex.at(enabled) = ex.initialized;
            if (!ex.vm.run(dt * 60, s.error, entry) || !ex.vm.notes.empty()) {
                rb::error = ex.vm.notes.empty() ? s.error : ex.vm.notes.front();
                return -1;
            }
            if (!rb::chain(ex, original, dt, s.error)) {
                rb::error = ex.asset + ": " + s.error;
                return -1;
            }
            entry = rb::word(original) >> 8;
        }
        if (!ex.vm.run(dt * 60, s.error, entry) || !ex.vm.notes.empty()) {
            rb::error = ex.vm.notes.empty() ? s.error : ex.vm.notes.front();
            return -1;
        }
        for (auto binding : ex.output) {
            rb::M m;
            std::memcpy(m.data(), ex.at(binding.slot), 64);
            if (!rb::finite(m)) {
                rb::error = "nonfinite render expression";
                return -1;
            }
            bones[s.rig + binding.bone] = m;
        }
    }
    for (int i = 0; i < n; ++i)
        std::memcpy(out + i * 16, bones[s.rig + i].data(), 64);
    return n * 16;
}
}
