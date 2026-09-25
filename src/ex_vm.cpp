/* ex_vm.cpp - the EX interpreter. See ex_vm.h, native/EX_VM_SPEC.md and
 * native/EX_KERNELS_SPEC.md; every address cited below is from those. */
#include "ex_vm.h"

#include "ant_graph.h"
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unordered_map>

namespace bf6ex {

/* ------------------------------------------------------------------ hashing */

uint32_t kernel_hash(const char* s)
{
    static uint32_t T[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i << 24;
            for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
            T[i] = c;
        }
        built = true;
    }
    uint32_t c = 0xffffffffu;
    for (; *s; ++s) c = (c << 8) ^ T[((c >> 24) ^ (uint8_t)*s) & 0xff];
    return ~c;
}

namespace {

template <class T> T rd(const uint8_t* p) { T v; std::memcpy(&v, p, sizeof(T)); return v; }
uint32_t u32(const std::vector<uint8_t>& b, size_t o) { return o + 4 <= b.size() ? rd<uint32_t>(&b[o]) : 0; }
uint16_t u16(const std::vector<uint8_t>& b, size_t o) { return o + 2 <= b.size() ? rd<uint16_t>(&b[o]) : 0; }
uint32_t align16(uint64_t v) { return (uint32_t)((v + 0xf) & ~0xfull); }

/* ---------------------------------------------------------------- kernels */

/* Direct-call kernels (opcodes 0x00-0x13) get their operands as separate pointers,
 * in program order; list kernels (0x14-0x1d, 0x23) get (contexts, outputs, inputs). */
struct Call {
    void** a = nullptr; int n = 0;                 /* direct */
    void** in = nullptr; int nin = 0;               /* list */
    void** out = nullptr; int nout = 0;
    void** ctx = nullptr; int nctx = 0;
    Instance* inst = nullptr;
};
using KernelFn = void (*)(Call&);
struct KernelImpl { const char* name; KernelFn direct; KernelFn list; };

inline float& F(void* p) { return *(float*)p; }
inline int32_t& I(void* p) { return *(int32_t*)p; }
inline uint8_t& B(void* p) { return *(uint8_t*)p; }
inline float* V(void* p) { return (float*)p; }

/* scalar helpers over the direct operand list */
#define A(i) (c.a[i])
#define UN_F(name, expr) void name(Call& c) { const float x = F(A(0)); F(A(1)) = (expr); }
#define BIN_F(name, expr) void name(Call& c) { const float x = F(A(0)), y = F(A(1)); F(A(2)) = (expr); }
#define BIN_I(name, expr) void name(Call& c) { const int32_t x = I(A(0)), y = I(A(1)); I(A(2)) = (expr); }
#define CMP_F(name, expr) void name(Call& c) { const float x = F(A(0)), y = F(A(1)); B(A(2)) = (expr) ? 1 : 0; }
#define CMP_I(name, expr) void name(Call& c) { const int32_t x = I(A(0)), y = I(A(1)); B(A(2)) = (expr) ? 1 : 0; }

UN_F(k_NegateFloat, -x)
UN_F(k_AbsoluteFloat, std::fabs(x))
UN_F(k_Sin, std::sin(x))
UN_F(k_CosFloat, std::cos(x))
UN_F(k_TanFloat, std::tan(x))
UN_F(k_ATanFloat, std::atan(x))
UN_F(k_ArcSineFloat, std::asin(x))
UN_F(k_ArcCosineFloat, std::acos(x))
UN_F(k_DegreesToRadiansFloat, x * 0.017453292f)
UN_F(k_RadiansToDegreesFloat, x * 57.29578f)
UN_F(k_TicksToTime, x * 0.016666668f)               /* 0x1405d9de0 */
BIN_F(k_AddFloat, x + y)
BIN_F(k_SubtractFloat, x - y)
BIN_F(k_MultiplyFloatFloatFloat, x * y)
BIN_F(k_DivideFloatFloatFloat, x / y)
BIN_F(k_MaxFloat, std::fmax(x, y))
BIN_F(k_MinFloat, std::fmin(x, y))
BIN_F(k_PowFloat, std::pow(x, y))                   /* 0x142498f10 */
BIN_F(k_ModuloFloat, std::fmod(x, y))               /* 0x14248de80 */
BIN_F(k_Atan2Float, std::atan2(x, y))
BIN_I(k_AddInt, x + y)
BIN_I(k_SubtractInt, x - y)
CMP_F(k_GreaterThanFloat, x > y)
CMP_F(k_GreaterThanOrEqualsFloat, x >= y)
CMP_F(k_LessThanFloat, x < y)
CMP_F(k_LessThanOrEqualsFloat, x <= y)
CMP_F(k_EqualsFloat, x == y)
CMP_F(k_NotEqualsFloat, x != y)
CMP_I(k_EqualsInt, x == y)
CMP_I(k_NotEqualsInt, x != y)
CMP_I(k_GreaterThanInt, x > y)
CMP_I(k_GreaterThanOrEqualsInt, x >= y)
CMP_I(k_LessThanInt, x < y)
CMP_I(k_LessThanOrEqualsInt, x <= y)

void k_Not(Call& c) { B(A(1)) = B(A(0)) ? 0 : 1; }
void k_EqualsBool(Call& c) { B(A(2)) = (B(A(0)) != 0) == (B(A(1)) != 0); }
void k_NotEqualsBool(Call& c) { B(A(2)) = (B(A(0)) != 0) != (B(A(1)) != 0); }
void k_AndN(Call& c) { uint8_t r = 1; for (int i = 0; i + 1 < c.n; ++i) r = r && B(A(i)); B(A(c.n - 1)) = r; }
void k_OrN(Call& c) { uint8_t r = 0; for (int i = 0; i + 1 < c.n; ++i) r = r || B(A(i)); B(A(c.n - 1)) = r; }
void k_BoolToFloat(Call& c) { F(A(1)) = B(A(0)) ? 1.f : 0.f; }
void k_IntToFloat(Call& c) { F(A(1)) = (float)I(A(0)); }
void k_ToInt32Float(Call& c) { I(A(1)) = (int32_t)F(A(0)); }
void k_ClampFloat(Call& c) { F(A(3)) = std::fmin(std::fmax(F(A(0)), F(A(1))), F(A(2))); }
void k_BetweenFloat(Call& c) { const float x = F(A(0)); B(A(3)) = x > F(A(1)) && x < F(A(2)); }
void k_BetweenOrEqualsFloat(Call& c) { const float x = F(A(0)); B(A(3)) = x >= F(A(1)) && x <= F(A(2)); }
void k_BetweenInt(Call& c) { const int32_t x = I(A(0)); B(A(3)) = x > I(A(1)) && x < I(A(2)); }
void k_NotNearlyEqualsFloatFloat(Call& c) { B(A(3)) = std::fabs(F(A(0)) - F(A(1))) > F(A(2)); }

/* float3 (stored as float4; w left as the destination had it) */
void k_AddFloat3(Call& c) { float *x = V(A(0)), *y = V(A(1)), *o = V(A(2)); for (int k = 0; k < 3; ++k) o[k] = x[k] + y[k]; }
void k_SubtractFloat3(Call& c) { float *x = V(A(0)), *y = V(A(1)), *o = V(A(2)); for (int k = 0; k < 3; ++k) o[k] = x[k] - y[k]; }
void k_MultiplyFloat3Float3Float3(Call& c) { float *x = V(A(0)), *y = V(A(1)), *o = V(A(2)); for (int k = 0; k < 3; ++k) o[k] = x[k] * y[k]; }
void k_MultiplyFloat3FloatFloat3(Call& c) { float *x = V(A(0)), y = F(A(1)), *o = V(A(2)); for (int k = 0; k < 3; ++k) o[k] = x[k] * y; }
void k_MultiplyFloatFloat3Float3(Call& c) { float x = F(A(0)), *y = V(A(1)), *o = V(A(2)); for (int k = 0; k < 3; ++k) o[k] = x * y[k]; }
void k_DivideFloat3FloatFloat3(Call& c) { float *x = V(A(0)), y = F(A(1)), *o = V(A(2)); for (int k = 0; k < 3; ++k) o[k] = x[k] / y; }
void k_NegateFloat3(Call& c) { float *x = V(A(0)), *o = V(A(1)); for (int k = 0; k < 3; ++k) o[k] = -x[k]; }
void k_TanFloat3(Call& c) { float *x = V(A(0)), *o = V(A(1)); for (int k = 0; k < 3; ++k) o[k] = std::tan(x[k]); }
void k_ATanFloat3(Call& c) { float *x = V(A(0)), *o = V(A(1)); for (int k = 0; k < 3; ++k) o[k] = std::atan(x[k]); }
void k_DotFloat3(Call& c) { float *x = V(A(0)), *y = V(A(1)); F(A(2)) = x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; }
void k_MagnitudeFloat3(Call& c) { float* x = V(A(0)); F(A(1)) = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]); }
void k_NormalizeFloat3(Call& c) { float *x = V(A(0)), *o = V(A(1)); const float l = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]); for (int k = 0; k < 3; ++k) o[k] = x[k] / l; }
void k_EqualsFloat3(Call& c) { float *x = V(A(0)), *y = V(A(1)); B(A(2)) = x[0] == y[0] && x[1] == y[1] && x[2] == y[2]; }
void k_ClampFloat3(Call& c) { float *x = V(A(0)), *lo = V(A(1)), *hi = V(A(2)), *o = V(A(3)); for (int k = 0; k < 3; ++k) o[k] = std::fmin(std::fmax(x[k], lo[k]), hi[k]); }

