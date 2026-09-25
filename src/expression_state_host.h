#ifndef BF6_EXPRESSION_STATE_HOST_H
#define BF6_EXPRESSION_STATE_HOST_H

/* THE STATE HALF OF A DiceExpression HOST.
 *
 * NamedBuiltins serves the pure operators and refuses everything state-backed,
 * which is why a vehicle graph runs to completion and returns a TAINTED value:
 * taint is total, and a graph computes only with zero unresolved operators. This
 * supplies the missing half - the scoped replicated state block, the field-address
 * identity, and the evaluation context - so the state family stops tainting.
 *
 * WHAT THESE OPERATORS ARE. Every key below is named in data/EngineNodes.tsv, read
 * from the engine's own descriptor table, with an implementation address and a note.
 * None of it is guessed from a hash. The ten state operators account for 913 of the
 * blocked uses across the 529 vehicle expression graphs, and the accessor family is
 * dominated by one wrapper whose note is the same sentence every time: "fetch the
 * current thread's evaluation context".
 *
 * WHY THE FIELD ADDRESS IS THE IDENTITY. 0x8B7CF7C9 is "field ADDRESS (base +
 * desc[0x24])", and the engine note says plainly that offline the path IS the
 * identity. So a cell is keyed by the path value the graph already carries, and no
 * base pointer has to be modelled.
 *
 * WRITES ARE OFF BY DEFAULT, AND THAT IS DELIBERATE. Three of the ten are writes,
 * and the engine table gives them arity 1 - which does not obviously account for
 * both a destination and a value. A wrong READ yields a tainted value and the VM
 * says so; a wrong WRITE silently corrupts the block and every later read of it. So
 * writes refuse unless allow_writes is set, which leaves the VM to report them
 * rather than letting this class invent a store layout. Turn them on only against a
 * graph whose output is known.
 */

#include "expression_vm.h"

#include <cstdint>
#include <cstring>
#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bf6 { namespace expression {

class StateHost final : public Host {
public:
    /* The evaluation context is one object offline, not a subsystem. Any value
     * does, so long as it is stable and marked known; the graphs only pass it
     * along to other accessors. */
    static const uint32_t kContextHandle = 0x00C0FFEEu;

    /* Seed a cell. `path` is the HashName a StateListDescriptor entry carries. */
    void set_u32(uint32_t path, uint32_t value);
    /* Seed a FRAME-RELATIVE cell as the native code would: `frame` is the pushed
     * frame's identity (bound[3], 0 for no frame), `path` the 0xFFFFiiff address. */
    void set_frame_cell(uint32_t frame, uint32_t path, uint32_t kind, uint32_t field,
                        uint32_t value) {
        cells_[make_key(frame, path, kind, field)] = value;
    }
    void set_bool(uint32_t path, bool value);
    void set_float(uint32_t path, float value);

    /* THE NATIVE PER-WHEEL STATE. A drivetrain reads each wheel's spin and ground
     * contact through frame-relative descriptors {0xFFFFii<cc>, 0xFFFFFFFF, 0, f}:
     * element ii = wheel index, field 3 = angular velocity (float bits), field 4 =
     * has-contact (0/1). Native code writes them in the game; a caller supplies
     * them here and they are served in whatever frame the graph has pushed.
     *
     * THE LOW BYTE IS THE PART CLASS AND IT IS NOT THE SAME FOR EVERY VEHICLE: a
     * car's wheels are 0x05 and a tank's road wheels are 0x00. It was hard-coded to
     * 0x05, so a tank's wheel speed was written where nothing read it, every tick
     * began with the tracks stationary, no slip ever built and the tank made no
     * force at all. The class is now LEARNED from the graph's own reads rather than
     * assumed, and the write-back goes back to the spelling that was read. */
    void set_wheel_state(int wheel, float angular_velocity, bool contact) {
        if (wheel < 0 || wheel >= 8) return;
        std::memcpy(&wheel_spin_[wheel], &angular_velocity, 4);
        wheel_contact_[wheel] = contact ? 1u : 0u;
        wheel_known_[wheel] = true;
    }

