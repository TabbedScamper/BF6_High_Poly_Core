/* DOES THE STATE HOST MAKE VEHICLE GRAPHS COMPUTE?
 *
 *   expression_vehicle_host_test [game dir]
 *
 * The claim under test is narrow and countable. A DiceExpression graph yields a
 * usable answer only when NO operator is left unresolved - taint is total, and a
 * graph with one unanswered call returns a shape rather than a number. Vehicle
 * graphs are blocked on 186 distinct operators, but 913 of those uses are ten state
 * operators and most of the rest are one repeated "fetch the evaluation context"
 * wrapper. StateHost answers exactly those.
 *
 * So: evaluate every vehicle graph twice, with the SAME instance data, differing
 * only in the host.
 *
 *   CONTROL   NamedBuiltins alone - the pure operators, nothing state-backed
 *   TEST      NamedBuiltins + StateHost
 *
 * and report how many graphs come back known-and-untainted each way. The control is
 * the whole point: a number from the test alone would not say whether the host did
 * anything, and "more graphs compute than before" is the only claim being made here.
 * It is NOT a claim that any particular vehicle's physics is right.
 */
#include "bf6_core.h"
#include "expression_graph.h"
#include "expression_registry.h"
#include "expression_pure_ops.h"
#include "expression_state_host.h"
#include "expression_vm.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

static const uint32_t kExpressionType = 0x7dd4cc89u;

static bool is_vehicle(const char* name) {
    if (!name) return false;
    /* BF6_HOST_SCOPE=soldier scores the soldier's own motion-machine graphs
     * (common/gameplay/soldier/...) instead of the vehicles, through the same host. */
    static const char* scope = std::getenv("BF6_HOST_SCOPE");
    if (scope && std::strcmp(scope, "soldier") == 0)
        return std::strstr(name, "gameplay/soldier") != nullptr;
    return std::strstr(name, "hardware/vehicles") != nullptr ||
           std::strstr(name, "fx/vehicles") != nullptr ||
           std::strstr(name, "gameplay/vehicles") != nullptr;
}

struct Outcome {
    size_t graphs = 0, parsed = 0, sound = 0, unresolved_uses = 0;
};

/* ACCEPTING A CALL IS NOT COMPUTING IT.
 *
 * Every count in this test rises just as happily if PureOps returns confident
 * nonsense, because the VM only asks whether a value came back known. For a target
 * of "1:1 with the game" that is the one failure mode that would not show up
 * anywhere - so each operator is checked against an answer worked out by hand.
 */
static int self_check() {
    using namespace bf6::expression;
    PureOps ops;
    /* The host keys on the name recovered from the executable, so the check
     * registers the names against invented keys the same way. */
    uint32_t k = 1;
    std::map<std::string, uint32_t> key;
    for (const char* n : {"SubtractFloat", "NegateFloat", "MinFloat", "MaxFloat",
                          "ClampFloat", "SignFloat", "RadiansToDegreesFloat",
                          "MagnitudeFloat3", "DistanceFloat3", "NormalizeFloat3",
                          "MultiplyFloat3FloatFloat3", "AddFloat3", "RangeChange",
                          "ToFloatBool", "ModuloInt",
                          "MultiplyLinearTransformLinearTransformLinearTransform",
                          "Translation", "InverseLinearTransform",
                          "LinearTransformfromXangle", "LinearTransformfromYangle",
                          "CrossVec3", "DotFloat3", "ComplementFloat", "HalfPi",
                          "NormalizeAngleMinusPiToPi", "InterpolateFloat", "MinInt",
                          "RotateFloat3", "MultiplyFloat3LinearTransformFloat3"}) {
        key[n] = k; ops.add(k, n); ++k;
    }
    auto f = [](float v) {
        Value x; x.bytes.resize(4); std::memcpy(x.bytes.data(), &v, 4);
        x.known = true; return x;
    };
    /* SIXTEEN bytes, per the engine's datatype table (Vec3 fb.DataTypes = 0x10):
     * three floats and four bytes of pad. Building these at 12 is what made an
     * earlier self-check pass against a host that was wrong. */
    auto v3 = [](float a, float b, float c) {
        const float t[3] = {a, b, c};
        Value x; x.bytes.assign(16, 0); std::memcpy(x.bytes.data(), t, 12);
        x.known = true; return x;
    };
    /* LinearTransform: four Vec3 rows at stride 16, so 64 bytes. */
    auto lt = [](const float rows[4][3]) {
        Value x; x.bytes.assign(64, 0);
        for (int r = 0; r < 4; ++r)
            std::memcpy(x.bytes.data() + (size_t)r * 16, rows[r], 12);
        x.known = true; return x;
    };
    auto lt_rows = [](const Value& x, float out[4][3]) {
        for (int r = 0; r < 4; ++r)
            std::memcpy(out[r], x.bytes.data() + (size_t)r * 16, 12);
    };
    auto as_f = [](const Value& x) {
        float v = 0.f; if (x.bytes.size() >= 4) std::memcpy(&v, x.bytes.data(), 4);
        return v;
    };
    int bad = 0;
    auto want = [&](const char* n, const std::vector<Value>& in, float expect) {
        Value out;
        if (!ops.invoke(key[n], in, out)) {
            std::printf("   %-28s REFUSED\n", n); ++bad; return;
        }
        const float got = as_f(out);
        const bool ok = std::fabs(got - expect) < 1e-4f;
        if (!ok) { std::printf("   %-28s got %g want %g\n", n, got, expect); ++bad; }
    };
    want("SubtractFloat", {f(5.f), f(3.f)}, 2.f);
    want("NegateFloat", {f(2.5f)}, -2.5f);
    want("MinFloat", {f(4.f), f(-1.f)}, -1.f);
    want("MaxFloat", {f(4.f), f(-1.f)}, 4.f);
    want("ClampFloat", {f(10.f), f(0.f), f(1.f)}, 1.f);
    want("ClampFloat", {f(-10.f), f(0.f), f(1.f)}, 0.f);
    want("SignFloat", {f(-7.f)}, -1.f);
    want("RadiansToDegreesFloat", {f(3.14159265f)}, 180.f);
    want("MagnitudeFloat3", {v3(3.f, 4.f, 0.f)}, 5.f);
    want("DistanceFloat3", {v3(1.f, 0.f, 0.f), v3(4.f, 4.f, 0.f)}, 5.f);
    want("RangeChange", {f(5.f), f(0.f), f(10.f), f(0.f), f(100.f)}, 50.f);
    want("ToFloatBool", {Value::from_bool(true)}, 1.f);
    want("ModuloInt", {Value::from_u32(7), Value::from_u32(3)}, 0.f); /* checked below */
    /* ModuloInt returns an int, so check its bytes rather than a float. */
    {
        Value out;
        if (!ops.invoke(key["ModuloInt"], {Value::from_u32(7), Value::from_u32(3)}, out)
            || out.as_u32() != 1u) {
            std::printf("   %-28s ModuloInt(7,3) != 1\n", ""); ++bad;
        }
    }
    /* Vector results, component by component. */
    {
        Value out;
        if (!ops.invoke(key["NormalizeFloat3"], {v3(0.f, 5.f, 0.f)}, out)) ++bad;
        else {
            float r[3]; std::memcpy(r, out.bytes.data(), 12);
            if (std::fabs(r[1] - 1.f) > 1e-4f || std::fabs(r[0]) > 1e-4f) {
                std::printf("   NormalizeFloat3 wrong\n"); ++bad;
            }
        }
        if (!ops.invoke(key["MultiplyFloat3FloatFloat3"], {v3(1.f, 2.f, 3.f), f(2.f)}, out)) ++bad;
        else {
            float r[3]; std::memcpy(r, out.bytes.data(), 12);
            if (std::fabs(r[2] - 6.f) > 1e-4f) {
                std::printf("   MultiplyFloat3FloatFloat3 wrong\n"); ++bad;
            }
        }
    }
    /* And the refusals, which must refuse rather than return an infinity the graph
     * would carry on as though it were real. */
    {
        Value out;
        if (ops.invoke(key["RangeChange"],
                       {f(1.f), f(2.f), f(2.f), f(0.f), f(1.f)}, out)) {
            std::printf("   RangeChange accepted a zero input span\n"); ++bad;
        }
        /* The native (0x142493da7) returns zero for a zero vector; it does not refuse. */
        if (!ops.invoke(key["NormalizeFloat3"], {v3(0.f, 0.f, 0.f)}, out) ||
            out.bytes.size() < 12 || out.bytes[0] || out.bytes[4] || out.bytes[8]) {
            std::printf("   NormalizeFloat3 did not return zero for a zero vector\n"); ++bad;
        }
    }
    want("ComplementFloat", {f(0.25f)}, 0.75f);
    want("HalfPi", {}, 1.5707963f);
    want("NormalizeAngleMinusPiToPi", {f(4.0f)}, 4.0f - 6.28318531f);
    want("InterpolateFloat", {f(10.f), f(20.f), f(0.25f)}, 12.5f);
    want("DotFloat3", {v3(1.f, 2.f, 3.f), v3(4.f, 5.f, 6.f)}, 32.f);
    {
        Value out;
        /* x cross y == z, which pins both the formula and the component order. */
        if (!ops.invoke(key["CrossVec3"], {v3(1.f,0.f,0.f), v3(0.f,1.f,0.f)}, out)) ++bad;
        else {
            float r[3]; std::memcpy(r, out.bytes.data(), 12);
            if (std::fabs(r[2]-1.f) > 1e-4f || std::fabs(r[0]) > 1e-4f) {
                std::printf("   CrossVec3: x cross y != z\n"); ++bad;
            }
        }
        if (!ops.invoke(key["MinInt"], {Value::from_u32((uint32_t)-5),
                                        Value::from_u32(3)}, out)
            || (int32_t)out.as_u32() != -5) {
            std::printf("   MinInt is not signed\n"); ++bad;
        }
        /* Rotate ignores the translation; the multiply carries it. That is the one
         * inference in this file drawn from the PAIRING of two names rather than from
         * a measurement, so it gets its own check. */
        const float moved2[4][3] = {{1,0,0},{0,1,0},{0,0,1},{5,0,0}};
        if (!ops.invoke(key["RotateFloat3"], {v3(1.f,0.f,0.f), lt(moved2)}, out)) ++bad;
        else {
            float r[3]; std::memcpy(r, out.bytes.data(), 12);
            if (std::fabs(r[0]-1.f) > 1e-4f) {
                std::printf("   RotateFloat3 applied the translation\n"); ++bad;
            }
        }
        if (!ops.invoke(key["MultiplyFloat3LinearTransformFloat3"],
                        {v3(1.f,0.f,0.f), lt(moved2)}, out)) ++bad;
        else {
            float r[3]; std::memcpy(r, out.bytes.data(), 12);
            if (std::fabs(r[0]-6.f) > 1e-4f) {
                std::printf("   MultiplyFloat3LinearTransformFloat3 dropped it\n");
                ++bad;
            }
        }
    }

    /* LinearTransform. The size and row order are measured; the checks below are the
     * ones that hold whatever the handedness turns out to be. */
    {
        const float ident[4][3] = {{1,0,0},{0,1,0},{0,0,1},{0,0,0}};
        const float moved[4][3] = {{1,0,0},{0,1,0},{0,0,1},{2,3,4}};
        Value out;
        /* Translation pulls out row 3. */
        if (!ops.invoke(key["Translation"], {lt(moved)}, out)) ++bad;
        else {
            float r[3]; std::memcpy(r, out.bytes.data(), 12);
            if (std::fabs(r[0]-2.f) > 1e-4f || std::fabs(r[1]-3.f) > 1e-4f ||
                std::fabs(r[2]-4.f) > 1e-4f) {
                std::printf("   Translation did not return row 3\n"); ++bad;
            }
        }
        /* identity * moved == moved. */
        const uint32_t mk =
            key["MultiplyLinearTransformLinearTransformLinearTransform"];
        if (!ops.invoke(mk, {lt(ident), lt(moved)}, out)) ++bad;
        else {
            float m[4][3]; lt_rows(out, m);
            if (std::fabs(m[3][0]-2.f) > 1e-4f || std::fabs(m[0][0]-1.f) > 1e-4f) {
                std::printf("   identity * M != M\n"); ++bad;
            }
        }
        /* M * inverse(M) == identity, which is the real test of both at once. */
        Value inv;
        if (!ops.invoke(key["InverseLinearTransform"], {lt(moved)}, inv)) ++bad;
        else if (!ops.invoke(mk, {lt(moved), inv}, out)) ++bad;
        else {
            float m[4][3]; lt_rows(out, m);
            bool ok = true;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    if (std::fabs(m[r][c] - (r == c ? 1.f : 0.f)) > 1e-4f) ok = false;
            for (int c = 0; c < 3; ++c)
                if (std::fabs(m[3][c]) > 1e-4f) ok = false;
            if (!ok) { std::printf("   M * inverse(M) != identity\n"); ++bad; }
        }
        /* And a rotation WITH scale, so the inverse is not quietly a transpose. */
        const float scaled[4][3] = {{2,0,0},{0,3,0},{0,0,4},{1,1,1}};
        if (!ops.invoke(key["InverseLinearTransform"], {lt(scaled)}, inv)) ++bad;
        else if (!ops.invoke(mk, {lt(scaled), inv}, out)) ++bad;
        else {
            float m[4][3]; lt_rows(out, m);
            if (std::fabs(m[0][0]-1.f) > 1e-4f || std::fabs(m[3][0]) > 1e-4f) {
                std::printf("   inverse is wrong when the transform has scale\n");
                ++bad;
            }
        }
        /* Rotations: R(a)*R(-a) == identity and R(a)*R(b) == R(a+b). Both hold for
         * either handedness, so they catch an implementation error without pretending
         * the sign convention has been verified. */
        for (const char* rn : {"LinearTransformfromXangle",
                               "LinearTransformfromYangle"}) {
            Value ra, rb, rc, rsum;
            if (!ops.invoke(key[rn], {f(0.7f)}, ra) ||
                !ops.invoke(key[rn], {f(-0.7f)}, rb) ||
                !ops.invoke(mk, {ra, rb}, out)) { ++bad; continue; }
            float m[4][3]; lt_rows(out, m);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    if (std::fabs(m[r][c] - (r == c ? 1.f : 0.f)) > 1e-4f) {
                        std::printf("   %s: R(a)*R(-a) != identity\n", rn);
                        ++bad; r = 3; break;
                    }
            if (!ops.invoke(key[rn], {f(0.3f)}, rb) ||
                !ops.invoke(key[rn], {f(1.0f)}, rsum) ||
                !ops.invoke(mk, {ra, rb}, rc)) { ++bad; continue; }
            float m1[4][3], m2[4][3];
            lt_rows(rc, m1); lt_rows(rsum, m2);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    if (std::fabs(m1[r][c] - m2[r][c]) > 1e-4f) {
                        std::printf("   %s: R(0.7)*R(0.3) != R(1.0)\n", rn);
                        ++bad; r = 3; break;
                    }
        }
    }

    std::printf("PureOps self-check: %s\n",
                bad == 0 ? "every operator matches a hand-worked answer"
                         : "FAILURES ABOVE");
    return bad;
}

/* A GROUND TO HIT. One 400 m quad at y = 0, which is a STAND-IN for a level and is
 * said out loud: the question here is whether the wheel rays fire at all and whether
 * they resolve against real triangles, not whether a particular map is under them.
 * bf6_ray_scene_trace works in metres with Y up, the same frame as the Godot scene. */
static int trace_scene(void* user, const double* from, const double* to,
                       double* hit, double* normal) {
    bf6_ray_scene* s = (bf6_ray_scene*)user;
    double out[7] = {};
    if (bf6_ray_scene_trace(s, from, to, out) < 0) return 0;
    for (int i = 0; i < 3; ++i) { hit[i] = out[i]; normal[i] = out[3 + i]; }
    return 1;
}

