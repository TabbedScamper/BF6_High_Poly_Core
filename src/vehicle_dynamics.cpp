#include "vehicle_dynamics.h"

#include <cmath>
#include <cstdio>

namespace bf6 {

namespace {
void qrot(const float q[4], const float v[3], float out[3]) {
    /* v' = q v q^-1 */
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2.0f * (y * v[2] - z * v[1]);
    const float ty = 2.0f * (z * v[0] - x * v[2]);
    const float tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}
void qconj(const float q[4], float out[4]) { out[0] = -q[0]; out[1] = -q[1]; out[2] = -q[2]; out[3] = q[3]; }
void cross(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}
float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
}

void VehicleDynamics::init() {
    /* DERIVED: the stiffness that holds the authored ride height. The suspension
     * graph's compression is 0 at the rig's rest pose and the spring's free length
     * sits at full droop, so one quarter of the weight at compression 0 needs
     * k = m g / (4 droop). */
    p.spring_k = p.mass * p.gravity / (4.0f * p.droop);
    for (auto& w : p.wheel) w.steered = w.mount[2] > 0.0f;
}

float VehicleDynamics::forward_speed() const {
    const float f[3] = {0, 0, 1};
    float fw[3];
    qrot(quat, f, fw);
    return dot(vel, fw);
}

void VehicleDynamics::root_rows(float out[16]) const {
    const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1};
    float r[3], u[3], f[3];
    qrot(quat, ex, r); qrot(quat, ey, u); qrot(quat, ez, f);
    const float rows[16] = {r[0], r[1], r[2], 0, u[0], u[1], u[2], 0, f[0], f[1], f[2], 0,
                            pos[0], pos[1], pos[2], 1};
    for (int i = 0; i < 16; ++i) out[i] = rows[i];
    /* BF6_ROOT_TRANSPOSE=1 publishes the basis transposed. The rotor's cyclic loop is
     * positive feedback offline - the cyclic's own output moves the force application
     * point, which rotates the aircraft, which commands more cyclic - and a basis whose
     * rows and columns are swapped is one way a feedback sign inverts. The cars and tanks
     * are the control: they work, so if transposing breaks them this convention is right
     * and the sign error is elsewhere. Diagnostic only. */
    if (std::getenv("BF6_ROOT_TRANSPOSE"))
        for (int i = 0; i < 3; ++i)
            for (int j = i + 1; j < 3; ++j) {
                const float t = out[i * 4 + j];
                out[i * 4 + j] = out[j * 4 + i];
                out[j * 4 + i] = t;
            }
}

void VehicleDynamics::step(float dt, const VehicleDriveInputs& in) {
    /* Substeps: the wheel/tyre coupling is stiff; the graph inputs hold for the frame. */
    const int n = 8;
    for (int i = 0; i < n; ++i) substep(dt / (float)n, in);
}

