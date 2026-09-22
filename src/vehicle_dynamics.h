/* A rigid-body step that closes the loop around a vehicle's own graphs.
 *
 * WHAT IS THE GAME'S AND WHAT IS NOT. The game's expression graphs decide the
 * throttle, brake, gear ratio, clutch, steering and each wheel's suspension
 * compression (VehicleSim runs them). What turns those into wheel spin and chassis
 * motion is native code that has not been located yet. This module stands in for
 * it, and every parameter says where its value comes from:
 *   DATA         read from the vehicle's assets (asset and field named)
 *   DERIVED      computed from DATA by a stated rule
 *   PLACEHOLDER  a stand-in until the native vehicle config is read; replace, do
 *                not tune
 * The outputs go back through the same public channels the native code writes
 * (AngularVelocity_Wheel_*, LinearVelocity, AngularVelocity, LinearAcceleration,
 * RootTransform), so swapping in the real model later changes nothing upstream. */
#ifndef BF6_VEHICLE_DYNAMICS_H
#define BF6_VEHICLE_DYNAMICS_H

#include <cstdint>
#include <string>

namespace bf6 {

struct VehicleWheelParams {
    float mount[3] = {0, 0, 0};    /* DATA: rig locator loc_Wheel_*, body frame   */
    float radius = 0.47f;          /* DATA: wheel collision shape half-extent     */
    bool driven = true;            /* PLACEHOLDER: all-wheel drive assumed        */
    bool steered = false;          /* DERIVED: front axle (mount z > 0)           */
};

struct VehicleDynamicsParams {
    float mass = 2441.0f;                       /* DATA: vb_* PhysicsConfigData 0x6e58e5b7 */
    float com[3] = {0.0f, 0.3f, 0.15f};         /* DATA: 0x2754aac9                         */
    float inertia_per_kg[3] = {1.7951f, 1.7951f, 0.538529f}; /* DATA: 0xa3bdfec6 (x, y, z) */
    VehicleWheelParams wheel[4];                /* FL, FR, RL, RR                          */
    float droop = 0.10f;                        /* DATA: suspension graph clamp low         */
    float bump = 0.175f;                        /* DATA: suspension graph clamp high        */
    float spring_k = 0.0f;                      /* DERIVED: mass*g / (4*droop) at open()    */
    float damping_ratio = 0.35f;                /* PLACEHOLDER                              */
    float tyre_mu = 1.0f;                       /* PLACEHOLDER                              */
    float tyre_stiffness = 10.0f;               /* PLACEHOLDER: force per unit slip, x mu*Fz*/
    float engine_peak_torque = 450.0f;          /* PLACEHOLDER (N m)                        */
    float brake_torque = 3000.0f;               /* PLACEHOLDER (N m per wheel)              */
    float wheel_inertia = 1.5f;                 /* PLACEHOLDER (kg m^2)                     */
    float max_steer_rad = 0.785f;               /* unused: the graph publishes the angle     */
    float gravity = 9.81f;
};

/* Per-frame inputs, as the graphs publish them. */
struct VehicleDriveInputs {
    float throttle = 0.0f;      /* VehicleThrottle 0..1          */
    float brake = 0.0f;         /* VehicleBrake 0..1             */
    float gear_ratio = 0.0f;    /* GearRatio (ratio x final)     */
    float clutch = 0.0f;        /* Clutch: 0 engaged, 1 open     */
    float steer = 0.0f;         /* SteeringAngle -1..1           */
    float compression[4] = {0, 0, 0, 0}; /* SpringCompression_A1, A2, B1, B2 */
    bool compression_known[4] = {false, false, false, false};
    bool handbrake = false;
};

class VehicleDynamics {
public:
    VehicleDynamicsParams p;
    /* state, world frame, Y up; body axes x right, y up, z forward */
    float pos[3] = {0, 0, 0};
    float quat[4] = {0, 0, 0, 1};   /* x, y, z, w */
    float vel[3] = {0, 0, 0};
    float angvel[3] = {0, 0, 0};    /* world frame, rad/s */
    float omega[4] = {0, 0, 0, 0};  /* wheel spin, rad/s */
    float last_accel[3] = {0, 0, 0};
    float fz[4] = {0, 0, 0, 0};     /* last normal loads (N), for reporting */

    void init();                    /* derive spring_k etc. */
    void step(float dt, const VehicleDriveInputs& in);
    /* GRAPH PHYSICS: integrate accelerations the vehicle's own graph computed (its
     * wheel functions sum them into LinearAcceleration / AngularAcceleration), body
     * frame, plus gravity when `add_gravity`. No tyre or suspension model here. */
    void integrate_body_accel(float dt, const float lin_body[3], const float ang_body[3], bool add_gravity);
    void substep(float dt, const VehicleDriveInputs& in);
    /* 16 floats, rows of stride 4 (right, up, forward, translation), lane 3 of the
     * first three rows 0 - the shape VehicleSim::set_root takes. */
    void root_rows(float out[16]) const;
    float forward_speed() const;
    std::string describe_params() const;
};

} // namespace bf6

#endif
