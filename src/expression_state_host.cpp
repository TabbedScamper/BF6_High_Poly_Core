#include "expression_state_host.h"
#include "soldier_fields.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace bf6 { namespace expression {

namespace {

/* The state family. Key, arity and behaviour all from data/EngineNodes.tsv. */
const uint32_t kPushFrame   = 0x30FAAAB6u; /* arity 4, side effect only        */
const uint32_t kPopFrame    = 0x254EE43Du; /* no operands, side effect only    */
const uint32_t kReadBit     = 0x1ED1F702u; /* arity 1 -> bool                  */
const uint32_t kReadU32A    = 0x328A61DDu; /* arity 1 -> u32                   */
const uint32_t kReadU32B    = 0xECA37AA7u; /* arity 1 -> u32, same routine     */
const uint32_t kFieldAddr   = 0x8B7CF7C9u; /* arity 1 -> the path, identity    */
const uint32_t kWriteFloatQ = 0x47BE3D90u; /* arity 1, quantised write         */
const uint32_t kWriteAny    = 0xA04FF621u; /* arity 1, type-generic write      */
const uint32_t kWriteBool   = 0x83B013E3u; /* arity 1, write bool              */
const uint32_t kWriteU32    = 0x6575DE53u; /* arity 1, write u32               */


/* THE EVALUATION-CONTEXT WRAPPERS. Every one of these carries the identical note,
 * "fetch the current thread's evaluation context (TLS[0x5c0]-8)", and offline that
 * is a single handle. They differ only in arity, which is why each is listed with
 * its own: describing an operator with the wrong arity is worse than refusing it,
 * because the VM then reads the wrong operands as a value. */
struct ContextOp { uint32_t key; uint32_t arity; };
const ContextOp kContextOps[] = {
    /* 0x18B2987B, 0x08F4A2D4, 0x45C08E62, 0xC491CD96, 0x6D86C436 moved to
     * kChannelOps: they are channel get/set, not context handles. */
    {0x04BEFF62u, 3}, {0xABAAAD01u, 3},
    /* 0xC58D8EA6 removed: its records carry ONE operand and it is the output, so it
     * takes zero inputs. Declared here with arity 1 (the EngineNodes value), every
     * call was rejected - 1,050 uses, unresolved in all eight suspension graphs.
     * Handled separately below as the frame delta. */
    /* 0xE2EEC2BE removed: it is the game CLOCK (WorldHost), zero inputs. */
    /* 0x9A39505A removed: the game TICK (WorldHost), zero inputs, three outputs. */
    {0x2B65ED67u, 3},
    /* 0xC8364385 removed: its records carry ONE operand and it is the output (zero
     * inputs), the same mistake as 0xC58D8EA6. It is the entity's own index, served
     * below. */
    /* 0xF83207B2 and 0xCA1E499E removed: they are shape-B node records (code takes
     * (ctx, outputs[], inputs[])), not context handles. 0xCA1E499E is the
     * damage-affector entity query FUN_141723e60; answering it as a handle
     * fabricated its outputs, which feed the drivetrain's gear request. */
};

/* THE CHANNEL OPERATORS. Operand 0 is a constant-pool handle slot the engine fills
 * at load (bf6_expression_channel_bindings names it), operand 1 the channel's own
 * hash (it equals the bound channel record's hash in every checked call), then an
 * optional space/mode constant, then - for a SET - the value. Classified across
 * all 676 vehicle graphs by which public channels each key touches and whether the
 * record has a slot output:
 *   get float 0x18B2987B  SteeringAngle, RPM, InputThrottle, DistanceToWater
 *   set float 0x6D86C436  SlipRatio, RPM, VehicleThrottle, SpringCompression_*
 *   get bool  0x59F02977  EntryActive_Driver, HasContact_Wheel_*, Airborne State
 *   set bool  0xC491CD96  HasContact_Wheel_*, Airborne State, IsBoosting
 *   get int   0x45C08E62  TeamId, Vehicle Damage State, MaterialID_*
 *   set int   0xCC162A96  MaterialID_*, GearNumber
 *   get Vec3  0x08F4A2D4  Linear/AngularVelocity, Linear/AngularAcceleration (moded)
 *   set Vec3  0x95954635  the same channels (moded)
 * A set used to be declared as a read with an output, so the VM wrote the 4-byte
 * stand-in handle INTO THE VALUE SLOT - clobbering the very value being set.
 * 0x59F02977 was answered as a "setting lookup" (always false). */
struct ChannelOp { uint32_t key; uint32_t width; bool write; bool moded; };
const ChannelOp kChannelOps[] = {
    {0x18B2987Bu, 4, false, false}, {0x59F02977u, 1, false, false},
    {0x45C08E62u, 4, false, false}, {0x08F4A2D4u, 16, false, true},
    {0x6D86C436u, 4, true, false},  {0xC491CD96u, 1, true, false},
    {0xCC162A96u, 4, true, false},  {0x95954635u, 16, true, true},
};

const ChannelOp* channel_op(uint32_t key) {
    for (const ChannelOp& c : kChannelOps)
        if (c.key == key) return &c;
    return nullptr;
}

Value known_u32(uint32_t v) { return Value::from_u32(v); }

Value known_bool(bool v) { return Value::from_bool(v); }

const ContextOp* context_op(uint32_t key) {
    for (const ContextOp& c : kContextOps)
        if (c.key == key) return &c;
    return nullptr;
}

} // namespace

uint32_t StateHost::path_of(const char* name) {
    uint32_t h = 5381u;
    if (!name) return h;
    for (const char* p = name; *p; ++p)
        h = ((h * 33u) & 0xFFFFFFFFu) ^ (uint32_t)(uint8_t)*p;
    return h;
}

void StateHost::set_float_named(const char* name, float value) {
    set_float(path_of(name), value);
}

void StateHost::set_u32_named(const char* name, uint32_t value) {
    set_u32(path_of(name), value);
}

void StateHost::set_bool_named(const char* name, bool value) {
    set_bool(path_of(name), value);
}

void StateHost::set_u32(uint32_t path, uint32_t value) { cells_[path] = value; }

void StateHost::set_bool(uint32_t path, bool value) {
    cells_[path] = value ? 1u : 0u;
}

void StateHost::set_float(uint32_t path, float value) {
    uint32_t raw = 0;
    std::memcpy(&raw, &value, 4);
    cells_[path] = raw;
}

/* A cell that was never seeded reads ZERO and is recorded. Zero rather than refusing
 * because a state block has authored defaults (DefaultBools/BoolCount in the
 * descriptor) and an unseeded read is a missing input, not a broken operator - and
 * the recording is what turns "the graph is tainted" into a list of what to supply. */
bool StateHost::read_cell(uint32_t path, uint32_t& out) {
    /* A read whose address is in the top page is FRAME-RELATIVE, not a hash: the
     * observed ones are 0xFFFFFFFF (the PUSH note's "no path") and 0xFFFFFF00..03,
     * four consecutive slots for four wheels. Record the frame stack alongside it,
     * because the arithmetic from (frame, offset) to a path has to be read off this
     * pairing. */
    if (trace_frames_ && path >= 0xFFFFFF00u && odd_reads_.size() < 4000) {
        ReadSeen r{};
        r.addr = path;
        r.depth = (int)frames_.size();
        for (int i = 0; i < 4; ++i)
            r.bound[i] = frames_.empty() ? 0u : frames_.back().bound[i];
        odd_reads_.push_back(r);
    }
    const auto it = cells_.find(cell_key(path));
    if (std::getenv("BF6_CELL_DEBUG"))
        std::fprintf(stderr, "cell read rec 0x%X key 0x%016llX %s depth %zu bound %08X %08X %08X %08X kind %u field %u\n",
                     cur_record_,
                     (unsigned long long)cell_key(path), it != cells_.end() ? "hit" : "MISS", frames_.size(),
                     frames_.empty() ? 0u : frames_.back().bound[0], frames_.empty() ? 0u : frames_.back().bound[1],
                     frames_.empty() ? 0u : frames_.back().bound[2], frames_.empty() ? 0u : frames_.back().bound[3],
                     cur_kind_, cur_field_);
    if (it != cells_.end()) { out = it->second; return true; }
    /* THE NATIVE PER-WHEEL STATE (see set_wheel_state). The low byte is the part
     * class and it differs by vehicle - 0x05 for a car's wheels, 0x00 for a tank's
     * road wheels - so it is LEARNED here from the graph's own read rather than
     * assumed, and only the class an index was first read with is ever served. */
    if ((path & 0xFFFF0000u) == 0xFFFF0000u && path != 0xFFFFFFFFu && cur_kind_ == 0 &&
        (cur_field_ == 3 || cur_field_ == 4)) {
        const uint32_t w = (path >> 8) & 0xFFu;
        const uint8_t cls = (uint8_t)(path & 0xFFu);
        if (w < 8) {
            if (wheel_class_[w] == 0xFF) wheel_class_[w] = cls;
            if (std::getenv("BF6_CELL_DEBUG"))
                std::fprintf(stderr, "  wheel-state gate: w %u cls %02X stored %02X known %d spin %g\n",
                             w, cls, wheel_class_[w], (int)wheel_known_[w],
                             *(const float*)&wheel_spin_[w]);
            if (wheel_class_[w] == cls && wheel_known_[w]) {
                out = cur_field_ == 3 ? wheel_spin_[w] : wheel_contact_[w];
                return true;
            }
        }
    }
    unseeded_.push_back(path);
    /* A FRAME-RELATIVE READ THAT NOTHING SEEDED is the per-part state the engine
     * would have written: a car's wheels use absolute paths (served above), but a
     * tank's tracks read the frame the graph pushed for them. Recorded in read
     * order, with its cell key, so the caller can write the tick's answer back into
     * exactly the cell that was asked for instead of guessing a spelling. */
    if (path >= 0xFFFF0000u && !frames_.empty()) {
        FrameRead fr{};
        fr.key = cell_key(path);
        fr.frame = frames_.back().bound[3];
        fr.path = path;
        fr.kind = cur_kind_;
        fr.field = cur_field_;
        if (unseeded_frame_reads_.size() < 256) unseeded_frame_reads_.push_back(fr);
        /* BF6_UNSEEDED_ONE=1 makes an unseeded per-part state read return 1 instead of
         * 0. DIAGNOSTIC: the airplane graphs ask a per-part question and take a ZERO
         * answer as "the wheel brake is applied" - they test the cell against zero
         * (17B8026A) and feed the result into the wheel-spin resistance - so the value
         * this returns decides whether an aircraft can roll at all. A car never binds
         * these paths (its descriptor is the all-ones sentinel) and reaches brake = 0
         * by another route, so this measures the aircraft path without touching cars. */
        if (std::getenv("BF6_UNSEEDED_ONE")) { out = 1; return true; }
        /* BF6_UNSEEDED_BOUND_ONE=1 narrows that to the reads whose path names a
         * PARTICULAR part - low byte not the all-ones sentinel - at field 0, which is
         * exactly the shape the aircraft brake test uses (paths FFFFFF00, FFFFFF03,
         * FFFFFF04 at frame 001479 on an f22). A car's equivalent reads carry the
         * unbound FFFFFFFF, so they are untouched by this. */
        if (std::getenv("BF6_UNSEEDED_BOUND_ONE") && cur_field_ == 0 &&
            (path & 0xFFu) != 0xFFu) { out = 1; return true; }
    }
    out = 0;
    return true;
}

bool StateHost::describe(uint32_t key, OperatorSignature& out) {
    out = OperatorSignature{};
    /* Served (BF6_NO_ENTITY_INDEX turns it off). It lets the staggered jobs run; on a
     * boat that job is a radius search for alive characters (0x16E0F8DA, 0xE88A04DB)
     * that keeps the boat awake, and it finds the driver in the character registry
     * (set_characters). Without the driver there the boat sleeps, as it would empty. */
    /* THE SEAT / DOOR OPERATORS (see invoke) */
    if (key == 0x91C21F3Cu) { out.input_widths = {260, 4}; out.output_width = 260; return true; }
    if (key == 0x0221B337u) { out.input_widths = {260, 260, 4}; out.output_width = 1; return true; }
    if (key == 0xBC689BF3u) { out.input_widths = {16}; out.output_width = 260; return true; }
    if (key == 0x2B3256FDu) { out.input_widths = {16, 260}; out.output_width = 0; return true; }
    if (key == 0xA04FF621u) { out.input_widths = {16}; out.output_width = 0; return true; }
    if (key == 0x16E0F8DAu) {   /* radius search over the characters, see invoke */
        out.input_widths = {16, 4, 4};
        out.output_width = 260;
        return true;
    }
    if (key == 0xE88A04DBu) {   /* sort a character set by a channel's value, see invoke */
        out.input_widths = {260, 4, 4};
        out.output_width = 260;
        return true;
    }
    if (key == 0xC8364385u && !std::getenv("BF6_NO_ENTITY_INDEX")) {   /* the entity index, zero inputs (see invoke) */
        out.input_widths = {};
        out.output_width = 4;
        return true;
    }
    if (key == 0x0F063D92u) {   /* the skeleton constraint, bone_constraint_solve */
        out.input_widths = {16, 16, 64};
        out.output_width = 0;
        return true;
    }
    if (key == 0xE7488ECEu) {   /* kNamedHandle, below: the named-provider lookup */
        out.input_widths = {4, 4};
        out.output_width = 8;
        return true;
    }
    if (key == kPushFrame) {
        out.input_widths = {4, 4, 4, 4};
        out.output_width = 0;
        return true;
    }
    if (key == kPopFrame) {
        /* NO operands. POP is a kind 0x23 record with three EMPTY lists (counts
         * 0/0/0); the one "operand" it seemed to take was those counts misread
         * before 0x23 was parsed by its lists. */
        out.input_widths = {};
        out.output_width = 0;
        return true;
    }
    /* Reads and writes take the full 16-byte state descriptor (see cell_key). */
    if (key == kReadBit) {
        out.input_widths = {16};
        out.output_width = 1;
        return true;
    }
    if (key == kReadU32A || key == kReadU32B) {
        out.input_widths = {16};
        out.output_width = 4;
        return true;
    }
    if (key == kFieldAddr) {
        out.input_widths = {4};
        out.output_width = 4;
        return true;
    }
    if (key == kWriteAny) {
        if (!allow_writes_) return false;
        /* FUN_142BB8D70 has one node descriptor operand and no result.  Unlike the
         * explicit scalar writers below, the value is embedded in the descriptor
         * and the native dispatches it through the field type's +0x30 virtual.  No
         * replicated object exists offline, so the native's observable effect is
         * absent; still consume the one operand with the record's real shape. */
        out.input_widths = {16};
        out.output_width = 0;
        return true;
    }
    if (key == kWriteFloatQ || key == kWriteBool || key == kWriteU32) {
        if (!allow_writes_) return false;
        /* TWO OPERANDS, MEASURED. The engine table gives these arity 1, which was
         * why this host declared one input and why every write refused: the records
         * carry TWO. Tracing what produced each one settles which is which:
         *
         *   operand 0   region 0 (constant pool)          the DESTINATION path
         *   operand 1   region 2 (slot), produced by
         *               SubtractFloat / MinInt / ClampFloat   the VALUE
         *
         * 93 float writes and 7 u32 writes across the vehicle graphs, all the same
         * shape. So a write is write(destination, value) and produces nothing. */
        out.input_widths = {16, key == kWriteBool ? 1u : 4u};
        out.output_width = 0;
        return true;
    }
    if (const ChannelOp* ch = channel_op(key)) {
        out.input_widths = {4, 4};
        if (ch->moded) out.input_widths.push_back(4);
        if (ch->write) out.input_widths.push_back(ch->width);
        out.output_width = ch->write ? 0u : ch->width;
        return true;
    }
    /* The three root operators handled in describe_call MUST also be claimed here.
     * The VM describes a call through describe_call, but ChainHost chooses which host
     * to INVOKE with describe(key) - so a key known only to describe_call is described
     * successfully and then invoked on nobody, and fails. That is exactly what kept
     * these three unresolved after they were first implemented. Fixed-shape, so the
     * per-key answer is the same as the per-call one. */
    /* 0x532B3BA9: (object, selector) -> the address of a field, which a kind 0x2E record
     * binds for the region-3 reads after it. It is the SAME nine bytes as 0x88030F01
     * (0x143B25460: object + dword[selector]) - verified from the node registry by an
     * adversarial decode. The FUN_1438068f0 (type, address) pair it was once paired with
     * in EngineNodes.tsv is not its implementation, and that pairing is what made every
     * non-zero field look unmeasured. The VM resolves it; invoke is not used. */
    if (key == 0x532B3BA9u) {
        out.input_widths = {4, 4};
        out.output_width = 16;
        out.output_is_reference_to_input0 = true;
        out.reference_offset_from_input1 = true;
        return true;
    }
    /* 0x88030F01: the same thing one field in. Nine bytes of machine code at
     * 0x143B25460 - `mov eax,[rdx]; add rax,rcx; mov [r8],rax; ret` - so the output is
     * input 0's address plus the dword at input 1. Ghidra made no function there,
     * which is why it read as unresolved for so long while gating a helicopter's rotor
     * config, a plane's jet config and a boat's last two branches at once.
     *
     * Input 1 is a POOL word, zero on disk because the loader fills it from the
     * reflected field table. The caller patches it with the field's own offset, so
     * nothing here needs to know which field this call wants. */
    if (key == 0x88030F01u) {
        out.input_widths = {4, 4, 4};
        out.output_width = 16;
        out.output_is_reference_to_input0 = true;
        out.reference_offset_from_input1 = true;
        return true;
    }
    if (key == 0xC58D8EA6u) { out.input_widths = {};           out.output_width = 4;  return true; }
    if (key == 0x6D98A861u) { out.input_widths = {16, 16, 16}; out.output_width = 64; return true; }
    if (key == 0xE22FCA6Fu) { out.input_widths = {8, 8, 4};    out.output_width = 1;  return true; }
    if (key == 0x4899CB44u) {
        /* Native 0x1443338E0: (int mode, ExpressionBoneId*, LinearTransform*).
         * It is a side effect and has no output. */
        out.input_widths = {4, 16, 64};
        out.output_width = 0;
        return true;
    }
    if (const ContextOp* c = context_op(key)) {
        out.input_widths.assign((size_t)c->arity, 4u);
        out.output_width = 4;
        return true;
    }
    return false;
}

