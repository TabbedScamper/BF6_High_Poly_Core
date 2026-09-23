#include "vehicle_wheel_ops.h"

#include <cmath>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace bf6 {
namespace expression {
namespace {

const uint32_t kTyreForce = 0x45A17BD8u;   /* thunk 147EE4600 -> FUN_1443F3F20 */
const uint32_t kBodyMass  = 0xF6003A7Du;   /* thunk 147EE00A0 -> FUN_1443E3CD0 */
const uint32_t kWheelRay  = 0x76B098FFu;   /* thunk 147EE4130 -> FUN_1443F2D80 -> FUN_1443F1370 */
const uint32_t kSuspension = 0x8C83D835u;  /* thunk 147EE35E0 -> FUN_1443ED130 */
const uint32_t kContactForce = 0x602F941Du; /* thunk 147EE4A40 -> FUN_1443F4840 */
const uint32_t kGravity   = 0x43223158u;   /* thunk 147EE21A0 -> FUN_1443E7F40 */
const uint32_t kGetCom    = 0x75311A22u;   /* thunk 147EDF970 -> FUN_1443E37D0 */
const uint32_t kSetCom    = 0x71C3128Fu;   /* thunk 147EDF890 -> FUN_1443E3740 */
const uint32_t kLocalGravity = 0x95C847CFu; /* thunk 147EE20C0 -> FUN_1443E79D0 */
const uint32_t kForceAtPos = 0x9855872Eu;  /* thunk 147EE2280 -> FUN_1443E7FF0 */
const uint32_t kAutoBrake = 0xEEB0D8D7u;   /* thunk 147EE4D40 -> FUN_1443F57A0 */
const uint32_t kStandStill = 0xEF25EE38u;  /* thunk 147EE4B80 -> FUN_1443F50C0 */
const uint32_t kBuoyancy  = 0x3C90753Cu;   /* thunk 147EE3DB0 -> FUN_1443EEF00 */
const uint32_t kAeroDrag  = 0xD6C99B22u;   /* thunk 147EE2690 -> FUN_1443E9430 -> FUN_1443E7400 */

/* TRACKED VEHICLES. The road-wheel contact sampler: the same ray as a car wheel,
 * run once per road wheel along the track (FUN_1443F3670 -> FUN_1443F1370). */
const uint32_t kTrackContacts = 0xC10D20ACu;
/* and the suspension pass over the contacts it produced (FUN_1443EDD10): the same
 * spring the car uses, run per contact, with the nearest one reported back. */
const uint32_t kTrackSuspension = 0x1E25FE26u;
/* and the pass that gives every track section a contact (FUN_1443EE870). */
const uint32_t kTrackShare = 0xF6E57070u;

/* BOATS AND HELICOPTERS: a KEYED curve, seven floats per key, reached through a
 * pointer the loader relocates (1479DD310 -> FUN_1405626E0). */
const uint32_t kCurveKeyed = 0x9AFB0561u;

/* BOATS: the whole hull in one call (simulateBoatHullNew, FUN_1443E1130 ->
 * FUN_1443E1960 to build the surface, FUN_1443E30C0 to simulate it). */
const uint32_t kBoatHull = 0x115A4FF4u;
/* and the probe that gives it a water plane under three corners (FUN_1443EF6C0). */
const uint32_t kWaterPlane = 0xD257C4ACu;
/* FUN_1443EFC30: THE WATER HEIGHT AT ONE POINT. Two operands, a position and the
 * height out. The native lifts the point by 1000 and asks every water probe in the
 * world for its surface, keeps the LOWEST that is above the world's own water
 * fallback, and when every probe misses (-3.4028235e+38) substitutes -1024.0, which
 * is the same no-water answer its sibling water-plane probe gives. Offline the
 * host's surface is one flat height, so the minimum is that height. */
const uint32_t kWaterHeightAt = 0xED79777Au;
const float kNoWater = -1024.0f;
/* FUN_1443E9D10: MOTION DAMPING, both velocities in one call.
 *
 * Per axis, in the frame its coefficients are in: the velocity decays by
 * exp(-10 * dt * k), the decayed vector is clamped to a maximum length, and what
 * comes out is the ACCELERATION that produces it, (damped - v) / dt. A default
 * maximum is substituted when the graph passes zero - 100 for the first, 20 for the
 * second - and a velocity with any non-finite lane makes the whole answer zero.
 *
 * WHICH VELOCITY IS WHICH IS THE ONE THING NOT MEASURED HERE. The native takes them
 * out of the physics context at two offsets; the first is rotated into the
 * coefficients' frame and rotated back afterwards, the second is not, and the
 * defaults (100 against 20) read like a speed against a rate. This host hands the
 * graph both velocities in the body frame already, so that round trip is a no-op and
 * only the pairing matters. It is stated at the call below rather than buried. */
const uint32_t kMotionDamping = 0x9EB2D5CEu;
/* FUN_143B254B0: A STRUCT BUILT FROM ITS ARGUMENTS.
 *
 * Its i-th argument goes at the i-th entry of an offsets array the loader hands it,
 * and for a struct that array is the type's own field table. The body also walks an
 * entity handle, but only to fetch a context word for its copy helpers - the bulk
 * source is the output type's default, and nothing is read out of a live object. So
 * this is servable, which an earlier reading of the same body denied.
 *
 * The caller measures the field table (the hosts have no type database) and hands it
 * over keyed by field count, which the record itself states: a helicopter's rotor
 * config declares fifteen, the record's count constant is 0xF, and the one field wide
 * enough to need a reference is the one argument passed as a bound register. */
/* D7D1BAB3, which the reflected registry names (Position, TerrainPosition,
 * TerrainHeight): a ray straight down onto the ground, which is what every wheel here
 * already casts. Its outputs come in that order, so the primary - the last operand -
 * is the height, with the point on the terrain as the extra before it. */
const uint32_t kHeliRotor = 0x76A51E81u;   /* simulateHelicopterEngine */
const uint32_t kTerrainAt = 0xD7D1BAB3u;
const uint32_t kStructBuild = 0x8B226FBBu;
const uint32_t kStructBuildLead = 4;   /* type, count, offsets, views - then the fields */

/* SHARED ACROSS CLASSES, transcribed from the shipped code (studies muse_shared,
 * muse_boat). Small, and each one unblocks more than one class. */
const uint32_t kInertia   = 0x7D5AFD1Au;  /* 1443E3BA0: (1,1,1,0) / the body's inverse inertia */
const uint32_t kCopy16    = 0xAACD8D3Au;  /* 1424A30E0: movups [rdx], [rcx]                    */
const uint32_t kDownRay   = 0xDD5EABF8u;  /* 1443F3CD0 -> FUN_1443F10C0: a vertical down ray   */
const uint32_t kIdIsNot   = 0xF0F74455u;  /* 147EED690: *p != the dword at 0x149771B48         */
const uint32_t kClearFlag = 0xE4BE428Cu;  /* 142C791E0: clears a word on an engine object      */
const uint32_t kNotify    = 0x6D0E5290u;  /* 144343710: forwards to a virtual, returns nothing */

/* What kIdIsNot compares against: read out of .rdata at 0x149771B48 (the exe the
 * corpus was built from, md5 6a1c1b). It is an id, not a sentinel, so the operator
 * is a type test and not a validity test - which is why this is a named constant
 * rather than a nullptr check. */
const uint32_t kIdConstant = 0x07BD0BC4u;

void qrot(const float q[4], const float v[3], float out[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2.0f * (y * v[2] - z * v[1]);
    const float ty = 2.0f * (z * v[0] - x * v[2]);
    const float tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

/* WheelConfig / WheelFrictionConfig / WheelContact byte offsets (reflection). */
enum : uint32_t {
    WC_RADIUS = 0x5C, WC_INERTIA = 0x60, WC_FRICTION = 0x10,
    WF_TORQUE_MULT = 0x00, WF_TOTAL_LAT = 0x04, WF_TOTAL_LONG = 0x08, WF_LAT_NEGK = 0x0C,
    WF_LONG_NEGK = 0x10, WF_ANGVEL_MIN = 0x14, WF_LAT_SLIP = 0x18, WF_VELX_MIN = 0x1C,
    WF_LAT_MAX = 0x20, WF_LAT_BRAKE = 0x24, WF_LONG_SLIP = 0x28, WF_BRAKE_LAT = 0x2C,
    WF_BRAKE_LONG = 0x30, WF_LAT_POSK = 0x34, WF_SLIPRATIO_MAX = 0x38, WF_LONG_POSK = 0x3C,
    WF_SIDESLIP_MAX = 0x40, WF_LONG_MAX = 0x44,
    CT_SURFVEL = 0x00, CT_NORMAL = 0x10, CT_POSITION = 0x20, CT_HASCONTACT = 0x3C,
};

float rf(const Value& v, uint32_t off) {
    float x = 0.0f;
    if (off + 4 <= v.bytes.size()) std::memcpy(&x, v.bytes.data() + off, 4);
    return x;
}
uint32_t ru(const Value& v, uint32_t off) {
    uint32_t x = 0;
    if (off + 4 <= v.bytes.size()) std::memcpy(&x, v.bytes.data() + off, 4);
    return x;
}
void wf(std::vector<uint8_t>& b, uint32_t off, float x) { std::memcpy(b.data() + off, &x, 4); }
void wu(std::vector<uint8_t>& b, uint32_t off, uint32_t x) { std::memcpy(b.data() + off, &x, 4); }

/* sign as the natives spell it: -1 below 0, else 1, and 0 when |x| <= -0 (x == 0). */
float sgn(float x) {
    float s = x < 0.0f ? -1.0f : 1.0f;
    if (std::fabs(x) <= -0.0f) s = 0.0f;
    return s;
}

/* The per-wheel state block the tyre native passes around (&local_968): material
 * handle +0x00, Load +0x08, contact-point velocity +0x10, clamped dot +0x20,
 * lateral speed +0x24, longitudinal speed +0x28, tyre frame +0x30 (longitudinal
 * axis) and +0x40 (lateral axis), friction torque +0x50. */
struct WheelState {
    uint64_t material = 0;
    float load = 0.0f;
    float vel[4] = {0, 0, 0, 0};
    float dot_clamped = 0.0f, v_lat = 0.0f, v_long = 0.0f;
    float long_axis[4] = {0, 0, 0, 0}, lat_axis[4] = {0, 0, 0, 0};
    float friction_torque = 0.0f;
};

/* The slip-state slots (&local_a08): status (int), wheel omega, window 5 (int),
 * slip-ratio running sum, window 3 (int), slip-angle running sum. The decompile's
 * int casts on the float slots are mistypings: the only conversions in the natives
 * are cvtsi2ss of the two window counts. */
struct SlipSlots {
    int32_t status = 0;
    float omega = 0.0f;
    uint32_t ratio_window = 5;
    float ratio_sum = 0.0f;
    uint32_t angle_window = 3;
    float angle_sum = 0.0f;
};

/* The chassis snapshot the natives build (&local_908), the fields their callees
 * read: mass +0x00, 1/mass +0x04, v +0x10, w +0x20, centre of mass +0xB0, inverse
 * inertia diagonal +0xD0. */
struct Snapshot {
    float mass = 0.0f, inv_mass = 0.0f;
    float v[4] = {0, 0, 0, 0}, w[4] = {0, 0, 0, 0};
    float com[4] = {0, 0, 0, 0}, inv_i[4] = {0, 0, 0, 0};
};

/* FUN_1443ECFF0: one force record applied to the snapshot, NaN/inf gated. */
bool finite3(const float* p) {
    for (int i = 0; i < 3; ++i) {
        uint32_t u; std::memcpy(&u, &p[i], 4);
        if ((u & 0x7f800000u) == 0x7f800000u) return false;
    }
    return true;
}
void apply_record(Snapshot& s, const float* f, const float* p) {
    if (!finite3(f) || !finite3(p)) return;
    float nv[3] = {s.inv_mass * f[0] + s.v[0], s.inv_mass * f[1] + s.v[1], s.inv_mass * f[2] + s.v[2]};
    if (finite3(nv)) {
        s.v[0] = nv[0]; s.v[1] = nv[1]; s.v[2] = nv[2];
        s.v[3] = s.inv_mass * f[3] + s.v[3];
    }
    const float r0 = p[0] - s.com[0], r1 = p[1] - s.com[1], r2 = p[2] - s.com[2];
    float nw[3] = {(r1 * f[2] - f[1] * r2) * s.inv_i[0] + s.w[0],
                   (r2 * f[0] - f[2] * r0) * s.inv_i[1] + s.w[1],
                   (r0 * f[1] - f[0] * r1) * s.inv_i[2] + s.w[2]};
    if (finite3(nw)) { s.w[0] = nw[0]; s.w[1] = nw[1]; s.w[2] = nw[2]; }
}

/* FUN_1443EBAD0's first loop over the records FUN_1443F05E0 pushed: each record
 * whose force magnitude is non-zero is applied. */
struct ForceRecord { float f[4]; float p[4]; };
void apply_all(Snapshot& s, const std::vector<ForceRecord>& recs) {
    for (const ForceRecord& r : recs) {
        const float m = std::sqrt(r.f[0] * r.f[0] + r.f[1] * r.f[1] + r.f[2] * r.f[2]);
        if (m != 0.0f) apply_record(s, r.f, r.p);
    }
}

/* FUN_1443F0870: tyre frame and slips. steer rotates local +Z about +Y by -steer
 * (quaternion (0, sin(-steer/2), 0, cos(-steer/2))); the engine evaluates that
 * sin/cos with a 13th-order polynomial, here libm (differences below 1e-7). */
void tyre_frame(float radius, float steer, float omega, const float* n, WheelState& st,
                float* slip_angle, float* slip_ratio) {
    const float h = steer * -0.5f;
    const float s = std::sin(h), c = std::cos(h);
    const float qx = s * 0.0f, qy = s * 1.0f, qz = s * 0.0f, qw = c;
    /* t = w v + q x v for v = (0,0,1); fwd = v + 2 (t x q) as the native orders it */
    const float t0 = qw * 0.0f + (qy * 1.0f - qz * 0.0f);
    const float t1 = qw * 0.0f + (qz * 0.0f - qx * 1.0f);
    const float t2 = qw * 1.0f + (qx * 0.0f - qy * 0.0f);
    const float c2 = t1 * qx - qy * t0;
    const float c0 = t2 * qy - qz * t1;
    const float c1 = t0 * qz - qx * t2;
    const float fx = c0 + c0 + 0.0f, fy = c1 + c1 + 0.0f, fz = c2 + c2 + 1.0f;
    const float lx = fz * 1.0f - fy * 0.0f, ly = fx * 0.0f - fz * 0.0f, lz = fy * 0.0f - fx * 1.0f;
    const float vx = st.vel[0], vy = st.vel[1], vz = st.vel[2];
    float vn[3] = {vx, vy, vz};
    const float vv = vx * vx + vy * vy + vz * vz;
    if (1e-06f < vv) { const float m = std::sqrt(vv); vn[0] = vx / m; vn[1] = vy / m; vn[2] = vz / m; }
    const float d = fz * n[2] + fy * n[1] + fx * n[0];
    float lg[4] = {fx - d * n[0], fy - d * n[1], fz - d * n[2], 0.0f - d * n[3]};
    float LA[4] = {0, 0, 0, 0};
    const float lgl = lg[1] * lg[1] + lg[0] * lg[0] + lg[2] * lg[2];
    if (1e-06f < lgl) { const float m = std::sqrt(lgl); for (int i = 0; i < 4; ++i) LA[i] = lg[i] / m; }
    const float d2 = ly * n[1] + lx * n[0] + lz * n[2];
    float TA[4] = {0, 0, 0, 0};
    const float lal = LA[1] * LA[1] + LA[0] * LA[0] + LA[2] * LA[2];
    if (1e-06f < lal) {
        const float m = std::sqrt(lal);
        TA[0] = (lx - d2 * n[0]) / m; TA[1] = (ly - d2 * n[1]) / m; TA[2] = (lz - d2 * n[2]) / m;
        TA[3] = (0.0f - d2 * n[3]) / m;
    }
    std::memcpy(st.long_axis, LA, 16);
    std::memcpy(st.lat_axis, TA, 16);
    st.v_long = LA[1] * vy + LA[0] * vx + LA[2] * vz;
    const float avl = std::fabs(st.v_long);
    float vl = TA[1] * vy + TA[0] * vx + TA[2] * vz;
    st.v_lat = vl;
    vl = avl <= 2.0f ? vl * 0.1f : vl / avl;
    *slip_angle = std::atan(vl);
    float dc = TA[1] * vn[1] + TA[0] * vn[0] + TA[2] * vn[2];
    float sr = radius * omega - st.v_long;
    if (dc <= -1.0f) dc = -1.0f;
    if (1.0f <= dc) dc = 1.0f;
    st.dot_clamped = dc;
    *slip_ratio = avl <= 2.0f ? sr * 0.1f : sr / avl;
}

/* FUN_1443F0090: combined-slip tyre curve. The load table at 0x1489FBEF0 has 20
 * rows of 4 floats, read from the executable: lanes +0x00 / +0x04 normalise the
 * slips (0.40..0.57 / 0.30..0.43), +0x08 / +0x0C are the lateral / longitudinal
 * peak gains (1.026..1.038 / 1.071..1.104); the row index is load/mass-driven and
 * clamped to 18 so row+1 stays inside. */
float g_table[20][4];
bool g_table_ok = false;

void combined_slip(float k_long, float k_lat, float inv_mass, float load, float ratio, float angle,
                   const Value& fric, uint32_t fb, float brake, float* out_long, float* out_lat) {
    const bool neg = ratio < 0.0f;
    if (neg) ratio = -ratio;
    float ta = std::tan(angle);
    if (1.1920929e-07f <= std::fabs(ratio + 1.0f)) {
        const float r = 1.0f / (ratio + 1.0f);
        ratio = r * ratio;
        ta = r * ta;
    }
    float nr = -ratio;
    const float L = inv_mass * 1200.0f * load;
    int i = (int)(L * 0.002f);
    int row = 0x12;
    if (i < 0x12) row = i;
    if (row < 0) row = 0;                                  /* the native does not clamp below */
    const float fr = L * 0.002f - (float)row;
    const float* a = g_table[row];
    const float* b = g_table[row + 1];
    float p4 = (b[1] - a[1]) * fr + a[1];
    float p0 = (b[0] - a[0]) * fr + a[0];
    if (1.1920929e-07f <= std::fabs(p4 + 1.0f)) {
        const float r = 1.0f / (p4 + 1.0f);
        p0 = p0 * r;
        p4 = r * p4;
    }
    p4 = -p4;
    float u = nr;
    if (1.1920929e-07f <= std::fabs(p4)) u = nr / p4;
    float w = ta;
    if (1.1920929e-07f <= std::fabs(p0)) w = ta / p0;
    const float mag = std::sqrt(std::fabs(w * w + u * u));
    float sr = p4 * mag * sgn(nr);
    if (1.1920929e-07f <= std::fabs(sr + 1.0f)) sr = sr / (sr + 1.0f);
    float st = -1.0f;
    if (0.0f <= ta) st = 1.0f;
    if (std::fabs(ta) <= -0.0f) st = 0.0f;
    const float sa = std::atan(mag * p0 * st);
    float fl = 0.0f, ft = 0.0f;
    if (0.0f < load) {
        float k = (1.75f - L * 0.0001f) * rf(fric, fb + WF_LONG_POSK) + 5.0f;
        if (k <= 1.0f) k = 1.0f;
        float e = std::exp(-std::fabs(k * sr));
        fl = ((1.0f - e) - std::fabs(sr) * rf(fric, fb + WF_LONG_NEGK)) * ((b[3] - a[3]) * fr + a[3]);
        if (fl <= 0.0f) fl = 0.0f;
        if (sr <= 0.0f) fl = -fl;
        k = (1.75f - L * 0.0001f) * rf(fric, fb + WF_LAT_POSK) + 5.0f;
        if (k <= 1.0f) k = 1.0f;
        e = std::exp(-std::fabs(k * sa));
        ft = ((1.0f - e) - std::fabs(sa) * rf(fric, fb + WF_LAT_NEGK)) * ((b[2] - a[2]) * fr + a[2]);
    }
    fl = k_long * load * fl;
    ft = k_lat * load * ft;
    float cl = mag;
    if (1.0f <= mag) cl = 1.0f;
    float fl2 = fl;
    if (1.1920929e-07f <= mag) {
        u = (1.0f / mag) * u;
        w = (1.0f / mag) * w;
        fl2 = fl - (fl - ft) * cl * w * w;
        ft = ft - (ft - fl) * cl * u * u;
    }
    float m2 = std::sqrt(std::fabs(nr * nr + ta * ta));
    if (1.1920929e-07f <= m2) { m2 = 1.0f / m2; nr = m2 * nr; ta = m2 * ta; }
    nr = nr * fl2;
    *out_lat = ta * ft;
    *out_long = -nr;
    if (neg) {
        *out_long = nr;
        if (0.0f < brake) {
            const float bl = rf(fric, fb + WF_BRAKE_LAT);
            *out_long = nr * rf(fric, fb + WF_BRAKE_LONG);
            *out_lat = ta * ft * bl;
        }
    }
}

/* FUN_1443F0D40: grip/slide state machine, then the curve, then the force clamp.
 * Surface (material) scaling applies only when the contact resolved a material
 * handle; offline it never does, which is the native's own no-material path. */
void friction(const Value& fric, uint32_t fb, float mass, float brake, const WheelState& st,
              SlipSlots& sl, float* out_long, float* out_lat) {
    const float F = 2.25f;
    float k5 = rf(fric, fb + WF_LONG_SLIP) * F * rf(fric, fb + WF_TOTAL_LONG);
    float k6 = rf(fric, fb + WF_TOTAL_LONG) * F;
    float k4 = rf(fric, fb + WF_TOTAL_LAT) * F;
    float k3 = rf(fric, fb + WF_LAT_SLIP) * F * rf(fric, fb + WF_TOTAL_LAT);
    float k7 = k5, k8 = k3;
    const float ravg = sl.ratio_sum / (float)sl.ratio_window;
    const float aavg = sl.angle_sum / (float)sl.angle_window;
    if (sl.status == 1) {
        const float om = sl.omega;
        bool slide;
        bool a = om <= 0.0f || rf(fric, fb + WF_ANGVEL_MIN) * 0.017453292f < std::fabs(om);
        if (!a) { k7 = k6; k8 = k4; a = om <= 0.0f; }
        if (a) {
            slide = rf(fric, fb + WF_SIDESLIP_MAX) * 0.017453292f * st.load * k4 * 0.00049999997f < std::fabs(aavg) ||
                    rf(fric, fb + WF_VELX_MIN) * 0.2777778f * k4 < std::fabs(st.v_lat);
            if (!slide) {
                k7 = k6; k8 = k4;
                slide = st.load * k6 * rf(fric, fb + WF_SLIPRATIO_MAX) * 0.0001f < std::fabs(ravg);
            }
        } else slide = false;
        if (slide) { sl.status = 2; k7 = k5; k8 = k3; }
    } else if (sl.status == 2) {
        if (std::fabs(aavg) < st.load * k3 * rf(fric, fb + WF_SIDESLIP_MAX) * 8.726645e-06f &&
            std::fabs(st.v_lat) < rf(fric, fb + WF_VELX_MIN) * 0.2777778f * k3 &&
            std::fabs(ravg) < st.load * k5 * 0.0001f * rf(fric, fb + WF_SLIPRATIO_MAX)) {
            sl.status = 1; k7 = k6; k8 = k4;
        }
    } else {
        sl.status = 2;
    }
    combined_slip(k7, (brake * rf(fric, fb + WF_LAT_BRAKE) + (1.0f - brake)) * k8, 1.0f / mass, st.load,
                  ravg, aavg, fric, fb, brake, out_long, out_lat);
    const float lat_max = mass * rf(fric, fb + WF_LAT_MAX);
    const float long_max = mass * rf(fric, fb + WF_LONG_MAX);
    float x = -long_max;
    if (-long_max <= *out_long) x = *out_long;
    if (long_max <= x) x = long_max;
    *out_long = x;
    x = -lat_max;
    if (-lat_max <= *out_lat) x = *out_lat;
    if (lat_max <= x) x = lat_max;
    *out_lat = x;
}

/* Running slip sum: S = x + S (1 - 1/N), reset when the sign flips. */
void run_sum(float& sum, uint32_t window, float x) {
    const float avg = (1.0f / (float)window) * sum;
    if (sgn(avg) != sgn(x)) sum = 0.0f;
    sum = x + (sum - (1.0f / (float)window) * sum);
}


/* ---- the suspension (FUN_1443ED130 and its helpers) ----
 * SuspensionConfig (0x54 bytes, a graph constant like the WheelConfig; no reflected
 * layout, fields named by what the natives do with them). */
enum : uint32_t {
    SC_RAMP_END_DEG = 0x00, SC_ANTIROLL = 0x04, SC_ANGLE_LIMIT_DEG = 0x08,
    SC_RATE_LOW_COMP = 0x0C, SC_KNEE_COMP = 0x10, SC_GROWTH_THRESH = 0x14, SC_TRAVEL = 0x18,
    SC_GROWTH_BASE = 0x1C, SC_KNEE_REBOUND = 0x24, SC_CLAMP_REBOUND = 0x28, SC_CLAMP_COMP = 0x2C,
    SC_RATE_HIGH_REBOUND = 0x30, SC_RATE_HIGH_COMP = 0x34, SC_RATE_LOW_REBOUND = 0x38,
    SC_MAXFORCE_SCALE = 0x40, SC_TOP_MOUNT = 0x44, SC_BLOWOFF = 0x48,
    SC_VELOCITY_TRAVEL = 0x4C, SC_FORCE_AT_CONTACT_OFF = 0x4D, SC_CHECK_60HZ = 0x4E,
    SC_TWO_STAGE = 0x4F, SC_SCALE_MAXFORCE = 0x50, SC_ALWAYS_NORMAL = 0x51, SC_CLAMP_VELOCITY = 0x52,
};
uint8_t rb(const Value& v, uint32_t off) { return off < v.bytes.size() ? v.bytes[off] : 0; }

/* FUN_1443DF8F0: spring force with optional log/exp stiffening past a threshold. */
float spring_growth(float k, const Value& sc, float c) {
    const float th = rf(sc, SC_GROWTH_THRESH);
    float g = 1.0f;
    if (0.0f < th && th < c) {
        g = std::log(rf(sc, SC_GROWTH_BASE));
        g = std::exp(g * ((c - th) / th));
    }
    return g * k * c * rf(sc, SC_TRAVEL);
}

/* FUN_1443DF970 (checked against the disassembly: both early exits return 0, and
 * the tail is gate * (own - other) * ramp): the ANTI-ROLL term, active only while
 * the chassis moves faster than 1 m/s at a drift angle past the limit. The heading
 * is the horizontal velocity turned by 0.1 s of yaw. */
float anti_roll(float k, const Value& sc, const Snapshot& s, float compr_own, float compr_other) {
    const float gate = rf(sc, SC_ANTIROLL);
    if (!(0.0f < gate)) return 0.0f;
    const float spd = std::sqrt(s.v[1] * s.v[1] + s.v[0] * s.v[0] + s.v[2] * s.v[2]);
    if (!(1.0f < spd)) return 0.0f;
    const float h2 = s.v[0] * s.v[0] + 0.0f + s.v[2] * s.v[2];
    const float inv = 1.0f / std::sqrt(h2);      /* rsqrtps + 2 Newton steps in the native */
    const float dx = inv * s.v[0], dy = inv * 0.0f, dz = inv * s.v[2];
    const float h = s.w[1] * -0.05f;
    const float sn = std::sin(h), cs = std::cos(h);
    const float qx = sn * 0.0f, qy = sn * 1.0f, qz = sn * 0.0f, qw = cs;
    const float t0 = dx * qw + (qy * dz - dy * qz);
    const float t1 = dy * qw + (qz * dx - dz * qx);
    const float t2 = dz * qw + (qx * dy - dx * qy);
    const float u2 = qx * t1 - qy * t0;
    const float u0 = qy * t2 - qz * t1;
    const float u1 = qz * t0 - qx * t2;
    float dot = (u1 + u1 + dy) * 0.0f + (u0 + u0 + dx) * 0.0f + (u2 + u2 + dz) * 1.0f;
    if (dot <= -1.0f) dot = -1.0f;
    if (1.0f <= dot) dot = 1.0f;
    const float ang = std::fabs(std::acos(dot)) * 57.29578f;
    const float lim = rf(sc, SC_ANGLE_LIMIT_DEG);
    if (!(lim < ang)) return 0.0f;
    const float a_other = spring_growth(k, sc, compr_other);
    const float a_own = spring_growth(k, sc, compr_own);
    const float diff = a_own - a_other;
    const float range = rf(sc, SC_RAMP_END_DEG) - lim;
    float t = 0.0f;
    if (range >= 9.99999997e-07f) {
        t = (ang - lim) / range;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
    }
    return gate * diff * t;
}

/* 1/dt rounded half away from zero is 60: the 60 Hz case of FUN_1443DFDF0. */
bool is_60hz(float inv_dt) {
    const float a = std::fabs(inv_dt);
    float r = std::nearbyint(a);
    if (a - r == 0.5f) r += 1.0f;
    return (int)std::copysign(r, inv_dt) == 0x3c;
}

/* FUN_1443DFDF0: spring force (returned), damper force (*damper) and the
 * compression fraction (*compr), from the contact against the wheel's rest height. */
float spring_damper(float dt, const Value& W, float k, float d, const Value& sc, const Snapshot& s,
                    const Value& C, float compr_prev, float* damper, float* compr, uint8_t* high) {
    float L = rf(sc, SC_TRAVEL);
    const float P[3] = {rf(C, CT_POSITION), rf(C, CT_POSITION + 4), rf(C, CT_POSITION + 8)};
    const float SV[3] = {rf(C, CT_SURFVEL), rf(C, CT_SURFVEL + 4), rf(C, CT_SURFVEL + 8)};
    const float N[3] = {rf(C, CT_NORMAL), rf(C, CT_NORMAL + 4), rf(C, CT_NORMAL + 8)};
    float x = (((rf(sc, SC_TOP_MOUNT) + rf(W, 4)) - rf(W, WC_RADIUS)) + L) - P[1];
    if (x <= 0.0f) x = 0.0f;
    if (L <= x) x = L;
    float c = (L - x) / L;
    *compr = c;
    const float rx = P[0] - s.com[0], ry = P[1] - s.com[1], rz = P[2] - s.com[2];
    const float Lt = rf(sc, SC_TRAVEL);
    float vy = ((rx * s.w[2] - s.w[0] * rz) + s.v[1]) - SV[1];   /* contact-point velocity, y */
    float cc = c;
    if (rb(sc, SC_VELOCITY_TRAVEL)) {
        float x2 = vy * dt + x;
        if (x2 <= 0.0f) x2 = 0.0f;
        if (Lt <= x2) x2 = Lt;
        cc = ((Lt - x2) / Lt + c) * 0.5f;
    }
    const float th = rf(sc, SC_GROWTH_THRESH);
    float g = 1.0f;
    if (0.0f < th && th < cc) {
        g = std::log(rf(sc, SC_GROWTH_BASE));
        g = std::exp(g * ((cc - th) / th));
    }
    const float spring = g * k * cc * Lt;
    const float sp = std::sqrt(s.v[1] * s.v[1] + s.v[0] * s.v[0] + s.v[2] * s.v[2]);
    if (2.0f < sp) {
        const float nx = s.v[0] / sp, ny = s.v[1] / sp, nz = s.v[2] / sp;
        const float along = nx * N[0];
        if (std::fabs(ny * N[1] + along + nz * N[2]) < 0.05f) {
            /* moving along the ground: the damper sees the point velocity on the normal */
            const float px = ((rz * s.w[1] - s.w[2] * ry) + s.v[0]) - SV[0];
            const float pz = ((ry * s.w[0] - s.w[1] * rx) + s.v[2]) - SV[2];
            vy = (pz * N[2] + vy * N[1] + px * N[0]) * N[1];
        }
    }
    if (!rb(sc, SC_VELOCITY_TRAVEL)) {
        vy = (compr_prev - c) / dt;
        if (rb(sc, SC_CLAMP_VELOCITY)) {
            const float lim = 0.0f < vy ? rf(sc, SC_CLAMP_REBOUND) : -rf(sc, SC_CLAMP_COMP);
            if (std::fabs(lim) < std::fabs(vy)) vy = lim;
        }
    } else if (vy < 0.0f) {
        const float bo = rf(sc, SC_BLOWOFF);
        if (bo < -vy) {
            float f = s.v[1];
            float m = 1.0f;
            if (f <= 0.0f) { f = std::fabs(f / vy); m = 1.0f - f; }
            vy = (-vy - bo) * m + vy;
        }
    }
    const float inv_dt = 1.0f / dt;
    const float sign = (!rb(sc, SC_CHECK_60HZ) || !is_60hz(inv_dt)) ? -1.0f : -0.54f;
    *high = 0;
    float rate;
    if (!rb(sc, SC_TWO_STAGE)) {
        rate = 0.0f < vy ? rf(sc, SC_RATE_LOW_REBOUND) : rf(sc, SC_RATE_LOW_COMP);
    } else if (0.0f < vy) {
        if (rf(sc, SC_KNEE_REBOUND) <= vy) { rate = rf(sc, SC_RATE_HIGH_REBOUND); *high = 1; }
        else rate = rf(sc, SC_RATE_LOW_REBOUND);
    } else if (rf(sc, SC_KNEE_COMP) <= std::fabs(vy)) {
        rate = rf(sc, SC_RATE_HIGH_COMP); *high = 1;
    } else {
        rate = rf(sc, SC_RATE_LOW_COMP);
    }
    const float f = vy * sign * rate * inv_dt * d * s.mass;
    const float lo = rb(sc, SC_SCALE_MAXFORCE) ? -(spring * rf(sc, SC_MAXFORCE_SCALE)) : -spring;
    *damper = lo <= f ? f : lo;
    return spring;
}

/* The world up axis's y component, from the body orientation: the native gates the
 * anti-roll term off (no contact) when the car is past 84 degrees of roll/pitch. */
float up_y(const float q[4]) {
    const float x = q[0], z = q[2];
    return 1.0f - 2.0f * (x * x + z * z);
}

} // namespace

/* ONE WHEEL RAY, FUN_1443F1370, in vehicle-local space: from the attachment point
 * plus (0, AttachOffsetY - Radius + SpringLength + AdditionalRayLength, 0) down to
 * plus (0, AttachOffsetY - Radius, 0), queried in the world and written back local.
 * A miss leaves Normal (0,1,0), the tag words at +0x30 and HasContact 0. Static
 * ground has no surface velocity and resolves no material, so those stay 0, which
 * is the native's own path for a hit that is not a body.
 *
 * Shared, because the tank's track sampler is this run once per road wheel. */
void WheelOps::cast_wheel_ray(const float at[3], float radius, float spring,
                              float attach, float extra, std::vector<uint8_t>& out) {
    const float y0 = (attach - radius) + at[1];
    const float from_l[3] = {extra * 0.0f + at[0] + 0.0f, extra * 1.0f + spring + y0,
                             extra * 0.0f + at[2] + 0.0f};
    const float to_l[3] = {at[0], y0, at[2]};
    float fw[3], tw[3];
    qrot(body_.quat, from_l, fw);
    qrot(body_.quat, to_l, tw);
    double from[3], to[3], hit[3] = {0, 0, 0}, nrm[3] = {0, 1, 0};
    for (int i = 0; i < 3; ++i) { from[i] = fw[i] + body_.pos[i]; to[i] = tw[i] + body_.pos[i]; }
    out.assign(0x40, 0);
    wf(out, CT_NORMAL + 4, 1.0f);
    wf(out, 0x30, 1.469367e-39f);
    wf(out, 0x34, 9.18341e-41f);
    ++rays_;
    if (std::getenv("BF6_RAY_DEBUG"))
        std::fprintf(stderr, "ray local (%.2f %.2f %.2f) -> (%.2f %.2f %.2f) world y %.2f -> %.2f "
                     "(body y %.2f radius %.2f attach %.2f spring %.2f extra %.2f)\n",
                     from_l[0], from_l[1], from_l[2], to_l[0], to_l[1], to_l[2], from[1], to[1],
                     body_.pos[1], radius, attach, spring, extra);
    if (!ray_ || !ray_(ray_user_, from, to, hit, nrm)) return;
    ++ray_hits_;
    const float qc[4] = {-body_.quat[0], -body_.quat[1], -body_.quat[2], body_.quat[3]};
    const float hw[3] = {(float)hit[0] - body_.pos[0], (float)hit[1] - body_.pos[1],
                         (float)hit[2] - body_.pos[2]};
    const float nw[3] = {(float)nrm[0], (float)nrm[1], (float)nrm[2]};
    float hl[3], nl[3];
    qrot(qc, hw, hl);
    qrot(qc, nw, nl);
    for (int i = 0; i < 3; ++i) {
        wf(out, CT_POSITION + 4 * i, hl[i]);
        wf(out, CT_NORMAL + 4 * i, nl[i]);
    }
    out[CT_HASCONTACT] = 1;
    /* THE SURFACE ID. A hit always carries one in the game, and graphs test it: the
     * flyer60 gates its tyre on HasContact (+0x3C), thebeast on this field (+0x38).
     * There is no material database offline, so this is a STAND-IN id, positive so
     * the native's own material lookup (which needs the runtime surface tables)
     * stays off, exactly as it does for an unresolved material. */
    wu(out, 0x38, 1u);
}

/* FUN_1405626E0, the keyed-curve evaluation, over `n` keys of seven floats:
 * x, mode, in-tangent x, in-tangent y, out-tangent x, y, out-tangent y.
 *
 * Modes 1, 2 and 3 are smoothstep, linear and a step at the halfway point. Mode 0
 * is a cubic solved by Newton and is NOT served: the decompiler reassociates that
 * arm and a curve built from a misread expression is a wrong number with no
 * symptom. False means exactly that, and the caller says so. */
bool curve_eval_keyed(const float* k, uint32_t n, float x, float& y) {
    auto K = [&](uint32_t i, uint32_t f) { return k[(size_t)i * 7 + f]; };
    if (n == 0) return false;
    if (n == 1) { y = K(0, 5); return true; }
    if (x < K(0, 0) && K(0, 1) != 0.0f) { y = K(0, 5); return true; }
    if (K(n - 1, 0) < x) {
        if (K(n - 1, 1) == 0.0f && std::fabs(K(n - 1, 2)) > 1e-06f)
            y = ((x - K(n - 1, 0)) * K(n - 1, 6)) / K(n - 1, 2) + K(n - 1, 5);
        else
            y = K(n - 1, 5);
        return true;
    }
    uint32_t i = 0;
    int lo = 0, hi = (int)n - 2;
    while (lo <= hi) {
        i = (uint32_t)((lo + hi) / 2);
        if (x == K(i, 0)) break;
        if (K(i, 0) <= x) { lo = (int)i + 1; i = (uint32_t)lo; }
        else { hi = (int)i - 1; i = (uint32_t)(hi < 0 ? 0 : hi); }
    }
    if (i > n - 2) i = n - 2;
    const float x0 = K(i, 0), x1 = K(i + 1, 0);
    if (x1 - x0 <= 0.0f) { y = (K(i + 1, 5) + K(i, 5)) * 0.5f; return true; }
    const float t = (x - x0) / (x1 - x0);
    const float mode = K(i, 1);
    uint32_t m = 0;
    std::memcpy(&m, &mode, 4);
    if (m == 1u) { y = (3.0f - (t + t)) * (K(i + 1, 5) - K(i, 5)) * t * t + K(i, 5); return true; }
    if (m == 2u) { y = (K(i + 1, 5) - K(i, 5)) * t + K(i, 5); return true; }
    if (m == 3u) { y = t >= 0.5f ? K(i + 1, 5) : K(i, 5); return true; }
    return false;
}

bool WheelOps::describe(uint32_t key, OperatorSignature& out) {
    out = OperatorSignature{};
    switch (key) {
    case kTyreForce:
        /* dt, steer, brake, torque, WheelContact, Load, omega, status, ratio sum,
         * angle sum, HandlingOffset, LongitudinalHandlingOffset, WheelConfig ->
         * dv/dt, dw/dt, omega, status, ratio sum, angle sum, slip ratio, slip angle,
         * longitudinal force (primary) */
        out.input_widths = {4, 4, 4, 4, 0x40, 4, 4, 4, 4, 4, 16, 16, 0x70};
        out.extra_output_widths = {16, 16, 4, 4, 4, 4, 4, 4};
        out.output_width = 4;
        return true;
    case kBodyMass:
        out.output_width = 4;
        return true;
    case kSuspension:
        /* dt, WheelContact, HandlingOffset, WheelConfig, SpringK, SpringD,
         * SuspensionConfig, compression previous (own, other), IsFrontWheel ->
         * dv/dt, dw/dt, normal force, compression (primary) */
        out.input_widths = {4, 0x40, 16, 0x70, 4, 4, 0x54, 4, 4, 1};
        out.extra_output_widths = {16, 16, 4};
        out.output_width = 4;
        return true;
    case kGravity:
    case kGetCom:
    case kLocalGravity:
        out.output_width = 16;
        return true;
    case kStandStill:
        /* dt, allowed, LinearAcceleration, AngularAcceleration, SurfaceVelocity,
         * SurfaceVelocityPreviousFrame, NumWheels, NumWheelsWithContact, DampingFactor,
         * StandStillDampingConfig (0x30) -> factor, dv/dt, dw/dt (primary) */
        out.input_widths = {4, 1, 16, 16, 16, 16, 4, 4, 4, 0x30};
        out.extra_output_widths = {4, 16};
        out.output_width = 16;
        return true;
    case kAeroDrag:
        /* dt, ForcePositionOffset, Drag, OffsetYZ, OffsetXZ, OffsetXY, WindVelocity
         * -> dv/dt, dw/dt (primary) */
        out.input_widths = {4, 16, 16, 16, 16, 16, 16};
        out.extra_output_widths = {16};
        out.output_width = 16;
        return true;
    case kBuoyancy:
        /* dt, Buoyancy, config -> dv/dt, dw/dt (primary) */
        out.input_widths = {4, 4, 0x44};
        out.extra_output_widths = {16};
        out.output_width = 16;
        return true;
    case kAutoBrake:
        /* dt, throttle, brake, steer, gear, AutoBrakeConfig (0x10), auto brake in -> out */
        out.input_widths = {4, 4, 4, 4, 4, 0x10, 4};
        out.output_width = 4;
        return true;
    case kForceAtPos:
        /* dt, Position, Force -> dv/dt, dw/dt (primary) */
        out.input_widths = {4, 16, 16};
        out.extra_output_widths = {16};
        out.output_width = 16;
        return true;
    case kSetCom:
        /* one Vec3 input, no output: a side effect on the body */
        out.input_widths = {16};
        return true;
    case kContactForce:
        /* dt, WheelContact, HandlingOffset, WheelConfig -> force, 2 Vec4 (primary last) */
        out.input_widths = {4, 0x40, 16, 0x70};
        out.extra_output_widths = {16, 16};
        out.output_width = 16;
        return true;
    case kWheelRay:
        /* WheelConfig, SpringLength, AttachOffsetY, AdditionalRayLength -> WheelContact */
        out.input_widths = {0x70, 4, 4, 4};
        out.output_width = 0x40;
        return true;
    case kInertia:
        /* no inputs: it reads the body it is bound to */
        out.output_width = 16;
        return true;
    case kCopy16:
        out.input_widths = {16};
        out.output_width = 16;
        return true;
    case kDownRay:
        /* point (local), ray length -> a surface id, 0 for nothing under it */
        out.input_widths = {16, 4};
        out.output_width = 4;
        return true;
    case kIdIsNot:
        out.input_widths = {4};
        out.output_width = 1;
        return true;
    case kClearFlag:
        /* no inputs and no outputs: an engine-side flag this host does not keep */
        return true;
    case kNotify:
        /* one input, nothing out: a call into the engine with no value */
        out.input_widths = {4};
        return true;
    case kWaterHeightAt:
        /* a position -> the water height there */
        out.input_widths = {16};
        out.output_width = 4;
        return true;
    case kHeliRotor:
        /* dt, collective, cyclic pitch and roll, a ground flag, a gravity modifier and
         * the 176-byte rotor config -> linear then angular acceleration */
        out.input_widths = {4, 4, 4, 4, 1, 4, 176};
        out.extra_output_widths = {16};
        out.output_width = 16;
        return true;
    case kTerrainAt:
        out.input_widths = {16};
        out.extra_output_widths = {16};
        out.output_width = 4;
        return true;
    case kStructBuild:
        /* CLAIMED SO THE CHAIN ROUTES IT HERE. The real shape needs the field count
         * the record states as a constant, which only describe_call is given, so this
         * is a routing claim and nothing else: if describe_call cannot find a layout
         * for that count it refuses, and the VM refuses the call with it. */
        out.input_widths.assign(kStructBuildLead, 4u);
        out.output_width = 4;
        return true;
    case kMotionDamping:
        /* dt, two per-axis coefficient vectors, two maximum lengths -> two
         * accelerations; the primary is the second, as the record's last slot */
        out.input_widths = {4, 16, 16, 4, 4};
        out.extra_output_widths = {16};
        out.output_width = 16;
        return true;
    case kWaterPlane:
        /* half width, length, centre, the body matrix, a layer -> hit flag, plane */
        out.input_widths = {4, 4, 16, 64, 4};
        out.extra_output_widths = {1};
        out.output_width = 16;
        return true;
    case kBoatHull:
        /* dt, hull description (12 floats), hull physics (12 floats), water plane A,
         * water plane B, substep count, a flag -> submerged fraction, and the
         * velocity of two body points (the last is the primary) */
        out.input_widths = {4, 0x30, 0x30, 16, 16, 4, 1};
        out.extra_output_widths = {4, 16};
        out.output_width = 16;
        return true;
    case kCurveKeyed:
        /* x and the curve. The native dereferences the operand and reads +0x18 of
         * what it finds, so the slot may hold the 40-byte asset itself or a handle
         * to it - both shapes ship, and the op below takes whichever is there. */
        out.input_widths = {4, 0x28};
        out.output_width = 4;
        return true;
    case kTrackShare:
        /* three groups of (contact, load, force, WheelConfig, sampler) in, three
         * groups of (contact, load, force) out; the last force is the primary */
        out.input_widths = {0x40, 4, 4, 0x70, 0x24, 0x40, 4, 4, 0x70, 0x24,
                            0x40, 4, 4, 0x70, 0x24};
        out.extra_output_widths = {0x40, 4, 4, 0x40, 4, 4, 0x40, 4};
        out.output_width = 4;
        return true;
    case kTrackSuspension:
        /* dt, contact array, HandlingOffset, WheelConfig, SpringK, SpringD,
         * SuspensionConfig, extra float -> dv/dt, dw/dt, the chosen contact, and
         * the force (primary) */
        out.input_widths = {4, 8, 16, 0x70, 4, 4, 0x54, 4};
        out.extra_output_widths = {16, 16, 0x40};
        out.output_width = 4;
        return true;
    case kTrackContacts:
        /* WheelConfig, SpringLength, AttachOffsetY, AdditionalRayLength, sampler
         * (start Vec4, step Vec4, count at +0x20) -> pointer to the contact array */
        out.input_widths = {0x70, 4, 4, 4, 0x24};
        out.output_width = 8;
        return true;
    default:
        return false;
    }
}

bool WheelOps::describe_call(uint32_t key, const std::vector<uint32_t>& consts,
                             OperatorSignature& out) {
    if (key == kStructBuild) {
        /* The second constant is the field count, and the layout is looked up by it. */
        const BuilderLayout* b = consts.size() >= 2 ? builder_for(consts[1]) : nullptr;
        if (std::getenv("BF6_BUILD_DEBUG")) {
            std::fprintf(stderr, "struct build describe_call: %zu const(s)", consts.size());
            for (uint32_t c : consts) std::fprintf(stderr, " %u", c);
            std::fprintf(stderr, " -> layout %s\n", b ? "found" : "none");
        }
        if (!b || b->widths.size() != consts[1]) return false;
        out = OperatorSignature{};
        out.input_widths.assign(kStructBuildLead, 4u);
        for (uint32_t w : b->widths) out.input_widths.push_back(w);
        out.output_width = b->size;
        return true;
    }
    if (!describe(key, out)) return false;
    /* operands = inputs + outputs (these calls have no context list), so a record with
     * fewer operands than the signature is one that left trailing inputs off. */
    const size_t outs = 1 + out.extra_output_widths.size();
    if (consts.size() >= outs) {
        const size_t ins = consts.size() - outs;
        if (ins < out.input_widths.size() && ins + 2 >= out.input_widths.size())
            out.input_widths.resize(ins);
    }
    return true;
}

bool WheelOps::invoke(uint32_t key, const std::vector<Value>& a, Value& out) {
    /* The byte ranges each native READS of its struct inputs; padding a graph never
     * writes does not make the call unknown. {input, offset, length}. */
    struct Need { size_t in; uint32_t off, len; };
    static const Need kRay[] = {{0, 0x00, 12}, {0, WC_RADIUS, 4}};
    /* the track pass reads the same fields of the same structs, minus the single
     * contact: its contacts come from the heap block, not from an operand */
    static const Need kTrackSusp[] = {{2, 0, 12}, {3, 0x00, 12}, {3, WC_RADIUS, 4}, {6, 0x00, 0x54}};
    /* per group: the contact's flag and position, and the config's radius+position */
    /* the hull reads its whole description and its whole physics block; the two
     * water planes are read only when they are not null, which is a length test */
    /* The hull needs its whole description, but only the first TEN floats of its
     * physics block: four drag gains, four flow gains, the attenuator and the
     * buoyancy gain. The graph assembles that block field by field and leaves the
     * last two words alone, so asking for all forty-eight bytes refused every call. */
    static const Need kHull[] = {{1, 0, 0x30}, {2, 0, 40}};
    /* the probe reads its centre and the matrix's rotation and translation */
    static const Need kPlane[] = {{2, 0, 12}, {3, 0, 48}, {3, 48, 12}};
    static const Need kShare[] = {{0, CT_POSITION, 12}, {3, 0x00, 12}, {3, WC_RADIUS, 4},
                                  {5, CT_POSITION, 12}, {8, 0x00, 12}, {8, WC_RADIUS, 4},
                                  {10, CT_POSITION, 12}, {13, 0x00, 12}, {13, WC_RADIUS, 4}};
    static const Need kSusp[] = {{1, 0x00, 12}, {1, 0x10, 12}, {1, 0x20, 12},
                                 {2, 0, 12}, {3, 0x00, 12}, {3, WC_RADIUS, 4}, {6, 0x00, 0x54}};
    static const Need kContact[] = {{3, 0x58, 4}};
    static const Need kForce[] = {{1, 0, 12}, {2, 0, 12}};
    static const Need kStill[] = {{2, 0, 12}, {3, 0, 12}, {4, 0, 12}, {5, 0, 12}};
    static const Need kAero[] = {{1, 0, 12}, {2, 0, 12}, {3, 0, 12}, {4, 0, 12}, {5, 0, 12}, {6, 0, 12}};
    /* HasContact is deliberately NOT required: the graph builds the first call's
     * WheelContact in place (velocity zeroed, Position from the wheel's
     * InitialPosition) and leaves the tail unwritten, which is "no contact". */
    static const Need kTyre[] = {{4, 0x00, 12}, {4, 0x10, 12}, {4, 0x20, 12},
                                 {10, 0, 12}, {11, 0, 12}, {12, 0x10, 0x48}, {12, WC_RADIUS, 8}};
    auto struct_input = [&](size_t i) {
        if (key == kWheelRay || key == kTrackContacts) return i == 0;
        if (key == kTrackSuspension) return i == 2 || i == 3 || i == 6;
        if (key == kTrackShare) return i % 5 == 0 || i % 5 == 3 || i % 5 == 4;
        if (key == kBoatHull) return i == 1 || i == 2 || i == 3 || i == 4;
        if (key == kWaterPlane) return i == 2 || i == 3;
        if (key == kTyreForce) return i == 4 || i == 10 || i == 11 || i == 12;
        if (key == kSuspension) return i == 1 || i == 2 || i == 3 || i == 6;
        if (key == kContactForce) return i == 1 || i == 2 || i == 3;
        if (key == kForceAtPos) return i == 1 || i == 2;
        if (key == kStandStill) return i == 2 || i == 3 || i == 4 || i == 5;
        if (key == kBuoyancy) return true;   /* the zero path reads nothing */
        if (key == kAeroDrag) return i >= 1;
        return false;
    };
    auto bytes_known = [&](const Value& v, uint32_t off, uint32_t len) {
        if (v.known) return true;
        if (v.known_bytes.size() < off + len) return false;
        for (uint32_t b = 0; b < len; ++b) if (!v.known_bytes[off + b]) return false;
        return true;
    };
    for (size_t i = 0; i < a.size(); ++i) {
        bool ok = a[i].known;
        if (!ok && struct_input(i)) {
            ok = true;
            const Need* nb = key == kWheelRay ? kRay : key == kSuspension ? kSusp
                           : key == kContactForce ? kContact : key == kForceAtPos ? kForce
                           : key == kWaterPlane ? kPlane
                           : key == kBoatHull ? kHull
                           : key == kTrackShare ? kShare
                           : key == kTrackSuspension ? kTrackSusp
                           : key == kStandStill ? kStill : key == kAeroDrag ? kAero
                           : kTyre;
            const size_t nn = key == kWheelRay ? sizeof kRay / sizeof *kRay
                            : key == kSuspension ? sizeof kSusp / sizeof *kSusp
                            : key == kContactForce ? sizeof kContact / sizeof *kContact
                            : key == kForceAtPos ? sizeof kForce / sizeof *kForce
                            : key == kStandStill ? sizeof kStill / sizeof *kStill
                            : key == kWaterPlane ? sizeof kPlane / sizeof *kPlane
                            : key == kBoatHull ? sizeof kHull / sizeof *kHull
                            : key == kTrackShare ? sizeof kShare / sizeof *kShare
                            : key == kTrackSuspension ? sizeof kTrackSusp / sizeof *kTrackSusp
                            : key == kAeroDrag ? sizeof kAero / sizeof *kAero
                            : sizeof kTyre / sizeof *kTyre;
            for (size_t k = 0; k < nn; ++k)
                if (nb[k].in == i && !bytes_known(a[i], nb[k].off, nb[k].len)) {
                    ok = false;
                    if (std::getenv("BF6_WHEEL_DEBUG"))
                        std::fprintf(stderr, "  input %zu misses bytes +0x%X..+0x%X (known_bytes %zu)\n",
                                     i, nb[k].off, nb[k].off + nb[k].len - 1, a[i].known_bytes.size());
                }
        }
        if (!ok) {
            if (std::getenv("BF6_WHEEL_DEBUG"))
                std::fprintf(stderr, "wheel op %08X refused: input %zu unknown (%zu bytes)\n", key, i, a[i].bytes.size());
            return false;
        }
    }
    ++served_[key];
    if (key == kBodyMass) {
        /* FUN_1443E3CD0: 1 / (the body's inverse mass), 0 when that is 0 */
        out.bytes.assign(4, 0);
        wf(out.bytes, 0, body_.mass);
        out.known = true;
        return true;
    }
    /* Every branch below indexes its inputs directly, so a record that carries fewer
     * than the signature (describe_call allows a trailing input to be left off) is
     * refused rather than read past its end. */
    static const struct { uint32_t key; size_t min_in; } kMinIn[] = {
        {kWheelRay, 4}, {kTyreForce, 13}, {kSuspension, 9}, {kContactForce, 4},
        {kForceAtPos, 3}, {kAeroDrag, 7}, {kStandStill, 10}, {kBuoyancy, 3},
        {kTrackContacts, 5},
        {kCopy16, 1}, {kDownRay, 2}, {kIdIsNot, 1},
        {kTrackSuspension, 8},
        {kTrackShare, 15},
        {kCurveKeyed, 2},
        {kBoatHull, 7},
        {kWaterPlane, 5},
        {kWaterHeightAt, 1},
        {kMotionDamping, 5},
    };
    for (const auto& m : kMinIn)
        if (m.key == key && a.size() < m.min_in) {
            if (std::getenv("BF6_WHEEL_DEBUG"))
                std::fprintf(stderr, "wheel op %08X refused: %zu inputs, needs %zu\n", key, a.size(), m.min_in);
            return false;
        }
    if (key == kHeliRotor) {
        /* FUN_1443EB0B0, the main rotor, as far as it is transcribed.
         *
         * The reflected registry names the operator (DeltaTime, Throttle, Pitch, Roll,
         * HasGroundContect, GravityModifier, Config, LinearAcceleration,
         * AngularAcceleration), so Throttle is the collective and Pitch/Roll the
         * cyclic. Its lift is built along the mast axis and submitted as a force at a
         * point, which this host's force accumulator already does.
         *
         * TWO THINGS ARE NOT READ FROM THE GAME AND ARE MARKED HERE:
         *
         *   THE ROTOR POWER. The kernel's base magnitude is
         *   -(mass * telemetry[1] * GravityModifier), where telemetry is a three-float
         *   block the wrapper reads off the entity at +0x1B10. It is not in any
         *   listing, so it is CALIBRATED here instead: the transcribed gains total
         *   0.99 + 1.65 = 2.64, so a power of g / 2.64 makes the rotor carry its own
         *   weight. That is a stand-in with its arithmetic shown, not a measurement,
         *   and it is the first thing to replace when the telemetry is read. Raw
         *   gravity is NOT the value - it would hover at under one per cent collective.
         *
         *   THE CEILING. The upward component fades to zero over 50 m above
         *   (a host scalar + Config 0x80), and neither the scalar nor that config field
         *   is available, so the fade is 1. It only ever REDUCES lift with height, so
         *   the error is a helicopter with no ceiling rather than one that cannot fly.
         *
         * What is transcribed exactly: the collective's own arithmetic. In the air the
         * factor is max(1, Throttle) - `if (HasGroundContect == 0 || Throttle > 0) f =
         * 1; if (f <= Throttle) f = Throttle` - so a collective below 1 does not scale
         * lift at all. That reads oddly for a collective and is left as the listing has
         * it rather than adjusted to taste; if a helicopter hovers without regard to
         * the stick, this is why, and the answer is in the graph's Throttle range, not
         * here. */
        const float dt = rf(a[0], 0);
        if (!(dt > 0.0f)) return false;
        float throttle = rf(a[1], 0);
        const float pitch = rf(a[2], 0), roll = rf(a[3], 0);
        const bool on_ground = a[4].bytes.size() && a[4].bytes[0] != 0;
        const float gmod = rf(a[5], 0);
        const Value& cfg = a[6];
        auto c = [&](uint32_t at) { return rf(cfg, at); };
        if (cfg.bytes.size() >= 0xB0 && cfg.bytes[0xAC] != 0) throttle = -throttle;
        const float ground_factor = (!on_ground || throttle > 0.0f) ? 1.0f : 0.0f;
        const float collective = ground_factor <= throttle ? throttle : ground_factor;

        /* The rotor power, calibrated as the note above sets out. */
        const float gain_total = c(0x9C) + c(0xA8);
        const float power = gain_total > 1e-6f ? 9.82f / gain_total : 0.0f;
        /* THE SIGN, settled by measurement. The kernel negates its base magnitude,
         * which pairs with a mast axis that points DOWN in the state block its wrapper
         * builds; the axis here is the body's up, so the negation goes with it. What
         * makes this a measurement rather than a coin flip: with the calibration above
         * the magnitude came out at exactly 9.82 m/s2, the weight it has to carry, so
         * the size was already right and the direction was the only thing left to fix
         * - and one of the two choices has a helicopter push itself into the ground. */
        const float base = body_.mass * power * gmod;
        const float deadband = body_.mass * 0.001f;

        /* The mast axis is the body's own up; this host builds the state block the
         * kernel reads, so the axis is chosen to be the one the kernel lifts along. */
        const float up[3] = {0.0f, 1.0f, 0.0f};
        Snapshot s;
        s.mass = body_.mass;
        s.inv_mass = body_.mass != 0.0f ? 1.0f / body_.mass : 0.0f;
        std::memcpy(s.v, body_.v, 16);
        std::memcpy(s.w, body_.w, 16);
        std::memcpy(s.com, body_.com, 16);
        std::memcpy(s.inv_i, body_.inv_inertia, 16);
        const Snapshot before = s;
        std::vector<ForceRecord> recs;
        auto push = [&](float mag, const float dir[3], const float at[3]) {
            if (std::fabs(mag) <= deadband) return;
            ForceRecord r{};
            for (int i = 0; i < 3; ++i) { r.f[i] = dt * mag * dir[i] * force_scale; r.p[i] = at[i]; }
            if (finite3(r.f) && finite3(r.p)) recs.push_back(r);
        };
        /* The application point, offset by the cyclic exactly as the kernel does. */
        float point[3] = {body_.com[0], body_.com[1], body_.com[2]};
        point[0] += roll * c(0xA4);
        point[2] -= pitch * c(0xA0);
        push(base * c(0x9C) * collective, up, point);            /* the main lift */
        push(base * (collective < 0.0f ? c(0x7C) : c(0xA8)) * collective, up,
             body_.com);                                         /* the torque path */
        /* The disc-tilt penalty: nothing at level, full downforce inverted. */
        float tilt = (up[1] - 1.0f) * -0.5f;
        tilt = tilt < 0.0f ? 0.0f : (tilt > 1.0f ? 1.0f : tilt);
        const float down[3] = {0.0f, -1.0f, 0.0f};
        push(tilt * c(0x94) * base, down, body_.com);
        apply_all(s, recs);
        out.bytes.assign(32, 0);
        for (int i = 0; i < 4; ++i) wf(out.bytes, (uint32_t)(4 * i), (s.w[i] - before.w[i]) / dt);
        for (int i = 0; i < 4; ++i) wf(out.bytes, (uint32_t)(16 + 4 * i), (s.v[i] - before.v[i]) / dt);
        out.known = true;
        if (std::getenv("BF6_ROTOR_DEBUG"))
            std::fprintf(stderr, "rotor: throttle %g collective %g power %g -> dv %.2f %.2f %.2f\n",
                         throttle, collective, power, (s.v[0] - before.v[0]) / dt,
                         (s.v[1] - before.v[1]) / dt, (s.v[2] - before.v[2]) / dt);
        return true;
    }
    if (key == kTerrainAt) {
        if (!ray_) return false;
        const float at[3] = {rf(a[0], 0), rf(a[0], 4), rf(a[0], 8)};
        /* Down from well above the point to well below it, in the world, the way every
         * wheel ray here is cast. No hit is a hole in the ground, not a height. */
        const double from[3] = {at[0], at[1] + 1000.0, at[2]};
        const double to[3] = {at[0], at[1] - 1000.0, at[2]};
        double hit[3] = {0, 0, 0}, nrm[3] = {0, 0, 0};
        if (!ray_(ray_user_, from, to, hit, nrm)) return false;
        out.bytes.assign(20, 0);
        wf(out.bytes, 0, (float)hit[1]);                  /* primary: TerrainHeight */
        for (int i = 0; i < 3; ++i) wf(out.bytes, (uint32_t)(4 + 4 * i), (float)hit[i]);
        out.known = true;
        return true;
    }
    if (key == kStructBuild) {
        if (a.size() < kStructBuildLead) return false;
        const BuilderLayout* b = builder_for(a.size() - kStructBuildLead);
        if (!b) return false;
        /* The type's default, which this host cannot read, is taken as zero - so a
         * field the record does not pass stays zero rather than becoming a guess. */
        out.bytes.assign(b->size, 0);
        out.known = true;
        for (size_t i = 0; i < b->offsets.size(); ++i) {
            const Value& src = a[kStructBuildLead + i];
            const uint32_t at = b->offsets[i], w = b->widths[i];
            if ((size_t)at + w > out.bytes.size() || src.bytes.size() < w) continue;
            std::memcpy(out.bytes.data() + at, src.bytes.data(), w);
            if (!src.known) out.known = false;
        }
        if (std::getenv("BF6_BUILD_DEBUG"))
            std::fprintf(stderr, "struct build: %zu field(s) into %u bytes, known %d\n",
                         b->offsets.size(), b->size, (int)out.known);
        if (std::getenv("BF6_BUILD_DUMP")) {
            std::fprintf(stderr, "built struct:");
            for (size_t o = 0; o + 4 <= out.bytes.size(); o += 4) {
                float f = 0.0f;
                std::memcpy(&f, out.bytes.data() + o, 4);
                if (f != 0.0f) std::fprintf(stderr, " 0x%zX=%g", o, f);
            }
            std::fprintf(stderr, "\n");
        }
        return true;
    }
    if (key == kMotionDamping) {
        /* FUN_1443E9D10 through its kernel FUN_1443E7850, line for line. */
        const float dt = rf(a[0], 0);
        if (!(dt > 0.0f)) return false;
        auto damp = [&](const float v[4], const Value& coeff, float maxlen,
                        float fallback, std::vector<uint8_t>& dst, uint32_t at) {
            if (maxlen == 0.0f) maxlen = fallback;
            for (int i = 0; i < 3; ++i) {
                uint32_t bits = 0;
                std::memcpy(&bits, &v[i], 4);
                if ((bits & 0x7F800000u) == 0x7F800000u) {   /* inf or NaN: all zero */
                    for (int k = 0; k < 4; ++k) wf(dst, at + (uint32_t)(4 * k), 0.0f);
                    return;
                }
            }
            const float s = dt * -10.0f;
            float d[4];
            for (int i = 0; i < 3; ++i) {
                const float e = std::exp(s * rf(coeff, (uint32_t)(4 * i)));
                d[i] = v[i] - v[i] * (1.0f - e);
            }
            d[3] = v[3] - v[3] * 0.0f;
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (maxlen < len) {
                const float k = maxlen / len;
                for (int i = 0; i < 4; ++i) d[i] *= k;
            }
            for (int i = 0; i < 4; ++i) wf(dst, at + (uint32_t)(4 * i), (d[i] - v[i]) / dt);
        };
        /* THE PAIRING, settled by the executable's own parameter names.
         *
         * The reflected registry names this operator
         *   MotionMachine(DeltaTime, Linear, Angular, LinearSpeedLimit,
         *                 AngularSpeedLimit, LinearAcceleration, AngularAcceleration)
         * which fixes every operand and both outputs outright: the second operand is
         * the LINEAR coefficient set and the fourth its speed limit, the third is the
         * ANGULAR set and the fifth its limit, and the outputs come out linear first.
         *
         * Two earlier arguments for the same conclusion are now superseded and worth
         * recording as traps. One read the default limits as evidence, noting that 20
         * is a CB90's forty knots - a coincidence: 20 is the ANGULAR limit in radians
         * a second and 100 the linear one. The other took the fact that the opposite
         * pairing diverges as proof, which was reading a symptom. The names settle it
         * and neither guess was needed. */
        const float v4[4] = {body_.v[0], body_.v[1], body_.v[2], 0.0f};
        const float w4[4] = {body_.w[0], body_.w[1], body_.w[2], 0.0f};
        out.bytes.assign(32, 0);
        damp(v4, a[1], rf(a[3], 0), 100.0f, out.bytes, 16);   /* extra: LINEAR */
        damp(w4, a[2], rf(a[4], 0), 20.0f, out.bytes, 0);     /* primary: ANGULAR */
        /* WHICH OUTPUT REACHES WHICH CHANNEL, measured rather than argued: with
         * BF6_DAMP_PROBE each output carries a distinct marker, and whichever channel
         * the host reads it out of is the one the graph sums it into. */
        if (std::getenv("BF6_DAMP_PROBE")) {
            const float mark[8] = {11.f, 12.f, 13.f, 0.f, 21.f, 22.f, 23.f, 0.f};
            std::memcpy(out.bytes.data(), mark, 32);
        }
        out.known = true;
        if (std::getenv("BF6_WHEEL_DEBUG")) {
            auto o = [&](uint32_t at) {
                float f = 0.0f;
                std::memcpy(&f, out.bytes.data() + at, 4);
                return f;
            };
            std::fprintf(stderr,
                         "damping: dt %g kA (%g %g %g) max %g -> (%g %g %g); "
                         "kB (%g %g %g) max %g -> (%g %g %g)\n",
                         dt, rf(a[1], 0), rf(a[1], 4), rf(a[1], 8), rf(a[3], 0),
                         o(16), o(20), o(24),
                         rf(a[2], 0), rf(a[2], 4), rf(a[2], 8), rf(a[4], 0),
                         o(0), o(4), o(8));
        }
        return true;
    }
    if (key == kWaterHeightAt) {
        out.bytes.assign(4, 0);
        wf(out.bytes, 0, body_.water ? body_.water_height : kNoWater);
        out.known = true;
        return true;
    }
    if (key == kWaterPlane) {
        /* FUN_1443EF6C0: THE WATER PLANE UNDER THREE CORNERS.
         *
         * Exactly as the native lays them out: half the given width, half the given
         * length either side, three corners at (cx - hw, cy, cz - l/2), (cx, cy,
         * cz + l/2) and (cx + hw, cy, cz - l/2), each mapped through the given
         * matrix, each asking the world for its water height. Three hits give the
         * plane through them, the normal from the cross product and flipped to point
         * up; nothing hit gives (0, 1, 0, -1024), which is the native's own no-water
         * answer and what a land map is.
         *
         * The host's surface is a flat height, so all three corners agree and the
         * plane comes out level. The native's one-and-two-hit case builds a plane
         * from a fallback vector and cannot arise from a flat surface, so it is left
         * out rather than invented. */
        const float hw = rf(a[0], 0) * 0.5f, hl = rf(a[1], 0) * 0.5f;
        const float c[3] = {rf(a[2], 0), rf(a[2], 4), rf(a[2], 8)};
        const float corner[3][3] = {
            {c[0] - hw, c[1], c[2] - hl},
            {c[0],      c[1], c[2] + hl},
            {c[0] + hw, c[1], c[2] - hl},
        };
        float h[3][3];
        int hits = 0;
        for (int k = 0; k < 3; ++k) {
            for (int i = 0; i < 3; ++i)
                h[k][i] = rf(a[3], (uint32_t)(4 * i)) * corner[k][0] +
                          rf(a[3], (uint32_t)(16 + 4 * i)) * corner[k][1] +
                          rf(a[3], (uint32_t)(32 + 4 * i)) * corner[k][2] +
                          rf(a[3], (uint32_t)(48 + 4 * i));
            h[k][1] = body_.water_height;      /* the surface the host supplied */
            if (body_.water) ++hits;
        }
        out.bytes.assign(16 + 1, 0);
        if (hits == 3) {
            const float u[3] = {h[1][0] - h[0][0], h[1][1] - h[0][1], h[1][2] - h[0][2]};
            const float v[3] = {h[2][0] - h[0][0], h[2][1] - h[0][1], h[2][2] - h[0][2]};
            float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                          u[0] * v[1] - u[1] * v[0]};
            const float m = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (m > 1e-9f) { for (int i = 0; i < 3; ++i) n[i] /= m; }
            else { n[0] = 0.0f; n[1] = 1.0f; n[2] = 0.0f; }
            if (n[1] < 0.0f) for (int i = 0; i < 3; ++i) n[i] = -n[i];
            for (int i = 0; i < 3; ++i) wf(out.bytes, (uint32_t)(4 * i), n[i]);
            wf(out.bytes, 12, -(n[0] * h[0][0] + n[1] * h[0][1] + n[2] * h[0][2]));
            out.bytes[16] = 1;
        } else {
            wf(out.bytes, 0, 0.0f);
            wf(out.bytes, 4, 1.0f);
            wf(out.bytes, 8, 0.0f);
            wf(out.bytes, 12, -1024.0f);
            out.bytes[16] = 0;
        }
        out.known = true;
        return true;
    }
    if (key == kBoatHull) {
        /* simulateBoatHullNew: FUN_1443E1130, which builds the hull surface
         * (FUN_1443E1960) and simulates it (FUN_1443E30C0), one triangle at a time.
         *
         * WHAT IS THE GAME'S, exactly: the water clip per triangle (three signed
         * distances, the all-dry and all-wet cases, the partial cut with its
         * centroid and submerged area), the force law with every constant - 9820 for
         * buoyancy, the 1000 and 100 gain scalings, the 0.2 attenuator, the
         * application at the submerged centroid - and the accumulate-then-integrate
         * that turns those forces into a velocity change over dt.
         *
         * WHAT IS OURS, and labelled: the hull SURFACE. The game grows its mesh from
         * the same twelve-float description through a parametric grid whose shaping
         * has not been transcribed yet; this builds a box of the description's own
         * length, width and depth, subdivided by its own station count. The
         * displacement is therefore the right order and the shape is not the ship's.
         *
         * Description (measured on the cb90, which is a 15.9 m boat): [0..2] the hull
         * origin in vehicle space, [4] a side exponent, [5] stations, [6] width, [7]
         * stern rows, [8] a stern fraction, [9] a bottom exponent, [10] depth, [11]
         * length. */
        const float dt = rf(a[0], 0);
        if (dt <= 0.0f) return false;
        const Value& H = a[1];      /* the hull description */
        const Value& P = a[2];      /* the hull physics gains */
        const float ox = rf(H, 0), oy = rf(H, 4), oz = rf(H, 8);
        const float width = rf(H, 6 * 4), depth = rf(H, 10 * 4), length = rf(H, 11 * 4);
        uint32_t stations = ru(H, 5 * 4);
        if (stations < 1 || stations > 64) stations = 4;
        if (!(width > 0.0f) || !(length > 0.0f) || !(depth > 0.0f)) return false;
        hull_half_length_ = length * 0.5f;

        /* THE BOX HULL, ours. Bottom, two sides and the two ends, each quad split the
         * way the game's own splitter splits one (FUN_1443E01E0): the normal is the
         * cross of its edges, the area half that cross's length, the reference point
         * the centroid. Outward normals, so a submerged panel pushes the hull up. */
        struct Tri { float v[3][3], n[3], c[3], area; };
        std::vector<Tri> tris;
        const float hw = width * 0.5f, hl = length * 0.5f;
        auto quad = [&](const float A[3], const float B[3], const float C[3], const float D[3]) {
            const float* q[4][3] = {{A, B, C}, {A, C, D}};
            for (int t = 0; t < 2; ++t) {
                Tri e{};
                for (int k = 0; k < 3; ++k)
                    for (int i = 0; i < 3; ++i) e.v[k][i] = q[t][k][i];
                const float e1[3] = {e.v[1][0] - e.v[0][0], e.v[1][1] - e.v[0][1], e.v[1][2] - e.v[0][2]};
                const float e2[3] = {e.v[2][0] - e.v[0][0], e.v[2][1] - e.v[0][1], e.v[2][2] - e.v[0][2]};
                e.n[0] = e2[2] * e1[1] - e1[2] * e2[1];
                e.n[1] = e2[0] * e1[2] - e1[0] * e2[2];
                e.n[2] = e2[1] * e1[0] - e1[1] * e2[0];
                const float m2 = e.n[0] * e.n[0] + e.n[1] * e.n[1] + e.n[2] * e.n[2];
                e.area = std::sqrt(m2) * 0.5f;
                if (e.area <= 1e-6f) continue;
                const float inv = 1.0f / std::sqrt(m2);
                for (int i = 0; i < 3; ++i) e.n[i] *= inv;
                for (int i = 0; i < 3; ++i) e.c[i] = (e.v[0][i] + e.v[1][i] + e.v[2][i]) * 0.33333334f;
                tris.push_back(e);
            }
        };
        const float y_top = oy + depth;
        /* THE HULL, from FUN_1443E1960 rather than the box that stood in for it. The
         * box displaced 208 cubic metres where this boat floats on 13.5, which is why
         * it was thrown half a kilometre into the air.
         *
         * The surface is a HALF hull, mirrored. Station i runs bow to joint, point j
         * runs centreline to gunwale, and the two exponents shape it: [9] the bottom
         * and stem fullness against a half-LENGTH base, [4] the transverse fullness
         * against a half-BEAM base. The forebody blends one into the other, the stern
         * run carries the section aft to the transom at a constant beam, and a transom
         * cap closes it. The deck is left open, as the native leaves it.
         *
         * Two things are pinned by continuity rather than read, because the sincos
         * polynomial's constants are not in the listing: the first factor is cos-like
         * so the joint station meets the stern run at the full half beam, the second
         * sin-like so the bow station collapses to a stem at +L/2. Both are labelled
         * here and the reconstruction is checked against the one number that can
         * falsify it - a floating hull displaces its own mass. */
        const uint32_t stern_rows = ru(H, 7 * 4) ? ru(H, 7 * 4) : 1u;
        if (std::getenv("BF6_HULL_DESC")) {
            std::fprintf(stderr, "hull desc:");
            for (int i = 0; i < 12; ++i) std::fprintf(stderr, " [%d]=%g", i, rf(H, (uint32_t)(4 * i)));
            std::fprintf(stderr, "\n");
        }
        const float fore = rf(H, 8 * 4);
        const float side_exp = rf(H, 4 * 4), bottom_exp = rf(H, 9 * 4);
        const float zslope = hl > 0.0f ? (fore * length) / hl : 0.0f;
        const float zoff = fore * length - hl;
        const float stern_span = (1.0f - fore) * length;
        const uint32_t rows = stations + stern_rows;
        std::vector<std::vector<std::array<float, 3>>> grid(
            rows, std::vector<std::array<float, 3>>(stations));
        const float step = stations > 1 ? 1.0f / (float)(stations - 1) : 1.0f;
        for (uint32_t i = 0; i < stations; ++i) {
            const float u = (float)i * step, w = 1.0f - u;
            const float ang = w * 1.5707964f;
            const float Sn = std::cos(ang);      /* cos-like, pinned by continuity */
            const float Cs = std::sin(ang);      /* sin-like, pinned by continuity */
            for (uint32_t j = 0; j < stations; ++j) {
                const float vv = (float)j * step;
                const float X = vv * hw * Sn;
                const float Y = depth * (w * std::pow(vv, bottom_exp) +
                                         u * std::pow(vv, side_exp));
                const float Z = (w * (vv * hl * Cs) +
                                 u * (vv * hw * Cs * (hl > 0.0f ? hw / hl : 0.0f)))
                                * zslope - zoff;
                grid[i][j] = {ox + X, oy + Y, oz + Z};
            }
        }
        for (uint32_t i2 = 0; i2 < stern_rows; ++i2) {
            const uint32_t r = stations + i2;
            for (uint32_t j = 0; j < stations; ++j) {
                const float X = (float)j * step * hw;
                const float Y = depth * std::pow(hw > 0.0f ? X / hw : 0.0f, side_exp);
                const float Z = (hl - fore * length) -
                                (float)(i2 + 1) * (1.0f / (float)stern_rows) * stern_span;
                grid[r][j] = {ox + X, oy + Y, oz + Z};
            }
        }
        /* Starboard, then the mirror with X negated about the origin. */
        for (int side = 0; side < 2; ++side) {
            const float sx = side ? -1.0f : 1.0f;
            auto at = [&](uint32_t r, uint32_t j, float out[3]) {
                out[0] = ox + sx * (grid[r][j][0] - ox);
                out[1] = grid[r][j][1];
                out[2] = grid[r][j][2];
            };
            for (uint32_t r = 1; r < rows; ++r)
                for (uint32_t j = 1; j < stations; ++j) {
                    float a[3], b[3], c[3], d[3];
                    at(r - 1, j - 1, a); at(r - 1, j, b); at(r, j, c); at(r, j - 1, d);
                    if (side) quad(a, d, c, b);
                    else quad(a, b, c, d);
                }
        }
        {   /* the transom, at the aft end, closing the section */
            const float zb = oz - hl;
            const float a0[3] = {ox - hw, oy, zb}, a1[3] = {ox + hw, oy, zb};
            const float a2[3] = {ox + hw, y_top, zb}, a3[3] = {ox - hw, y_top, zb};
            quad(a0, a3, a2, a1);
        }

        /* THE GAINS, exactly as the native scales them. */
        float drag[4], flow[4];
        for (int i = 0; i < 4; ++i) {
            drag[i] = rf(P, (uint32_t)(i * 4)) * 1000.0f;
            flow[i] = rf(P, (uint32_t)((4 + i) * 4)) * 100.0f;
        }
        const float att_gain = rf(P, 8 * 4), buoy_gain = rf(P, 9 * 4);

        Snapshot s;
        s.mass = body_.mass;
        s.inv_mass = body_.mass != 0.0f ? 1.0f / body_.mass : 0.0f;
        std::memcpy(s.v, body_.v, 16);
        std::memcpy(s.w, body_.w, 16);
        std::memcpy(s.com, body_.com, 16);
        std::memcpy(s.inv_i, body_.inv_inertia, 16);
        const Snapshot before = s;

        /* The water plane in VEHICLE space: the host's surface is a world height, so
         * the plane is (0,1,0, -(height - body y)) rotated into the body frame. With
         * no water at all every triangle is dry, which is the native's own answer on
         * a land map. */
        std::vector<ForceRecord> recs;
        float wet_sum = 0.0f, wet_norm = 0.0f;
        int wet_tris = 0;
        if (body_.water) {
            const float qc[4] = {-body_.quat[0], -body_.quat[1], -body_.quat[2], body_.quat[3]};
            float up_l[3];
            const float up_w[3] = {0.0f, 1.0f, 0.0f};
            qrot(qc, up_w, up_l);                       /* world up, in vehicle space */
            const float surface = body_.water_height - body_.pos[1];
            for (const Tri& e : tris) {
                /* signed distance of each vertex below the surface, vehicle space */
                float d[3];
                for (int k = 0; k < 3; ++k) {
                    const float h = e.v[k][0] * up_l[0] + e.v[k][1] * up_l[1] + e.v[k][2] * up_l[2];
                    d[k] = surface - h;
                }
                const int wet = (d[0] > 0.0f) + (d[1] > 0.0f) + (d[2] > 0.0f);
                if (wet == 0) continue;
                float frac = 1.0f, cent[3] = {e.c[0], e.c[1], e.c[2]};
                if (wet != 3) {
                    /* THE PARTIAL CUT. Both of the native's partial arms are here, with
                     * the submerged area normalised by the triangle's own - the two arms
                     * as transcribed disagree on whether the answer is an area or a
                     * fraction, and everything downstream multiplies by the area again,
                     * so a fraction is the reading that is dimensionally sound. */
                    int dry = 0, w0 = 0, w1 = 0;
                    for (int k = 0; k < 3; ++k) (d[k] > 0.0f ? (w0 ? w1 : w0) = k : dry = k);
                    const int A_ = wet == 1 ? w0 : dry;
                    const int B_ = wet == 1 ? (A_ + 1) % 3 : w0;
                    const int C_ = wet == 1 ? (A_ + 2) % 3 : w1;
                    const float t1 = std::fabs(d[A_]) / (std::fabs(d[B_]) + std::fabs(d[A_]) + 1e-12f);
                    const float t2 = std::fabs(d[A_]) / (std::fabs(d[C_]) + std::fabs(d[A_]) + 1e-12f);
                    float Pp[3], Qq[3];
                    for (int i = 0; i < 3; ++i) {
                        Pp[i] = e.v[A_][i] + (e.v[B_][i] - e.v[A_][i]) * t1;
                        Qq[i] = e.v[A_][i] + (e.v[C_][i] - e.v[A_][i]) * t2;
                    }
                    const float u[3] = {Pp[0] - e.v[A_][0], Pp[1] - e.v[A_][1], Pp[2] - e.v[A_][2]};
                    const float vv[3] = {Qq[0] - e.v[A_][0], Qq[1] - e.v[A_][1], Qq[2] - e.v[A_][2]};
                    const float cx = u[1] * vv[2] - u[2] * vv[1];
                    const float cy = u[2] * vv[0] - u[0] * vv[2];
                    const float cz = u[0] * vv[1] - u[1] * vv[0];
                    const float half = std::sqrt(cx * cx + cy * cy + cz * cz) * 0.5f;
                    if (wet == 1) {
                        frac = e.area > 0.0f ? half / e.area : 0.0f;
                        for (int i = 0; i < 3; ++i)
                            cent[i] = (e.v[A_][i] + Pp[i] + Qq[i]) * 0.33333334f;
                    } else {
                        frac = e.area > 0.0f ? (e.area - half) / e.area : 0.0f;
                        for (int i = 0; i < 3; ++i)
                            cent[i] = (Pp[i] + Qq[i] + e.v[B_][i] + e.v[C_][i]) * 0.25f;
                    }
                    if (frac <= 0.0f) continue;
                }
                const float ch = cent[0] * up_l[0] + cent[1] * up_l[1] + cent[2] * up_l[2];
                float dc = surface - ch;                  /* depth at the centroid */
                if (dc <= 0.0f) continue;
                if (dc > depth) dc = depth;               /* the hull's own depth limit */
                ++wet_tris;

                /* the water's velocity relative to this panel: the body's own motion at
                 * the centroid, w x r + v */
                const float r[3] = {cent[0] - body_.com[0], cent[1] - body_.com[1], cent[2] - body_.com[2]};
                const float rel[3] = {
                    (s.w[1] * r[2] - s.w[2] * r[1]) + s.v[0],
                    (s.w[2] * r[0] - s.w[0] * r[2]) + s.v[1],
                    (s.w[0] * r[1] - s.w[1] * r[0]) + s.v[2],
                };
                const float nvel = rel[0] * e.n[0] + rel[1] * e.n[1] + rel[2] * e.n[2];
                const float wet_mass = e.area * frac * dc;
                wet_sum += wet_mass;
                wet_norm += e.area * depth;

                /* buoyancy along world up, in vehicle space */
                const float fb = wet_mass * 9820.0f * buoy_gain;
                float fbv[3] = {up_l[0] * fb, up_l[1] * fb, up_l[2] * fb};
                float att = (rel[0] * up_l[0] + rel[1] * up_l[1] + rel[2] * up_l[2]) * 0.2f * att_gain;
                if (att <= 0.0f) att = 0.0f;
                if (1.0f <= att) att = 1.0f;
                const float slap = nvel > 0.0f ? e.area * frac * nvel : 0.0f;
                ForceRecord rec{};
                for (int i = 0; i < 3; ++i) {
                    const float tang = rel[i] - nvel * e.n[i];
                    rec.f[i] = dt * force_scale * (att * (-fbv[i]) + (-tang) * (e.area * frac) * flow[i] +
                                                   slap * (-e.n[i]) * drag[i] + fbv[i]);
                    rec.p[i] = cent[i];
                }
                if (finite3(rec.f) && finite3(rec.p)) recs.push_back(rec);
            }
        }
        apply_all(s, recs);
        /* The velocity change goes to the vehicle, not to the graph: this function
         * integrates into the body itself (FUN_1443EBAD0 on its own snapshot), and
         * what it RETURNS is two point velocities and how submerged the hull is. */
        for (int i = 0; i < 3; ++i) {
            hull_dv_[i] += s.v[i] - before.v[i];
            hull_dw_[i] += s.w[i] - before.w[i];
        }
        hull_ran_ = true;
        /* THE OUTPUT ORDER IS THE REGISTRY'S. The executable names this operator
         * (DeltaTime, HullConfig, SimulationConfig, WaterPlane, SecondWaterPlane,
         * SecondWaterPlaneTickDifference, WavesCanAffectBoatHorizontally,
         * UnderWaterRatio, LinearAccelerationOut, AngularAccelerationOut), so its
         * three outputs come in that order - and the VM's primary is the LAST
         * operand, which makes the primary the ANGULAR one. These were the other way
         * round, which fed the hull's linear force in as torque and its torque in as
         * force: the boat tumbled, and suppressing the hull's torque was what made
         * the tumble stop. */
        out.bytes.assign(16 + 4 + 16, 0);
        for (int i = 0; i < 4; ++i) wf(out.bytes, (uint32_t)(4 * i), (s.w[i] - before.w[i]) / dt);
        wf(out.bytes, 16, wet_norm > 0.0f ? wet_sum / wet_norm : 0.0f);
        for (int i = 0; i < 4; ++i) wf(out.bytes, (uint32_t)(20 + 4 * i), (s.v[i] - before.v[i]) / dt);
        out.known = true;
        if (std::getenv("BF6_HULL_DEBUG")) {
            /* THE ONE NUMBER THAT SAYS WHETHER THE SURFACE IS RIGHT. Buoyancy is
             * rho g V, so the upward acceleration times the mass, over rho g, is the
             * volume of water this surface claims to displace - and a floating hull
             * displaces its own mass, 13.5 cubic metres for a 13.5 t boat. Printed
             * next to the weight's own acceleration so the two can be compared at a
             * glance: equal means it floats, and anything else says by how much the
             * surface is wrong rather than merely that it is. */
            const float up = (s.v[1] - before.v[1]) / dt;
            const float volume = up * body_.mass / (1000.0f * 9.82f);
            std::fprintf(stderr,
                         "hull: %zu panels, %d wet, submerged %.3f, dv %.3f %.3f %.3f"
                         "  -> displaces %.1f m3 (floats at %.1f)\n",
                         tris.size(), wet_tris, wet_norm > 0.0f ? wet_sum / wet_norm : 0.0f,
                         (s.v[0] - before.v[0]) / dt, up, (s.v[2] - before.v[2]) / dt,
                         volume, body_.mass / 1000.0f);
        }
        return true;
    }
    if (key == kCurveKeyed) {
        /* FUN_1479DD310 -> FUN_1405626E0: a KEYED curve.
         *
         * The operand is a 40-byte struct and the keys are not in it: its +0x18 is a
         * pointer the loader relocates, and offline that field holds the pool offset
         * of the key array, whose count sits in the word BEFORE it. Seven floats a
         * key: x, mode, in-tangent x, in-tangent y, out-tangent x, y, out-tangent y.
         *
         * The mode picks the interpolation - 1 smoothstep, 2 linear, 3 a step at the
         * halfway point - and mode 0 is a cubic solved by Newton, which is NOT served
         * here. The decompiler reassociates that arm's arithmetic, and a curve
         * evaluated from a misread expression is a wrong number with no symptom, so
         * it is refused and says so under BF6_CURVE_DEBUG instead. */
        const bool dbg = std::getenv("BF6_CURVE_DEBUG") != nullptr;
        auto no = [&](const char* why) {
            if (dbg) std::fprintf(stderr, "curve refused: %s\n", why);
            return false;
        };
        const Value& C = a[1];
        if (C.bytes.size() < 4) return no("the operand is short");
        /* THE CURVE IS EITHER THERE OR POINTED AT. A slot wide enough for the asset
         * holds it, and its keys field is +0x18; a slot the graph spaces eight bytes
         * apart holds a handle instead, and the keys field is the handle itself.
         * Both ship - a car keeps its curves inline, a boat by reference - so the
         * seeded tag decides which one this is rather than an assumption. */
        uint32_t at = 0;
        std::memcpy(&at, C.bytes.data(), 4);
        if (C.bytes.size() >= 0x1C) {
            uint32_t inl = 0;
            std::memcpy(&inl, C.bytes.data() + 0x18, 4);
            if ((at & 0xFFFF0000u) != 0xC0DE0000u || (inl & 0xFFFF0000u) == 0xC0DE0000u) at = inl;
        }
        /* THE CURVE THE LOADER WOULD HAVE BOUND. The slot holds a HANDLE, not the
         * curve: the native dereferences it (`*param_2 + 0x18` into a CurveAsset the
         * loader allocated). Offline that handle names one of the curves the caller
         * resolved from this graph's own EBX (see vehicle_typed_seed.inc), where the
         * keys actually are. Everything below is the native's own evaluation. */
        if ((at & 0xFFFF0000u) == 0xC0DE0000u) {
            const size_t idx = at & 0xFFFFu;
            if (idx >= curves_.size() || curves_.size() == 0) return no("no curve bound there");
            const std::vector<float>& kv = curves_[idx];
            if (kv.size() < 7) return no("the bound curve has no keys");
            const uint32_t n = (uint32_t)(kv.size() / 7);
            const float x = rf(a[0], 0);
            float y = 0.0f;
            if (!curve_eval_keyed(kv.data(), n, x, y)) return no("the cubic arm");
            out.bytes.assign(4, 0);
            wf(out.bytes, 0, y);
            out.known = true;
            if (dbg) std::fprintf(stderr, "curve %zu (%u keys): x %g -> %g\n", idx, n, x, y);
            return true;
        }
        if (!heap_) return no("no pool access");
        std::vector<uint8_t> head;
        /* A HANDLE THAT NAMES NOTHING. The pool word is zero on disk (the loader
         * writes the CurveAsset pointer there) and 0x000FFFFF is the engine's
         * unbound placeholder, so neither is an offset to read keys from. Anything
         * else is tried as a pool offset, which is what a relocated array field
         * holds offline. */
        if (dbg) std::fprintf(stderr, "curve handle 0x%X%s\n", at,
                              at == 0x000FFFFFu || at == 0
                                  ? " (nothing bound: no curve was seeded for this slot)" : "");
        if (at < 4 || !heap_->pool(at - 4, 4, head)) return no("the key pointer does not resolve");
        uint32_t count = 0;
        std::memcpy(&count, head.data(), 4);
        count &= 0x7FFFFFFFu;               /* the native takes the count's magnitude */
        if (count == 0 || count > 64) return no("the key count is out of range");
        std::vector<uint8_t> keys;
        if (!heap_->pool(at, count * 28u, keys)) return no("the keys do not resolve");
        auto k = [&](uint32_t i, uint32_t f) {
            float v = 0.0f;
            std::memcpy(&v, keys.data() + (size_t)i * 28 + (size_t)f * 4, 4);
            return v;
        };
        const float x = rf(a[0], 0);
        float y = 0.0f;
        bool served = true;
        if (count == 1) {
            y = k(0, 5);
        } else if (x < k(0, 0) && k(0, 1) != 0.0f) {
            y = k(0, 5);                     /* before the first key, and not a cubic */
        } else if (k(count - 1, 0) < x) {
            /* past the last key: along its out tangent when it has one */
            if (k(count - 1, 1) == 0.0f && std::fabs(k(count - 1, 2)) > 1e-06f)
                y = ((x - k(count - 1, 0)) * k(count - 1, 6)) / k(count - 1, 2) + k(count - 1, 5);
            else
                y = k(count - 1, 5);
        } else {
            /* the native binary-searches for the bracketing key */
            uint32_t i = 0;
            int lo = 0, hi = (int)count - 2;
            while (lo <= hi) {
                i = (uint32_t)((lo + hi) / 2);
                if (x == k(i, 0)) break;
                if (k(i, 0) <= x) { lo = (int)i + 1; i = (uint32_t)lo; }
                else { hi = (int)i - 1; i = (uint32_t)(hi < 0 ? 0 : hi); }
            }
            if (i > count - 2) i = count - 2;
            const float x0 = k(i, 0), x1 = k(i + 1, 0);
            if (x1 - x0 <= 0.0f) {
                y = (k(i + 1, 5) + k(i, 5)) * 0.5f;
            } else {
                const float t = (x - x0) / (x1 - x0);
                const float mode = k(i, 1);
                uint32_t m = 0;
                std::memcpy(&m, &mode, 4);
                if (m == 1u) {               /* smoothstep between the two values */
                    y = (3.0f - (t + t)) * (k(i + 1, 5) - k(i, 5)) * t * t + k(i, 5);
                } else if (m == 2u) {        /* linear */
                    y = (k(i + 1, 5) - k(i, 5)) * t + k(i, 5);
                } else if (m == 3u) {        /* a step at the halfway point */
                    y = t >= 0.5f ? k(i + 1, 5) : k(i, 5);
                } else {
                    served = false;          /* the cubic arm: see the note above */
                }
            }
        }
        if (std::getenv("BF6_CURVE_DEBUG"))
            std::fprintf(stderr, "curve at pool 0x%X: %u keys, x %g -> %s%g\n", at, count, x,
                         served ? "" : "REFUSED (cubic) ", y);
        if (!served) return false;
        out.bytes.assign(4, 0);
        wf(out.bytes, 0, y);
        out.known = true;
        return true;
    }
    if (key == kTrackShare) {
        /* FUN_1443EE870: EVERY TRACK SECTION GETS A CONTACT.
         *
         * Three sections come in, each with its contact, its load, its force, its
         * wheel config and its road-wheel sampler. A section touching the ground
         * passes through unchanged. A section that is NOT touching borrows from the
         * nearest section that is - nearest in the GROUND PLANE, the original drops
         * the height term by multiplying it by zero - and gets:
         *
         *   load  = |gravity| * mass / (2 * the road wheels of all three sections)
         *   force = the sum over touching sections of (force * wheel radius),
         *           divided by (how many are touching * the donor's radius)
         *
         * With none or all three touching there is nothing to fill and the function
         * is a straight copy, which is the case a tank on flat ground is in. */
        const size_t kIn = 5;
        uint8_t touching[3] = {0, 0, 0};
        uint32_t n_touch = 0, wheels = 0;
        float force_radius = 0.0f;
        for (size_t g = 0; g < 3; ++g) {
            const Value& C = a[g * kIn];
            uint32_t cnt = 0;
            if (a[g * kIn + 4].bytes.size() >= 0x24)
                std::memcpy(&cnt, a[g * kIn + 4].bytes.data() + 0x20, 4);
            wheels += cnt;
            if (C.bytes.size() > CT_HASCONTACT && C.bytes[CT_HASCONTACT] != 0) {
                touching[g] = 1;
                ++n_touch;
                force_radius += rf(a[g * kIn + 2], 0) * rf(a[g * kIn + 3], WC_RADIUS);
            }
        }
        float gmag = 0.0f;
        for (int i = 0; i < 3; ++i) gmag += body_.gravity[i] * body_.gravity[i];
        gmag = std::sqrt(gmag);
        const float share = wheels ? gmag * body_.mass / (float)(wheels * 2u) : 0.0f;
        /* group g as (contact 0x40, load, force) */
        std::vector<uint8_t> group(3 * (0x40 + 4 + 4), 0);
        for (size_t g = 0; g < 3; ++g) {
            size_t donor = g;
            if (!touching[g] && n_touch != 0 && n_touch != 3) {
                float best = 3.4028235e+38f;
                const Value& W = a[g * kIn + 3];
                for (size_t d = 0; d < 3; ++d) {
                    if (!touching[d]) continue;
                    const Value& CD = a[d * kIn];
                    const float dx = rf(W, 0) - rf(CD, CT_POSITION);
                    const float dz = rf(W, 8) - rf(CD, CT_POSITION + 8);
                    const float d2 = dx * dx + dz * dz;
                    if (d2 < best) { best = d2; donor = d; }
                }
            }
            const Value& C = a[donor * kIn];
            const size_t at = g * (0x40 + 4 + 4);
            for (size_t b = 0; b < 0x40 && b < C.bytes.size(); ++b) group[at + b] = C.bytes[b];
            if (donor == g) {
                wf(group, (uint32_t)(at + 0x40), rf(a[g * kIn + 1], 0));
                wf(group, (uint32_t)(at + 0x44), rf(a[g * kIn + 2], 0));
            } else {
                const float radius = rf(a[donor * kIn + 3], WC_RADIUS);
                wf(group, (uint32_t)(at + 0x40), share);
                wf(group, (uint32_t)(at + 0x44),
                   (n_touch && radius != 0.0f) ? force_radius / ((float)n_touch * radius) : 0.0f);
            }
        }
        /* The record lays its outputs out as (contact, load, force) per group with the
         * LAST force as the primary, and this host answers primary first, then the
         * extras in record order. */
        out.bytes.assign(3 * (0x40 + 4 + 4), 0);
        std::memcpy(out.bytes.data(), group.data() + 2 * (0x40 + 8) + 0x44, 4);
        size_t w = 4;
        for (size_t g = 0; g < 3; ++g) {
            const size_t at = g * (0x40 + 4 + 4);
            std::memcpy(out.bytes.data() + w, group.data() + at, 0x40); w += 0x40;
            std::memcpy(out.bytes.data() + w, group.data() + at + 0x40, 4); w += 4;
            if (g == 2) break;
            std::memcpy(out.bytes.data() + w, group.data() + at + 0x44, 4); w += 4;
        }
        if (std::getenv("BF6_TRACK_DEBUG"))
            std::fprintf(stderr, "track share: %u of 3 touching, %u road wheels, share %.1f, "
                         "omega in %.3f %.3f %.3f\n", n_touch, wheels, share,
                         rf(a[2], 0), rf(a[kIn + 2], 0), rf(a[2 * kIn + 2], 0));
        out.known = true;
        return true;
    }
    if (key == kTrackSuspension) {
        /* FUN_1443EDD10: THE TANK'S SUSPENSION, which is the car's spring run once
         * per road-wheel contact.
         *
         * Per contact, gated exactly as a car wheel is - HasContact set and the
         * contact normal's y above 0.1 - it calls the same FUN_1443DFDF0 this file
         * already serves, pushes a spring record and a damper record at
         * HandlingOffset + the contact position, and keeps the contact NEAREST to the
         * wheel config's InitialPosition as the one it reports back. The force it
         * reports is that contact's spring plus its damper when the damper pushes,
         * floored at zero.
         *
         * The direction starts straight up and switches to the contact normal when
         * the config says to always use it (+0x51) or the running direction has
         * drifted more than 0.6 from it: the same rule as the single-wheel path. */
        if (!heap_) return false;
        uint32_t count = 0, stride = 0;
        const uint8_t* raw = nullptr;
        if (!heap_->read(a[1], count, stride, raw) || stride != 0x40) return false;
        const float dt = rf(a[0], 0);
        const Value& HO = a[2];
        const Value& W = a[3];
        const Value& SC = a[6];
        const float K = rf(a[4], 0), D = rf(a[5], 0), p8 = rf(a[7], 0);
        Snapshot s;
        s.mass = body_.mass;
        s.inv_mass = body_.mass != 0.0f ? 1.0f / body_.mass : 0.0f;
        std::memcpy(s.v, body_.v, 16);
        std::memcpy(s.w, body_.w, 16);
        std::memcpy(s.com, body_.com, 16);
        std::memcpy(s.inv_i, body_.inv_inertia, 16);
        const Snapshot before = s;
        float dir[4] = {0.0f, 1.0f, 0.0f, 0.0f};
        float best_d = 3.4028235e+38f, report = 0.0f;
        int best = -1;
        std::vector<ForceRecord> recs;
        for (uint32_t i = 0; i < count; ++i) {
            Value C;
            C.bytes.assign(raw + (size_t)i * 0x40, raw + (size_t)i * 0x40 + 0x40);
            C.known = true;
            if (C.bytes[CT_HASCONTACT] == 0 || rf(C, CT_NORMAL + 4) <= 0.1f) continue;
            float damper = 0.0f, compr = 0.0f;
            uint8_t high = 0;
            const float spring = spring_damper(dt, W, K, D, SC, s, C, p8, &damper, &compr, &high);
            const float nx = rf(C, CT_NORMAL), ny = rf(C, CT_NORMAL + 4), nz = rf(C, CT_NORMAL + 8);
            if (rb(SC, SC_ALWAYS_NORMAL) || dir[0] * nx + dir[1] * ny + dir[2] * nz < 0.6f)
                for (int k = 0; k < 4; ++k) dir[k] = rf(C, CT_NORMAL + 4 * k);
            ForceRecord r1{}, r2{};
            for (int k = 0; k < 4; ++k) {
                const float pt = rf(HO, 4 * k) + rf(C, CT_POSITION + 4 * k);
                r1.p[k] = r2.p[k] = pt;
                r1.f[k] = force_scale * dt * spring * dir[k];
                r2.f[k] = force_scale * dt * damper * dir[k];
            }
            if (finite3(r1.f) && finite3(r1.p)) recs.push_back(r1);
            if (finite3(r2.f) && finite3(r2.p)) recs.push_back(r2);
            const float dx = rf(C, CT_POSITION) - rf(W, 0);
            const float dy = rf(C, CT_POSITION + 4) - rf(W, 4);
            const float dz = rf(C, CT_POSITION + 8) - rf(W, 8);
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best_d) {
                float total = spring;
                if (0.0f < damper) total += damper;
                if (total <= 0.0f) total = 0.0f;
                report = total;
                best_d = d2;
                best = (int)i;
            }
        }
        apply_all(s, recs);
        out.bytes.assign(4 + 16 + 16 + 0x40, 0);
        wf(out.bytes, 0, report);
        for (int i = 0; i < 4; ++i) wf(out.bytes, 4 + 4 * i, (s.v[i] - before.v[i]) / dt);
        for (int i = 0; i < 4; ++i) wf(out.bytes, 20 + 4 * i, (s.w[i] - before.w[i]) / dt);
        if (best >= 0)
            std::memcpy(out.bytes.data() + 36, raw + (size_t)best * 0x40, 0x40);
        else
            wf(out.bytes, 36 + CT_NORMAL + 4, 1.0f);   /* no contact: normal straight up */
        if (std::getenv("BF6_SUSP_DEBUG"))
            std::fprintf(stderr, "track susp: %u contacts, best %d, force %.1f, dv %.3f %.3f %.3f\n",
                         count, best, report, (s.v[0] - before.v[0]) / dt,
                         (s.v[1] - before.v[1]) / dt, (s.v[2] - before.v[2]) / dt);
        out.known = true;
        return true;
    }
    if (key == kInertia) {
        /* FUN_1443E3BA0: the body's float4 at the +0xB98 array divided INTO
         * (1, 1, 1, 0) - so it is the inertia read back out of the inverse the
         * solver stores. The fourth lane divides 0 by 0 in the original; nothing
         * reads it, and this writes a plain 0 rather than reproducing a NaN. */
        out.bytes.assign(16, 0);
        for (int i = 0; i < 3; ++i)
            wf(out.bytes, 4 * i, body_.inv_inertia[i] != 0.0f ? 1.0f / body_.inv_inertia[i] : 0.0f);
        out.known = true;
        return true;
    }
    if (key == kCopy16) {
        /* movups xmm0, [rcx]; movups [rdx], xmm0 */
        out = a[0];
        out.bytes.resize(16, 0);
        out.known = true;
        return true;
    }
    if (key == kIdIsNot) {
        out = Value::from_bool(a[0].as_u32() != kIdConstant);
        return true;
    }
    if (key == kSetCom && a.empty()) {
        /* A RECORD THAT SETS NOTHING. Tracked vehicles call this with no operand at
         * all, and there is nothing to move the centre of mass to; the call exists
         * for its engine-side effect and produces no value, so doing nothing is the
         * whole of it. With an operand it is the real thing, below. */
        out = Value{};
        out.known = true;
        return true;
    }
    if (key == kClearFlag || key == kNotify) {
        /* Both reach into engine state and produce no value: one clears a word on a
         * context object, the other forwards to a virtual. Nothing downstream reads a
         * result, so doing nothing here is the whole behaviour offline - and saying so
         * is what stops them being counted as missing physics. */
        out = Value{};
        out.known = true;
        return true;
    }
    if (key == kDownRay) {
        /* FUN_1443F3CD0 -> FUN_1443F10C0: a VERTICAL segment, from the body's centre
         * plus the rotated local point, straight down by the given length, and the id
         * of whatever it hits. The hit has to be past 1% of the segment to count.
         *
         * OUR READING of the miss value is 0: the original leaves the id from a path
         * the decompiler discards, so 0 is this host's answer for "nothing there"
         * rather than a transcribed constant, and a positive id is the same STAND-IN
         * the wheel ray writes for a surface it cannot name. */
        if (!ray_) return false;
        const float len = rf(a[1], 0);
        const float pl[3] = {rf(a[0], 0), rf(a[0], 4), rf(a[0], 8)};
        float pw[3];
        qrot(body_.quat, pl, pw);
        double from[3], to[3], hit[3] = {0, 0, 0}, nrm[3] = {0, 1, 0};
        for (int i = 0; i < 3; ++i) { from[i] = pw[i] + body_.pos[i]; to[i] = from[i]; }
        to[1] -= len;
        ++rays_;
        uint32_t id = 0;
        if (ray_(ray_user_, from, to, hit, nrm)) {
            ++ray_hits_;
            const double travelled = from[1] - hit[1];
            if (len > 0.0f && travelled >= 0.01 * len) id = 1u;
        }
        out.bytes.assign(4, 0);
        wu(out.bytes, 0, id);
        out.known = true;
        return true;
    }
    if (key == kWheelRay) {
        if (!ray_) return false;
        const Value& W = a[0];
        const float spring = rf(a[1], 0), attach = rf(a[2], 0), extra = rf(a[3], 0);
        const float at[3] = {rf(W, 0), rf(W, 4), rf(W, 8)};
        cast_wheel_ray(at, rf(W, WC_RADIUS), spring, attach, extra, out.bytes);
        out.known = true;
        return true;
    }
    if (key == kTrackContacts) {
        /* FUN_1443F3670: THE TANK'S ROAD WHEELS, one ray each.
         *
         * Same ray as a car's wheel (it calls FUN_1443F1370, which is kWheelRay's own
         * native), run N times along the track: the first road wheel sits at the
         * config's InitialPosition plus the sampler's start offset, and each one after
         * it a fixed step further. The sampler operand carries start (4 floats), step
         * (4 floats) and the count as a uint at +0x20, and the whole offset is mirrored
         * for the left track - the native flips its sign on the sign of the config's
         * InitialPosition.x, which is what makes one descriptor serve both sides.
         *
         * The engine grows a vector and hands back a pointer; offline the contacts go
         * into one heap block of the same 0x40-byte stride, which is what the array
         * operators walk. */
        if (!ray_ || !heap_) return false;
        const Value& W = a[0];
        const float spring = rf(a[1], 0), attach = rf(a[2], 0), extra = rf(a[3], 0);
        const Value& S = a[4];
        uint32_t count = 0;
        if (S.bytes.size() >= 0x24) std::memcpy(&count, S.bytes.data() + 0x20, 4);
        if (count == 0 || count > 64) return false;
        const float base[3] = {rf(W, 0), rf(W, 4), rf(W, 8)};
        const float mirror = base[0] >= 0.0f ? -1.0f : 1.0f;
        std::vector<uint8_t> all((size_t)count * 0x40, 0);
        for (uint32_t i = 0; i < count; ++i) {
            const float at[3] = {
                base[0] + mirror * (rf(S, 0) + (float)i * rf(S, 0x10)),
                base[1] + mirror * (rf(S, 4) + (float)i * rf(S, 0x14)),
                base[2] + mirror * (rf(S, 8) + (float)i * rf(S, 0x18)),
            };
            std::vector<uint8_t> one;
            cast_wheel_ray(at, rf(W, WC_RADIUS), spring, attach, extra, one);
            std::memcpy(all.data() + (size_t)i * 0x40, one.data(), 0x40);
        }
        if (std::getenv("BF6_TRACK_DEBUG")) {
            int hits = 0;
            for (uint32_t i = 0; i < count; ++i) hits += all[(size_t)i * 0x40 + CT_HASCONTACT] != 0;
            std::fprintf(stderr, "track sampler: %u contacts, %d in contact, mirror %+.0f\n",
                         count, hits, mirror);
        }
        return heap_->alloc(count, 0x40, all.data(), out);
    }
    if (key == kGravity) {
        /* FUN_1443E7F40 copies the world's Vec3 at body block +0x1B0C bit for bit; the
         * graph takes |y| * mass / 4 of it as the static wheel load, so it is gravity. */
        out.bytes.assign(16, 0);
        for (int i = 0; i < 3; ++i) wf(out.bytes, 4 * i, body_.gravity[i]);
        out.known = true;
        return true;
    }
    if (key == kGetCom) {
        /* FUN_1443E37D0: the centre of mass, vehicle-local (the graph stores it and
         * hands it back through 0x71C3128F) */
        out.bytes.assign(16, 0);
        for (int i = 0; i < 3; ++i) wf(out.bytes, 4 * i, body_.com[i]);
        out.known = true;
        return true;
    }
    if (key == kSetCom) {
        /* FUN_1443E3740 -> FUN_1435D0F80: move the body's centre of mass */
        for (int i = 0; i < 3; ++i) body_.com[i] = rf(a[0], 4 * i);
        com_set_ = true;
        out.bytes.clear();
        out.known = true;
        return true;
    }
    if (key == kLocalGravity) {
        /* FUN_1443E79D0: the world gravity (+0x1B0C..) rotated into the body frame and
         * scaled by a component value (+200, not readable offline: 1.0, STAND-IN).
         * PROBABLE: the native also blends a second frame when a 0.6 dot test passes;
         * that branch is not reproduced. */
        const float qc[4] = {-body_.quat[0], -body_.quat[1], -body_.quat[2], body_.quat[3]};
        float g[3];
        qrot(qc, body_.gravity, g);
        out.bytes.assign(16, 0);
        for (int i = 0; i < 3; ++i) wf(out.bytes, 4 * i, g[i]);
        out.known = true;
        return true;
    }
    if (key == kAeroDrag) {
        /* FUN_1443E9430 / FUN_1443E7400 (study_t4_aero): airspeed = local velocity -
         * the world wind turned into the body; per axis an IMPULSE
         * min(m * Drag[i] * a[i]^2 * dt, m * |a[i]|) against a[i] (the cap stops the
         * axis without reversing it), at OffsetYZ / OffsetXZ / OffsetXY (for x / y / z)
         * plus ForcePositionOffset; integrated on the snapshot, change over dt out.
         * Local form (binding word 7 bit 1 set), as for every function here. */
        const float dt = rf(a[0], 0);
        Snapshot s;
        s.mass = body_.mass;
        s.inv_mass = body_.mass != 0.0f ? 1.0f / body_.mass : 0.0f;
        std::memcpy(s.v, body_.v, 16);
        std::memcpy(s.w, body_.w, 16);
        std::memcpy(s.com, body_.com, 16);
        std::memcpy(s.inv_i, body_.inv_inertia, 16);
        const Snapshot before = s;
        const float wind_w[3] = {rf(a[6], 0), rf(a[6], 4), rf(a[6], 8)};
        const float qc[4] = {-body_.quat[0], -body_.quat[1], -body_.quat[2], body_.quat[3]};
        float wind[3];
        qrot(qc, wind_w, wind);
        std::vector<ForceRecord> recs;
        for (int ax = 0; ax < 3; ++ax) {
            const float air = s.v[ax] - wind[ax];
            float mag = s.mass * rf(a[2], 4 * ax) * air * air * dt;
            const float cap = s.mass * std::fabs(air);
            if (cap < mag) mag = cap;
            mag = mag * sgn(-air);
            ForceRecord r{};
            r.f[ax] = mag * force_scale;
            const Value& off = a[3 + ax];
            for (int i = 0; i < 4; ++i) r.p[i] = rf(off, 4 * i) + rf(a[1], 4 * i);
            if (finite3(r.f) && finite3(r.p)) recs.push_back(r);
        }
        apply_all(s, recs);
        out.bytes.assign(32, 0);
        for (int i = 0; i < 4; ++i) wf(out.bytes, 4 * i, (s.w[i] - before.w[i]) / dt);
        for (int i = 0; i < 4; ++i) wf(out.bytes, 16 + 4 * i, (s.v[i] - before.v[i]) / dt);
        out.known = true;
        return true;
    }
    if (key == kBuoyancy) {
        /* FUN_1443EEF00 asks the world for the water height under the body and acts
         * only when bodyPos.y - 50 <= height; above water it zeroes both outputs and
         * pushes nothing. Offline there is no water query: that zero path. */
        out.bytes.assign(32, 0);
        out.known = true;
        return true;
    }
    if (key == kStandStill) {
        /* FUN_1443F50C0 (shard_18.c:460342, study_t4_buoy; the frame settled there):
         * velocities are vehicle-local; the acceleration inputs are rotated into the
         * world (body rotation, local -> world) for the gates only. When the car is
         * nearly at rest on still ground it damps the velocity toward zero and returns
         * the correction over dt; otherwise all outputs are 0. */
        const float dt = rf(a[0], 0);
        const bool allowed = !a[1].bytes.empty() && a[1].bytes[0] != 0;
        float la[4], aa[4], sv[4], svp[4], C[9];
        for (int i = 0; i < 4; ++i) {
            la[i] = rf(a[2], 4 * i); aa[i] = rf(a[3], 4 * i);
            sv[i] = rf(a[4], 4 * i); svp[i] = rf(a[5], 4 * i);
        }
        const int32_t nw = (int32_t)ru(a[6], 0), nc = (int32_t)ru(a[7], 0);
        const float damp = rf(a[8], 0);
        for (int i = 0; i < 9; ++i) C[i] = rf(a[9], 4 * i);
        const float* v = body_.v;
        const float* w = body_.w;
        float factor = 0.0f, ol[4] = {0, 0, 0, 0}, oa[4] = {0, 0, 0, 0};
        bool go = false;
        if (0 < nw && allowed) {
            const float d0 = sv[0] - svp[0], d1 = sv[1] - svp[1], d2 = sv[2] - svp[2];
            if (d1 * d1 + d0 * d0 + d2 * d2 < 0.0025000002f) {
                float wa[3];
                qrot(body_.quat, la, wa);
                if (nc != 0) {
                    const float r0 = v[0] - sv[0], r1 = v[1] - sv[1], r2 = v[2] - sv[2];
                    if (r1 * r1 + r0 * r0 + r2 * r2 < C[4] * C[4] &&
                        w[1] * w[1] + w[0] * w[0] + w[2] * w[2] < C[7] * C[7] * 0.00030461742f) {
                        const float wa2 = wa[1] * wa[1] + wa[0] * wa[0] + wa[2] * wa[2];
                        const float aa2 = aa[1] * aa[1] + aa[0] * aa[0] + aa[2] * aa[2];
                        if (sv[1] * sv[1] + sv[0] * sv[0] + sv[2] * sv[2] <= 1e-06f)
                            go = nc == nw || (0 < nc && -6.0f < wa[1] && wa2 < 225.0f && aa2 < 81.0f);
                        else
                            go = !(nc < 1 || wa[1] <= -6.0f || 2.25f <= wa2 || 0.80999994f <= aa2);
                    }
                }
            }
        }
        if (go) {
            factor = dt + dt + damp;
            if (1.0f <= factor) factor = 1.0f;
            float pl[4], pa[4];
            for (int i = 0; i < 4; ++i) { pl[i] = (v[i] - sv[i]) + dt * la[i]; pa[i] = dt * aa[i] + w[i]; }
            const float kl = 1.0f - (1.0f - std::exp(-(dt * C[8]))) * damp;
            float nl[4] = {pl[0] * kl, pl[1], pl[2] * kl, pl[3]};
            if (std::fabs(nl[0]) < C[6]) nl[0] = 0.0f;
            if (std::fabs(nl[2]) < C[6]) nl[2] = 0.0f;
            float na[4];
            for (int i = 0; i < 3; ++i) na[i] = (1.0f - (1.0f - std::exp(-(dt * C[i]))) * damp) * pa[i];
            na[3] = pa[3];
            const float cut = C[5] * 0.017453292f;
            for (int i = 0; i < 3; ++i) if (std::fabs(na[i]) < cut) na[i] = 0.0f;
            for (int i = 0; i < 4; ++i) { ol[i] = (nl[i] - pl[i]) / dt; oa[i] = (na[i] - pa[i]) / dt; }
        }
        out.bytes.assign(16 + 4 + 16, 0);
        for (int i = 0; i < 4; ++i) wf(out.bytes, 4 * i, oa[i]);
        wf(out.bytes, 16, factor);
        for (int i = 0; i < 4; ++i) wf(out.bytes, 20 + 4 * i, ol[i]);
        out.known = true;
        return true;
    }
    if (key == kAutoBrake) {
        /* FUN_1443F57A0 (complete, study_w_misc): with no input at all, either ramp
         * (AutoBrakeWhenNoInput, or AutoBrakeAtLowSpeed below SpeedLimit) or hold 0;
         * with input, 0 unless in neutral with AutoBrakeInNeutral. The ramp is
         * min(MaxBrakingFactor, MaxBrakingFactor * dt / ApplyTime + previous). */
        const float dt = rf(a[0], 0), thr = rf(a[1], 0), brk = rf(a[2], 0), str = rf(a[3], 0);
        const int32_t gear = (int32_t)ru(a[4], 0);
        const Value& C = a[5];
        const float prev = rf(a[6], 0);
        const float maxf = rf(C, 0), apply = rf(C, 4), limit = rf(C, 8);
        const bool at_low = rb(C, 0x0C) != 0, in_neutral = rb(C, 0x0D) != 0, no_input = rb(C, 0x0E) != 0;
        bool ramp = false;
        float v = 0.0f;
        if (thr == 0.0f && str == 0.0f && brk == 0.0f) {
            if (no_input) ramp = true;
            else {
                const float sp = std::sqrt(body_.v[0] * body_.v[0] + body_.v[1] * body_.v[1] + body_.v[2] * body_.v[2]);
                if (at_low && sp < limit) ramp = true;
                else ramp = gear == 0 && in_neutral;
            }
        } else {
            ramp = gear == 0 && in_neutral;
        }
        if (ramp) {
            const float f = (maxf * dt) / apply + prev;
            v = maxf <= f ? maxf : f;
        }
        out.bytes.assign(4, 0);
        wf(out.bytes, 0, v);
        out.known = true;
        return true;
    }
    if (key == kForceAtPos) {
        /* FUN_1443E7FF0: one force record (FUN_1443EB8E0, tag 10) at Position on the
         * chassis snapshot, integrated (FUN_1443EBAD0); outputs the velocity change
         * over dt. The native rotates the linear part by the body matrix when bit 1 of
         * the component's binding word 7 is CLEAR; the tyre and suspension outputs it
         * is summed with are never rotated, so the local form (bit set) is taken. */
        const float dt = rf(a[0], 0);
        Snapshot s;
        s.mass = body_.mass;
        s.inv_mass = body_.mass != 0.0f ? 1.0f / body_.mass : 0.0f;
        std::memcpy(s.v, body_.v, 16);
        std::memcpy(s.w, body_.w, 16);
        std::memcpy(s.com, body_.com, 16);
        std::memcpy(s.inv_i, body_.inv_inertia, 16);
        const Snapshot before = s;
        ForceRecord r{};
        for (int i = 0; i < 4; ++i) { r.f[i] = dt * rf(a[2], 4 * i) * force_scale; r.p[i] = rf(a[1], 4 * i); }
        std::vector<ForceRecord> recs;
        if (finite3(r.f) && finite3(r.p)) recs.push_back(r);
        /* Every force the graph applies, in the body frame, so a thrust pointing
         * across the hull instead of along it is visible rather than inferred. */
        if (std::getenv("BF6_FORCE_DEBUG") && dt > 0.0f)
            std::fprintf(stderr, "force: (%9.1f %9.1f %9.1f) N at (%5.2f %5.2f %5.2f)\n",
                         r.f[0] / dt, r.f[1] / dt, r.f[2] / dt, r.p[0], r.p[1], r.p[2]);
        apply_all(s, recs);
        out.bytes.assign(32, 0);
        for (int i = 0; i < 4; ++i) wf(out.bytes, 4 * i, (s.w[i] - before.w[i]) / dt);
        for (int i = 0; i < 4; ++i) wf(out.bytes, 16 + 4 * i, (s.v[i] - before.v[i]) / dt);
        out.known = true;
        return true;
    }
    if (key == kContactForce) {
        /* FUN_1443F4840 zeroes its three outputs, then acts only when the contact
         * resolved a surface MATERIAL (FUN_140E53370 non-null) and Resistance != 0.
         * Offline no material resolves, so this is the native's zero path. */
        out.bytes.assign(16 + 16 + 16, 0);
        out.known = true;
        return true;
    }
    if (key == kSuspension) {
        const float dt = rf(a[0], 0);
        const Value& C = a[1];
        const Value& W = a[3];
        const Value& SC = a[6];
        const float K = rf(a[4], 0), D = rf(a[5], 0);
        const float cprev = rf(a[7], 0), cother = rf(a[8], 0);
        const bool contact = C.bytes.size() > CT_HASCONTACT && C.bytes[CT_HASCONTACT] != 0 &&
                             (C.known || (C.known_bytes.size() > CT_HASCONTACT && C.known_bytes[CT_HASCONTACT]));
        Snapshot s;
        s.mass = body_.mass;
        s.inv_mass = body_.mass != 0.0f ? 1.0f / body_.mass : 0.0f;
        std::memcpy(s.v, body_.v, 16);
        std::memcpy(s.w, body_.w, 16);
        std::memcpy(s.com, body_.com, 16);
        std::memcpy(s.inv_i, body_.inv_inertia, 16);
        const Snapshot before = s;
        float compr = 0.0f;
        float ar = anti_roll(K, SC, s, cprev, cother);
        float spring = 0.0f, damper = 0.0f;
        float dir[4] = {0.0f, 1.0f, 0.0f, 0.0f};
        if (!contact) {
            if (0.0f < ar && up_y(body_.quat) < 0.1f) ar = 0.0f;
        } else if (rf(C, CT_NORMAL + 4) <= 0.1f) {
            if (0.0f <= ar) ar = 0.0f;
        } else {
            uint8_t high = 0;
            spring = spring_damper(dt, W, K, D, SC, s, C, cprev, &damper, &compr, &high);
            if (rb(SC, SC_ALWAYS_NORMAL) || rf(C, CT_NORMAL + 4) < 0.6f)
                for (int i = 0; i < 4; ++i) dir[i] = rf(C, CT_NORMAL + 4 * i);
        }
        /* the force point: HandlingOffset + the wheel's InitialPosition, or + the
         * contact position when SuspensionConfig+0x4D is clear and there is contact */
        const uint32_t base = (!rb(SC, SC_FORCE_AT_CONTACT_OFF) && contact) ? CT_POSITION : 0u;
        const Value& PB = base ? C : W;
        ForceRecord r1{}, r2{}, r3{};
        for (int i = 0; i < 4; ++i) {
            const float pt = rf(a[2], 4 * i) + rf(PB, base + 4 * i);
            r1.p[i] = r2.p[i] = r3.p[i] = pt;
            r1.f[i] = dt * (spring * dir[i]) * force_scale;
            r2.f[i] = dt * (damper * dir[i]) * force_scale;
        }
        r3.f[0] = dt * (ar * 0.0f) * force_scale; r3.f[1] = dt * (ar * 1.0f) * force_scale;
        r3.f[2] = dt * (ar * 0.0f) * force_scale; r3.f[3] = dt * (ar * 0.0f) * force_scale;
        std::vector<ForceRecord> recs;
        if (finite3(r1.f) && finite3(r1.p)) recs.push_back(r1);
        if (finite3(r2.f) && finite3(r2.p)) recs.push_back(r2);
        if (0.0f < rf(SC, SC_ANTIROLL) && finite3(r3.f) && finite3(r3.p)) recs.push_back(r3);
        apply_all(s, recs);
        if (std::getenv("BF6_SUSP_DEBUG"))
            std::fprintf(stderr, "susp contact %d P.y %.3f K %.1f D %.1f L %.3f top %.3f W.y %.3f R %.3f -> c %.3f spring %.1f damper %.1f ar %.1f\n",
                         (int)contact, rf(C, CT_POSITION + 4), K, D, rf(SC, SC_TRAVEL), rf(SC, SC_TOP_MOUNT),
                         rf(W, 4), rf(W, WC_RADIUS), compr, spring, damper, ar);
        float total = ar + spring;
        if (0.0f < damper) total = total + damper;
        if (total <= 0.0f) total = 0.0f;
        out.bytes.assign(4 + 16 + 16 + 4, 0);
        wf(out.bytes, 0, compr);
        for (int i = 0; i < 4; ++i) wf(out.bytes, 4 + 4 * i, (s.v[i] - before.v[i]) / dt);
        for (int i = 0; i < 4; ++i) wf(out.bytes, 20 + 4 * i, (s.w[i] - before.w[i]) / dt);
        wf(out.bytes, 36, total);
        out.known = true;
        return true;
    }
    if (key != kTyreForce || a.size() < 13 || !g_table_ok) return false;
    const float dt = rf(a[0], 0), steer = rf(a[1], 0), brake = rf(a[2], 0), torque = rf(a[3], 0);
    const Value& C = a[4];
    const Value& W = a[12];
    /* an unwritten flag byte is "no contact" (see kTyre) */
    const bool contact = C.bytes.size() > CT_HASCONTACT && C.bytes[CT_HASCONTACT] != 0 &&
                         (C.known || (C.known_bytes.size() > CT_HASCONTACT && C.known_bytes[CT_HASCONTACT]));