void VehicleDynamics::substep(float dt, const VehicleDriveInputs& in) {
    if (dt <= 0.0f) return;
    const float m = p.mass;
    float force[3] = {0.0f, -m * p.gravity, 0.0f};
    float torque[3] = {0, 0, 0};

    const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1};
    float right[3], up[3], fwd[3];
    qrot(quat, ex, right); qrot(quat, ey, up); qrot(quat, ez, fwd);

    /* Drive torque at the wheels: engine torque x throttle x gear ratio, through the
     * clutch, split over the driven wheels. PLACEHOLDER engine curve (flat). */
    int driven = 0;
    for (const auto& w : p.wheel) driven += w.driven ? 1 : 0;
    const float engaged = clampf(1.0f - in.clutch, 0.0f, 1.0f);
    const float drive_total = p.engine_peak_torque * clampf(in.throttle, 0.0f, 1.0f) * in.gear_ratio * engaged;
    /* SteeringAngle is the WHEEL ANGLE in radians: the graph converts DegreesToRadians
     * just before publishing it (max 45 deg from its own speed curve). */
    const float steer = clampf(in.steer, -1.2f, 1.2f);

    /* world centre of mass */
    float comw[3];
    qrot(quat, p.com, comw);
    for (int i = 0; i < 3; ++i) comw[i] += pos[i];

    for (int wi = 0; wi < 4; ++wi) {
        const VehicleWheelParams& w = p.wheel[wi];
        fz[wi] = 0.0f;
        /* contact point: below the wheel centre by the radius minus compression */
        float mw[3];
        qrot(quat, w.mount, mw);
        float centre[3] = {pos[0] + mw[0], pos[1] + mw[1], pos[2] + mw[2]};
        /* compression from the GAME's suspension graph when it ran, else from the
         * flat-ground geometry (y = 0) as a fallback */
        float comp = in.compression_known[wi] ? in.compression[wi] : (w.radius - centre[1]);
        comp = clampf(comp, -p.droop, p.bump);
        const float x = comp + p.droop;             /* spring extension from free length */
        if (x <= 0.0f) { omega[wi] *= 0.999f; continue; }  /* wheel off the ground */
        float contact[3] = {centre[0] - up[0] * w.radius, centre[1] - up[1] * w.radius,
                            centre[2] - up[2] * w.radius};
        float rc[3] = {contact[0] - comw[0], contact[1] - comw[1], contact[2] - comw[2]};
        float wxr[3];
        cross(angvel, rc, wxr);
        const float vc[3] = {vel[0] + wxr[0], vel[1] + wxr[1], vel[2] + wxr[2]};
        /* suspension: spring + damper along the body up axis */
        const float c = 2.0f * p.damping_ratio * std::sqrt(p.spring_k * m * 0.25f);
        float n = p.spring_k * x - c * dot(vc, up);
        if (n < 0.0f) n = 0.0f;
        fz[wi] = n;
        /* wheel frame: steered forward axis */
        float wf[3] = {fwd[0], fwd[1], fwd[2]}, wr[3] = {right[0], right[1], right[2]};
        if (w.steered) {
            const float cs = std::cos(steer), sn = std::sin(steer);
            for (int i = 0; i < 3; ++i) {
                wf[i] = fwd[i] * cs + right[i] * sn;
                wr[i] = right[i] * cs - fwd[i] * sn;
            }
        }
        const float vlong = dot(vc, wf), vlat = dot(vc, wr);
        /* tyre: slip-based, saturating at mu*Fz (PLACEHOLDER curve) */
        const float denom = std::fmax(std::fabs(vlong), 1.0f);
        const float slip = (omega[wi] * w.radius - vlong) / denom;
        const float alpha = std::atan2(vlat, std::fmax(std::fabs(vlong), 0.5f));
        const float fmax = p.tyre_mu * n;
        float fx = clampf(p.tyre_stiffness * slip, -1.0f, 1.0f) * fmax;
        float fy = -clampf(p.tyre_stiffness * alpha, -1.0f, 1.0f) * fmax;
        const float mag = std::sqrt(fx * fx + fy * fy);
        if (mag > fmax && mag > 0.0f) { fx *= fmax / mag; fy *= fmax / mag; }
        /* wheel spin: drive - tyre reaction - brake. The tyre reaction is limited so
         * one step cannot carry the wheel past rolling (omega*r == vlong): with a
         * light wheel and a stiff tyre, an explicit step overshoots and pumps energy
         * into the body, which is what the first version did. */
        float tq = w.driven && driven ? drive_total / (float)driven : 0.0f;
        const float roll = vlong / w.radius;
        const float tyre_tq = fx * w.radius;
        const float max_tyre = std::fabs(omega[wi] - roll) * p.wheel_inertia / dt;
        tq -= clampf(tyre_tq, -max_tyre, max_tyre);
        const float brake = p.brake_torque * clampf(in.brake, 0.0f, 1.0f) +
                            ((in.handbrake && !w.steered) ? p.brake_torque : 0.0f);
        float new_omega = omega[wi] + tq / p.wheel_inertia * dt;
        const float bstep = brake / p.wheel_inertia * dt;
        if (std::fabs(new_omega) <= bstep) new_omega = 0.0f;
        else new_omega -= bstep * (new_omega > 0 ? 1.0f : -1.0f);
        omega[wi] = new_omega;
        /* forces on the body at the contact */
        const float f[3] = {up[0] * n + wf[0] * fx + wr[0] * fy,
                            up[1] * n + wf[1] * fx + wr[1] * fy,
                            up[2] * n + wf[2] * fx + wr[2] * fy};
        float t[3];
        cross(rc, f, t);
        for (int i = 0; i < 3; ++i) { force[i] += f[i]; torque[i] += t[i]; }
    }

    /* integrate (semi-implicit Euler). Inertia is diagonal in body axes. */
    for (int i = 0; i < 3; ++i) last_accel[i] = force[i] / m;
    for (int i = 0; i < 3; ++i) vel[i] += last_accel[i] * dt;
    float qi[4];
    qconj(quat, qi);
    float tb[3], wb[3];
    qrot(qi, torque, tb);
    qrot(qi, angvel, wb);
    for (int i = 0; i < 3; ++i) wb[i] += tb[i] / (m * p.inertia_per_kg[i]) * dt;
    qrot(quat, wb, angvel);
    /* the body frame moves with the centre of mass */
    float comw2[3];
    for (int i = 0; i < 3; ++i) comw2[i] = comw[i] + vel[i] * dt;
    const float wx = angvel[0], wy = angvel[1], wz = angvel[2];
    const float dq[4] = {0.5f * dt * (wx * quat[3] + wy * quat[2] - wz * quat[1]),
                         0.5f * dt * (wy * quat[3] + wz * quat[0] - wx * quat[2]),
                         0.5f * dt * (wz * quat[3] + wx * quat[1] - wy * quat[0]),
                         0.5f * dt * (-wx * quat[0] - wy * quat[1] - wz * quat[2])};
    float len = 0.0f;
    for (int i = 0; i < 4; ++i) { quat[i] += dq[i]; len += quat[i] * quat[i]; }
    len = std::sqrt(len);
    for (int i = 0; i < 4; ++i) quat[i] /= len;
    float comb[3];
    qrot(quat, p.com, comb);
    for (int i = 0; i < 3; ++i) pos[i] = comw2[i] - comb[i];
}