static const uint32_t kPartTransform = 0x04BEFF62u;
static const uint32_t kSetPartTransform = 0x4899CB44u;

/* A known 16-byte value, for the soldier vec4 field reader. */
static Value vec16_value(const std::vector<uint8_t>& b) {
    Value v;
    v.bytes = b;
    v.known = true;
    return v;
}

static void lt_identity(float out[16]) {
    std::memset(out, 0, 64);
    out[0] = out[5] = out[10] = 1.0f;
}

/* Frostbite LinearTransform is four Vec3 rows (stride 16) and composes as a
 * row-vector affine transform: p' = p * a * b. */
static void lt_mul(const float a[16], const float b[16], float out[16]) {
    std::memset(out, 0, 64);
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 3; ++col) {
            out[row * 4 + col] = a[row * 4 + 0] * b[0 * 4 + col] +
                                 a[row * 4 + 1] * b[1 * 4 + col] +
                                 a[row * 4 + 2] * b[2 * 4 + col];
            if (row == 3) out[row * 4 + col] += b[3 * 4 + col];
        }
}

static bool lt_inverse(const float m[16], float out[16]) {
    const float a=m[0], b=m[1], c=m[2], d=m[4], e=m[5], f=m[6],
                g=m[8], h=m[9], i=m[10];
    const float det = a*(e*i-f*h) - b*(d*i-f*g) + c*(d*h-e*g);
    if (!std::isfinite(det) || std::fabs(det) < 1e-12f) return false;
    const float q = 1.0f / det;
    std::memset(out, 0, 64);
    out[0]=(e*i-f*h)*q; out[1]=(c*h-b*i)*q; out[2]=(b*f-c*e)*q;
    out[4]=(f*g-d*i)*q; out[5]=(a*i-c*g)*q; out[6]=(c*d-a*f)*q;
    out[8]=(d*h-e*g)*q; out[9]=(b*g-a*h)*q; out[10]=(a*e-b*d)*q;
    for (int col = 0; col < 3; ++col)
        out[12 + col] = -(m[12] * out[col] + m[13] * out[4 + col] +
                          m[14] * out[8 + col]);
    return true;
}

/* THE SKELETON CONSTRAINT (0x0F063D92), transcribed from the natives:
 *   FUN_1443342C0 -> FUN_144335CB0  resolves the two bones and gathers the poses
 *   FUN_1443324B0                   the setup from rest poses and the 64-byte config
 *   FUN_144332A40                   the solve, into the bone's LOCAL pose
 *   FUN_144185640                   the look-at basis
 * Every suspension graph ends in a block of these: a damper aims at its lower mount,
 * a spring stretches between its seats, a CV shaft hinges to follow the knuckle.
 * Rows are Frostbite LinearTransform rows (right, up, forward, translation) and points
 * transform as row vectors, as lt_mul composes. The polynomial sin/cos the native uses
 * are its own approximations of sin and cos, taken here from the standard library. */