    Snapshot s;
    s.mass = body_.mass;
    s.inv_mass = body_.mass != 0.0f ? 1.0f / body_.mass : 0.0f;
    std::memcpy(s.v, body_.v, 16);
    std::memcpy(s.w, body_.w, 16);
    std::memcpy(s.com, body_.com, 16);
    std::memcpy(s.inv_i, body_.inv_inertia, 16);
    const Snapshot before = s;

    WheelState st;
    st.load = rf(a[5], 0);
    SlipSlots sl;
    sl.status = (int32_t)ru(a[7], 0);
    sl.omega = rf(a[6], 0);
    sl.ratio_sum = rf(a[8], 0);
    sl.angle_sum = rf(a[9], 0);

    float slip_ratio = 0.0f, slip_angle = 0.0f, f_long = 0.0f, f_lat = 0.0f;
    std::vector<ForceRecord> recs;
    /* ---- FUN_1443EFCB0 ---- */
    if (!contact) {
        slip_angle = 0.0f; slip_ratio = 0.0f; sl.status = 0;
    } else {
        float P[4], SV[4];
        for (int i = 0; i < 4; ++i) { P[i] = rf(C, CT_POSITION + 4 * i); SV[i] = rf(C, CT_SURFVEL + 4 * i); }
        st.vel[0] = (((P[2] - s.com[2]) * s.w[1] - (P[1] - s.com[1]) * s.w[2]) + s.v[0]) - SV[0];
        st.vel[1] = (((P[0] - s.com[0]) * s.w[2] - (P[2] - s.com[2]) * s.w[0]) + s.v[1]) - SV[1];
        st.vel[2] = (((P[1] - s.com[1]) * s.w[0] - (P[0] - s.com[0]) * s.w[1]) + s.v[2]) - SV[2];
        st.vel[3] = (((P[3] - s.com[3]) * s.w[3] - (P[3] - s.com[3]) * s.w[3]) + s.v[3]) - SV[3];
        float n[4];
        for (int i = 0; i < 4; ++i) n[i] = rf(C, CT_NORMAL + 4 * i);
        tyre_frame(rf(W, WC_RADIUS), steer, sl.omega, n, st, &slip_angle, &slip_ratio);
        run_sum(sl.angle_sum, sl.angle_window, slip_angle);
        run_sum(sl.ratio_sum, sl.ratio_window, slip_ratio);
    }
    if (torque != 0.0f) sl.omega = (dt * torque) / rf(W, WC_INERTIA) + sl.omega;
    if (!contact) {
        f_long = 0.0f;
    } else {
        friction(W, WC_FRICTION, s.mass, brake, st, sl, &f_long, &f_lat);
        /* ---- FUN_1443F05E0: two force records ---- */
        float P[4], H[4], LH[4];
        for (int i = 0; i < 4; ++i) {
            P[i] = rf(C, CT_POSITION + 4 * i);
            H[i] = rf(a[10], 4 * i);
            LH[i] = rf(a[11], 4 * i);
        }
        ForceRecord r1{}, r2{};
        for (int i = 0; i < 4; ++i) {
            r1.f[i] = f_long * st.long_axis[i] * dt * force_scale;
            r1.p[i] = LH[i] + P[i];
            r2.f[i] = dt * (0.0f - st.lat_axis[i]) * f_lat * force_scale;
            r2.p[i] = P[i] + H[i];
        }
        if (finite3(r1.f) && finite3(r1.p)) recs.push_back(r1);
        if (finite3(r2.f) && finite3(r2.p)) recs.push_back(r2);
        /* friction torque back on the wheel, no overshoot through rolling speed */
        const float radius = rf(W, WC_RADIUS);
        float ft = f_long * radius * rf(W, WC_FRICTION + WF_TORQUE_MULT) * -0.0625f;
        st.friction_torque = ft;
        const float roll = st.v_long / radius;
        const float s_before = sgn(sl.omega - roll);
        float om = (ft * dt) / rf(W, WC_INERTIA) + sl.omega;
        sl.omega = om;
        om = om - roll;
        float s_after = 0.0f;
        if (-0.0f < std::fabs(om)) s_after = om < 0.0f ? -1.0f : 1.0f;
        if (s_after != s_before) sl.omega = roll;
    }
    if (std::getenv("BF6_TYRE_DEBUG"))
        std::fprintf(stderr, "tyre contact %d torque %.1f brake %.2f steer %.3f w_in %.3f w_out %.3f load %.1f "
                     "status %d ratio %.3f angle %.3f Flong %.1f Flat %.1f vlong %.3f at (%.2f %.2f)\n",
                     (int)contact, torque, brake, steer, rf(a[6], 0), sl.omega, st.load, sl.status, slip_ratio,
                     slip_angle, f_long, f_lat, st.v_long, rf(W, 0), rf(W, 8));
    tyre_omega_.push_back(sl.omega);
    tyre_contact_.push_back(contact ? 1 : 0);
    /* ---- FUN_1443EBAD0 on the snapshot, then the velocity change over dt ---- */
    apply_all(s, recs);