/* quaternions (x, y, z, w) */
void qmul(const float* a, const float* b, float* o)
{
    const float r[4] = { a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                         a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                         a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                         a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2] };
    std::memcpy(o, r, sizeof r);
}
void k_MultiplyQuaternionQuaternionQuaternion(Call& c) { qmul(V(A(0)), V(A(1)), V(A(2))); }
void k_InverseQuaternion(Call& c) { float *q = V(A(0)), *o = V(A(1)); const float r[4] = { -q[0], -q[1], -q[2], q[3] }; std::memcpy(o, r, sizeof r); }
void k_EqualsQuaternion(Call& c) { float *x = V(A(0)), *y = V(A(1)); B(A(2)) = x[0] == y[0] && x[1] == y[1] && x[2] == y[2] && x[3] == y[3]; }
/* 0x142496b00: v + 2 cross(q, cross(q, v) + w v); w carried */
void rotate(const float* q, const float* v, float* o)
{
    const float t[3] = { q[1] * v[2] - q[2] * v[1] + q[3] * v[0],
                         q[2] * v[0] - q[0] * v[2] + q[3] * v[1],
                         q[0] * v[1] - q[1] * v[0] + q[3] * v[2] };
    const float r[3] = { v[0] + 2 * (q[1] * t[2] - q[2] * t[1]),
                         v[1] + 2 * (q[2] * t[0] - q[0] * t[2]),
                         v[2] + 2 * (q[0] * t[1] - q[1] * t[0]) };
    o[0] = r[0]; o[1] = r[1]; o[2] = r[2];
}
void k_TransformQuaternion(Call& c) { float *v = V(A(0)), *q = V(A(1)), *o = V(A(2)); const float w = v[3]; rotate(q, v, o); o[3] = w; }
void k_OrientationToLocalAxes(Call& c)   /* 0x142495da0: +X, +Z, +Y - in that order */
{
    const float* q = V(A(0));
    const float X[3] = { 1, 0, 0 }, Z[3] = { 0, 0, 1 }, Y[3] = { 0, 1, 0 };
    rotate(q, X, V(A(1))); V(A(1))[3] = 0;
    rotate(q, Z, V(A(2))); V(A(2))[3] = 0;
    rotate(q, Y, V(A(3))); V(A(3))[3] = 0;
}
/* 0x14248eb80: shortest-hemisphere normalized lerp; operands (a, b, t, out) */
void k_InterpolateQuaternion(Call& c)
{
    const float *a = V(A(0)), *b = V(A(1)); const float t = F(A(2)); float* o = V(A(3));
    const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    float v[4];
    for (int k = 0; k < 4; ++k) v[k] = d > 0 ? a[k] + (b[k] - a[k]) * t : a[k] - (b[k] + a[k]) * t;
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]);
    for (int k = 0; k < 4; ++k) o[k] = v[k] / l;
}
/* 0x1408fdf40 */
void k_RotationDecompose(Call& c)
{
    const float* q = V(A(0));
    const float x = q[0], y = q[1], z = q[2], w = q[3], s = x * y + z * w;
    float o0, o1, o2;
    if (std::fabs(s - 0.5f) <= 1.5258789e-5f) { o0 = 2 * std::atan2(x, w); o1 = 1.5707964f; o2 = 0; }
    else if (std::fabs(s + 0.5f) <= 1.5258789e-5f) { o0 = -2 * std::atan2(x, w); o1 = -1.5707964f; o2 = 0; }
    else {
        o0 = std::atan2(2 * (y * w - x * z), 1 - 2 * y * y - 2 * z * z);
        o1 = std::asin(std::fmin(std::fmax(2 * s, -1.f), 1.f));
        o2 = std::atan2(2 * (x * w - y * z), 1 - 2 * x * x - 2 * z * z);
    }
    F(A(1)) = o0; F(A(2)) = o1; F(A(3)) = o2;
}
/* 0x142495280 / 0x1424953f0 */
float norm_pi(float x) { return x >= 0 ? std::fmod(x + 3.1415927f, 6.2831855f) - 3.1415927f : std::fmod(x - 3.1415927f, 6.2831855f) + 3.1415927f; }
void k_NormalizeAngleMinusPiToPi(Call& c) { F(A(1)) = norm_pi(F(A(0))); }
void k_NormalizeAngleZeroToTwoPi(Call& c) { float r = norm_pi(F(A(0))); if (r < 0) r += 6.2831855f; F(A(1)) = r; }
/* 0x14249a2e0: heading of the rotated +X in the xz plane */
void k_AngleAroundYQuat(Call& c)
{
    const float X[3] = { 1, 0, 0 }; float r[3];
    rotate(V(A(0)), X, r);
    if (r[0] * r[0] + r[2] * r[2] <= 0) { F(A(1)) = 0; B(A(2)) = 0; return; }
    const float l = std::sqrt(r[0] * r[0] + r[2] * r[2]);
    F(A(1)) = norm_pi(std::atan2(r[2] / l, r[0] / l)); B(A(2)) = 1;
}