namespace cns {
struct V3 { float x, y, z; };
static V3 v3(float x, float y, float z) { return {x, y, z}; }
static V3 row(const float* m, int r) { return {m[r * 4], m[r * 4 + 1], m[r * 4 + 2]}; }
static void set_row(float* m, int r, V3 v) { m[r * 4] = v.x; m[r * 4 + 1] = v.y; m[r * 4 + 2] = v.z; }
static V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 mul(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static float len(V3 a) { return std::sqrt(dot(a, a)); }
/* p * m, a point */
static V3 xf(const float* m, V3 p) {
    return add(add(add(mul(row(m, 0), p.x), mul(row(m, 1), p.y)), mul(row(m, 2), p.z)), row(m, 3));
}
/* v * m, a direction */
static V3 xd(const float* m, V3 v) {
    return add(add(mul(row(m, 0), v.x), mul(row(m, 1), v.y)), mul(row(m, 2), v.z));
}
/* rotate v by the quaternion (axis * sin(a/2), cos(a/2)): v + 2 q x (q x v + w v) */
static V3 qrot(V3 axis, float angle, V3 v) {
    const float s = std::sin(angle * 0.5f), w = std::cos(angle * 0.5f);
    const V3 q = mul(axis, s);
    const V3 t = add(mul(v, w), cross(q, v));
    return add(v, mul(cross(q, t), 2.0f));
}
static float sign_of(float v) { return v < 0.0f ? -1.0f : (v > 0.0f ? 1.0f : 0.0f); }
} // namespace cns

/* rest_local, rest_model: the constrained bone; rest_model_t, cur_model_t: its target;
 * cur_model_p: its parent (identity for a root); cfg: the 16-float config block;
 * out: in the bone's current local pose, out the solved one. */
static void bone_constraint_solve(const float rest_local[16], const float rest_model[16],
                                  const float rest_model_t[16], const float cur_model_t[16],
                                  const float cur_model_p[16], const float cfg[16], float out[16]) {
    using namespace cns;
    uint8_t flag_stretch = 0, flag_target_frame = 0;
    std::memcpy(&flag_stretch, (const uint8_t*)cfg + 0x38, 1);
    std::memcpy(&flag_target_frame, (const uint8_t*)cfg + 0x39, 1);
    int32_t type = 0;
    std::memcpy(&type, cfg + 13, 4);

    /* ---- FUN_1443324B0: the setup ---- */
    const float* frame = flag_target_frame ? rest_model_t : rest_model;
    const V3 tp_model = xf(frame, v3(cfg[0], cfg[1], cfg[2]));
    float inv_rest_model[16];
    if (!lt_inverse(rest_model, inv_rest_model)) return;
    const V3 L = xf(inv_rest_model, tp_model);          /* target in the bone's rest frame */
    int axis = 0;
    float sgn = 1.0f;
    /* The axis code is an INTEGER in the block (the quad's dampers carry 2, +Y), though
     * the decompile types the block as floats: 0 picks the dominant component, 1..3 are
     * +X..+Z and 4..6 are -X..-Z. */
    int32_t axis_code = 0;
    std::memcpy(&axis_code, cfg + 12, 4);
    if (axis_code == 0) {                               /* the dominant component */
        const float ax = std::fabs(L.x), ay = std::fabs(L.y), az = std::fabs(L.z);
        if (std::max(ay, az) < ax) { axis = 0; sgn = sign_of(L.x); }
        else if (ay <= az)         { axis = 2; sgn = sign_of(L.z); }
        else                       { axis = 1; sgn = sign_of(L.y); }
    } else {                                            /* 1..3 = +X..+Z, 4..6 = -X..-Z */
        const int code = axis_code;
        axis = ((code - 1) % 3 + 3) % 3;
        sgn = code < 4 ? 1.0f : -1.0f;
    }
    const V3 aim_row = mul(row(rest_local, axis), sgn);
    bool plane = false;
    V3 normal{0, 0, 0};
    {
        const V3 n = v3(cfg[8], cfg[9], cfg[10]);
        const float n2 = dot(n, n);
        if (1e-6f < n2) { plane = true; normal = xd(rest_local, mul(n, 1.0f / std::sqrt(n2))); }
    }
    const V3 unit = axis == 0 ? v3(1, 0, 0) : axis == 1 ? v3(0, 1, 0) : v3(0, 0, 1);
    const float along = dot(L, mul(unit, sgn));
    const V3 aim_rest = xf(rest_local, mul(unit, sgn * along));   /* on the aim axis, parent space */
    const float rest_len = len(sub(aim_rest, row(rest_local, 3)));
    float inv_rest_model_t[16];
    if (!lt_inverse(rest_model_t, inv_rest_model_t)) return;
    const V3 tp_in_target = xf(inv_rest_model_t, xf(rest_model, L));
    const V3 tp_rest_parent = xf(rest_local, L);

    /* ---- FUN_144332A40: the solve ---- */
    float inv_p[16];
    if (!lt_inverse(cur_model_p, inv_p)) return;
    V3 delta = sub(xf(inv_p, xf(cur_model_t, tp_in_target)), tp_rest_parent);
    if (plane) delta = mul(normal, dot(delta, normal));
    const V3 aim = add(add(v3(cfg[4], cfg[5], cfg[6]), delta), aim_rest);
    const V3 pos = row(out, 3);
    const float stretch = flag_stretch && rest_len > 0.0f ? len(sub(pos, aim)) / rest_len : 1.0f;

    if (type == 3) {                                    /* look-at, FUN_144185640 */
        const V3 up = axis == 1 ? v3(0, 0, -sgn) : v3(0, 1, 0);
        V3 f = sub(aim, pos);
        const float fl = dot(f, f);
        f = fl < 1e-6f ? v3(0, 0, 1) : mul(f, 1.0f / std::sqrt(fl));
        V3 u = up;
        if (0.9999f < std::fabs(dot(f, up))) u = v3(up.y, up.z, up.x);
        V3 r = cross(u, f);
        r = mul(r, 1.0f / std::max(len(r), 1e-12f));
        const V3 b1 = cross(f, r);
        if (axis == 0) {
            set_row(out, 0, mul(f, sgn * stretch));
            set_row(out, 2, mul(r, -sgn));
            set_row(out, 1, b1);
        } else if (axis == 1) {
            set_row(out, 1, mul(f, sgn * stretch));
            set_row(out, 0, r);
            set_row(out, 2, mul(b1, -sgn));
        } else {
            set_row(out, 2, mul(f, sgn * stretch));
            set_row(out, 0, mul(r, sgn));
            set_row(out, 1, b1);
        }
        return;
    }
    if (type == 0 || type == 1 || type == 2) {          /* hinge about a rest axis */
        const V3 hinge = row(rest_local, type);
        V3 d = sub(aim, pos);
        d = sub(d, mul(hinge, dot(d, hinge)));
        const float dl = len(d);
        if (dl > 0.0f) d = mul(d, 1.0f / dl);
        float c = dot(d, aim_row);
        c = std::max(-1.0f, std::min(1.0f, c));
        float a = std::acos(c);
        if (type == 0 && 0.0f < dot(d, row(rest_local, 1))) a = -a;
        if (type == 1 && dot(d, row(rest_local, 0)) < 0.0f) a = -a;
        if (type == 2 && 0.0f < dot(d, row(rest_local, 1))) a = -a;
        if (type == 0) {
            set_row(out, 2, qrot(hinge, a, row(rest_local, 2)));
            set_row(out, 1, cross(row(out, 2), row(out, 0)));
        } else if (type == 1) {
            set_row(out, 2, qrot(hinge, a, row(rest_local, 2)));
            set_row(out, 0, cross(row(out, 1), row(out, 2)));
        } else {
            set_row(out, 0, qrot(hinge, a, row(rest_local, 0)));
            set_row(out, 1, cross(row(out, 2), row(out, 0)));
        }
    }
    if (flag_stretch) {                                 /* LAB_14433362B */
        V3 r = row(out, axis);
        const float rl = len(r);
        if (rl > 0.0f) set_row(out, axis, mul(r, stretch / rl));
    }
}

/* THREE ROOT OPERATORS, each identified by measuring the records rather than trusting
 * the table's arity.
 *
 *   0xC58D8EA6  ONE operand and it is the output: zero inputs, float out. 1,050 uses,
 *               consumed by float multiplies and divides and divided BY 44 times - the
 *               shape of a frame delta, and the FX logic names a GetDeltaTimeNode. The
 *               value is a stand-in (1/60 s) until a real frame clock drives it.
 *   0x6D98A861  Function_6d98a861, LinearTransform ExpressionStdLib(Vec3 Translation,
 *               Vec3 Rotation, Vec3 Scale): 4 operands, three Vec3 in, a transform out.
 *               The Euler order is NOT verified; it composes X then Y then Z, the same
 *               stated convention as LinearTransformfrom{X,Y,Z}angle.
 *   0xE22FCA6F  "out = operand0 == operand1, 64-bit exact": 4 operands, three in, a
 *               bool out. The third input is read and not used - the note accounts
 *               only for the first two. */
static const uint32_t kDeltaTime = 0xC58D8EA6u;
static const uint32_t kTRS = 0x6D98A861u;
static const uint32_t kEq64 = 0xE22FCA6Fu;
static const uint32_t kNamedTransform = 0xABAAAD01u; /* see the header */
/* 0xE7488ECE, engine node FUN_14432A210: looks the id up in the engine's registry of
 * named providers and, when found, writes an 8-byte packed handle whose low 20 bits
 * are the provider's index; the unbound default is 0x000FFFFF / 0xFFFF / 0 / 0, the
 * sentinel 0xF0F74455 tests against. Its consumers ask whether a named transform
 * exists before they read it (the quad's suspension gates all its detail parts on
 * RootTransform). Offline the providers are exactly the transforms this host
 * publishes, so a published name answers a bound handle (index 0: the consumers seen
 * only test validity) and an unpublished one stays unknown rather than guessed. */
static const uint32_t kNamedHandle = 0xE7488ECEu;

void StateHost::set_named_transform(uint32_t name_hash, const float rows[16]) {
    std::vector<uint8_t> bytes(64, 0);
    std::memcpy(bytes.data(), rows, 64);
    named_transforms_[name_hash] = bytes;
}

void StateHost::set_skeleton_bone(int32_t index, int32_t parent,
                                  const float local[16], const float model[16]) {
    if (index < 0) return;
    if ((size_t)index >= skeleton_poses_.size()) skeleton_poses_.resize((size_t)index + 1);
    SkeletonPose& p = skeleton_poses_[(size_t)index];
    p.parent = parent;
    std::memcpy(p.rest_local.data(), local, 64);
    std::memcpy(p.rest_model.data(), model, 64);
    std::memcpy(p.local.data(), local, 64);
    std::memcpy(p.model.data(), model, 64);
}

/* Write a bone's LOCAL pose and recompose its subtree, so a later getter in this
 * evaluation observes it immediately. Bone indices are topological. */
void StateHost::commit_local(int32_t index, const float local[16]) {
    if (index < 0 || (size_t)index >= skeleton_poses_.size()) return;
    std::memcpy(skeleton_poses_[(size_t)index].local.data(), local, 64);
    std::vector<uint8_t> changed(skeleton_poses_.size(), 0);
    changed[(size_t)index] = 1;
    for (size_t n = (size_t)index; n < skeleton_poses_.size(); ++n) {
        const int32_t p = skeleton_poses_[n].parent;
        if (n != (size_t)index && p >= 0 && changed[(size_t)p]) changed[n] = 1;
        if (!changed[n]) continue;
        if (p >= 0 && (size_t)p < skeleton_poses_.size())
            lt_mul(skeleton_poses_[n].local.data(), skeleton_poses_[(size_t)p].model.data(),
                   skeleton_poses_[n].model.data());
        else
            std::memcpy(skeleton_poses_[n].model.data(), skeleton_poses_[n].local.data(), 64);
    }
    for (const auto& mapped : skeleton_bone_index_) {
        const int32_t n = mapped.second;
        if (n < 0 || (size_t)n >= changed.size() || !changed[(size_t)n]) continue;
        set_bone_pose(mapped.first, 0, skeleton_poses_[(size_t)n].local.data());
        set_bone_pose(mapped.first, 1, skeleton_poses_[(size_t)n].model.data());
        set_bone_pose(mapped.first, 2, skeleton_poses_[(size_t)n].model.data());
    }
}

void StateHost::begin_bone_tick() {
    bone_writes_.clear();
    for (SkeletonPose& p : skeleton_poses_) {
        p.local = p.rest_local;
        p.model = p.rest_model;
    }
    for (const auto& mapped : skeleton_bone_index_) {
        const int32_t n = mapped.second;
        if (n < 0 || (size_t)n >= skeleton_poses_.size()) continue;
        set_bone_pose(mapped.first, 0, skeleton_poses_[(size_t)n].local.data());
        set_bone_pose(mapped.first, 1, skeleton_poses_[(size_t)n].model.data());
        set_bone_pose(mapped.first, 2, skeleton_poses_[(size_t)n].model.data());
    }
}

void StateHost::map_skeleton_bone(uint32_t channel_hash, int32_t index) {
    if (index < 0 || (size_t)index >= skeleton_poses_.size()) return;
    skeleton_bone_index_[channel_hash] = index;
    set_bone_pose(channel_hash, 0, skeleton_poses_[(size_t)index].local.data());
    set_bone_pose(channel_hash, 1, skeleton_poses_[(size_t)index].model.data());
    set_bone_pose(channel_hash, 2, skeleton_poses_[(size_t)index].model.data());
}

bool StateHost::describe_call(uint32_t key, const std::vector<uint32_t>& consts,
                              OperatorSignature& out) {
    if (key == kNamedTransform && consts.size() >= 3) {
        /* All three operands constants and a mode this measurement has seen;
         * anything else is refused rather than guessed. */
        if (consts[1] == 0xFFFFFFFFu || consts[2] > 2u) { out = OperatorSignature{}; return false; }
        out.input_widths = {4, 4, 4};
        out.output_width = 64;
        return true;
    }
    if (key == kNamedHandle && consts.size() >= 2) {
        out.input_widths = {4, 4};
        out.output_width = 8;
        return true;
    }
    if (key == kPartTransform && !consts.empty()) {
        /* Operand 0 is a mode, 0 or 1, and BOTH return a 64-byte LinearTransform.
         * CORRECTED: mode 1 was declared a float on the strength of two SignFloat
         * consumers, but the flyer60 suspension reads three mode-1 outputs as
         * transforms - their translation rows (+0x30) are the wheel point and the
         * steering geometry. A SignFloat reading a row's .x fits a transform too.
         * Any other mode is refused rather than guessed. */
        if (consts[0] > 2u) { out = OperatorSignature{}; return false; }
        /* The third input is an ExpressionBoneId, 16 bytes per the reflection. */
        out.input_widths = {4, 4, 16};
        out.output_width = 64u;
        return true;
    }
    if (key == kDeltaTime) { out.input_widths = {};           out.output_width = 4;  return true; }
    if (key == kTRS)       { out.input_widths = {16, 16, 16}; out.output_width = 64; return true; }
    if (key == kEq64)      { out.input_widths = {8, 8, 4};    out.output_width = 1;  return true; }
    return describe(key, out);
}

bool StateHost::invoke(uint32_t key, const std::vector<Value>& args, Value& out) {
    if (key == kSetPartTransform) {
        if (args.size() != 3 || !args[0].known || !args[1].known || !args[2].known ||
            args[1].bytes.size() < 16 || args[2].bytes.size() < 64) return false;
        const uint32_t mode = args[0].as_u32();
        if (mode > 2u) return false;
        uint32_t bone_hash = 0;
        std::memcpy(&bone_hash, args[1].bytes.data(), 4);
        float requested[16], local[16];
        std::memcpy(requested, args[2].bytes.data(), 64);
        /* Native 0x1443338E0 rejects a transform when any of its twelve
         * meaningful floats is NaN or infinity. Padding lanes are ignored. */
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 3; ++col)
                if (!std::isfinite(requested[row * 4 + col])) return false;
        std::memcpy(local, requested, 64);

        const auto bi = skeleton_bone_index_.find(bone_hash);
        if (bi != skeleton_bone_index_.end() && bi->second >= 0 &&
            (size_t)bi->second < skeleton_poses_.size()) {
            const int32_t index = bi->second;
            const int32_t parent = skeleton_poses_[(size_t)index].parent;
            float desired_model[16];
            std::memcpy(desired_model, requested, 64);
            if (mode == 2u) {
                float root[16], inv_root[16];
                lt_identity(root);
                const auto ri = named_transforms_.find(0x5F9C8163u);
                if (ri != named_transforms_.end() && ri->second.size() >= 64)
                    std::memcpy(root, ri->second.data(), 64);
                if (!lt_inverse(root, inv_root)) return false;
                lt_mul(requested, inv_root, desired_model);
            }
            if (mode != 0u && parent >= 0 && (size_t)parent < skeleton_poses_.size()) {
                float inv_parent[16];
                if (!lt_inverse(skeleton_poses_[(size_t)parent].model.data(), inv_parent)) return false;
                lt_mul(desired_model, inv_parent, local);
            } else if (mode != 0u) {
                std::memcpy(local, desired_model, 64);
            }
            commit_local(index, local);
        } else {
            /* Retain unknown channel writes too: a later same-mode getter still
             * sees exactly what was written, even if this rig lacks the mapping. */
            set_bone_pose(bone_hash, mode, requested);
        }
        std::vector<uint8_t>& written = bone_writes_[bone_hash];
        written.resize(64);
        std::memcpy(written.data(), local, 64);
        served_[key] += 1;
        out = Value{};
        return true;
    }
    if (key == kDeltaTime) {
        if (!args.empty()) return false;
        served_[key] += 1;
        const float dt = 1.0f / 60.0f;                     /* STAND-IN frame delta */
        out.bytes.assign(4, 0);
        std::memcpy(out.bytes.data(), &dt, 4);
        out.known = true;
        return true;
    }
    if (key == kEq64) {
        if (args.size() != 3 || !args[0].known || !args[1].known) return false;
        served_[key] += 1;
        const bool eq = args[0].bytes.size() >= 8 && args[1].bytes.size() >= 8 &&
                        std::memcmp(args[0].bytes.data(), args[1].bytes.data(), 8) == 0;
        out = Value::from_bool(eq);
        return true;
    }
    if (key == kTRS) {
        if (args.size() != 3) return false;
        for (const Value& v : args) if (!v.known || v.bytes.size() < 12) return false;
        served_[key] += 1;
        float t[3], r[3], s[3];
        std::memcpy(t, args[0].bytes.data(), 12);
        std::memcpy(r, args[1].bytes.data(), 12);
        std::memcpy(s, args[2].bytes.data(), 12);
        /* Rows right/up/forward, each scaled, rotated X then Y then Z (unverified
         * order, see the note above), translation in row 3. */
        const float cx = std::cos(r[0]), sx = std::sin(r[0]);
        const float cy = std::cos(r[1]), sy = std::sin(r[1]);
        const float cz = std::cos(r[2]), sz = std::sin(r[2]);
        const float X[3][3] = {{1,0,0},{0,cx,sx},{0,-sx,cx}};
        const float Y[3][3] = {{cy,0,-sy},{0,1,0},{sy,0,cy}};
        const float Z[3][3] = {{cz,sz,0},{-sz,cz,0},{0,0,1}};
        float XY[3][3], R[3][3];
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
            XY[i][j] = X[i][0]*Y[0][j] + X[i][1]*Y[1][j] + X[i][2]*Y[2][j];
        }
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
            R[i][j] = XY[i][0]*Z[0][j] + XY[i][1]*Z[1][j] + XY[i][2]*Z[2][j];
        }
        out.bytes.assign(64, 0);
        for (int i = 0; i < 3; ++i) {
            float row[3] = {R[i][0]*s[i], R[i][1]*s[i], R[i][2]*s[i]};
            std::memcpy(out.bytes.data() + (size_t)i * 16, row, 12);
        }
        std::memcpy(out.bytes.data() + 48, t, 12);
        out.known = true;
        return true;
    }
    if (const ChannelOp* ch = channel_op(key)) {
        const size_t want = 2u + (ch->moded ? 1u : 0u) + (ch->write ? 1u : 0u);
        if (args.size() != want || !args[1].known) return false;
        const uint32_t mode = ch->moded && args[2].known ? args[2].as_u32() : 0u;
        const uint64_t ck = ((uint64_t)mode << 32) | args[1].as_u32();
        served_[key] += 1;
        if (ch->write) {
            const Value& v = args.back();
            /* AN UNKNOWN WRITE USED TO ERASE THE CHANNEL, and a read of an absent
             * channel returns a KNOWN zero - so one refused contribution to an
             * accumulated channel wiped everything summed before it and handed the
             * rest of the graph a trusted 0. Every aircraft read AngularAcceleration
             * as exactly (0,0,0) that way: 17.7 of gear torque, then a refused wing,
             * then zero. The channel now stays UNKNOWN until the
             * next known write. BF6_CHANNEL_LAUNDER=1 restores the old erase, for A/B. */
            static const bool honest = std::getenv("BF6_CHANNEL_LAUNDER") == nullptr;
            /* BF6_CHANNEL_TRACE=<hex hash>: every write of that channel, all lanes, in
             * order - the accumulation chain of a force channel one step at a time. */
            if (const char* tr = std::getenv("BF6_CHANNEL_TRACE"))
                if ((uint32_t)ck == (uint32_t)std::strtoul(tr, nullptr, 16)) {
                    float f[3] = {0, 0, 0};
                    if (v.bytes.size() == 1)
                        f[0] = v.bytes[0] ? 1.0f : 0.0f;
                    else if (!v.bytes.empty())
                        std::memcpy(f, v.bytes.data(), std::min<size_t>(v.bytes.size(), 12));
                    std::fprintf(stderr, "chtrace %08X rec 0x%X %s %g %g %g\n", (uint32_t)ck,
                                 cur_record_, v.known ? "known" : "UNKNOWN", f[0], f[1], f[2]);
                }
            if (v.known) {
                channels_[ck] = std::vector<uint8_t>(v.bytes.begin(),
                                v.bytes.begin() + std::min<size_t>(v.bytes.size(), ch->width));
                unknown_channels_.erase(ck);
            } else {
                channels_.erase(ck);
                if (honest) unknown_channels_.insert(ck);
            }
            channel_writes_[ck] += 1;
            out = Value{};
            return true;
        }
        if (unknown_channels_.count(ck)) return false;   /* honest: still unknown */
        const auto it = channels_.find(ck);
        out.bytes.assign(ch->width, 0);
        if (it != channels_.end())
            std::memcpy(out.bytes.data(), it->second.data(), std::min<size_t>(it->second.size(), ch->width));
        else
            unsupplied_channels_[ck] += 1;       /* reads ZERO, recorded */
        out.known = true;
        return true;
    }
    /* THE SEAT / DOOR OPERATORS, from the natives (Codex study, corpus 6a1c1b):
     *   0x91C21F3C  FUN_1475F9B20: from the vehicle in the input collection, the entry
     *               at the index (the vehicle's +0xD8 entry vector), its associated
     *               player's entity: a collection of zero or one id
     *   0x0221B337  FUN_143B67BD0: reflection equality of two values of the given type
     *               (here two id collections), true when equal
     *   0xBC689BF3  FUN_1443364B0: read the 260-byte value stored at a state locator
     *   0x2B3256FD  FUN_144336550: write it back
     *   0xA04FF621  FUN_142BB8D70: the locator's storage handler, slot +0x30 - its
     *               target is unresolved; taken as a reset of that state to default,
     *               stated
     * So a door graph compares the seat's occupant with the one it stored and animates
     * the door when they differ - someone got in or out. */
    auto empty_collection = []() {
        std::vector<uint8_t> b(260, 0);
        const uint32_t none = 0x000FFFFFu;
        for (int i = 1; i <= 64; ++i) std::memcpy(b.data() + 4 * i, &none, 4);
        return b;
    };
    if (key == 0x91C21F3Cu) {
        if (args.size() != 2 || !args[0].known || !args[1].known || args[0].bytes.size() < 8) return false;
        served_[key] += 1;
        out = Value{};
        out.bytes = empty_collection();
        out.known = true;
        uint32_t count = 0;
        std::memcpy(&count, args[0].bytes.data(), 4);
        const uint32_t seat = args[1].as_u32();
        if (count > 0 && seat < seats_.size() && seats_[seat] != 0) {
            const uint32_t one = 1;
            std::memcpy(out.bytes.data(), &one, 4);
            std::memcpy(out.bytes.data() + 4, &seats_[seat], 4);
        }
        return true;
    }
    if (key == 0x0221B337u) {
        if (args.size() != 3 || !args[0].known || !args[1].known ||
            args[0].bytes.size() < 260 || args[1].bytes.size() < 260) return false;
        served_[key] += 1;
        out = known_bool(std::memcmp(args[0].bytes.data(), args[1].bytes.data(), 260) == 0);
        return true;
    }
    if (key == 0xBC689BF3u) {
        if (args.empty() || !args[0].known) return false;
        served_[key] += 1;
        take_descriptor(args[0]);
        const auto it = wide_cells_.find(cell_key(args[0].as_u32()));
        out = Value{};
        out.bytes = it != wide_cells_.end() ? it->second : empty_collection();
        out.known = true;
        return true;
    }
    if (key == 0x2B3256FDu) {
        if (args.size() != 2 || !args[0].known || !args[1].known || args[1].bytes.size() < 260) return false;
        served_[key] += 1;
        take_descriptor(args[0]);
        wide_cells_[cell_key(args[0].as_u32())] =
            std::vector<uint8_t>(args[1].bytes.begin(), args[1].bytes.begin() + 260);
        out = Value{};
        out.known = true;
        return true;
    }
    if (key == 0xA04FF621u) {
        if (args.empty() || !args[0].known) return false;
        served_[key] += 1;
        take_descriptor(args[0]);
        const uint64_t k = cell_key(args[0].as_u32());
        wide_cells_.erase(k);
        cells_.erase(k);
        out = Value{};
        out.known = true;
        return true;
    }
    if (key == 0x16E0F8DAu) {
        /* FUN_14433DA50 via thunk 147EEBF10: every character whose bounds, grown by the
         * radius, reach the point, as a count-prefixed id set (4 + 64 ids = 260 bytes,
         * capped at 64). The filter struct's flags: +8 grows each bounds by an extra
         * margin, +9 excludes the evaluating entity's own character, +10 a second one;
         * a set flag is refused here rather than guessed at. */
        if (args.size() != 3 || !args[0].known || !args[1].known || !args[2].known ||
            args[0].bytes.size() < 12 || args[1].bytes.size() < 4) return false;
        float p[3], radius = 0.0f;
        std::memcpy(p, args[0].bytes.data(), 12);
        std::memcpy(&radius, args[1].bytes.data(), 4);
        if (heap_) {
            std::vector<uint8_t> filter;
            if (heap_->pool(args[2].as_u32(), 16, filter) && filter.size() >= 11 &&
                (filter[8] || filter[9] || filter[10])) return false;
        }
        served_[key] += 1;
        std::vector<uint32_t> set(65, 0u);
        uint32_t n = 0;
        for (const Character& c : characters_) {
            if (!c.active || c.id == 0x000FFFFFu || n >= 64) continue;
            const float dx = c.center[0] - p[0], dy = c.center[1] - p[1], dz = c.center[2] - p[2];
            const float reach = std::sqrt(c.half[0] * c.half[0] + c.half[1] * c.half[1] +
                                          c.half[2] * c.half[2]) + radius;
            if (dx * dx + dy * dy + dz * dz <= reach * reach) set[1 + n++] = c.id;
        }
        set[0] = n;
        out.bytes.assign(260, 0);
        std::memcpy(out.bytes.data(), set.data(), 260);
        out.known = true;
        return true;
    }
    if (key == 0xE88A04DBu) {
        /* FUN_144340420 with ONE condition: the ids of the input set whose value of the
         * bound channel equals the given value (FUN_14434BC60 compares each condition
         * equal / not-equal and combines them And / Or). Only the HealthState channel is
         * known for a character offline, and only the single-condition form is served;
         * the condition's compare mode sits in the channel's runtime entry, which this
         * loader does not expose, so EQUALS - the form every graph seen here uses, a
         * state value to match - is assumed and anything else refused. */
        if (args.size() != 3 || !args[0].known || !args[1].known || !args[2].known ||
            args[0].bytes.size() < 260) return false;
        const uint32_t kHealthState = 0x4BB2B05Bu, kOpenDoor = 0x8990F80Fu;
        uint32_t channel = args[1].as_u32();
        const auto cc = condition_channel_.find(channel);   /* a config pointer: its channel */
        if (cc != condition_channel_.end()) channel = cc->second;
        if (channel != kHealthState && channel != kOpenDoor) return false;
        const int32_t want = (int32_t)args[2].as_u32();
        served_[key] += 1;
        uint32_t in[65];
        std::memcpy(in, args[0].bytes.data(), 260);
        std::vector<uint32_t> set(65, 0u);
        uint32_t n = 0;
        for (uint32_t i = 0; i < std::min<uint32_t>(in[0], 64u); ++i)
            for (const Character& c : characters_)
                if (c.id == in[1 + i] && n < 64 &&
                    (channel == kHealthState ? c.health_state : c.open_door) == want) set[1 + n++] = c.id;
        set[0] = n;
        out.bytes.assign(260, 0);
        std::memcpy(out.bytes.data(), set.data(), 260);
        out.known = true;
        return true;
    }
    if (key == 0xC8364385u) {
        /* FUN_142BBC2C0: the evaluating entity's index in the engine's entity table (the
         * unbound sentinel 0x000FFFFF when it is not registered). Graphs use it only to
         * STAGGER periodic work across entities - the F-14 casts its airborne ray on
         * the ticks where (index + tick / N) % M matches - so any stable index is a
         * valid phase; in the game it is spawn order. The single offline vehicle is
         * entity 0. */
        if (!args.empty()) return false;
        served_[key] += 1;
        out = Value::from_u32(0u);
        return true;
    }
    if (key == 0x0F063D92u) {
        /* FUN_144335CB0: both bones resolved, or nothing happens (the native returns). */
        if (args.size() != 3 || !args[0].known || !args[1].known || !args[2].known ||
            args[0].bytes.size() < 4 || args[1].bytes.size() < 4 || args[2].bytes.size() < 64)
            return false;
        served_[key] += 1;
        out = Value{};
        out.known = true;
        uint32_t hb = 0, ht = 0;
        std::memcpy(&hb, args[0].bytes.data(), 4);
        std::memcpy(&ht, args[1].bytes.data(), 4);
        const auto bi = skeleton_bone_index_.find(hb), ti = skeleton_bone_index_.find(ht);
        if (bi == skeleton_bone_index_.end() || ti == skeleton_bone_index_.end()) return true;
        const int32_t b = bi->second, t = ti->second;
        if (b < 0 || t < 0 || (size_t)b >= skeleton_poses_.size() || (size_t)t >= skeleton_poses_.size())
            return true;
        const SkeletonPose& pb = skeleton_poses_[(size_t)b];
        const SkeletonPose& pt = skeleton_poses_[(size_t)t];
        float parent_model[16];
        lt_identity(parent_model);
        if (pb.parent >= 0 && (size_t)pb.parent < skeleton_poses_.size())
            std::memcpy(parent_model, skeleton_poses_[(size_t)pb.parent].model.data(), 64);
        float cfg[16], local[16];
        std::memcpy(cfg, args[2].bytes.data(), 64);
        std::memcpy(local, pb.local.data(), 64);
        bone_constraint_solve(pb.rest_local.data(), pb.rest_model.data(), pt.rest_model.data(),
                              pt.model.data(), parent_model, cfg, local);
        for (int i = 0; i < 16; ++i) if (!std::isfinite(local[i])) return true;
        commit_local(b, local);
        std::vector<uint8_t>& written = bone_writes_[hb];
        written.resize(64);
        std::memcpy(written.data(), local, 64);
        return true;
    }
    if (key == kNamedHandle && std::getenv("BF6_NAMED_HANDLE_DEBUG")) {
        std::fprintf(stderr, "named handle: %zu arg(s)", args.size());
        for (const auto& a : args) std::fprintf(stderr, " %s%08X", a.known ? "" : "?", a.as_u32());
        std::fprintf(stderr, ", %zu published\n", named_transforms_.size());
    }
    if (key == kNamedHandle && args.size() == 2 && args[1].known) {
        const uint32_t name = args[1].as_u32();
        if (named_transforms_.find(name) == named_transforms_.end()) return false;
        served_[key] += 1;
        out.bytes.assign(8, 0);   /* index 0, bound */
        out.known = true;
        return true;
    }
    if (key == kNamedTransform && args.size() == 3 && args[1].known && args[2].known) {
        served_[key] += 1;
        const uint32_t name = args[1].as_u32();
        const auto it = named_transforms_.find(name);
        if (it != named_transforms_.end()) {
            out.bytes = it->second;
        } else {
            /* UNSUPPLIED IS UNKNOWN, NOT IDENTITY. A named transform is published by a
             * native component (a helicopter's rotor, a linked sub-skeleton). Reading
             * identity for one nobody publishes made skeleton-link graphs copy an
             * identity pose - zero translation included - over bones another graph had
             * just posed: the MH-47's link graph flattened the rotors that its own rotor
             * graph was spinning. Unknown lets the dependent write refuse, so the bone
             * keeps its last pose. Still recorded, so the gap stays visible. */
            unsupplied_transforms_[name] += 1;
            out.bytes.assign(64, 0);
            static const bool old_identity = std::getenv("BF6_NAMED_IDENTITY") != nullptr;
            if (old_identity) {                          /* the old rule, to compare */
                const float one = 1.0f;
                std::memcpy(out.bytes.data() + 0,  &one, 4);
                std::memcpy(out.bytes.data() + 20, &one, 4);
                std::memcpy(out.bytes.data() + 40, &one, 4);
                out.known = true;
                return true;
            }
            out.known = false;
            return true;
        }
        out.known = true;
        return true;
    }
    if (key == kPartTransform) {
        /* Mode comes from operand 0, which the VM has already read as a constant. */
        if (args.size() != 3 || !args[0].known) return false;
        const uint32_t mode = args[0].as_u32();
        /* Modes from the exe (FUN_144333a60): 0 LOCAL, 1 MODEL, 2 WORLD. */
        if (mode > 2u || !args[1].known) return false;
        served_[key] += 1;
        PartKey pk{mode, args[1].as_u32(), {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu}};
        if (args[2].known && args[2].bytes.size() >= 16)
            std::memcpy(pk.bone, args[2].bytes.data(), 16);
        const auto it = part_transforms_.find(pk);
        const auto bp = bone_poses_.find(((uint64_t)mode << 32) | pk.bone[0]);
        if (it != part_transforms_.end()) {
            out.bytes = it->second;
        } else if (bp != bone_poses_.end()) {
            out.bytes = bp->second;
            /* WORLD = MODEL x the entity's world transform. Offline that is the
             * RootTransform channel when a caller placed the vehicle. */
            const auto root = named_transforms_.find(0x5F9C8163u);
            if (mode == 2u && root != named_transforms_.end() && root->second.size() >= 64) {
                float m[16], r[16], o[16] = {};
                std::memcpy(m, out.bytes.data(), 64);
                std::memcpy(r, root->second.data(), 64);
                for (int row = 0; row < 4; ++row)
                    for (int col = 0; col < 3; ++col) {
                        float v = m[row * 4 + 0] * r[0 * 4 + col] + m[row * 4 + 1] * r[1 * 4 + col] +
                                  m[row * 4 + 2] * r[2 * 4 + col];
                        if (row == 3) v += r[3 * 4 + col];
                        o[row * 4 + col] = v;
                    }
                std::memcpy(out.bytes.data(), o, 64);
            }
        } else {
            unsupplied_parts_[pk] += 1;
            /* Identity LinearTransform: four Vec3 rows at stride 16, recorded. */
            out.bytes.assign(64, 0);
            const float one = 1.0f;
            std::memcpy(out.bytes.data() + 0,  &one, 4);   /* right.x   */
            std::memcpy(out.bytes.data() + 20, &one, 4);   /* up.y      */
            std::memcpy(out.bytes.data() + 40, &one, 4);   /* forward.z */
        }
        out.known = true;
        return true;
    }
    OperatorSignature signature;
    if (!describe(key, signature) ||
        args.size() != signature.input_widths.size())
        return false;
    served_[key] += 1;

    if (key == kPushFrame) {
        /* ALL FOUR operands are kept. The note says "binding up to three paths;
         * 0xFFFFFFFF means no path" but the operator's arity is 4, and which operand
         * is the odd one out was never established - so recording three and guessing
         * at the fourth is exactly the mistake that made reads land on sentinels. */
        Frame f{};
        PushSeen seen{};
        /* THE FALLBACK MUST NOT COLLIDE WITH THE DATA. An earlier version used
         * 0xFFFFFFFF for an operand that arrived !known - which is the exact value
         * the PUSH note says means "no path". Two runs of analysis were spent on
         * "the operands arrive unknown" before the real reading turned out to be
         * "the operands are known and say NO PATH". The known flag is now carried
         * separately so the two can never be confused again. */
        seen.known_mask = 0;
        for (size_t i = 0; i < 4; ++i) {
            const bool k = i < args.size() && args[i].known;
            if (k) seen.known_mask |= (1u << i);
            const uint32_t p = k ? args[i].as_u32() : 0xDEAD0000u + (uint32_t)i;
            f.bound[i] = p;
            seen.arg[i] = p;
        }
        frames_.push_back(f);
        if (std::getenv("BF6_CELL_DEBUG"))
            std::fprintf(stderr, "push depth %zu  %08X %08X %08X %08X\n", frames_.size(),
                         f.bound[0], f.bound[1], f.bound[2], f.bound[3]);
        if (trace_frames_ && pushes_seen_.size() < 4000)
            pushes_seen_.push_back(seen);
        out = Value{};
        return true;
    }
    if (key == kPopFrame) {
        if (!frames_.empty()) frames_.pop_back();
        out = Value{};
        return true;
    }
    if (key == kFieldAddr) {
        /* Offline the path IS the identity - the engine's own note. */
        if (!args[0].known) return false;
        out = known_u32(args[0].as_u32());
        return true;
    }
    if (key == kReadBit) {
        if (!args[0].known) return false;
        uint32_t raw = 0;
        take_descriptor(args[0]);
        read_cell(args[0].as_u32(), raw);
        out = known_bool(raw != 0);
        return true;
    }
    if (key == kReadU32A || key == kReadU32B) {
        if (!args[0].known) return false;
        uint32_t raw = 0;
        take_descriptor(args[0]);
        read_cell(args[0].as_u32(), raw);
        if (std::getenv("BF6_CELL_DEBUG") && args[0].bytes.size() >= 16) {
            uint32_t d[4];
            std::memcpy(d, args[0].bytes.data(), 16);
            float fv; std::memcpy(&fv, &raw, 4);
            std::fprintf(stderr, "u32 read desc %08X %08X %08X %08X -> %08X (%g)\n", d[0], d[1], d[2], d[3], raw, fv);
        }
        out = known_u32(raw);
        return true;
    }
    if (key == kWriteAny) {
        if (args.size() != 1 || !args[0].known || args[0].bytes.size() < 16) return false;
        ++writes_;
        served_[key] += 1;
        out = Value{};
        out.known = true;
        return true;
    }
    if (key == kWriteFloatQ || key == kWriteBool || key == kWriteU32) {
        /* args[0] is the destination path, args[1] the value - measured, see
         * describe(). An unknown destination is refused rather than written to a
         * fabricated address; an unknown VALUE is stored as unknown by marking the
         * cell absent, so a later read reports it rather than reading a zero that was
         * never computed.
         *
         * The float write is "QUANTISED by the field's range" per the engine note,
         * and the range lives in the descriptor's Replication/Correction network
         * data. That is not modelled here, so the value is stored unquantised - which
         * is MORE precise than the game, not less, and is the one place this host
         * knowingly differs. */
        if (!args[0].known) return false;
        take_descriptor(args[0]);
        const uint32_t dest = args[0].as_u32();
        if (!args[1].known) { cells_.erase(cell_key(dest)); out = Value{}; return true; }
        cells_[cell_key(dest)] = args[1].as_u32();
        if (std::getenv("BF6_CELL_DEBUG"))
            std::fprintf(stderr, "cell write rec 0x%X op %08X key 0x%016llX value 0x%08X depth %zu bound3 %08X kind %u field %u path %08X\n",
                         cur_record_, key, (unsigned long long)cell_key(dest), args[1].as_u32(),
                         frames_.size(), frames_.empty() ? 0u : frames_.back().bound[3],
                         cur_kind_, cur_field_, dest);
        ++writes_;
        out = Value{};
        return true;
    }
    if (context_op(key) != nullptr) {
        out = known_u32(kContextHandle);
        return true;
    }
    return false;
}