    /* THE SAME CELLS, BY THEIR AUTHORED NAME.
     *
     * A StateListDescriptor carries both a `DebugName` and a `HashName`, and the
     * hash is derivable: djb2-x33-XOR from seed 5381 over the raw, CASE-SENSITIVE
     * bytes. Verified on all 128 DebugName/HashName pairs across simex_car_flyer60,
     * presex_flyer60_suspensionmovement_1, _doormovement and
     * presex_vehicle_controller_flyer60 - 128 of 128, no mismatches.
     *
     * So a caller can seed "SmoothedInputYaw" or "Wheel Front Left" without reading
     * the EBX, which is what makes feeding a vehicle its inputs mechanical rather
     * than another lookup problem. NOTE it is NOT the same as the add-on's
     * BF6Mvdb.djb2, which lowercases; state names do not. */
    static uint32_t path_of(const char* name);
    void set_float_named(const char* name, float value);
    void set_u32_named(const char* name, uint32_t value);
    void set_bool_named(const char* name, bool value);

    /* Every cell the graph READ that had not been seeded, so a caller can see what
     * a vehicle actually asks for instead of guessing what to supply. */
    const std::vector<uint32_t>& unseeded_reads() const { return unseeded_; }
    /* Every state operator this host served, by key, for measurement. */
    const std::map<uint32_t, uint32_t>& served() const { return served_; }
    int frame_depth() const { return (int)frames_.size(); }
    int writes() const { return writes_; }

    /* DIAGNOSTIC, for working out frame-relative resolution.
     *
     * Reads land on addresses like 0xFFFFFF00..03 and 0xFFFFFFFF, which are not
     * hashes - they are frame-relative, and the arithmetic from (frame, offset) to a
     * real path is not yet established. So record what PUSH was actually handed and
     * what the frame stack held at each such read, because the relationship has to
     * be read off the data rather than guessed. */
    struct PushSeen { uint32_t arg[4]; uint32_t known_mask; };
    struct ReadSeen { uint32_t addr; int depth; uint32_t bound[4]; };
    const std::vector<PushSeen>& pushes_seen() const { return pushes_seen_; }
    const std::vector<ReadSeen>& odd_reads() const { return odd_reads_; }
    void set_trace_frames(bool on) { trace_frames_ = on; }

    /* What a setting/rule lookup (0x59F02977) answers. One value for all of them:
     * this host does not model the settings registry, it only stops it tainting. */
    void set_setting_default(bool value) { setting_default_ = value; }
    void set_allow_writes(bool value) { allow_writes_ = value; }

    bool describe(uint32_t key, OperatorSignature& out) override;
    bool invoke(uint32_t key, const std::vector<Value>& args, Value& out) override;

    /* THE PART-TRANSFORM READER, 0x04BEFF62.
     *
     * Filed in EngineNodes.tsv as one more "fetch the current thread's evaluation
     * context" wrapper, and stubbed here for a long time as a 4-byte handle. What it
     * actually RETURNS, measured by what every call's output is consumed as across all
     * vehicle graphs: a 64-byte LinearTransform when operand 0 is 0 (MultiplyLT x1144,
     * InverseLT x21, InterpolateLT, RotationAndTranslation) and a 4-byte float when
     * operand 0 is 1 (SignFloat). Operand 1 looks like a part selector (0x9D0001,
     * 0x880001, 0x8801...). The 4-byte handle made every transform chain read 60
     * unknown bytes, which is why no wheel ray was ever queued.
     *
     * The VALUE is a stand-in: identity, and 0.0 for the float mode, until something
     * supplies real part poses. That is labelled here rather than hidden - a chain that
     * computes through an identity is proving the plumbing, not the physics. */
    bool describe_call(uint32_t key, const std::vector<uint32_t>& consts,
                       OperatorSignature& out) override;