int main(int argc, char** argv) {
    const int self_bad = self_check();
    if (self_bad) return 3;

    const char* game = argc > 1 ? argv[1] :
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Battlefield 6";
    char err[1024] = {};
    bf6_ctx* ctx = bf6_open(game, err, (int)sizeof(err));
    if (!ctx) { std::fprintf(stderr, "open: %s\n", err); return 2; }
    if (!bf6_mount_all(ctx, 1, err, (int)sizeof(err))) {
        std::fprintf(stderr, "mount_all: %s\n", err); bf6_close(ctx); return 2;
    }
    /* The soldier graphs read the soldier's named fields; load their table from the
     * motion-machine asset so the host serves them (authored defaults: an idle soldier). */
    if (const char* scope = std::getenv("BF6_HOST_SCOPE"))
        if (std::strcmp(scope, "soldier") == 0)
            std::printf("soldier fields: %d\n", bf6_soldier_fields_load(ctx));
    bf6_ray_scene* scene = bf6_ray_scene_create();
    {
        const float verts[12] = {-200.f, 0.f, -200.f,  200.f, 0.f, -200.f,
                                  200.f, 0.f,  200.f, -200.f, 0.f,  200.f};
        const int32_t tris[6] = {0, 1, 2, 0, 2, 3};
        const int32_t mesh = bf6_ray_scene_add_mesh(scene, verts, 4, tris, 6);
        const double ident[12] = {1,0,0,0, 0,1,0,0, 0,0,1,0};
        bf6_ray_scene_add_instance(scene, mesh, ident);
    }

    const int total = bf6_list_res(ctx, nullptr, nullptr, 0);
    std::vector<bf6_asset> assets((size_t)std::max(total, 0));
    const int got = bf6_list_res(ctx, nullptr, assets.data(), total);

    Outcome control, tested;
    int ray_queries = 0, ray_hits = 0;
    std::map<uint32_t, uint32_t> suspension_wants;
    std::set<std::string> suspension_graphs;
    std::map<std::string, uint32_t> push_shapes;
    std::map<std::string, uint32_t> read_shapes;
    std::map<std::string, uint32_t> header_shapes;
    std::map<std::string, uint32_t> push_regions;
    std::map<std::string, uint32_t> write_operands;
    std::map<std::string, uint32_t> prepare_io;
    std::map<std::string, uint32_t> prepare_kinds;
    std::map<std::string, uint32_t> lt_inputs;
    std::map<std::string, uint32_t> accessor_use;
    std::map<std::string, uint32_t> slot_type_of;
    std::map<std::string, uint32_t> selector_seen;
    std::map<std::string, uint32_t> ray_point_origin;
    std::map<std::string, int> queue_graphs;
    std::map<std::string, uint32_t> queue_reach;
    std::map<std::string, uint32_t> susp_unresolved;
    std::map<std::string, uint32_t> root_shapes;
    std::map<std::string, uint32_t> susp_causes;
    std::map<std::string, uint32_t> physq_inputs;
    std::map<std::string, uint32_t> field_ids;
    std::map<std::string, uint32_t> slot_room;
    std::map<std::string, uint32_t> ray_culprits;
    std::map<std::string, uint32_t> engine_filled;
    std::map<std::string, uint32_t> move_kinds;
    std::map<int, uint32_t> tick_first_ray;   /* pass number -> graphs that fired */
    int tick_rays = 0, tick_writes = 0;
    int seeded_points_total = 0, seeded_xforms_total = 0, seeded_attempts = 0,
        seeded_unknown = 0, seeded_rays = 0, seeded_hits = 0;
    std::map<uint32_t, uint32_t> served;
    std::map<uint32_t, uint32_t> still_blocked;
    /* Every key whose NAME we recovered, so a still-blocked key can be reported by
     * name. A blocked key that HAS a name is a pure operator NamedBuiltins simply
     * does not implement yet, which is the cheapest kind of gap to close. */
    std::map<uint32_t, std::string> name_of;
    /* operator name -> {operand count -> how many records had it} */
    std::map<std::string, std::map<size_t, uint32_t>> operands_of;
    struct RegionStat {
        uint32_t records = 0, with_slot = 0, without_slot = 0;
        std::map<std::string, uint32_t> shapes;
    };
    std::map<std::string, RegionStat> regions_of;
    std::map<uint32_t, uint32_t> described_but_failed;   /* implemented, fed badly */
    std::map<uint32_t, uint32_t> not_described;          /* genuinely missing      */
    /* query node name -> input index -> "producer:width" -> how many records */
    std::map<std::string,
             std::map<size_t, std::map<std::string, uint32_t>>> query_inputs;
    const std::string exe = std::string(game) + "\\bf6.exe";

    /* Data type sizes for typed copies, read from this install's executable. */
    std::map<uint32_t, uint32_t> type_size_cache;
    /* The current graph's channel bindings, read from its EBX; patched into every
     * instance's constant pool the way the engine does at load. */
    std::vector<bf6_channel_binding> cur_bindings;
    std::map<std::string, uint32_t> chan_ops;
    int graphs_with_bindings = 0, bindings_total = 0;
    auto fill_types = [&](const bf6::expression::Graph& g, bf6::expression::Instance& in) {
        /* Public channels: the pool entry gets the channel hash. Bone channels: the
         * pool entry is an ExpressionBoneId; its first dword gets the bone channel
         * hash, the rest stays as authored (zero). */
        for (const auto& b : cur_bindings)
            if (b.region == 0) in.pool_patches[b.pool_offset] = b.channel_hash;
        /* Relocated pointers -> their pool target (a PUSH's feature identity). */
        for (const auto& rl : g.relocations) in.pool_patches[rl.pointer_field] = rl.target;
        for (const auto& grp : g.slot_values) {
            auto it = type_size_cache.find(grp.data_type_id);
            if (it == type_size_cache.end())
                it = type_size_cache.emplace(grp.data_type_id,
                                             bf6_type_size_by_hash(ctx, grp.data_type_id)).first;
            if (it->second) in.type_sizes[grp.data_type_id] = it->second;
        }
    };

    /* Feed a StateHost the vehicle's bone poses: each bone binding of the CURRENT
     * graph -> the skeleton in its folder -> rig bone -> model pose. Called for
     * every graph as soon as the TEST pass makes its host. */
    std::map<std::string, std::map<uint32_t, int32_t>> ske_tables;  /* skeleton -> channel -> bone */
    auto feed_rig = [&](bf6::expression::StateHost& state, const bf6_asset& a) {
        /* THE VEHICLE'S OWN BONE POSES. Each bone binding names a bone
         * channel; the skeleton in the graph's folder maps its hash to a
         * rig bone; the rig gives that bone's MODEL pose (bind pose,
         * vehicle space). Supplied for mode 1 (the mode whose translation
         * the rays use as the wheel point) and, UNVERIFIED, mode 0. */
        int bones_fed = 0, bones_missing = 0;
        {
            std::string dir(a.name);
            dir.resize(dir.rfind('/') + 1);
            /* PICK THE RIG BY COVERAGE, not by folder. Shared graphs (abilities, FX,
             * _vehiclescommon) sit outside any vehicle folder, and a vehicle folder
             * can hold several ske_veh_* rigs. Every vehicle skeleton's bone-channel
             * table is read once; the graph gets the one that maps the most of its
             * bound bone channels, its own folder winning ties. */
            if (ske_tables.empty()) {
                const int total_ske = bf6_list_ebx(ctx, "ske_veh_", nullptr, 0);
                std::vector<bf6_asset> all((size_t)std::max(total_ske, 0));
                const int got_ske = bf6_list_ebx(ctx, "ske_veh_", all.data(), total_ske);
                for (int si = 0; si < got_ske; ++si) {
                    std::vector<uint32_t> hs(1024);
                    std::vector<int32_t> bs(1024);
                    const int nc = bf6_skeleton_channel_bones(ctx, all[(size_t)si].name, hs.data(), bs.data(), 1024);
                    if (nc <= 0) continue;
                    auto& t = ske_tables[all[(size_t)si].name];
                    for (int ci = 0; ci < nc && ci < 1024; ++ci) t[hs[(size_t)ci]] = bs[(size_t)ci];
                }
                std::printf("RIGS: %zu vehicle skeletons carry a bone-channel table\n", ske_tables.size());
            }
            std::string ske_name;
            int best = 0;
            /* NAMED VEHICLE FIRST. Generic channels exist on many rigs, so pure
             * coverage put the flyer60 rig on thebeast. A rig's vehicle name is the
             * token before its suffix (ske_veh_car_flyer60_base -> flyer60). If the
             * graph path names a rigged vehicle, only that vehicle's rigs qualify. A
             * graph scoped to one vehicle (its hardware folder, or per-vehicle FX
             * logic) whose vehicle has NO rig gets none - a wrong rig is worse than
             * none. Only shared graphs (_vehiclescommon, gameplay) use coverage. */
            auto veh_of = [](const std::string& ske) {
                std::string leaf = ske.substr(ske.rfind('/') + 1);
                const size_t us = leaf.rfind('_');
                if (us != std::string::npos) leaf.resize(us);
                const size_t us2 = leaf.rfind('_');
                return us2 == std::string::npos ? leaf : leaf.substr(us2 + 1);
            };
            const std::string path_l(a.name);
            std::set<std::string> named;
            for (const auto& st : ske_tables) {
                const std::string v = veh_of(st.first);
                if (v.size() >= 3 && path_l.find(v) != std::string::npos) named.insert(v);
            }
            const bool vehicle_scoped =
                (path_l.compare(0, 25, "common/hardware/vehicles/") == 0 &&
                 path_l.find("/_vehiclescommon/") == std::string::npos) ||
                path_l.compare(0, 26, "common/fx/vehicles/logic/p") == 0 ||
                path_l.compare(0, 26, "common/fx/vehicles/logic/v") == 0;
            /* Candidates in coverage order (own folder first on ties). Each binding
             * then resolves against the FIRST candidate rig that carries its channel:
             * a tank's wheels live in _base and its track bones in _track. */
            std::vector<std::pair<int, std::string>> cands;
            for (const auto& st : ske_tables) {
                if (!named.empty() && !named.count(veh_of(st.first))) continue;
                if (named.empty() && vehicle_scoped) continue;
                int cover = 0;
                for (const auto& b : cur_bindings)
                    if (b.kind == 1 && st.second.count(b.channel_hash)) ++cover;
                if (!cover) continue;
                const bool own = st.first.compare(0, dir.size(), dir) == 0;
                cands.push_back({-(cover * 2 + (own ? 1 : 0)), st.first});
            }
            std::sort(cands.begin(), cands.end());
            if (!cands.empty()) ske_name = cands.front().second;
            (void)best;
            std::map<std::string, bf6_skeleton*> rigs;
            for (const auto& b : cur_bindings) {
                if (b.kind != 1) continue;
                const std::string* rig = nullptr;
                int32_t bone = -1;
                for (const auto& c : cands) {
                    const auto& t = ske_tables[c.second];
                    const auto f = t.find(b.channel_hash);
                    if (f != t.end()) { rig = &c.second; bone = f->second; break; }
                }
                if (!rig) {
                    if (!cands.empty()) ++bones_missing;
                    continue;
                }
                if (!rigs.count(*rig)) rigs[*rig] = bf6_skeleton_read(ctx, rig->c_str());
                bf6_skeleton* sk = rigs[*rig];
                {
                    const auto bo = std::make_pair(b.channel_hash, bone);
                    if (!sk || bo.second < 0 || bo.second >= sk->bone_count) {
                        ++bones_missing;
                        continue;
                    }
                    const float* m = sk->bones[bo.second].model;
                    float rows[16] = {m[0], m[1], m[2], 0, m[3], m[4], m[5], 0,
                                      m[6], m[7], m[8], 0, m[9], m[10], m[11], 0};
                    /* MODES, READ FROM THE EXE (FUN_144333a60 and its six callees):
                     *   0 LOCAL  one bone record, no parent walk, no entity matrix
                     *   1 MODEL  parent chain to the rig root (entity divided out)
                     *   2 WORLD  MODEL post-multiplied by the entity world matrix
                     * Offline the entity sits at the origin, so WORLD = MODEL until a
                     * caller places it; the RootTransform channel carries placement. */
                    const float* l = sk->bones[bo.second].local;
                    float lrows[16] = {l[0], l[1], l[2], 0, l[3], l[4], l[5], 0,
                                       l[6], l[7], l[8], 0, l[9], l[10], l[11], 0};
                    state.set_bone_pose(b.channel_hash, 1, rows);
                    state.set_bone_pose(b.channel_hash, 0, lrows);
                    state.set_bone_pose(b.channel_hash, 2, rows);
                    ++bones_fed;
                    if (std::strstr(a.name, "flyer60") && std::strstr(b.channel_name, "Wheel"))
                        std::printf("BONE %-24s -> rig %3d %-24s t (%.3f %.3f %.3f)\n", b.channel_name,
                                    bo.second, sk->bones[bo.second].name, m[9], m[10], m[11]);
                }
            }
            for (auto& r : rigs) if (r.second) bf6_free(ctx, r.second);
            int bone_binds = 0;
            for (const auto& b : cur_bindings) if (b.kind == 1) ++bone_binds;
            std::printf("RIG %-40s skeleton %s  bone poses fed %d, unmapped %d, bone bindings %d  path %s\n",
                        std::strrchr(a.name, '/') + 1,
                        ske_name.empty() ? "(none found)" : std::strrchr(ske_name.c_str(), '/') + 1,
                        bones_fed, bones_missing, bone_binds, a.name);
        }
    };

    /* BF6_ONLY=<substring>: run only matching graphs, for fast study loops. The
     * scoreboard numbers are only comparable on a full (unfiltered) run. */
    const char* only = std::getenv("BF6_ONLY");
    if (only && *only) std::printf("FILTERED RUN: only graphs containing \"%s\"\n", only);
    /* BF6_SHARD=k/n: take every n-th matching graph starting at k, so n processes
     * cover the whole set in parallel (tools/run_vehicle_host_sharded.sh sums them). */
    int shard_k = 0, shard_n = 1, matched = 0;
    if (const char* sh = std::getenv("BF6_SHARD"))
        if (std::sscanf(sh, "%d/%d", &shard_k, &shard_n) != 2 || shard_n < 1) { shard_k = 0; shard_n = 1; }
    for (int i = 0; i < got; ++i) {
        const bf6_asset& a = assets[(size_t)i];
        if (a.type != kExpressionType || !is_vehicle(a.name)) continue;
        if (only && *only && !std::strstr(a.name, only)) continue;
        if ((matched++ % shard_n) != shard_k) continue;
        ++control.graphs; ++tested.graphs;

        const uint8_t* raw = nullptr;
        const int64_t bytes = bf6_read_raw(ctx, BF6_RAW_RES, a.name, &raw);
        bf6::expression::Graph graph;
        std::string why;
        if (bytes <= 0 || !raw ||
            !bf6::expression::parse(raw, (size_t)bytes, graph, why))
            continue;
        ++control.parsed; ++tested.parsed;

        /* WHY A GRAPH DOES NOT TILE. Walk the record stream the way the
         * interpreter does - by each record's own next offset and its control
         * targets, never by length - then report, for every discovered start,
         * its kind and the gap to the next discovered start. Kinds without a
         * proven length show up with their measured gaps. */
        /* TWEAKABLE operands: BF6_TWEAK=<substr> prints the constant each
         * __GetTweakableFloat/Bool record passes (pool offset and value). */
        if (const char* tw = std::getenv("BF6_TWEAK"))
            if (std::strstr(a.name, tw))
                for (const auto& rec : graph.records)
                    if ((rec.operator_key == 0x10E7EF91u || rec.operator_key == 0xC6485A8Bu) &&
                        !rec.operands.empty()) {
                        const auto& op = rec.operands[0];
                        uint32_t v = 0;
                        if (op.region == 0 && op.offset + 4 <= graph.constant_pool.size())
                            std::memcpy(&v, graph.constant_pool.data() + op.offset, 4);
                        std::printf("TWEAK %s r%u@0x%X value 0x%08X (%u) ops %zu kind 0x%02X\n",
                                    rec.operator_key == 0x10E7EF91u ? "float" : "bool ",
                                    op.region, op.offset, v, v, rec.operands.size(), rec.kind);
                    }
        /* 0x23 LENGTH STUDY: on graphs that DO tile, every 0x23 record's length is
         * known; print it beside its first dwords so a count field can be fit. */
        if (graph.exact_record_tiling) {
            const uint8_t* region = raw + graph.region_base;
            for (const auto& rec : graph.records) {
                if (rec.kind != 0x23) continue;
                std::printf("K23 len %u", rec.byte_length);
                for (uint32_t w = 12; w + 4 <= rec.byte_length && w <= 28; w += 4) {
                    uint32_t v; std::memcpy(&v, region + rec.offset + w, 4);
                    std::printf(" +%u=%08X", w, v);
                }
                std::printf("\n");
            }
        }
        if (!graph.exact_record_tiling) {
            const uint8_t* region = raw + graph.region_base;
            const size_t rb = (size_t)graph.header.record_dwords * 4u;
            std::set<uint32_t> starts{0};
            for (const auto& f : graph.fixups) starts.insert(f.record_offset);
            std::vector<uint32_t> work(starts.begin(), starts.end());
            auto push = [&](uint32_t o) {
                if (o < rb && !(o & 3u) && starts.insert(o).second) work.push_back(o);
            };
            while (!work.empty()) {
                const uint32_t o = work.back(); work.pop_back();
                if (o + 4 > rb) continue;
                uint32_t h; std::memcpy(&h, region + o, 4);
                const uint8_t k = (uint8_t)(h & 0xff);
                const uint32_t nx = h >> 8;
                if (nx) push(nx);
                auto rd = [&](uint32_t at) { uint32_t v = 0; if (o + at + 4 <= rb) std::memcpy(&v, region + o + at, 4); return v; };
                if (k == 0x26) push(rd(12));
                if (k == 0x29 || k == 0x2a) push(rd(4));
                if (k == 0x28) {
                    const uint32_t cnt = rd(12);
                    for (uint32_t i = 0; i < cnt && i < 64; ++i) push(rd(16 + i * 8 + 4));
                }
            }
            std::vector<uint32_t> s(starts.begin(), starts.end());
            for (size_t i = 0; i < s.size(); ++i) {
                const uint8_t k = region[s[i]];
                const uint32_t gap = (i + 1 < s.size() ? s[i + 1] : (uint32_t)rb) - s[i];
                const uint32_t proven = bf6::expression::proven_record_length(k);
                if (proven && proven == gap) continue;
                std::printf("TILEGAP kind 0x%02X proven %u gap %u  %s\n", k, proven, gap,
                            std::strrchr(a.name, '/') + 1);
            }
            std::printf("UNTILED %s ways %u records-walked %zu bytes %zu\n", a.name,
                        (unsigned)graph.tiling_ways, s.size(), rb);
        }

        {
            cur_bindings.assign(256, bf6_channel_binding{});
            const int nb = bf6_expression_channel_bindings(ctx, a.name, cur_bindings.data(),
                                                           (int)cur_bindings.size());
            cur_bindings.resize(nb > 0 ? (size_t)std::min(nb, 256) : 0u);
            if (nb > 0) { ++graphs_with_bindings; bindings_total += nb; }
        }

        std::set<uint32_t> keyset;
        for (const auto& f : graph.fixups) keyset.insert(f.key);
        std::vector<uint32_t> keys(keyset.begin(), keyset.end());
        std::vector<bf6::expression::NamedOperator> names;
        std::string scan_why;
        bf6::expression::resolve_named_operators(exe, keys, names, scan_why);
        bf6::expression::NamedBuiltins builtins;
        bf6::expression::PureOps pure;
        for (const auto& row : names) {
            if (row.match_count != 1) continue;
            builtins.add(row.key, row.name);
            pure.add(row.key, row.name);
            name_of[row.key] = row.name;
        }

        /* THE ARITY QUESTION, measured rather than assumed.
         *
         * The biggest remaining bucket is operators whose NAME we already know but
         * which still fail, because NamedBuiltins declares a fixed arity per name
         * and the record disagrees. The registry does not carry arity, but the
         * RECORD knows how many operands it has and the NAME states its types
         * ("MultiplyFloatFloatFloat"). So collect (name -> operand counts seen) and
         * let the data say what the convention is. */
        for (const auto& rec : graph.records) {
            if (!rec.operator_key) continue;
            const auto nm = name_of.find(rec.operator_key);
            if (nm == name_of.end()) continue;
            operands_of[nm->second][rec.operands.size()] += 1;
        }

        /* WHY DOES AN IMPLEMENTED OPERATOR STILL FAIL?
         *
         * AddFloat is in NamedBuiltins with two 4-byte inputs, the records carry 3
         * operands (inputs + 1), and it still blocks 116 graphs. The VM refuses a
         * call when `signature.output_width && !call.output`, and `call.output` is
         * the LAST REGION-2 OPERAND - so a record with no region-2 operand cannot
         * satisfy any signature that declares an output, whatever its arity.
         *
         * So tally the operand REGIONS per name. This is the one measurement that
         * has to happen before a 155-entry signature table gets written, because a
         * table built on the wrong assumption is wrong 155 times. */
        for (const auto& rec : graph.records) {
            if (!rec.operator_key) continue;
            const auto nm = name_of.find(rec.operator_key);
            if (nm == name_of.end()) continue;
            bool has_slot = false;
            std::string shape;
            for (const auto& op : rec.operands) {
                if (op.region == 2) has_slot = true;
                shape += (char)('0' + (op.region > 9 ? 9 : op.region));
            }
            RegionStat& rs = regions_of[nm->second];
            rs.records += 1;
            if (has_slot) rs.with_slot += 1; else rs.without_slot += 1;
            rs.shapes[shape] += 1;
        }

        /* THE PHYSICS QUERY NODES' SIGNATURE, measured rather than guessed.
         *
         * `__queueAsyncPhysicsRayQueryNode` has 5 operands and its
         * HasExcludedEntities sibling 6, and `__consumeAsyncPhysicsQueryNode` 4. The
         * widths are not in the datatype table (those nodes' types are not
         * DiceExpression datatypes), so guessing them would make the VM slice the
         * wrong bytes silently.
         *
         * They can be DERIVED. Records execute in order, so walking the graph and
         * remembering which operator last wrote each slot offset gives, for every
         * input of a query node, the OUTPUT WIDTH of whatever produced it. A Vec3
         * producer means a 16-byte input; a float producer means 4. */
        {
            std::map<uint32_t, std::pair<std::string, uint32_t>> wrote;
            for (const auto& rec : graph.records) {
                const auto nm = name_of.find(rec.operator_key);
                const std::string opname =
                    nm == name_of.end() ? std::string() : nm->second;
                if (opname.compare(0, 7, "__queue") == 0 ||
                    opname.compare(0, 9, "__consume") == 0) {
                    for (size_t i = 0; i + 1 < rec.operands.size(); ++i) {
                        const auto w = wrote.find(rec.operands[i].offset);
                        query_inputs[opname][i][
                            w == wrote.end() ? std::string("(unwritten)")
                                             : w->second.first + ":" +
                                               std::to_string(w->second.second)] += 1;
                    }
                }
                /* Remember this record's own output, for the records after it. The
                 * width comes from whichever host describes the operator, which is
                 * the same answer the VM itself would use. */
                if (!opname.empty() && !rec.operands.empty()) {
                    bf6::expression::OperatorSignature sig;
                    uint32_t ow = 0;
                    if (builtins.describe(rec.operator_key, sig) ||
                        pure.describe(rec.operator_key, sig))
                        ow = sig.output_width;
                    for (auto it = rec.operands.rbegin();
                         it != rec.operands.rend(); ++it) {
                        if (it->region == 2) {
                            wrote[it->offset] = {opname, ow};
                            break;
                        }
                    }
                }
            }
        }

                /* THE __Dice...Prepare*Ex PROLOGUES, both directions.
                 *
                 * Everything else has been eliminated as the source of the vehicle's
                 * world transform, and these four sit at the top of the
                 * never-described list with the right name and shape. So trace what
                 * PRODUCES each of their operands, and - the half that actually
                 * decides it - which operators later CONSUME the slot they write. If
                 * a Prepare's output feeds the LinearTransform chain, it is the
                 * transform source and this is over.
                 */
                {
                    std::map<uint32_t, std::string> prod;
                    /* pass 1: who writes each slot */
                    for (const auto& rec : graph.records) {
                        std::string pn;
                        if (rec.operator_key == 0x8B7CF7C9u) pn = "FIELD_ADDRESS";
                        else {
                            const auto nm = name_of.find(rec.operator_key);
                            if (nm != name_of.end()) pn = nm->second;
                        }
                        if (pn.empty()) continue;
                        for (auto it = rec.operands.rbegin();
                             it != rec.operands.rend(); ++it)
                            if (it->region == 2) { prod[it->offset] = pn; break; }
                    }
                    /* pass 2: the Prepare nodes' inputs, and their consumers */
                    const char* leaf = std::strrchr(a.name, '/');
                    leaf = leaf ? leaf + 1 : a.name;
                    std::string kind(leaf, std::min<size_t>(std::strlen(leaf), 8));
                    bool has_prepare = false;
                    for (const auto& rec : graph.records) {
                        const auto nm = name_of.find(rec.operator_key);
                        if (nm == name_of.end()) continue;
                        if (nm->second.find("_Prepare") == std::string::npos) continue;
                        has_prepare = true;
                        for (size_t i = 0; i < rec.operands.size(); ++i) {
                            const auto w = prod.find(rec.operands[i].offset);
                            char pb2[176];
                            std::snprintf(pb2, sizeof pb2,
                                          "%.46s  operand %zu of %zu  r%u  <- %s",
                                          nm->second.c_str(), i, rec.operands.size(),
                                          rec.operands[i].region,
                                          w == prod.end() ? "(unwritten)"
                                                          : w->second.c_str());
                            prepare_io[pb2] += 1;
                        }
                        /* its output slot, and who reads it afterwards */
                        uint32_t outslot = 0xFFFFFFFFu;
                        for (auto it = rec.operands.rbegin();
                             it != rec.operands.rend(); ++it)
                            if (it->region == 2) { outslot = it->offset; break; }
                        if (outslot == 0xFFFFFFFFu) continue;
                        for (const auto& other : graph.records) {
                            if (&other == &rec) continue;
                            const auto on = name_of.find(other.operator_key);
                            const std::string oname =
                                on == name_of.end()
                                    ? (other.operator_key == 0x8B7CF7C9u
                                           ? std::string("FIELD_ADDRESS")
                                           : std::string("(unnamed op)"))
                                    : on->second;
                            for (size_t i = 0; i + 1 < other.operands.size(); ++i) {
                                if (other.operands[i].region != 2 ||
                                    other.operands[i].offset != outslot) continue;
                                char cb[176];
                                std::snprintf(cb, sizeof cb,
                                              "%.40s output -> consumed by %.60s",
                                              nm->second.c_str(), oname.c_str());
                                prepare_io[cb] += 1;
                            }
                        }
                    }
                    prepare_kinds[kind + (has_prepare ? "  HAS Prepare" : "  no Prepare")] += 1;
                }

        /* WHAT DO THE CONTEXT ACCESSORS ACTUALLY RETURN?
         *
         * 0x04BEFF62 is stubbed with a 4-byte handle, and in the suspension graphs its
         * output feeds a LinearTransform multiply 154 times. Widening it to 64 bytes is
         * only safe if EVERY consumer reads it as a transform: the VM writes
         * output_width bytes into the slot, so a 64-byte answer to a use that expects 4
         * would clobber neighbouring slots. So tally every consumer of every
         * accessor's output, over ALL vehicle graphs, by consumer name and operand.
         */
        {
            static const uint32_t kWatch[] = {
                0x04BEFF62u, 0x6D98A861u, 0x18B2987Bu, 0x08F4A2D4u, 0xABAAAD01u,
                0xC58D8EA6u, 0x45C08E62u, 0xC491CD96u, 0x6D86C436u, 0x4899CB44u,
                0x040F4924u, 0x8AEFC858u, 0x63D604B7u, 0x532B3BA9u, 0xF0F74455u,
                0x0F063D92u, 0xE7488ECEu, 0x85A781A7u, 0x88030F01u,
            };
            for (const auto& rec : graph.records) {
                bool watched = false;
                for (uint32_t w : kWatch) if (rec.operator_key == w) watched = true;
                if (!watched) continue;
                uint32_t outslot = 0xFFFFFFFFu;
                for (auto it = rec.operands.rbegin(); it != rec.operands.rend(); ++it)
                    if (it->region == 2) { outslot = it->offset; break; }
                if (outslot == 0xFFFFFFFFu) {
                    char nb[96];
                    std::snprintf(nb, sizeof nb, "0x%08X  (no output slot)", rec.operator_key);
                    accessor_use[nb] += 1;
                    continue;
                }
                bool consumed = false;
                for (const auto& other : graph.records) {
                    if (&other == &rec) continue;
                    for (size_t i = 0; i < other.operands.size(); ++i) {
                        if (other.operands[i].region != 2 ||
                            other.operands[i].offset != outslot) continue;
                        const auto on = name_of.find(other.operator_key);
                        char cb[176];
                        if (on != name_of.end())
                            std::snprintf(cb, sizeof cb, "0x%08X -> %.52s operand %zu",
                                          rec.operator_key, on->second.c_str(), i);
                        else
                            std::snprintf(cb, sizeof cb, "0x%08X -> key 0x%08X operand %zu",
                                          rec.operator_key, other.operator_key, i);
                        accessor_use[cb] += 1;
                        consumed = true;
                    }
                }
                if (!consumed) {
                    char nb[96];
                    std::snprintf(nb, sizeof nb, "0x%08X  (output never read)", rec.operator_key);
                    accessor_use[nb] += 1;
                }
            }
        }

        /* DOES THE SLOT TYPE MAP GIVE A GENERIC READ ITS WIDTH?
         *
         * The arity-3 accessors (0x04BEFF62, 0x08F4A2D4, 0xABAAAD01) feed a transform
         * in one place and a float in another, so no single width per KEY is right.
         * graph.slot_values is a (data_type_id, offsets) map over the slot scratch.
         * If each accessor call's OUTPUT slot is declared there with the type its
         * consumers need, the width is per-call and comes from data. Tally, per
         * accessor key, the declared data_type_id of its output slot.
         */
        {
            std::map<uint32_t, uint32_t> type_at;
            for (const auto& grp : graph.slot_values)
                for (uint32_t off : grp.offsets) type_at[off] = grp.data_type_id;
            static const uint32_t kWatch2[] = {
                0x04BEFF62u, 0x08F4A2D4u, 0xABAAAD01u, 0x18B2987Bu, 0x45C08E62u,
                0xC491CD96u, 0xC58D8EA6u, 0x6D86C436u, 0x6D98A861u, 0x4899CB44u,
            };
            for (const auto& rec : graph.records) {
                bool watched = false;
                for (uint32_t w : kWatch2) if (rec.operator_key == w) watched = true;
                if (!watched) continue;
                uint32_t outslot = 0xFFFFFFFFu;
                for (auto it = rec.operands.rbegin(); it != rec.operands.rend(); ++it)
                    if (it->region == 2) { outslot = it->offset; break; }
                char tb[96];
                if (outslot == 0xFFFFFFFFu) {
                    std::snprintf(tb, sizeof tb, "0x%08X  no output slot", rec.operator_key);
                } else {
                    const auto t = type_at.find(outslot);
                    if (t == type_at.end())
                        std::snprintf(tb, sizeof tb, "0x%08X  slot NOT in type map",
                                      rec.operator_key);
                    else
                        std::snprintf(tb, sizeof tb, "0x%08X  slot type 0x%08X",
                                      rec.operator_key, t->second);
                }
                slot_type_of[tb] += 1;
            }
        }

        /* IS THE WIDTH SELECTED BY AN OPERAND?
         *
         * The slot type map does not cover these outputs (0 of ~8000), so for the
         * three generic arity-3 readers the per-call width has to come from the call
         * itself. Classify each call by what its output is USED as (transform,
         * Vec3 or float, from the first consumer that reads it as an input) and print
         * the constant-pool value of every region-0 operand next to it. An operand
         * whose value tracks the use - a datatype id like 0x417/0x411/0x0A, or a byte
         * size like 64/16/4 - is the selector.
         */
        {
            static const uint32_t kGeneric[] = {0x04BEFF62u, 0x08F4A2D4u, 0xABAAAD01u};
            auto kind_of = [](const std::string& n) -> const char* {
                if (n.find("LinearTransform") != std::string::npos ||
                    n == "RotationAndTranslation" || n == "InverseTransform")
                    return "TRANSFORM";
                if (n.find("Float3") != std::string::npos ||
                    n.find("Vec3") != std::string::npos || n == "InverseRotate")
                    return "VEC3";
                if (n.find("Float") != std::string::npos) return "FLOAT";
                return nullptr;
            };
            for (const auto& rec : graph.records) {
                bool generic = false;
                for (uint32_t g : kGeneric) if (rec.operator_key == g) generic = true;
                if (!generic) continue;
                uint32_t outslot = 0xFFFFFFFFu;
                for (auto it = rec.operands.rbegin(); it != rec.operands.rend(); ++it)
                    if (it->region == 2) { outslot = it->offset; break; }
                if (outslot == 0xFFFFFFFFu) continue;
                const char* use = nullptr;
                for (const auto& other : graph.records) {
                    if (&other == &rec || other.operands.size() < 2) continue;
                    const auto on = name_of.find(other.operator_key);
                    if (on == name_of.end()) continue;
                    /* inputs only: every operand except the record's last */
                    for (size_t i = 0; i + 1 < other.operands.size(); ++i)
                        if (other.operands[i].region == 2 &&
                            other.operands[i].offset == outslot) {
                            use = kind_of(on->second);
                            break;
                        }
                    if (use) break;
                }
                if (!use) continue;
                char sb[200];
                int at = std::snprintf(sb, sizeof sb, "0x%08X used as %-9s ops:",
                                       rec.operator_key, use);
                for (size_t i = 0; i + 1 < rec.operands.size(); ++i) {
                    const auto& op = rec.operands[i];
                    if (op.region == 0 && op.offset + 4 <= graph.constant_pool.size()) {
                        uint32_t v = 0;
                        std::memcpy(&v, graph.constant_pool.data() + op.offset, 4);
                        at += std::snprintf(sb + at, sizeof sb - (size_t)at,
                                            " [%zu]r0=0x%X", i, v);
                    } else {
                        at += std::snprintf(sb + at, sizeof sb - (size_t)at,
                                            " [%zu]r%u", i, op.region);
                    }
                }
                selector_seen[sb] += 1;
            }
        }

        /* WHICH GRAPHS ACTUALLY HOLD A RAY QUEUE NODE? The seeded suspension graphs
         * never invoke one, so count queue nodes per graph across the whole corpus. */
        {
            int nq = 0;
            for (const auto& rec : graph.records) {
                const auto nm = name_of.find(rec.operator_key);
                if (nm != name_of.end() &&
                    nm->second.compare(0, 26, "__queueAsyncPhysicsRayQuer") == 0) ++nq;
            }
            if (nq) queue_graphs[a.name] = nq;
        }

        /* THE ROOT KEYS' TRUE SHAPE. EngineNodes arity has already been wrong once
         * (the writes carry two operands, not one), so for the keys that are
         * unresolved in every suspension graph, record the operand count and regions
         * the RECORDS actually carry, across all vehicle graphs. */
        {
            static const uint32_t kRoots[] = {
                0xE22FCA6Fu, 0xC58D8EA6u, 0x63D604B7u, 0xE7488ECEu, 0xF0F74455u,
                0x040F4924u, 0x0F063D92u, 0x532B3BA9u, 0x6D98A861u,
                0xB19CEDF0u, 0x42D8AA50u, 0xAACB562Au, 0x09635061u,
                0x45DAEC6Au, 0xC1AE90A7u, 0x19A7BA16u, 0x91C21F3Cu,
                0xED79777Au, 0xC0C3BE9Fu, 0xFC69D4E9u, 0xE9560B5Eu,
                0x94A8B80Bu, 0x78AD5912u, 0x55DAC91Du, 0x1AE1C80Au,
            };
            for (const auto& rec : graph.records) {
                bool root = false;
                for (uint32_t r : kRoots) if (rec.operator_key == r) root = true;
                if (!root) continue;
                char rb[160];
                int at = std::snprintf(rb, sizeof rb, "0x%08X  %zu operands:",
                                       rec.operator_key, rec.operands.size());
                for (const auto& op : rec.operands)
                    at += std::snprintf(rb + at, sizeof rb - (size_t)at, " r%u", op.region);
                root_shapes[rb] += 1;
            }
        }

        /* THE SYNCHRONOUS WHEEL RAYCAST (0x040F4924): what produces each input.
         * Function_040f4924 PhysicsQueryResult Physics(CString Identifier, Vec3 Start,
         * Vec3 End, PhysicsQueryPreset, List<EcsEntityHandle> ...) - the widths are
         * read off the producers, the same way the async queue's were. */
        {
            std::map<uint32_t, std::string> mk;
            for (const auto& rec : graph.records) {
                if (rec.operator_key == 0x040F4924u) {
                    for (size_t i = 0; i + 1 < rec.operands.size(); ++i) {
                        const auto& op = rec.operands[i];
                        std::string what = "(unwritten)";
                        if (op.region == 0) {
                            uint32_t v = 0;
                            if (op.offset + 4 <= graph.constant_pool.size())
                                std::memcpy(&v, graph.constant_pool.data() + op.offset, 4);
                            char cv[40];
                            std::snprintf(cv, sizeof cv, "const 0x%X", v);
                            what = cv;
                        } else {
                            const auto w = mk.find(op.offset);
                            if (w != mk.end()) what = w->second;
                        }
                        char pb3[160];
                        std::snprintf(pb3, sizeof pb3, "input %zu  r%u  <- %s",
                                      i, op.region, what.c_str());
                        physq_inputs[pb3] += 1;
                    }
                }
                std::string pn;
                const auto nm = name_of.find(rec.operator_key);
                if (nm != name_of.end()) pn = nm->second;
                else if (rec.operator_key) {
                    char hx[24];
                    std::snprintf(hx, sizeof hx, "key 0x%08X", rec.operator_key);
                    pn = hx;
                } else if (rec.operands.size() >= 2) {
                    char mv[48];
                    std::snprintf(mv, sizeof mv, "MOVE kind 0x%02X", (unsigned)rec.kind);
                    pn = mv;
                }
                if (pn.empty()) continue;
                for (auto it = rec.operands.rbegin(); it != rec.operands.rend(); ++it)
                    if (it->region == 2) { mk[it->offset] = pn; break; }
            }
        }

        /* WHICH FIELD DOES 0x532B3BA9 TAKE? Its constant operand, for every call whose
         * object comes straight from the synchronous raycast. If these are the
         * PhysicsQueryResult field name hashes (HitPoint 0xFFE1C3B6, HitNormal
         * 0x23915220, LineCoordinate 0xCCD1A692 ...) the mapping is exact. */
        {
            std::set<uint32_t> ray_out;
            for (const auto& rec : graph.records) {
                if (rec.operator_key == 0x040F4924u)
                    for (auto it = rec.operands.rbegin(); it != rec.operands.rend(); ++it)
                        if (it->region == 2) { ray_out.insert(it->offset); break; }
                if (rec.operator_key != 0x532B3BA9u || rec.operands.size() < 2) continue;
                const bool from_ray = rec.operands[0].region == 2 &&
                                      ray_out.count(rec.operands[0].offset);
                uint32_t v = 0xFFFFFFFFu;
                const auto& c = rec.operands[1];
                if (c.region == 0 && c.offset + 4 <= graph.constant_pool.size())
                    std::memcpy(&v, graph.constant_pool.data() + c.offset, 4);
                char fb[96];
                std::snprintf(fb, sizeof fb, "%s field const 0x%08X",
                              from_ray ? "FROM RAYCAST" : "other object", v);
                field_ids[fb] += 1;
            }
        }

        /* HOW MUCH ROOM DOES EACH TYPED READER'S OUTPUT SLOT HAVE?
         *
         * Widening a reader from 4 bytes to 16 or 64 is only safe if the compiler
         * reserved that much, or the wider write clobbers the next slot. The distance
         * from an output slot to the next slot ANY record writes (or the end of the
         * slot file) bounds its allocation from above. Bucket it per reader. */
        {
            std::vector<uint32_t> starts;
            for (const auto& rec : graph.records)
                for (const auto& op : rec.operands)
                    if (op.region == 2) starts.push_back(op.offset);
            std::sort(starts.begin(), starts.end());
            starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
            static const uint32_t kReaders[] = {0x08F4A2D4u, 0xABAAAD01u, 0x4899CB44u,
                                                0x04BEFF62u, 0x18B2987Bu};
            for (const auto& rec : graph.records) {
                bool r = false;
                for (uint32_t k : kReaders) if (rec.operator_key == k) r = true;
                if (!r) continue;
                uint32_t outslot = 0xFFFFFFFFu;
                for (auto it = rec.operands.rbegin(); it != rec.operands.rend(); ++it)
                    if (it->region == 2) { outslot = it->offset; break; }
                if (outslot == 0xFFFFFFFFu) continue;
                const auto nx = std::upper_bound(starts.begin(), starts.end(), outslot);
                const uint32_t room = nx == starts.end()
                    ? graph.header.slot_file_size - outslot : *nx - outslot;
                const char* b = room >= 64 ? ">=64" : room >= 16 ? "16..63" : room >= 8 ? "8..15" : "<8";
                char rb[160];
                int rat = std::snprintf(rb, sizeof rb, "0x%08X  room %s", rec.operator_key, b);
                /* For the transform reader, which constant selects what? */
                if (rec.operator_key == 0xABAAAD01u || rec.operator_key == 0x08F4A2D4u)
                    for (size_t oi = 0; oi + 1 < rec.operands.size() && oi < 3; ++oi) {
                        const auto& c = rec.operands[oi];
                        uint32_t v = 0xFFFFFFFFu;
                        if (c.region == 0 && c.offset + 4 <= graph.constant_pool.size())
                            std::memcpy(&v, graph.constant_pool.data() + c.offset, 4);
                        rat += std::snprintf(rb + rat, sizeof rb - (size_t)rat, "  op%zu r%u=0x%X",
                                             oi, c.region, v);
                    }
                slot_room[rb] += 1;
                /* Mode -> first consumer, in record order (an approximation that is fine
                 * for a straight-line reader-then-use). */
                if (rec.operator_key == 0xABAAAD01u && rec.operands.size() >= 3) {
                    uint32_t mode = 0xFFFFFFFFu;
                    const auto& c = rec.operands[2];
                    if (c.region == 0 && c.offset + 4 <= graph.constant_pool.size())
                        std::memcpy(&mode, graph.constant_pool.data() + c.offset, 4);
                    std::string who = "(none)";
                    bool past = false;
                    for (const auto& r2 : graph.records) {
                        if (&r2 == &rec) { past = true; continue; }
                        if (!past) continue;
                        int hit = -1;
                        for (size_t oi = 0; oi < r2.operands.size(); ++oi)
                            if (r2.operands[oi].region == 2 && r2.operands[oi].offset == outslot) {
                                hit = (int)oi; break;
                            }
                        if (hit < 0) continue;
                        const auto nm = name_of.find(r2.operator_key);
                        char wb[96];
                        std::snprintf(wb, sizeof wb, "%s op%d/%zu",
                                      r2.operator_key == 0 ? "MOVE" :
                                      nm != name_of.end() ? nm->second.c_str() : "?",
                                      hit, r2.operands.size());
                        if (r2.operator_key != 0 && nm == name_of.end()) {
                            char hb[16]; std::snprintf(hb, sizeof hb, "0x%08X", r2.operator_key);
                            std::snprintf(wb, sizeof wb, "%s op%d/%zu", hb, hit, r2.operands.size());
                        }
                        who = wb;
                        break;
                    }
                    char mb[160];
                    std::snprintf(mb, sizeof mb, "ABAAAD01 mode %u -> %s", mode, who.c_str());
                    slot_room[mb] += 1;
                }
            }
        }

        /* MOVE KIND -> WIDTH, measured. For each operator-less move of kind 0x1E..0x25,
         * the operator that last wrote its source slot (record order), and the
         * operator that next reads its destination. A Vec3 producer or consumer means
         * the move is 16 bytes wide, a float one 4. */
        {
            for (size_t ri = 0; ri < graph.records.size(); ++ri) {
                const auto& mv = graph.records[ri];
                if (mv.has_operator || mv.kind < 0x1e || mv.kind > 0x25 || mv.operands.size() < 2)
                    continue;
                const auto& src = mv.operands.front();
                const auto& dst = mv.operands.back();
                std::string prod = src.region == 2 ? "(no writer)" :
                                   src.region == 0 ? "const" : src.region == 1 ? "instance" : "immediate";
                if (src.region == 2)
                    for (size_t j = ri; j-- > 0;) {
                        const auto& w = graph.records[j];
                        const bf6::expression::Operand* ls = nullptr;
                        for (auto it = w.operands.rbegin(); it != w.operands.rend(); ++it)
                            if (it->region == 2) { ls = &*it; break; }
                        if (!ls || ls->offset != src.offset || !w.has_operator) continue;
                        const auto nm = name_of.find(w.operator_key);
                        if (nm == name_of.end()) continue;
                        prod = nm->second;
                        break;
                    }
                std::string cons = "(none named)";
                for (size_t j = ri + 1; j < graph.records.size(); ++j) {
                    const auto& r2 = graph.records[j];
                    if (!r2.has_operator) continue;
                    bool hit = false;
                    for (size_t oi = 0; oi + 1 < r2.operands.size(); ++oi)
                        if (r2.operands[oi].region == 2 && r2.operands[oi].offset == dst.offset) hit = true;
                    if (!hit) continue;
                    const auto nm = name_of.find(r2.operator_key);
                    if (nm == name_of.end()) continue;
                    cons = nm->second;
                    break;
                }
                auto cls = [](const std::string& n) -> const char* {
                    if (n.find("LinearTransform") != std::string::npos) return "LT";
                    if (n.find("Float3") != std::string::npos || n.find("Vec3") != std::string::npos) return "V3";
                    if (n.find("Float") != std::string::npos) return "F";
                    if (n.find("Int") != std::string::npos) return "I";
                    if (n.find("Bool") != std::string::npos) return "B";
                    return n.c_str()[0] == '(' ? "-" : "?";
                };
                char kb[96];
                std::snprintf(kb, sizeof kb, "kind 0x%02X  src %-3s dst-use %-3s", mv.kind,
                              cls(prod), cls(cons));
                move_kinds[kb] += 1;
                if ((mv.kind == 0x24 || mv.kind == 0x25) && mv.trailing_dword == 0) {
                    /* Width-less typed copies: is the destination (or source) in the
                     * graph's typed slot table, and with what type and what spacing? */
                    uint32_t ty = 0;
                    bool in_tab = false;
                    for (const auto& g : graph.slot_values)
                        for (uint32_t o : g.offsets)
                            if (dst.region == 2 && o == dst.offset) { in_tab = true; ty = g.data_type_id; }
                    bool in_itab = false;
                    for (const auto& g : graph.instance_values)
                        for (uint32_t o : g.offsets)
                            if (src.region == 1 && o == src.offset) { in_itab = true; ty = g.data_type_id; }
                    char zb[128];
                    std::snprintf(zb, sizeof zb, "trail0 kind 0x%02X src r%u dst r%u  dst-in-slot-table %d src-in-inst-table %d type 0x%08X",
                                  mv.kind, src.region, dst.region, (int)in_tab, (int)in_itab, ty);
                    move_kinds[zb] += 1;
                }
                if (mv.kind == 0x22 && dst.region == 2) {
                    /* Room at the destination: the nearest OTHER slot any record names
                     * above it. Under 16 means a 16-byte write would reach another slot. */
                    uint32_t room = 0xFFFFFFFFu;
                    for (const auto& rr : graph.records)
                        for (const auto& op : rr.operands)
                            if (op.region == 2 && op.offset > dst.offset &&
                                op.offset - dst.offset < room) room = op.offset - dst.offset;
                    char mb2[96];
                    std::snprintf(mb2, sizeof mb2, "kind 0x22 dst room %s  src %s dst-use %s",
                                  room >= 16 ? ">=16" : room >= 8 ? "8..15" : "<8",
                                  cls(prod), cls(cons));
                    move_kinds[mb2] += 1;
                }
                /* Region-3 sources within 3 records after a kind 0x2E: offsets, and
                 * which operator built the reference 0x2E bound. */
                if (src.region == 3) {
                    std::string after = "no 0x2E before";
                    for (size_t j = ri, n = 0; j-- > 0 && n < 3; ++n) {
                        const auto& b = graph.records[j];
                        if (b.kind != 0x2E || b.operands.empty()) continue;
                        std::string by = "?";
                        for (size_t k = j; k-- > 0;) {
                            const auto& w = graph.records[k];
                            if (!w.has_operator || w.operands.empty() ||
                                w.operands.back().offset != b.operands[0].offset) continue;
                            char hb[16]; std::snprintf(hb, sizeof hb, "0x%08X", w.operator_key);
                            by = hb;
                            break;
                        }
                        after = "after 0x2E(ref from " + by + ")";
                        break;
                    }
                    char rb3[160];
                    std::snprintf(rb3, sizeof rb3, "r3 src kind 0x%02X  %s  +0x%X", mv.kind,
                                  after.c_str(), src.offset);
                    move_kinds[rb3] += 1;
                }
            }
        }

        /* WHICH OPERATORS TAKE A BOUND CHANNEL, with what shape. Operand 0 in a
         * pool entry the EBX binds = a channel operator. */
        {
            std::map<uint32_t, const bf6_channel_binding*> bound;
            for (const auto& b : cur_bindings) if (b.region == 0) bound[b.pool_offset] = &b;
            for (const auto& rec : graph.records) {
                if (!rec.has_operator || rec.operands.empty()) continue;
                const auto& op0 = rec.operands[0];
                if (op0.region != 0) continue;
                const auto bi = bound.find(op0.offset);
                if (bi == bound.end()) continue;
                const auto nm = name_of.find(rec.operator_key);
                std::string shape;
                for (const auto& op : rec.operands) {
                    char ob[8];
                    std::snprintf(ob, sizeof ob, " r%u", op.region > 2 ? 3u : op.region);
                    shape += ob;
                }
                char cb[160];
                std::snprintf(cb, sizeof cb, "0x%08X %-12s shape%s", rec.operator_key,
                              nm != name_of.end() ? nm->second.c_str() : "", shape.c_str());
                chan_ops[cb] += 1;
                std::snprintf(cb, sizeof cb, "  0x%08X on %s", rec.operator_key, bi->second->channel_name);
                chan_ops[cb] += 1;
            }
        }

        /* CONTROL: pure operators only. */
        bf6::expression::Instance inst_c;
        if (bf6::expression::make_instance(graph, inst_c, why)) {
            fill_types(graph, inst_c);
            const auto r = bf6::expression::evaluate(graph, &inst_c, {}, &builtins);
            control.unresolved_uses += r.unresolved_keys.size();
            if (r.result.known && !r.result.tainted) { ++control.sound; std::printf("SOUNDCTL %s\n", a.name); }
            if (std::strstr(a.name, "diceex_aim_attachoffset_freelook") != nullptr) {
                bf6::expression::Instance ti;
                std::string te;
                if (bf6::expression::make_instance(graph, ti, te)) {
                    fill_types(graph, ti);
                    ti.trace_records = true;
                    const auto tr2 = bf6::expression::evaluate(graph, &ti, {}, &builtins);
                    std::printf("AIM result known %d tainted %d termination %d\n",
                                (int)tr2.result.known, (int)tr2.result.tainted, (int)tr2.termination);
                    for (const auto& rec : graph.records) {
                        const auto nm = name_of.find(rec.operator_key);
                        std::string ops_s;
                        for (const auto& op : rec.operands) {
                            char ob[40];
                            std::snprintf(ob, sizeof ob, " r%u@0x%X", op.region, op.offset);
                            ops_s += ob;
                        }
                        std::printf("AIM rec 0x%X kind 0x%02X %-36s%s\n", rec.offset, rec.kind,
                                    !rec.has_operator ? "-" : nm != name_of.end() ? nm->second.c_str() : "?",
                                    ops_s.c_str());
                    }
                    for (const auto& row : ti.trace)
                        std::printf("AIM row rec 0x%X key 0x%08X slot 0x%X w%u %s\n", row.record_offset,
                                    row.key, row.slot, row.width, row.known ? "known" : "UNKNOWN");
                }
            }
        }

        /* TEST: the same, plus the state host. A FRESH instance, because an
         * evaluation mutates its instance image and reusing it would hand the
         * second run the first run's leftovers. */
        bf6::expression::Instance inst_t;
        if (bf6::expression::make_instance(graph, inst_t, why)) {
            fill_types(graph, inst_t);
            bf6::expression::StateHost state;
            feed_rig(state, a);
            /* BF6_CHAN_SET="Name=value,Name=value": set bound channels BY NAME
             * before the run - a driver's inputs. value suffix: b bool, i int,
             * none float. Only channels this graph binds are set. */
            if (const char* cs = std::getenv("BF6_CHAN_SET")) {
                std::string all(cs);
                size_t pos = 0;
                while (pos < all.size()) {
                    size_t comma = all.find(',', pos);
                    if (comma == std::string::npos) comma = all.size();
                    const std::string item = all.substr(pos, comma - pos);
                    pos = comma + 1;
                    const size_t eq = item.find('=');
                    if (eq == std::string::npos) continue;
                    const std::string nm = item.substr(0, eq);
                    std::string val = item.substr(eq + 1);
                    std::vector<uint8_t> bytes;
                    if (!val.empty() && val.back() == 'b') {
                        bytes.assign(1, (uint8_t)(std::atoi(val.c_str()) != 0));
                    } else if (!val.empty() && val.back() == 'i') {
                        const int32_t iv = std::atoi(val.c_str());
                        bytes.resize(4); std::memcpy(bytes.data(), &iv, 4);
                    } else {
                        const float fv = (float)std::atof(val.c_str());
                        bytes.resize(4); std::memcpy(bytes.data(), &fv, 4);
                    }
                    for (const auto& b : cur_bindings)
                        if (b.kind == 0 && nm == b.channel_name) state.set_channel(b.channel_hash, bytes);
                }
            }
            /* WRITES ON. Everything else is now ruled out as the source of the
             * unknowns: PUSH is known and binds no paths (authored), the state reads
             * resolve, the constant pool is in bounds, there is no slot file to load
             * and external_bindings is 0. The one remaining candidate is the piece
             * this host switched OFF itself - if the graph stores a transform in a
             * slot and reads it back, refusing the write breaks the chain. */
            /* WRITES ON, and now with a MEASURED two-operand signature
             * (destination, value) rather than the arity-1 guess that made every
             * write refuse. */
            state.set_allow_writes(true);
            if (std::strstr(a.name, "suspensionmovement") != nullptr)
                state.set_trace_frames(true);
            bf6::expression::PhysicsQueryHost physics;
            physics.set_tracer(trace_scene, scene);
            for (const auto& row : names)
                if (row.match_count == 1) physics.add(row.key, row.name);
            bf6::expression::ChainHost both;
            both.add(&builtins);
            bf6::expression::RecoveredOps recovered;
            bf6::expression::WorldHost world;
            for (const auto& b : cur_bindings)
                if (b.kind == 2) world.set_tweakable(b.channel_hash, b.default_bits);
            both.add(&pure);
            both.add(&recovered);
            both.add(&world);
            both.add(&physics);
            both.add(&state);
            const auto r = bf6::expression::evaluate(graph, &inst_t, {}, &both);
            tested.unresolved_uses += r.unresolved_keys.size();
            /* RANK what still blocks: per key, how many graphs it leaves unresolved,
             * whether the graph came out sound otherwise, and its name. */
            for (uint32_t k : r.unresolved_keys) {
                const auto nm = name_of.find(k);
                char kb[128];
                std::snprintf(kb, sizeof kb, "0x%08X %s", k, nm != name_of.end() ? nm->second.c_str() : "");
                std::printf("UNRES %s\n", kb);
                bf6::expression::OperatorSignature dsig;
                std::printf("%s %s\n", both.describe(k, dsig) ? "SYMPTOM" : "CAUSE", kb);
                std::printf("CAUSEIN 0x%08X %s\n", k, a.name);
                if (r.unresolved_keys.size() == 1) std::printf("SOLEBLOCK %s\n", kb);
            }
            if (r.result.known && !r.result.tainted) { ++tested.sound; std::printf("SOUNDTEST %s\n", a.name); }
            /* BF6_CHANDUMP=<substr>: for matching graphs, the channels the TEST pass
             * set and the ones it read unset, by bound name, plus its run shape. */
            if (const char* cd = std::getenv("BF6_CHANDUMP"))
                if (std::strstr(a.name, cd)) {
                    std::map<uint32_t, std::string> cname;
                    for (const auto& b : cur_bindings) cname[b.channel_hash] = b.channel_name;
                    auto nm_c = [&](uint64_t k) {
                        const auto f = cname.find((uint32_t)k);
                        return (f != cname.end() ? f->second : std::string("?")) +
                               " (mode " + std::to_string((unsigned)(k >> 32)) + ")";
                    };
                    std::printf("RUN %s termination %d steps %u guessed %u unresolved %zu\n", a.name,
                                (int)r.termination, r.steps, r.guessed_branches, r.unresolved_keys.size());
                    for (const auto& kv : state.channel_writes()) {
                        const auto v = state.channels().find(kv.first);
                        float fv = 0;
                        if (v != state.channels().end() && v->second.size() >= 4) std::memcpy(&fv, v->second.data(), 4);
                        std::printf("SET %-40s %s %g\n", nm_c(kv.first).c_str(),
                                    v == state.channels().end() ? "UNKNOWN" : "known", fv);
                    }
                    for (const auto& kv : state.unsupplied_channels())
                        std::printf("READ-UNSET %s x%u\n", nm_c(kv.first).c_str(), kv.second);
                }
            for (const auto& kv : state.served()) served[kv.first] += kv.second;
            ray_queries += physics.queries();
            ray_hits += physics.hits();
            /* WHAT DOES THE SUSPENSION GRAPH ACTUALLY ASK FOR?
             *
             * Every cell it READ without being seeded, recorded by the host. This is
             * the list of inputs a vehicle needs supplied, discovered rather than
             * guessed - and the hashes resolve back to authored names through
             * StateHost::path_of, which reproduces all 128 known pairs. */
            if (std::strstr(a.name, "suspensionmovement") != nullptr) {
                /* THE TICK TEST.
                 *
                 * The issue-then-consume ray pattern assumes FRAMES, and the FX logic
                 * names the gate ("RayCast Ready To Be Consumed" / "Consumed"). One
                 * evaluation cannot express that. So run the same graph repeatedly
                 * against the SAME StateHost and the same PhysicsQueryHost - state and
                 * queued queries both persist - and report when rays first appear.
                 * A fresh Instance each pass, because an evaluation mutates its image.
                 *
                 * If rays appear on pass 2+ the hypothesis holds and the missing piece
                 * was always the tick. If they never appear it is refuted and the
                 * cause is elsewhere. */
                const int before_q = physics.queries();
                for (int pass = 2; pass <= 9; ++pass) {
                    bf6::expression::Instance again;
                    std::string aw;
                    if (!bf6::expression::make_instance(graph, again, aw)) break;
                    fill_types(graph, again);
                    bf6::expression::evaluate(graph, &again, {}, &both);
                    if (physics.queries() > before_q) {
                        tick_first_ray[pass] += 1;
                        break;
                    }
                }
                /* THE SEEDED PASS: give the graph the inputs the engine would.
                 *
                 * The wheel point the ray-origin multiply reads is written by NOTHING in
                 * the graph (26 of 26), so the engine fills it before the graph runs.
                 * Fill it the same way through Instance::slot_seed: every "written
                 * nowhere" point operand gets a real wheel position off the flyer60 rig
                 * (loc_Wheel_FrontLeft, 0.725 0.473 1.639 - the bind pose that matched
                 * the Godot build to the centimetre), and every "written nowhere"
                 * transform operand gets identity. Then count queue ATTEMPTS as well as
                 * queries, so a refusal cannot hide as silence.
                 */
                {
                    std::set<uint32_t> ever;
                    for (const auto& rec : graph.records)
                        for (auto it = rec.operands.rbegin(); it != rec.operands.rend(); ++it)
                            if (it->region == 2) { ever.insert(it->offset); break; }
                    /* ENGINE-FILLED SLOTS: read by a record, written by none. Does the
                     * graph's own slot value table type them? */
                    std::vector<std::pair<uint32_t, std::string>> ef_slots;
                    {
                        std::map<uint32_t, uint32_t> typed;
                        for (const auto& g : graph.slot_values)
                            for (uint32_t o : g.offsets) typed[o] = g.data_type_id;
                        std::set<uint32_t> done;
                        for (const auto& rec : graph.records) {
                            int last2 = -1;
                            for (int oi = 0; oi < (int)rec.operands.size(); ++oi)
                                if (rec.operands[oi].region == 2) last2 = oi;
                            for (int oi = 0; oi < (int)rec.operands.size(); ++oi) {
                                const auto& op = rec.operands[oi];
                                if (op.region != 2 || oi == last2 || ever.count(op.offset)) continue;
                                if (!done.insert(op.offset).second) continue;
                                const auto t = typed.find(op.offset);
                                const auto nm = name_of.find(rec.operator_key);
                                char eb[200];
                                std::snprintf(eb, sizeof eb, "%-38s -> %s op%d  %s",
                                              std::strrchr(a.name, '/') + 1,
                                              rec.operator_key == 0 ? "MOVE" :
                                              nm != name_of.end() ? nm->second.c_str() : "?",
                                              oi,
                                              t == typed.end() ? "NOT in slot table" : "typed");
                                engine_filled[eb] += 1;
                                ef_slots.push_back({op.offset,
                                    rec.operator_key == 0 ? std::string("MOVE") :
                                    nm != name_of.end() ? nm->second : std::string("?")});
                                if (t != typed.end()) {
                                    char tb[64];
                                    std::snprintf(tb, sizeof tb, "type 0x%08X", t->second);
                                    engine_filled[std::string("   ") + tb] += 1;
                                }
                            }
                        }
                        char sb[128];
                        std::snprintf(sb, sizeof sb, "%-38s slot table: %zu groups",
                                      std::strrchr(a.name, '/') + 1, graph.slot_values.size());
                        engine_filled[sb] += 1;
                    }
                    bf6::expression::Instance seeded;
                    std::string sw;
                    if (bf6::expression::make_instance(graph, seeded, sw)) {
                        fill_types(graph, seeded);
                        const float wheel[3] = {0.725f, 0.473f, 1.639f};
                        bf6::expression::Value point;
                        point.bytes.assign(16, 0);
                        std::memcpy(point.bytes.data(), wheel, 12);
                        point.known = true;
                        bf6::expression::Value ident;
                        ident.bytes.assign(64, 0);
                        const float one = 1.0f;
                        std::memcpy(ident.bytes.data() + 0, &one, 4);
                        std::memcpy(ident.bytes.data() + 20, &one, 4);
                        std::memcpy(ident.bytes.data() + 40, &one, 4);
                        ident.known = true;
                        int seeded_points = 0, seeded_xforms = 0;
                        for (const auto& rec : graph.records) {
                            const auto nm = name_of.find(rec.operator_key);
                            if (nm == name_of.end() ||
                                nm->second != "MultiplyFloat3LinearTransformFloat3" ||
                                rec.operands.size() < 2) continue;
                            const auto& pt = rec.operands[0];
                            const auto& xf = rec.operands[1];
                            if (pt.region == 2 && !ever.count(pt.offset)) {
                                seeded.slot_seed[pt.offset] = point; ++seeded_points;
                            }
                            if (xf.region == 2 && !ever.count(xf.offset)) {
                                seeded.slot_seed[xf.offset] = ident; ++seeded_xforms;
                            }
                        }
                        const int q0 = physics.queries(), a0 = physics.attempts();
                        const int u0 = physics.unknown_ends(), h0 = physics.hits();
                        seeded.trace_records = true;
                        /* BF6_ROOT_Y=<metres>: place the vehicle (the RootTransform
                         * channel, hash 0x5F9C8163) that high above the ground plane. */
                        if (const char* ry = std::getenv("BF6_ROOT_Y")) {
                            float rows[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,0};
                            rows[13] = (float)std::atof(ry);
                            /* BF6_ROOT_PITCH=<radians>: pitch about X (positive lifts the REAR), so the front and
                             * rear wheels sit at different heights. */
                            if (const char* rp = std::getenv("BF6_ROOT_PITCH")) {
                                const float p = (float)std::atof(rp), c = std::cos(p), s = std::sin(p);
                                rows[4] = 0; rows[5] = c;  rows[6] = s;    /* up      */
                                rows[8] = 0; rows[9] = -s; rows[10] = c;   /* forward */
                            }
                            state.set_named_transform(0x5F9C8163u, rows);
                        }
                        const size_t ray0 = physics.ray_log().size();
                        const auto sr = bf6::expression::evaluate(graph, &seeded, {}, &both);
                        /* FORWARD from the first executed raycast: every later row that
                         * reads its result or anything derived from it. */
                        if (std::strstr(a.name, "flyer60") != nullptr) {
                            for (const auto& rec : graph.records) {
                                if ((rec.offset < 0x1C0 || rec.offset > 0x2D0) && rec.offset < 0x5A00) continue;
                                const auto nm = name_of.find(rec.operator_key);
                                std::string ops_s;
                                for (const auto& op : rec.operands) {
                                    char ob[40];
                                    uint32_t v = 0xFFFFFFFFu;
                                    if (op.region == 0 && op.offset + 4 <= graph.constant_pool.size())
                                        std::memcpy(&v, graph.constant_pool.data() + op.offset, 4);
                                    if (op.region == 0) std::snprintf(ob, sizeof ob, " c=0x%X", v);
                                    else std::snprintf(ob, sizeof ob, " r%u@0x%X", op.region, op.offset);
                                    ops_s += ob;
                                }
                                std::printf("LIST rec 0x%X kind 0x%02X %-44s%s  next 0x%X ctl 0x%X len %u trail %s0x%X\n",
                                            rec.offset, rec.kind,
                                            !rec.has_operator ? "-" :
                                            nm != name_of.end() ? nm->second.c_str() : "?",
                                            ops_s.c_str(), rec.next, rec.control_target,
                                            rec.byte_length, rec.has_trailing_dword ? "" : "(none)",
                                            rec.trailing_dword);
                                if (rec.has_operator && nm == name_of.end())
                                    std::printf("        (key 0x%08X)\n", rec.operator_key);
                            }
                            std::map<uint32_t, const bf6::expression::Record*> by_off2;
                            for (const auto& rec : graph.records) by_off2[rec.offset] = &rec;
                            std::vector<std::pair<uint32_t, uint32_t>> live; /* [lo, hi) */
                            bool started = false;
                            int shown = 0;
                            for (const auto& row : seeded.trace) {
                                const auto rit = by_off2.find(row.record_offset);
                                if (rit == by_off2.end()) continue;
                                const auto& ops = rit->second->operands;
                                if (!started) {
                                    if (row.key == 0x040F4924u && row.width) {
                                        started = true;
                                        live.push_back({row.slot, row.slot + row.width});
                                        std::printf("FWD ray out slot 0x%X w%u\n", row.slot, row.width);
                                    }
                                    continue;
                                }
                                int last2 = -1;
                                for (int oi = 0; oi < (int)ops.size(); ++oi)
                                    if (ops[oi].region == 2) last2 = oi;
                                bool reads = false;
                                std::string which;
                                for (int oi = 0; oi < (int)ops.size(); ++oi) {
                                    if (ops[oi].region != 2) continue;
                                    if (oi == last2 && ops[oi].offset == row.slot) continue;
                                    for (const auto& lv : live)
                                        if (ops[oi].offset >= lv.first && ops[oi].offset < lv.second) {
                                            reads = true;
                                            char ob[48]; std::snprintf(ob, sizeof ob, " in%d@+0x%X", oi, ops[oi].offset - lv.first);
                                            which += ob;
                                            break;
                                        }
                                }
                                if (!reads) continue;
                                if (row.width) live.push_back({row.slot, row.slot + row.width});
                                if (shown++ < 30) {
                                    const auto nm = name_of.find(row.key);
                                    std::string ops_s;
                                    for (const auto& op : ops) {
                                        char ob[40];
                                        uint32_t v = 0xFFFFFFFFu;
                                        if (op.region == 0 && op.offset + 4 <= graph.constant_pool.size())
                                            std::memcpy(&v, graph.constant_pool.data() + op.offset, 4);
                                        if (op.region == 0) std::snprintf(ob, sizeof ob, " c=0x%X", v);
                                        else std::snprintf(ob, sizeof ob, " r%u@0x%X", op.region, op.offset);
                                        ops_s += ob;
                                    }
                                    std::printf("FWD rec 0x%X %-40s w%-3u %-7s%s |%s\n", row.record_offset,
                                                row.key == 0 ? "MOVE" : nm != name_of.end() ? nm->second.c_str() : "?",
                                                row.width, row.known ? "known" : "UNKNOWN",
                                                which.c_str(), ops_s.c_str());
                                    if (row.key != 0 && nm == name_of.end())
                                        std::printf("        (key 0x%08X)\n", row.key);
                                }
                            }
                        }
                        if (std::strstr(a.name, "flyer60") != nullptr)
                            for (const auto& row : seeded.trace)
                                if (row.record_offset >= 0x1C0 && row.record_offset < 0x2D0) {
                                    std::string val;
                                    if (row.width == 4 || row.width == 16) {
                                        const auto v = bf6::expression::Instance{}; (void)v;
                                    }
                                    std::printf("F60 row rec 0x%X key 0x%08X slot 0x%X w%u %s\n",
                                                row.record_offset, row.key, row.slot, row.width,
                                                row.known ? "known" : "UNKNOWN");
                                }
                        if (std::strstr(a.name, "flyer60") != nullptr) {
                            std::printf("HDR pool %u slots %u relocs %zu fixups %zu ptrtab %u extb %u inst_img %zu\n",
                                        graph.header.constant_pool_size, graph.header.slot_file_size,
                                        graph.relocations.size(), graph.fixups.size(),
                                        graph.header.pointer_table_entries, graph.header.external_bindings,
                                        graph.instance_image.size());
                            for (const auto& rec : graph.records)
                                if (rec.operator_key == 0x30FAAAB6u) {
                                    std::printf("PUSHOPS rec 0x%X", rec.offset);
                                    for (const auto& op : rec.operands) {
                                        uint32_t v = 0;
                                        if (op.region == 0 && op.offset + 4 <= graph.constant_pool.size())
                                            std::memcpy(&v, graph.constant_pool.data() + op.offset, 4);
                                        std::printf("  r%u@0x%X=%08X", op.region, op.offset, v);
                                    }
                                    std::printf("\n");
                                }
                            for (const auto& rl : graph.relocations) {
                                std::string str;
                                for (uint32_t b = rl.target; b < graph.constant_pool.size() && b < rl.target + 160; ++b) {
                                    const char ch = (char)graph.constant_pool[b];
                                    if (!ch) break;
                                    str += (ch >= 32 && ch < 127) ? ch : '.';
                                }
                                std::printf("RELOC field 0x%X -> 0x%X  \"%s\"\n", rl.pointer_field, rl.target, str.c_str());
                            }
                            for (const auto& fx : graph.fixups)
                                std::printf("FIXUP key 0x%08X rec 0x%X\n", fx.key, fx.record_offset);
                            for (const auto& rec : graph.records)
                                if (!rec.has_operator && rec.kind == 0x24 && rec.trailing_dword == 0 &&
                                    !rec.operands.empty() && rec.operands[0].region == 0) {
                                    const uint32_t po = rec.operands[0].offset;
                                    std::printf("BONECONST rec 0x%X pool+0x%X ->0x%X  bytes", rec.offset, po,
                                                rec.operands.back().offset);
                                    for (uint32_t b = 0; b < 16 && po + b < graph.constant_pool.size(); ++b)
                                        std::printf(" %02X", graph.constant_pool[po + b]);
                                    std::printf("\n");
                                }
                        }
                        if (std::strstr(a.name, "flyer60") != nullptr)
                            for (const auto& t : graph.types)
                                std::printf("GTYPE 0x%08X carried %08X %08X %08X %08X\n", t.type_id,
                                            t.carried[0], t.carried[1], t.carried[2], t.carried[3]);
                        if (std::strstr(a.name, "flyer60") != nullptr) {
                            std::map<uint32_t, std::string> cname;
                            for (const auto& b : cur_bindings) cname[b.channel_hash] = b.channel_name;
                            auto nm_c = [&](uint64_t k) {
                                const auto f = cname.find((uint32_t)k);
                                char nb[96];
                                std::snprintf(nb, sizeof nb, "%s (mode %u)",
                                              f != cname.end() ? f->second.c_str() : "?", (unsigned)(k >> 32));
                                return std::string(nb);
                            };
                            for (const auto& kv : state.channel_writes()) {
                                const auto v = state.channels().find(kv.first);
                                float fv = 0; uint32_t uv = 0;
                                if (v != state.channels().end() && v->second.size() >= 4) {
                                    std::memcpy(&fv, v->second.data(), 4); std::memcpy(&uv, v->second.data(), 4);
                                }
                                std::printf("CHAN SET %-40s x%u  value %s f=%g u=%u\n", nm_c(kv.first).c_str(),
                                            kv.second, v == state.channels().end() ? "UNKNOWN" : "known", fv, uv);
                            }
                            for (const auto& kv : state.unsupplied_channels())
                                std::printf("CHAN READ unsupplied %-40s x%u\n", nm_c(kv.first).c_str(), kv.second);
                        }
                        if (std::strstr(a.name, "flyer60") != nullptr)
                            for (const auto& kv : state.unsupplied_parts())
                                std::printf("PART asked: mode %u selector 0x%X bone %08X %08X %08X %08X  x%u\n",
                                            kv.first.mode, kv.first.selector, kv.first.bone[0],
                                            kv.first.bone[1], kv.first.bone[2], kv.first.bone[3], kv.second);
                        if (std::strstr(a.name, "thebeast") != nullptr) {
                            std::printf("BEAST termination %d steps %u diags %zu\n",
                                        (int)sr.termination, sr.steps, sr.diagnostics.size());
                            for (const auto& d : sr.diagnostics) std::printf("BEAST diag %s\n", d.c_str());
                            for (const auto& row : seeded.trace)
                                std::printf("BEAST row rec 0x%X key 0x%08X slot 0x%X w%u %s\n",
                                            row.record_offset, row.key, row.slot, row.width,
                                            row.known ? "known" : "UNKNOWN");
                        }
                        for (size_t ri = ray0; ri < physics.ray_log().size(); ++ri) {
                            const auto& rl = physics.ray_log()[ri];
                            std::printf("RAY %-38s %s from (%.3f %.3f %.3f) to (%.3f %.3f %.3f) %s\n",
                                        std::strrchr(a.name, '/') + 1, rl.sync ? "sync " : "async",
                                        rl.from[0], rl.from[1], rl.from[2],
                                        rl.to[0], rl.to[1], rl.to[2], rl.hit ? "HIT" : "miss");
                        }
                        /* Is each "written nowhere" slot actually INSIDE a wider write at
                         * runtime? Report the covering writer and the byte offset into it. */
                        for (const auto& ef : ef_slots) {
                            std::string cover = "covered by NOTHING at runtime";
                            for (const auto& row : seeded.trace) {
                                if (!row.width || row.slot == ef.first) continue;
                                if (row.slot < ef.first && ef.first < row.slot + row.width) {
                                    const auto nm = name_of.find(row.key);
                                    char cb[160];
                                    std::snprintf(cb, sizeof cb, "INSIDE a w%u write by %s at +%u",
                                                  row.width,
                                                  row.key == 0 ? "MOVE" :
                                                  nm != name_of.end() ? nm->second.c_str() : "?",
                                                  ef.first - row.slot);
                                    if (row.key != 0 && nm == name_of.end())
                                        std::snprintf(cb, sizeof cb, "INSIDE a w%u write by 0x%08X at +%u",
                                                      row.width, row.key, ef.first - row.slot);
                                    cover = cb;
                                    break;
                                }
                            }
                            engine_filled[std::string("  COVER: ") + ef.second + " input  " + cover] += 1;
                            /* Every OTHER record that names this slot, anywhere in its
                             * operands. A multi-output operator would show up here as a
                             * non-last reference preceding the read. */
                            std::string refs;
                            for (const auto& rec : graph.records) {
                                for (size_t oi = 0; oi < rec.operands.size(); ++oi) {
                                    if (rec.operands[oi].region != 2 ||
                                        rec.operands[oi].offset != ef.first) continue;
                                    const auto nm = name_of.find(rec.operator_key);
                                    char rb2[96];
                                    if (rec.operator_key == 0) std::snprintf(rb2, sizeof rb2, " MOVE:%zu/%zu", oi, rec.operands.size());
                                    else if (nm != name_of.end()) std::snprintf(rb2, sizeof rb2, " %s:%zu/%zu", nm->second.c_str(), oi, rec.operands.size());
                                    else std::snprintf(rb2, sizeof rb2, " 0x%08X:%zu/%zu", rec.operator_key, oi, rec.operands.size());
                                    refs += rb2;
                                }
                            }
                            if (refs.size() > 150) refs.resize(150);
                            engine_filled[std::string("  REFS: ") + refs] += 1;
                        }
                        /* WALK BACK FROM EACH EXECUTED RAYCAST, AT RUNTIME.
                         *
                         * For every slot input of a raycast row, find the LAST trace row
                         * before it that wrote that slot. If that write was unknown, recurse
                         * into the writer's own slot inputs. The walk stops at the FIRST
                         * CULPRIT: an unknown write whose slot inputs were all known (the
                         * operator itself produced the unknown, or was refused), or a slot
                         * nothing wrote and nothing seeded (born unknown). */
                        {
                            std::map<uint32_t, const bf6::expression::Record*> by_off;
                            for (const auto& rec : graph.records) by_off[rec.offset] = &rec;
                            const auto& tr = seeded.trace;
                            auto last_writer = [&](size_t before, uint32_t slot) -> long {
                                for (size_t i = before; i-- > 0;) {
                                    const uint32_t w = tr[i].width ? tr[i].width : 4u;
                                    if (tr[i].slot <= slot && slot < tr[i].slot + w)
                                        return (long)i;
                                }
                                return -1;
                            };
                            auto nm_of = [&](uint32_t k) -> std::string {
                                if (k == 0) return "MOVE";
                                const auto f = name_of.find(k);
                                char hb[16];
                                std::snprintf(hb, sizeof hb, "0x%08X", k);
                                return f == name_of.end() ? std::string(hb)
                                                          : std::string(hb) + " " + f->second;
                            };
                            bool printed_one = false;
                            /* BF6_WHY=<hex record offset>: walk back from that record
                             * instead of from the raycasts. */
                            const char* why_s = std::getenv("BF6_WHY");
                            const uint32_t why = why_s ? (uint32_t)std::strtoul(why_s, nullptr, 16) : 0u;
                            /* BF6_TOUCH=lo-hi (hex): every record naming a slot in range. */
                            if (const char* touch = std::getenv("BF6_TOUCH")) {
                                unsigned lo = 0, hi = 0;
                                if (std::sscanf(touch, "%x-%x", &lo, &hi) == 2)
                                    for (const auto& rec : graph.records)
                                        for (size_t oi = 0; oi < rec.operands.size(); ++oi) {
                                            const auto& op = rec.operands[oi];
                                            /* BF6_TOUCH_ANY=1: any region, not only slots. */
                                            if ((op.region != 2 && !std::getenv("BF6_TOUCH_ANY")) ||
                                                op.region > 2 || op.offset < lo || op.offset >= hi) continue;
                                            const auto nm = name_of.find(rec.operator_key);
                                            std::printf("TOUCH rec 0x%X kind 0x%02X %-40s op%zu/%zu r%u@0x%X\n",
                                                        rec.offset, rec.kind,
                                                        !rec.has_operator ? "-" : nm != name_of.end() ? nm->second.c_str() : "?",
                                                        oi, rec.operands.size(), op.region, op.offset);
                                            if (rec.has_operator && nm == name_of.end())
                                                std::printf("        (key 0x%08X)\n", rec.operator_key);
                                        }
                            }
                            for (size_t ri = 0; ri < tr.size(); ++ri) {
                                if (why ? tr[ri].record_offset != why : tr[ri].key != 0x040F4924u) continue;
                                std::vector<std::pair<size_t, int>> stack{{ri, 0}};
                                std::set<size_t> seen;
                                std::string chain;
                                while (!stack.empty()) {
                                    const auto cur = stack.back();
                                    stack.pop_back();
                                    if (!seen.insert(cur.first).second || cur.second > 40) continue;
                                    const auto rit = by_off.find(tr[cur.first].record_offset);
                                    if (rit == by_off.end()) continue;
                                    const auto& ops = rit->second->operands;
                                    int last2 = -1;
                                    for (int oi = 0; oi < (int)ops.size(); ++oi)
                                        if (ops[oi].region == 2) last2 = oi;
                                    const bool has_out = last2 >= 0 &&
                                        tr[cur.first].slot == ops[last2].offset;
                                    int unknown_inputs = 0;
                                    for (int oi = 0; oi < (int)ops.size(); ++oi) {
                                        if (ops[oi].region != 2 || (has_out && oi == last2)) continue;
                                        const long w = last_writer(cur.first, ops[oi].offset);
                                        if (w < 0) {
                                            if (seeded.slot_seed.count(ops[oi].offset)) continue;
                                            ++unknown_inputs;
                                            char cb[200];
                                            std::snprintf(cb, sizeof cb,
                                                "BORN UNKNOWN: input %d of %s (w%u)",
                                                oi, nm_of(tr[cur.first].key).c_str(), tr[cur.first].width);
                                            ray_culprits[cb] += 1;
                                            if (!printed_one) chain += std::string("      ") + cb + "\n";
                                            continue;
                                        }
                                        if (tr[(size_t)w].known) continue;
                                        ++unknown_inputs;
                                        if (!printed_one) {
                                            char cb[200];
                                            std::snprintf(cb, sizeof cb,
                                                "      depth %d: rec 0x%X %s <- input %d slot 0x%X from rec 0x%X %s (w%u)\n",
                                                cur.second, tr[cur.first].record_offset,
                                                nm_of(tr[cur.first].key).c_str(), oi,
                                                ops[oi].offset, tr[(size_t)w].record_offset,
                                                nm_of(tr[(size_t)w].key).c_str(), tr[(size_t)w].width);
                                            chain += cb;
                                        }
                                        stack.push_back({(size_t)w, cur.second + 1});
                                    }
                                    if (unknown_inputs == 0 && cur.first != ri) {
                                        char cb[200];
                                        int at = std::snprintf(cb, sizeof cb, "FIRST CULPRIT: %s w%u %s ops:",
                                                      nm_of(tr[cur.first].key).c_str(), tr[cur.first].width,
                                                      tr[cur.first].width ? "(ran, output unknown)"
                                                                          : "(REFUSED)");
                                        for (const auto& op : ops)
                                            at += std::snprintf(cb + at, sizeof cb - (size_t)at,
                                                                " r%u:%s", op.region,
                                                                op.region == 2 ? "slot" : "c");
                                        /* Who wrote the SOURCE span of a wide move? A 64-byte
                                         * copy of a partly written transform is unknown. */
                                        if (!printed_one && tr[cur.first].key == 0 &&
                                            tr[cur.first].width > 4 && ops.size() >= 2 &&
                                            ops[0].region == 2) {
                                            const uint32_t lo = ops[0].offset;
                                            const uint32_t hi = lo + tr[cur.first].width;
                                            for (size_t i = 0; i < cur.first; ++i) {
                                                const uint32_t w = tr[i].width ? tr[i].width : 4u;
                                                if (tr[i].slot + w <= lo || tr[i].slot >= hi) continue;
                                                char cb3[200];
                                                std::snprintf(cb3, sizeof cb3,
                                                    "        src+0x%X w%u %s by rec 0x%X %s\n",
                                                    tr[i].slot - lo, tr[i].width,
                                                    tr[i].known ? "known" : "UNKNOWN",
                                                    tr[i].record_offset, nm_of(tr[i].key).c_str());
                                                chain += cb3;
                                            }
                                            for (const auto& kv : seeded.slot_seed)
                                                if (kv.first >= lo && kv.first < hi) {
                                                    char cb3[96];
                                                    std::snprintf(cb3, sizeof cb3, "        src+0x%X SEEDED w%zu\n",
                                                                  kv.first - lo, kv.second.bytes.size());
                                                    chain += cb3;
                                                }
                                        }
                                        if (!printed_one) {
                                            char cb2[200];
                                            int at2 = std::snprintf(cb2, sizeof cb2, "      culprit rec 0x%X operands:", tr[cur.first].record_offset);
                                            for (const auto& op : ops)
                                                at2 += std::snprintf(cb2 + at2, sizeof cb2 - (size_t)at2, " r%u@0x%X", op.region, op.offset);
                                            chain += std::string(cb2) + "\n";
                                        }
                                        ray_culprits[cb] += 1;
                                        if (!printed_one) chain += std::string("      ") + cb + "\n";
                                    }
                                }
                                if (!printed_one && !chain.empty()) {
                                    std::printf("RUNTIME CHAIN for first raycast in %s:\n%s",
                                                std::strrchr(a.name, '/') + 1, chain.c_str());
                                    printed_one = true;
                                }
                            }
                        }
                        /* REJECTED OR NEVER REACHED? If a queue key is in
                         * unresolved_keys the VM refused the CALL (a shape or width
                         * mismatch) before invoking it. If it is absent and was not
                         * invoked, control flow never got there. Also print each queue
                         * record's operand regions so a shape mismatch can be read. */
                        for (const auto& rec : graph.records) {
                            const auto nm = name_of.find(rec.operator_key);
                            if (nm == name_of.end() ||
                                nm->second.compare(0, 26, "__queueAsyncPhysicsRayQuer") != 0)
                                continue;
                            const bool unres = std::find(sr.unresolved_keys.begin(),
                                                         sr.unresolved_keys.end(),
                                                         rec.operator_key) !=
                                               sr.unresolved_keys.end();
                            char qb[200];
                            int at = std::snprintf(qb, sizeof qb, "%s  %s  regions:",
                                                   std::strrchr(a.name, '/') + 1,
                                                   unres ? "REJECTED by the VM"
                                                         : "not rejected");
                            for (const auto& op : rec.operands)
                                at += std::snprintf(qb + at, sizeof qb - (size_t)at,
                                                    " r%u", op.region);
                            queue_reach[qb] += 1;
                        }
                        char tb2[160];
                        std::snprintf(tb2, sizeof tb2,
                                      "%s  termination %d  steps %u  guessed_branches %u",
                                      std::strrchr(a.name, '/') + 1, (int)sr.termination,
                                      sr.steps, sr.guessed_branches);
                        queue_reach[tb2] += 1;
                        /* What is still unresolved INSIDE the suspension graphs - the
                         * operators whose unknown outputs make branch conditions
                         * unknown, so the VM guesses them, and the guesses lead to the
                         * early Return before the queue nodes. */
                        /* CAUSES ONLY, with their real record shapes. A key nothing can
                         * describe is a cause; a key that describes fine but refused is a
                         * symptom of something upstream. Target the causes. */
                        for (uint32_t k : sr.unresolved_keys) {
                            bf6::expression::OperatorSignature sig;
                            if (both.describe(k, sig)) continue;
                            for (const auto& rec : graph.records) {
                                if (rec.operator_key != k) continue;
                                const auto cn = name_of.find(k);
                                char cb[200];
                                int at = std::snprintf(cb, sizeof cb, "0x%08X %-34.34s %zu ops:",
                                    k, cn == name_of.end() ? "" : cn->second.c_str(),
                                    rec.operands.size());
                                for (const auto& op : rec.operands)
                                    at += std::snprintf(cb + at, sizeof cb - (size_t)at, " r%u",
                                                        op.region);
                                susp_causes[cb] += 1;
                            }
                        }
                        for (uint32_t k : sr.unresolved_keys) {
                            const auto nm = name_of.find(k);
                            char ub[128];
                            if (nm != name_of.end())
                                std::snprintf(ub, sizeof ub, "%s", nm->second.c_str());
                            else
                                std::snprintf(ub, sizeof ub, "key 0x%08X", k);
                            susp_unresolved[ub] += 1;
                        }
                        seeded_points_total += seeded_points;
                        seeded_xforms_total += seeded_xforms;
                        seeded_attempts += physics.attempts() - a0;
                        seeded_unknown += physics.unknown_ends() - u0;
                        seeded_rays += physics.queries() - q0;
                        seeded_hits += physics.hits() - h0;
                    }
                }
                tick_rays += physics.queries() - before_q;
                tick_writes += state.writes();
                for (uint32_t p : state.unseeded_reads()) suspension_wants[p] += 1;
                suspension_graphs.insert(a.name);
                /* WHAT THE GRAPH EXPECTS TO BE HANDED. evaluate() takes an
                 * `arguments` vector and this test passes {} - so if the header
                 * declares external bindings or instance buffers, the graph is being
                 * starved of its inputs before any host gets involved. */
                /* IS THERE A SLOT IMAGE ON DISK AT ALL?
                 *
                 * The instance image sits at image_at for instance_header_size bytes.
                 * If the slot file is stored the same way it should follow, 16-byte
                 * aligned. Print the arithmetic rather than assume it: payload size,
                 * where the instance image ends, slot_file_size, and whether that
                 * much still fits inside the asset. */
                const size_t image_end =
                    graph.image_at + graph.header.instance_header_size;
                const size_t aligned = (image_end + 15u) & ~(size_t)15u;
                char hb[400];
                std::snprintf(hb, sizeof hb,
                              "ext_bind %u  inst_bufs %u  inst_groups %zu  "
                              "slot_groups %zu | payload %lld  image_at %zu  "
                              "inst_size %u  image_end %zu  slot_file_size %u  "
                              "fits_after_image %s",
                              graph.header.external_bindings,
                              (unsigned)graph.header.instance_buffer_count,
                              graph.instance_values.size(),
                              graph.slot_values.size(),
                              (long long)bytes, graph.image_at,
                              graph.header.instance_header_size, image_end,
                              graph.header.slot_file_size,
                              (aligned + graph.header.slot_file_size <= (size_t)bytes)
                                  ? "YES" : "no");
                /* THE CONSTANT POOL'S SIZE, against the offsets PUSH reads.
                 *
                 * Every PUSH operand is region 0, and materialize() returns region 0
                 * as KNOWN unless the read is out of bounds. PUSH's operands arrive
                 * unknown anyway, so the bounds check must be failing. Its offsets
                 * run 1072..1432, so the question is simply whether the pool is that
                 * big. */
                std::snprintf(hb + std::strlen(hb), sizeof hb - std::strlen(hb),
                              "  | const_pool %zu  const_base %zu  region_base %zu",
                              graph.constant_pool.size(), graph.constant_base,
                              graph.region_base);
                header_shapes[hb] += 1;

                /* WHAT REGION ARE PUSH'S OPERANDS?
                 *
                 * materialize() makes this decisive. Region 0 is the constant pool
                 * and is always known; region 1 is the instance image, known unless
                 * the offset is in an instance_values group (and instance_groups is
                 * 0 here, so nothing is withheld); region 3+ is an inline immediate,
                 * known at width <= 4; region 2 is a SLOT, unknown until written.
                 * PUSH is declared with four 4-byte inputs, so an unknown operand
                 * can only be region 2 - or an out-of-bounds read in another region.
                 * This prints the regions so the answer is read, not deduced. */
                /* ONE HOP UP THE RAY CHAIN: who makes the TRANSFORM?
                 *
                 * The ray origin is MultiplyFloat3LinearTransformFloat3(point, transform).
                 * Every source of the transform outside the graph is ruled out, and the
                 * Prepare prologue is not in these graphs at all. So name the producer of
                 * each of that multiply's operands - INCLUDING unnamed keys, printed in
                 * hex, because the prime suspect is one of the evaluation-context
                 * accessors this host stubs with a 4-byte handle. If one of those really
                 * returns a 64-byte transform, the multiply reads a mostly-unknown slot.
                 */
                {
                    std::map<uint32_t, std::string> who;
                    for (const auto& rec : graph.records) {
                        const auto nm = name_of.find(rec.operator_key);
                        const std::string opname = nm != name_of.end()
                            ? nm->second : std::string();
                        if (opname == "MultiplyFloat3LinearTransformFloat3" ||
                            opname == "MultiplyLinearTransformLinearTransformLinearTransform") {
                            for (size_t i = 0; i < rec.operands.size(); ++i) {
                                const auto w = who.find(rec.operands[i].offset);
                                char lb[176];
                                std::snprintf(lb, sizeof lb,
                                              "%.40s  operand %zu of %zu  r%u  <- %s",
                                              opname.c_str(), i, rec.operands.size(),
                                              rec.operands[i].region,
                                              w == who.end() ? "(unwritten)"
                                                             : w->second.c_str());
                                lt_inputs[lb] += 1;
                            }
                        }
                        std::string pn = opname;
                        if (!rec.operator_key) {
                            /* A MOVE copies into a slot rather than computing it, and the
                             * earlier trace skipped them - which is how both inputs of
                             * the ray-origin multiply read "(unwritten)". Name the move
                             * by its kind and where it copies FROM. */
                            if (rec.operands.size() < 2) continue;
                            const auto& src = rec.operands.front();
                            char mv[64];
                            std::snprintf(mv, sizeof mv, "MOVE kind 0x%02X from r%u@%u",
                                          (unsigned)rec.kind, src.region, src.offset);
                            pn = mv;
                        } else if (pn.empty()) {
                            char hx[24];
                            std::snprintf(hx, sizeof hx, "key 0x%08X", rec.operator_key);
                            pn = hx;
                        }
                        for (auto it = rec.operands.rbegin(); it != rec.operands.rend(); ++it)
                            if (it->region == 2) { who[it->offset] = pn; break; }
                    }
                }

                /* WRITTEN LATER, OR WRITTEN NOWHERE?
                 *
                 * The ray-origin multiply reads its wheel POINT from a slot that no
                 * record writes BEFORE it. Two readings, two different fixes: written
                 * LATER in record order (a loop-carried value - four wheels iterated)
                 * or written NOWHERE in the graph (a parameter a caller hands in). So
                 * collect every slot any record writes anywhere, and classify the
                 * point operand of every ray-origin multiply.
                 */
                {
                    std::set<uint32_t> ever;
                    for (const auto& rec : graph.records)
                        for (auto it = rec.operands.rbegin(); it != rec.operands.rend(); ++it)
                            if (it->region == 2) { ever.insert(it->offset); break; }
                    std::set<uint32_t> before;
                    for (const auto& rec : graph.records) {
                        const auto nm = name_of.find(rec.operator_key);
                        if (nm != name_of.end() &&
                            nm->second == "MultiplyFloat3LinearTransformFloat3" &&
                            !rec.operands.empty()) {
                            const auto& pt = rec.operands[0];
                            const char* verdict =
                                pt.region != 2 ? "not a slot"
                                : before.count(pt.offset) ? "written earlier"
                                : ever.count(pt.offset)   ? "written LATER (loop-carried?)"
                                                          : "written NOWHERE in this graph";
                            ray_point_origin[verdict] += 1;
                        }
                        for (auto it = rec.operands.rbegin(); it != rec.operands.rend(); ++it)
                            if (it->region == 2) { before.insert(it->offset); break; }
                    }
                }

                /* WHAT IS A WRITE'S SINGLE OPERAND: a destination, or a value?
                 *
                 * The four write operators have arity 1, which cannot carry both. If
                 * the operand is produced by the field-address operator it is a
                 * DESTINATION and the value comes from elsewhere; if it is produced by
                 * arithmetic it is a VALUE. Same producer trace that settled the ray
                 * endpoints, and it is what has to be known before a write can store
                 * anything - which the tick test in turn depends on. */
                {
                    std::map<uint32_t, std::string> wrote2;
                    for (const auto& rec : graph.records) {
                        const bool is_write = rec.operator_key == 0x47BE3D90u ||
                                              rec.operator_key == 0xA04FF621u ||
                                              rec.operator_key == 0x83B013E3u ||
                                              rec.operator_key == 0x6575DE53u;
                        if (is_write) {
                            for (size_t i = 0; i < rec.operands.size(); ++i) {
                                const auto w = wrote2.find(rec.operands[i].offset);
                                char wb[144];
                                std::snprintf(wb, sizeof wb,
                                              "write %08X  operand %zu of %zu  r%u  <- %s",
                                              rec.operator_key, i,
                                              rec.operands.size(),
                                              rec.operands[i].region,
                                              w == wrote2.end() ? "(unwritten)"
                                                                : w->second.c_str());
                                write_operands[wb] += 1;
                            }
                        }
                        std::string pn;
                        if (rec.operator_key == 0x8B7CF7C9u) pn = "FIELD_ADDRESS";
                        else {
                            const auto nm = name_of.find(rec.operator_key);
                            if (nm != name_of.end()) pn = nm->second;
                        }
                        if (!pn.empty()) {
                            for (auto it = rec.operands.rbegin();
                                 it != rec.operands.rend(); ++it)
                                if (it->region == 2) { wrote2[it->offset] = pn; break; }
                        }
                    }
                }

                for (const auto& rec : graph.records) {
                    if (rec.operator_key != 0x30FAAAB6u) continue;
                    char pb[128];
                    int at = std::snprintf(pb, sizeof pb, "PUSH operands:");
                    for (const auto& op : rec.operands)
                        at += std::snprintf(pb + at, sizeof pb - (size_t)at,
                                            " r%u@%u", op.region, op.offset);
                    push_regions[pb] += 1;
                }
                for (const auto& ps : state.pushes_seen()) {
                    char buf[128];
                    std::snprintf(buf, sizeof buf,
                                  "%08X %08X %08X %08X   known %c%c%c%c",
                                  ps.arg[0], ps.arg[1], ps.arg[2], ps.arg[3],
                                  (ps.known_mask & 1) ? 'y' : 'n',
                                  (ps.known_mask & 2) ? 'y' : 'n',
                                  (ps.known_mask & 4) ? 'y' : 'n',
                                  (ps.known_mask & 8) ? 'y' : 'n');
                    push_shapes[buf] += 1;
                }
                for (const auto& rd : state.odd_reads()) {
                    char buf[128];
                    std::snprintf(buf, sizeof buf,
                                  "read %08X  depth %d  frame %08X %08X %08X %08X",
                                  rd.addr, rd.depth, rd.bound[0], rd.bound[1],
                                  rd.bound[2], rd.bound[3]);
                    read_shapes[buf] += 1;
                }
            }
            /* CAUSE OR SYMPTOM?
             *
             * A key lands in unresolved_keys for two very different reasons. Either
             * the host could not DESCRIBE it - a real, missing operator - or it was
             * described fine and `invoke` refused, which NamedBuiltins does as soon
             * as any input is `!known`. The second is a DOWNSTREAM effect: the
             * operator is implemented and merely fed by something that already
             * failed. Counting them together is what made "3,156 blocked uses on
             * named operators" look like 3,156 operators to write. */
            for (uint32_t k : r.unresolved_keys) {
                still_blocked[k] += 1;
                bf6::expression::OperatorSignature sig;
                if (both.describe(k, sig)) described_but_failed[k] += 1;
                else not_described[k] += 1;
            }
        }
    }
    bf6_ray_scene_free(scene);
    bf6_close(ctx);

    std::printf("WHEEL RAYS: %d queued, %d hit the ground plane\n",
                ray_queries, ray_hits);
    std::printf("SUSPENSION GRAPH HEADERS (what they expect to be handed):\n");
    for (const auto& h : header_shapes)
        std::printf("   %s   x%u\n", h.first.c_str(), h.second);

    std::printf("THE WHEEL POINT FED TO THE RAY-ORIGIN MULTIPLY:\n");
    for (const auto& kv : ray_point_origin)
        std::printf("   %-34s x%u\n", kv.first.c_str(), kv.second);
    std::printf("GENERIC READERS: use vs operand values (top 30):\n");
    {
        std::vector<std::pair<std::string, uint32_t>> v(selector_seen.begin(),
                                                       selector_seen.end());
        std::sort(v.begin(), v.end(), [](const std::pair<std::string, uint32_t>& x,
                                         const std::pair<std::string, uint32_t>& y) {
            return x.second > y.second; });
        for (size_t i = 0; i < v.size() && i < 30; ++i)
            std::printf("   %s   x%u\n", v[i].first.c_str(), v[i].second);
        std::printf("   (%zu distinct)\n", v.size());
    }
    std::printf("DECLARED SLOT TYPE OF EACH ACCESSOR CALL'S OUTPUT:\n");
    std::printf("   (datatype ids: 0x04 Float, 0x0A Int32?, 0x01 Bool, 0x29/0x411 Vec3,\n");
    std::printf("    0x417 LinearTransform - see data/expression_datatypes.tsv)\n");
    for (const auto& kv : slot_type_of)
        std::printf("   %s   x%u\n", kv.first.c_str(), kv.second);
    std::printf("WHAT CONSUMES EACH CONTEXT ACCESSOR'S OUTPUT (all vehicle graphs):\n");
    {
        std::vector<std::pair<std::string, uint32_t>> v(accessor_use.begin(), accessor_use.end());
        std::sort(v.begin(), v.end());
        for (const auto& kv : v)
            std::printf("   %s   x%u\n", kv.first.c_str(), kv.second);
    }
    std::printf("WHO PRODUCES THE TRANSFORM THE RAY IS MULTIPLIED BY (suspension graphs):\n");
    {
        std::vector<std::pair<std::string, uint32_t>> v(lt_inputs.begin(), lt_inputs.end());
        std::sort(v.begin(), v.end(), [](const std::pair<std::string, uint32_t>& x,
                                         const std::pair<std::string, uint32_t>& y) {
            return x.second > y.second; });
        for (size_t i = 0; i < v.size() && i < 16; ++i)
            std::printf("   %s   x%u\n", v[i].first.c_str(), v[i].second);
    }
    std::printf("WHICH GRAPH KINDS CARRY A Prepare PROLOGUE:\n");
    for (const auto& kv : prepare_kinds)
        std::printf("   %-32s %u graph(s)\n", kv.first.c_str(), kv.second);
    std::printf("THE __Dice...Prepare*Ex PROLOGUES, in and out:\n");
    {
        std::vector<std::pair<std::string, uint32_t>> v(prepare_io.begin(),
                                                       prepare_io.end());
        std::sort(v.begin(), v.end(), [](const std::pair<std::string, uint32_t>& x,
                                         const std::pair<std::string, uint32_t>& y) {
            return x.second > y.second; });
        for (size_t i = 0; i < v.size() && i < 22; ++i)
            std::printf("   %s   x%u\n", v[i].first.c_str(), v[i].second);
        std::printf("   (%zu distinct)\n", v.size());
    }

    std::printf("MOVE KIND vs what produces / consumes it (all vehicle graphs):\n");
    for (const auto& kv : move_kinds)
        std::printf("   %-50s x%u\n", kv.first.c_str(), kv.second);
    std::printf("CHANNEL BINDINGS: %d graphs carry them, %d bindings\n",
                graphs_with_bindings, bindings_total);
    for (const auto& kv : chan_ops)
        std::printf("CHANOP %-90s x%u\n", kv.first.c_str(), kv.second);
    std::printf("ENGINE-FILLED SLOTS (read by a record, written by none), suspension graphs:\n");
    for (const auto& kv : engine_filled)
        std::printf("   %-100s x%u\n", kv.first.c_str(), kv.second);
    std::printf("RUNTIME: WHAT FIRST MAKES THE RAYCAST INPUTS UNKNOWN (all suspension raycasts):\n");
    for (const auto& kv : ray_culprits)
        std::printf("   %-110s x%u\n", kv.first.c_str(), kv.second);
    std::printf("ROOM IN EACH TYPED READER'S OUTPUT SLOT (all vehicle graphs):\n");
    for (const auto& kv : slot_room)
        std::printf("   %-34s x%u\n", kv.first.c_str(), kv.second);
    std::printf("0x532B3BA9 FIELD CONSTANTS:\n");
    for (const auto& kv : field_ids)
        std::printf("   %s   x%u\n", kv.first.c_str(), kv.second);
    std::printf("SYNCHRONOUS RAYCAST 0x040F4924 INPUTS, by producer:\n");
    for (const auto& kv : physq_inputs)
        std::printf("   %s   x%u\n", kv.first.c_str(), kv.second);
    std::printf("CAUSES INSIDE THE SUSPENSION GRAPHS (nothing describes them):\n");
    for (const auto& kv : susp_causes)
        std::printf("   %s   x%u\n", kv.first.c_str(), kv.second);
    std::printf("ROOT KEYS, AS THE RECORDS CARRY THEM:\n");
    for (const auto& kv : root_shapes)
        std::printf("   %-52s x%u\n", kv.first.c_str(), kv.second);
    std::printf("STILL UNRESOLVED INSIDE THE 8 SEEDED SUSPENSION GRAPHS (graphs affected):\n");
    {
        std::vector<std::pair<std::string, uint32_t>> v(susp_unresolved.begin(),
                                                       susp_unresolved.end());
        std::sort(v.begin(), v.end(), [](const std::pair<std::string, uint32_t>& x,
                                         const std::pair<std::string, uint32_t>& y) {
            return x.second > y.second; });
        for (const auto& kv : v)
            std::printf("   %-58s %u\n", kv.first.c_str(), kv.second);
    }
    std::printf("QUEUE NODES IN THE SEEDED SUSPENSION GRAPHS: rejected, or never reached?\n");
    for (const auto& kv : queue_reach)
        std::printf("   %s   x%u\n", kv.first.c_str(), kv.second);
    std::printf("GRAPHS THAT CONTAIN A RAY QUEUE NODE: %zu\n", queue_graphs.size());
    for (const auto& kv : queue_graphs)
        std::printf("   %2d  %s\n", kv.second, kv.first.c_str());
    std::printf("SEEDED PASS (wheel point + transform supplied the way the engine would):\n");
    std::printf("   slots seeded: %d wheel points, %d transforms\n",
                seeded_points_total, seeded_xforms_total);
    std::printf("   queue node invoked: %d   refused on an unknown endpoint: %d\n",
                seeded_attempts, seeded_unknown);
    std::printf("   RAYS QUEUED: %d   HIT THE GROUND: %d\n", seeded_rays, seeded_hits);
    std::printf("TICK TEST: 8 extra passes per suspension graph, same StateHost\n");
    std::printf("   state writes that landed: %d\n", tick_writes);
    std::printf("   rays queued on passes 2..9: %d\n", tick_rays);
    if (tick_first_ray.empty())
        std::printf("   no graph ever queued a ray -> the tick hypothesis is REFUTED\n");
    else
        for (const auto& kv : tick_first_ray)
            std::printf("   first ray on pass %d: %u graph(s)\n", kv.first, kv.second);

    std::printf("WRITE OPERANDS: destination or value?\n");
    {
        std::vector<std::pair<std::string, uint32_t>> v(write_operands.begin(),
                                                       write_operands.end());
        std::sort(v.begin(), v.end(), [](const std::pair<std::string, uint32_t>& x,
                                         const std::pair<std::string, uint32_t>& y) {
            return x.second > y.second; });
        for (size_t i = 0; i < v.size() && i < 12; ++i)
            std::printf("   %s   x%u\n", v[i].first.c_str(), v[i].second);
        std::printf("   (%zu distinct)\n", v.size());
    }

    std::printf("PUSH OPERAND REGIONS (r0 const pool, r1 instance, r2 slot, r3+ immediate):\n");
    {
        std::vector<std::pair<std::string, uint32_t>> v(push_regions.begin(),
                                                       push_regions.end());
        std::sort(v.begin(), v.end(), [](const std::pair<std::string, uint32_t>& x,
                                         const std::pair<std::string, uint32_t>& y) {
            return x.second > y.second; });
        for (size_t i = 0; i < v.size() && i < 10; ++i)
            std::printf("   %s   x%u\n", v[i].first.c_str(), v[i].second);
        std::printf("   (%zu distinct)\n", v.size());
    }

    std::printf("PUSH OPERANDS the suspension graphs actually pass (top 8):\n");
    std::printf("   NOTE 0xFFFFFFFF here is THIS HOST'S OWN fallback for an operand\n");
    std::printf("   that arrived !known, not a value the graph supplied.\n");
    {
        std::vector<std::pair<std::string, uint32_t>> v(push_shapes.begin(), push_shapes.end());
        std::sort(v.begin(), v.end(), [](const std::pair<std::string, uint32_t>& x,
                                         const std::pair<std::string, uint32_t>& y) {
            return x.second > y.second; });
        for (size_t i = 0; i < v.size() && i < 8; ++i)
            std::printf("   %s   x%u\n", v[i].first.c_str(), v[i].second);
        std::printf("   (%zu distinct shapes)\n", v.size());
    }
    std::printf("FRAME-RELATIVE READS, with the frame that was live (top 10):\n");
    {
        std::vector<std::pair<std::string, uint32_t>> v(read_shapes.begin(), read_shapes.end());
        std::sort(v.begin(), v.end(), [](const std::pair<std::string, uint32_t>& x,
                                         const std::pair<std::string, uint32_t>& y) {
            return x.second > y.second; });
        for (size_t i = 0; i < v.size() && i < 10; ++i)
            std::printf("   %s   x%u\n", v[i].first.c_str(), v[i].second);
        std::printf("   (%zu distinct)\n", v.size());
    }
    std::printf("SUSPENSION INPUTS: %zu graph(s), %zu distinct state cells read "
                "but never seeded\n",
                suspension_graphs.size(), suspension_wants.size());
    {
        /* Resolve what can be resolved from the names this test knows, which is the
         * flyer60 suspension descriptor set. A hash with no name here is simply one
         * whose DebugName lives in an asset this test does not read. */
        static const char* known[] = {
            "Driveshaft Rotation", "Rotation", "OffsetLastFrame", "SleepUpdateCount",
            "Wheel_Front_Left", "Wheel_Front_Right", "Wheel_Rear_Left",
            "Wheel_Rear_Right", "WheelRaycastHandle", "WheelRaycastQueryHandle",
            "SleepUpdate", "SpringCompression", "HasContact", "Wheel Status",
            "Wheel Angular Velocity", "PreviousNormalisedLoad", "NumContacts",
            "Wheel AverageSlipRatioSum", "Wheel AverageSlipAngleSum",
        };
        std::map<uint32_t, const char*> byhash;
        for (const char* n : known)
            byhash[bf6::expression::StateHost::path_of(n)] = n;
        std::vector<std::pair<uint32_t, uint32_t>> w(suspension_wants.begin(),
                                                     suspension_wants.end());
        std::sort(w.begin(), w.end(),
                  [](const std::pair<uint32_t, uint32_t>& x,
                     const std::pair<uint32_t, uint32_t>& y) {
                      return x.second > y.second; });
        int named = 0;
        for (size_t i = 0; i < w.size() && i < 16; ++i) {
            const auto it = byhash.find(w[i].first);
            if (it != byhash.end()) ++named;
            std::printf("   0x%08X read x%-4u %s\n", w[i].first, w[i].second,
                        it == byhash.end() ? "(name not in this test's set)"
                                           : it->second);
        }
        std::printf("   %d of the top %zu resolve to a known suspension state name\n",
                    named, w.size() < 16 ? w.size() : (size_t)16);
    }
    std::printf("vehicle DiceExpression graphs: %zu found, %zu parsed\n",
                tested.graphs, tested.parsed);
    std::printf("  CONTROL  (NamedBuiltins only)      %zu sound, %zu unresolved uses\n",
                control.sound, control.unresolved_uses);
    std::printf("SCORE %zu %zu %zu %zu %d %d\n", control.sound, control.unresolved_uses,
                tested.sound, tested.unresolved_uses, ray_queries, ray_hits);
    std::printf("  TEST     (+ StateHost)             %zu sound, %zu unresolved uses\n",
                tested.sound, tested.unresolved_uses);
    std::printf("\nstate/context operators the host served (top 12):\n");
    std::vector<std::pair<uint32_t, uint32_t>> s(served.begin(), served.end());
    std::sort(s.begin(), s.end(), [](auto& x, auto& y) { return x.second > y.second; });
    for (size_t i = 0; i < s.size() && i < 12; ++i)
        std::printf("   0x%08X  %u\n", s[i].first, s[i].second);
    std::printf("\nstill blocked, most common (top 12):\n");
    std::vector<std::pair<uint32_t, uint32_t>> b(still_blocked.begin(),
                                                 still_blocked.end());
    std::sort(b.begin(), b.end(), [](auto& x, auto& y) { return x.second > y.second; });
    size_t named_blocked = 0, unnamed_blocked = 0;
    for (const auto& kv : b) {
        if (name_of.count(kv.first)) named_blocked += kv.second;
        else unnamed_blocked += kv.second;
    }
    for (size_t i = 0; i < b.size() && i < 20; ++i) {
        const auto it = name_of.find(b[i].first);
        std::printf("   0x%08X  %5u graph(s)  %s\n", b[i].first, b[i].second,
                    it == name_of.end() ? "(no name resolved)" : it->second.c_str());
    }
    std::printf("\n   blocked keys WITH a resolved name (pure ops, cheap): %zu\n",
                named_blocked);
    std::printf("   blocked keys with no name (state/reflected/etc):      %zu\n",
                unnamed_blocked);

    std::printf("\nOPERAND COUNT PER NAMED OPERATOR, as the records actually carry it:\n");
    size_t consistent = 0, varies = 0;
    for (const auto& kv : operands_of) {
        if (kv.second.size() == 1) ++consistent; else ++varies;
    }
    std::printf("   %zu names carry ONE operand count; %zu vary between records\n",
                consistent, varies);
    size_t shown = 0;
    for (const auto& kv : operands_of) {
        if (shown++ >= 18) break;
        std::printf("   %-52s", kv.first.c_str());
        for (const auto& oc : kv.second)
            std::printf(" %zu operands x%u", oc.first, oc.second);
        std::printf("\n");
    }

    std::printf("\nPHYSICS QUERY NODES: what produced each input\n");
    for (const auto& q : query_inputs) {
        std::printf("   %s\n", q.first.c_str());
        for (const auto& idx : q.second) {
            std::printf("      input %zu:", idx.first);
            size_t n = 0;
            for (const auto& p2 : idx.second) {
                if (n++ >= 3) { std::printf("  ..."); break; }
                std::printf("  %s x%u", p2.first.c_str(), p2.second);
            }
            std::printf("\n");
        }
    }

    std::printf("\nCAUSE vs SYMPTOM for every blocked key:\n");
    {
        uint32_t d_named = 0, d_unnamed = 0, n_named = 0, n_unnamed = 0;
        for (const auto& kv : described_but_failed)
            (name_of.count(kv.first) ? d_named : d_unnamed) += kv.second;
        for (const auto& kv : not_described)
            (name_of.count(kv.first) ? n_named : n_unnamed) += kv.second;
        std::printf("   described, invoke refused (SYMPTOM: fed an unknown input)\n");
        std::printf("      named   %6u\n      unnamed %6u\n", d_named, d_unnamed);
        std::printf("   never described (CAUSE: a real missing operator)\n");
        std::printf("      named   %6u\n      unnamed %6u\n", n_named, n_unnamed);
        std::printf("\n   named operators genuinely MISSING, most blocking first:\n");
        std::vector<std::pair<uint32_t, uint32_t>> miss;
        for (const auto& kv : not_described)
            if (name_of.count(kv.first)) miss.push_back(kv);
        std::sort(miss.begin(), miss.end(),
                  [](const std::pair<uint32_t, uint32_t>& x,
                     const std::pair<uint32_t, uint32_t>& y) {
                      return x.second > y.second; });
        for (size_t i = 0; i < miss.size() && i < 40; ++i) {
            const std::string& n = name_of[miss[i].first];
            /* The measured operand count, so the implementation takes its arity
             * from the records rather than from reading the name. */
            std::string ops = "?";
            const auto oc = operands_of.find(n);
            if (oc != operands_of.end() && !oc->second.empty()) {
                ops.clear();
                for (const auto& e : oc->second)
                    ops += std::to_string(e.first) + " ";
            }
            std::printf("      %-54s %4u graph(s)  operands: %s\n",
                        n.c_str(), miss[i].second, ops.c_str());
        }
        std::printf("      ---- %zu distinct named operators are missing\n",
                    miss.size());
    }

    std::printf("\nOPERAND REGIONS, for the names that block despite being implemented:\n");
    std::printf("   region 2 is a slot; the VM's output is the LAST region-2 operand,\n");
    std::printf("   so a record with none cannot satisfy a signature declaring an output.\n");
    const char* watch[] = {"AddFloat", "SubtractFloat", "MultiplyFloatFloatFloat",
                           "DivideFloatFloatFloat", "And", "ClampFloat",
                           "AbsoluteFloat", "GreaterThanFloat"};
    std::printf("   %-26s %7s %9s %12s  %s\n", "name", "records", "with slot",
                "without", "region shapes seen");
    for (const char* w : watch) {
        const auto it = regions_of.find(w);
        if (it == regions_of.end()) continue;
        const RegionStat& rs = it->second;
        std::printf("   %-26s %7u %9u %12u  ", w, rs.records, rs.with_slot,
                    rs.without_slot);
        size_t n = 0;
        for (const auto& sh : rs.shapes) {
            if (n++ >= 4) { std::printf("..."); break; }
            std::printf("%s x%u  ", sh.first.c_str(), sh.second);
        }
        std::printf("\n");
    }
    uint32_t tot = 0, noslot = 0;
    for (const auto& kv : regions_of) {
        tot += kv.second.records;
        noslot += kv.second.without_slot;
    }
    std::printf("   ---- across every named operator: %u records, %u with NO slot (%.1f%%)\n",
                tot, noslot, tot ? 100.0 * noslot / tot : 0.0);

    /* The host must help and must not hurt: more graphs sound, fewer unresolved
     * uses. Anything else is a regression however good the absolute number looks. */
    const bool helped = tested.sound >= control.sound &&
                        tested.unresolved_uses <= control.unresolved_uses;
    std::printf("\n%s\n", helped ? "OK" : "REGRESSION");
    return (tested.parsed > 0 && helped) ? 0 : 1;
}