/* 4x4 row-major, row vectors (p' = p M) */
void mmul(const float* a, const float* b, float* o)
{
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4 + 1] * b[4 + j] + a[i * 4 + 0] * b[j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    std::memcpy(o, r, sizeof r);
}
void k_MultiplyFloat44Float44Float44(Call& c) { mmul(V(A(0)), V(A(1)), V(A(2))); }
/* 0x145305ff0: (count, matrices..., out) - the matrices are contiguous in the second operand */
void k_matrixnmulta(Call& c)
{
    const uint32_t n = *(uint32_t*)A(0); const float* m = V(A(1)); float acc[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    for (uint32_t i = 0; i < n; ++i) mmul(acc, m + 16 * i, acc);
    std::memcpy(V(A(2)), acc, sizeof acc);
}
void quat_rows(const float* q, float* r /* 3 rows of 4 */)
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float R[12] = { 1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0,
                          2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0,
                          2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0 };
    std::memcpy(r, R, sizeof R);
}
/* 0x142496900: (scale, rotation, translation, out) */
void k_Matrix44Compose(Call& c)
{
    const float *s = V(A(0)), *q = V(A(1)), *t = V(A(2)); float* o = V(A(3));
    float R[12]; quat_rows(q, R);
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) o[i * 4 + j] = R[i * 4 + j] * s[i];
    o[12] = t[0]; o[13] = t[1]; o[14] = t[2]; o[15] = 1;
}
void mat_to_quat(const float* m /* rows */, float* q)
{
    const float m00 = m[0], m01 = m[1], m02 = m[2], m10 = m[4], m11 = m[5], m12 = m[6], m20 = m[8], m21 = m[9], m22 = m[10];
    const float tr = m00 + m11 + m22;
    if (tr > 0) { const float s = std::sqrt(tr + 1) * 2; q[3] = 0.25f * s; q[0] = (m12 - m21) / s; q[1] = (m20 - m02) / s; q[2] = (m01 - m10) / s; }
    else if (m00 > m11 && m00 > m22) { const float s = std::sqrt(1 + m00 - m11 - m22) * 2; q[3] = (m12 - m21) / s; q[0] = 0.25f * s; q[1] = (m01 + m10) / s; q[2] = (m20 + m02) / s; }
    else if (m11 > m22) { const float s = std::sqrt(1 + m11 - m00 - m22) * 2; q[3] = (m20 - m02) / s; q[0] = (m01 + m10) / s; q[1] = 0.25f * s; q[2] = (m12 + m21) / s; }
    else { const float s = std::sqrt(1 + m22 - m00 - m11) * 2; q[3] = (m01 - m10) / s; q[0] = (m20 + m02) / s; q[1] = (m12 + m21) / s; q[2] = 0.25f * s; }
}
/* 0x1424964f0: (m, out_scale, out_rotation, out_translation) */
void k_Matrix44Decompose(Call& c)
{
    const float* m = V(A(0)); float n[16]; std::memcpy(n, m, sizeof n); float s[3];
    for (int i = 0; i < 3; ++i) { s[i] = std::sqrt(m[i * 4] * m[i * 4] + m[i * 4 + 1] * m[i * 4 + 1] + m[i * 4 + 2] * m[i * 4 + 2]); for (int j = 0; j < 3; ++j) n[i * 4 + j] = m[i * 4 + j] / s[i]; }
    float* so = V(A(1)); so[0] = s[0]; so[1] = s[1]; so[2] = s[2];
    mat_to_quat(n, V(A(2)));
    float* t = V(A(3)); t[0] = m[12]; t[1] = m[13]; t[2] = m[14];
}
void k_InverseMatrix44(Call& c)
{
    const float* m = V(A(0)); float inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    float* o = V(A(1)); for (int i = 0; i < 16; ++i) o[i] = inv[i] / det;
}

/* 0x1405d8b80: euler (x about +X, y about +Y, z about +Z) -> qZ*qY*qX */
void k_EulerToQuaternion(Call& c)
{
    const float* e = V(A(0)); float* o = V(A(1));
    const float sx = std::sin(e[0] * 0.5f), cx = std::cos(e[0] * 0.5f), sy = std::sin(e[1] * 0.5f), cy = std::cos(e[1] * 0.5f),
                sz = std::sin(e[2] * 0.5f), cz = std::cos(e[2] * 0.5f);
    const float r[4] = { sx * cy * cz - cx * sy * sz, sx * cy * sz + cx * sy * cz, cx * cy * sz - sx * sy * cz, cx * cy * cz + sx * sy * sz };
    std::memcpy(o, r, sizeof r);
}
/* 0x1405d8f50: the inverse of the above, not normalizing its input */
void k_QuaternionToEuler(Call& c)
{
    const float* q = V(A(0)); float* o = V(A(1));
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float Aa = 2 * (w * x + y * z), Bb = 2 * (w * y - x * z), Cc = 2 * (w * z + x * y);
    const float D0 = 1 - 2 * (x * x + y * y), D2 = 1 - 2 * (y * y + z * z), cc = std::sqrt(Aa * Aa + D0 * D0);
    float r[4];
    if (cc > 0.001f) { r[0] = std::atan2(Aa, D0); r[1] = std::atan2(Bb, cc); r[2] = std::atan2(Cc, D2); r[3] = 0; }
    else { r[0] = std::atan2(2 * (w * x - y * z), 1 - 2 * (x * x + z * z)); r[1] = std::atan2(Bb, cc); r[2] = 0; r[3] = 0; }
    std::memcpy(o, r, sizeof r);
}
/* 0x1408fd460 / 0x1408fdba0: (angle, out_quat, out_normalized_angle) */
void k_RotateY(Call& c) { const float a = F(A(0)); float* q = V(A(1)); q[0] = 0; q[1] = std::sin(a * 0.5f); q[2] = 0; q[3] = std::cos(a * 0.5f); F(A(2)) = norm_pi(a); }
void k_RotateZ(Call& c) { const float a = F(A(0)); float* q = V(A(1)); q[0] = 0; q[1] = 0; q[2] = std::sin(a * 0.5f); q[3] = std::cos(a * 0.5f); F(A(2)) = norm_pi(a); }
/* 0x140f0c040 / 0x140f0c300: (a, b, t, out), out = (a - a t) + b t */
void k_InterpolateFloat(Call& c) { const float a = F(A(0)), b = F(A(1)), t = F(A(2)); F(A(3)) = (a - a * t) + b * t; }
void k_InterpolateFloat3(Call& c) { const float *a = V(A(0)), *b = V(A(1)); const float t = F(A(2)); float* o = V(A(3)); for (int k = 0; k < 4; ++k) o[k] = (a[k] - a[k] * t) + b[k] * t; }
/* 0x142495fd0 */
void k_RangeChange(Call& c)
{
    const float x = F(A(0)), il = F(A(1)), ih = F(A(2)), ol = F(A(3)), oh = F(A(4));
    const float d = ih - il;
    if (!(std::fabs(d) > 1.52587890625e-5f)) { F(A(5)) = ol; return; }
    float t = (x - il) / d;
    t = t > 0.f ? t : 0.f;            /* maxss: a NaN t becomes 0 */
    t = t < 1.f ? t : 1.f;
    F(A(5)) = (ol - ol * t) + oh * t;
}
void k_Sign(Call& c) { F(A(1)) = F(A(0)) < 0.f ? -1.f : 1.f; }                       /* 0x1405dd300 */
void k_RoundFloatFloat(Call& c) { F(A(1)) = std::round(F(A(0))); }                  /* halves away from zero */
void k_CeilingFloatFloat(Call& c) { F(A(1)) = std::ceil(F(A(0))); }
/* 0x1405dd7a0: b - a normalized to [-pi, pi) */
void k_AngleDelta(Call& c)
{
    const float v = (F(A(1)) - F(A(0))) + 3.1415927410125732f;
    const float k = std::floor(v * 0.15915493667125702f);
    F(A(2)) = (v - k * 6.2831854820251465f) - 3.1415927410125732f;
}