/* --------------------------------------------------------- PhysicsQueryHost */

void PhysicsQueryHost::add(uint32_t key, const std::string& name) {
    if (!key) return;
    if (name == "__queueAsyncPhysicsRayQueryNode" ||
        name == "__queueAsyncPhysicsRayQueryNodeHasExcludedEntities" ||
        name == "__consumeAsyncPhysicsQueryNode")
        names_[key] = name;
}

/* THE SYNCHRONOUS WHEEL RAYCAST, 0x040F4924.
 *
 * Function_040f4924: PhysicsQueryResult Physics(CString Identifier, Vec3 Start,
 * Vec3 End, PhysicsQueryPreset QueryPreset, ..., List<EcsEntityHandle> Excluded, ...).
 * 26 calls across the 8 suspension graphs, about four wheels each - this, not only the
 * async queue, is the wheel-contact path. Inputs measured by their producers: [1] and
 * [2] are Vec3 (MultiplyFloat3LinearTransformFloat3 / SubtractFloat3), [3] a preset
 * constant, [5] the exclude list from the array constructor, [6] unwritten.
 *
 * The RESULT is PhysicsQueryResult, laid out from the game's own type data
 * (ebx_type_identities, owner 0xB8492288, size 0x50):
 *
 *   0x00 HitPoint Vec3   0x10 HitNormal Vec3   0x20 HitEntity   0x28 (unnamed)
 *   0x30 HitEcsEntity    0x38 HitSourcePart    0x3C Overlap     0x40 LineCoordinate
 *   0x44 HitMaterial
 *
 * Written inline into the output slot so fields read straight out of that range.
 * Entities and material stay zero: nothing offline knows what was hit. */
static const uint32_t kSyncRay = 0x040F4924u;

/* THE SOLDIER'S RAY, __physicsRayQueryNode 0xC087CFCD: (Start, End, preset, flag) ->
 * PhysicsQueryHit. The result TYPE is read from the graphs' own slot declarations (every
 * call's output slot is 0xAEE2957B PhysicsQueryHit, MotionMachineShared, 0x50 bytes), and
 * its CONTENT from the engine function that writes it, FUN_144338970 (called by the
 * single-hit query FUN_14434E360 after FUN_14434EC40 "RayQueryNode"):
 *
 *   hit:  0x00 hit position  0x10 hit normal  0x30 |start - hit|  0x41 HasHit = 1
 *         0x42 IsTerrainHit, 0x2C/0x3C material, 0x28 a component float, 0x43 a material
 *         flag, 0x20 the entity - none of which exist offline: left 0 (OURS, not read).
 *   miss: 0x00 End  0x10 (0, 1, 0, 0)  0x20 00100000 0000FFFF (the no-entity sentinels)
 *         0x30 |start - End|  everything else 0.
 *   0x34 and 0x38 are written 0 by the miss path and never by the hit path, so they read 0.
 * A non-finite Start or End (FUN_1443380A0) leaves the result untouched - refused here. */
static const uint32_t kSoldierRay = 0xC087CFCDu;

bool PhysicsQueryHost::describe(uint32_t key, OperatorSignature& out) {
    out = OperatorSignature{};
    if (key == kSoldierRay) {
        out.input_widths = {16, 16, 4, 4};
        out.output_width = 0x50;
        return true;
    }
    if (key == kSyncRay) {
        /* Six inputs and TWO outputs. The seventh operand (a byte-aligned slot such
         * as 0x32DE) is written, not read: nothing in the graph writes it, and the
         * very next record is a branch on it that skips the HitPoint read when it is
         * false. It is the hit flag. */
        out.input_widths = {4, 16, 16, 4, 4, 4};
        out.extra_output_widths = {1};
        out.output_width = 0x50;
        return true;
    }
    const auto it = names_.find(key);
    if (it == names_.end()) return false;
    const std::string& n = it->second;
    /* Inputs 2 and 3 are MEASURED as Vec3 (16 bytes) from their producers; the rest
     * are declared 4 and that is a guess - see the header. */
    /* THE QUEUE HAS NO SLOT OUTPUT. Measured on every suspension graph's queue
     * record: the operand regions are  r0 r0 r2 r2 r0  - the LAST operand is a
     * constant-pool path, not a slot. Declaring a slot output made the VM take
     * operand 3 (the ray END) as the destination, find three inputs where four were
     * declared, and reject the call - which is what happened on thebeast, the one
     * vehicle whose queue was actually reached. So: five inputs, the last being the
     * STATE path the handle is stored under (WheelRaycastQueryHandle in the
     * descriptor), and nothing written to a slot. */
    if (n == "__queueAsyncPhysicsRayQueryNode") {
        out.input_widths = {4, 4, 16, 16, 4};
        out.output_width = 0;
        return true;
    }
    if (n == "__queueAsyncPhysicsRayQueryNodeHasExcludedEntities") {
        out.input_widths = {4, 4, 16, 16, 4, 4};
        out.output_width = 0;
        return true;
    }
    if (n == "__consumeAsyncPhysicsQueryNode") {
        /* (id, id2) -> ready flag, PhysicsQueryResult. Measured record shape
         * r0 r0 r2 r2: the same two constants the matching queue takes first, then
         * a byte-aligned flag the next branch tests, then the result that the field
         * reader 0x88030F01 reads. The old shape (3 inputs, a Vec3 out) read the
         * flag slot as an input. */
        out.input_widths = {4, 4};
        out.extra_output_widths = {1};
        out.output_width = 0x50;
        return true;
    }
    return false;
}