    out.bytes.assign(4 + 16 + 16 + 4 * 6, 0);
    wf(out.bytes, 0, f_long);
    uint32_t at = 4;
    for (int i = 0; i < 4; ++i) wf(out.bytes, at + 4 * i, (s.v[i] - before.v[i]) / dt);
    at += 16;
    for (int i = 0; i < 4; ++i) wf(out.bytes, at + 4 * i, (s.w[i] - before.w[i]) / dt);
    at += 16;
    wf(out.bytes, at, sl.omega); at += 4;
    wu(out.bytes, at, (uint32_t)sl.status); at += 4;
    wf(out.bytes, at, sl.ratio_sum); at += 4;
    wf(out.bytes, at, sl.angle_sum); at += 4;
    wf(out.bytes, at, slip_ratio); at += 4;
    wf(out.bytes, at, slip_angle);
    out.known = true;
    return true;
}

/* THE LOAD TABLE, read from the installed executable: FUN_1443F0090 lerps rows
 * 0..19 of a 16-byte-row table at 0x1489FBEF0 (current exe). A game update can move
 * it, so the rows are checked for the table's shape (lanes 0/1 rising in (0,1),
 * lanes 2/3 rising gains in (1,1.2)) and the tyre force is refused, loudly, if they
 * do not fit. */
bool wheel_ops_load_table(const std::string& exe, std::string& err) {
    std::FILE* f = std::fopen(exe.c_str(), "rb");
    if (!f) { err = "cannot open " + exe; return false; }
    std::vector<uint8_t> hdr(0x1000);
    const size_t got = std::fread(hdr.data(), 1, hdr.size(), f);
    auto r32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= got) std::memcpy(&v, hdr.data() + o, 4); return v; };
    const uint32_t pe = r32(0x3c);
    const uint16_t nsec = (uint16_t)r32(pe + 6), opt = (uint16_t)r32(pe + 20);
    uint64_t base = 0;
    if (pe + 56 <= got) std::memcpy(&base, hdr.data() + pe + 48, 8);
    const uint64_t va = 0x1489FBEF0ull;
    long off = -1;
    for (uint16_t i = 0; i < nsec; ++i) {
        const size_t at = pe + 24 + opt + i * 40u;
        const uint32_t vs = r32(at + 8), rva = r32(at + 12), rs = r32(at + 16), ro = r32(at + 20);
        if (va >= base + rva && va + sizeof g_table <= base + rva + (vs < rs ? vs : rs))
            off = (long)(ro + (va - base - rva));
    }
    if (off < 0) { std::fclose(f); err = "tyre load table VA not in any section"; return false; }
    std::fseek(f, off, SEEK_SET);
    float t[20][4];
    const bool ok = std::fread(t, 1, sizeof t, f) == sizeof t;
    std::fclose(f);
    if (!ok) { err = "tyre load table short read"; return false; }
    for (int r = 0; r < 20; ++r) {
        const bool rising = r == 0 || (t[r][0] >= t[r - 1][0] && t[r][2] >= t[r - 1][2]);
        if (!(rising && t[r][0] > 0.0f && t[r][0] < 1.0f && t[r][1] > 0.0f && t[r][1] < 1.0f &&
              t[r][2] > 1.0f && t[r][2] < 1.2f && t[r][3] > 1.0f && t[r][3] < 1.2f)) {
            err = "tyre load table at 0x1489FBEF0 does not have its shape (game updated?)";
            return false;
        }
    }
    std::memcpy(g_table, t, sizeof t);
    g_table_ok = true;
    return true;
}

} // namespace expression
} // namespace bf6
