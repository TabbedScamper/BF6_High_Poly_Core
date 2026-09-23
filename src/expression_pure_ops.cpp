#include "expression_pure_ops.h"

#include <immintrin.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace bf6 { namespace expression {

namespace {

/* WIDTHS FROM THE ENGINE'S OWN DATATYPE TABLE, not from counting components.
 *
 * `data/expression_datatypes.tsv`, read out of the executable, gives
 * `Vec3 fb.DataTypes size 0x10` and `LinearTransform fb.DataTypes size 0x40`. So a
 * Float3 is SIXTEEN bytes, not twelve: three floats and four bytes of padding, at
 * align 16. An earlier version of this file used 12 and the self-check still passed,
 * because the check built 12-byte values to match - self-consistent and wrong
 * against the engine, which is exactly the error a self-check cannot catch.
 *
 * The three floats sit at 0, 4, 8 and bytes 12-15 are padding, which is why the
 * helpers below copy 12 bytes into and out of a 16-byte value rather than 16. */
const uint32_t kF = 4;   /* Float, Int, UInt   */
const uint32_t kB = 1;   /* Bool               */
const uint32_t kV = 16;  /* Vec3 / Float3      */
const uint32_t kT = 64;  /* LinearTransform    */

struct Spec {
    const char* name;
    uint32_t in_count;
    uint32_t in_width[5];
    uint32_t out_width;
};

/* Arity is the MEASURED operand count minus one. The comment on each line is that
 * measured count, so a future reader can check the table against the data rather
 * than against the spelling. */
const Spec kSpecs[] = {
    /* scalar float */
    {"SubtractFloat",            2, {kF, kF}, kF},      /* 3 operands */
    {"NegateFloat",              1, {kF},     kF},      /* 2 */
    {"AbsoluteFloat",            1, {kF},     kF},      /* 2: name-registered builtin, sign bit cleared */
    {"MagnitudeSquaredFloat3",   1, {kV},     kF},
    {"MagnitudeFloat2",          1, {kF * 2}, kF},
    {"ToUInt32Float",            1, {kF},     kF},
    {"ToInt32UInt",              1, {kF},     kF},
    {"MinFloat",                 2, {kF, kF}, kF},      /* 3 */
    {"MaxFloat",                 2, {kF, kF}, kF},      /* 3 */
    {"ClampFloat",               3, {kF, kF, kF}, kF},  /* 4 */
    {"SignFloat",                1, {kF},     kF},      /* 2 */
    {"EqualsFloat",              2, {kF, kF}, kB},      /* 3 */
    {"RadiansToDegreesFloat",    1, {kF},     kF},      /* 2 */
    {"DegreesToRadiansFloat",    1, {kF},     kF},      /* 2 */
    {"NormalizeAngleZeroToTwoPi",1, {kF},     kF},      /* 2 */
    {"Atan2Float",               2, {kF, kF}, kF},      /* 3 */
    {"ArcSineFloat",             1, {kF},     kF},      /* 2 */
    {"ArcCosineFloat",           1, {kF},     kF},      /* 2 */
    {"ATanFloat",                1, {kF},     kF},      /* 2 */
    {"CosineFloat",              1, {kF},     kF},      /* 2 */
    {"RangeChange",              5, {kF, kF, kF, kF, kF}, kF}, /* 6 */

    /* int and conversions */
    {"DivideIntIntInt",          2, {kF, kF}, kF},      /* 3 */
    {"ModuloInt",                2, {kF, kF}, kF},      /* 3 */
    {"ToUInt32Int",              1, {kF},     kF},      /* 2 */
    {"ToFloatInt",               1, {kF},     kF},      /* 2 */
    /* one input each (atlas shape s: ToInt32Bool 277, AbsoluteInt 50, SignInt 36 uses) */
    {"ToInt32Bool",              1, {kB},     kF},      /* 2 */
    {"AbsoluteInt",              1, {kF},     kF},      /* 2 */
    {"SignInt",                  1, {kF},     kF},      /* 2 */
    {"ToFloatUInt",              1, {kF},     kF},      /* 2 */
    {"ToFloatBool",              1, {kB},     kF},      /* 2 */
    {"NotEqualsBool",            2, {kB, kB}, kB},      /* 3 */

    /* Float3. The name lists input types in order, result last. */
    {"AddFloat3",                2, {kV, kV}, kV},      /* 3 */
    {"SubtractFloat3",           2, {kV, kV}, kV},      /* 3 */
    {"AbsoluteFloat3",           1, {kV},     kV},      /* 2 */
    {"MagnitudeFloat3",          1, {kV},     kF},      /* 2 */
    {"NormalizeFloat3",          1, {kV},     kV},      /* 2 */
    {"DistanceFloat3",           2, {kV, kV}, kF},      /* 3 */
    /* ANGLE AT A PIVOT, transcribed from 0x14249AF00.
     *
     * Its body was found by the route that works for a named builtin whose key is
     * nowhere in the image: the name literal is referenced by a lazy crc32
     * initializer that returns the operator's descriptor, and that descriptor's
     * first qword is the implementation. It is a fourth registry, invisible to the
     * three this library scans because the key field is still zero on disk.
     *
     * AngleBetweenFloat(a, b, pivot) is the angle AT the pivot: acos of the dot of
     * the two normalised pivot-relative directions, the dot clamped to [-1, 1]
     * first. Three Vec3 in, one float out - the shape of the boat's own record. */
    {"AngleBetweenFloat",        3, {kV, kV, kV}, kF},  /* 4 */
    {"MultiplyFloat3FloatFloat3",2, {kV, kF}, kV},      /* 3 */
    {"MultiplyFloatFloat3Float3",2, {kF, kV}, kV},      /* 3 */
    {"MultiplyFloat3Float3Float3",2,{kV, kV}, kV},      /* 3 */

    /* LinearTransform, 64 bytes, four Vec3 rows at stride 16. */
    {"MultiplyLinearTransformLinearTransformLinearTransform",
                                 2, {kT, kT}, kT},      /* 3 */
    {"AddLinearTransform",       2, {kT, kT}, kT},      /* 3 */
    {"Translation",              1, {kT},     kV},      /* 2 */
    {"InverseLinearTransform",   1, {kT},     kT},      /* 2 */
    {"LinearTransformfromXangle",1, {kF},     kT},      /* 2 */
    {"LinearTransformfromYangle",1, {kF},     kT},      /* 2 */
    {"LinearTransformfromZangle",1, {kF},     kT},      /* 2, same family */

    /* The tail: small, low-risk, and between them another ~180 blocked uses. */
    {"CrossVec3",                2, {kV, kV}, kV},      /* 3 */
    {"DotFloat3",                2, {kV, kV}, kF},      /* 3 */
    {"CosFloat",                 1, {kF},     kF},      /* 2 */
    {"MinInt",                   2, {kF, kF}, kF},      /* 3 */
    {"MaxInt",                   2, {kF, kF}, kF},      /* 3 */
    {"ComplementFloat",          1, {kF},     kF},      /* 2 */
    {"HalfPi",                   0, {},       kF},      /* 1, a constant */
    {"NormalizeAngleMinusPiToPi",1, {kF},     kF},      /* 2 */
    {"ImplicitToInt32Bool",      1, {kB},     kF},      /* 2 */
    {"GreaterThanUInt",          2, {kF, kF}, kB},      /* 3 */
    {"LessThanUInt",             2, {kF, kF}, kB},      /* 3 */
    {"InterpolateFloat",         3, {kF, kF, kF}, kF},  /* 4 */
    {"InterpolateLinearTransform",3,{kT, kT, kF}, kT},  /* 4 */
    /* Transform a Float3 BY a LinearTransform. Two of these exist and the pair is
     * what tells them apart: `RotateFloat3` rotates only, so the one that is spelled
     * as a multiply is the FULL transform and carries the translation. That is an
     * inference from the pairing, not a measurement, and it is the one entry here
     * worth re-checking if a vehicle part ends up offset by a constant. */
    {"MultiplyFloat3LinearTransformFloat3", 2, {kV, kT}, kV},  /* 3 */
    {"RotateFloat3",             2, {kV, kT}, kV},      /* 3 */
    /* The same rotation the other way. A rotation matrix's inverse is its transpose,
     * which is what "inverse rotate" means for an orthonormal basis and all a
     * vehicle's frames are; it is NOT a general matrix inverse and this does not
     * pretend to be one. */
    {"InverseRotate",            2, {kV, kT}, kV},      /* 3 */

    /* The last of the pure tail. Everything still missing after these is a host
     * SERVICE (the __ prefixed ones), not arithmetic. */
    {"Pi",                       0, {},       kF},      /* 1 */
    {"TwoPi",                    0, {},       kF},      /* 1 */
    {"RoundFloatInt",            1, {kF},     kF},      /* 2 */
    {"NotEqualsFloat3",          2, {kV, kV}, kB},      /* 3 */
    {"EqualsFloat3",             2, {kV, kV}, kB},      /* 3 */
    {"DivideFloat3FloatFloat3",  2, {kV, kF}, kV},      /* 3 */

    /* THE FIVE THE AIRCRAFT GRAPHS CALL THAT NOTHING SERVED. The eurocopter's
     * drivetrain graph leaves 71 operators unserved, and the exe's own registry
     * names 43 of them as plain arithmetic - which is NOT the same as unimplemented.
     * NamedBuiltins already serves AddFloat, MultiplyFloatFloatFloat, SinFloat, the
     * comparisons and the boolean trio; they appear in that list because an
     * operator whose INPUT is unknown refuses, and the refusal is counted the same
     * way as a missing one. Only these five had no home at all.
     *
     * Arity is read off the name, whose type list is INPUTS THEN OUTPUT (the
     * existing DivideFloat3FloatFloat3 is (Float3, Float) -> Float3), and the VM
     * checks it: a record whose operand count disagrees is refused and named in the
     * run report, so a wrong line here surfaces as a refusal rather than as a
     * quietly wrong number. ToVec3Float and EnumEqualFunc are deliberately absent:
     * their behaviour cannot be read off the name, and guessing it is how a wrong
     * number gets in. AngleBetweenFloat was in that list until its body was read
     * (see below); it is no longer a guess.
     *
     * Float modulo is left to the hardware, which is what the game's own code does;
     * a guard here would invent a value the game never had. */
    {"ModuloFloat",              2, {kF, kF}, kF},      /* 3 */
    {"CosFloat",                 1, {kF},     kF},      /* 2, next to the existing CosineFloat */
    {"NotEqualsUInt",            2, {kF, kF}, kB},      /* 3 */
    {"BitwiseAndInt",            2, {kF, kF}, kF},      /* 3 */
    {"NormalizeFloat2",          1, {kF * 2}, kF * 2},  /* 2 */
    /* THE ONE OPERATOR HERE READ FROM ITS NAME. AverageFloat3 has no reflected
     * descriptor and no entry in the engine-node table, so there is no body to
     * transcribe; what IS measured is its record in the tank's graph - two inputs
     * and one output, all written by 16-byte moves - and the name says what is done
     * with them. Flagged so that a tank rotating oddly is checked here first. */
    {"AverageFloat3",            2, {kV, kV}, kV},      /* 3 */
};

const Spec* spec_for(const std::string& name) {
    for (const Spec& s : kSpecs)
        if (name == s.name) return &s;
    return nullptr;
}

float f32(const Value& v) {
    float f = 0.f;
    if (v.bytes.size() >= 4) std::memcpy(&f, v.bytes.data(), 4);
    return f;
}

Value put_f32(float f) {
    Value v;
    v.bytes.resize(4);
    std::memcpy(v.bytes.data(), &f, 4);
    v.known = true;
    return v;
}

void vec3(const Value& v, float out[3]) {
    out[0] = out[1] = out[2] = 0.f;
    if (v.bytes.size() >= 12) std::memcpy(out, v.bytes.data(), 12);
}

/* Sixteen bytes wide, twelve of them meaningful. The pad is zeroed rather than left
 * uninitialised so two equal vectors compare equal byte for byte. */
Value put_vec3(const float in[3]) {
    Value v;
    v.bytes.assign(kV, 0);
    std::memcpy(v.bytes.data(), in, 12);
    v.known = true;
    return v;
}

/* A LinearTransform is FOUR Vec3 rows at stride 16: right, up, forward, translation.
 *
 * THE CONVENTION IS MEASURED, not chosen. Walking the flyer60 skeleton in Python with
 * `global = local * parent_global`, rows in that order and the translation added,
 * reproduces the rig's own ModelPose and puts the four wheel locators at
 * (+/-0.725, 0.473, +1.639/-1.523) - which the Godot build then matched to the
 * centimetre. A different row order or a column-major reading does not land there. */
void xform(const Value& v, float m[4][3]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c) m[r][c] = 0.f;
    if (v.bytes.size() < kT) return;
    for (int r = 0; r < 4; ++r)
        std::memcpy(m[r], v.bytes.data() + (size_t)r * 16, 12);
}

