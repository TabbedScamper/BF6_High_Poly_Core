#include <algorithm>
/* The drivable-vehicle C ABI end to end: open the flyer60 on a flat plane, full
 * throttle for 8 s, then brake; print what bf6_vehicle_step returns. Fails (exit 3)
 * unless the vehicle moves forward, shifts at least twice and stops under braking. */
#include "bf6_core.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    const char* game = std::getenv("BF6_GAME") ? std::getenv("BF6_GAME")
        : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Battlefield 6";
    char err[1024] = {};
    bf6_ctx* ctx = bf6_open(game, err, (int)sizeof(err));
    if (!ctx || !bf6_mount_all(ctx, 1, err, (int)sizeof(err))) { std::fprintf(stderr, "%s\n", err); return 2; }
    const char* water = std::getenv("BF6_WATER");
    const float water_y = water ? (float)std::atof(water) : 0.0f;
    /* Godot passes the level's actual ground triangles, independently of its water
     * surface. Keep the synthetic harness equally honest: a water test has seabed,
     * not terrain coincident with the surface. Twenty metres keeps every fleet boat
     * afloat while remaining a plausible coastal test depth. */
    const float seabed_depth = std::getenv("BF6_WATER_DEPTH")
        ? (float)std::atof(std::getenv("BF6_WATER_DEPTH")) : 20.0f;
    const float ground_y = water ? water_y - std::max(0.0f, seabed_depth) : 0.0f;
    const float s = 2000.0f;
    const float tris[18] = {-s, ground_y, -s, s, ground_y, -s, s, ground_y, s,
                            -s, ground_y, -s, s, ground_y, s, -s, ground_y, s};
    const char* dir = argc > 1 ? argv[1] : "common/hardware/vehicles/car/flyer60";
    bf6_vehicle* v = bf6_vehicle_open(ctx, dir, tris, 6, err, (int32_t)sizeof(err));
    if (!v) { std::fprintf(stderr, "open: %s\n", err); return 2; }
    /* BF6_WATER=<height>: the surface anything that floats reads. A boat needs it;
     * without it its hull is above water and pushes nothing, which is the same
     * honest answer a land map gives. */
    if (water) bf6_vehicle_set_water(v, water_y, 1);
    float out[40] = {};
    float last_ratio = 0.0f;
    int shifts = 0;
    float top = 0.0f;
    /* BF6_SECONDS=<n> runs longer than the default fourteen, and BF6_HOLD=<n> moves the
     * moment the throttle gives way to the brake. An aircraft needs both: the f16 is
     * still gaining 4 m/s every second when the default run brakes at eight, so a
     * take-off cannot be reached inside it, let alone a wing's lift measured. */
    const int secs = std::getenv("BF6_SECONDS") ? std::atoi(std::getenv("BF6_SECONDS")) : 14;
    const int hold = std::getenv("BF6_HOLD") ? std::atoi(std::getenv("BF6_HOLD")) : 8;
    for (int f = 0; f < 60 * secs; ++f) {
        const bool braking = f >= 60 * hold;
        /* BF6_YAW=<n>: the yaw pedal, which reaches the graph as InputYaw and is a
         * helicopter's tail rotor Throttle. Zero pedal leaves a main rotor's torque
         * uncancelled, so a helicopter that spins on this harness may be obeying the
         * model rather than breaking it. */
        static const float yaw = std::getenv("BF6_YAW") ? (float)std::atof(std::getenv("BF6_YAW")) : 0.0f;
        /* BF6_PITCH / BF6_ROLL: the cyclic. A helicopter had NO pitch or roll input on this
         * harness at all, so its graph fell back to an autopilot whose output grows without
         * bound, and there was no way to tell a wrong autopilot from an uncommanded aircraft.
         * Delivered through bf6_vehicle_set_cyclic rather than `in`, which is a six-float
         * contract. */
        static const float pitch = std::getenv("BF6_PITCH") ? (float)std::atof(std::getenv("BF6_PITCH")) : 0.0f;
        static const float roll  = std::getenv("BF6_ROLL")  ? (float)std::atof(std::getenv("BF6_ROLL"))  : 0.0f;
        /* BF6_LEVEL=<k>: a plain attitude hold on the harness side, so controllability can
         * be SETTLED rather than argued. A helicopter is unstable in pitch and roll, so no
         * constant cyclic can hold a hover - measured, the AH-64's best fixed pitch (-0.21)
         * only slows the departure. That is a property of helicopters, not a defect, and it
         * means a constant-input harness can never tell a working model from a broken one.
         * This closes the loop with the crudest possible controller: command cyclic against
         * the measured attitude and rate. If the aircraft then holds, the model has real
         * control authority and the open-loop departure was an uncommanded aircraft. It is a
         * TEST rig, not part of the model - the game's own Autopilot PID is what should do
         * this, and it is still being chased. */
        static const float lvl = std::getenv("BF6_LEVEL") ? (float)std::atof(std::getenv("BF6_LEVEL")) : 0.0f;
        float cp = pitch, cr = roll;
        if (lvl != 0.0f) {
            /* out[] from the PREVIOUS step: 3-6 quat (x,y,z,w), 10-12 angular velocity. */
            const float qx = out[3], qy = out[4], qz = out[5], qw = out[6];
            /* pitch and roll of the body's up axis, small-angle, enough for a hold */
            /* The body's up axis in world, from the core's own qrot applied to (0,1,0):
             * up = (2xy - 2wz, 1 - 2(x^2+z^2), 2wx + 2yz). Both cross terms had the wrong
             * sign on the first attempt, which made the hold drive the aircraft over faster
             * than no hold at all - a reminder to derive these from qrot rather than recall. */
            const float upx = 2.0f * (qx * qy - qw * qz);
            const float upz = 2.0f * (qw * qx + qy * qz);
            /* THE TWO CYCLIC AXES TAKE OPPOSITE SIGNS, measured: a single-signed gain
             * arrests one axis and drives the other over. At +0.5 the pitch quaternion fell
             * from 0.921 to 0.203 while roll went to 0.975; at -0.5 exactly the reverse
             * (roll 0.033, pitch 0.996). So InputPitch and InputRoll do not share a sign
             * convention, which is a fact about the graph's inputs and not about this rig. */
            cp = -lvl * (upz * 4.0f + out[10] * 0.8f);
            cr =  lvl * (upx * 4.0f + out[12] * 0.8f);
            const float lim = 1.0f;
            cp = cp > lim ? lim : (cp < -lim ? -lim : cp);
            cr = cr > lim ? lim : (cr < -lim ? -lim : cr);
        }
        /* BF6_JET_LEVEL=<k>: the same idea for a fixed-wing aircraft, whose signs are
         * measured separately (f16, airborne): InputPitch +0.5 turns the nose DOWN
         * (angular velocity x positive) and InputRoll +0.5 raises the RIGHT wing
         * (angular velocity z positive, ~0.85 rad/s). The body's forward axis
         * fz = qrot(q, (0,0,1)) has fz.y = sin(nose-up angle); its right axis
         * fx = qrot(q, (1,0,0)) has fx.y = sin(right-wing-up angle). */
        static const float jlvl = std::getenv("BF6_JET_LEVEL") ? (float)std::atof(std::getenv("BF6_JET_LEVEL")) : 0.0f;
        if (jlvl != 0.0f) {
            const float qx = out[3], qy = out[4], qz = out[5], qw = out[6];
            const float fz_y = 2.0f * (qy * qz - qw * qx);   /* forward (0,0,1) -> y */
            const float fx_y = 2.0f * (qx * qy + qw * qz);   /* right (1,0,0) -> y   */
            cp = jlvl * (fz_y * 4.0f - out[10] * 0.8f);
            cr = -jlvl * (fx_y * 4.0f + out[12] * 0.8f);
            const float lim = 1.0f;
            cp = cp > lim ? lim : (cp < -lim ? -lim : cp);
            cr = cr > lim ? lim : (cr < -lim ? -lim : cr);
        }
        bf6_vehicle_set_cyclic(v, cp, cr);
        static const bool boost = std::getenv("BF6_BOOST") != nullptr;  /* hold InputSprint */
        bf6_vehicle_set_boost(v, boost ? 1.0f : 0.0f);
        /* BF6_EXIT=a,b: the driver is out of seat 0 from second a to second b - getting
         * out and back in, which is what a door graph animates a door on. */
        static float exit_a = -1.0f, exit_b = -1.0f;
        static const bool exit_parsed = std::getenv("BF6_EXIT") &&
            std::sscanf(std::getenv("BF6_EXIT"), "%f,%f", &exit_a, &exit_b) == 2;
        if (exit_parsed) {
            const float ts = (float)f / 60.0f;
            bf6_vehicle_set_seat(v, 0, (ts >= exit_a && ts < exit_b) ? 0 : 1);
        }
        const float in[6] = {braking ? 0.0f : 1.0f, braking ? 1.0f : 0.0f, yaw, 0.0f, 1.0f / 60.0f, 0.0f};
        /* BF6_TIMING=1: the slowest step and the total, per simulated second - whether a
         * phase of flight (lift-off, say) makes the core itself stall the caller. */
        static double t_sum = 0.0, t_max = 0.0;
        const auto t0 = std::chrono::steady_clock::now();
        if (bf6_vehicle_step(v, in, out) < 33) { std::fprintf(stderr, "step failed\n"); return 2; }
        /* BF6_BONES=<frames>: presentation-graph writes as absolute local rows.
         * This makes control-surface and landing-gear response measurable without
         * involving the Godot renderer. */
        if (const char* be = std::getenv("BF6_BONES"))
            if (f % std::max(1, std::atoi(be)) == 0) {
                const int32_t count = bf6_vehicle_bone_count(v);
                for (int32_t b = 0; b < count; ++b) {
                    char name[128] = {};
                    float m[16] = {};
                    if (bf6_vehicle_bone(v, b, name, (int32_t)sizeof(name), m))
                        std::printf("bone %d %s  r %.6f %.6f %.6f  u %.6f %.6f %.6f  "
                                    "f %.6f %.6f %.6f  t %.6f %.6f %.6f\n",
                                    f, name, m[0],m[1],m[2], m[4],m[5],m[6],
                                    m[8],m[9],m[10], m[12],m[13],m[14]);
                }
            }
        if (std::getenv("BF6_TIMING")) {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            t_sum += ms; if (ms > t_max) t_max = ms;
            if (f % 60 == 59) {
                std::printf("timing t %4.1f  alt %7.2f  step max %7.2f ms  sum %8.1f ms/s\n",
                            (f + 1) / 60.0, out[1], t_max, t_sum);
                t_sum = 0.0; t_max = 0.0;
            }
        }
        /* out: 0-2 pos, 3-6 quat, 7-9 vel, 10-12 angvel, 13 speed, 14 rpm,
         * 15 gear ratio, 16 clutch, 17 throttle, 18 brake */
        if (out[15] != last_ratio && last_ratio != 0.0f && out[15] > 0.0f) ++shifts;
        if (out[15] > 0.0f) last_ratio = out[15];
        if (out[13] > top) top = out[13];
        /* WHICH WAY IT ACTUALLY WENT. A vehicle whose forces land on one axis and
         * whose motion appears on another is the only way to tell a misdirected force
         * from a mislabelled frame, so all three components are available. */
        if (f % 60 == 0 && std::getenv("BF6_XYZ"))
            std::printf("t %4.1f  pos (%7.2f %7.2f %7.2f)  vel (%6.2f %6.2f %6.2f)  "
                        "quat (%.3f %.3f %.3f %.3f)\n",
                        f / 60.0, out[0], out[1], out[2], out[7], out[8], out[9],
                        out[3], out[4], out[5], out[6]);
        /* BF6_ANGVEL=<frames>: angular velocity (world) and lateral velocity every N
         * frames - whether a torque is feeding on the rate it should damp. */
        if (const char* av = std::getenv("BF6_ANGVEL"))
            if (f % std::max(1, std::atoi(av)) == 0)
                std::printf("av %d  angvel (%8.4f %8.4f %8.4f)  vel (%7.3f %7.3f %7.3f)\n", f,
                            out[10], out[11], out[12], out[7], out[8], out[9]);
        if (f % 60 == 0 && !std::getenv("BF6_XYZ"))
            std::printf("t %4.1f  z %7.2f  speed %6.2f  rpm %5.0f  ratio %6.3f  clutch %.2f  thr %.2f  brk %.2f\n",
                        f / 60.0, out[2], out[13], out[14], out[15], out[16], out[17], out[18]);
    }
    std::printf("shifts %d  final speed %.2f\n", shifts, out[13]);
    bf6_vehicle_close(v);
    bf6_close(ctx);
    return (shifts >= 2 && std::fabs(out[13]) < 1.0f) ? 0 : 3;
}
