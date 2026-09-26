#ifndef BF6_VEHICLE_SIM_H
#define BF6_VEHICLE_SIM_H

/* A VEHICLE'S OWN EXPRESSION GRAPHS, RUN FRAME BY FRAME.
 *
 * One object per vehicle. It loads the vehicle's graphs (e.g. its drivetrain
 * simex_car_* and its presex_*_suspensionmovement) from the install, prepares each
 * one exactly as the engine would at load (public-channel and bone bindings patched
 * into the constant pool, typed-copy sizes from the executable's reflection, operator
 * names from the executable), and keeps ONE shared channel store across graphs and
 * frames - so the drivetrain's GearRatio is what the next graph reads, and last
 * frame's RPM is what this frame's drivetrain reads, the way public channels work.
 *
 * Each graph keeps a persistent Instance (the engine keeps instance data between
 * frames); its slot file is scratch per evaluation. Graphs run in the order given.
 *
 * What this does NOT do: move the vehicle. The graphs publish throttle, brake,
 * gear ratio, steering and suspension compression; the force solver that turns
 * those into motion is native engine code and is not modelled here. */

#include "bf6_core.h"
#include "expression_graph.h"
#include "expression_vm.h"
#include "expression_pure_ops.h"
#include "expression_state_host.h"
#include "vehicle_wheel_ops.h"
#include "expression_registry.h"

#include <map>
#include <array>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace bf6 {

class VehicleSim {
public:
    struct PresentedBone {
        uint32_t channel_hash = 0;
        int32_t bone_index = -1;
        std::string name;
        std::array<float, 16> local{};
    };
    /* graphs: EBX/RES names; skeleton: the vehicle's ske_veh_*_base (may be empty).
     * exe: the install's executable (operator names). A tracer + user for rays. */
    bool open(bf6_ctx* ctx, const std::string& exe, const std::vector<std::string>& graphs,
              const std::string& skeleton, expression::bf6_ray_trace_fn tracer, void* tracer_user,
              std::string& err);
    /* A SUB-SKELETON (tracks, a launcher, a mount): its bones are appended after the
     * rig's, its channels mapped where the base rig does not already take them. Its root
     * stays a root; what the graphs write there are local poses. Returns bones added. */
    int add_skeleton(bf6_ctx* ctx, const std::string& skeleton);
    /* Poses from outside the graphs (animation clip layers), after tick(): write, then
     * publish_bones() again so they are presented and tracked. */
    bool write_bone_local(uint32_t channel_hash, const float local[16]);
    bool bone_rest_local(uint32_t channel_hash, float out[16]) const;
    bool maps_bone(uint32_t channel_hash) const;
    /* by rig index: rest local, current model, parent, and a local write (recorded
     * under the bone's channel when it has one, else under a synthetic key) */
    bool rig_rest_local(int32_t index, float out[16]) const { return state_.rest_local(index, out); }
    bool rig_local(int32_t index, float out[16]) const { return state_.current_local(index, out); }
    bool rig_model(int32_t index, float out[16]) const { return state_.current_model(index, out); }
    int32_t rig_parent(int32_t index) const { return state_.bone_parent(index); }
    void write_rig_local(int32_t index, const float local[16]);
    /* the bone channel a graph binds under this name (kind-1 binding), for clip DOFs */
    bool bone_channel_named(const std::string& name, uint32_t& hash) const {
        for (const auto& kv : bone_binds_) if (kv.second == name) { hash = kv.first; return true; }
        return false;
    }
    void publish_bones();
    void set_seats(const std::vector<uint32_t>& s) { state_.set_seats(s); }
    /* the drivetrain's reverse mode (see RecoveredOps::reverse_mode) */
    int reverse_mode() const { return recovered_.reverse_mode(); }
    /* graphs whose root-level local state is their own (the feature graphs from outside
     * the vehicle's folder); the rest share owner 0 */
    void isolate_graphs(const std::vector<std::string>& names);
    /* Warm the executable's name tables (named builtins, literal index) so the first
     * vehicle open does not pay ~1.8 s for them. Thread-safe; touches no context. */
    static void prewarm(const std::string& exe);
    static const std::vector<expression::NamedBuiltin>& named_builtins_cached(const std::string& exe);
    void set_weapons(const std::map<uint32_t, bf6::expression::StateHost::WeaponView>& w) { state_.set_weapons(w); }

    /* Inputs by public-channel NAME (any channel any loaded graph binds). */
    bool set_float(const std::string& channel, float v, uint32_t mode = 0);
    bool set_bool(const std::string& channel, bool v);
    bool set_int(const std::string& channel, int32_t v);
    bool channel_hash(const std::string& name, uint32_t& h) const { return hash_of(name, h); }
    /* The vehicle's placement (RootTransform), 16 floats = 4 rows of stride 16. */
    void set_root(const float rows[16]);
    /* A Vec3 channel (16 bytes, lane 3 zero) in a given mode. */
    bool set_vec3(const std::string& channel, const float v[3], uint32_t mode = 0);

    /* Outputs by name; known=false when no graph has set that channel (or its value
     * was unknown). */
    float get_float(const std::string& channel, bool* known = nullptr, uint32_t mode = 0) const;
    bool get_vec3(const std::string& channel, float out[3], uint32_t mode = 0) const;