    /* THE NAMED-TRANSFORM READER, 0xABAAAD01.
     *
     * Also filed as an "evaluation context" wrapper and stubbed as a 4-byte handle.
     * Its three operands are all constants: operand 0 is always 0, operand 1 is a
     * NAME HASH (which transform), operand 2 a mode 0/1/2. Measured by what reads
     * the output across all vehicle graphs: modes 0 and 2 feed LinearTransform
     * consumers (MultiplyLT, InverseLT, AddLT, 64-byte moves, the 0x85B133CD
     * transform argument); mode 1 outputs are never read, only overwritten, and
     * always sit in >= 64 bytes of slot room. So every mode is a 64-byte transform.
     *
     * The runtime trace named this as the ONE thing making every suspension raycast's
     * Start and End unknown: the rays are built from a mode-2 read moved as 64 bytes,
     * and the 4-byte stub left 60 of them unwritten.
     *
     * The VALUE is the caller's: set_named_transform(name hash, 16 floats as four
     * Vec3 rows of stride 16). A name nobody supplied reads IDENTITY and is recorded
     * in unsupplied_transforms(), the same policy as unseeded state cells: the
     * record turns a silent stand-in into a list of inputs to provide. */
    void set_named_transform(uint32_t name_hash, const float rows[16]);

    /* PUBLIC CHANNELS, keyed by (mode << 32 | channel hash); mode is 0 for the
     * un-moded operators. The hash is the channel record's own (bindings report it
     * with its name). A read of a channel nobody set returns zero and is recorded;
     * every set is stored, so a later read sees it and the caller can read what a
     * graph OUTPUT (e.g. SpringCompression_A1). */
    void set_channel(uint32_t hash, const std::vector<uint8_t>& bytes, uint32_t mode = 0) {
        channels_[((uint64_t)mode << 32) | hash] = bytes;
        unknown_channels_.erase(((uint64_t)mode << 32) | hash);
    }
    const std::map<uint64_t, std::vector<uint8_t>>& channels() const { return channels_; }
    const std::map<uint64_t, uint32_t>& channel_writes() const { return channel_writes_; }
    const std::map<uint64_t, uint32_t>& unsupplied_channels() const { return unsupplied_channels_; }