Value put_xform(const float m[4][3]) {
    Value v;
    v.bytes.assign(kT, 0);
    for (int r = 0; r < 4; ++r)
        std::memcpy(v.bytes.data() + (size_t)r * 16, m[r], 12);
    v.known = true;
    return v;
}

/* a then b, in the same sense as the skeleton walk: rows 0-2 rotate, row 3 is the
 * translation carried through b's rotation and offset by b's translation. */
void mul_xform(const float a[4][3], const float b[4][3], float out[4][3]) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 3; ++c) {
            float s = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
            if (r == 3) s += b[3][c];
            out[r][c] = s;
        }
    }
}

} // namespace

void PureOps::add(uint32_t key, const std::string& current_exe_name) {
    if (key && !current_exe_name.empty() && spec_for(current_exe_name))
        names_[key] = current_exe_name;
}

const std::string* PureOps::name_for(uint32_t key) const {
    const auto it = names_.find(key);
    return it == names_.end() ? nullptr : &it->second;
}

bool PureOps::describe(uint32_t key, OperatorSignature& out) {
    out = OperatorSignature{};
    const std::string* n = name_for(key);
    if (!n) return false;
    const Spec* s = spec_for(*n);
    if (!s) return false;
    out.input_widths.assign(s->in_width, s->in_width + s->in_count);
    out.output_width = s->out_width;
    return true;
}

