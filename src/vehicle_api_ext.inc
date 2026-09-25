/* ------------------------------------------------ Drivable vehicles (C ABI)
 *
 * bf6_vehicle_open / _step / _set_pose / _close run a vehicle's OWN expression
 * graphs (drivetrain simex_<class>_<veh>, suspension presex_<veh>_suspension-
 * movement_1) every step and close the loop with VehicleDynamics, feeding back
 * exactly what native code supplies in the game: per-wheel spin and ground contact
 * (frame-relative state), AngularVelocity_Wheel_*, LinearVelocity, AngularVelocity,
 * LinearAcceleration and RootTransform. See vehicle_dynamics.h for which numbers
 * are the game's and which are stand-ins.
 *
 * Frame: metres, Y up, vehicle forward = +Z, the core's frame (the same one the
 * walker uses). Ground: world-space triangles given at open, like walk_open. */

#include "vehicle_sim.h"
#include "vehicle_dynamics.h"
#include "ant_graph.h"

struct bf6_vehicle_collision_point { float p[3]; };

struct bf6_vehicle {
    bf6::VehicleSim sim;
    bf6::VehicleDynamics dyn;
    bf6_ray_scene* scene = nullptr;
    bool no_wheel_locators = false;   /* the rig had none: graph physics only */
    /* Wheel and track speeds by channel name, from the last tick's tyre calls, placed
     * by where each call's contact point sits on the body (see the step). */
    std::map<std::string, float> wheel_spin;
    float water_height = 0.0f;        /* the surface the hull and buoyancy read */
    bool water_present = false;
    /* THE CYCLIC, which nothing could command before. The step's `in` array carries
     * throttle, brake, yaw, handbrake and dt and nothing else, so a helicopter offline had
     * no pitch or roll input at all and its graph fell back to an autopilot whose output
     * grows without bound. Judging that autopilot wrong was impossible while there was no
     * control input to compare it against. Out of band rather than a seventh element of
     * `in`, because the Godot binding only guarantees six and would read past its buffer. */
    float cyclic_pitch = 0.0f, cyclic_roll = 0.0f;
    float aim_yaw = 0.0f, aim_pitch = 0.0f;   /* bf6_vehicle_set_aim */
    /* THE VEHICLE'S ANIMATION CLIP LAYERS (see bf6__open_clip_layers): a clip scrubbed
     * by a phase channel the graphs write, its DOFs on the bones their channels map to. */
    struct ClipLayer {
        std::string phase;                 /* e.g. Anim.LandingGear.Float */
        bf6_anim_clip* clip = nullptr;
        int frames = 0;
        struct Dof { int channel; int component; uint32_t bone_channel; };
        std::vector<Dof> dofs;
    };
    std::vector<ClipLayer> clip_layers;
    /* FAKEHINGE SECONDARY MOTION (see bf6__fakehinge_step): one per bound bone. */
    struct FakeHinge {
        int32_t bone = -1;
        std::map<uint32_t, float> d;       /* FakeHingeData by runtime byte offset */
        float theta = 0.0f, w = 0.0f;       /* +0x108 angle, +0x104 angular velocity */
        float prev_t[3] = {0, 0, 0}, prev_v[3] = {0, 0, 0};
        bool history = false;
        float last_local[16] = {};   /* what this hinge wrote last tick */
        bool wrote = false;
        float f(uint32_t off) const { auto it = d.find(off); return it == d.end() ? 0.0f : it->second; }
    };
    std::vector<FakeHinge> hinges;
    bf6_ctx* ctx = nullptr;
    float rws_yaw[4] = {}, rws_pitch[4] = {}; /* bf6_vehicle_set_rws_aim */
    bool seat_occupied[8] = {true, false, false, false, false, false, false, false};
    bool boost = false;
    double time = 0.0;
    std::string name;
    std::vector<bf6_vehicle_collision_point> collision_points;
    bool physics_body_valid = false, chassis_contact = false, graph_com_written = false;
    float physics_mass = 0.0f, physics_com[3] = {}, physics_inertia_per_kg[3] = {};
};