    /* Run every graph once. Returns per-graph termination/unresolved in report(). */
    void tick();
    /* The game clock the graphs read (0xE2EEC2BE), seconds. */
    void set_time(double t) { world_.set_time(t); }
    const std::string& report() const { return report_; }
    /* THE MOTION SCOREBOARD for this vehicle, as JSON: the graphs, every public channel
     * the graphs READ split by who supplies it (the host's set_*, a graph's own write,
     * or nobody: the engine-native inputs still missing), part poses nobody supplied,
     * and every bone channel the graphs bind with how far its pose moved since open. */
    std::string motion_json() const;
    std::vector<std::string> channel_names() const;
    const expression::StateHost& state() const { return state_; }
    expression::StateHost& state_mut() { return state_; }
    const std::vector<PresentedBone>& presented_bones() const { return presented_bones_; }
    /* The car wheel physics functions (tyre, ray contact, ...) and the chassis
     * snapshot they read; the caller sets the body each tick. */
    expression::WheelOps& wheel_ops() { return wheel_; }
    /* Why the wheel functions are not served (empty when they are). */
    const std::string& wheel_ops_error() const { return wheel_err_; }
    /* channel hash -> name, over every binding of every loaded graph */
    std::map<uint32_t, std::string> channel_name_map() const;
    /* The graph's instance layout: header sizes and each constructor-owned
     * (type id, offsets) group, one line per group. */
    std::string instance_layout() const;
    /* The tree of records feeding one record, back through slot writes of the last
     * tick, down to max_depth; channel reads are labelled with the channel. */
    std::string sources(uint32_t record_offset, int max_depth = 12) const;
    /* Every record with lo <= offset < hi in the named graph (substring), one line
     * each: offset, kind, key/name, next, control target, operands. */
    std::string list(const std::string& graph_substr, uint32_t lo, uint32_t hi) const;
    /* WHY A CHANNEL IS UNKNOWN after the last tick: walk back from every record
     * that sets it, through the slots it reads, to the first culprits - an operator
     * that produced an unknown from known inputs (or was refused), or a slot
     * nothing wrote this tick. One line per culprit with its count. */
    std::string why(const std::string& channel) const;
    /* Every record that writes the channel, with the records that fed it (sources). */
    std::string writers(const std::string& channel, int depth = 10) const;
    /* Every record naming the channel's pool entry, with the next `follow` bytes of records. */
    std::string readers(const std::string& channel, int follow = 0x60) const;
    /* Every record, in every graph, naming a region-`region` operand in [lo, hi),
     * and whether this tick executed it (a trace row). */
    std::string touch(uint32_t region, uint32_t lo, uint32_t hi) const;
    /* One record's operands, and for each slot operand the last write before the
     * record first ran this tick (known or not). */
    std::string record_info(uint32_t offset) const;

    /* THE GRAPH AND ITS POOL, for the caller that fills in what the engine binds at
     * load. A graph's curves live in its EBX, not in its blob, and the link is the
     * graph's own typed slot groups - so the caller needs to see both the groups and
     * the records, and to add the pool patches it works out. */
    const expression::Graph* graph_named(const std::string& name) const {
        for (const auto& g : graphs_) if (g->name == name) return &g->graph;
        return nullptr;
    }
    /* The pool word at `off` as the loader patched it (false when not patched). */
    bool pool_patch(const std::string& name, uint32_t off, uint32_t& v) const {
        for (const auto& g : graphs_)
            if (g->name == name) {
                const auto it = g->inst.pool_patches.find(off);
                if (it == g->inst.pool_patches.end()) return false;
                v = it->second;
                return true;
            }
        return false;
    }
    void add_pool_patches(const std::string& name, const std::map<uint32_t, uint32_t>& p) {
        for (auto& g : graphs_)
            if (g->name == name)
                for (const auto& kv : p) g->inst.pool_patches[kv.first] = kv.second;
    }

private:
    struct G {
        std::string name;
        std::vector<uint8_t> raw;
        expression::Graph graph;
        expression::Instance inst;
        std::vector<bf6_channel_binding> binds;
        expression::NamedBuiltins builtins;
        expression::PureOps pure;
        std::map<uint32_t, std::string> names;
        uint32_t owner = 0;   /* root-level state owner, see isolate_graphs */
    };
    bool hash_of(const std::string& name, uint32_t& h) const;
    std::vector<std::unique_ptr<G>> graphs_;
    std::map<std::string, uint32_t> channel_hash_;
    /* feature path -> the state frame id every graph of this vehicle uses for it */
    std::map<std::string, uint32_t> frame_ids_;
    std::set<uint32_t> frame_ids_taken_;
    std::set<std::string> frame_ids_presentation_;   /* paths first met in a presex graph */
    expression::StateHost state_;
    expression::PhysicsQueryHost physics_;
    expression::RecoveredOps recovered_;
    expression::WorldHost world_;
    expression::WheelOps wheel_;
    struct BoneIdentity { int32_t index = -1; std::string name; };
    std::map<uint32_t, BoneIdentity> bone_identity_;
    std::vector<PresentedBone> presented_bones_;
    /* motion scoreboard (motion_json) */
    std::set<std::string> host_set_;
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> host_values_;   /* (hash, mode) */
    std::set<uint32_t> primary_written_;   /* channels a non-derived graph writes */
    bool primary_written_ready_ = false;
    std::map<uint32_t, std::string> bone_binds_;
    std::vector<std::string> rig_names_;
    std::vector<int32_t> rig_parents_;
    struct MotionTrack { std::array<float, 16> first{}; float rot_deg = 0, move_m = 0, scale = 0; uint32_t writes = 0; };
    std::map<uint32_t, MotionTrack> motion_;
    std::string wheel_err_;
    std::string report_;
};

} // namespace bf6

#endif