/* timers and ramps */
/* 0x1405dcc70: (up, initial, increment, decrement, lo, hi, reset, out, value, initialized) */
void k_IncDec(Call& c)
{
    F(A(7)) = 0;
    if (!B(A(9))) { F(A(8)) = F(A(1)); B(A(9)) = 1; }
    float v = F(A(8));
    v = B(A(0)) ? v + F(A(2)) : v - F(A(3));
    F(A(8)) = v;
    if (B(A(6))) { v = F(A(1)); F(A(8)) = v; }
    v = std::fmin(std::fmax(F(A(4)), v), F(A(5)));
    F(A(8)) = v; F(A(7)) = v;
}
/* 0x1405dc9b0: (trigger, duration:int, active, remaining_out:int, remaining:int state) */
void k_DurationBool(Call& c)
{
    I(A(3)) = 0;
    if (B(A(0))) I(A(4)) = I(A(1));
    if (I(A(4)) > 0) { B(A(2)) = 1; I(A(3)) = I(A(4)); I(A(4)) -= 1; }
    else B(A(2)) = 0;
}
/* 0x1405dcaf0: (trigger, duration_s, dt_ticks, active, remaining_out, remaining) */
void k_DurationBoolSeconds(Call& c)
{
    F(A(4)) = 0;
    if (B(A(0))) F(A(5)) = F(A(1));
    if (0.f < F(A(5))) {
        B(A(3)) = 1;
        float r = F(A(5)) - F(A(2)) * 0.016666668f;
        if (r < 0) r = 0;
        F(A(5)) = r; F(A(4)) = r;
    } else { B(A(3)) = 0; F(A(5)) = 0; F(A(4)) = 0; }
}
/* 0x1407912a0: (target4, smooth_time, dt_ticks, out4, initialized, position4, velocity4);
 * the maximum-change clamp is loaded as (0,0,0,0), the disabled sentinel */
void k_SpringDamper(Call& c)
{
    float *tgt = V(A(0)), *out = V(A(3)), *pos = V(A(5)), *vel = V(A(6));
    const float st = F(A(1)), dt = F(A(2));
    if (!B(A(4))) { for (int k = 0; k < 4; ++k) { pos[k] = tgt[k]; vel[k] = 0; } B(A(4)) = 1; }
    if (st != 0.f) {
        const float omega = std::fmax(1.f / st, 0.f);
        const float e = std::exp(-(omega * dt)), a = e * (1.f + omega * dt), cc = -e * omega * omega * dt;
        for (int k = 0; k < 4; ++k) {
            const float old = pos[k], ov = vel[k], d = old - tgt[k];
            pos[k] = tgt[k] + a * d + (e * dt) * ov;
            vel[k] = cc * d + (e - e * omega * dt) * ov;
        }
    }
    for (int k = 0; k < 4; ++k) out[k] = pos[k];
}
/* 0x1405da5c0 (read off the disassembly): (up, dt_ticks, in_time, out_time, out, value, initialized) */
void k_IncDecNormal(Call& c)
{
    F(A(4)) = 0;
    if (!B(A(6))) { F(A(5)) = 0; B(A(6)) = 1; }
    const float d = F(A(1)) * 0.016666668f;
    float v = B(A(0)) ? d / F(A(2)) + F(A(5)) : F(A(5)) - d / F(A(3));
    v = std::fmin(std::fmax(v, 0.f), 1.f);
    F(A(5)) = v; F(A(4)) = v;
}
/* 0x1405d9f10: (enable, duration_ticks, dt_ticks, reset, out, elapsed, initialized, latched) */
void k_DiceTimer(Call& c)
{
    F(A(4)) = 0;
    bool old_latched = false;
    if (!B(A(6)) || B(A(3))) { F(A(5)) = 0; B(A(6)) = 1; B(A(7)) = 0; }
    else old_latched = B(A(7)) != 0;
    if (B(A(0))) B(A(7)) = 1;
    if (B(A(0)) || old_latched)
        if (F(A(5)) < F(A(1))) F(A(5)) += F(A(2)) * 0.016666668f;
    F(A(4)) = std::fmin(F(A(1)), std::fmax(0.f, F(A(5))));
}
/* 0x1405ddf30: (delta_ticks, out_seconds, accumulated) */
void k_TimeInSeconds(Call& c) { F(A(2)) += F(A(0)); F(A(1)) = F(A(2)) * 0.016666668f; }
/* 0x1405de060, minus the rdtsc reseed - deterministic here on purpose */
void k_RandomFloatInRange(Call& c)
{
    const float lo = F(A(0)), hi = F(A(1));
    if (!B(A(2))) { F(A(3)) = F(A(6)); return; }
    uint32_t seed = *(uint32_t*)A(4);
    if (seed == 0xffffffffu || seed == 0) seed = 0xaaaaaaaau;
    const uint64_t u = (uint64_t)seed * 0x41c64e6dull + 0x3039ull;
    *(uint32_t*)A(4) = (uint32_t)u;
    F(A(5)) = (float)(uint32_t)(u >> 16) * 2.3283064e-10f;
    F(A(6)) = F(A(3)) = (hi - lo) * F(A(5)) + lo;
}
/* 0x1405daeb0: (trigger, target4, half_life, dt_ticks, out4, prev_trigger, initialized, output4, offset4, velocity4) */
void k_InertializationNode(Call& c)
{
    const bool trig = B(A(0)) != 0; float *tgt = V(A(1)), *out = V(A(4)), *o = V(A(7)), *off = V(A(8)), *vel = V(A(9));
    const float hl = F(A(2)), dt = std::fmax(F(A(3)), 1e-6f) * 0.016666668f;
    if (!B(A(6))) { for (int k = 0; k < 4; ++k) { o[k] = tgt[k]; off[k] = vel[k] = 0; } B(A(6)) = 1; }
    if (trig && !B(A(5))) for (int k = 0; k < 4; ++k) off[k] = o[k] - tgt[k];
    const float y = 1.3862944f / (hl + 1e-5f), ydt = y * dt;
    const float e = 1.f / (((0.235f * ydt + 0.48f) * ydt + 1) * ydt + 1);
    for (int k = 0; k < 4; ++k) {
        const float j0 = vel[k] + y * off[k];
        off[k] = (off[k] + j0 * dt) * e;
        vel[k] = (vel[k] - j0 * y * dt) * e;
        o[k] = tgt[k] + off[k]; out[k] = o[k];
    }
    B(A(5)) = trig;
}

/* SpringDampenFloat / Vec3 (0x1405dc4c0, 0x1405dc750; solver 0x1405dc110, wrap 0x1405dc0b0).
 * List form: ins (target, stiffness, damping, max_step, dt, lo, hi, reset, wrap), outs (pos),
 * contexts (pos state, vel state, initialized). */