    /* PART POSES for 0x04BEFF62, keyed by (mode, part selector constant such as
     * 0x9D0001, the call's ExpressionBoneId, 16 bytes).
     * Unsupplied keys read identity and are recorded, so a caller can see exactly
     * which part poses a vehicle asks for. */
    struct PartKey {
        uint32_t mode, selector;
        uint32_t bone[4];   /* the ExpressionBoneId value, all 0xFF.. when unknown */
        bool operator<(const PartKey& o) const {
            if (mode != o.mode) return mode < o.mode;
            if (selector != o.selector) return selector < o.selector;
            return std::memcmp(bone, o.bone, sizeof bone) < 0;
        }
    };
    void set_part_transform(const PartKey& key, const float rows[16]) {
        std::vector<uint8_t> b(64);
        for (int i = 0; i < 16; ++i) std::memcpy(b.data() + i * 4, &rows[i], 4);
        part_transforms_[key] = b;
    }
    const std::map<PartKey, uint32_t>& unsupplied_parts() const { return unsupplied_parts_; }
    /* BONE POSES by (mode, bone channel hash). A BoneId's first dword is the bone
     * channel hash the engine binds at load (bf6_expression_channel_bindings kind 1);
     * the skeleton maps that hash to a rig bone (bf6_skeleton_channel_bones). Used
     * when no exact PartKey was set. */
    void set_bone_pose(uint32_t bone_hash, uint32_t mode, const float rows[16]) {
        std::vector<uint8_t> b(64);
        std::memcpy(b.data(), rows, 64);
        bone_poses_[((uint64_t)mode << 32) | bone_hash] = b;
    }
    /* Skeleton state used by SetPartTransform (0x4899CB44). The game's setter
     * always commits an absolute LOCAL pose; model/world modes are converted
     * through the current parent/root transforms first. */
    void set_skeleton_bone(int32_t index, int32_t parent,
                           const float local[16], const float model[16]);
    void map_skeleton_bone(uint32_t channel_hash, int32_t index);
    /* Write a bone's local pose and recompose its subtree (the setter's commit). */
    void commit_local(int32_t index, const float local[16]);
    void record_bone_write(uint32_t channel_hash, const float local[16]) {
        std::vector<uint8_t>& w = bone_writes_[channel_hash];
        w.resize(64);
        std::memcpy(w.data(), local, 64);
    }
    bool current_local(int32_t index, float out[16]) const {
        if (index < 0 || (size_t)index >= skeleton_poses_.size()) return false;
        std::memcpy(out, skeleton_poses_[(size_t)index].local.data(), 64);
        return true;
    }
    bool current_model(int32_t index, float out[16]) const {
        if (index < 0 || (size_t)index >= skeleton_poses_.size()) return false;
        std::memcpy(out, skeleton_poses_[(size_t)index].model.data(), 64);
        return true;
    }
    int32_t bone_parent(int32_t index) const {
        if (index < 0 || (size_t)index >= skeleton_poses_.size()) return -1;
        return skeleton_poses_[(size_t)index].parent;
    }
    bool rest_local(int32_t index, float out[16]) const {
        if (index < 0 || (size_t)index >= skeleton_poses_.size()) return false;
        std::memcpy(out, skeleton_poses_[(size_t)index].rest_local.data(), 64);
        return true;
    }
    void begin_bone_tick();
    const std::map<uint32_t, std::vector<uint8_t>>& bone_writes() const {
        return bone_writes_;
    }
    const std::map<uint32_t, uint32_t>& unsupplied_transforms() const {
        return unsupplied_transforms_;
    }

private:
    /* FOUR, not three. The note says "up to three paths" but PUSH has arity 4 and
     * which operand is which was never established, so all four are kept. */
    struct Frame { uint32_t bound[4]; };

    bool read_cell(uint32_t path, uint32_t& out);