void VehicleDynamics::integrate_body_accel(float dt, const float lin_body[3], const float ang_body[3],
                                           bool add_gravity) {
    float comb[3], comw[3];
    qrot(quat, p.com, comb);
    for (int i = 0; i < 3; ++i) comw[i] = pos[i] + comb[i];
    float aw[3], alw[3];
    qrot(quat, lin_body, aw);
    qrot(quat, ang_body, alw);
    if (add_gravity) aw[1] -= p.gravity;
    for (int i = 0; i < 3; ++i) { last_accel[i] = aw[i]; vel[i] += aw[i] * dt; angvel[i] += alw[i] * dt; }
    float comw2[3];
    for (int i = 0; i < 3; ++i) comw2[i] = comw[i] + vel[i] * dt;
    const float wx = angvel[0], wy = angvel[1], wz = angvel[2];
    const float dq[4] = {0.5f * dt * (wx * quat[3] + wy * quat[2] - wz * quat[1]),
                         0.5f * dt * (wy * quat[3] + wz * quat[0] - wx * quat[2]),
                         0.5f * dt * (wz * quat[3] + wx * quat[1] - wy * quat[0]),
                         0.5f * dt * (-wx * quat[0] - wy * quat[1] - wz * quat[2])};
    float len = 0.0f;
    for (int i = 0; i < 4; ++i) { quat[i] += dq[i]; len += quat[i] * quat[i]; }
    len = std::sqrt(len);
    for (int i = 0; i < 4; ++i) quat[i] /= len;
    qrot(quat, p.com, comb);
    for (int i = 0; i < 3; ++i) pos[i] = comw2[i] - comb[i];
}

std::string VehicleDynamics::describe_params() const {
    char b[1024];
    std::snprintf(b, sizeof b,
        "dynamics: mass %g (DATA) com (%g %g %g) (DATA) inertia/kg (%g %g %g) (DATA)\n"
        "  wheels FL (%g %g %g) FR (%g %g %g) RL (%g %g %g) RR (%g %g %g) radius %g (DATA)\n"
        "  spring k %g (DERIVED m g / 4 droop, droop %g DATA) damping ratio %g, tyre mu %g, stiffness %g,\n"
        "  engine %g N m, brake %g N m, wheel inertia %g, max steer %g rad (PLACEHOLDER)\n",
        p.mass, p.com[0], p.com[1], p.com[2], p.inertia_per_kg[0], p.inertia_per_kg[1], p.inertia_per_kg[2],
        p.wheel[0].mount[0], p.wheel[0].mount[1], p.wheel[0].mount[2], p.wheel[1].mount[0], p.wheel[1].mount[1],
        p.wheel[1].mount[2], p.wheel[2].mount[0], p.wheel[2].mount[1], p.wheel[2].mount[2], p.wheel[3].mount[0],
        p.wheel[3].mount[1], p.wheel[3].mount[2], p.wheel[0].radius, p.spring_k, p.droop, p.damping_ratio,
        p.tyre_mu, p.tyre_stiffness, p.engine_peak_torque, p.brake_torque, p.wheel_inertia, p.max_steer_rad);
    return b;
}

} // namespace bf6