float spring_wrap(float x, float lo, float hi) { return x < lo ? hi - std::fmod(lo - x, hi - lo) : std::fmod(x - lo, hi - lo) + lo; }
void spring(Call& c, int lanes)
{
    float* tgt = V(c.in[0]);
    const float stiff = F(c.in[1]), damp = F(c.in[2]), maxs = F(c.in[3]), dt = F(c.in[4]);
    float *lo = V(c.in[5]), *hi = V(c.in[6]);
    const bool reset = B(c.in[7]) != 0, wrap = B(c.in[8]) != 0;
    float *pos = V(c.ctx[0]), *vel = V(c.ctx[1]); uint8_t& init = B(c.ctx[2]);
    float* out = V(c.out[0]);
    if (!init || reset) { for (int k = 0; k < lanes; ++k) { pos[k] = tgt[k]; vel[k] = 0; } init = 1; }
    if (dt > 1.52587890625e-05f || dt < -1.52587890625e-05f) {
        const float h = dt * 0.01666666753590107f, kk = h * stiff * h, sq = std::sqrt(kk);
        const float a = std::fmax(1.f - kk, 0.f), b = std::fmax(1.f - (damp + damp) * sq, 0.f);
        for (int k = 0; k < lanes; ++k) {
            /* lo/hi are SCALARS for every lane, Vec3 included: the post-update program's
             * SpineX_Loco spring passes the constants -10000 and 10000, and reading them
             * as vectors took the next pool words as bounds and pinned y at 10000. */
            const float l = lo[0], u = hi[0];
            const bool ranged = std::fabs(u - l) > 1.1920928955078125e-07f;
            const float tc = ranged ? std::fmin(u, std::fmax(l, tgt[k])) : tgt[k];
            const float d = pos[k] - tc, bv = b * vel[k];
            const float vn = ((a - 1.f) * d) / h + bv;
            float delta = ((a * d + h * bv) + tc) - pos[k];
            delta = std::fmin(maxs, std::fmax(-maxs, delta));
            float np = delta + pos[k];
            if (ranged && wrap) np = spring_wrap(np, l, u);
            if (ranged) np = std::fmin(u, std::fmax(l, np));
            pos[k] = np; vel[k] = vn;
        }
    }
    for (int k = 0; k < lanes; ++k) out[k] = pos[k];
}
void k_SpringDampenFloat(Call& c) { spring(c, 1); }
void k_SpringDampenVec3(Call& c) { spring(c, 3); }

/* engine interfaces */
struct StateCtx { Host* host; };
/* OPERAND ORDER FROM THE SHIPPED CALL SITES, not the spec prose: a reader is
 * (context, handle) -> value (IBoolReader at pc 216 of the pre-update program reads
 * ins 1:5192 = the program's state-context slot, then 1:784 = an input's handle);
 * a writer is (value, context, handle). EX_VM_SPEC.md has context and handle swapped. */
void rdstate(Call& c, Host::Kind kind)
{
    StateCtx* s = *(StateCtx**)c.in[0];
    if (s && s->host) s->host->read_state(*(uint64_t*)c.in[1], c.out[0], kind);
}
void wrstate(Call& c, Host::Kind kind)
{
    StateCtx* s = *(StateCtx**)c.in[1];
    if (s && s->host) s->host->write_state(*(uint64_t*)c.in[2], c.in[0], kind);
}
void k_IBoolReader(Call& c) { rdstate(c, Host::Bool); }
void k_IFloatReader(Call& c) { rdstate(c, Host::Float); }
void k_IIntegerReader(Call& c) { rdstate(c, Host::Int); }
void k_IVector3Reader(Call& c) { rdstate(c, Host::Vec3); }
void k_IQuaternionReader(Call& c) { rdstate(c, Host::Quat); }
void k_IBoolWriter(Call& c) { wrstate(c, Host::Bool); }
void k_IFloatWriter(Call& c) { wrstate(c, Host::Float); }
void k_IIntegerWriter(Call& c) { wrstate(c, Host::Int); }
void k_IVector3Writer(Call& c) { wrstate(c, Host::Vec3); }
void k_IQuaternionWriter(Call& c) { wrstate(c, Host::Quat); }

/* 0x1408ee0e0 / 0x1408ee270 */
void k_DofReader(Call& c)
{
    Instance::PoseCtx* pc = *(Instance::PoseCtx**)c.in[0];
    const DofHandle h = rd<DofHandle>((const uint8_t*)c.in[1]);
    const uint8_t n = B(c.in[3]);
    const uint8_t* src = (const uint8_t*)c.in[2];
    if (pc && pc->pose && h.index != kUnbound && (size_t)h.index < pc->pose->valid.size() && pc->pose->valid[(size_t)h.index]
        && h.offset + n <= pc->pose->bytes.size())
        src = pc->pose->bytes.data() + h.offset;
    std::memmove(c.out[0], src, n);
}
/* 0x1408f43f0: DofReader without the validity check */
void k_ForceDofReader(Call& c)
{
    Instance::PoseCtx* pc = *(Instance::PoseCtx**)c.in[0];
    const DofHandle h = rd<DofHandle>((const uint8_t*)c.in[1]);
    const uint8_t n = B(c.in[3]);
    const uint8_t* src = (const uint8_t*)c.in[2];
    if (pc && pc->pose && h.index != kUnbound && h.offset + n <= pc->pose->bytes.size()) src = pc->pose->bytes.data() + h.offset;
    std::memmove(c.out[0], src, n);
}
void k_DofWriter(Call& c)
{
    Instance::PoseCtx* pc = *(Instance::PoseCtx**)c.in[0];
    const DofHandle h = rd<DofHandle>((const uint8_t*)c.in[1]);
    const uint8_t n = B(c.in[3]);
    if (!pc || !pc->pose || h.index == kUnbound || (size_t)h.index >= pc->pose->valid.size()
        || h.offset + n > pc->pose->bytes.size()) return;
    std::memcpy(pc->pose->bytes.data() + h.offset, c.in[2], n);
    pc->pose->valid[(size_t)h.index] = 0xff;
}

#undef A