bool PureOps::invoke(uint32_t key, const std::vector<Value>& a, Value& out) {
    OperatorSignature signature;
    if (!describe(key, signature) || a.size() != signature.input_widths.size())
        return false;
    const std::string& n = *name_for(key);
    /* Same rule NamedBuiltins uses: an unknown input makes the result unknown, and
     * saying so is better than computing with a zero that was never there.
     *
     * EXCEPT THE PADDING LANE OF A FLOAT3. A Vec3 is stored in 16 bytes and these
     * operators read three components (vec3() below) and write a fresh value. The graph
     * often assembles a Vec3 lane by lane - three 4-byte moves into x, y and z - and
     * nothing ever writes the fourth. Demanding all sixteen bytes refused the boat's
     * final force sum on exactly that lane (x <- 0, y <- r2+1188, z <- r2+1192, w never)
     * and the refusal took the matrix transform, the second add and the normaliser after
     * it down too. So for an operator whose EVERY input is a Float3, x/y/z known is
     * enough. RotateFloat3 is left out on purpose: its quaternion's fourth lane is data. */
    /* ON BY DEFAULT since the aircraft blocks were cleared (BF6_FLOAT3_STRICT=1 for A/B).
     * It was opt-in because on the f22 it exposed what looked like a gear defect: once the
     * airspeed was known the gear stopped running and the jet fell through the runway.
     * The gear was never at fault. Its master gate reads the Airborne State channel, and
     * that channel was being set by a block behind an UNRESOLVED affector query
     * (0x02C66B00) whose IsActive the VM guessed true - a "vehicle disabled" effect that
     * also zeroes the throttle. With that query answered (FUN_141727850's own miss path),
     * every jet takes off under this rule, and the boat drives. */
    static const bool relax_float3 = std::getenv("BF6_FLOAT3_STRICT") == nullptr;
    const bool all_float3 = relax_float3 && (n == "AddFloat3" || n == "SubtractFloat3" ||
                            n == "AbsoluteFloat3" || n == "AverageFloat3" ||
                            n == "DistanceFloat3" || n == "DotFloat3" ||
                            n == "EqualsFloat3" || n == "NotEqualsFloat3" ||
                            n == "MagnitudeFloat3" || n == "MagnitudeSquaredFloat3" ||
                            n == "NormalizeFloat3" ||
                            n == "MultiplyFloat3Float3Float3" || n == "CrossVec3");
    /* BF6_FLOAT3_ONLY=<name>[,<name>]: apply the relaxation to named operators only, so
     * which one changes a vehicle can be found by bisection. Diagnostic. */
    bool relax = all_float3;
    if (relax)
        if (const char* only = std::getenv("BF6_FLOAT3_ONLY"))
            relax = std::string(",") .append(only).append(",").find("," + n + ",") != std::string::npos;
    /* BF6_FLOAT3_RECS=<rec>[,<rec>]: and only at these record offsets (decimal). */
    if (relax)
        if (const char* recs = std::getenv("BF6_FLOAT3_RECS"))
            relax = std::string(",").append(recs).append(",").find("," + std::to_string(cur_record_) + ",") != std::string::npos;
    for (const Value& v : a) {
        if (v.known) continue;
        bool xyz = relax && v.bytes.size() >= 12 && v.known_bytes.size() >= 12;
        for (int i = 0; xyz && i < 12; ++i) xyz = v.known_bytes[(size_t)i] != 0;
        if (!xyz) return false;
    }
    served_[n] += 1;

    if (n == "SubtractFloat")        { out = put_f32(f32(a[0]) - f32(a[1])); return true; }
    if (n == "NegateFloat")          { out = put_f32(-f32(a[0])); return true; }
    if (n == "AbsoluteFloat")        { out = put_f32(std::fabs(f32(a[0]))); return true; }
    if (n == "MagnitudeSquaredFloat3" || n == "MagnitudeFloat2") {
        float v[3] = {0, 0, 0};
        const size_t c = n == "MagnitudeFloat2" ? 2 : 3;
        for (size_t i = 0; i < c; ++i) std::memcpy(&v[i], a[0].bytes.data() + 4 * i, 4);
        const float m2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        out = put_f32(n == "MagnitudeFloat2" ? std::sqrt(m2) : m2);
        return true;
    }
    if (n == "ToUInt32Float") {
        /* cvttss2si into 64 bits, low 32 kept (what an MSVC float->uint32 cast emits) */
        const float x = f32(a[0]);
        const uint32_t u = (uint32_t)(int64_t)x;
        Value v; v.bytes.assign(4, 0); std::memcpy(v.bytes.data(), &u, 4); v.known = true;
        out = v;
        return true;
    }
    if (n == "ToInt32UInt") { out = a[0]; out.bytes.resize(4); return true; }

    /* The five with no other home. See the note on their spec-table entries: the
     * rest of the arithmetic the aircraft graphs call is already served by
     * NamedBuiltins, and appears "unresolved" only when its input is unknown. */
    if (n == "ModuloFloat")          { out = put_f32(std::fmod(f32(a[0]), f32(a[1]))); return true; }
    if (n == "CosFloat")             { out = put_f32(std::cos(f32(a[0]))); return true; }
    if (n == "BitwiseAndInt")        { out = Value::from_u32(a[0].as_u32() & a[1].as_u32()); return true; }
    if (n == "NotEqualsUInt")        { out = Value::from_bool(a[0].as_u32() != a[1].as_u32()); return true; }
    if (n == "AverageFloat3") {
        Value r; r.bytes.assign(16, 0); r.known = true;
        for (int i = 0; i < 3; ++i) {
            float x = 0, y = 0;
            std::memcpy(&x, a[0].bytes.data() + 4 * i, 4);
            std::memcpy(&y, a[1].bytes.data() + 4 * i, 4);
            const float m = (x + y) * 0.5f;
            std::memcpy(r.bytes.data() + 4 * i, &m, 4);
        }
        out = r;
        return true;
    }
    if (n == "NormalizeFloat2") {
        float v[2] = {0, 0};
        std::memcpy(v, a[0].bytes.data(), 8);
        const float m = std::sqrt(v[0] * v[0] + v[1] * v[1]);
        /* A zero vector normalises to itself here rather than to a NaN: the
         * alternative is dividing by zero and handing a NaN to the rest of the
         * graph, where it would spread silently. */
        if (m > 0.f) { v[0] /= m; v[1] /= m; }
        Value r; r.bytes.assign(8, 0); std::memcpy(r.bytes.data(), v, 8); r.known = true;
        out = r;
        return true;
    }

    if (n == "MinFloat")             { out = put_f32(std::fmin(f32(a[0]), f32(a[1]))); return true; }
    if (n == "MaxFloat")             { out = put_f32(std::fmax(f32(a[0]), f32(a[1]))); return true; }
    if (n == "ClampFloat") {
        const float lo = f32(a[1]), hi = f32(a[2]);
        out = put_f32(std::fmin(std::fmax(f32(a[0]), lo), hi));
        return true;
    }
    if (n == "SignFloat") {
        const float v = f32(a[0]);
        out = put_f32(v > 0.f ? 1.f : (v < 0.f ? -1.f : 0.f));
        return true;
    }
    if (n == "EqualsFloat")          { out = Value::from_bool(f32(a[0]) == f32(a[1])); return true; }
    if (n == "RadiansToDegreesFloat"){ out = put_f32(f32(a[0]) * 57.2957795130823f); return true; }
    if (n == "DegreesToRadiansFloat"){ out = put_f32(f32(a[0]) * 0.0174532925199433f); return true; }
    if (n == "NormalizeAngleZeroToTwoPi") {
        const float two_pi = 6.28318530717959f;
        float v = std::fmod(f32(a[0]), two_pi);
        if (v < 0.f) v += two_pi;
        out = put_f32(v);
        return true;
    }
    if (n == "Atan2Float")           { out = put_f32(std::atan2(f32(a[0]), f32(a[1]))); return true; }
    if (n == "ArcSineFloat")         { out = put_f32(std::asin(f32(a[0]))); return true; }
    if (n == "ArcCosineFloat")       { out = put_f32(std::acos(f32(a[0]))); return true; }
    if (n == "ATanFloat")            { out = put_f32(std::atan(f32(a[0]))); return true; }
    if (n == "CosineFloat")          { out = put_f32(std::cos(f32(a[0]))); return true; }
    if (n == "RangeChange") {
        /* (value, inMin, inMax, outMin, outMax). A zero input span has no answer,
         * so it refuses rather than dividing by zero and returning an infinity the
         * graph would carry onwards as though it were real. */
        const float v = f32(a[0]), i0 = f32(a[1]), i1 = f32(a[2]);
        const float o0 = f32(a[3]), o1 = f32(a[4]);
        if (i1 == i0) return false;
        out = put_f32(o0 + (v - i0) * (o1 - o0) / (i1 - i0));
        if (std::getenv("BF6_RANGE_DEBUG"))
            std::fprintf(stderr, "RangeChange(%g, %g..%g -> %g..%g) = %g\n",
                         v, i0, i1, o0, o1, f32(out));
        return true;
    }
    if (n == "DivideIntIntInt") {
        const int32_t d = (int32_t)a[1].as_u32();
        if (d == 0) return false;
        out = Value::from_u32((uint32_t)((int32_t)a[0].as_u32() / d));
        return true;
    }
    if (n == "ModuloInt") {
        const int32_t d = (int32_t)a[1].as_u32();
        if (d == 0) return false;
        out = Value::from_u32((uint32_t)((int32_t)a[0].as_u32() % d));
        return true;
    }
    if (n == "ToUInt32Int")          { out = Value::from_u32(a[0].as_u32()); return true; }
    if (n == "ToFloatInt")           { out = put_f32((float)(int32_t)a[0].as_u32()); return true; }
    if (n == "ToInt32Bool")          { out = Value::from_u32(a[0].as_bool() ? 1u : 0u); return true; }
    if (n == "AbsoluteInt") {
        const int32_t v = (int32_t)a[0].as_u32();
        out = Value::from_u32((uint32_t)(v < 0 ? -v : v));
        return true;
    }
    if (n == "SignInt") {
        const int32_t v = (int32_t)a[0].as_u32();
        out = Value::from_u32((uint32_t)(int32_t)(v > 0 ? 1 : (v < 0 ? -1 : 0)));
        return true;
    }
    if (n == "ToFloatUInt")          { out = put_f32((float)a[0].as_u32()); return true; }
    if (n == "ToFloatBool")          { out = put_f32(a[0].as_bool() ? 1.f : 0.f); return true; }
    if (n == "NotEqualsBool")        { out = Value::from_bool(a[0].as_bool() != a[1].as_bool()); return true; }

    float u[3], v[3];
    if (n == "AddFloat3") {
        vec3(a[0], u); vec3(a[1], v);
        const float r[3] = {u[0] + v[0], u[1] + v[1], u[2] + v[2]};
        out = put_vec3(r); return true;
    }
    if (n == "SubtractFloat3") {
        vec3(a[0], u); vec3(a[1], v);
        const float r[3] = {u[0] - v[0], u[1] - v[1], u[2] - v[2]};
        out = put_vec3(r); return true;
    }
    if (n == "AbsoluteFloat3") {
        vec3(a[0], u);
        const float r[3] = {std::fabs(u[0]), std::fabs(u[1]), std::fabs(u[2])};
        out = put_vec3(r); return true;
    }
    if (n == "MagnitudeFloat3") {
        vec3(a[0], u);
        out = put_f32(std::sqrt(u[0]*u[0] + u[1]*u[1] + u[2]*u[2])); return true;
    }
    if (n == "NormalizeFloat3") {
        vec3(a[0], u);
        /* The native (0x142493da7) returns zero when every lane is within the per-lane
         * eps at 0x14931f710 (1.19e-7), as MagnitudeFloat3 does. Refusing instead made
         * an aircraft at rest - relative wind exactly zero - lose every wing's angle
         * of attack and with it the whole AngularAcceleration channel.
         * BF6_NORMALIZE_REFUSE_ZERO=1 restores the old refusal for A/B. */
        static const bool refuse_zero = std::getenv("BF6_NORMALIZE_REFUSE_ZERO") != nullptr;
        const float eps = 1.19209290e-7f;
        if (std::fabs(u[0]) <= eps && std::fabs(u[1]) <= eps && std::fabs(u[2]) <= eps) {
            if (refuse_zero) return false;
            const float z[3] = {0.f, 0.f, 0.f};
            out = put_vec3(z); return true;
        }
        const float m = std::sqrt(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
        const float r[3] = {u[0]/m, u[1]/m, u[2]/m};
        out = put_vec3(r); return true;
    }
    if (n == "AngleBetweenFloat") {
        /* FUN_14249AF00, line for line. The two directions are normalised with
         * rsqrtps and TWO Newton steps (the refinement appears once on its own and
         * again inside the final expression), which is not the same number an exact
         * reciprocal square root gives - so the hardware instruction is used rather
         * than 1/sqrt, and the angle matches the game's. */
        float p[3], A[3], b3[3];
        vec3(a[0], A); vec3(a[1], b3); vec3(a[2], p);
        const float ua[3] = {p[0] - A[0], p[1] - A[1], p[2] - A[2]};
        const float ub[3] = {p[0] - b3[0], p[1] - b3[1], p[2] - b3[2]};
        auto inv_len = [](const float v[3]) {
            const float s = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
            const float half = s * 0.5f;
            float y = _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(s)));
            y = (0.5f - y * y * half) * y + y;      /* first Newton step */
            y = (0.5f - y * y * half) * y + y;      /* and the second */
            return y;
        };
        const float ra = inv_len(ua), rb = inv_len(ub);
        /* The association and the summation order are the native's: each term is
         * ((rb * ub[i]) * ra) * ua[i], and the lanes are added y, then x, then z. */
        auto term = [&](int i) { return ((rb * ub[i]) * ra) * ua[i]; };
        float d = term(1) + term(0) + term(2);
        if (d <= -1.0f) d = -1.0f;
        if (1.0f <= d) d = 1.0f;
        out = put_f32(std::acos(d));
        return true;
    }
    if (n == "DistanceFloat3") {
        vec3(a[0], u); vec3(a[1], v);
        const float d[3] = {u[0]-v[0], u[1]-v[1], u[2]-v[2]};
        out = put_f32(std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2])); return true;
    }
    if (n == "MultiplyFloat3FloatFloat3") {
        vec3(a[0], u);
        const float s = f32(a[1]);
        const float r[3] = {u[0]*s, u[1]*s, u[2]*s};
        out = put_vec3(r); return true;
    }
    if (n == "MultiplyFloatFloat3Float3") {
        const float s = f32(a[0]);
        vec3(a[1], v);
        const float r[3] = {v[0]*s, v[1]*s, v[2]*s};
        out = put_vec3(r); return true;
    }
    if (n == "MultiplyFloat3Float3Float3") {
        vec3(a[0], u); vec3(a[1], v);
        const float r[3] = {u[0]*v[0], u[1]*v[1], u[2]*v[2]};
        out = put_vec3(r); return true;
    }

    if (n == "NotEqualsFloat3" || n == "EqualsFloat3") {
        vec3(a[0], u); vec3(a[1], v);
        const bool same = u[0] == v[0] && u[1] == v[1] && u[2] == v[2];
        out = Value::from_bool(n[0] == 'N' ? !same : same);
        return true;
    }
    if (n == "DivideFloat3FloatFloat3") {
        vec3(a[0], u);
        const float d = f32(a[1]);
        if (d == 0.f) return false;
        const float r[3] = {u[0]/d, u[1]/d, u[2]/d};
        out = put_vec3(r); return true;
    }
    if (n == "CrossVec3") {
        vec3(a[0], u); vec3(a[1], v);
        const float r[3] = {u[1]*v[2] - u[2]*v[1],
                            u[2]*v[0] - u[0]*v[2],
                            u[0]*v[1] - u[1]*v[0]};
        out = put_vec3(r); return true;
    }
    if (n == "DotFloat3") {
        vec3(a[0], u); vec3(a[1], v);
        out = put_f32(u[0]*v[0] + u[1]*v[1] + u[2]*v[2]); return true;
    }
    if (n == "CosFloat")             { out = put_f32(std::cos(f32(a[0]))); return true; }
    if (n == "ComplementFloat")      { out = put_f32(1.f - f32(a[0])); return true; }
    if (n == "HalfPi")               { out = put_f32(1.57079632679490f); return true; }
    if (n == "Pi")                   { out = put_f32(3.14159265358979f); return true; }
    if (n == "TwoPi")                { out = put_f32(6.28318530717959f); return true; }
    if (n == "RoundFloatInt") {
        /* Named ...Int, so the result is an integer value even though the slot is
         * four bytes wide either way. Half away from zero, which is what std::round
         * does and what a gear or a wheel-count would want. */
        out = Value::from_u32((uint32_t)(int32_t)std::lround(f32(a[0])));
        return true;
    }
    if (n == "NormalizeAngleMinusPiToPi") {
        const float two_pi = 6.28318530717959f, pi = 3.14159265358979f;
        float x = std::fmod(f32(a[0]) + pi, two_pi);
        if (x < 0.f) x += two_pi;
        out = put_f32(x - pi); return true;
    }
    if (n == "ImplicitToInt32Bool")  { out = Value::from_u32(a[0].as_bool() ? 1u : 0u); return true; }
    if (n == "MinInt") {
        const int32_t x = (int32_t)a[0].as_u32(), y = (int32_t)a[1].as_u32();
        out = Value::from_u32((uint32_t)(x < y ? x : y)); return true;
    }
    if (n == "MaxInt") {
        const int32_t x = (int32_t)a[0].as_u32(), y = (int32_t)a[1].as_u32();
        out = Value::from_u32((uint32_t)(x > y ? x : y)); return true;
    }
    if (n == "GreaterThanUInt")      { out = Value::from_bool(a[0].as_u32() > a[1].as_u32()); return true; }
    if (n == "LessThanUInt")         { out = Value::from_bool(a[0].as_u32() < a[1].as_u32()); return true; }
    if (n == "InterpolateFloat") {
        const float x = f32(a[0]), y = f32(a[1]), t = f32(a[2]);
        out = put_f32(x + (y - x) * t); return true;
    }

    float ma[4][3], mb[4][3], mr[4][3];
    if (n == "InverseRotate") {
        vec3(a[0], u);
        xform(a[1], ma);
        float r[3];
        for (int c = 0; c < 3; ++c) r[c] = u[0]*ma[c][0] + u[1]*ma[c][1] + u[2]*ma[c][2];
        out = put_vec3(r); return true;
    }
    if (n == "MultiplyFloat3LinearTransformFloat3" || n == "RotateFloat3") {
        vec3(a[0], u);
        xform(a[1], ma);
        float r[3];
        for (int c = 0; c < 3; ++c) {
            r[c] = u[0]*ma[0][c] + u[1]*ma[1][c] + u[2]*ma[2][c];
            /* RotateFloat3 is the rotation alone; the multiply carries row 3. */
            if (n[0] == 'M') r[c] += ma[3][c];
        }
        out = put_vec3(r); return true;
    }
    if (n == "InterpolateLinearTransform") {
        /* Component-wise, which is what an expression graph can express. It is NOT a
         * proper rotation interpolation, and for a small step between two nearby
         * transforms - which is what a suspension or a door does per frame - the
         * difference is not observable. Noted rather than hidden. */
        xform(a[0], ma); xform(a[1], mb);
        const float t = f32(a[2]);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c)
                mr[r][c] = ma[r][c] + (mb[r][c] - ma[r][c]) * t;
        out = put_xform(mr); return true;
    }
    if (n == "MultiplyLinearTransformLinearTransformLinearTransform") {
        xform(a[0], ma); xform(a[1], mb);
        mul_xform(ma, mb, mr);
        out = put_xform(mr); return true;
    }
    if (n == "AddLinearTransform") {
        xform(a[0], ma); xform(a[1], mb);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c) mr[r][c] = ma[r][c] + mb[r][c];
        out = put_xform(mr); return true;
    }
    if (n == "Translation") {
        xform(a[0], ma);
        out = put_vec3(ma[3]); return true;
    }
    if (n == "InverseLinearTransform") {
        /* A GENERAL inverse, not a transpose. A transpose is only the inverse of an
         * orthonormal rotation, and a Frostbite LinearTransform can carry scale -
         * which is exactly what a suspension or a wheel rig does. So invert the 3x3
         * by its adjugate and refuse a singular one rather than returning infinities
         * the graph would carry on as though they were real. */
        xform(a[0], ma);
        const float det =
            ma[0][0] * (ma[1][1]*ma[2][2] - ma[1][2]*ma[2][1]) -
            ma[0][1] * (ma[1][0]*ma[2][2] - ma[1][2]*ma[2][0]) +
            ma[0][2] * (ma[1][0]*ma[2][1] - ma[1][1]*ma[2][0]);
        if (det == 0.f) return false;
        const float id = 1.f / det;
        float inv[3][3];
        inv[0][0] = (ma[1][1]*ma[2][2] - ma[1][2]*ma[2][1]) * id;
        inv[0][1] = (ma[0][2]*ma[2][1] - ma[0][1]*ma[2][2]) * id;
        inv[0][2] = (ma[0][1]*ma[1][2] - ma[0][2]*ma[1][1]) * id;
        inv[1][0] = (ma[1][2]*ma[2][0] - ma[1][0]*ma[2][2]) * id;
        inv[1][1] = (ma[0][0]*ma[2][2] - ma[0][2]*ma[2][0]) * id;
        inv[1][2] = (ma[0][2]*ma[1][0] - ma[0][0]*ma[1][2]) * id;
        inv[2][0] = (ma[1][0]*ma[2][1] - ma[1][1]*ma[2][0]) * id;
        inv[2][1] = (ma[0][1]*ma[2][0] - ma[0][0]*ma[2][1]) * id;
        inv[2][2] = (ma[0][0]*ma[1][1] - ma[0][1]*ma[1][0]) * id;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) mr[r][c] = inv[r][c];
        /* p' = p*R + t, so p = (p' - t)*Rinv and the inverse translation is -t*Rinv. */
        for (int c = 0; c < 3; ++c)
            mr[3][c] = -(ma[3][0]*inv[0][c] + ma[3][1]*inv[1][c] + ma[3][2]*inv[2][c]);
        out = put_xform(mr); return true;
    }
    if (n == "LinearTransformfromXangle" || n == "LinearTransformfromYangle" ||
        n == "LinearTransformfromZangle") {
        /* HANDEDNESS IS NOT VERIFIED AGAINST THE GAME. The 64-byte size and the row
         * order are measured; which way a positive angle turns is not, because no
         * shipped asset was found that states a rotation and its result together. A
         * wrong sign here turns a wheel or a control surface the wrong way - visible,
         * and not silently corrupting - so it is implemented with the ordinary
         * right-handed convention and flagged here rather than left blocking 193
         * graphs. The self-check pins the identities that hold either way:
         * R(a)*R(-a) = identity and R(a)*R(b) = R(a+b). */
        const float ang = f32(a[0]);
        const float c = std::cos(ang), s = std::sin(ang);
        for (int r = 0; r < 4; ++r)
            for (int q = 0; q < 3; ++q) mr[r][q] = 0.f;
        if (n == "LinearTransformfromXangle") {
            mr[0][0] = 1.f;
            mr[1][1] = c;  mr[1][2] = s;
            mr[2][1] = -s; mr[2][2] = c;
        } else if (n == "LinearTransformfromYangle") {
            mr[1][1] = 1.f;
            mr[0][0] = c;  mr[0][2] = -s;
            mr[2][0] = s;  mr[2][2] = c;
        } else {
            mr[2][2] = 1.f;
            mr[0][0] = c;  mr[0][1] = s;
            mr[1][0] = -s; mr[1][1] = c;
        }
        out = put_xform(mr); return true;
    }
    return false;
}