/* One PhysicsQueryResult (0x50 bytes, offsets from the type's field table) followed
 * by the one-byte hit/ready flag the VM writes to the call's second output. */
static void write_query_result(Value& out, const double from[3], const double to[3],
                               bool got, const double hit[3], const double normal[3],
                               bool flag) {
    const double dx = to[0]-from[0], dy = to[1]-from[1], dz = to[2]-from[2];
    const double len = std::sqrt(dx*dx + dy*dy + dz*dz);
    const double hx = hit[0]-from[0], hy = hit[1]-from[1], hz = hit[2]-from[2];
    const float coord = (float)(got && len > 0 ? std::sqrt(hx*hx+hy*hy+hz*hz) / len : 1.0);
    out.bytes.assign(0x51, 0);
    const float hp[3] = {(float)hit[0], (float)hit[1], (float)hit[2]};
    const float hn[3] = {(float)normal[0], (float)normal[1], (float)normal[2]};
    std::memcpy(out.bytes.data() + 0x00, hp, 12);          /* HitPoint       */
    std::memcpy(out.bytes.data() + 0x10, hn, 12);          /* HitNormal      */
    const uint32_t overlap = got ? 1u : 0u;
    std::memcpy(out.bytes.data() + 0x3C, &overlap, 4);     /* Overlap        */
    std::memcpy(out.bytes.data() + 0x40, &coord, 4);       /* LineCoordinate */
    out.bytes[0x50] = flag ? 1 : 0;                        /* second output  */
    out.known = true;
}

bool PhysicsQueryHost::invoke(uint32_t key, const std::vector<Value>& args,
                              Value& out) {
    if (key == kSoldierRay) {
        if (!trace_ || args.size() != 4) return false;
        ++attempts_;
        auto xyz = [](const Value& v, float o[3]) {
            if (v.bytes.size() < 12) return false;
            if (!v.known) {
                if (v.known_bytes.size() < 12) return false;
                for (int i = 0; i < 12; ++i) if (!v.known_bytes[(size_t)i]) return false;
            }
            std::memcpy(o, v.bytes.data(), 12);
            return true;
        };
        float f[3], t[3];
        if (!xyz(args[0], f) || !xyz(args[1], t)) { ++unknown_ends_; return false; }
        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(f[i]) || !std::isfinite(t[i])) return false;
        const double from[3] = {f[0], f[1], f[2]};
        const double to[3] = {t[0], t[1], t[2]};
        double hit[3] = {to[0], to[1], to[2]}, normal[3] = {0, 1, 0};
        ++queries_;
        const bool got = trace_(user_, from, to, hit, normal) != 0;
        if (got) ++hits_;
        ray_log_.push_back({{f[0], f[1], f[2]}, {t[0], t[1], t[2]}, got, true});
        out = Value{};
        out.bytes.assign(0x50, 0);
        const float hp[4] = {(float)hit[0], (float)hit[1], (float)hit[2], 0.0f};
        const float hn[4] = {(float)normal[0], (float)normal[1], (float)normal[2], 0.0f};
        const float up[4] = {0.0f, 1.0f, 0.0f, 0.0f};
        std::memcpy(out.bytes.data() + 0x00, hp, 16);
        std::memcpy(out.bytes.data() + 0x10, got ? hn : up, 16);
        if (!got) {
            const uint32_t e0 = 0x00100000u, e1 = 0x0000FFFFu;
            std::memcpy(out.bytes.data() + 0x20, &e0, 4);
            std::memcpy(out.bytes.data() + 0x24, &e1, 4);
        }
        const float dx = (float)hit[0] - f[0], dy = (float)hit[1] - f[1], dz = (float)hit[2] - f[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        std::memcpy(out.bytes.data() + 0x30, &dist, 4);
        out.bytes[0x41] = got ? 1 : 0;
        out.known = true;
        return true;
    }
    if (key == kSyncRay) {
        if (!trace_ || args.size() != 6) return false;
        ++attempts_;
        /* Only Start and End are needed to trace; the preset and the exclude list do
         * not change what a ray over our triangles hits. */
        if (!args[1].known || !args[2].known) { ++unknown_ends_; return false; }
        float f[3] = {}, t[3] = {};
        std::memcpy(f, args[1].bytes.data(), 12);
        std::memcpy(t, args[2].bytes.data(), 12);
        const double from[3] = {f[0], f[1], f[2]};
        const double to[3] = {t[0], t[1], t[2]};
        double hit[3] = {to[0], to[1], to[2]}, normal[3] = {0, 1, 0};
        ++queries_;
        const bool got = trace_(user_, from, to, hit, normal) != 0;
        if (got) ++hits_;
        ray_log_.push_back({{f[0], f[1], f[2]}, {t[0], t[1], t[2]}, got, true});
        write_query_result(out, from, to, got, hit, normal, got);
        return true;
    }
    OperatorSignature signature;
    if (!describe(key, signature) ||
        args.size() != signature.input_widths.size())
        return false;
    const std::string& n = names_[key];

    /* A queued query and its consume share their first two constants; that pair
     * is the query's identity. */
    auto id_of = [&](size_t a, size_t b) -> uint64_t {
        return ((uint64_t)args[a].as_u32() << 32) | args[b].as_u32();
    };

    if (n == "__consumeAsyncPhysicsQueryNode") {
        if (!args[0].known || !args[1].known) return false;
        const auto it = pending_.find(id_of(0, 1));
        if (it == pending_.end()) {
            /* Nothing queued yet (the first tick): NOT READY. The result bytes are
             * zero and the graph only reads them behind the flag. */
            const double z[3] = {0, 0, 0};
            write_query_result(out, z, z, false, z, z, false);
            return true;
        }
        const Result r = it->second;
        pending_.erase(it);
        write_query_result(out, r.from, r.to, r.hit, r.point, r.normal, true);
        return true;
    }

    /* queue: read the two Vec3 ends and trace now; the answer is handed back by
     * the consume with the same identity on a LATER tick. */
    if (!trace_) return false;
    const size_t from_i = 2, to_i = 3;
    ++attempts_;
    if (!args[from_i].known || !args[to_i].known) { ++unknown_ends_; return false; }
    float f[3] = {}, t[3] = {};
    if (args[from_i].bytes.size() >= 12) std::memcpy(f, args[from_i].bytes.data(), 12);
    if (args[to_i].bytes.size() >= 12) std::memcpy(t, args[to_i].bytes.data(), 12);
    Result r;
    for (int i = 0; i < 3; ++i) { r.from[i] = f[i]; r.to[i] = t[i]; r.point[i] = t[i]; }
    ++queries_;
    if (trace_(user_, r.from, r.to, r.point, r.normal)) { r.hit = true; ++hits_; }
    ray_log_.push_back({{f[0], f[1], f[2]}, {t[0], t[1], t[2]}, r.hit, false});
    if (args[0].known && args[1].known) pending_[id_of(0, 1)] = r;
    out = Value{};
    return true;
}

bool CompositeHost::describe(uint32_t key, OperatorSignature& out) {
    if (first_ && first_->describe(key, out)) return true;
    if (second_ && second_->describe(key, out)) return true;
    out = OperatorSignature{};
    return false;
}

bool CompositeHost::invoke(uint32_t key, const std::vector<Value>& args,
                           Value& out) {
    OperatorSignature signature;
    if (first_ && first_->describe(key, signature))
        return first_->invoke(key, args, out);
    if (second_ && second_->describe(key, signature))
        return second_->invoke(key, args, out);
    return false;
}

/* ---------------------------------------------------------------- WorldHost */

namespace {
const uint32_t kSetFromSlot   = 0x85A781A7u; /* thunk FUN_147ee94b0 / native 147ee9480 */
const uint32_t kHandleOfFirst = 0x63D604B7u; /* thunk 147E0E790 / native 143EDC990     */
const uint32_t kPartitionA    = 0xB7F6A5BDu; /* thunk 1475E3700 -> 141727020           */
const uint32_t kPartitionB    = 0xF87C766Au; /* thunk 1475E39E0 (sub-filter -1)        */
const uint32_t kEntryState    = 0x85766025u; /* thunk 147592EE0 -> 141564A10           */
/* THREE THE DIRT BIKE NEEDS, decoded from their natives:
 *   0x77E24C80 Battlefield(Subjects, Tags) -> subjects carrying every tag. FUN_1417443C0
 *              clears the result first; this world has no tagged entities: empty.
 *   0x0F063D92 a skeleton constraint (FUN_1443342C0 -> FUN_144335CB0): two bone
 *              references and a 64-byte block, NO result. Served by StateHost (bone_constraint_solve).
 *   0xF743C0B8 current RealmEx == input (FUN_14566BA10). The offline vehicle is the
 *              authoritative, server half: true only for RealmEx_Server 0x98BE5555.
 *              The bike tests Client (0xBF0F9789), which gates a client-only reset. */
const uint32_t kFilterByTags  = 0x77E24C80u;
const uint32_t kBoneConstraint= 0x0F063D92u;
const uint32_t kRealmEquals   = 0xF743C0B8u;
const uint32_t kRealmServer   = 0x98BE5555u;
/* 0x893E29C6 is NOT a*b: the reflected registry names it MotionMachine(Rpm, GearRatio,
 * AverageDriveWheelSpeed, Load) - three inputs, engine load out. Not served here. */
const uint32_t kControllerOf  = 0x9D712CE7u; /* MotionMachine thunk 147EE9DB0 -> 14433B290 */
const uint32_t kControllerFlag= 0x0CB2F866u; /* Battlefield thunk 1475F45F0 -> 1475F4510  */
const uint32_t kGameTick      = 0x9A39505Au; /* node 0x142BCE260: tick, u16 +0x24, +0x26 */
/* THREE MORE THE REFLECTED REGISTRY NAMES OUTRIGHT, so there is nothing to infer.
 *
 *   5DB3C702  (FrameStartTick, FrameEndTick, FrameStartTime, FrameEndTime, DeltaTime)
 *   8F280F3D  (Player, Option, DefaultValue, ReturnValue)
 *
 * The terrain query D7D1BAB3 (Position, TerrainPosition, TerrainHeight) is a ray
 * straight down and lives with the wheel rays, which have the tracer.
 *
 * The frame clock is five outputs this host already knows: it has the time and the
 * step. The player-option lookup has its own answer built in - a lookup that
 * finds no setting returns the DefaultValue it was handed, and offline there is never
 * a setting, so passing the default through is the engine's own miss path rather than
 * a stand-in. */
const uint32_t kFrameClock    = 0x5DB3C702u;
const uint32_t kPlayerOption  = 0x8F280F3Du;
const uint32_t kGameTime      = 0xE2EEC2BEu; /* node 0x142BCE380: (float)double time      */
const uint32_t kAffectorQuery = 0xCA1E499Eu; /* shape-B node -> FUN_141723e60             */
/* 0xA1D70F8E MotionMachine(ToFilter, Filter, ReturnValue): thunk 0x147EEA460, native
 * 0x147EEA320. Filters a set's ids against the current execution context (TLS) under
 * the flag bytes at +9/+10 of the constant Filter struct. Only the case whose RESULT
 * does not depend on that context is served: the native clears the result first and
 * inserts only inside the loop over members, so an EMPTY set filters to an empty set.
 * Slot 2 (0x85A781A7) is the related Vehicle; C22CF89C after it is a
 * PlayerAbilityState lookup (5 = Active, 7 = Invalid on a miss), not a seat state. */
const uint32_t kFilterSet     = 0xA1D70F8Eu;
/* Reflected/engine nodes whose native miss paths are completely defined. */
const uint32_t kShooterStatus = 0x9C8D786Fu; /* 147B6AA20 -> 142CC9510 */
const uint32_t kActorStat     = 0x79F15D30u; /* 147591A10 -> 147591900 */
const uint32_t kClientString  = 0xFE6B9F7Cu; /* 1474ACB80 -> 1474ACAB0 */
const uint32_t kWeaponState   = 0x75BF546Fu; /* 147B6ADA0 -> 142CC9850 */
const uint32_t kGameplayFlags = 0x39497415u; /* engine node 14172B810 */
/* Native-backed offline fallback for the reflected Player query. */
const uint32_t kPlayerTeam    = 0x2948B3E1u; /* native 1475F4920       */
/* A setting/rule bool on the soldier, by descriptor: 626 uses, 43 soldier graphs. */
const uint32_t kSettingBool   = 0x63E71248u; /* native 144328E80       */
/* The soldier's float / int / vec4 field readers by descriptor (FC_N1): not served yet -
 * which field an id names is being established - only logged under BF6_LOG_FIELD_IDS. */
const uint32_t kFieldFloat    = 0xF4311C3Du; /* native 144328FC0       */
const uint32_t kFieldInt      = 0x9FC488C4u; /* native 144328F40       */
const uint32_t kFieldVec      = 0x32E399EBu; /* native 144329040       */
/* Their write twins (read from the natives): (descriptor, value) into the same tables. */
const uint32_t kStoreBool     = 0x16D34E05u; /* native 144329670 -> FUN_14436B9E0 */
const uint32_t kStoreFloat    = 0x371C27B1u; /* native 1443297B0 -> FUN_14436A080 */
const uint32_t kStoreInt      = 0x02A56C25u; /* native 144329700, presence bit + value */
/* A transform field, built from a stored rotation + position (native 1443290E0). */
const uint32_t kFieldXform    = 0xD927CB31u;
/* CLAMPED stores, natives from expression_engine_nodes_6a1c1b.tsv (the EngineNodes.tsv
 * addresses for these keys land mid-function): (key, descriptor, min, max, -, value)
 * stores min(max(value, min), max) into the int / float field. Operand 4 is never read. */
const uint32_t kClampStoreInt   = 0xB7428609u; /* native 144329F30 */
const uint32_t kClampStoreFloat = 0x239DC415u; /* native 14432A010 -> FUN_14436A080 */
/* THE "SETTINGS LOOKUP BY PATH" (dotted names such as VO.Enable in operand 0). Its native,
 * 0x143B6A450 from expression_engine_nodes_6a1c1b.tsv, is three instructions:
 * `movzx eax, byte [rdx]; mov [r8], al; ret` - the output IS operand 1, and the name in
 * operand 0 is never read. Pure; known exactly when operand 1 is. */
const uint32_t kSettingByPath   = 0xBC999B66u;
/* "IS THIS INT FIELD EQUAL TO N", the field named by its link HASH, not a descriptor.
 * Native 0x14432B240 (expression_engine_nodes_6a1c1b.tsv): FUN_1442E0F00 finds the hash in
 * the machine's hash list, FUN_14436B890 reads that field - the stored value if its
 * presence bit is set, else the authored default at +0x54 - and the output is value == N;
 * a hash not in the list gives false. Operands (hash, hash, N): operand 0 repeats the
 * hash (measured on every call), operand 1 is the one the native reads. */
const uint32_t kIntFieldIs      = 0x9132CD71u;
/* IS THIS THE LOCAL PLAYER'S SOLDIER (reflected MotionMachine, bool CheckForSpectator ->
 * bool; thunk 0x147EE5760, native 0x14432B690). On a client, flag 0 returns the player's
 * is-local byte (+0x1D8) and flag 1 whether the soldier is the local player's controlled,
 * or spectated, one; a server context returns 0. MODELLED, not read: offline the runner
 * IS the local client's own soldier - the first-person walker - so both flags answer
 * true. BF6_REMOTE_SOLDIER=1 models a remote soldier instead (false). */
const uint32_t kIsLocalSoldier  = 0x56A77320u;
/* AngleAroundYVec (Vec3) -> (angle, valid). Its body, 0x14249A0C0 (found through the
 * name's crc32 initializer): x^2 + z^2 <= 0 gives angle 0 and valid false; otherwise the
 * vector is scaled by a Newton-refined rsqrt of its full length^2 and the angle is
 * atan2(z, x) of the scaled lanes, wrapped to [-pi, pi] by the fmodf pair. The valid flag
 * is the last native output, so it is the VM primary and the angle the extra output. */
const uint32_t kAngleAroundY    = 0x6D534FD9u;
/* VEC FIELD STORES WITH A SPACE MODE. 0x47D7C5A6 (native 144329850) names the field by
 * descriptor, 0x62FE6638 (native 144329BA0) by link hash; operands (key or hash,
 * descriptor or hash, mode, Vec3). Mode 0 is the plain store (FUN_14436A850); any other
 * mode goes through FUN_14436AF10, which zeroes a non-finite vector and, in mode 2,
 * converts through the field's reference space - and when the field names none (its
 * reference descriptors all 0xFFFF) falls through to the same plain store. Measured
 * modes: 0 and 2 only. A field with a reference space, or another mode, is refused. */
const uint32_t kVecStoreDesc    = 0x47D7C5A6u;
const uint32_t kVecStoreHash    = 0x62FE6638u;
/* THE DEBUG DRAWS. Reflected functions that return void and take only what to draw -
 * Position/Start/End/Transform, Color32, Wireframe, DepthTest, TimeVisible - per
 * ReflectedFunctions.tsv (DrawSphere, DrawText, DrawArrow, DrawBox, DrawLine, ...). They
 * write nothing a graph can read back, so offline they draw nothing and produce nothing.
 * Graphs pass a varying number of their parameters (observed arity 0..9), so each call
 * consumes whatever operands it has. */
static bool is_debug_draw(uint32_t key) {
    switch (key) {
    case 0x7B549E87u: case 0x21A2D460u: case 0x4F5E0255u: case 0xF086DE07u:
    case 0xEAADFDD0u: case 0xCDF6B1BDu: case 0x290A3467u: case 0x96FE5D21u:
    case 0xF7B11F4Eu: case 0xC7C9F7F4u: case 0xA73FDA72u: case 0x1CFF86BEu:
        return true;
    default: return false;
    }
}
/* 0xEA5D1359 Aiming(EntryTagId -> Yaw, Pitch, Roll, ZoomLevel): thunk 0x14736B4A0 ->
 * FUN_1405794A0, which ZEROES all four outputs first and fills them only when an
 * entry in the vehicle's entry list carries the tag (vtable +0x250 gives the trio,
 * +0x258 the zoom; a second mode uses +0x270/+0x278). Nobody aiming offline is that
 * miss path: all zero. Outputs named by reflection descriptor 0x14AF1E1E8. */
const uint32_t kAiming        = 0xEA5D1359u;
/* 0x02C66B00 Battlefield(TargetList, Affector -> LastGiver, LastGiverPlayerId, IsActive,
 * Rank, Duration, EscalationLevel): thunk -> FUN_141727850, which first sets Rank,
 * Duration, EscalationLevel = 0, LastGiverPlayerId = 0xFFFFFFFF, IsActive = 0 and clears
 * the LastGiver set (FUN_147f13ab0), then overwrites them only for a target entity that
 * carries the affector. No entity here carries one. The f16 guessed IsActive TRUE, which
 * zeroes the throttle channel below 100 (records @3304..@3536). */
const uint32_t kAffectorPick  = 0x02C66B00u;
/* Batch G0. These are kept here (rather than in the name-driven pure host) because
 * all but the easing helper are engine/reflection services with explicit offline
 * miss paths.
 *
 * 0xB19CEDF0, native 0x140BA5CB0: MOV EAX,[RCX]; MOV [RDX],EAX.
 * 0xAACB562A, native 0x1417315C0: clears four u32 outputs before any context lookup.
 * 0x19A7BA16, native 0x146BA4330 -> 0x143AB21E0: PropertyInterpolationMode
 * wrapper. Only the two recovered base curves (Linear and Quad) are admitted.
 * 0xC0C3BE9F, native 0x14759ECF0 -> 0x14133D4A0: the null Option path returns
 * the third argument (DefaultValue) unchanged.
 * The four set/query helpers clear their destination before walking live handles;
 * consequently an empty/null input has the exact empty result offline. Non-empty
 * inputs are refused because this host has no entity/handle registry to walk. */
const uint32_t kIdentityI32   = 0xB19CEDF0u;
const uint32_t kContextStats  = 0xAACB562Au;
const uint32_t kSubHandle     = 0x09635061u;
const uint32_t kEntityEntry   = 0x91C21F3Cu;
const uint32_t kEaseProperty  = 0x19A7BA16u;
const uint32_t kPlayerDefault = 0xC0C3BE9Fu;
const uint32_t kEntityWalk    = 0xFC69D4E9u;
const uint32_t kSliceHandles  = 0xE9560B5Eu;
/* Batch-1 state/world nodes.  Each fallback below is the native's initialized miss
 * path, not a guessed gameplay value. */
const uint32_t kAimingPlayer  = 0x368D3BC1u; /* FUN_14736B5D0: ptr -> u32 x4 */
const uint32_t kSetMerge      = 0xA46470C2u; /* FUN_144341D40: set,set -> set */
const uint32_t kContextPair   = 0x1833369Eu; /* FUN_141730D50: -> bool,u32 */
const uint32_t kAbilityState  = 0xC22CF89Cu; /* FUN_14174E130: ids -> state,value */
const uint32_t kEntityFlags   = 0xE029A05Du; /* FUN_14172B9B0: id -> ten values */
const uint32_t kRelated       = 0x76E6401Cu; /* FUN_141737FC0: set -> set */
const uint32_t kApplyAffector = 0x329770B8u; /* FUN_141726A10: query -> bool */
const uint32_t kSetBytes      = 260u;        /* u32 count + 64 x u32                   */
const uint32_t kNoId          = 0x000FFFFFu; /* DAT_149b71b48, read from the exe .data */
/* 0x09D1F6BA, reflected MotionMachine () -> GameQueryResult: native 0x147EE9340 is
 * `FUN_147f13ab0(out); return out;` - the empty set, always. */
const uint32_t kEmptyQuery    = 0x09D1F6BAu;
/* 0xD95D4A17, reflected MotionMachine (ToFilter, FilterData, Transform) -> GameQueryResult:
 * native 0x147EEAFA0 -> FUN_14433BD10 clears the output, then loops over ToFilter's count
 * keeping the ids whose measure lies in [min, max]. With a count of 0 the loop never runs:
 * empty in, empty out. A non-empty input needs the entities' live positions - refused. */
const uint32_t kFilterByTransform = 0xD95D4A17u;

Value empty_set() {
    Value v;
    v.bytes.assign(kSetBytes, 0);
    for (uint32_t i = 0; i < 64; ++i) std::memcpy(v.bytes.data() + 4 + i * 4, &kNoId, 4);
    v.known = true;
    return v;
}
}