const KernelImpl kKernels[] = {
    { "NegateFloat", k_NegateFloat, nullptr }, { "AbsoluteFloat", k_AbsoluteFloat, nullptr },
    { "Sin", k_Sin, nullptr }, { "CosFloat", k_CosFloat, nullptr }, { "TanFloat", k_TanFloat, nullptr },
    { "ATanFloat", k_ATanFloat, nullptr }, { "ArcSineFloat", k_ArcSineFloat, nullptr }, { "ArcCosineFloat", k_ArcCosineFloat, nullptr },
    { "DegreesToRadiansFloat", k_DegreesToRadiansFloat, nullptr }, { "RadiansToDegreesFloat", k_RadiansToDegreesFloat, nullptr },
    { "TicksToTime", k_TicksToTime, nullptr }, { "TicksToTime_60fps", k_TicksToTime, nullptr },
    { "AddFloat", k_AddFloat, nullptr }, { "SubtractFloat", k_SubtractFloat, nullptr },
    { "MultiplyFloatFloatFloat", k_MultiplyFloatFloatFloat, nullptr }, { "DivideFloatFloatFloat", k_DivideFloatFloatFloat, nullptr },
    { "MaxFloat", k_MaxFloat, nullptr }, { "MinFloat", k_MinFloat, nullptr }, { "PowFloat", k_PowFloat, nullptr },
    { "ModuloFloat", k_ModuloFloat, nullptr }, { "Atan2Float", k_Atan2Float, nullptr },
    { "AddInt", k_AddInt, nullptr }, { "SubtractInt", k_SubtractInt, nullptr },
    { "GreaterThanFloat", k_GreaterThanFloat, nullptr }, { "GreaterThanOrEqualsFloat", k_GreaterThanOrEqualsFloat, nullptr },
    { "LessThanFloat", k_LessThanFloat, nullptr }, { "LessThanOrEqualsFloat", k_LessThanOrEqualsFloat, nullptr },
    { "EqualsFloat", k_EqualsFloat, nullptr }, { "NotEqualsFloat", k_NotEqualsFloat, nullptr },
    { "EqualsInt", k_EqualsInt, nullptr }, { "NotEqualsInt", k_NotEqualsInt, nullptr },
    { "GreaterThanInt", k_GreaterThanInt, nullptr }, { "GreaterThanOrEqualsInt", k_GreaterThanOrEqualsInt, nullptr },
    { "LessThanInt", k_LessThanInt, nullptr }, { "LessThanOrEqualsInt", k_LessThanOrEqualsInt, nullptr },
    { "Not", k_Not, nullptr }, { "EqualsBool", k_EqualsBool, nullptr }, { "NotEqualsBool", k_NotEqualsBool, nullptr },
    { "And", k_AndN, nullptr }, { "And3", k_AndN, nullptr }, { "And4", k_AndN, nullptr }, { "And5", k_AndN, nullptr },
    { "Or", k_OrN, nullptr }, { "Or3", k_OrN, nullptr }, { "Or4", k_OrN, nullptr }, { "Or5", k_OrN, nullptr }, { "OrMulti", k_OrN, nullptr },
    { "BoolToFloat", k_BoolToFloat, nullptr }, { "IntToFloat", k_IntToFloat, nullptr }, { "ToInt32Float", k_ToInt32Float, nullptr },
    { "ClampFloat", k_ClampFloat, nullptr }, { "BetweenFloat", k_BetweenFloat, nullptr },
    { "BetweenOrEqualsFloat", k_BetweenOrEqualsFloat, nullptr }, { "BetweenInt", k_BetweenInt, nullptr },
    { "NotNearlyEqualsFloatFloat", k_NotNearlyEqualsFloatFloat, nullptr },
    { "AddFloat3", k_AddFloat3, nullptr }, { "SubtractFloat3", k_SubtractFloat3, nullptr },
    { "MultiplyFloat3Float3Float3", k_MultiplyFloat3Float3Float3, nullptr },
    { "MultiplyFloat3FloatFloat3", k_MultiplyFloat3FloatFloat3, nullptr },
    { "MultiplyFloatFloat3Float3", k_MultiplyFloatFloat3Float3, nullptr },
    { "DivideFloat3FloatFloat3", k_DivideFloat3FloatFloat3, nullptr }, { "NegateFloat3", k_NegateFloat3, nullptr },
    { "TanFloat3", k_TanFloat3, nullptr }, { "ATanFloat3", k_ATanFloat3, nullptr },
    { "DotFloat3", k_DotFloat3, nullptr }, { "MagnitudeFloat3", k_MagnitudeFloat3, nullptr },
    { "NormalizeFloat3", k_NormalizeFloat3, nullptr }, { "EqualsFloat3", k_EqualsFloat3, nullptr }, { "ClampFloat3", k_ClampFloat3, nullptr },
    { "MultiplyQuaternionQuaternionQuaternion", k_MultiplyQuaternionQuaternionQuaternion, nullptr },
    { "InverseQuaternion", k_InverseQuaternion, nullptr }, { "EqualsQuaternion", k_EqualsQuaternion, nullptr },
    { "TransformQuaternion", k_TransformQuaternion, nullptr }, { "OrientationToLocalAxes", k_OrientationToLocalAxes, nullptr },
    { "InterpolateQuaternion", k_InterpolateQuaternion, nullptr }, { "RotationDecompose", k_RotationDecompose, nullptr },
    { "NormalizeAngleMinusPiToPi", k_NormalizeAngleMinusPiToPi, nullptr },
    { "NormalizeAngleZeroToTwoPi", k_NormalizeAngleZeroToTwoPi, nullptr }, { "AngleAroundYQuat", k_AngleAroundYQuat, nullptr },
    { "MultiplyFloat44Float44Float44", k_MultiplyFloat44Float44Float44, nullptr }, { "matrixnmulta", k_matrixnmulta, nullptr },
    { "Matrix44Compose", k_Matrix44Compose, nullptr }, { "Matrix44Decompose", k_Matrix44Decompose, nullptr },
    { "InverseMatrix44", k_InverseMatrix44, nullptr }, { "InverseFloat44", k_InverseMatrix44, nullptr },
    { "IncDecNormal", k_IncDecNormal, nullptr }, { "DiceTimer", k_DiceTimer, nullptr }, { "TimeInSeconds", k_TimeInSeconds, nullptr },
    { "RandomFloatInRange", k_RandomFloatInRange, nullptr }, { "InertializationNode", k_InertializationNode, nullptr },
    { "SpringDampenFloat", nullptr, k_SpringDampenFloat }, { "SpringDampenVec3", nullptr, k_SpringDampenVec3 },
    { "IBoolReader", nullptr, k_IBoolReader }, { "IFloatReader", nullptr, k_IFloatReader },
    { "IIntegerReader", nullptr, k_IIntegerReader }, { "IVector3Reader", nullptr, k_IVector3Reader },
    { "IQuaternionReader", nullptr, k_IQuaternionReader },
    { "IBoolWriter", nullptr, k_IBoolWriter }, { "IFloatWriter", nullptr, k_IFloatWriter },
    { "IIntegerWriter", nullptr, k_IIntegerWriter }, { "IVector3Writer", nullptr, k_IVector3Writer },
    { "IQuaternionWriter", nullptr, k_IQuaternionWriter },
    { "DofReader", nullptr, k_DofReader }, { "DofWriter", nullptr, k_DofWriter }, { "ForceDofReader", nullptr, k_ForceDofReader },
    { "EulerToQuaternion", k_EulerToQuaternion, nullptr }, { "QuaternionCompose", k_EulerToQuaternion, nullptr },
    { "QuaternionToEuler", k_QuaternionToEuler, nullptr }, { "RotateY", k_RotateY, nullptr }, { "RotateZ", k_RotateZ, nullptr },
    { "InterpolateFloat", k_InterpolateFloat, nullptr }, { "InterpolateFloat3", k_InterpolateFloat3, nullptr },
    { "RangeChange", k_RangeChange, nullptr }, { "Sign", k_Sign, nullptr }, { "RoundFloatFloat", k_RoundFloatFloat, nullptr },
    { "CeilingFloatFloat", k_CeilingFloatFloat, nullptr }, { "AngleDelta", k_AngleDelta, nullptr },
    { "IncDec", k_IncDec, nullptr }, { "DurationBool", k_DurationBool, nullptr },
    { "DurationBoolSeconds", k_DurationBoolSeconds, nullptr }, { "SpringDamper", k_SpringDamper, nullptr },
};

/* Every kernel name the programs are known to call, implemented or not, so a
 * missing one is reported by NAME. */
const char* const kKnownNames[] = {
    "IncDec", "TuningCurveExp", "Sign", "DurationBool", "DurationBoolSeconds", "RangeChange", "RoundFloatFloat",
    "CeilingFloatFloat", "AngleDelta", "EulerToQuaternion", "QuaternionCompose", "QuaternionToEuler", "RotateY", "RotateZ",
    "ForceDofReader", "SpringDamper", "InterpolateFloat", "InterpolateFloat3", "AngleBetweenQuaternion", "DiceAim",
    "DiceDelay", "InertializationBlendNode", "VerletDICEPlanarCollision", "AntJointReader", "RootJointReader",
    "MultiplyQuaternionFloatQuaternion",
};

const std::unordered_map<uint32_t, const KernelImpl*>& registry()
{
    static std::unordered_map<uint32_t, const KernelImpl*> m;
    if (m.empty()) for (const KernelImpl& k : kKernels) m[kernel_hash(k.name)] = &k;
    return m;
}
std::string name_of(uint32_t h)
{
    if (auto it = registry().find(h); it != registry().end()) return it->second->name;
    for (const char* n : kKnownNames) if (kernel_hash(n) == h) return n;
    char b[16]; std::snprintf(b, sizeof b, "%08x", h); return b;
}

}  // namespace

/* ------------------------------------------------------------------ program */