    /* Cells by key: a plain path, or - for a FRAME-RELATIVE address (0xFFFFxxxx,
     * e.g. 0xFFFF0105 = field 5 of element 1 of the pushed feature's state) - the
     * current frame's identity in the high dword. The identity is PUSH operand 3, a
     * relocated pointer to the pushed feature's path (the caller patches relocation
     * targets into the pool), so each feature keeps its own state and that state
     * persists across ticks, as a per-feature state block does. */
    std::map<uint64_t, uint32_t> cells_;
    /* A STATE OPERAND IS A 16-BYTE DESCRIPTOR {path, path2, kind, field}, not a
     * 4-byte path. Measured on simex_car_flyer60: the Automatic Gearbox's five reads
     * and six writes all carry path 0xFFFFFFFF ("the pushed frame's struct") and
     * differ only in the last dword (fields 0, 1, 2, 3, 5); keyed by the path alone
     * they all landed in ONE cell, so the gearbox forgot its phase and gear every
     * frame. The cell is (frame, path, kind, field). */
    /* THE FIELDS OVERLAPPED, so distinct cells shared one key. frame sat at bits 40..63
     * and kind at bits 40..47, XORed, so a kind-1 read on frame N produced exactly the key
     * of a kind-0 read on frame N+1: measured, an f22's `frame 0005AE kind 1` keyed as
     * 0005AF. Frames are sequential small ids and kinds 0 and 1 both occur, so this was
     * not hypothetical - and the only two aircraft that read NO kind-1 cells (f16, jas39)
     * are exactly the two that climb properly, while every plane that reads them barely
     * leaves the ground.
     *
     * Partitioned with no overlap: path 0..31, field 32..39, kind 40..47, frame 48..63.
     * Measured over the fleet, 5436 cells: max frame 0x27EA, max kind 2, max field 22, so
     * every field has room. A frame past 16 bits would truncate and could bring the
     * collision back, so it reports itself rather than failing silently as the original
     * did. */
    static uint64_t make_key(uint32_t frame, uint32_t path, uint32_t kind, uint32_t field) {
        if (frame > 0xFFFFu) {
            static bool told = false;
            if (!told) {
                told = true;
                std::fprintf(stderr, "state cell frame 0x%X exceeds 16 bits: keys may collide\n",
                             frame);
            }
        }
        return ((uint64_t)(frame & 0xFFFFu) << 48) | ((uint64_t)(kind & 0xFFu) << 40) |
               ((uint64_t)(field & 0xFFu) << 32) | (uint64_t)path;
    }
    uint64_t cell_key(uint32_t path) const {
        const uint32_t frame = (path >= 0xFFFF0000u && !frames_.empty()) ? frames_.back().bound[3] : 0u;
        return make_key(frame, path, cur_kind_, cur_field_);
    }
    uint32_t cur_kind_ = 0, cur_field_ = 0, cur_record_ = 0;
public:
    void set_current_record(uint32_t r) override { cur_record_ = r; }
private:
public:
    /* A frame-relative read this tick that nothing had seeded: the cell the engine
     * would have written. Kept in read order so a caller can answer the same cells
     * next tick (see read_cell). */
    struct FrameRead { uint64_t key; uint32_t frame, path, kind, field; };
    const std::vector<FrameRead>& unseeded_frame_reads() const { return unseeded_frame_reads_; }
    void clear_unseeded_frame_reads() { unseeded_frame_reads_.clear(); }
    /* Every graph starts at the entity's root: an empty frame stack. Pushes a graph did
     * not pop must not leak into the next graph, where they re-key its root-level state
     * (a derived graph's reads landed on the simex's last pushed frame). */
    void reset_frames() { frames_.clear(); }

