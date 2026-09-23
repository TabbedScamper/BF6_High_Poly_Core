#pragma once
/* THE CAR WHEEL PHYSICS, AS THE GAME RUNS IT.
 *
 * The simex_car_<veh> drivetrain graph calls reflected MotionMachine functions once
 * per wheel: ray contact, tyre force, wheel-spin decay, suspension, contact
 * friction. They are the car's physics: each builds a snapshot of the chassis body,
 * applies its forces to the snapshot through a native force accumulator, and
 * returns the velocity change over dt as accelerations, which the graph sums into
 * the chassis acceleration channels. (Research: car-wheel-physics-runs-inside-the-
 * drivetrain-graph; transcriptions in impl/vehicle_and_1p/studies.)
 *
 * Everything here is in VEHICLE-LOCAL space (x left, y up, z forward, origin at the
 * rig root): the ray contact writes its hit through the inverse body matrix and
 * the tyre frame is built about local +Y.
 *
 * The host supplies the body snapshot each tick (WheelBody); this class answers the
 * functions. Each function names its native and how exact it is. */
#include "expression_vm.h"

#include <cstdint>
#include <map>
#include <string>

namespace bf6 {
namespace expression {

struct WheelBody {
    float v[4] = {0, 0, 0, 0};        // chassis linear velocity, local, m/s
    float w[4] = {0, 0, 0, 0};        // chassis angular velocity, local, rad/s
    float com[4] = {0, 0, 0, 0};      // centre of mass, local, m
    float mass = 0.0f;                // kg
    float inv_inertia[4] = {0, 0, 0, 0}; // diagonal, local, 1/(kg m^2)
    float gravity[4] = {0, -9.81f, 0, 0};
    float pos[3] = {0, 0, 0};         // rig root in the world (for the wheel rays)
    float quat[4] = {0, 0, 0, 1};     // rig orientation, x y z w
    /* THE WATER SURFACE the buoyancy and the boat hull ask the world for. Flat,
     * because a map's water is a level per body in the game; `water` false is a
     * land map, which is the functions' own above-water path. */
    float water_height = 0.0f;
    bool water = false;
};

/* World ray: from, to -> hit, normal; nonzero when it hits (the core's tracer). */
typedef int (*wheel_ray_fn)(void* user, const double* from, const double* to,
                            double* hit, double* normal);

/* Read the tyre load table from the game executable; false with a reason if the
 * executable no longer has it where expected. Until it succeeds the tyre force
 * is refused. */
bool wheel_ops_load_table(const std::string& exe, std::string& err);

class WheelOps final : public Host {
public:
    void set_body(const WheelBody& b) { body_ = b; }
    void set_tracer(wheel_ray_fn fn, void* user) { ray_ = fn; ray_user_ = user; }
    int rays() const { return rays_; }
    int ray_hits() const { return ray_hits_; }
    /* The graph moved the centre of mass (0x71C3128F) since the last clear. */
    bool com_set() const { return com_set_; }
    /* Per tyre-force call this tick, in call order: the wheel speed it produced and
     * whether that wheel had contact. The graph calls it twice per wheel section;
     * what native code writes back into the per-wheel state is the section's last. */
    const std::vector<float>& tyre_omegas() const { return tyre_omega_; }
    const std::vector<uint8_t>& tyre_contacts() const { return tyre_contact_; }
    void clear_tick() {
        tyre_omega_.clear();
        tyre_contact_.clear();
        for (int i = 0; i < 3; ++i) { hull_dv_[i] = 0.0f; hull_dw_[i] = 0.0f; }
        hull_ran_ = false;
    }
    /* THE HULL APPLIES ITS OWN FORCES. A wheel function hands its acceleration back
     * for the graph to sum into the chassis channel; the boat hull integrates into
     * the body block itself and returns only two point velocities, so its velocity
     * change reaches the vehicle through here instead. Vehicle-local, per tick. */
    bool hull_ran() const { return hull_ran_; }
    /* Half the hull's length along its forward axis, out of the description the
     * graph hands its own hull operator; zero until that operator has run once. */
    float hull_half_length() const { return hull_half_length_; }
    const float* hull_dv() const { return hull_dv_; }
    const float* hull_dw() const { return hull_dw_; }
    void clear_com_set() { com_set_ = false; }
    /* The graph set the inertia per kg (0x00945C6D) since the last clear. */
    bool inertia_set() const { return inertia_set_; }
    const float* inertia_per_kg() const { return inertia_per_kg_; }
    void clear_inertia_set() { inertia_set_ = false; }
    const WheelBody& body() const { return body_; }
    /* THE CURVES THE ENGINE BINDS AT LOAD. A keyed curve's keys are not in the
     * graph's pool: the descriptor there holds a pointer the loader fills, so the
     * caller resolves the keys from the resource's EBX and hands them over, and the
     * descriptor gets an index into this list instead of an address. */
    size_t add_curve(std::vector<float> keys) {
        curves_.push_back(std::move(keys));
        return curves_.size() - 1;
    }
    static uint32_t curve_handle(size_t index) { return 0xC0DE0000u | (uint32_t)index; }