namespace {

/* The tracer is handed the VEHICLE, not its scene, and reads the scene on every ray:
 * bf6_vehicle_set_ground swaps the scene underneath a running vehicle, and a
 * pointer captured at open would keep tracing the freed one. */
/* Every import path an EBX partition names, across all its instances. */
std::vector<std::string> bf6__ebx_import_paths(bf6_ctx* c, const std::string& name) {
    std::vector<std::string> out;
    std::string err;
    std::vector<uint8_t> raw = c->src.get_ebx(name, err);
    if (raw.empty()) return out;
    bf6::Ebx e(*c->types);
    e.set_guid_index(&c->src.partition_index());
    if (!e.parse(std::move(raw), err)) return out;
    std::function<void(const bf6::EbxValue&, int)> walk = [&](const bf6::EbxValue& v, int depth) {
        if (depth > 16) return;
        if (v.kind == bf6::EbxValue::Kind::ImportRef && !v.import_path.empty() &&
            v.import_path != "<not indexed>") {
            std::string p = v.import_path;
            if (p.size() > 4 && p.compare(p.size() - 4, 4, ".ebx") == 0) p.resize(p.size() - 4);
            out.push_back(p);
        }
        for (const auto& kv : v.fields) walk(kv.second, depth + 1);
        for (const auto& it : v.items) walk(it, depth + 1);
    };
    for (size_t i = 0; i < e.instance_count(); ++i) walk(e.read_instance(i), 0);
    return out;
}

/* PUBLIC CHANNEL IDS BY NAME. A channel's id is the one its public channel list
 * declares (field 0xCDC6F6A3 beside its Name), not a hash of the name; an animation
 * clip names its DOFs by channel name ("LeftRearWheel.q"), and the skeleton maps ids
 * to bones. Built once per context from every *publicchannels / *channellist asset. */
const std::map<std::string, uint32_t>& bf6__public_channel_ids(bf6_ctx* c) {
    static std::map<bf6_ctx*, std::map<std::string, uint32_t>> cache;
    auto found = cache.find(c);
    if (found != cache.end()) return found->second;
    std::map<std::string, uint32_t>& ids = cache[c];
    for (const char* term : {"publicchannels", "channellist"}) {
        const int n = bf6_list_ebx(c, term, nullptr, 0);
        std::vector<bf6_asset> rows((size_t)std::max(n, 0));
        const int got = n > 0 ? std::clamp(bf6_list_ebx(c, term, rows.data(), n), 0, n) : 0;
        for (int i = 0; i < got; ++i) {
            if (!rows[(size_t)i].name) continue;
            std::string err;
            std::vector<uint8_t> raw = c->src.get_ebx(rows[(size_t)i].name, err);
            if (raw.empty()) continue;
            bf6::Ebx e(*c->types);
            if (!e.parse(std::move(raw), err)) continue;
            for (size_t k = 0; k < e.instance_count(); ++k) {
                const bf6::EbxValue v = e.read_instance(k);
                const bf6::EbxValue* id = v.field(0xCDC6F6A3u);
                const bf6::EbxValue* nm = v.field(0x0C59FA06u);
                if (!id || !nm || nm->kind != bf6::EbxValue::Kind::Str || nm->s.empty()) continue;
                const uint32_t idv = id->kind == bf6::EbxValue::Kind::Uint ? (uint32_t)id->u : (uint32_t)id->i;
                ids.emplace(nm->s, idv);
            }
        }
    }
    return ids;
}

/* ---- FAKEHINGE, transcribed from the natives (Codex study, corpus 6a1c1b):
 *   FUN_142b69620  per-frame motion history: the driven bone's origin velocity and its
 *                  change since the last frame (dv), then the integrator (virtual +0x70)
 *   FUN_142b67fa0  the integrator (same math as FUN_144ed12e0): gravity along the swing
 *                  tangent, the inertial response to -dv/dt, a progressive restoring
 *                  spring, fractional velocity loss, a dead zone, a speed cap on the angle
 *                  step, min/max angles with impact damping
 *   FUN_142b67c30  the pose: rotation about RotationAxis at Pivot
 *   FUN_140e122b0  the bone: local = hinge output then the rest pose
 * Data offsets (FakeHingeData, runtime layout): +0x20 max speed, +0x24 upper impact
 * damping, +0x28 mass, +0x2c gravity multiplier, +0x30 dead zone, +0x38 lower impact
 * damping, +0x40 pivot, +0x50 rotation axis, +0x60 extension axis, +0x70 restoring
 * coefficient, +0x74 min angle (deg), +0x78 response divisor, +0x7c progressive
 * exponent base, +0x80 velocity loss, +0x84 max angle (deg).
 * NOT MODELLED, stated: the history step's optional speed cap (its source is unknown)
 * and the delta-reuse countdown after an irregular frame; no force at a point. */
namespace fh {
struct V { float x, y, z; };
static V sub(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V mul(V a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V cross(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static V norm(V a) { const float l = std::sqrt(dot(a, a)); return l > 1e-12f ? mul(a, 1.0f / l) : a; }
/* a direction through a row-major LinearTransform (rows = images of the basis) */
static V dir(const float* m, V v) {
    return {v.x * m[0] + v.y * m[4] + v.z * m[8], v.x * m[1] + v.y * m[5] + v.z * m[9], v.x * m[2] + v.y * m[6] + v.z * m[10]};
}
/* rotation by theta about axis through pivot, as LinearTransform rows (FUN_142b67c30) */
static void axis_pivot(V a, V p, float th, float out[16]) {
    const float c = std::cos(th), s = std::sin(th), k = 1.0f - c;
    const float R[3][3] = {{c + a.x * a.x * k, a.x * a.y * k - a.z * s, a.x * a.z * k + a.y * s},
                           {a.y * a.x * k + a.z * s, c + a.y * a.y * k, a.y * a.z * k - a.x * s},
                           {a.z * a.x * k - a.y * s, a.z * a.y * k + a.x * s, c + a.z * a.z * k}};
    std::memset(out, 0, 64);
    for (int r = 0; r < 3; ++r)                  /* row r = column r of R */
        for (int col = 0; col < 3; ++col) out[r * 4 + col] = R[col][r];
    const V rp = {R[0][0] * p.x + R[0][1] * p.y + R[0][2] * p.z, R[1][0] * p.x + R[1][1] * p.y + R[1][2] * p.z,
                  R[2][0] * p.x + R[2][1] * p.y + R[2][2] * p.z};
    out[12] = p.x - rp.x; out[13] = p.y - rp.y; out[14] = p.z - rp.z;
}
/* row-vector composition: p * a * b */
static void lt(const float* a, const float* b, float* o) {
    std::memset(o, 0, 64);
    for (int r = 0; r < 4; ++r)
        for (int col = 0; col < 3; ++col) {
            o[r * 4 + col] = a[r * 4] * b[col] + a[r * 4 + 1] * b[4 + col] + a[r * 4 + 2] * b[8 + col];
            if (r == 3) o[r * 4 + col] += b[12 + col];
        }
}
} // namespace fh

int bf6__vehicle_trace(void* user, const double* from, const double* to, double* hit, double* normal) {
    const bf6_vehicle* vv = (const bf6_vehicle*)user;
    bf6_ray_scene* s = vv ? vv->scene : nullptr;
    double out[7] = {};
    if (!s || bf6_ray_scene_trace(s, from, to, out) < 0) return 0;
    for (int i = 0; i < 3; ++i) { hit[i] = out[i]; normal[i] = out[3 + i]; }
    return 1;
}

float bf6__real(const bf6::EbxValue* v, float fallback) {
    if (!v) return fallback;
    if (v->kind == bf6::EbxValue::Kind::Real) return (float)v->f;
    if (v->kind == bf6::EbxValue::Kind::Int) return (float)v->i;
    if (v->kind == bf6::EbxValue::Kind::Uint) return (float)v->u;
    return fallback;
}

/* v' = q v q^-1, q = (x, y, z, w) */
void bf6__qrot(const float q[4], const float v[3], float out[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2.0f * (y * v[2] - z * v[1]);
    const float ty = 2.0f * (z * v[0] - x * v[2]);
    const float tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

bool bf6__vec3(const bf6::EbxValue* v, float out[3]) {
    if (!v || v->kind != bf6::EbxValue::Kind::Struct) return false;
    const uint32_t k[3] = {0x3901db14u, 0x42fc0f5eu, 0x32a99b9cu};   /* x, y, z */
    for (int i = 0; i < 3; ++i) {
        const bf6::EbxValue* c = v->field(k[i]);
        if (!c) return false;
        out[i] = bf6__real(c, 0.0f);
    }
    return true;
}

/* The vehicle blueprint (vb_veh_<class>_<veh>): PhysicsConfigData (mass 0x6e58e5b7,
 * centre of mass 0x2754aac9, inertia per kg 0xa3bdfec6) and the wheel collision
 * shape (half-extents 0x0d497f25 with y == z = radius, at a transform 0xd6351ede /
 * 0xbc4b07b4 translation near a wheel locator). */
void bf6__vehicle_blueprint(bf6_ctx* c, const std::string& vb, bf6::VehicleDynamicsParams& p) {
    std::string err;
    if (!ensure_types(c, err)) return;
    std::vector<uint8_t> raw = c->src.get_ebx(vb, err);
    if (raw.empty()) return;
    bf6::Ebx e(*c->types);
    if (!e.parse(std::move(raw), err)) return;
    float best_r = 0.0f, best_w = 0.0f, best_d = 1e9f;
    for (size_t i = 0; i < e.instance_count(); ++i) {
        const bf6::EbxValue v = e.read_instance(i);
        if (const bf6::EbxValue* m = v.field(0x6e58e5b7u)) {
            float com[3], inertia[3];
            if (bf6__vec3(v.field(0x2754aac9u), com) && bf6__vec3(v.field(0xa3bdfec6u), inertia)) {
                p.mass = bf6__real(m, p.mass);
                for (int k = 0; k < 3; ++k) { p.com[k] = com[k]; p.inertia_per_kg[k] = inertia[k]; }
            }
        }
        float he[3], t[3];
        const bf6::EbxValue* xf = v.field(0xd6351ede);
        if (bf6__vec3(v.field(0x0d497f25u), he) && xf && bf6__vec3(xf->field(0xbc4b07b4u), t) &&
            std::fabs(he[1] - he[2]) < 1e-4f && he[0] < he[1] && he[1] > 0.1f) {
            for (const auto& w : p.wheel) {
                const float d = std::fabs(std::fabs(t[0]) - std::fabs(w.mount[0])) + std::fabs(t[1] - w.mount[1]) +
                                std::fabs(t[2] - w.mount[2]);
                if (d < best_d) { best_d = d; best_r = he[1]; best_w = he[0]; }
            }
        }
    }
    if (best_r > 0.0f && best_d < 0.2f)
        for (auto& w : p.wheel) w.radius = best_r;
    (void)best_w;
}

/* Preserve the authored rigid-body fallback. SetInertia's all-zero argument
 * means "keep the physics body", rather than "use arbitrary host defaults". */
void bf6__vehicle_physics(bf6_ctx* c, const std::string& dir, const std::string& cls,
                          const std::string& veh, bf6_vehicle* v) {
    const std::string name = dir + "/art/ob_veh_" + cls + "_" + veh + "_base.physics";
    bf6_physics* ph = bf6_physics_read(c, name.c_str());
    if (!ph || ph->body_rec_count <= 0) {
        if (ph) bf6_free(c, ph);
        return;
    }
    const bf6_phys_body& b = ph->bodies[0];
    if (b.reciprocal_mass > 0.0f) {
        v->physics_body_valid = true;
        v->physics_mass = 1.0f / b.reciprocal_mass;
        for (int i = 0; i < 3; ++i) {
            v->physics_com[i] = b.center_of_mass[i];
            if (b.inv_inertia[i] > 0.0f) v->physics_inertia_per_kg[i] = 1.0f / b.inv_inertia[i];
        }
    }
    /* Four low vertices per x/z quadrant and shape retain a compact chassis
     * support polygon. Ignore the game's explicit NoCollision preset. */
    float detail_low[4][3] = {};
    bool detail_have[4] = {};
    const bool placeholder_body = std::fabs(b.reciprocal_mass - 1.0f) <= 1e-6f &&
        b.inv_inertia[0] == 0.0f && b.inv_inertia[1] == 0.0f && b.inv_inertia[2] == 0.0f;
    for (int j = 0; j < ph->inst_count; ++j) {
        const bf6_phys_inst& in = ph->instances[j];
        if (in.body_index != 0 || in.shape_index < 0 || in.shape_index >= ph->shape_count) continue;
        const bool detail_fallback = placeholder_body && in.preset_a == 0x2AF24804u &&
            in.preset_b == 0x2AF24804u && in.preset_c == 0xBD957D4Au;
        if (in.preset_a == 0x2AF24804u && !detail_fallback) continue;
        const bf6_phys_shape& sh = ph->shapes[in.shape_index];
        if (sh.vertex_first < 0 || sh.vertex_count == 0) continue;
        float low[4][3] = {};
        bool have[4] = {};
        const float scale = in.scale_or_halfheight != 0.0f ? in.scale_or_halfheight : 1.0f;
        for (uint32_t k = 0; k < sh.vertex_count; ++k) {
            const float* src = ph->vertices + 3 * (sh.vertex_first + k);
            float sv[3] = {src[0] * scale, src[1] * scale, src[2] * scale}, rv[3];
            bf6__qrot(in.quat, sv, rv);
            for (int q = 0; q < 3; ++q) rv[q] += in.pos[q];
            const int bin = (rv[0] >= in.pos[0] ? 1 : 0) | (rv[2] >= in.pos[2] ? 2 : 0);
            if (detail_fallback) {
                const int global_bin = (rv[0] >= 0.0f ? 1 : 0) | (rv[2] >= 0.0f ? 2 : 0);
                if (!detail_have[global_bin] || rv[1] < detail_low[global_bin][1]) {
                    for (int q = 0; q < 3; ++q) detail_low[global_bin][q] = rv[q];
                    detail_have[global_bin] = true;
                }
                continue;
            }
            if (!have[bin] || rv[1] < low[bin][1]) {
                for (int q = 0; q < 3; ++q) low[bin][q] = rv[q];
                have[bin] = true;
            }
        }
        if (detail_fallback) continue;
        for (int bin = 0; bin < 4; ++bin) if (have[bin]) {
            bf6_vehicle_collision_point cp{};
            for (int q = 0; q < 3; ++q) cp.p[q] = low[bin][q];
            v->collision_points.push_back(cp);
        }
    }
    /* Some cinematic/gameplay DefinitionMeshes (notably the MH-47) link a
     * placeholder Fixed body whose only authored geometry is the detailed
     * PhysicsAsset: mass 1, zero inertia, and NoCollision/NoCollision/Detail.
     * It is not honest to treat all 41 detail pieces as normal collision. When
     * no solid hull exists, reduce that linked geometry to its four lowest
     * chassis supports; the vehicle host supplies the dynamic mass properties. */
    if (v->collision_points.empty())
        for (int bin = 0; bin < 4; ++bin) if (detail_have[bin]) {
            bf6_vehicle_collision_point cp{};
            for (int q = 0; q < 3; ++q) cp.p[q] = detail_low[bin][q];
            v->collision_points.push_back(cp);
        }
    v->chassis_contact = !v->collision_points.empty();
    bf6_free(c, ph);
}

/* Normal-only rigid-body contact. Tyre operators continue to supply the
 * authored tangential forces. */
void bf6__collide_vehicle(bf6_vehicle* v, float dt) {
    if (!v || dt <= 0.0f || v->collision_points.empty()) return;
    bf6::VehicleDynamics& d = v->dyn;
    for (int pass = 0; pass < 4; ++pass) {
        float deepest = 0.0f;
        for (const auto& cp : v->collision_points) {
            float rp[3], wp[3];
            bf6__qrot(d.quat, cp.p, rp);
            for (int i = 0; i < 3; ++i) wp[i] = d.pos[i] + rp[i];
            double from[3] = {wp[0], wp[1] + 0.25, wp[2]};
            double to[3] = {wp[0], wp[1] - 0.25, wp[2]}, hit[3], nd[3];
            if (!bf6__vehicle_trace(v, from, to, hit, nd)) continue;
            float n[3] = {(float)nd[0], (float)nd[1], (float)nd[2]};
            const float penetration = (float)((hit[0] - wp[0]) * nd[0] +
                                              (hit[1] - wp[1]) * nd[1] +
                                              (hit[2] - wp[2]) * nd[2]);
            if (penetration < -1e-4f) continue;
            if (penetration > deepest) deepest = penetration;
            float comb[3], comw[3], r[3];
            bf6__qrot(d.quat, d.p.com, comb);
            for (int i = 0; i < 3; ++i) { comw[i] = d.pos[i] + comb[i]; r[i] = wp[i] - comw[i]; }
            float wxr[3] = {d.angvel[1] * r[2] - d.angvel[2] * r[1],
                            d.angvel[2] * r[0] - d.angvel[0] * r[2],
                            d.angvel[0] * r[1] - d.angvel[1] * r[0]};
            const float vn = (d.vel[0] + wxr[0]) * n[0] + (d.vel[1] + wxr[1]) * n[1] +
                             (d.vel[2] + wxr[2]) * n[2];
            if (vn >= 0.0f) continue;
            float rxn[3] = {r[1] * n[2] - r[2] * n[1],
                            r[2] * n[0] - r[0] * n[2],
                            r[0] * n[1] - r[1] * n[0]};
            float qi[4] = {-d.quat[0], -d.quat[1], -d.quat[2], d.quat[3]}, tb[3], dwb[3], dww[3];
            bf6__qrot(qi, rxn, tb);
            for (int i = 0; i < 3; ++i) dwb[i] = tb[i] / (d.p.mass * d.p.inertia_per_kg[i]);
            bf6__qrot(d.quat, dwb, dww);
            const float cr[3] = {dww[1] * r[2] - dww[2] * r[1],
                                 dww[2] * r[0] - dww[0] * r[2],
                                 dww[0] * r[1] - dww[1] * r[0]};
            const float k = 1.0f / d.p.mass + cr[0] * n[0] + cr[1] * n[1] + cr[2] * n[2];
            if (k <= 0.0f) continue;
            const float impulse = -vn / k;
            for (int i = 0; i < 3; ++i) {
                d.vel[i] += n[i] * impulse / d.p.mass;
                d.angvel[i] += dww[i] * impulse;
            }
        }
        if (pass == 3 && deepest > 0.0f) d.pos[1] += deepest;
    }
}

} // namespace

#include "vehicle_typed_seed.inc"

bf6_vehicle* bf6_vehicle_open(bf6_ctx* c, const char* vehicle_dir, const float* tris, int32_t vert_count,
                              char* err, int32_t err_len)
{
    return bf6_vehicle_open_ex(c, vehicle_dir, tris, vert_count, 0, err, err_len);
}

bf6_vehicle* bf6_vehicle_open_ex(bf6_ctx* c, const char* vehicle_dir, const float* tris, int32_t vert_count,
                                 int32_t flags, char* err, int32_t err_len)
{
    auto fail = [&](const std::string& m) -> bf6_vehicle* {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", m.c_str());
        return nullptr;
    };
    if (!c || !vehicle_dir || !*vehicle_dir) return fail("no context or vehicle");
    std::string dir = vehicle_dir;
    while (!dir.empty() && dir.back() == '/') dir.pop_back();
    const size_t s1 = dir.rfind('/');
    const size_t s0 = s1 == std::string::npos ? std::string::npos : dir.rfind('/', s1 - 1);
    if (s1 == std::string::npos || s0 == std::string::npos) return fail("vehicle dir must be .../<class>/<vehicle>");
    const std::string veh = dir.substr(s1 + 1), cls = dir.substr(s0 + 1, s1 - s0 - 1);
    /* THE GRAPHS AND THE RIG CAN LIVE IN DIFFERENT FOLDERS. The rig is always this
     * vehicle's own; the graphs come from gdir. A vehicle whose folder is art only runs
     * its twin's graphs: the Cheetah's folder holds a rig, meshes and a customization
     * whose abilities are ability_gepard_* and nothing else, so it drives as the Gepard. */
    const std::string root = dir.substr(0, s0 + 1);
    std::string gdir = dir, gveh = veh, gcls = cls;
    static const struct { const char* vehicle; const char* donor; } kGraphDonor[] = {
        {"tank/cheetah", "tank/gepard"},
    };
    for (const auto& d : kGraphDonor)
        if (cls + "/" + veh == d.vehicle) {
            const std::string dd = d.donor;
            gdir = root + dd;
            gcls = dd.substr(0, dd.find('/'));
            gveh = dd.substr(dd.find('/') + 1);
        }

    std::string exe;
    for (const std::string& cand : bf6::TypeDb::exe_candidates(c->src.game_dir())) { exe = cand; break; }
    if (exe.empty()) return fail("no executable for operator names");

    /* THE VEHICLE'S HARDWARE PACKAGES. A level mount does not carry them (a vehicle
     * folder is mostly in the Update packages); widen the mount, which only ever
     * adds names, exactly as the first-person path does. */
    auto have = [&](const std::string& name) {
        const uint8_t* probe = nullptr;
        return bf6_read_raw(c, BF6_RAW_RES, name.c_str(), &probe) > 0;
    };
    /* THE DRIVETRAIN GRAPH, whatever its class spells it: cars, helicopters and tanks
     * use simex_<class>_<vehicle>, airplanes simex_veh_airplane_<vehicle>. */
    const std::string sim_a = gdir + "/simex_" + gcls + "_" + gveh;
    const std::string sim_b = gdir + "/simex_veh_" + gcls + "_" + gveh;
    /* Unconditionally: a level mount can carry the drivetrain graph and still miss
     * the presentation ones: the AH-64 rotor graph is only in the level archives of
     * levels that field it, so every level is mounted, as the soldier sim does. */
    {
        char merr[256] = {};
        bf6_mount_all(c, 1, merr, (int)sizeof(merr));
    }
    /* A HYPHENATED DESIGNATION. The MQ-9 drone's folder is mq9 but its graph is
     * simex_veh_airplane_mq-9: the hyphen sits between the letters and the digits. */
    std::string hy = gveh;
    {
        size_t k = hy.size();
        while (k > 0 && std::isdigit((unsigned char)hy[k - 1])) --k;
        if (k > 0 && k < hy.size() && hy[k - 1] != '-') hy.insert(k, "-");
    }
    const std::string sim_c = gdir + "/simex_veh_" + gcls + "_" + hy;
    const std::string simex = have(sim_a) ? sim_a : have(sim_b) ? sim_b : have(sim_c) ? sim_c : sim_b;
    /* PRESENTATION ONLY (BF6_VEHICLE_OPEN_PRESENTATION): no drivetrain graph is not a
     * refusal when the caller only wants the parts to move. Emplacements and drones
     * ship presentation graphs and no simex; a drone's live in drone/<veh>/expressions. */
    const bool presentation_only = !have(simex);
    if (presentation_only && !(flags & BF6_VEHICLE_OPEN_PRESENTATION))
        return fail("no drivetrain graph: " + sim_a);
    std::unique_ptr<bf6_vehicle> v(new bf6_vehicle());
    v->name = veh;
    v->scene = bf6_ray_scene_create();
    if (tris && vert_count >= 3) {
        std::vector<int32_t> idx((size_t)(vert_count - vert_count % 3));
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = (int32_t)i;
        const int32_t mesh = bf6_ray_scene_add_mesh(v->scene, tris, vert_count, idx.data(), (int32_t)idx.size());
        const double identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        if (mesh >= 0) bf6_ray_scene_add_instance(v->scene, mesh, identity);
    }
    /* THE WHEEL / GEAR / AIRCRAFT PRESENTATION GRAPHS, again per class: cars ship
     * presex_<veh>_suspensionmovement (the flyer60 with a _1 suffix), helicopters
     * presex_helicopter_<veh>_wheelmovement, tanks presex_vehicle_<veh>_wheelmovement,
     * airplanes use either one combined landinggearandcontrolsurfaces partition or
     * TWO split partitions. Stopping at the first presentation candidate omitted the
     * F-16/JAS-39 control surfaces or gear entirely. */
    std::vector<std::string> graphs;
    if (!presentation_only) graphs.push_back(simex);
    /* THE DERIVED GRAPHS. Every vehicle folder ships derivedex_<class>_<vehicle> and
     * some ship more (derivedex_ptv_pedals, the Gepard's radar, the MH-47 ramp, turret
     * motion): graphs that derive channels from the drivetrain's for the presentation to
     * read, so they run after the simex and before the presex. BF6_NO_DERIVED leaves
     * them out, to compare. */
    if (!std::getenv("BF6_NO_DERIVED")) {
        const std::string pre = gdir + "/derivedex_";
        const int n = bf6_list_res(c, pre.c_str(), nullptr, 0);
        std::vector<bf6_asset> rows((size_t)std::max(n, 0));
        const int got = n > 0 ? std::clamp(bf6_list_res(c, pre.c_str(), rows.data(), n), 0, n) : 0;
        std::vector<std::string> derived;
        for (int i = 0; i < got; ++i) {
            const std::string name = rows[(size_t)i].name ? rows[(size_t)i].name : "";
            if (name.compare(0, pre.size(), pre) != 0 || name.find('/', pre.size()) != std::string::npos) continue;
            if (std::find(derived.begin(), derived.end(), name) == derived.end()) derived.push_back(name);
        }
        std::sort(derived.begin(), derived.end());
        graphs.insert(graphs.end(), derived.begin(), derived.end());
    }
    if (gcls == "airplane") {
        const std::string aircraft[] = {
            gdir + "/presex_" + gveh + "_controlsurfaces",
            gdir + "/presex_" + gveh + "_landinggear",
            gdir + "/presex_" + gveh + "_landinggearandcontrolsurfaces",
        };
        for (const std::string& cand : aircraft)
            if (have(cand)) graphs.push_back(cand);
    } else {
        const std::string cands[] = {
            gdir + "/presex_" + gveh + "_suspensionmovement_1",
            gdir + "/presex_" + gveh + "_suspensionmovement",
            gdir + "/presex_" + gcls + "_" + gveh + "_wheelmovement",
            gdir + "/presex_vehicle_" + gveh + "_wheelmovement",
        };
        for (const std::string& cand : cands)
            if (have(cand)) { graphs.push_back(cand); break; }
    }
    /* THE REST OF THE VEHICLE'S PRESENTATION GRAPHS. Each fl_veh_*_singlefeature_*
     * feature imports one presex graph: handlebars and steering wheels
     * (presex_vehicle_controller_<veh>), helicopter rotors (presex_<veh>_rotormovement),
     * turret and gunner parts. Every graph directly in the vehicle's folder is hosted,
     * except the damage-shader ones, which write material parameters, not bones.
     * Ability graphs live in subfolders and are not presentation. */
    /* AN EMPLACEMENT'S CONTROLLERS. With no drivetrain, what aims a Phalanx or an SSM
     * battery is its own simex_controller_* / simex_*_aiming graphs; they come first,
     * as a simex would. BF6_NO_CONTROLLERS leaves them out, to compare. */
    if (presentation_only && !std::getenv("BF6_NO_CONTROLLERS")) {
        const std::string pre = gdir + "/simex_";
        const int n = bf6_list_res(c, pre.c_str(), nullptr, 0);
        std::vector<bf6_asset> rows((size_t)std::max(n, 0));
        const int got = n > 0 ? std::clamp(bf6_list_res(c, pre.c_str(), rows.data(), n), 0, n) : 0;
        std::vector<std::string> ctl;
        for (int i = 0; i < got; ++i) {
            const std::string name = rows[(size_t)i].name ? rows[(size_t)i].name : "";
            if (name.compare(0, pre.size(), pre) != 0 || name.find('/', pre.size()) != std::string::npos) continue;
            if (name.find("audio") != std::string::npos) continue;
            if (std::find(ctl.begin(), ctl.end(), name) == ctl.end()) ctl.push_back(name);
        }
        std::sort(ctl.begin(), ctl.end());
        graphs.insert(graphs.begin(), ctl.begin(), ctl.end());
    }
    for (const std::string& pre : {gdir + "/presex_", gdir + "/expressions/presex_",
                                   root + "drone/" + gveh + "/expressions/presex_"}) {
        const int n = bf6_list_res(c, pre.c_str(), nullptr, 0);
        std::vector<bf6_asset> rows((size_t)std::max(n, 0));
        const int got = n > 0 ? std::clamp(bf6_list_res(c, pre.c_str(), rows.data(), n), 0, n) : 0;
        std::vector<std::string> extra;
        for (int i = 0; i < got; ++i) {
            const std::string name = rows[(size_t)i].name ? rows[(size_t)i].name : "";
            if (name.compare(0, pre.size(), pre) != 0) continue;
            if (name.find('/', pre.size()) != std::string::npos) continue;
            if (name.find("damageshader") != std::string::npos) continue;
            /* one wheel graph was picked above; a second spelling would drive them twice */
            if (name.find("suspensionmovement") != std::string::npos ||
                name.find("wheelmovement") != std::string::npos) continue;
            if (std::find(graphs.begin(), graphs.end(), name) != graphs.end()) continue;
            if (std::find(extra.begin(), extra.end(), name) != extra.end()) continue;
            extra.push_back(name);
        }
        std::sort(extra.begin(), extra.end());
        graphs.insert(graphs.end(), extra.begin(), extra.end());
    }
    /* BF6_SKIP_GRAPHS=<substr>[,<substr>...]: leave matching graphs out, to test what one
     * graph contributes (a diagnostic, never a default). */
    if (const char* sk = std::getenv("BF6_SKIP_GRAPHS")) {
        std::vector<std::string> subs;
        for (std::string t = sk; !t.empty();) {
            const size_t comma = t.find(',');
            subs.push_back(t.substr(0, comma));
            t = comma == std::string::npos ? std::string() : t.substr(comma + 1);
        }
        graphs.erase(std::remove_if(graphs.begin(), graphs.end(), [&](const std::string& g) {
            for (const std::string& sub : subs) if (!sub.empty() && g.find(sub) != std::string::npos) return true;
            return false;
        }), graphs.end());
    }
    if (graphs.empty()) return fail("no drivetrain or presentation graph: " + sim_a);
    if (std::getenv("BF6_VEHICLE_GRAPHS"))
        for (const std::string& g : graphs) std::fprintf(stderr, "vehicle graph %s\n", g.c_str());
    const std::string ske = dir + "/art/ske_veh_" + cls + "_" + veh + "_base";
    std::string e;
    if (!v->sim.open(c, exe, graphs, ske, bf6__vehicle_trace, v.get(), e)) {
        bf6_ray_scene_free(v->scene);
        return fail(e);
    }
    /* THE SUB-SKELETONS: a tank's tracks, the Bradley's TOW launcher, a boat's mounts.
     * Their channels (Track_Joint_*, TOW_Mount) are bound by the graphs and had nowhere
     * to land. Cinematic and first-person rigs are not the vehicle's. */
    if (!std::getenv("BF6_NO_SUBSKELETONS")) {
        const std::string pre = dir + "/art/ske_veh_" + cls + "_" + veh + "_";
        const int n = bf6_list_ebx(c, pre.c_str(), nullptr, 0);
        std::vector<bf6_asset> rows((size_t)std::max(n, 0));
        const int got = n > 0 ? std::clamp(bf6_list_ebx(c, pre.c_str(), rows.data(), n), 0, n) : 0;
        std::vector<std::string> subs;
        for (int i = 0; i < got; ++i) {
            const std::string name = rows[(size_t)i].name ? rows[(size_t)i].name : "";
            if (name.compare(0, pre.size(), pre) != 0 || name == ske) continue;
            if (name.find('/', pre.size()) != std::string::npos) continue;
            if (name.find("_cin") != std::string::npos || name.find("_1p") != std::string::npos) continue;
            if (std::find(subs.begin(), subs.end(), name) == subs.end()) subs.push_back(name);
        }
        std::sort(subs.begin(), subs.end());
        for (const std::string& sub : subs) v->sim.add_skeleton(c, sub);
    }
    /* THE VEHICLE'S OWN ANIMATION LAYER. A few vehicles animate parts with clips, not
     * graphs: the F-14's landing gear, gear doors and lower arms, the F/A-18's gear
     * and hatches. Their top layer (mm.vehicle.top.<class>.<vehicle>.mmlayer) imports
     * phase controllers (.pc), each scrubbing a clip picked per vehicle by its .ec by
     * a game state (mm.vehicle.<part>.timescale.float) - and the vehicle's graphs write
     * that phase as Anim.<Part>.Float (LandingGear, LeftRightDoors, Hatches: both
     * sides name the same parts). The clip's DOFs are channel names, mapped to ids by
     * the public channel lists and to bones by the skeleton.
     * TWO CHOICES MADE BY NAME, stated: the .ec picks by the vehicle-name enum, whose
     * value for this vehicle is not read, so the clip whose path names this vehicle
     * (veh_<vehicle>_) is taken; and the rig is the shared airplane rig
     * (vehicle_airplane_common_01), the one whose DOF sets these clips bind through. */
    v->ctx = c;
    /* THE FAKEHINGES: the base rig's binding list (field 0x4CE93112: bone, hinge data,
     * weight). Per bone the engine sorts its entries by weight and takes the first at or
     * above a requested weight (FUN_140e10510); the requested weight is not read here, so
     * the lowest entry is taken. The data is laid out by the type's reflected layout. */
    if (!std::getenv("BF6_NO_FAKEHINGE")) {
        std::string err;
        std::vector<uint8_t> raw = c->src.get_ebx(ske, err);
        bf6::Ebx e(*c->types);
        if (!raw.empty() && e.parse(std::move(raw), err) && e.instance_count() > 0) {
            const bf6::EbxValue root_v = e.read_instance(0);
            const bf6::EbxValue* list = root_v.field(0x4CE93112u);
            std::map<int32_t, std::pair<float, int32_t>> pick;   /* bone -> (weight, instance) */
            if (list)
                for (const auto& item : list->items) {
                    const bf6::EbxValue* bone = item.field(0xB1C23DA7u);
                    const bf6::EbxValue* data = item.field(0x8176EE9Au);
                    const bf6::EbxValue* wt = item.field(0x9BBA073Cu);
                    if (!bone || !data || data->kind != bf6::EbxValue::Kind::InstanceRef || data->instance < 0) continue;
                    const int32_t b = bone->kind == bf6::EbxValue::Kind::Uint ? (int32_t)bone->u : (int32_t)bone->i;
                    const float w = wt ? (float)wt->f : 0.0f;
                    auto it = pick.find(b);
                    if (it == pick.end() || w < it->second.first) pick[b] = {w, data->instance};
                }
            for (const auto& kv : pick) {
                const bf6::EbxValue hv = e.read_instance((size_t)kv.second.second);
                const bf6::TypeLayout& lay = c->types->layout_full(hv.guid);
                if (!lay.valid) continue;
                std::map<uint32_t, uint32_t> bits;
                bf6__seed_value(*c->types, lay, hv, 0, bits, 0);
                bf6_vehicle::FakeHinge h;
                h.bone = kv.first;
                for (const auto& b : bits) { float f; std::memcpy(&f, &b.second, 4); h.d[b.first] = f; }
                v->hinges.push_back(std::move(h));
            }
            if (std::getenv("BF6_VEHICLE_GRAPHS"))
                std::fprintf(stderr, "fakehinges: %zu bones\n", v->hinges.size());
            if (std::getenv("BF6_HINGE_DEBUG") && !v->hinges.empty())
                for (const auto& kv : v->hinges.front().d)
                    std::fprintf(stderr, "  hinge0 +0x%X = %g\n", kv.first, kv.second);
        }
    }
    if (!std::getenv("BF6_NO_CLIP_LAYERS") && cls == "airplane") {
        const char* rig = "animations/kingston/global/rigging/vehicle_airplane_common_01.rig";
        const std::string key = "." + veh + ".mmlayer";
        const int n = bf6_list_ebx(c, "mm.vehicle.top.", nullptr, 0);
        std::vector<bf6_asset> rows((size_t)std::max(n, 0));
        const int got = n > 0 ? std::clamp(bf6_list_ebx(c, "mm.vehicle.top.", rows.data(), n), 0, n) : 0;
        const std::vector<std::string> channels = v->sim.channel_names();
        const auto& ids = bf6__public_channel_ids(c);
        for (int i = 0; i < got; ++i) {
            const std::string layer = rows[(size_t)i].name ? rows[(size_t)i].name : "";
            if (layer.size() < key.size() || layer.compare(layer.size() - key.size(), key.size(), key) != 0) continue;
            if (std::getenv("BF6_CLIP_DEBUG"))
                for (const std::string& p : bf6__ebx_import_paths(c, layer))
                    std::fprintf(stderr, "layer %s imports %s\n", layer.c_str(), p.c_str());
            for (const std::string& pc : bf6__ebx_import_paths(c, layer)) {
                if (pc.size() < 3 || pc.compare(pc.size() - 3, 3, ".pc") != 0) continue;
                std::string ec, state;
                for (const std::string& p : bf6__ebx_import_paths(c, pc)) {
                    if (p.size() >= 3 && p.compare(p.size() - 3, 3, ".ec") == 0) ec = p;
                    if (p.find(".timescale.float") != std::string::npos) state = p;
                }
                if (ec.empty() || state.empty()) continue;
                /* the part: mm.vehicle.<part>.timescale.float -> Anim.<Part>.Float */
                const size_t a = state.find("mm.vehicle."), b = state.find(".timescale");
                if (a == std::string::npos || b == std::string::npos || b <= a + 11) continue;
                const std::string part = state.substr(a + 11, b - a - 11);
                std::string phase;
                for (const std::string& ch : channels) {
                    std::string low = ch;
                    for (char& x : low) x = (char)std::tolower((unsigned char)x);
                    if (low.rfind("anim.", 0) != 0 || low.size() < 11 || low.compare(low.size() - 6, 6, ".float") != 0) continue;
                    const std::string mid = low.substr(5, low.size() - 11);
                    if (mid == part || mid + "s" == part) { phase = ch; break; }
                }
                if (phase.empty()) continue;
                std::string clip;
                for (const std::string& p : bf6__ebx_import_paths(c, ec))
                    if (p.find("veh_" + veh + "_") != std::string::npos) { clip = p; break; }
                if (clip.empty()) continue;
                bf6_anim_binding_stats st{};
                const int nb = bf6_anim_bindings(c, clip.c_str(), rig, ske.c_str(), nullptr, 0, &st);
                if (nb < 1) continue;
                std::vector<bf6_anim_binding> bind((size_t)nb);
                bf6_anim_bindings(c, clip.c_str(), rig, ske.c_str(), bind.data(), nb, &st);
                bf6_vehicle::ClipLayer L;
                L.phase = phase;
                L.clip = bf6_anim_clip_open(c, clip.c_str());
                if (!L.clip) continue;
                L.frames = L.clip->key_time_count;
                for (const auto& x : bind) {
                    if (x.component != BF6_ANIM_DOF_QUATERNION && x.component != BF6_ANIM_DOF_VECTOR3) continue;
                    if (x.channel < 0 || x.channel >= L.clip->channel_count) continue;
                    std::string dof = x.dof_name;
                    const size_t dot = dof.rfind('.');
                    if (dot != std::string::npos) dof = dof.substr(0, dot);
                    const auto id = ids.find(dof);
                    if (std::getenv("BF6_CLIP_DEBUG"))
                        std::fprintf(stderr, "  dof %s id %s%08X maps %d\n", dof.c_str(),
                                     id == ids.end() ? "none " : "", id == ids.end() ? 0u : id->second,
                                     id == ids.end() ? 0 : (int)v->sim.maps_bone(id->second));
                    /* The skeleton keys bones by the id the GRAPHS bind a channel under,
                     * which is not the public list's id for the same name (Hatch1:
                     * 0xEE8DA98B bound, 0xC00C0E2B listed). A DOF no graph binds has
                     * no bone to land on here. */
                    uint32_t bone_channel = 0;
                    if (!v->sim.bone_channel_named(dof, bone_channel) || !v->sim.maps_bone(bone_channel)) continue;
                    L.dofs.push_back({x.channel, x.component, bone_channel});
                }
                if (std::getenv("BF6_VEHICLE_GRAPHS"))
                    std::fprintf(stderr, "clip layer %s <- %s, %zu of %d DOFs on bones\n",
                                 clip.c_str(), phase.c_str(), L.dofs.size(), nb);
                if (L.dofs.empty()) { bf6_free(c, L.clip); continue; }
                v->clip_layers.push_back(std::move(L));
            }
        }
    }
    /* WHAT THE ENGINE BINDS AT LOAD. A graph's curves are in its EBX, not in its
     * expression blob, and the graph's own typed slot groups say which slot takes
     * which; patched into the pool the copies read from. See vehicle_typed_seed.inc. */
    for (const std::string& gname : graphs) bf6__seed_typed_values(c, gname, v->sim);
    /* wheel mounts from the rig's locators */
    bf6_skeleton* sk = bf6_skeleton_read(c, ske.c_str());
    const char* loc[4] = {"loc_Wheel_FrontLeft", "loc_Wheel_FrontRight", "loc_Wheel_RearLeft", "loc_Wheel_RearRight"};
    int found = 0;
    for (int b = 0; sk && b < sk->bone_count; ++b)
        for (int w = 0; w < 4; ++w)
            if (sk->bones[b].name && std::strcmp(sk->bones[b].name, loc[w]) == 0) {
                for (int i = 0; i < 3; ++i) v->dyn.p.wheel[w].mount[i] = sk->bones[b].model[9 + i];
                ++found;
            }
    /* WHICH WAY THE HULL RUNS. A boat's forces are only in the right place if the
     * frame is, and the rig is the one thing that says so without assumption: the
     * span of its bones along each axis is the vehicle's own length, width and
     * height. Printed rather than assumed, because the hull surface this library
     * builds picks an axis for the description's "length". */
    if (sk && std::getenv("BF6_RIG_SPAN")) {
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        for (int b = 0; b < sk->bone_count; ++b)
            for (int i = 0; i < 3; ++i) {
                const float p = sk->bones[b].model[9 + i];
                if (p < lo[i]) lo[i] = p;
                if (p > hi[i]) hi[i] = p;
            }
        std::fprintf(stderr, "rig span: x %.2f..%.2f (%.2f)  y %.2f..%.2f (%.2f)  "
                             "z %.2f..%.2f (%.2f)  over %d bones\n",
                     lo[0], hi[0], hi[0] - lo[0], lo[1], hi[1], hi[1] - lo[1],
                     lo[2], hi[2], hi[2] - lo[2], sk->bone_count);
    }
    if (sk) bf6_free(c, sk);
    /* THE RIG LOCATORS ARE NOT THE WHEELS ANY MORE. Graph physics takes each wheel's
     * position from the WheelConfig the graph carries (InitialPosition), so a vehicle
     * whose rig has no loc_Wheel_* - thebeast is a single-player prop car with no
     * skeleton at all - still drives. The locators are only the stand-in dynamics'
     * mounts, so refuse only there. */
    if (found != 4) {
        if (std::getenv("BF6_STANDIN_PHYSICS")) {
            bf6_ray_scene_free(v->scene);
            return fail("vehicle rig has no four loc_Wheel_* locators (stand-in physics needs them)");
        }
        v->no_wheel_locators = true;
    }
    bf6__vehicle_blueprint(c, dir + "/vb_veh_" + cls + "_" + veh, v->dyn.p);
    bf6__vehicle_physics(c, dir, cls, veh, v.get());
    v->dyn.init();
    if (std::getenv("BF6_DYN_DEBUG")) std::fprintf(stderr, "%s", v->dyn.describe_params().c_str());
    return v.release();
}

void bf6_vehicle_set_water(bf6_vehicle* v, float height, int32_t present) {
    if (!v) return;
    v->water_height = height;
    v->water_present = present != 0;
}

/* Pitch and roll cyclic, reaching the graph as InputPitch and InputRoll - both bound
 * channels on every helicopter, confirmed by listing them. Range follows the other inputs:
 * nominally -1..1, and the graph's own filters decide what it does with that. */
void bf6_vehicle_set_cyclic(bf6_vehicle* v, float pitch, float roll) {
    if (!v) return;
    v->cyclic_pitch = pitch;
    v->cyclic_roll = roll;
}

void bf6_vehicle_set_aim(bf6_vehicle* v, float yaw, float pitch) {
    if (!v) return;
    v->aim_yaw = yaw;
    v->aim_pitch = pitch;
}

void bf6_vehicle_set_rws_aim(bf6_vehicle* v, int32_t index, float yaw, float pitch) {
    if (!v || index < 0 || index > 3) return;
    v->rws_yaw[index] = yaw;
    v->rws_pitch[index] = pitch;
}

void bf6_vehicle_set_seat(bf6_vehicle* v, int32_t seat, int32_t occupied) {
    if (!v || seat < 0 || seat > 7) return;
    v->seat_occupied[seat] = occupied != 0;
}

void bf6_vehicle_set_boost(bf6_vehicle* v, float boost) {
    if (!v) return;
    v->boost = boost > 0.5f;
}

int32_t bf6_vehicle_bone_count(bf6_vehicle* v) {
    return v ? (int32_t)v->sim.presented_bones().size() : 0;
}

int32_t bf6_vehicle_bone(bf6_vehicle* v, int32_t index,
                         char* name, int32_t name_capacity, float local16[16]) {
    if (!v || index < 0 || (size_t)index >= v->sim.presented_bones().size()) return 0;
    const bf6::VehicleSim::PresentedBone& bone = v->sim.presented_bones()[(size_t)index];
    if (name && name_capacity > 0)
        std::snprintf(name, (size_t)name_capacity, "%s", bone.name.c_str());
    if (local16) std::memcpy(local16, bone.local.data(), 64);
    return 1;
}

int32_t bf6_vehicle_motion_report(bf6_vehicle* v, char* out, int32_t cap) {
    if (!v) return 0;
    const std::string j = v->sim.motion_json();
    if (out && cap > 0) {
        const size_t n = std::min(j.size(), (size_t)cap - 1);
        std::memcpy(out, j.data(), n);
        out[n] = 0;
    }
    return (int32_t)j.size();
}

void bf6_vehicle_set_pose(bf6_vehicle* v, const float pos[3], const float quat[4]) {
    if (!v) return;
    for (int i = 0; i < 3; ++i) { v->dyn.pos[i] = pos[i]; v->dyn.vel[i] = 0; v->dyn.angvel[i] = 0; }
    for (int i = 0; i < 4; ++i) { v->dyn.quat[i] = quat[i]; v->dyn.omega[i] = 0; }
}

int32_t bf6_vehicle_step(bf6_vehicle* v, const float* in, float* out) {
    if (!v || !in || !out) return 0;
    const float throttle = in[0], brake = in[1], steer = in[2], dt = in[4] > 0 ? in[4] : 1.0f / 60.0f;
    const bool handbrake = in[3] > 0.5f;
    bf6::VehicleSim& s = v->sim;
    bf6::VehicleDynamics& d = v->dyn;
    s.set_bool("EntryActive_Driver", true);
    /* THE DRIVER AS A CHARACTER. The graphs' entity queries look for soldiers near the
     * vehicle, and the driver is one: alive (MM.HealthState 1), seated in the vehicle.
     * Seated at the body's origin with a soldier's bounds (0.8 x 1.8 x 0.8 m, standing
     * extents - the seat locator is not read yet), which only matters against a
     * search radius, and those are metres. Entity id 2; the vehicle is entity 1. */
    {
        bf6::expression::StateHost::Character driver;
        driver.id = 2u;   /* the vehicle is 1 (WorldHost self id) */
        driver.center[0] = d.pos[0];
        driver.center[1] = d.pos[1] + 0.9f;
        driver.center[2] = d.pos[2];
        driver.half[0] = 0.4f; driver.half[1] = 0.9f; driver.half[2] = 0.4f;
        driver.active = true;
        driver.health_state = 1;
        /* the door the driver enters by: LeftDoor1 (2), the value the door graphs test
         * seat 0's occupant against (the flyer60's per-door constant for seat 0); the
         * seat's own entry data is not read */
        driver.open_door = 2;
        /* BF6_NO_DRIVER: an empty vehicle, to check the queries find the driver */
        const bool seated = v->seat_occupied[0] && !std::getenv("BF6_NO_DRIVER");
        s.state_mut().set_characters(seated ? std::vector<bf6::expression::StateHost::Character>{driver}
                                            : std::vector<bf6::expression::StateHost::Character>{});
        /* the seats: the driver in seat 0 when seated; other seats have no one to fill
         * them offline (entities 3.. would be passengers) */
        std::vector<uint32_t> seats(8, 0u);
        if (seated) seats[0] = driver.id;
        s.state_mut().set_seats(seats);
    }
    /* BF6_HAS_PILOT=1: a helicopter's state list declares HasPilot beside Autopilot,
     * AutohoverEnabled and a Roll PID with its own errorSum and lastError, and nothing
     * here had ever seeded it - so offline it reads false. Three of the five
     * helicopters carry a small negative cyclic Roll that the eurocopter does not
     * (-0.05 against exactly 0), it comes from field 2 of a PID struct, and it WINDS UP
     * rather than settling, which is what an integrator does when its feedback is
     * missing. This tests whether telling the graph a pilot is aboard stops it. */
    /* CORRECTED: BF6_HAS_PILOT NEVER DID ANYTHING. set_bool addresses a bound CHANNEL by
     * name hash and returns false for a name that is not one, and HasPilot is a state CELL
     * inside the graph's Autopilot struct, not a channel. Measured, the flag changes nothing
     * to six decimal places on all five helicopters. It is kept only because its return now
     * says so out loud rather than failing silently. */
    if (std::getenv("BF6_HAS_PILOT") && !s.set_bool("HasPilot", true))
        std::fprintf(stderr, "BF6_HAS_PILOT: no channel named HasPilot; it is a state cell, "
                             "so this flag does nothing\n");
    /* BF6_LIST_CHANNELS=1: every channel name the graph binds, which is the only set
     * set_float and set_bool can reach. Guessing a name here silently does nothing, so the
     * list has to be printable. */
    if (std::getenv("BF6_LIST_CHANNELS") && v->time < 0.02) {
        const std::map<uint32_t, std::string> ch = s.channel_name_map();
        std::fprintf(stderr, "bound channels (%zu):\n", ch.size());
        for (const auto& kv : ch) std::fprintf(stderr, "  %08X %s\n", kv.first, kv.second.c_str());
    }
    s.set_float("InputThrottle", throttle);
    s.set_float("InputBrake", brake);
    s.set_float("InputYaw", steer);
    /* These are presentation inputs supplied by the game's vehicle-control
     * component, not outputs written by simex. The aircraft presex graphs bind
     * them explicitly to drive elevator, aileron, and rudder animation. */
    s.set_float("Flap_Elevator", v->cyclic_pitch);
    s.set_float("Flap_Aileron", v->cyclic_roll);
    s.set_float("Flap_Rudder", steer);
    s.set_bool("InputSprint", v->boost);
    s.set_float("InputPitch", v->cyclic_pitch);
    s.set_float("Aim_Driver_Yaw", v->aim_yaw);
    s.set_float("Aim_Driver_Pitch", v->aim_pitch);
    for (int i = 0; i < 4; ++i) {
        const std::string rws = "Aim_RWS_" + std::to_string(i) + "_";
        s.set_float(rws + "Yaw", v->rws_yaw[i]);
        s.set_float(rws + "Pitch", v->rws_pitch[i]);
    }
    s.set_float("InputRoll", v->cyclic_roll);
    s.set_bool("Handbrake", handbrake);
    s.set_time(v->time);
    float rows[16];
    d.root_rows(rows);
    s.set_root(rows);
    const char* wch[4] = {"AngularVelocity_Wheel_FL", "AngularVelocity_Wheel_FR",
                          "AngularVelocity_Wheel_RL", "AngularVelocity_Wheel_RR"};
    static const bool graph_wheels = std::getenv("BF6_STANDIN_PHYSICS") == nullptr;   /* the car's own graph physics; BF6_STANDIN_PHYSICS for the old stand-in */
    /* In graph physics the graph owns the wheel speed (its tyre and spin functions
     * update it and it persists it in replicated state); feeding the stand-in
     * dynamics' spin back in would overwrite it. */
    if (!graph_wheels)
        for (int w = 0; w < 4; ++w) {
            s.set_float(wch[w], d.omega[w]);
            s.state_mut().set_wheel_state(w, d.omega[w], d.fz[w] > 0.0f);
        }
    /* THE WHEEL SPEEDS THE PRESENTATION READS. Native code publishes each wheel's
     * speed on AngularVelocity_Wheel_*; the suspension and wheel graphs spin the hubs
     * from it. In graph physics the value is the graph's own last tyre speed per wheel
     * (written back into d.omega after the previous tick), so this publishes the
     * drivetrain's answer rather than a second model. Four-wheel spellings only: the
     * aircraft FC/RL/RR and the tank tracks are not mapped to a tyre section yet. */
    else if (!v->no_wheel_locators)
        for (int w = 0; w < 4; ++w) s.set_float(wch[w], d.omega[w]);
    /* the positional wheel and track speeds (a car's four keep the locator path above) */
    for (const auto& kv : v->wheel_spin) {
        const bool car_wheel = !v->no_wheel_locators &&
            (kv.first == wch[0] || kv.first == wch[1] || kv.first == wch[2] || kv.first == wch[3]);
        if (!car_wheel) s.set_float(kv.first, kv.second);
    }
    /* WHAT IS KNOWN ABOUT A VEHICLE IN THE EDITOR. These are game-state channels whose
     * value here is not in doubt: a vehicle placed on a map at full health, driven by
     * the local player, not in a level-insertion sequence, not a free-to-play call-in,
     * not on a scripted exotic spline, awake. Channels whose meaning is not yet proven
     * (Vehicle Damage State's enum, Airborne State, TeamId) are left unsupplied. */
    s.set_float("Vehicle Health Normalized", 1.0f);
    s.set_bool("Vehicle Is Wreck", false);
    s.set_bool("EntryActive_Driver Local", true);
    s.set_bool("IsInLevelInsertion", false);
    s.set_bool("HoldVehicleInInsertion", false);
    s.set_bool("IsF2PCallIn", false);
    s.set_bool("IsOnExoticSpline", false);
    s.set_float("ExoticVehicleThrottle", 0.0f);
    s.set_bool("IsSleeping", false);
    s.set_bool("IsPlayingExternalState", false);
    s.set_bool("IsPlayingMainExternalState", false);
    /* player inputs this binding has no key for: none held */
    s.set_float("InputCrawlSpeed", 0.0f);
    s.set_bool("Input Freelook", false);
    s.set_bool("ThermalCamera_Active", false);
    s.set_float("InputJump", 0.0f);
    s.set_float("InputStrafe", 0.0f);
    /* nobody in the open seats: the driver is the only occupant */
    s.set_bool("EntryActive_OpenSeat_Left", false);
    s.set_bool("EntryActive_OpenSeat_Right", false);
    /* the default load-out shows these parts (a customization hides them) */
    s.set_bool("HideHMG", false);
    s.set_bool("HideRopeHolder", false);
    /* the driver seat's aim pitch target, by name the same aim as Aim_Driver_Pitch */
    s.set_float("AimTargetPitch_0_Driver", v->aim_pitch);
    /* THE INPUTS A DRIVETRAIN GRAPH DOES NOT FILTER. A car's simex writes
     * VehicleThrottle / VehicleBrake from InputThrottle / InputBrake; a motorcycle's does
     * not, and native code supplies them. Passed through unfiltered here (the native
     * smoothing is not modelled); a graph that does write them overwrites this. */
    s.set_float("VehicleThrottle", throttle);
    s.set_float("VehicleBrake", brake);
    s.set_bool("HandbrakeActive", handbrake);
    if (!v->water_present) s.set_bool("Vehicle.IsInWater", false);
    /* HEIGHT ABOVE WHAT IS UNDER IT: one ray straight down from the body through the
     * same ground the wheels trace (and the water plane when there is one). */
    {
        const double from[3] = {d.pos[0], d.pos[1] + 0.5, d.pos[2]};
        const double to[3] = {d.pos[0], d.pos[1] - 2000.0, d.pos[2]};
        double hit[3] = {}, nrm[3] = {};
        float h = 2000.0f;
        if (bf6__vehicle_trace(v, from, to, hit, nrm)) h = d.pos[1] - (float)hit[1];
        s.set_float("Vehicle DistanceToTerrain", h);
        const float hw = v->water_present ? std::min(h, d.pos[1] - v->water_height) : h;
        s.set_float("Vehicle DistanceToTerrainOrWater", hw);
    }
    s.set_vec3("LinearVelocity", d.vel, 0);
    s.set_vec3("AngularVelocity", d.angvel, 0);
    /* GRAPH PHYSICS: the car's own wheel functions (ray, suspension, tyre, spin) run
     * inside the drivetrain graph against a snapshot of this body and sum their
     * velocity changes into LinearAcceleration / AngularAcceleration; this side
     * only integrates them. The default; BF6_STANDIN_PHYSICS selects the old stand-in
     * tyre/suspension model. */
    static const bool graph_physics = std::getenv("BF6_STANDIN_PHYSICS") == nullptr;   /* the car's own graph physics; BF6_STANDIN_PHYSICS for the old stand-in */
    if (graph_physics) {
        bf6::expression::WheelBody b;
        float qi[4] = {-d.quat[0], -d.quat[1], -d.quat[2], d.quat[3]};
        float vl[3], wl[3];
        bf6__qrot(qi, d.vel, vl);
        bf6__qrot(qi, d.angvel, wl);
        for (int i = 0; i < 3; ++i) {
            b.v[i] = vl[i]; b.w[i] = wl[i]; b.com[i] = d.p.com[i];
            b.inv_inertia[i] = 1.0f / (d.p.mass * d.p.inertia_per_kg[i]);
            b.pos[i] = d.pos[i];
        }
        for (int i = 0; i < 4; ++i) b.quat[i] = d.quat[i];
        b.mass = d.p.mass;
        b.gravity[1] = -d.p.gravity;
        b.water_height = v->water_height;
        b.water = v->water_present;
        /* WHAT A BOAT ASKS THE WORLD ABOUT THE WATER. Its graph binds
         * Vehicle.IsInWater and the height of bow and stern above the surface and
         * gates its engine on them; with nothing bound the cb90 floated on the hull
         * force and produced no thrust at all, because as far as the graph was
         * concerned its propellers were in the air. The bow and stern heights are
         * taken along the body's own forward axis, so they follow it as it pitches.
         * The half-length is the hull's, from the hull description the graph passes
         * to its own hull operator. */
        if (v->water_present) {
            const float up = d.pos[1] - v->water_height;
            float fwd[3] = {0, 0, 1}, fw[3];
            bf6__qrot(d.quat, fwd, fw);
            const float half = s.wheel_ops().hull_half_length();
            s.set_bool("Vehicle.IsInWater", up < 1.0f);
            s.set_float("Vehicle DistanceToWater", up);
            s.set_float("Bow_HeightOverWater", up + fw[1] * half);
            s.set_float("Stern_HeightOverWater", up - fw[1] * half);
        }
        s.wheel_ops().set_body(b);
        s.wheel_ops().clear_tick();
        const float zero[3] = {0, 0, 0};
        s.set_vec3("LinearAcceleration", zero, 0);
        s.set_vec3("AngularAcceleration", zero, 0);
    } else {
        s.set_vec3("LinearAcceleration", d.last_accel, 0);
    }
    s.tick();
    /* THE CLIP LAYERS, after the graphs have written this tick's phases: each clip at
     * phase x its last frame, each DOF over its bone's rest pose (rotation and/or
     * translation), then the bones are published again with these poses in them. */
    /* THE FAKEHINGES, after the graphs and clips have posed their parents. */
    if (!v->hinges.empty() && dt > 1e-6f) {
        float root[16];
        d.root_rows(root);
        for (auto& h : v->hinges) {
            float rest[16], parent_model[16], model[16], B[16];
            if (!s.rig_rest_local(h.bone, rest)) continue;
            /* THE HINGE SWINGS WHATEVER POSE THE GRAPHS GAVE THE BONE. A door graph opens
             * the flyer60's doors by writing the door bone; a hinge built on the rest pose
             * shut them again every tick. The bone's current local is the base when a
             * graph wrote it this tick (it differs from the hinge's own last write),
             * otherwise the rest pose, so the swing never compounds on itself. */
            {
                float cur[16];
                if (s.rig_local(h.bone, cur) && !(h.wrote && std::memcmp(cur, h.last_local, 64) == 0))
                    std::memcpy(rest, cur, 64);
            }
            const int32_t par = s.rig_parent(h.bone);
            if (par >= 0 && s.rig_model(par, parent_model)) fh::lt(rest, parent_model, model);
            else std::memcpy(model, rest, 64);
            fh::lt(model, root, B);                               /* the driving world transform */
            /* FUN_142b69620: velocity of the driving origin and its change */
            const fh::V t = {B[12], B[13], B[14]};
            fh::V vel = {0, 0, 0}, dv = {0, 0, 0};
            if (h.history) {
                vel = fh::mul(fh::sub(t, {h.prev_t[0], h.prev_t[1], h.prev_t[2]}), 1.0f / dt);
                dv = fh::sub(vel, {h.prev_v[0], h.prev_v[1], h.prev_v[2]});
                if (fh::dot(dv, dv) < 0.01f && fh::dot(vel, vel) < 0.01f) vel = dv = {0, 0, 0};
            }
            h.prev_t[0] = t.x; h.prev_t[1] = t.y; h.prev_t[2] = t.z;
            h.prev_v[0] = vel.x; h.prev_v[1] = vel.y; h.prev_v[2] = vel.z;
            const bool had = h.history;
            h.history = true;
            if (!had) continue;
            /* FUN_142b67fa0 */
            const fh::V axis = fh::norm({h.f(0x50), h.f(0x54), h.f(0x58)});
            const fh::V ext = fh::norm({h.f(0x60), h.f(0x64), h.f(0x68)});
            const fh::V pivot = {h.f(0x40), h.f(0x44), h.f(0x48)};
            const float theta = h.theta, oldw = h.w;
            float A[16];
            fh::axis_pivot(axis, pivot, theta, A);
            const fh::V tangent = fh::dir(B, fh::dir(A, fh::cross(axis, ext)));
            const float inertial = fh::dot(fh::mul(dv, -1.0f / dt), tangent);
            const float g_along = fh::dot({0.0f, -9.82f, 0.0f}, tangent);
            float progressive = 1.0f;
            if (h.f(0x7c) != 0.0f) {
                const float limit = theta <= 0.0f ? h.f(0x74) : h.f(0x84);
                const float denom = std::max(std::fabs(limit * 0.017453292f), 1.0f);
                progressive = std::exp((std::fabs(theta) / denom) * std::log(h.f(0x7c)));
            }
            const float div = h.f(0x78) != 0.0f ? h.f(0x78) : 1.0f;
            const float dw = ((h.f(0x2c) * g_along + inertial) / div - theta * h.f(0x70) * progressive) * dt;
            float w = (1.0f - h.f(0x80)) * (oldw + dw);
            if (std::fabs(w) < h.f(0x30) + 0.01f) w = 0.0f;
            float next = theta + (oldw + w) * 0.5f * dt + dw * dt * 0.5f;
            const float max_speed = std::max(h.f(0x20), 0.0f);
            if (max_speed > 0.0f && std::fabs(next - theta) > max_speed * dt)
                next = theta + (next > theta ? 1.0f : -1.0f) * max_speed * dt;
            if (next > h.f(0x84) * 0.017453292f) { next = h.f(0x84) * 0.017453292f; w *= h.f(0x24) - 1.0f; }
            else if (next < h.f(0x74) * 0.017453292f) { next = h.f(0x74) * 0.017453292f; w *= h.f(0x38) - 1.0f; }
            if (!std::isfinite(next) || !std::isfinite(w)) { next = 0.0f; w = 0.0f; }
            h.w = w;
            h.theta = next;
            float H[16], local[16];
            fh::axis_pivot(axis, pivot, next, H);
            fh::lt(H, rest, local);                               /* hinge, then the rest pose */
            s.write_rig_local(h.bone, local);
            std::memcpy(h.last_local, local, 64);
            h.wrote = true;
        }
        if (v->clip_layers.empty()) s.publish_bones();
    }
    if (!v->clip_layers.empty()) {
        std::map<uint32_t, std::pair<const float*, const float*>> pose;   /* quat, translation */
        std::vector<std::vector<float>> samples;
        samples.reserve(v->clip_layers.size());
        for (const auto& layer : v->clip_layers) {
            bool known = false;
            float phase = s.get_float(layer.phase, &known);
            if (!known || !layer.clip || layer.frames < 1) continue;
            phase = std::max(0.0f, std::min(1.0f, phase));
            samples.emplace_back((size_t)layer.clip->channel_count * 4, 0.0f);
            std::vector<float>& buf = samples.back();
            if (!bf6_anim_clip_sample_time(v->ctx, layer.clip, phase * (float)(layer.frames - 1), 0, buf.data()))
                continue;
            for (const auto& d : layer.dofs) {
                const float* p = buf.data() + (size_t)d.channel * 4;
                if (d.component == BF6_ANIM_DOF_QUATERNION) pose[d.bone_channel].first = p;
                else pose[d.bone_channel].second = p;
            }
        }
        for (const auto& kv : pose) {
            float local[16];
            if (!s.bone_rest_local(kv.first, local)) continue;
            if (kv.second.first) {
                float m9[9];
                bf6ant::rows_from_quat(kv.second.first, m9);
                for (int r = 0; r < 3; ++r)
                    for (int col = 0; col < 3; ++col) local[r * 4 + col] = m9[r * 3 + col];
            }
            if (kv.second.second)
                for (int col = 0; col < 3; ++col) local[12 + col] = kv.second.second[col];
            s.write_bone_local(kv.first, local);
        }
        s.publish_bones();
    }
    if (graph_physics) {
        float la[3], aa[3];
        const bool kl = s.get_vec3("LinearAcceleration", la, 0);
        const bool ka = s.get_vec3("AngularAcceleration", aa, 0);
        if (std::getenv("BF6_ACCEL_DEBUG") && v->time < 0.02)
            std::fprintf(stderr, "channels: LinearAcceleration (%g %g %g) known %d, "
                                 "AngularAcceleration (%g %g %g) known %d\n",
                         la[0], la[1], la[2], (int)kl, aa[0], aa[1], aa[2], (int)ka);
        if (const char* recs = std::getenv("BF6_GRAPH_REC"))
            if (std::fabs(v->time - (std::getenv("BF6_GRAPH_REC_T") ? std::atof(std::getenv("BF6_GRAPH_REC_T")) : 2.0)) < 0.009)
                for (const char* p = recs; *p;) {
                    char* end = nullptr;
                    const unsigned long r = std::strtoul(p, &end, 16);
                    if (end == p) break;
                    std::fprintf(stderr, "%s", s.record_info((uint32_t)r).c_str());
                    p = *end ? end + 1 : end;
                }
        /* BF6_POOL_DUMP=<graph name>:<hex>[,<hex>...]: the on-disk pool words (8 bytes)
         * at those offsets and the loader's patch, once */
        if (const char* pd = std::getenv("BF6_POOL_DUMP"))
            if (v->time < 0.02) {
                const std::string spec = pd;
                const size_t colon = spec.rfind(':');
                const std::string gname = spec.substr(0, colon);
                const bf6::expression::Graph* gr = s.graph_named(gname);
                for (size_t p = colon + 1; gr && p < spec.size();) {
                    const size_t comma = spec.find(',', p);
                    const uint32_t off = (uint32_t)std::strtoul(spec.substr(p, comma - p).c_str(), nullptr, 16);
                    uint32_t w0 = 0, w1 = 0, patched = 0;
                    if (off + 8 <= gr->constant_pool.size()) {
                        std::memcpy(&w0, gr->constant_pool.data() + off, 4);
                        std::memcpy(&w1, gr->constant_pool.data() + off + 4, 4);
                    }
                    const bool has = s.pool_patch(gname, off, patched);
                    std::fprintf(stderr, "pool 0x%X: %08X %08X%s", off, w0, w1, has ? "" : "\n");
                    if (has) std::fprintf(stderr, "  patched %08X\n", patched);
                    if (comma == std::string::npos) break;
                    p = comma + 1;
                }
            }
        /* BF6_LIST_RECS=<graph substr>:<lo hex>-<hi hex>: the records in that range, once */
        if (const char* ls = std::getenv("BF6_LIST_RECS"))
            if (v->time < 0.02) {
                char gname[128] = {};
                unsigned lo = 0, hi = 0;
                if (std::sscanf(ls, "%127[^:]:%x-%x", gname, &lo, &hi) == 3)
                    std::fprintf(stderr, "%s", s.list(gname, lo, hi).c_str());
            }
        /* BF6_CHANNEL_USE=<name>: once, every record that names that channel and what follows */
        if (const char* cu = std::getenv("BF6_CHANNEL_USE"))
            if (v->time < 0.02) std::fprintf(stderr, "%s", s.readers(cu).c_str());
        /* BF6_CHANNEL_SRC=<name>: at BF6_GRAPH_REC_T, who writes that channel and from what */
        if (const char* ch = std::getenv("BF6_CHANNEL_SRC"))
            if (std::fabs(v->time - (std::getenv("BF6_GRAPH_REC_T") ? std::atof(std::getenv("BF6_GRAPH_REC_T")) : 2.0)) < 0.009)
                std::fprintf(stderr, "%s", s.writers(ch).c_str());
        if (const char* src = std::getenv("BF6_GRAPH_SRC"))
            if (std::fabs(v->time - (std::getenv("BF6_GRAPH_REC_T") ? std::atof(std::getenv("BF6_GRAPH_REC_T")) : 2.0)) < 0.009)
                std::fprintf(stderr, "%s", s.sources((uint32_t)std::strtoul(src, nullptr, 16)).c_str());
        if (std::getenv("BF6_GRAPH_PHYSICS_REPORT") && (v->time < 0.02 || std::fabs(v->time - std::atof(std::getenv("BF6_GRAPH_PHYSICS_REPORT"))) < 0.009))
            std::fprintf(stderr, "%s%s%s", s.report().c_str(), s.why("LinearAcceleration").c_str(),
                         s.why("AngularAcceleration").c_str());
        if (std::getenv("BF6_GRAPH_PHYSICS_CH") && v->time < 0.02)
            for (const auto& kv : s.state().channels())
                if ((uint32_t)kv.first == 0x4381D143u || (uint32_t)kv.first == 0x1FC6ADB7u) {
                    float f[4] = {0, 0, 0, 0};
                    std::memcpy(f, kv.second.data(), kv.second.size() < 16 ? kv.second.size() : 16);
                    std::fprintf(stderr, "channel %08X mode %u bytes %zu = %g %g %g %g\n", (uint32_t)kv.first,
                                 (uint32_t)(kv.first >> 32), kv.second.size(), f[0], f[1], f[2], f[3]);
                }
        if (std::getenv("BF6_GRAPH_PHYSICS_DEBUG"))
            std::fprintf(stderr, "t %.3f lin %d (%.3f %.3f %.3f) ang %d (%.3f %.3f %.3f) rays %d/%d tyres %zu\n",
                         v->time, kl, la[0], la[1], la[2], ka, aa[0], aa[1], aa[2],
                         s.wheel_ops().ray_hits(), s.wheel_ops().rays(), s.wheel_ops().tyre_omegas().size());
        /* WHAT NATIVE CODE WRITES BACK: each wheel section's final tyre speed and
         * contact into the per-wheel frame state (0xFFFFii05 fields 3 and 4), which the
         * next tick's first tyre call reads as AngularVelocityWheel. Section k is
         * frame k (sections run in frame order). */
        {
            const auto& om = s.wheel_ops().tyre_omegas();
            const auto& ct = s.wheel_ops().tyre_contacts();
            const size_t per = om.size() >= 8 ? 2 : 1;
            for (int w = 0; w < 4 && (size_t)(w + 1) * per <= om.size(); ++w) {
                const size_t i = (size_t)(w + 1) * per - 1;
                d.omega[w] = om[i];
                s.state_mut().set_wheel_state(w, om[i], ct[i] != 0);
            }
            if (std::getenv("BF6_FRAME_READS"))
                for (const auto& r : s.state().unseeded_frame_reads())
                    std::fprintf(stderr, "unseeded frame read: frame %08X path %08X kind %u field %u\n",
                                 r.frame, r.path, r.kind, r.field);
            /* THE SAME WRITE-BACK FOR A VEHICLE WHOSE WHEELS HAVE NO INDEX. A car
             * reads each wheel's speed at an absolute 0xFFFFii05; a tank's tracks
             * read their own frame's field 3 instead (path 0xFFFFFFFF, "self"), one
             * read per track per tick, because a track is one part with many road
             * wheels rather than four separate wheels.
             *
             * So rather than assume a spelling, the cells the graph ASKED for and
             * nothing seeded are answered: each such frame, in the order it was
             * read, gets the wheel speed of its own share of this tick's tyre calls.
             * Without it every tick began with the tracks stationary, no slip ever
             * built, and the tank made no force at all. */
            std::vector<uint32_t> part_frames;
            for (const auto& r : s.state().unseeded_frame_reads())
                if (r.path == 0xFFFFFFFFu && r.kind == 0 && r.field == 3 &&
                    std::find(part_frames.begin(), part_frames.end(), r.frame) == part_frames.end())
                    part_frames.push_back(r.frame);
            if (!part_frames.empty() && !om.empty() && om.size() % part_frames.size() == 0) {
                const size_t per_part = om.size() / part_frames.size();
                for (size_t t = 0; t < part_frames.size(); ++t) {
                    const size_t i = (t + 1) * per_part - 1;   /* the part's last section */
                    uint32_t bits = 0;
                    std::memcpy(&bits, &om[i], 4);
                    s.state_mut().set_frame_cell(part_frames[t], 0xFFFFFFFFu, 0, 3, bits);
                    if (std::getenv("BF6_FRAME_READS"))
                        std::fprintf(stderr, "  seeded frame %08X field 3 with %g\n",
                                     part_frames[t], om[i]);
                }
            }
            s.state_mut().clear_unseeded_frame_reads();
            /* WHICH WHEEL EACH TYRE CALL IS, BY WHERE IT TOUCHES. A vehicle without the
             * four car locators names its wheels by position instead - an aircraft's
             * AngularVelocity_Wheel_FC / RL / RR, a tank's AngularVelocity_Track_Left and
             * _Right - and native code publishes each from that wheel's physics. The
             * graph's own tyre calls carry their contact points, so each call is placed
             * on the body (forward +Z, left +X, the core frame) and its speed goes to the
             * channel for that position; several calls on one wheel or track average. */
            {
                const auto& tp = s.wheel_ops().tyre_positions();
                const auto& tom = s.wheel_ops().tyre_omegas();
                v->wheel_spin.clear();
                if (!tp.empty() && tp.size() == tom.size()) {
                    const float qi[4] = {-d.quat[0], -d.quat[1], -d.quat[2], d.quat[3]};
                    std::vector<std::array<float, 3>> loc(tp.size());
                    float zmin = 1e9f, zmax = -1e9f;
                    for (size_t i = 0; i < tp.size(); ++i) {
                        const float rel[3] = {tp[i][0] - d.pos[0], tp[i][1] - d.pos[1], tp[i][2] - d.pos[2]};
                        float l[3];
                        bf6__qrot(qi, rel, l);
                        loc[i] = {l[0], l[1], l[2]};
                        zmin = std::min(zmin, l[2]);
                        zmax = std::max(zmax, l[2]);
                    }
                    const float zmid = 0.5f * (zmin + zmax);
                    std::map<std::string, std::pair<float, int>> acc;
                    auto add = [&](const std::string& ch, float w) { acc[ch].first += w; acc[ch].second += 1; };
                    for (size_t i = 0; i < loc.size(); ++i) {
                        const bool left = loc[i][0] > 0.0f, front = loc[i][2] > zmid;
                        const bool centre = std::fabs(loc[i][0]) < 0.25f;
                        add(left ? "AngularVelocity_Track_Left" : "AngularVelocity_Track_Right", tom[i]);
                        if (front && centre) add("AngularVelocity_Wheel_FC", tom[i]);
                        else add(std::string("AngularVelocity_Wheel_") + (front ? "F" : "R") + (left ? "L" : "R"), tom[i]);
                    }
                    for (const auto& kv : acc) v->wheel_spin[kv.first] = kv.second.first / (float)kv.second.second;
                    if (std::getenv("BF6_WHEEL_MAP") && v->time > 1.0 && v->time < 1.02)
                        for (size_t i = 0; i < loc.size(); ++i)
                            std::fprintf(stderr, "tyre %zu at body (%.2f %.2f %.2f) omega %.2f\n",
                                         i, loc[i][0], loc[i][1], loc[i][2], tom[i]);
                }
            }
        }
        if (s.wheel_ops().com_set()) {
            v->graph_com_written = true;
            if (std::getenv("BF6_COM_DEBUG"))
                std::fprintf(stderr, "com: host (%g %g %g) -> graph (%g %g %g)\n",
                             d.p.com[0], d.p.com[1], d.p.com[2], s.wheel_ops().body().com[0],
                             s.wheel_ops().body().com[1], s.wheel_ops().body().com[2]);
            for (int i = 0; i < 3; ++i) d.p.com[i] = s.wheel_ops().body().com[i];
        }
        s.wheel_ops().clear_com_set();
        if (s.wheel_ops().inertia_set() && !std::getenv("BF6_NO_GRAPH_INERTIA")) {
            if (std::getenv("BF6_INERTIA_DEBUG"))
                std::fprintf(stderr, "inertia per kg: host (%g %g %g) -> graph (%g %g %g)\n",
                             d.p.inertia_per_kg[0], d.p.inertia_per_kg[1], d.p.inertia_per_kg[2],
                             s.wheel_ops().inertia_per_kg()[0], s.wheel_ops().inertia_per_kg()[1],
                             s.wheel_ops().inertia_per_kg()[2]);
            /* FUN_1443E39C0 stores the reciprocal of every lane, and ignores the call
             * only when all three lanes are within 1e-6 of zero. */
            const float* gi = s.wheel_ops().inertia_per_kg();
            if (std::fabs(gi[0]) > 1e-6f || std::fabs(gi[1]) > 1e-6f || std::fabs(gi[2]) > 1e-6f)
                for (int i = 0; i < 3; ++i) d.p.inertia_per_kg[i] = gi[i];
        }
        if (s.wheel_ops().inertia_set() && v->physics_body_valid) {
            const float* gi = s.wheel_ops().inertia_per_kg();
            if (std::fabs(gi[0]) <= 1e-6f && std::fabs(gi[1]) <= 1e-6f && std::fabs(gi[2]) <= 1e-6f) {
                d.p.mass = v->physics_mass;
                for (int i = 0; i < 3; ++i) d.p.inertia_per_kg[i] = v->physics_inertia_per_kg[i];
                if (!v->graph_com_written)
                    for (int i = 0; i < 3; ++i) d.p.com[i] = v->physics_com[i];
            }
        }
        s.wheel_ops().clear_inertia_set();
        d.integrate_body_accel(dt, la, aa, std::getenv("BF6_GRAPH_GRAVITY") != nullptr);
        if (v->chassis_contact) bf6__collide_vehicle(v, dt);
        /* Keep the established extra linear hull delta (it supplies the calibrated
         * drag path), but not the angular delta: the graph already adds the hull
         * operator's AngularAccelerationOut to AngularAcceleration. Applying hull_dw
         * here a second time is what destabilizes a turning boat. */
        if (s.wheel_ops().hull_ran()) {
            const float* hv = s.wheel_ops().hull_dv();
            float wv[3];
            bf6__qrot(d.quat, hv, wv);
            for (int i = 0; i < 3; ++i) {
                d.vel[i] += wv[i];
            }
        }
    }
    bf6::VehicleDriveInputs di;
    bool k = false;
    di.throttle = s.get_float("VehicleThrottle", &k);
    di.brake = s.get_float("VehicleBrake", &k);
    di.gear_ratio = s.get_float("GearRatio", &k);
    di.clutch = s.get_float("Clutch", &k);
    di.steer = s.get_float("SteeringAngle", &k);
    di.handbrake = handbrake;
    const char* comp[4] = {"SpringCompression_A1", "SpringCompression_A2", "SpringCompression_B1",
                           "SpringCompression_B2"};
    for (int w = 0; w < 4; ++w) di.compression[w] = s.get_float(comp[w], &di.compression_known[w]);
    if (!graph_physics) d.step(dt, di);
    v->time += dt;
    /* out: pos3 quat4 vel3 angvel3 speed rpm gear_ratio clutch throttle brake steer
     *      omega4 compression4 fz4 steer_rad  = 33 floats */
    int o = 0;
    for (int i = 0; i < 3; ++i) out[o++] = d.pos[i];
    for (int i = 0; i < 4; ++i) out[o++] = d.quat[i];
    for (int i = 0; i < 3; ++i) out[o++] = d.vel[i];
    for (int i = 0; i < 3; ++i) out[o++] = d.angvel[i];
    out[o++] = d.forward_speed();
    out[o++] = s.get_float("RPM", &k);
    out[o++] = di.gear_ratio;
    out[o++] = di.clutch;
    out[o++] = di.throttle;
    out[o++] = di.brake;
    out[o++] = di.steer;
    for (int i = 0; i < 4; ++i) out[o++] = d.omega[i];
    for (int i = 0; i < 4; ++i) out[o++] = di.compression[i];
    for (int i = 0; i < 4; ++i) out[o++] = d.fz[i];
    out[o++] = di.steer;   /* the wheel angle, radians, as the graph published it */
    return o;
}

void bf6_vehicle_close(bf6_vehicle* v) {
    if (!v) return;
    for (auto& layer : v->clip_layers)
        if (layer.clip && v->ctx) bf6_free(v->ctx, layer.clip);
    if (v->scene) bf6_ray_scene_free(v->scene);
    delete v;
}

/* NEW GROUND UNDER A RUNNING VEHICLE. The Godot driver re-gathers the ground as the
 * vehicle travels, and used to do it by closing and reopening the vehicle - which
 * zeroes the velocities and the graphs' state, so a jet that had climbed past the
 * re-gather distance stopped dead in the air and dropped back down. This replaces the
 * ray scene only: the new one is built first, swapped in, and the old one freed. */
int32_t bf6_vehicle_set_ground(bf6_vehicle* v, const float* tris, int32_t vert_count) {
    if (!v) return 0;
    bf6_ray_scene* fresh = bf6_ray_scene_create();
    if (!fresh) return 0;
    int32_t added = 0;
    if (tris && vert_count >= 3) {
        const int32_t n = vert_count - vert_count % 3;
        std::vector<int32_t> idx((size_t)n);
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = (int32_t)i;
        const int32_t mesh = bf6_ray_scene_add_mesh(fresh, tris, n, idx.data(), (int32_t)idx.size());
        const double identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        if (mesh >= 0 && bf6_ray_scene_add_instance(fresh, mesh, identity) >= 0) added = n / 3;
    }
    bf6_ray_scene* old = v->scene;
    v->scene = fresh;
    if (old) bf6_ray_scene_free(old);
    return added;
}
