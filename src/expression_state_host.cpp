#include "expression_state_host.h"

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
    {0x4899CB44u, 2}, {0x2B65ED67u, 3},
    {0xC8364385u, 1},
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
        std::fprintf(stderr, "cell read  key 0x%016llX %s depth %zu bound %08X %08X %08X %08X kind %u field %u\n",
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
    }
    out = 0;
    return true;
}

bool StateHost::describe(uint32_t key, OperatorSignature& out) {
    out = OperatorSignature{};
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
    if (key == kWriteFloatQ || key == kWriteAny || key == kWriteBool ||
        key == kWriteU32) {
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
    /* 0x532B3BA9: (object, field 0) -> a reference to the object's storage, which a
     * kind 0x2E record binds for the region-3 reads after it (FUN_1438068f0 builds a
     * type-pointer/address pair; 16 bytes). The VM resolves it; invoke is not used. */
    if (key == 0x532B3BA9u) {
        out.input_widths = {4, 4};
        out.output_width = 16;
        out.output_is_reference_to_input0 = true;
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
    if (const ContextOp* c = context_op(key)) {
        out.input_widths.assign((size_t)c->arity, 4u);
        out.output_width = 4;
        return true;
    }
    return false;
}

static const uint32_t kPartTransform = 0x04BEFF62u;

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

void StateHost::set_named_transform(uint32_t name_hash, const float rows[16]) {
    std::vector<uint8_t> bytes(64, 0);
    std::memcpy(bytes.data(), rows, 64);
    named_transforms_[name_hash] = bytes;
}

bool StateHost::describe_call(uint32_t key, const std::vector<uint32_t>& consts,
                              OperatorSignature& out) {
    if (key == 0x532B3BA9u && consts.size() >= 2 && consts[1] != 0u) {
        /* Every measured call passes field 0 (403 of 403); a non-zero field would
         * need an offset rule nobody has measured, so refuse it. */
        out = OperatorSignature{};
        return false;
    }
    if (key == kNamedTransform && consts.size() >= 3) {
        /* All three operands constants and a mode this measurement has seen;
         * anything else is refused rather than guessed. */
        if (consts[1] == 0xFFFFFFFFu || consts[2] > 2u) { out = OperatorSignature{}; return false; }
        out.input_widths = {4, 4, 4};
        out.output_width = 64;
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
            if (v.known) channels_[ck] = std::vector<uint8_t>(v.bytes.begin(),
                                            v.bytes.begin() + std::min<size_t>(v.bytes.size(), ch->width));
            else channels_.erase(ck);
            channel_writes_[ck] += 1;
            out = Value{};
            return true;
        }
        const auto it = channels_.find(ck);
        out.bytes.assign(ch->width, 0);
        if (it != channels_.end())
            std::memcpy(out.bytes.data(), it->second.data(), std::min<size_t>(it->second.size(), ch->width));
        else
            unsupplied_channels_[ck] += 1;       /* reads ZERO, recorded */
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
            unsupplied_transforms_[name] += 1;
            out.bytes.assign(64, 0);                       /* identity, recorded */
            const float one = 1.0f;
            std::memcpy(out.bytes.data() + 0,  &one, 4);
            std::memcpy(out.bytes.data() + 20, &one, 4);
            std::memcpy(out.bytes.data() + 40, &one, 4);
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
    if (key == kWriteFloatQ || key == kWriteAny || key == kWriteBool ||
        key == kWriteU32) {
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
            std::fprintf(stderr, "cell write op %08X key 0x%016llX value 0x%08X depth %zu bound3 %08X kind %u field %u path %08X\n",
                         key, (unsigned long long)cell_key(dest), args[1].as_u32(),
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

bool PhysicsQueryHost::describe(uint32_t key, OperatorSignature& out) {
    out = OperatorSignature{};
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
const uint32_t kSetBytes      = 260u;        /* u32 count + 64 x u32                   */
const uint32_t kNoId          = 0x000FFFFFu; /* DAT_149b71b48                          */

Value empty_set() {
    Value v;
    v.bytes.assign(kSetBytes, 0);
    for (uint32_t i = 0; i < 64; ++i) std::memcpy(v.bytes.data() + 4 + i * 4, &kNoId, 4);
    v.known = true;
    return v;
}
}

/* __GetTweakableFloat / __GetTweakableBool (CRC-32 of the node names). */
const uint32_t kTweakFloat = 0x10E7EF91u;
const uint32_t kTweakBool  = 0xC6485A8Bu;

bool WorldHost::describe(uint32_t key, OperatorSignature& out) {
    out = OperatorSignature{};
    switch (key) {
    case kTweakFloat: out.input_widths = {4}; out.output_width = 4; return true;
    case kTweakBool:  out.input_widths = {4}; out.output_width = 1; return true;
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
    case kEntryState:
        /* (entity/slot, bool gate) -> (state int, ==0, ==1, ==2, ==3) */
        out.input_widths = {4, 1};
        out.extra_output_widths = {4, 1, 1, 1};
        out.output_width = 1;
        return true;
    default: return false;
    }
}

bool WorldHost::invoke(uint32_t key, const std::vector<Value>& args, Value& out) {
    OperatorSignature sig;
    if (!describe(key, sig) || args.size() != sig.input_widths.size()) return false;
    for (size_t i = 0; i < args.size(); ++i)
        if (!args[i].known || args[i].bytes.size() < sig.input_widths[i]) return false;
    if (key == kTweakFloat || key == kTweakBool) {
        const auto it = tweakables_.find(args[0].as_u32());
        if (it == tweakables_.end()) return false;
        served_[key] += 1;
        out = key == kTweakBool ? Value::from_bool(it->second != 0) : Value::from_u32(it->second);
        return true;
    }
    served_[key] += 1;
    switch (key) {
    case kSetFromSlot: {
        /* FUN_14433ac10(slot): slots 0 and 1 are the graph's own entity; slot 2 is
         * a related entity this world does not have. A miss adds nothing. */
        out = empty_set();
        const uint32_t slot = args[0].as_u32();
        if (slot < 2) {
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