    /* A STRUCT BUILDER'S FIELD TABLE. 0x8B226FBB writes its i-th argument at the i-th
     * entry of an offsets array the loader hands it, and for a struct that array is
     * the type's own field table - which the hosts cannot read, because they have no
     * type database. So the caller measures it and hands it over, keyed by the field
     * COUNT, which the record states as a constant. */
    struct BuilderLayout {
        uint32_t size = 0;                  /* the struct's own size */
        std::vector<uint32_t> offsets;      /* per argument, in declaration order */
        std::vector<uint32_t> widths;       /* per argument */
    };
    void set_builder(const BuilderLayout& b) { builders_[b.offsets.size()] = b; }
    const BuilderLayout* builder_for(size_t count) const {
        const auto it = builders_.find(count);
        return it == builders_.end() ? nullptr : &it->second;
    }

    /* Lent by the evaluator: the track sampler returns an array of contacts. */
    void set_heap_sink(HeapSink* sink) override { heap_ = sink; }
    bool describe(uint32_t key, OperatorSignature& out) override;
    /* Per call: a graph may leave a trailing input off (thebeast's suspension record
     * has 9 inputs, not 10 - it omits IsFrontWheel, which the native never reads). */
    bool describe_call(uint32_t key, const std::vector<uint32_t>& consts,
                       OperatorSignature& out) override;
    bool invoke(uint32_t key, const std::vector<Value>& args, Value& out) override;
    /* The accumulator force scale the native reads from its component (+0xC8).
     * Not readable offline; 1.0 is a STAND-IN. */
    float force_scale = 1.0f;
    const std::map<uint32_t, uint32_t>& served() const { return served_; }
private:
    /* One wheel ray into a 0x40-byte WheelContact; the tank's sampler runs it once
     * per road wheel. */
    void cast_wheel_ray(const float at[3], float radius, float spring, float attach,
                        float extra, std::vector<uint8_t>& out);
    WheelBody body_;
    wheel_ray_fn ray_ = nullptr;
    void* ray_user_ = nullptr;
    HeapSink* heap_ = nullptr;
    int rays_ = 0, ray_hits_ = 0;
    bool com_set_ = false;
    bool inertia_set_ = false;
    float inertia_per_kg_[3] = {0, 0, 0};
    std::vector<float> tyre_omega_;
    std::vector<uint8_t> tyre_contact_;
    float hull_dv_[3] = {0, 0, 0}, hull_dw_[3] = {0, 0, 0};
    bool hull_ran_ = false;
    float hull_half_length_ = 0.0f;
    std::map<uint32_t, uint32_t> served_;
    std::vector<std::vector<float>> curves_;
    std::map<size_t, BuilderLayout> builders_;
};

} // namespace expression
} // namespace bf6