/* __GetTweakableFloat / __GetTweakableBool / __GetTweakableInt
 * (CRC-32/BZIP2 of the node names). */
const uint32_t kTweakFloat = 0x10E7EF91u;
const uint32_t kTweakBool  = 0xC6485A8Bu;
const uint32_t kTweakInt   = 0x94A8B80Bu;
const uint32_t kWaterHeight = 0xED79777Au;

bool WorldHost::describe(uint32_t key, OperatorSignature& out) {
    out = OperatorSignature{};
    /* no-operand default; describe_call sizes each call to its own operands */
    if (is_debug_draw(key)) { out.output_width = 0; return true; }
    switch (key) {
    case kTweakFloat: out.input_widths = {4}; out.output_width = 4; return true;
    case kTweakBool:  out.input_widths = {4}; out.output_width = 1; return true;
    case kTweakInt:   out.input_widths = {4}; out.output_width = 4; return true;
    case kWaterHeight: out.input_widths = {16}; out.output_width = 4; return true;
    case kIdentityI32:
        out.input_widths = {4}; out.output_width = 4; return true;
    case kContextStats:
        out.input_widths = {};
        out.extra_output_widths = {4, 4, 4};
        out.output_width = 4;
        return true;
    case kSubHandle:
        out.input_widths = {8, 4}; out.output_width = kSetBytes; return true;
    case kEntityEntry:   /* served by StateHost, which has the seats */
        return false;
    case kEaseProperty:
        out.input_widths = {4, 4, 4}; out.output_width = 4; return true;
    case kPlayerDefault:
        out.input_widths = {4, 8, 4}; out.output_width = 4; return true;
    case kEntityWalk:
        out.input_widths = {kSetBytes, 4, 4}; out.output_width = kSetBytes; return true;
    case kSliceHandles:
        out.input_widths = {kSetBytes, 4, 4}; out.output_width = kSetBytes; return true;
    case kSetFromSlot:   out.input_widths = {4};          out.output_width = kSetBytes; return true;
    case kHandleOfFirst: out.input_widths = {kSetBytes};  out.output_width = 8;         return true;
    case kPartitionA:
    case kPartitionB:
        /* (set, filter list, bool) -> (matched set, matched count, unmatched set,
         * unmatched count); the primary is the LAST output. */
        out.input_widths = {kSetBytes, 4, 1};
        out.extra_output_widths = {kSetBytes, 4, kSetBytes};
        out.output_width = 4;
        return true;
    case kControllerOf:
        /* (entity set) -> (handle 8, found byte); primary = found (last) */
        out.input_widths = {kSetBytes};
        out.extra_output_widths = {8};
        out.output_width = 1;
        return true;
    case kControllerFlag:
        /* (handle) -> byte at +0xBC of the resolved object (server path) */
        out.input_widths = {8};
        out.output_width = 1;
        return true;
    case kFrameClock:
        /* No inputs; five outputs, the last being DeltaTime. */
        out.input_widths = {};
        out.extra_output_widths = {4, 4, 4, 4};
        out.output_width = 4;
        return true;
    case kPlayerOption:
        /* player, option id, default -> the setting, which offline is the default */
        out.input_widths = {4, 4, 4};
        out.output_width = 4;
        return true;
    case kGameTick:
        /* Three operands, all outputs (rcx, rdx, r8 in record order): the timing
         * component's u32 at +0x18 and u16s at +0x24 / +0x26. The primary is the
         * last. */
        out.input_widths = {};
        out.extra_output_widths = {4, 4};
        out.output_width = 4;
        return true;
    case kGameTime:
        /* One operand, the output: FUN at 0x142BCE380 finds a component through the
         * update context and writes (float) of the double at its +0x10. */
        out.input_widths = {};
        out.output_width = 4;
        return true;
    case kAffectorQuery:
        /* (affector asset ptr, entity set) -> (matched set, state u32, found byte,
         * affector +0x1c, affector +0x14); the primary is the last output. */
        out.input_widths = {8, kSetBytes};
        out.extra_output_widths = {kSetBytes, 4, 1, 4};
        out.output_width = 4;
        return true;
    case kFilterByTags:
        out.input_widths = {kSetBytes, 8};
        out.output_width = kSetBytes;
        return true;
    case kBoneConstraint:   /* served by StateHost, which has the skeleton */
        return false;
    case kRealmEquals:
        out.input_widths = {4};
        out.output_width = 1;
        return true;
    case kAffectorPick:
        /* (target set, affector ptr) -> (giver set, giver id, active, rank, duration,
         * escalation); the primary is the last */
        out.input_widths = {kSetBytes, 8};
        out.extra_output_widths = {kSetBytes, 4, 1, 4, 4};
        out.output_width = 4;
        return true;
    case kAiming:
        /* (entry tag) -> (yaw, pitch, roll, zoom); the primary is the last */
        out.input_widths = {4};
        out.extra_output_widths = {4, 4, 4};
        out.output_width = 4;
        return true;
    case kFilterSet:
        /* (set, constant filter struct) -> set */
        out.input_widths = {kSetBytes, 12};
        out.output_width = kSetBytes;
        return true;
    case kAimingPlayer:
        out.input_widths = {8};
        out.extra_output_widths = {4, 4, 4};
        out.output_width = 4;
        return true;
    case kSetMerge:
        out.input_widths = {kSetBytes, kSetBytes};
        out.output_width = kSetBytes;
        return true;
    case kContextPair:
        out.input_widths = {};
        out.extra_output_widths = {1};
        out.output_width = 4;
        return true;
    case kAbilityState:
        out.input_widths = {4, 4};
        out.extra_output_widths = {4};
        out.output_width = 4;
        return true;
    case kEntityFlags:
        out.input_widths = {4};
        out.extra_output_widths = {1, 1, 1, 1, 1, 4, 1, 1, 1};
        out.output_width = 1;
        return true;
    case kRelated:
        out.input_widths = {kSetBytes};
        out.output_width = kSetBytes;
        return true;
    case kApplyAffector:
        out.input_widths = {kSetBytes, kSetBytes, 8, 4, 1, 4, 64, 8};
        out.output_width = 1;
        return true;
    case kPlayerTeam:
        /* Battlefield(Player PointerRef) -> TeamId. */
        out.input_widths = {8};
        out.output_width = 4;
        return true;
    case kSettingBool:
        /* (descriptor, second constant) -> bool; engine arity 2, atlas shape k,k. */
        out.input_widths = {4, 4};
        out.output_width = 1;
        return true;
    case kStoreBool:
    case kStoreFloat:
    case kStoreInt:
        /* The field STORES, write twins of the readers. MEASURED under BF6_LOG_FIELD_IDS
         * across the 118 soldier graphs, all 93 calls: (0xF263DF78, descriptor, value) -
         * the same collection key and the same (u16 id, flags, lane) descriptor the
         * readers take, then a 1-byte bool or a 4-byte float/int. No output. */
        if (!bf6::SoldierFields::get().loaded() && !std::getenv("BF6_LOG_FIELD_IDS")) return false;
        out.input_widths = {4, 4, key == kStoreBool ? 1u : 4u};
        out.output_width = 0;
        return true;
    case kSettingByPath:
        /* (CString name - never read, bool) -> bool */
        out.input_widths = {8, 1};
        out.output_width = 1;
        return true;
    case kIsLocalSoldier:
        out.input_widths = {1};
        out.output_width = 1;
        return true;
    case kEmptyQuery:
        out.input_widths = {};
        out.output_width = kSetBytes;
        return true;
    case kFilterByTransform:
        /* Three operands here; describe_call sizes the call to its own operand count. */
        out.input_widths = {kSetBytes, 4, 64};
        out.output_width = kSetBytes;
        return true;
    case kVecStoreDesc:
    case kVecStoreHash:
        if (!bf6::SoldierFields::get().loaded()) return false;
        out.input_widths = {4, 4, 4, 16};
        out.output_width = 0;
        return true;
    case kAngleAroundY:
        out.input_widths = {16};
        out.extra_output_widths = {4};
        out.output_width = 1;
        return true;
    case kIntFieldIs:
        if (!bf6::SoldierFields::get().loaded()) return false;
        out.input_widths = {4, 4, 4};
        out.output_width = 1;
        return true;
    case kClampStoreInt:
    case kClampStoreFloat:
        /* Six operands here; describe_call also admits the five-operand form. */
        if (!bf6::SoldierFields::get().loaded() && !std::getenv("BF6_LOG_FIELD_IDS")) return false;
        out.input_widths = {4, 4, 4, 4, 4, 4};
        out.output_width = 0;
        return true;
    case kFieldXform:
        /* (0xF263DF78, descriptor, space mode) -> LinearTransform. Native 1443290E0 via
         * FUN_141320DA0; all 29 calls measured with this shape. */
        if (!bf6::SoldierFields::get().loaded() && !std::getenv("BF6_LOG_FIELD_IDS")) return false;
        out.input_widths = {4, 4, 4};
        out.output_width = 64;
        return true;
    case kFieldFloat:
    case kFieldInt:
    case kFieldVec:
        /* Served from the soldier field table (soldier_fields.h) once it is loaded;
         * without it these are not described at all, as before. BF6_LOG_FIELD_IDS
         * describes them anyway so the descriptors can be logged. */
        if (!bf6::SoldierFields::get().loaded() && !std::getenv("BF6_LOG_FIELD_IDS")) return false;
        out.input_widths = key == kFieldVec ? std::vector<uint32_t>{4, 4, 4} : std::vector<uint32_t>{4, 4};
        out.output_width = key == kFieldVec ? 16u : 4u;
        return true;
    case kShooterStatus:
        /* Native outputs status first, secondary state last (the VM primary). */
        out.input_widths = {};
        out.extra_output_widths = {4};
        out.output_width = 4;
        return true;
    case kActorStat:
        /* GameQueryResult, UseCurrentContext -> valid, value (primary last). */
        out.input_widths = {4, 1};
        out.extra_output_widths = {1};
        out.output_width = 4;
        return true;
    case kClientString:
        /* LocalPlayerId, PointerRef, CString default -> CString. */
        out.input_widths = {4, 8, 8};
        out.output_width = 8;
        return true;
    case kWeaponState:
        /* Sixteen native outputs; the final float is the VM primary. */
        out.input_widths = {};
        out.extra_output_widths = {4, 4, 4, 1, 1, 1, 1, 1, 1,
                                   4, 4, 4, 4, 4, 4};
        out.output_width = 4;
        return true;
    case kGameplayFlags:
        /* One entity/id input; ten outputs, final bool is the VM primary. */
        out.input_widths = {4};
        out.extra_output_widths = {1, 1, 1, 1, 1, 4, 1, 1, 1};
        out.output_width = 1;
        return true;
    case kEntryState:
        /* (entity/slot, bool gate) -> (state int, ==0, ==1, ==2, ==3) */
        out.input_widths = {4, 1};
        out.extra_output_widths = {4, 1, 1, 1};
        out.output_width = 1;
        return true;
    default: return false;
    }
}