bool Program::open_image(std::string& err)
{
    const auto& b = image;
    if (b.size() < 0x50) { err = "program image too small"; return false; }
    cbase =((uint32_t)(u16(b, 0x3e) + u16(b, 0x40) + u16(b, 0x42)) * 4 + 0x5f) & ~0xfu;
    code = (u32(b, 0x24) + 3 + cbase) & ~3u;
    clen = u32(b, 0x2c) * 4;
    h20 = u32(b, 0x20); h28 = u32(b, 0x28); h30 = u32(b, 0x30); h34 = u32(b, 0x34); h44 = b[0x44];
    const uint32_t n38 = u16(b, 0x38), n3a = u16(b, 0x3a), n3c = u16(b, 0x3c);
    const size_t relC = (size_t)code + clen + 8u * (n38 + n3a);
    if (relC + 8ull * n3c > b.size()) { err = "kernel relocation table past the image"; return false; }
    call_sites.clear();
    for (uint32_t i = 0; i < n3c; ++i) call_sites[u32(b, relC + 8 * i + 4)] = u32(b, relC + 8 * i);
    return true;
}

std::vector<std::string> Program::missing_kernels() const
{
    std::set<std::string> out;
    for (const auto& [pc, h] : call_sites) if (!registry().count(h)) out.insert(name_of(h));
    return { out.begin(), out.end() };
}

namespace {
int64_t ivv(const bf6::EbxValue* v)
{
    if (!v) return 0;
    return v->kind == bf6::EbxValue::Kind::Int ? v->i : (int64_t)(int32_t)(uint32_t)v->u;
}
}  // namespace

bool load_program(bf6_ctx* c, const std::string& asset, Program& out, std::string& err)
{
    bf6ant::Graph g(c);
    const bf6ant::Obj* ex = g.root(asset.c_str());
    if (!ex) { err = "no asset " + asset; return false; }
    if (!ex->f(0x5db2cfc4u)) {
        const bf6ant::Obj* p = g.resolve(ex->f(0x20280736u), ex);
        if (!p) p = g.resolve(ex->f(0x2bf03460u), ex);
        if (p) ex = p;
    }
    const bf6::EbxValue* img = ex->f(0x5db2cfc4u);
    if (!img) { err = "no program image in " + ex->path; return false; }
    out = Program{};
    out.path = ex->path;
    for (const auto& r : img->items)
        for (uint32_t h : { 0x3901db14u, 0x42fc0f5eu, 0x32a99b9cu, 0x7c8062f2u }) {
            const uint32_t v = (uint32_t)ivv(r.field(h));
            for (int k = 0; k < 4; ++k) out.image.push_back((uint8_t)(v >> (8 * k)));
        }
    if (!out.open_image(err)) return false;
    const bf6::EbxValue* in = ex->f(0x202e54f7u);
    const bf6::EbxValue* sl = ex->f(0x246d8578u);
    const bf6::EbxValue* key = ex->f(0x653bfba0u);
    if (in && sl)
        for (size_t i = 0; i < in->items.size() && 2 * i + 1 < sl->items.size(); ++i) {
            const bf6ant::Obj* o = g.resolve(&in->items[i], ex);
            Program::Input I;
            I.path = o ? o->path : std::string();
            I.a = (uint32_t)ivv(&sl->items[2 * i]); I.b = (uint32_t)ivv(&sl->items[2 * i + 1]);
            I.key = key && i < key->items.size() ? (uint32_t)ivv(&key->items[i]) : 0;
            out.inputs.push_back(I);
        }
    if (const bf6::EbxValue* t = ex->f(0x68b8a237u))
        for (size_t i = 0; i + 5 < t->items.size(); i += 6) {
            Program::Seed s;
            s.hash = (uint32_t)ivv(&t->items[i]); s.kind = (uint32_t)ivv(&t->items[i + 1]);
            s.a = (uint32_t)ivv(&t->items[i + 2]); s.b = (uint32_t)ivv(&t->items[i + 3]);
            s.da = (uint32_t)ivv(&t->items[i + 4]); s.db = (uint32_t)ivv(&t->items[i + 5]);
            out.seeds.push_back(s);
        }
    auto pair = [&](uint32_t fa, uint32_t fb, uint32_t* dst) {
        dst[0] = (uint32_t)ivv(ex->f(fa)); dst[1] = (uint32_t)ivv(ex->f(fb));
        if (!ex->f(fa) || !ex->f(fb)) dst[0] = dst[1] = ~0u;
    };
    pair(0x571dbcbcu, 0x1cb250dcu, out.slot_ctx0);
    pair(0x147c2fa3u, 0x2316973cu, out.slot_ctx1);
    pair(0x5d4fe63fu, 0xd4c91e35u, out.slot_dt);
    pair(0x6d9f1155u, 0x01770188u, out.slot_ctx2);
    pair(0x8369f5a7u, 0xd1dc3818u, out.slot_ctx3);
    return true;
}

/* ----------------------------------------------------------------- instance */

uint8_t* Instance::addr(uint32_t a, uint32_t b)
{
    switch (a) {
    case 0: return konst.data() + p->cbase + b;
    case 1: return mem.data() + r1 + b;
    case 2: return mem.data() + r2 + b;
    default: {
        const size_t k = (size_t)a - 3;
        return k < ext.size() ? (uint8_t*)(uintptr_t)ext[k] + b : nullptr;
    }
    }
}

bool Instance::init(const Program& prog, Host* h, PoseArena* arena, std::string& err)
{
    p = &prog; host = h; pose = arena;
    konst = prog.image;
    /* FUN_142486be0: the serialized initial instance image sits after the code and
     * the three relocation tables, 16-aligned; its first h20 bytes become the block. */
    const auto& b = prog.image;
    const uint32_t n38 = u16(b, 0x38), n3a = u16(b, 0x3a), n3c = u16(b, 0x3c);
    const uint64_t init_off = ((uint64_t)prog.code + prog.clen + 8ull * (n38 + n3a + n3c) + 0xf) & ~0xfull;
    table = align16(prog.h20);
    r2 = table + align16((uint64_t)prog.h30 * 8);
    mem.assign((size_t)r2 + prog.h28 + 64, 0);
    if (init_off + prog.h20 > b.size()) { err = "initial instance image past the program"; return false; }
    std::memcpy(mem.data(), b.data() + init_off, prog.h20);
    std::memset(mem.data(), 0, 0x20);                                  /* header: program ptr, lock, depth, flags */
    *(uint32_t*)(mem.data() + 0x18) = 3;                                /* flags: embedded + persistent (created with 3) */
    r1 = (uint32_t)(((uint64_t)prog.h44 + (uint64_t)prog.h34 * 2) * 4 + 0x2f) & ~0xfu;
    stack_at = 0x20 + 8 * prog.h34;
    ext.assign(prog.h30 > 3 ? prog.h30 - 3 : 0, 0);
    pose_ctx.pose = arena;
    state_ctx.host = h;
    /* FUN_1408ee7b0: each input's handle into its slot */
    for (const Program::Input& in : prog.inputs) {
        uint8_t* d = addr(in.a, in.b);
        if (!d) continue;
        const uint64_t hv = host ? host->bind_state(in.path, in.key) : 0;
        std::memcpy(d, &hv, 8);
    }
    bind_seeds();
    return true;
}

void Instance::bind_seeds()
{
    /* FUN_1408ee560 */
    for (const Program::Seed& s : p->seeds) {
        DofHandle hd{ kUnbound, ~0u };
        int32_t idx = kUnbound;
        if (pose) { auto it = pose->by_hash.find(s.hash); if (it != pose->by_hash.end()) idx = it->second; }
        if (idx != kUnbound && (size_t)idx < pose->offset.size()) hd = DofHandle{ idx, pose->offset[(size_t)idx] };
        if (uint8_t* d = addr(s.a, s.b)) std::memcpy(d, &hd, 8);
        if (s.db != ~0u && idx != kUnbound)
            if (uint8_t* d = addr(s.da, s.db))
                std::memcpy(d, pose->bytes.data() + hd.offset, pose->width[(size_t)idx]);
    }
}