bool ChainHost::describe(uint32_t key, OperatorSignature& out) {
    for (Host* h : hosts_)
        if (h->describe(key, out)) return true;
    out = OperatorSignature{};
    return false;
}

bool ChainHost::invoke(uint32_t key, const std::vector<Value>& args, Value& out) {
    OperatorSignature signature;
    for (Host* h : hosts_)
        if (h->describe(key, signature)) return h->invoke(key, args, out);
    return false;
}

}} // namespace bf6::expression

namespace bf6 { namespace expression {

namespace {
const uint32_t kIdTriple   = 0x42C97598u; /* ctr thunk 147EE9FC0 (disassembled)   */
const uint32_t kIdValid    = 0xF0F74455u; /* ctr thunk 147EED6A0 (disassembled)   */
const uint32_t kLtFlagCopy = 0x4B537EA5u; /* FUN_147d20f70 / FUN_143a62b60        */
const uint32_t kLtToM44    = 0xE137982Eu; /* ctr thunk 147D552D0 (disassembled)   */
const uint32_t kVec3Length = 0x672C0739u; /* FUN_147EE2BC0 -> FUN_1443EA180       */
const uint32_t kU32Cast    = 0x27F16E6Eu; /* ctr thunk 148413990 -> 146E9AE40      */
/* DRIVETRAIN (MotionMachine) - thunks disassembled, natives decompiled: */
const uint32_t kCurve15    = 0xEEDA8B09u; /* thunk 147EE2950 -> FUN_1443e4f30(x, curve, 15) */
const uint32_t kCurveScale = 0xC7688D2Du; /* thunk 147EE1040 -> curve(x)*(1-load)*scale   */
const uint32_t kGearRatio  = 0xE4610A9Fu; /* thunk 147EE18C0 -> FUN_1443e6b80             */
const uint32_t kRandom     = 0xDEDE1EF5u; /* engine node FUN_14566B2A0 (+ FUN_14271D400/D540) */
const uint32_t kRevLimit   = 0x1C214DBCu; /* thunk 147EE0E80 -> FUN_1443E5210             */
const uint32_t kClutchGear = 0xAFFD9D93u; /* thunk 147EE1BD0 -> FUN_1443E6CB0             */
const uint32_t kClutchSlip  = 0x893E29C6u; /* thunk 147EE0CF0 (inline) == FUN_1443E51C0     */
/* BOOLEAN AND / OR, by key for the same reason AbsoluteFloat is: the name scan does
 * not resolve these two uniquely, and neither is in any of the three registries this
 * library reads. Their identity comes from the kernel disassembly recorded in the
 * research databank (impl/anim_ant/specs/spec_expression.md 5.2): And at 0x1424A6FC0
 * is `a && b`, Or at 0x1424A7200 is `a || b`, both two-operand forms with n-ary
 * siblings at their own keys. Every boat branch that decides whether the engine
 * makes thrust runs through one of them, and an unknown condition there is a
 * guessed branch and a graph that never reaches its own force. */
const uint32_t kBoolAnd     = 0x8EB2196Du; /* And  (0x1424A6FC0): a && b */
const uint32_t kBoolOr      = 0xEABFEC6Fu; /* Or   (0x1424A7200): a || b */
const uint32_t kAbsFloat    = 0x41BE725Eu; /* AbsoluteFloat: name-registered builtin (0x142268900); the
                                              * name scan does not resolve this key uniquely, so by key */
const uint32_t kIntNonNeg   = 0xF474EAFDu; /* thunk 147D4E590 == FUN_143B22DF0: x < 0 ? 0 : x (int) */
const uint32_t kWheelLimit  = 0x0293E1A4u; /* thunk 147EE4EE0 (inline) == FUN_1443F5920 */
const uint32_t kWheelSpin   = 0x3B247E83u; /* thunk 147EE4890 -> FUN_1443F47E0 (not in the decompile) */
const uint32_t kTopGear     = 0xD92FFA67u; /* thunk 147EE1F80 (inline): last non-zero of 10 */
const float    kRadToRpm    = 9.549296379f;/* .rdata 0x1493195C4 = 0x4118C9EB = 60/(2 pi)  */
const uint32_t kLerpFloat   = 0xA9C36449u; /* builtin 0x140F0C040: (a - a*t) + b*t, unclamped */
const uint32_t kPtrNotEq    = 0x7E1F69AAu; /* node 0x1424AB3C0: *p1 != *p2 (loop guard)       */
const uint32_t kFirstTrue   = 0xBD059C86u; /* shape-B node 0x143B67880: first true of n bools */
const uint32_t kCurveBytes = 15u * 8u;    /* 15 (x, y) float pairs                         */
const uint32_t kGearBytes  = 0x54u;       /* 10 forward, 10 reverse, final drive at +0x50  */

/* FUN_1443e4f30, ported line for line: piecewise-linear over n (x,y) pairs at an
 * 8-byte stride. The scan stops at the first x that does not increase (the used
 * count) or at the first x above the input; the bracketing segment is then
 * evaluated as a line, which also EXTRAPOLATES beyond either end. */
float curve_eval(float x, const float* p, uint32_t n) {
    if (n == 1) return p[1];
    uint32_t u5 = 0, u8 = n;
    for (uint32_t i = 0; i < n; ++i) {
        u5 = i;
        if (i > 0 && p[2 * i] <= p[2 * i - 2]) { u8 = i; goto done; }
        u8 = n;
        if (x < p[2 * i]) goto done;
    }
    u5 = n;
done:
    const uint32_t u7 = u5 ? u5 - 1 : 0;
    const bool mask = u5 != u8;
    const uint32_t hi = mask ? u7 + 1 : u8 - 1;
    const uint32_t lo = mask ? u7 : u8 - 2;
    /* Degenerate curve (one usable point): the engine's lo index is -1 and it
     * reads the pair before the table. That memory does not exist here, so the
     * single point's y is returned. */
    if (hi >= n || lo >= n) return p[1];
    float y = p[2 * hi + 1];
    const float x1 = p[2 * hi];
    const float dx = x1 - p[2 * lo];
    if (dx != 0.0f) {
        const float slope = (y - p[2 * lo + 1]) / dx;
        y = slope * x + (y - x1 * slope);
    }
    return y;
}
/* The invalid-id sentinel the id tests compare against: the dword at
 * 0x149B71B48 in .data, read from the installed image (0x000FFFFF). */
const uint32_t kInvalidId  = 0x000FFFFFu;
}

bool RecoveredOps::describe(uint32_t key, OperatorSignature& out) {
    out = OperatorSignature{};
    switch (key) {
    case kIdTriple:
        /* in0 u32 -> out0 (in0 == 0), out1 (in0 != 0), out2 = in0.
         * The primary output is the LAST operand (the u32); the two bools are the
         * operands before it. */
        out.input_widths = {4};
        out.extra_output_widths = {1, 1};
        out.output_width = 4;
        return true;
    case kIdValid:   out.input_widths = {4};  out.output_width = 1;  return true;
    case kLtFlagCopy:out.input_widths = {64}; out.output_width = 64; return true;
    case kLtToM44:   out.input_widths = {64}; out.output_width = 64; return true;
    case kVec3Length:out.input_widths = {16}; out.output_width = 4;  return true;
    case kU32Cast:   out.input_widths = {4};  out.output_width = 4;  return true;
    case kCurve15:   out.input_widths = {kCurveBytes, 4}; out.output_width = 4; return true;
    case kCurveScale:out.input_widths = {4, 4, 4, kCurveBytes}; out.output_width = 4; return true;
    case kGearRatio:
        /* inputs (gear int, gear table); outputs (ratio, ratio * final drive) -
         * the primary is the LAST output, the ratio is the extra before it. */
        out.input_widths = {4, kGearBytes};
        out.extra_output_widths = {4};
        out.output_width = 4;
        return true;
    case kLerpFloat: out.input_widths = {4, 4, 4}; out.output_width = 4; return true;
    case kFirstTrue:
        /* (count n, bool x n) -> index; the default shape is n = 2, per call see
         * describe_call */
        out.input_widths = {4, 1, 1};
        out.output_width = 4;
        return true;
    case kPtrNotEq:  out.input_widths = {8, 8};    out.output_width = 1; return true;
    case kClutchSlip:
        /* (rpm, gear ratio, drive wheel speed) -> slip in [0, 1] */
        out.input_widths = {4, 4, 4};
        out.output_width = 4;
        return true;
    case kBoolAnd:
    case kBoolOr:
        out.input_widths = {1, 1};
        out.output_width = 1;
        return true;
    case kAbsFloat:
        out.input_widths = {4};
        out.output_width = 4;
        return true;
    case kIntNonNeg:
        out.input_widths = {4};
        out.output_width = 4;
        return true;
    case kWheelLimit:
        /* (engine rpm, gear ratio, wheel angular velocity) -> the wheel speed the engine allows */
        out.input_widths = {4, 4, 4};
        out.output_width = 4;
        return true;
    case kWheelSpin:
        /* (dt, wheel angular velocity, resistance torque, WheelConfig 0x70) -> float */
        out.input_widths = {4, 4, 4, 0x70};
        out.output_width = 4;
        return true;
    case kTopGear:
        /* (10-float gear table) -> the highest non-zero entry, 0 if none */
        out.input_widths = {40};
        out.output_width = 4;
        return true;
    case kRandom:
        /* (seed, mode, a, b) -> (sample float, new state u32); primary = state */
        out.input_widths = {4, 4, 4, 4};
        out.extra_output_widths = {4};
        out.output_width = 4;
        return true;
    case kRevLimit:
        /* 9 scalars then the limiter table (fields +0x00..+0x14 read) -> float */
        out.input_widths = {4, 4, 4, 4, 4, 4, 4, 4, 4, 24};
        out.output_width = 4;
        return true;
    case kClutchGear:
        /* (dt, gear, clutch times[2], unused, phase, target gear, clutch, dir byte,
         * bypass byte) -> (phase, target gear, clutch, engaged, dir byte);
         * primary = the dir byte (last), extras the four before it. */
        out.input_widths = {4, 4, 8, 0, 4, 4, 4, 1, 1};
        out.extra_output_widths = {4, 4, 4, 4};
        out.output_width = 1;
        return true;
    default: return false;
    }
}

bool RecoveredOps::describe_call(uint32_t key, const std::vector<uint32_t>& consts,
                                 OperatorSignature& out) {
    if (key == kFirstTrue && !consts.empty() && consts[0] != 0xFFFFFFFFu && consts[0] <= 64) {
        out = OperatorSignature{};
        out.input_widths.assign(1 + consts[0], 1u);
        out.input_widths[0] = 4;
        out.output_width = 4;
        return true;
    }
    return describe(key, out);
}

bool RecoveredOps::invoke(uint32_t key, const std::vector<Value>& args, Value& out) {
    if (key == kFirstTrue) {
        /* 0x143B67880, disassembled: n = *inputs[0]; for i in 0..n-1, if the byte at
         * inputs[1 + i] is non-zero the result is i; none -> 0xFFFFFFFF. */
        if (args.empty() || !args[0].known) return false;
        const uint32_t n = args[0].as_u32();
        if (args.size() < 1 + (size_t)n) return false;
        uint32_t r = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < n; ++i) {
            if (!args[1 + i].known) return false;
            if (args[1 + i].bytes[0] != 0) { r = i; break; }
        }
        served_[key] += 1;
        out = Value::from_u32(r);
        return true;
    }
    /* Which operand of which operator is a curve table (see the knownness note
     * below). The gear table is NOT one of these: its entries are not a rising
     * curve and a missing ratio there is a real gap, not a shorter list. */
    auto curve_input = [](uint32_t k, size_t i) {
        return (k == kCurve15 && i == 0) || (k == kCurveScale && i == 3);
    };
    OperatorSignature sig;
    if (!describe(key, sig) || args.size() != sig.input_widths.size()) return false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (sig.input_widths[i] == 0) continue;   /* a width-0 input is one the native never reads */
        if (args[i].bytes.size() < sig.input_widths[i]) {
            if (std::getenv("BF6_RECOV_DEBUG"))
                std::fprintf(stderr, "recovered op %08X refused: input %zu is %zu bytes, wants %u\n",
                             key, i, args[i].bytes.size(), sig.input_widths[i]);
            return false;
        }
        if (args[i].known) continue;
        /* The wheel-spin native reads ONLY Inertia (+0x60) of its WheelConfig; a
         * mirrored config the graph assembles field by field leaves its padding
         * unwritten, which must not make the call unknown. */
        if (key == kWheelSpin && i == 3 && args[i].known_bytes.size() >= 0x64) {
            bool ok = true;
            for (uint32_t b = 0x60; b < 0x64; ++b) ok = ok && args[i].known_bytes[b] != 0;
            if (ok) continue;
        }
        /* A CURVE IS AS LONG AS ITS RISING X VALUES, not as long as its array.
         * The table operands are fixed-size (15 or 8 entries) and a graph that
         * carries a four-point curve writes four points and leaves the rest
         * alone. The evaluator already stops at the first x that does not rise,
         * and an unwritten entry reads as zero, which is exactly that stop - so
         * requiring the whole array to be known refused every short curve, which
         * is most of them. The first TWO entries must be known, because a line
         * needs two points; beyond that the tail is allowed to be silent.
         *
         * This is the one place knownness is relaxed on a table rather than on a
         * struct field, so it is spelled out: the bytes are not assumed to be
         * anything, they are assumed to END the curve. */
        if (curve_input(key, i) && args[i].known_bytes.size() >= 16) {
            bool ok = true;
            for (uint32_t b = 0; b < 16; ++b) ok = ok && args[i].known_bytes[b] != 0;
            if (ok) continue;
        }
        if (std::getenv("BF6_RECOV_DEBUG")) {
            size_t known = 0;
            for (uint8_t b : args[i].known_bytes) known += b != 0;
            float first = 0.0f;
            if (args[i].bytes.size() >= 4) std::memcpy(&first, args[i].bytes.data(), 4);
            std::fprintf(stderr, "recovered op %08X refused: input %zu, %zu bytes, map %zu, %zu known, first %g\n",
                         key, i, args[i].bytes.size(), args[i].known_bytes.size(), known, first);
        }
        return false;
    }
    served_[key] += 1;
    out = Value{};
    out.known = true;
    switch (key) {
    case kIdTriple: {
        /* mov ecx,[in0]; test; [out2]=ecx; sete [out0]; setne [out1] */
        const uint32_t v = args[0].as_u32();
        out.bytes.assign(6, 0);
        std::memcpy(out.bytes.data(), &v, 4);          /* primary: out2 = in0 */
        out.bytes[4] = v == 0 ? 1 : 0;                  /* extra 0: out0 */
        out.bytes[5] = v != 0 ? 1 : 0;                  /* extra 1: out1 */
        return true;
    }
    case kIdValid:
        /* cmp [in0], dword [0x149B71B48]; setne [out0] */
        out = Value::from_bool(args[0].as_u32() != kInvalidId);
        return true;
    case kLtFlagCopy: {
        /* qwords 0..6 copied, qword 7 = in[7] | 0x0F0F0F0F00000000 */
        out.bytes.assign(args[0].bytes.begin(), args[0].bytes.begin() + 64);
        uint64_t q;
        std::memcpy(&q, out.bytes.data() + 56, 8);
        q |= 0x0F0F0F0F00000000ull;
        std::memcpy(out.bytes.data() + 56, &q, 8);
        return true;
    }
    case kLtToM44: {
        /* each row copied with lane 3 blended: 0 for rows 0-2, 1.0 for row 3 */
        out.bytes.assign(args[0].bytes.begin(), args[0].bytes.begin() + 64);
        const float zero = 0.0f, one = 1.0f;
        for (int r = 0; r < 3; ++r) std::memcpy(out.bytes.data() + r * 16 + 12, &zero, 4);
        std::memcpy(out.bytes.data() + 48 + 12, &one, 4);
        return true;
    }
    case kVec3Length: {
        float v[3];
        std::memcpy(v, args[0].bytes.data(), 12);
        const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        out.bytes.assign(4, 0);
        std::memcpy(out.bytes.data(), &len, 4);
        return true;
    }
    case kU32Cast:
        /* mov ecx,[in0]; mov [out0],ecx */
        out = Value::from_u32(args[0].as_u32());
        return true;
    case kCurve15: {
        float c[30], x;
        std::memcpy(c, args[0].bytes.data(), kCurveBytes);
        std::memcpy(&x, args[1].bytes.data(), 4);
        const float y = curve_eval(x, c, 15);
        out.bytes.assign(4, 0); std::memcpy(out.bytes.data(), &y, 4);
        return true;
    }
    case kCurveScale: {
        float x, scale, load, c[30];
        std::memcpy(&x, args[0].bytes.data(), 4);
        std::memcpy(&scale, args[1].bytes.data(), 4);
        std::memcpy(&load, args[2].bytes.data(), 4);
        std::memcpy(c, args[3].bytes.data(), kCurveBytes);
        const float y = curve_eval(x, c, 15) * (1.0f - load) * scale;
        out.bytes.assign(4, 0); std::memcpy(out.bytes.data(), &y, 4);
        return true;
    }
    case kGearRatio: {
        const int32_t g = (int32_t)args[0].as_u32();
        float t[21];
        std::memcpy(t, args[1].bytes.data(), kGearBytes);
        float ratio = 0.0f, geared = 0.0f;
        const int32_t mag = g < 0 ? -g : g;
        if (g != 0 && mag <= 10) {
            ratio = g > 0 ? t[g - 1] : t[10 + mag - 1];
            geared = ratio * t[20];
        }
        out.bytes.assign(8, 0);
        std::memcpy(out.bytes.data(), &geared, 4);   /* primary: ratio * final */
        std::memcpy(out.bytes.data() + 4, &ratio, 4);/* extra: the gear ratio  */
        return true;
    }
    case kLerpFloat: {
        /* movss a,[rcx]; t,[r8]; x0 = a*t; x1 = t*[rdx]; a - x0 + x1 -> [r9] */
        float a, b, t;
        std::memcpy(&a, args[0].bytes.data(), 4);
        std::memcpy(&b, args[1].bytes.data(), 4);
        std::memcpy(&t, args[2].bytes.data(), 4);
        const float x0 = a * t, x1 = t * b;
        const float y = (a - x0) + x1;
        out.bytes.assign(4, 0);
        std::memcpy(out.bytes.data(), &y, 4);
        return true;
    }
    case kPtrNotEq:
        /* mov rax,[rdx]; cmp [rcx],rax; setne [r8] */
        out = Value::from_bool(std::memcmp(args[0].bytes.data(), args[1].bytes.data(), 8) != 0);
        return true;
    case kClutchSlip: {
        /* Disassembled (both copies identical): if ratio == 0 (ucomiss, so -0 too)
         * the result is 0; else 1 - |ratio| * speed * 60/(2 pi) / max(rpm, 1),
         * clamped to [0, 1]. Multiply order kept: (|ratio| * speed) * K. */
        float rpm, ratio, speed, y = 0.0f;
        std::memcpy(&rpm, args[0].bytes.data(), 4);
        std::memcpy(&ratio, args[1].bytes.data(), 4);
        std::memcpy(&speed, args[2].bytes.data(), 4);
        if (ratio != 0.0f) {
            float t = std::fabs(ratio) * speed;
            t = t * kRadToRpm;
            t = t / (rpm < 1.0f ? 1.0f : rpm);
            y = 1.0f - t;
            if (y < 0.0f) y = 0.0f;
            if (y > 1.0f) y = 1.0f;
        }
        out.bytes.assign(4, 0);
        std::memcpy(out.bytes.data(), &y, 4);
        return true;
    }
    case kBoolAnd:
    case kBoolOr: {
        const bool a = args[0].bytes[0] != 0, b = args[1].bytes[0] != 0;
        out.bytes.assign(1, (uint8_t)((key == kBoolAnd ? (a && b) : (a || b)) ? 1 : 0));
        return true;
    }
    case kAbsFloat: {
        float x;
        std::memcpy(&x, args[0].bytes.data(), 4);
        x = std::fabs(x);
        out.bytes.assign(4, 0);
        std::memcpy(out.bytes.data(), &x, 4);
        return true;
    }
    case kIntNonNeg: {
        /* mov eax,ecx; shr eax,31; xor al,1; mov eax,0; cmove ecx,eax */
        int32_t x = (int32_t)args[0].as_u32();
        if (x < 0) x = 0;
        out = Value::from_u32((uint32_t)x);
        return true;
    }
    case kWheelLimit: {
        /* FUN_1443F5920, disassembled: ratio == 0 (ucomiss, NaN too) passes w through;
         * else |w| capped at |rpm / ratio * 0.10471976| (rpm -> rad/s), sign of w kept,
         * 0 when w is 0. Constants 0x1493183E4 = 2 pi / 60, sign pair 1 / -1, -0.0. */
        float rpm, ratio, w;
        std::memcpy(&rpm, args[0].bytes.data(), 4);
        std::memcpy(&ratio, args[1].bytes.data(), 4);
        std::memcpy(&w, args[2].bytes.data(), 4);
        float y = w;
        if (!(ratio == 0.0f || ratio != ratio)) {
            float x = rpm / ratio;
            x = x * 0.10471976548433304f;
            x = std::fabs(x);
            const float aw = std::fabs(w);
            if (aw < x) x = aw;
            float sign = w < 0.0f ? -1.0f : 1.0f;
            if (!(aw > -0.0f)) sign = 0.0f;
            y = x * sign;
        }
        out.bytes.assign(4, 0);
        std::memcpy(out.bytes.data(), &y, 4);
        return true;
    }
    case kWheelSpin: {
        /* FUN_1443F47E0, disassembled: sign = w >= 0 ? 1 : -1, and 0 unless |w| > -0.0
         * (i.e. 0 when w is 0); out = max(|w| - |resistance| * dt / Inertia, 0) * sign.
         * Inertia is WheelConfig +0x60. Rolling-resistance decay of the wheel spin. */
        float dt, w, res, inertia;
        std::memcpy(&dt, args[0].bytes.data(), 4);
        std::memcpy(&w, args[1].bytes.data(), 4);
        std::memcpy(&res, args[2].bytes.data(), 4);
        std::memcpy(&inertia, args[3].bytes.data() + 0x60, 4);
        float sign = w >= 0.0f ? 1.0f : -1.0f;
        float aw = std::fabs(w);
        if (!(aw > -0.0f)) sign = 0.0f;
        /* BF6_WHEELSPIN_NO_RES=1 drops the resistance. DIAGNOSTIC ONLY: on an f22 one
         * gear arrives here with a resistance of 623.875 against the other two gears'
         * 71.4 and 10, which with an inertia of 1 removes 10.4 rad/s of wheel spin per
         * tick and pins that wheel's omega at zero for ever. This measures whether
         * that is what holds the aircraft on the runway. */
        if (std::getenv("BF6_WHEELSPIN_NO_RES")) res = 0.0f;
        float d = std::fabs(res) * dt;
        d = d / inertia;
        float y = aw - d;
        if (y < 0.0f) y = 0.0f;
        y = y * sign;
        out.bytes.assign(4, 0);
        std::memcpy(out.bytes.data(), &y, 4);
        return true;
    }
    case kTopGear: {
        float t[10], y = 0.0f;
        std::memcpy(t, args[0].bytes.data(), 40);
        for (int i = 9; i >= 0; --i)
            if (t[i] != 0.0f) { y = t[i]; break; }
        out.bytes.assign(4, 0);
        std::memcpy(out.bytes.data(), &y, 4);
        return true;
    }
    case kRandom: {
        /* FUN_14566B2A0: a Park-Miller style two-multiplier step (0x5E30, 0x661F) on
         * a seed offset by 0x759DC150; mode 1 Gaussian (polar Box-Muller), mode 2
         * exponential, otherwise uniform in [a, b). Scaling is 2^-24. */
        const int32_t seed = (int32_t)args[0].as_u32(), mode = (int32_t)args[1].as_u32();
        float a, b;
        std::memcpy(&a, args[2].bytes.data(), 4);
        std::memcpy(&b, args[3].bytes.data(), 4);
        const float k = 5.9604645e-08f;
        const uint32_t u = (uint32_t)seed + 0x759DC150u;
        uint32_t s0 = (uint32_t)(((int32_t)(0u - u | u) >> 31) & (int32_t)((uint32_t)seed + 0x759DC14Fu)) + 1u;
        s0 &= 0x7FFFFFFFu;
        auto step = [](uint32_t v) {
            const uint32_t nz = (uint32_t)(((int32_t)(0u - v | v) >> 31) & (int32_t)(v - 1u)) + 1u;
            const uint32_t lo = (nz & 0xFFFFu) * 0x5E30u;
            const uint32_t hi = (uint32_t)((int32_t)(nz >> 16) * (int32_t)0x5E30 + ((int32_t)lo >> 16));
            const int32_t i = (int32_t)(((hi & 0x7FFFu) * 0x10000u) + (uint32_t)((int32_t)hi >> 15) + (lo & 0xFFFFu) + 0x80000001u);
            const uint32_t w = (uint32_t)(((i >> 31) & 0x7FFFFFFF) + i);
            const uint32_t lo2 = (w & 0xFFFFu) * 0x661Fu;
            const uint32_t hi2 = (uint32_t)(((int32_t)w >> 16) * (int32_t)0x661F + ((int32_t)lo2 >> 16));
            const int32_t j = (int32_t)(((hi2 & 0x7FFFu) * 0x10000u) + (uint32_t)((int32_t)hi2 >> 15) + (lo2 & 0xFFFFu) + 0x80000001u);
            return (uint32_t)(((j >> 31) & 0x7FFFFFFF) + j);
        };
        auto unit = [k](uint32_t v) { return (float)(((int32_t)(v | 0x80u) >> 7) + 1) * k; };
        float sample;
        uint32_t state;
        if (mode == 1) {
            /* FUN_14271D540, first call (no spare): x is returned. */
            uint32_t st = s0;
            float x, y, S;
            do {
                st = step(st); const float f1 = unit(st); x = (f1 + f1) - 1.0f;
                st = step(st); const float f2 = unit(st); y = (f2 + f2) - 1.0f;
                S = y * y + x * x;
            } while (1.0f < S);
            const float R = (float)std::sqrt((double)std::log(S) * -2.0 / (double)S);
            sample = R * x * b + a;
            state = st;
        } else {
            state = step(s0);
            sample = mode == 2 ? -(std::log(unit(state)) * a) : unit(state) * (b - a) + a;
        }
        out.bytes.assign(8, 0);
        std::memcpy(out.bytes.data(), &state, 4);      /* primary: new state */
        std::memcpy(out.bytes.data() + 4, &sample, 4); /* extra: sample       */
        return true;
    }
    case kRevLimit: {
        /* FUN_1443E5210, from muse's transcription checked against the corpus:
         * a target RPM (idle clamp when not engaged, else wheel speed * ratio *
         * 60/2pi blended by load) approached at a table rate, clamped [lo, hi]. */
        float f[9], tbl[6];
        for (int i = 0; i < 9; ++i) std::memcpy(&f[i], args[(size_t)i].bytes.data(), 4);
        std::memcpy(tbl, args[9].bytes.data(), 24);
        const float p1 = f[0], p2 = f[1], p3 = f[2];
        const int32_t p4 = (int32_t)args[3].as_u32();
        float p5 = f[4];
        const float p6 = f[5], p8 = f[7], p9 = f[8];
        const float lo = tbl[0], hi = tbl[4];
        if (std::getenv("BF6_REV_DEBUG"))
            std::fprintf(stderr, "revlimit dt %g rpm %g thr %g engaged %d speed %g p6 %g load %g ratio %g | tbl %g %g %g %g %g %g\n",
                         p1, p2, p3, p4, p5, p6, p8, p9, tbl[0], tbl[1], tbl[2], tbl[3], tbl[4], tbl[5]);
        float target;
        if (p4 == 0) {
            p5 = 10.0f;
            target = (1.0f - p8) * p3 * hi;
            if (target <= lo) target = lo;
        } else {
            const float w = 1.0f - p8;
            const float f3 = (0.25f - w * 0.25f) + w * p3;
            const float kk = 1.0f - (tbl[2] - 1.0f);
            target = std::fabs(p5) * p9 * 9.549296f * ((kk - kk * f3) + tbl[2] * f3);
            if (target <= lo) target = lo;
            if (hi <= target) target = hi;
        }
        const float d = ((target - lo) * (1.0f - p6) + lo) - p2;
        const float ad = std::fabs(d);
        float sgn = d < 0.0f ? -1.0f : 1.0f;
        float rate;
        if (-0.0f < ad) rate = sgn < 0.0f ? tbl[3] : tbl[5];
        else { rate = tbl[5]; sgn = 0.0f; }
        float stp = std::fabs(p5) * ad * p1 * rate;
        if (stp < 1.0f) { stp = ad; if (1.0f <= ad) stp = 1.0f; }
        float y = stp * sgn + p2;
        if (y <= lo) y = lo;
        if (hi <= y) y = hi;
        out.bytes.assign(4, 0);
        std::memcpy(out.bytes.data(), &y, 4);
        return true;
    }
    case kClutchGear: {
        /* FUN_1443E6CB0 state machine. The native first validates the vehicle's
         * live physics handles (each failure leaves the outputs unwritten); a
         * spawned vehicle passes them, so the machine runs here unconditionally. */
        float p1, p3[2], p7;
        std::memcpy(&p1, args[0].bytes.data(), 4);
        std::memcpy(p3, args[2].bytes.data(), 8);
        std::memcpy(&p7, args[6].bytes.data(), 4);
        const int32_t p2 = (int32_t)args[1].as_u32();
        const int32_t p5 = (int32_t)args[4].as_u32();
        int32_t p6 = (int32_t)args[5].as_u32();
        uint8_t p8 = args[7].bytes[0];
        const uint8_t p9 = args[8].bytes[0];
        int32_t o10 = 0, o11 = 0;
        float o12 = 0.0f, o13 = 0.0f;
        uint8_t o14 = 0;
        if (p9 != 0) {
            o10 = p5; o11 = p6; o12 = p7; o13 = 0.0f; o14 = (uint8_t)(p8 ^ 1u);
        } else {
            float f5 = p3[1], f6 = p7;
            int32_t cur = p5;
            bool done = false;
            if (p5 == 0) {
                if (p2 != p6) {
                    if (p6 != 0) {
                        const int32_t a = p6 < 0 ? -p6 : p6;
                        if (a == 1 && p2 == 0) { o10 = 0; f6 = 0.0f; o11 = 0; o12 = p7; }
                        else { o10 = 1; f6 = 0.0f; o11 = p6; o12 = 0.0f; }
                    } else {
                        if (p8 == 0) { if (p2 < 0) p8 ^= 1u; }
                        else if (0 < p2) p8 ^= 1u;
                        o10 = 0; f6 = 0.0f; o11 = p2; o12 = p7;
                    }
                    done = true;
                }
            } else if (p5 == 1) {
                if (f5 <= 0.01f) f5 = 0.01f;
                f6 = p1 / f5 + p7;
                if (1.0f <= f6) f6 = 1.0f;
                if (f6 == 1.0f) { p6 = p2; cur = 2; }
            } else if (p5 == 2) {
                float t = p3[0];
                if (t <= 0.01f) t = 0.01f;
                t = p7 - p1 / t;
                f6 = t;
                if (t <= 0.0f) f6 = 1.0f;
                cur = 0.0f < t ? p5 : 3;
            } else if (p5 == 3) {
                if (f5 <= 0.01f) f5 = 0.01f;
                f6 = p7 - p1 / f5;
                if (f6 <= 0.0f) f6 = 0.0f;
                if (f6 == 0.0f) { o10 = 0; o11 = p6; o12 = f6; f6 = 0.0f; done = true; }
            }
            if (!done) {
                o10 = cur; o11 = p6; o12 = f6;
                if (cur != 1 && cur != 3) f6 = cur == 2 ? 1.0f : 0.0f;
            }
            o13 = f6;
            o14 = p8;
        }
        out.bytes.assign(17, 0);
        out.bytes[0] = o14;                                  /* primary: dir byte */
        std::memcpy(out.bytes.data() + 1, &o10, 4);          /* extras, in order  */
        std::memcpy(out.bytes.data() + 5, &o11, 4);
        std::memcpy(out.bytes.data() + 9, &o12, 4);
        std::memcpy(out.bytes.data() + 13, &o13, 4);
        return true;
    }
    default:
        return false;
    }
}

}} // namespace bf6::expression