    /* THE CHARACTERS NEAR THE VEHICLE. The engine's motion database holds every soldier;
     * graphs query it by radius (0x16E0F8DA) and sort the hits by a channel's value
     * (0xE88A04DB): a boat stays awake while an ALIVE character is within reach, a door
     * opens for the soldier entering. Offline the only character is the vehicle's own
     * driver, supplied by the caller each tick. */
    struct Character {
        uint32_t id = 0;              /* entity id; 0x000FFFFF is the unbound sentinel */
        float center[3] = {0, 0, 0};  /* world-space centre of its bounds */
        float half[3] = {0, 0, 0};    /* half extents of its bounds */
        bool active = true;           /* the flag the search requires (+0x28 bit 3) */
        int32_t health_state = 1;     /* MM.HealthState: 1 Alive, 2 ManDown, 5 Dead ... */
        int32_t open_door = 0;        /* Vehicle.OpenDoor.Enum: the door it entered by (2 LeftDoor1) */
    };
    void set_characters(const std::vector<Character>& c) { characters_ = c; }
    /* THE SEATS: seat index -> the id of the entity sitting there (0 = empty). A door
     * graph animates a door when its seat's occupant changes (0x91C21F3C / 0x0221B337). */
    void set_seats(const std::vector<uint32_t>& s) { seats_ = s; }
    /* the channel an entity-query config (a relocated pool pointer) tests */
    void map_condition_channel(uint32_t config, uint32_t channel) { condition_channel_[config] = channel; }
    void set_heap_sink(HeapSink* sink) override { heap_ = sink; }
    void set_cell_raw(uint64_t key, uint32_t value) { cells_[key] = value; }

private:
    std::vector<FrameRead> unseeded_frame_reads_;
    uint32_t wheel_spin_[8] = {}, wheel_contact_[8] = {};
    bool wheel_known_[8] = {};
    /* The part-class byte the graph actually reads for each wheel index, learned on
     * the first read; 0xFF until one has been seen. */
    uint8_t wheel_class_[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    void take_descriptor(const Value& v) {
        cur_kind_ = cur_field_ = 0;
        if (v.bytes.size() >= 16) {
            std::memcpy(&cur_kind_, v.bytes.data() + 8, 4);
            std::memcpy(&cur_field_, v.bytes.data() + 12, 4);
        }
    }
    std::vector<Frame> frames_;
    std::vector<Character> characters_;
    std::vector<uint32_t> seats_;
    /* 260-byte state values (an id collection kept between ticks) by cell key */
    std::map<uint64_t, std::vector<uint8_t>> wide_cells_;
    std::map<uint32_t, uint32_t> condition_channel_;
    HeapSink* heap_ = nullptr;
    std::vector<PushSeen> pushes_seen_;
    std::vector<ReadSeen> odd_reads_;
    bool trace_frames_ = false;
    int writes_ = 0;
    std::vector<uint32_t> unseeded_;
    std::map<uint32_t, uint32_t> served_;
    std::map<uint32_t, std::vector<uint8_t>> named_transforms_;
    std::map<uint32_t, uint32_t> unsupplied_transforms_;
    std::map<uint64_t, std::vector<uint8_t>> channels_;
    std::map<uint64_t, uint32_t> channel_writes_;
    std::map<uint64_t, uint32_t> unsupplied_channels_;
    /* Channels whose last write was UNKNOWN (reads refuse until a known write). */
    std::set<uint64_t> unknown_channels_;
    std::map<PartKey, std::vector<uint8_t>> part_transforms_;
    std::map<PartKey, uint32_t> unsupplied_parts_;
    std::map<uint64_t, std::vector<uint8_t>> bone_poses_;
    struct SkeletonPose {
        int32_t parent = -1;
        std::array<float, 16> rest_local{};
        std::array<float, 16> rest_model{};
        std::array<float, 16> local{};
        std::array<float, 16> model{};
    };
    std::vector<SkeletonPose> skeleton_poses_;
    std::map<uint32_t, int32_t> skeleton_bone_index_;
    /* Actual local transforms committed this tick, keyed by bone channel. */
    std::map<uint32_t, std::vector<uint8_t>> bone_writes_;
    bool setting_default_ = false;
    bool allow_writes_ = false;
};

/* THE WHEEL RAYCAST: the async physics query pair.
 *
 * `__queueAsyncPhysicsRayQueryNode` issues a ray and `__consumeAsyncPhysicsQueryNode`
 * collects it, which is the `DicePhysicsCastQueryHandle` issue-and-consume pattern
 * the suspension graph declares four of, one per wheel. Backed by the core's own ray
 * scene, so no Godot bridge is needed to answer a ray: `bf6_ray_scene_trace` already
 * returns point, normal and distance over real triangles, in metres with Y up.
 *
 * WHAT IS MEASURED AND WHAT IS NOT. Walking the graphs and recording which operator
 * wrote each input slot shows `queue`'s inputs 2 and 3 are fed by
 * `MultiplyFloat3LinearTransformFloat3` and `SubtractFloat3` - both Vec3, 16 bytes -
 * which are the ray's FROM and TO. Inputs 0 and 1, and all of `consume`'s, are not
 * written by any operator in the graph (they are constants, instance values or state
 * cells), so their widths are NOT measured. They are declared 4 bytes here, which is
 * a GUESS, and it is the first thing to revisit if these nodes keep refusing.
 *
 * SYNCHRONOUS ON PURPOSE. The game spreads the query over frames; a single evaluate()
 * cannot, so the trace runs at queue time and consume returns the stored answer. That
 * makes the pattern work in one pass at the cost of a frame of latency the game has
 * and this does not - which matters for a moving vehicle and is noted rather than
 * hidden.
 */
typedef int (*bf6_ray_trace_fn)(void* user, const double* from, const double* to,
                               double* hit, double* normal);

class PhysicsQueryHost final : public Host {
public:
    void set_tracer(bf6_ray_trace_fn fn, void* user) { trace_ = fn; user_ = user; }
    void add(uint32_t key, const std::string& current_exe_name);