namespace {
struct Op { uint32_t a, b; };
Op opnd(const uint8_t* p) { return { rd<uint32_t>(p), rd<uint32_t>(p + 4) }; }
}  // namespace

bool Instance::run(float dt_ticks, std::string& err, uint32_t entry, uint64_t max_steps)
{
    const Program& P = *p;
    /* FUN_1408f23b0: the special host values */
    auto put = [&](const uint32_t* s, const void* v, size_t n) { if (s[1] != ~0u) if (uint8_t* d = addr(s[0], s[1])) std::memcpy(d, v, n); };
    PoseCtx* pcp = &pose_ctx; StateCtx* scp = reinterpret_cast<StateCtx*>(&state_ctx);
    const uint64_t zero = 0;
    put(P.slot_ctx0, &pcp, 8);
    put(P.slot_ctx1, &zero, 8);
    put(P.slot_dt, &dt_ticks, 4);
    put(P.slot_ctx2, &scp, 8);
    put(P.slot_ctx3, &zero, 8);

    const uint8_t* code = konst.data() + P.code;
    auto R = [&](const uint8_t* at) { const Op o = opnd(at); return addr(o.a, o.b); };
    uint32_t pc = entry;
    uint32_t& depth = *(uint32_t*)(mem.data() + 0x14);
    uint32_t* stack = (uint32_t*)(mem.data() + stack_at);
    std::vector<void*> ins, outs, ctxs;
    static const int kFixed[10][2] = { {1,1},{1,2},{1,3},{1,4},{2,0},{2,1},{3,0},{3,1},{4,0},{4,1} };
    for (uint64_t step = 0; step < max_steps; ++step) {
        if (pc + 4 > P.clen) { err = "pc outside the code"; return false; }
        const uint8_t* I = code + pc;
        const uint32_t w = rd<uint32_t>(I);
        const uint32_t op = w & 0xff, next = w >> 8;
        ++steps;
        /* BF6_EX_TRACE=N: the first N instructions of each run, to stderr */
        static const long trace_n = std::getenv("BF6_EX_TRACE") ? std::atol(std::getenv("BF6_EX_TRACE")) : 0;
        if ((long)step < trace_n) {
            auto site = P.call_sites.find(pc);
            std::fprintf(stderr, "ex %6u op %02x next %6u %s\n", pc, op, next,
                         site != P.call_sites.end() ? name_of(site->second).c_str() : "");
        }
        auto call = [&](Call& c) {
            auto site = P.call_sites.find(pc);
            const KernelImpl* k = nullptr;
            if (site != P.call_sites.end()) { auto it = registry().find(site->second); if (it != registry().end()) k = it->second; }
            c.inst = this;
            if (!k) { notes.push_back("no kernel " + (site != P.call_sites.end() ? name_of(site->second) : std::string("?"))); return; }
            KernelFn f = c.a ? k->direct : k->list;
            if (!f) { notes.push_back(std::string("kernel ") + k->name + " called in the other form"); return; }
            f(c);
        };
        if (op <= 0x13) {
            ins.clear();
            for (uint32_t i = 0; i <= op; ++i) ins.push_back(R(I + 0xc + 8 * i));
            Call c; c.a = ins.data(); c.n = (int)ins.size();
            call(c);
            pc = next; continue;
        }
        if (op <= 0x1d) {
            const int ni = kFixed[op - 0x14][0], no = kFixed[op - 0x14][1];
            ins.clear(); outs.clear();
            for (int i = 0; i < ni; ++i) ins.push_back(R(I + 0xc + 8 * (size_t)i));
            for (int i = 0; i < no; ++i) outs.push_back(R(I + 0xc + 8 * (size_t)(ni + i)));
            Call c; c.in = ins.data(); c.nin = ni; c.out = outs.data(); c.nout = no;
            call(c);
            pc = next; continue;
        }
        switch (op) {
        case 0x1e: case 0x1f: case 0x20: case 0x21: case 0x22: {
            static const size_t W[5] = { 1, 2, 4, 8, 16 };
            std::memmove(R(I + 0xc), R(I + 4), W[op - 0x1e]); pc = next; break;
        }
        case 0x23: {
            ins.clear(); outs.clear(); ctxs.clear();
            const uint8_t* q = I + 0xc;
            const uint32_t ni = rd<uint32_t>(q); q += 4;
            for (uint32_t i = 0; i < ni; ++i, q += 8) ins.push_back(R(q));
            const uint32_t no = rd<uint32_t>(q); q += 4;
            for (uint32_t i = 0; i < no; ++i, q += 8) outs.push_back(R(q));
            const uint32_t nc = rd<uint32_t>(q); q += 4;
            for (uint32_t i = 0; i < nc; ++i, q += 8) ctxs.push_back(R(q));
            Call c; c.in = ins.data(); c.nin = (int)ni; c.out = outs.data(); c.nout = (int)no; c.ctx = ctxs.data(); c.nctx = (int)nc;
            call(c);
            pc = next; break;
        }
        case 0x24: case 0x31: case 0x3e:
            notes.push_back("typed dispatch opcode " + std::to_string(op) + " not implemented");
            pc = next; break;
        case 0x25: std::memmove(R(I + 0xc), R(I + 4), rd<uint32_t>(I + 0x14)); pc = next; break;
        case 0x26: pc = *R(I + 4) == 0 ? rd<uint32_t>(I + 0xc) : next; break;
        case 0x27: pc = *(uint32_t*)R(I + 4); break;
        case 0x28: {
            const uint32_t sel = *(uint32_t*)R(I + 4), n = rd<uint32_t>(I + 0xc);
            uint32_t to = next;
            for (uint32_t i = 0; i < n; ++i)
                if (rd<uint32_t>(I + 0x10 + 4 * i) == sel) { to = rd<uint32_t>(I + 0x10 + 4 * (size_t)(n + i)); break; }
            pc = to; break;
        }
        case 0x29: stack[depth++] = next; pc = rd<uint32_t>(I + 4); break;
        case 0x2a: {
            uint8_t* latch = R(I + 8);
            if (*latch) { stack[depth++] = next; *latch = 0; pc = rd<uint32_t>(I + 4); }
            else pc = next;
            break;
        }
        case 0x2b:
            if (depth == 0) { err = "return with an empty VM stack"; return false; }
            pc = stack[--depth]; break;
        case 0x2c: return true;
        case 0x2e: { const uint32_t k = rd<uint32_t>(I + 0xc); if (k < ext.size() + 3 && k >= 3) ext[k - 3] = *(uint64_t*)R(I + 4); pc = next; break; }
        case 0x2f: { uint8_t* s = R(I + 4); std::memcpy(R(I + 0xc), &s, 8); pc = next; break; }
        case 0x30: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: {
            uint8_t* base = *(uint8_t**)R(I + 4);
            const uint32_t off = rd<uint32_t>(I + 0x14);
            static const size_t W[5] = { 1, 2, 4, 8, 16 };
            const size_t n = op == 0x30 ? rd<uint32_t>(I + 0x18) : W[op - 0x32];
            if (base) std::memmove(base + off, R(I + 0xc), n);
            pc = next; break;
        }
        case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: {
            static const size_t W[5] = { 1, 2, 4, 8, 16 };
            const size_t n = op == 0x38 ? rd<uint32_t>(I + 0x24) : W[op - 0x39];
            std::memmove(R(I + 0x1c), *R(I + 4) ? R(I + 0xc) : R(I + 0x14), n);
            pc = next; break;
        }
        default: pc = next; break;   /* reserved: follow next_pc */
        }
    }
    err = "step limit reached";
    return false;
}

}  // namespace bf6ex