bool WorldHost::describe_call(uint32_t key, const std::vector<uint32_t>& consts,
                              OperatorSignature& out) {
    if (is_debug_draw(key)) {
        out = OperatorSignature{};
        out.input_widths.assign(consts.size(), 4u);
        out.output_width = 0;
        return true;
    }
    /* Only the first input (the set) is read; the filter struct's width is not in data,
     * so the rest are consumed at 4 bytes whatever their count. */
    if (key == kFilterByTransform && !consts.empty()) {
        out = OperatorSignature{};
        out.input_widths.assign(consts.size(), 4u);
        out.input_widths[0] = kSetBytes;
        out.output_width = kSetBytes;
        return true;
    }
    if ((key == kClampStoreInt || key == kClampStoreFloat) &&
        (consts.size() == 5 || consts.size() == 6)) {
        if (!describe(key, out)) return false;
        out.input_widths.assign(consts.size(), 4u);
        return true;
    }
    return describe(key, out);
}

bool WorldHost::invoke(uint32_t key, const std::vector<Value>& args, Value& out) {
    if (key == kEmptyQuery) {
        served_[key] += 1;
        out = empty_set();
        return true;
    }
    if (key == kFilterByTransform) {
        if (args.empty() || !args[0].known || args[0].bytes.size() < 4) return false;
        uint32_t count = 0;
        std::memcpy(&count, args[0].bytes.data(), 4);
        if (count != 0) return false;
        served_[key] += 1;
        out = empty_set();
        return true;
    }
    if (is_debug_draw(key)) {
        served_[key] += 1;
        out = Value{};
        return true;
    }
    if (key == kClampStoreInt || key == kClampStoreFloat) {
        if (std::getenv("BF6_LOG_FIELD_IDS")) {
            std::string hex;
            for (size_t a = 0; a < args.size(); ++a) {
                hex += a ? " | " : "";
                for (size_t b = 0; b < args[a].bytes.size() && b < 16; ++b) {
                    char t[4]; std::snprintf(t, sizeof t, "%02x", args[a].bytes[b]); hex += t;
                }
                hex += args[a].known ? "" : "?";
            }
            std::fprintf(stderr, "fieldop %08X n %zu raw %s\n", key, args.size(), hex.c_str());
        }
        /* MEASURED, all 34 calls the soldier graphs reach: (0xF263DF78, descriptor, min,
         * max, unread, value), e.g. min -15.0 max 2.0. The five-operand form the atlas
         * also lists has not been seen reaching a call, so it is refused, not guessed. */
        bf6::SoldierFields& sf = bf6::SoldierFields::get();
        if (!sf.loaded() || args.size() != 6 || !args[1].known || args[1].bytes.size() < 4)
            return false;
        uint16_t id = 0;
        std::memcpy(&id, args[1].bytes.data(), 2);
        out = Value{};
        served_[key] += 1;
        if (id == 0xFFFFu) return true;
        const bf6::SoldierFields::Field* f = sf.find(
            key == kClampStoreInt ? bf6::SoldierFields::kInt : bf6::SoldierFields::kFloat,
            args[1].bytes[3], id);
        if (!f) { served_[key] -= 1; return false; }
        const Value& lo = args[2]; const Value& hi = args[3]; const Value& val = args[5];
        if (!lo.known || !hi.known || !val.known || lo.bytes.size() < 4 || hi.bytes.size() < 4 ||
            val.bytes.size() < 4) { sf.store_unknown(*f); return true; }
        float v = 0.0f;
        if (key == kClampStoreInt) {
            int32_t a, l, h;
            std::memcpy(&a, val.bytes.data(), 4); std::memcpy(&l, lo.bytes.data(), 4);
            std::memcpy(&h, hi.bytes.data(), 4);
            /* the native's order: raise to min first, then cap at max */
            if (a < l) a = l;
            if (h < a) a = h;
            v = (float)a;
        } else {
            float a, l, h;
            std::memcpy(&a, val.bytes.data(), 4); std::memcpy(&l, lo.bytes.data(), 4);
            std::memcpy(&h, hi.bytes.data(), 4);
            if (!(l > a)) l = a;   /* fVar5 = min; if (min <= value) fVar5 = value */
            if (!(h > l)) l = h;   /* if (max <= fVar5) fVar5 = max */
            v = l;
        }
        sf.store(*f, &v, 1);
        return true;
    }
    OperatorSignature sig;
    if (!describe(key, sig) || args.size() != sig.input_widths.size()) return false;
    if (key == kSettingByPath) {
        if (!args[1].known || args[1].bytes.empty()) return false;
        served_[key] += 1;
        out = Value::from_bool(args[1].bytes[0] != 0);
        return true;
    }
    if (key == kVecStoreDesc || key == kVecStoreHash) {
        if (!args[1].known || !args[2].known || args[1].bytes.size() < 4 || args[2].bytes.size() < 4)
            return false;
        bf6::SoldierFields& sf = bf6::SoldierFields::get();
        const bf6::SoldierFields::Field* f = nullptr;
        if (key == kVecStoreDesc) {
            uint16_t id = 0;
            std::memcpy(&id, args[1].bytes.data(), 2);
            if (id == 0xFFFFu) { served_[key] += 1; out = Value{}; return true; }
            f = sf.find(bf6::SoldierFields::kVec, args[1].bytes[3], id);
        } else {
            uint32_t h = 0;
            std::memcpy(&h, args[1].bytes.data(), 4);
            f = sf.by_hash(h);
        }
        int32_t mode = 0;
        std::memcpy(&mode, args[2].bytes.data(), 4);
        if (!f || f->kind != bf6::SoldierFields::kVec) return false;
        if (mode != 0 && (mode != 2 || f->space_ref)) return false;
        served_[key] += 1;
        out = Value{};
        /* x, y and z known is enough: the pad lane of a Vec3 assembled lane by lane is
         * never written (the same rule PureOps applies to its Float3 operators). */
        bool xyz = args[3].known && args[3].bytes.size() >= 12;
        if (!xyz && args[3].bytes.size() >= 12 && args[3].known_bytes.size() >= 12) {
            xyz = true;
            for (int i = 0; i < 12; ++i) xyz = xyz && args[3].known_bytes[(size_t)i] != 0;
        }
        if (!xyz) { sf.store_unknown(*f); return true; }
        float v[4] = {0, 0, 0, 0};
        std::memcpy(v, args[3].bytes.data(), 12);
        if (mode != 0 && (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])))
            v[0] = v[1] = v[2] = v[3] = 0.0f;
        sf.store(*f, v, 4);
        return true;
    }
    if (key == kAngleAroundY) {
        if (!args[0].known || args[0].bytes.size() < 12) return false;
        float v[3];
        std::memcpy(v, args[0].bytes.data(), 12);
        float angle = 0.0f;
        bool valid = false;
        if (v[0] * v[0] + v[2] * v[2] > 0.0f) {
            const float s = 1.0f / std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            angle = std::atan2(v[2] * s, v[0] * s);
            angle = angle < 0.0f ? std::fmod(angle - 3.1415927f, 6.2831855f) + 3.1415927f
                                 : std::fmod(angle + 3.1415927f, 6.2831855f) - 3.1415927f;
            valid = true;
        }
        served_[key] += 1;
        out = Value{};
        out.bytes.resize(5);
        out.bytes[0] = valid ? 1 : 0;
        std::memcpy(out.bytes.data() + 1, &angle, 4);
        out.known = true;
        return true;
    }
    if (key == kIsLocalSoldier) {
        served_[key] += 1;
        out = Value::from_bool(std::getenv("BF6_REMOTE_SOLDIER") == nullptr);
        return true;
    }
    if (key == kIntFieldIs) {
        if (!args[1].known || !args[2].known || args[1].bytes.size() < 4 || args[2].bytes.size() < 4)
            return false;
        uint32_t h = 0; int32_t n = 0;
        std::memcpy(&h, args[1].bytes.data(), 4);
        std::memcpy(&n, args[2].bytes.data(), 4);
        const bf6::SoldierFields& sf = bf6::SoldierFields::get();
        const bf6::SoldierFields::Field* f = sf.by_hash(h);
        /* The native reads false for a hash not in its list, but a hash missing from THIS
         * table may only mean a link that did not load - refused, not answered false. */
        if (!f) return false;
        sf.mark_read(*f);
        if (f->kind != bf6::SoldierFields::kInt || !sf.known(*f)) return false;
        float v[4];
        sf.value(*f, v);
        served_[key] += 1;
        out = Value::from_bool((int32_t)v[0] == n);
        return true;
    }
    /* FUN_1443EFC30 maps its missing-table sentinel (-FLT_MAX) to -1024.0f.
     * Offline there is no simulation table, so the result does not depend on
     * whether the position operand itself is known. */
    if (key == kPlayerTeam) {
        /* 0x1475F4920 returns zero if either link in the Player PointerRef chain is
         * null; only the live-object path reads TeamId at object +0x60. */
        served_[key] += 1;
        out = Value::from_u32(0);
        return true;
    }
    if (key == kStoreBool || key == kStoreFloat || key == kStoreInt || key == kFieldXform) {
        if (std::getenv("BF6_LOG_FIELD_IDS")) {
            std::string hex;
            for (size_t a = 0; a < args.size(); ++a) {
                hex += a ? " | " : "";
                for (size_t b = 0; b < args[a].bytes.size() && b < 16; ++b) {
                    char t[4]; std::snprintf(t, sizeof t, "%02x", args[a].bytes[b]); hex += t;
                }
                hex += args[a].known ? "" : "?";
            }
            std::fprintf(stderr, "fieldop %08X n %zu raw %s\n", key, args.size(), hex.c_str());
        }
        bf6::SoldierFields& sf = bf6::SoldierFields::get();
        if (!sf.loaded() || args.size() != 3 || !args[1].known || args[1].bytes.size() < 4)
            return false;
        uint16_t id = 0;
        std::memcpy(&id, args[1].bytes.data(), 2);
        const uint8_t flags = args[1].bytes[2];
        const int lane = args[1].bytes[3];
        if (key == kFieldXform) {
            /* FUN_141320DA0. Space mode 0, or mode 2 with (flags & 6) == 0, reads the
             * stored (position, rotation) raw; a field never written - or id 0xFFFF -
             * reads position 0 and the identity rotation, which built into a transform is
             * identity. Every other mode goes through a space conversion
             * (FUN_144369A20) against the live soldier, which does not exist offline:
             * refused, not faked. */
            if (!args[2].known || args[2].bytes.size() < 4) return false;
            uint32_t mode = 0;
            std::memcpy(&mode, args[2].bytes.data(), 4);
            if (!(mode == 0 || (mode == 2 && (flags & 6) == 0))) return false;
            float m[16];
            if (id == 0xFFFFu) {
                lt_identity(m);
                m[15] = 1.0f;
            } else {
                const bf6::SoldierFields::Field* f = sf.find(bf6::SoldierFields::kXform, lane, id);
                if (f) sf.mark_read(*f);
                if (!f || !sf.known(*f)) return false;
                sf.xform(*f, m);
            }
            out = Value{};
            out.bytes.resize(64);
            std::memcpy(out.bytes.data(), m, 64);
            out.known = true;
            served_[key] += 1;
            return true;
        }
        /* A STORE. id 0xFFFF names nothing and the native skips it. */
        out = Value{};
        served_[key] += 1;
        if (id == 0xFFFFu) return true;
        const int kind = key == kStoreBool ? bf6::SoldierFields::kBool
                       : key == kStoreFloat ? bf6::SoldierFields::kFloat : bf6::SoldierFields::kInt;
        const bf6::SoldierFields::Field* f = sf.find(kind, lane, id);
        if (!f) { served_[key] -= 1; return false; }
        if (!args[2].known || args[2].bytes.empty()) { sf.store_unknown(*f); return true; }
        float v = 0.0f;
        if (key == kStoreBool) v = args[2].bytes[0] ? 1.0f : 0.0f;
        else if (args[2].bytes.size() >= 4) {
            if (key == kStoreFloat) std::memcpy(&v, args[2].bytes.data(), 4);
            else { int32_t i = 0; std::memcpy(&i, args[2].bytes.data(), 4); v = (float)i; }
        } else { sf.store_unknown(*f); return true; }
        sf.store(*f, &v, 1);
        return true;
    }
    if (key == kFieldFloat || key == kFieldInt || key == kFieldVec ||
        (key == kSettingBool && std::getenv("BF6_LOG_FIELD_IDS"))) {
        /* BF6_LOG_FIELD_IDS: which soldier field each descriptor names, as (reader, id,
         * lane) - to test whether the u16 id is the field's index in its type's list in
         * common/gameplay/soldier/soldiermotionmachine. */
        if (std::getenv("BF6_LOG_FIELD_IDS") && args.size() >= 2 && args[1].known &&
            args[1].bytes.size() >= 4) {
            /* Operand 1 is the descriptor (u16 id, flags byte, lane byte). */
            uint16_t id = 0; uint8_t lane = args[1].bytes[3];
            std::memcpy(&id, args[1].bytes.data(), 2);
            std::string hex;
            for (size_t a = 0; a < args.size(); ++a) {
                hex += a ? " | " : "";
                for (size_t b = 0; b < args[a].bytes.size() && b < 16; ++b) {
                    char t[4]; std::snprintf(t, sizeof t, "%02x", args[a].bytes[b]); hex += t;
                }
                hex += args[a].known ? "" : "?";
            }
            std::fprintf(stderr, "fieldid %08X id %u lane %u flags %02X raw %s\n", key, id,
                         lane, args[1].bytes[2], hex.c_str());
        }
        if (key != kSettingBool && !bf6::SoldierFields::get().loaded()) return false;
    }
    /* THE SOLDIER'S NAMED FIELDS. Operand 1 is the descriptor: u16 id, flags byte, lane
     * byte. The field table names it; its value is the walker's live value for that name,
     * else the authored default. A descriptor that names no field is refused. */
    if ((key == kFieldFloat || key == kFieldInt || key == kFieldVec) && bf6::SoldierFields::get().loaded()) {
        if (args.size() < 2 || !args[1].known || args[1].bytes.size() < 4) return false;
        uint16_t id = 0;
        std::memcpy(&id, args[1].bytes.data(), 2);
        const int lane = args[1].bytes[3];
        if (id == 0xFFFFu) {
            /* the native's own miss: 0 / 0.0 / (0,0,0,0) */
            out = key == kFieldVec ? vec16_value(std::vector<uint8_t>(16, 0)) : Value::from_u32(0);
            served_[key] += 1;
            return true;
        }
        const int kind = key == kFieldFloat ? bf6::SoldierFields::kFloat
                       : key == kFieldInt ? bf6::SoldierFields::kInt : bf6::SoldierFields::kVec;
        const bf6::SoldierFields::Field* f = bf6::SoldierFields::get().find(kind, lane, id);
        if (f) bf6::SoldierFields::get().mark_read(*f);
        if (!f || !bf6::SoldierFields::get().known(*f)) return false;
        float v[4];
        bf6::SoldierFields::get().value(*f, v);
        if (key == kFieldInt) out = Value::from_u32((uint32_t)(int32_t)v[0]);
        else if (key == kFieldFloat) {
            uint32_t raw = 0;
            std::memcpy(&raw, &v[0], 4);
            out = Value::from_u32(raw);
        } else {
            std::vector<uint8_t> b(16, 0);
            std::memcpy(b.data(), v, 16);
            out = vec16_value(b);
        }
        served_[key] += 1;
        return true;
    }
    if (key == kSettingBool && bf6::SoldierFields::get().loaded() && args.size() >= 2 &&
        args[1].known && args[1].bytes.size() >= 4) {
        uint16_t id = 0;
        std::memcpy(&id, args[1].bytes.data(), 2);
        if (const bf6::SoldierFields::Field* f = bf6::SoldierFields::get().find(bf6::SoldierFields::kBool,
                                                                      args[1].bytes[3], id)) {
            bf6::SoldierFields::get().mark_read(*f);
            if (!bf6::SoldierFields::get().known(*f)) return false;
            float v[4];
            bf6::SoldierFields::get().value(*f, v);
            out = Value::from_bool(v[0] != 0.0f);
            served_[key] += 1;
            return true;
        }
    }
    if (key == kSettingBool) {
        /* FUN_144328E80: the descriptor (u16 id, u16 flags) indexes an override bitmap
         * (+0x60) and a value bitmap (+0x10); with no override set it returns the
         * authored default, (flags >> 1) & 1. Offline nothing is overridden - there is
         * no server or gameplay code writing these - so the default IS the value.
         * The descriptor must be known; an unknown one is refused, not defaulted.
         * It is OPERAND 1: operand 0 is the same constant 0xF263DF78 in every soldier
         * call (the state collection's key), measured under BF6_LOG_FIELD_IDS - and
         * reading the default bit from it returned true for every setting. */
        if (args.size() < 2 || !args[1].known || args[1].bytes.size() < 4) return false;
        uint16_t flags = 0;
        std::memcpy(&flags, args[1].bytes.data() + 2, 2);
        served_[key] += 1;
        out = Value::from_bool(((flags >> 1) & 1) != 0);
        return true;
    }
    if (key == kWaterHeight) {
        const float value = -1024.0f;
        uint32_t raw = 0;
        std::memcpy(&raw, &value, sizeof(raw));
        served_[key] += 1;
        out = Value::from_u32(raw);
        return true;
    }

    if (key == kShooterStatus && args.empty()) {
        /* FUN_142CC9510 initializes (status, secondary) to (7, 0), then returns
         * unchanged when FUN_142CC9300 finds no shooter. Primary comes first. */
        const uint32_t secondary = 0, status = 7;
        out.bytes.resize(8);
        std::memcpy(out.bytes.data(), &secondary, 4);
        std::memcpy(out.bytes.data() + 4, &status, 4);
        out.known = true;
        served_[key] += 1;
        return true;
    }
    if (key == kActorStat && args.size() == 2) {
        /* FUN_147591900 zeroes valid/value before resolving either entity path. */
        out.bytes.assign(5, 0);          /* float primary, bool extra */
        out.known = true;
        served_[key] += 1;
        return true;
    }
    if (key == kClientString && args.size() == 3 && args[2].known &&
        args[2].bytes.size() >= 8) {
        /* FUN_1474ACAB0 copies DefaultValue when entity is null or player id is FF. */
        out.bytes.assign(args[2].bytes.begin(), args[2].bytes.begin() + 8);
        out.known = true;
        served_[key] += 1;
        return true;
    }
    if (key == kWeaponState && args.empty()) {
        /* FUN_142CC9850 native order:
         *   7, 0, FFFFFFFF, six false, four zero words, three zero floats.
         * The VM stores the last native output first, followed by the first 15. */
        out.bytes.assign(46, 0);
        const uint32_t status = 7, no_id = 0xFFFFFFFFu;
        std::memcpy(out.bytes.data() + 4, &status, 4);
        std::memcpy(out.bytes.data() + 12, &no_id, 4);
        out.known = true;
        served_[key] += 1;
        return true;
    }
    if (key == kGameplayFlags && args.size() == 1) {
        /* FUN_14172B810 clears all ten outputs before looking up the live object. */
        out.bytes.assign(13, 0);         /* bool primary + nine extras */
        out.known = true;
        served_[key] += 1;
        return true;
    }
    for (size_t i = 0; i < args.size(); ++i)
        if (!args[i].known || args[i].bytes.size() < sig.input_widths[i]) return false;
    if (key == kTweakFloat || key == kTweakBool || key == kTweakInt) {
        const auto it = tweakables_.find(args[0].as_u32());
        if (it == tweakables_.end()) return false;
        served_[key] += 1;
        out = key == kTweakBool ? Value::from_bool(it->second != 0) : Value::from_u32(it->second);
        return true;
    }
    if (key == kIdentityI32) {
        served_[key] += 1;
        out = Value::from_u32(args[0].as_u32());
        return true;
    }
    if (key == kContextStats) {
        served_[key] += 1;
        out.bytes.assign(16, 0); /* primary fourth output, then outputs 0..2 */
        out.known = true;
        return true;
    }
    if (key == kPlayerDefault) {
        served_[key] += 1;
        out = Value::from_u32(args[2].as_u32());
        return true;
    }
    if (key == kEaseProperty) {
        const uint32_t mode = args[0].as_u32();
        const uint32_t type = args[1].as_u32();
        float x = 0.0f;
        std::memcpy(&x, args[2].bytes.data(), 4);
        auto base = [&](float t) { return type == 0 ? t : t * t; };
        if (type > 1) return false; /* remaining function-table entries unrecovered */
        float y = x;
        if (mode == 0) y = base(x);
        else if (mode == 1) y = 1.0f - base(1.0f - x);
        else if (mode == 2)
            y = x < 0.5f ? base(x + x) * 0.5f
                         : 1.0f - base(2.0f - x - x) * 0.5f;
        else if (mode == 3)
            y = 0.5f < x ? (base(x + x - 1.0f) + 1.0f) * 0.5f
                         : 0.5f - base(1.0f - x - x) * 0.5f;
        uint32_t raw = 0;
        std::memcpy(&raw, &y, 4);
        served_[key] += 1;
        out = Value::from_u32(raw);
        return true;
    }
    if (key == kSubHandle) {
        uint64_t handle = 0;
        std::memcpy(&handle, args[0].bytes.data(), 8);
        if (handle != 0) return false;
        served_[key] += 1;
        out = empty_set();
        return true;
    }
    if (key == kEntityEntry || key == kEntityWalk || key == kSliceHandles) {
        uint32_t count = 0;
        std::memcpy(&count, args[0].bytes.data(), 4);
        if (count != 0) return false;
        served_[key] += 1;
        out = empty_set();
        return true;
    }
    if (key == kFilterByTags) {
        served_[key] += 1;
        out = empty_set();
        return true;
    }
    if (key == kRealmEquals) {
        served_[key] += 1;
        out = Value::from_bool(args[0].as_u32() == kRealmServer);
        return true;
    }
    if (key == kAffectorPick) {
        served_[key] += 1;
        const Value none = empty_set();
        const uint32_t no_player = 0xFFFFFFFFu;
        out.bytes.assign(4, 0);                                               /* escalation */
        out.bytes.insert(out.bytes.end(), none.bytes.begin(), none.bytes.end()); /* giver set */
        out.bytes.insert(out.bytes.end(), (const uint8_t*)&no_player, (const uint8_t*)&no_player + 4);
        out.bytes.insert(out.bytes.end(), 1 + 4 + 4, 0);                   /* active, rank, duration */
        out.known = true;
        return true;
    }
    if (key == kAiming) {
        served_[key] += 1;
        out.bytes.assign(16, 0);                 /* zoom (primary), yaw, pitch, roll */
        out.known = true;
        return true;
    }
    if (key == kFilterSet) {
        uint32_t count = 0;
        std::memcpy(&count, args[0].bytes.data(), 4);
        if (count != 0) return false;          /* context-dependent: not served */
        served_[key] += 1;
        out = empty_set();
        return true;
    }
    if (key == kAimingPlayer) {
        served_[key] += 1;
        /* FUN_14736B5D0 stores zero to all four outputs before resolving Player. */
        out.bytes.assign(16, 0);       /* primary fourth output, then first..third */
        out.known = true;
        return true;
    }
    if (key == kSetMerge) {
        served_[key] += 1;
        /* FUN_144341D40 copies input 0 verbatim, then appends unseen valid ids from
         * input 1, preserving order and stopping at the native's 64-entry cap. */
        out.bytes.assign(args[0].bytes.begin(), args[0].bytes.begin() + kSetBytes);
        uint32_t count = 0, incoming = 0;
        std::memcpy(&count, out.bytes.data(), 4);
        std::memcpy(&incoming, args[1].bytes.data(), 4);
        count = std::min(count, 64u);
        incoming = std::min(incoming, 64u);
        for (uint32_t i = 0; i < incoming && count < 64u; ++i) {
            uint32_t id = kNoId;
            std::memcpy(&id, args[1].bytes.data() + 4 + i * 4, 4);
            if (id == kNoId) continue;
            bool present = false;
            for (uint32_t j = 0; j < count; ++j) {
                uint32_t old = kNoId;
                std::memcpy(&old, out.bytes.data() + 4 + j * 4, 4);
                if (old == id) { present = true; break; }
            }
            if (!present) {
                std::memcpy(out.bytes.data() + 4 + count * 4, &id, 4);
                ++count;
            }
        }
        std::memcpy(out.bytes.data(), &count, 4);
        out.known = true;
        return true;
    }
    if (key == kContextPair) {
        served_[key] += 1;
        /* FUN_141730D50 initializes (bool,u32) to (0,0) before every context lookup. */
        out.bytes.assign(5, 0);        /* primary u32, then extra bool */
        out.known = true;
        return true;
    }
    if (key == kAbilityState) {
        served_[key] += 1;
        /* FUN_14174E130 initializes state=7 (Invalid), value=0. */
        const uint32_t invalid = 7;
        out.bytes.assign(8, 0);        /* primary value, then extra state */
        std::memcpy(out.bytes.data() + 4, &invalid, 4);
        out.known = true;
        return true;
    }
    if (key == kEntityFlags) {
        served_[key] += 1;
        /* FUN_14172B9B0 clears all ten outputs before resolving the entity. */
        out.bytes.assign(13, 0);       /* primary byte + nine extras (12 bytes) */
        out.known = true;
        return true;
    }
    if (key == kRelated) {
        served_[key] += 1;
        /* FUN_141737FC0 clears the destination before entity resolution. */
        out = empty_set();
        return true;
    }
    if (key == kApplyAffector) {
        served_[key] += 1;
        /* Server path FUN_141726A10 clears the result before scanning TargetList;
         * the offline world has no resolvable target/affector components. */
        out = Value::from_bool(false);
        return true;
    }
    served_[key] += 1;
    switch (key) {
    case kSetFromSlot: {
        /* FUN_14433ac10(slot): slots 0 and 1 are the graph's own entity; slot 2 is the
         * related entity at +0xF8 / +0xD0 of the context. A door graph passes slot 2 and
         * the next call (0x91C21F3C) requires it to be a VehicleEntityData; offline the
         * only one is the vehicle itself, so slot 2 answers the vehicle (stated). */
        out = empty_set();
        const uint32_t slot = args[0].as_u32();
        if (slot <= 2) {
            const uint32_t one = 1;
            std::memcpy(out.bytes.data(), &one, 4);
            std::memcpy(out.bytes.data() + 4, &self_id_, 4);
        }
        return true;
    }
    case kHandleOfFirst: {
        /* count == 0 or an unknown id -> null (0); the one entity -> a token */
        uint32_t count = 0, id = kNoId;
        std::memcpy(&count, args[0].bytes.data(), 4);
        std::memcpy(&id, args[0].bytes.data() + 4, 4);
        const uint64_t hd = (count > 0 && id == self_id_) ? (0x7E000000ull | id) : 0ull;
        out.bytes.assign(8, 0);
        std::memcpy(out.bytes.data(), &hd, 8);
        out.known = true;
        return true;
    }
    case kPartitionA:
    case kPartitionB: {
        /* No entity in this world has components, so every member fails the filter:
         * matched = {}, 0; unmatched = the input set and its count. */
        uint32_t count = 0;
        std::memcpy(&count, args[0].bytes.data(), 4);
        const Value none = empty_set();
        const uint32_t zero = 0;
        out.bytes.clear();
        const uint8_t* cp = reinterpret_cast<const uint8_t*>(&count);
        const uint8_t* zp = reinterpret_cast<const uint8_t*>(&zero);
        out.bytes.insert(out.bytes.end(), cp, cp + 4);                            /* primary: unmatched count */
        out.bytes.insert(out.bytes.end(), none.bytes.begin(), none.bytes.end());  /* matched set  */
        out.bytes.insert(out.bytes.end(), zp, zp + 4);                            /* matched count */
        out.bytes.insert(out.bytes.end(), args[0].bytes.begin(), args[0].bytes.begin() + kSetBytes); /* unmatched set */
        out.known = true;
        return true;
    }
    case kControllerOf: {
        /* 14433B290: clears the handle and the found byte; if the set holds exactly
         * one entity, resolves it, walks its components for a controller object,
         * stores a refcounted handle and sets found = 1. The one entity here is the
         * vehicle itself. */
        uint32_t count = 0, id = 0;
        std::memcpy(&count, args[0].bytes.data(), 4);
        std::memcpy(&id, args[0].bytes.data() + 4, 4);
        const bool found = count == 1 && id == self_id_;
        const uint64_t hd = found ? (0x7C000000ull | id) : 0ull;
        out = Value{};
        out.bytes.assign(9, 0);
        out.bytes[0] = found ? 1 : 0;                 /* primary: found */
        std::memcpy(out.bytes.data() + 1, &hd, 8);    /* extra: handle */
        out.known = true;
        return true;
    }
    case kControllerFlag: {
        /* INFERRED: the byte at +0xBC of the controlling object has no offline
         * source; false (0) for a null handle is the native's answer, and false is
         * also served for the vehicle's own controller until the field is named. */
        out = Value::from_bool(false);
        return true;
    }
    case kFrameClock: {
        /* Ticks at 60 Hz, times in seconds, the step last. */
        const uint32_t tick = (uint32_t)(time_ * 60.0);
        const float t0 = (float)time_, dt = 1.0f / 60.0f;
        out.bytes.assign(20, 0);
        std::memcpy(out.bytes.data() + 0, &dt, 4);          /* primary: DeltaTime */
        std::memcpy(out.bytes.data() + 4, &tick, 4);
        const uint32_t tick_end = tick + 1;
        std::memcpy(out.bytes.data() + 8, &tick_end, 4);
        const float t1 = t0 + dt;
        std::memcpy(out.bytes.data() + 12, &t0, 4);
        std::memcpy(out.bytes.data() + 16, &t1, 4);
        out.known = true;
        return true;
    }
    case kPlayerOption: {
        /* The miss path: hand back the default. */
        out = args.size() >= 3 ? args[2] : Value{};
        out.known = true;
        return true;
    }
    case kGameTick: {
        /* INFERRED values: the gearbox computes (tick - stored) / (out1 * out2), so
         * out0 is a tick counter and out1 * out2 a rate. Offline: the sim's tick,
         * 60 ticks per second, and 1. */
        const uint32_t tick = (uint32_t)(time_ * 60.0 + 0.5), rate = 60u, mul = 1u;
        out = Value{};
        out.bytes.assign(12, 0);
        std::memcpy(out.bytes.data(), &mul, 4);       /* primary: out2 (+0x26) */
        std::memcpy(out.bytes.data() + 4, &tick, 4);  /* extra 0: out0 (+0x18) */
        std::memcpy(out.bytes.data() + 8, &rate, 4);  /* extra 1: out1 (+0x24) */
        out.known = true;
        return true;
    }
    case kGameTime: {
        const float t = (float)time_;
        out = Value{};
        out.bytes.assign(4, 0);
        std::memcpy(out.bytes.data(), &t, 4);
        out.known = true;
        return true;
    }
    case kAffectorQuery: {
        /* FUN_141723e60: clears every output, then walks the entity set for an
         * ACTIVE affector of the asset's type in each entity's damage component
         * table, and on the first match fills the outputs and sets found = 1.
         * This world applies no affectors (an undamaged vehicle), so no entity
         * matches: empty set, zeros, found 0. The native's sixth output pointer
         * aliases the next register array and is not modelled. */
        const Value none = empty_set();
        out.bytes.assign(4, 0);                                              /* primary: +0x14 */
        out.bytes.insert(out.bytes.end(), none.bytes.begin(), none.bytes.end()); /* matched set */
        out.bytes.insert(out.bytes.end(), 4 + 1 + 4, 0);                   /* state, found, +0x1c */
        out.known = true;
        return true;
    }
    case kEntryState: {
        /* FUN_141564A10's defaults when no live entry/vehicle object answers:
         * state 0, (==0) true, the others false. This world has no entry objects. */
        out.bytes.assign(1 + 4 + 3, 0);
        out.bytes[0] = 0;      /* primary: (state == 3) */
        out.bytes[5] = 1;      /* extras: state 0, (==0) 1, (==1) 0, (==2) 0 */
        out.known = true;
        return true;
    }
    default:
        return false;
    }
}

}} // namespace bf6::expression