    int queries() const { return queries_; }
    int hits() const { return hits_; }
    /* Every time a queue node was INVOKED, and how many of those refused because an
     * endpoint was unknown - so "0 rays" can be told apart from "never reached". */
    int attempts() const { return attempts_; }
    int unknown_ends() const { return unknown_ends_; }
    /* Every ray actually traced, in order: its ends and whether it hit. */
    struct RayLog { float from[3]; float to[3]; bool hit; bool sync; };
    const std::vector<RayLog>& ray_log() const { return ray_log_; }

    bool describe(uint32_t key, OperatorSignature& out) override;
    bool invoke(uint32_t key, const std::vector<Value>& args, Value& out) override;

private:
    std::vector<RayLog> ray_log_;
    struct Result { bool hit = false; double point[3] = {}; double normal[3] = {};
                    double from[3] = {}; double to[3] = {}; };
    std::map<uint64_t, Result> pending_;   /* (id, id2) -> answer for the next consume */

    std::map<uint32_t, std::string> names_;
    std::map<uint32_t, Result> results_;   /* handle -> answer */
    std::map<uint32_t, uint32_t> results_by_path_;  /* state path -> handle */
    uint32_t next_handle_ = 1;
    bf6_ray_trace_fn trace_ = nullptr;
    void* user_ = nullptr;
    int queries_ = 0;
    int hits_ = 0;
    int attempts_ = 0;
    int unknown_ends_ = 0;
};

/* NamedBuiltins for the pure operators, StateHost for the state family, in that
 * order of preference. The VM takes one Host, and a vehicle graph needs both. */
class CompositeHost final : public Host {
public:
    CompositeHost(Host* first, Host* second) : first_(first), second_(second) {}
    bool describe(uint32_t key, OperatorSignature& out) override;
    bool invoke(uint32_t key, const std::vector<Value>& args, Value& out) override;

private:
    Host* first_ = nullptr;
    Host* second_ = nullptr;
};

/* THE OFFLINE WORLD, v0: one entity - the vehicle running the graph - with no
 * components, no occupants and no other entities. Every operator here returns what
 * the ENGINE would return in exactly that world, read from its implementation
 * (native/EXPRESSION_OPERATOR_SPECS.tsv and the world study); nothing is made up for
 * a richer world. What such a world cannot answer (who is driving, live motion
 * state, the clutch machine's vehicle handle) is left refused.
 *
 *   entity set    {u32 count, 64 x u32 id, rest 0x000FFFFF}           260 bytes
 *   handle        8-byte refcounted pointer, 0 = null; offline a token
 */
class WorldHost final : public Host {
public:
    /* The registry id the vehicle entity answers to (any u32 but 0x000FFFFF). */
    void set_self_id(uint32_t id) { self_id_ = id; }
    /* A TWEAKABLE's value by its hash (a kind-2 binding: the authored default
     * unless a caller overrides it). __GetTweakableFloat/Bool serve from here; an
     * unregistered hash stays unresolved rather than guessing zero. */
    void set_tweakable(uint32_t hash, uint32_t bits) { tweakables_[hash] = bits; }
    /* The game clock in seconds, served by 0xE2EEC2BE. */
    void set_time(double t) { time_ = t; }
    bool describe(uint32_t key, OperatorSignature& out) override;
    bool describe_call(uint32_t key, const std::vector<uint32_t>& consts,
                       OperatorSignature& out) override;
    bool invoke(uint32_t key, const std::vector<Value>& args, Value& out) override;
    const std::map<uint32_t, uint32_t>& served() const { return served_; }
private:
    uint32_t self_id_ = 1;
    std::map<uint32_t, uint32_t> served_;
    std::map<uint32_t, uint32_t> tweakables_;
    double time_ = 0.0;
};

}} // namespace bf6::expression

#endif
