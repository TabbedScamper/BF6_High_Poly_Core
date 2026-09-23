#include "expression_vm.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>

namespace bf6 { namespace expression {
namespace {

static const uint32_t kParameterValue = 0x9a38f86fu;
static const uint32_t kParameterObject = 0x24637f30u;
static const uint32_t kSink = 0x26730cb8u;
static const uint32_t kTerminator = 0x225e7a5bu;
static const uint32_t kVariableStore = 0x0b201cc7u;

struct Slots {
    explicit Slots(size_t size) : bytes(size), initialized(size), tainted(size) {}
    std::vector<uint8_t> bytes, initialized, tainted;
    std::map<uint32_t, uint32_t> widths;
    /* Slot offset -> the storage a field reference written there points at. A side
     * table rather than a value in the slot, so no sentinel can collide with data. */
    std::map<uint32_t, Operand> refs;
    /* REFERENCE REGISTERS, indexed by region - 3. A kind 0x2E record binds the
     * region its trailing dword names (interpreter case 0x2e). 0 = never bound,
     * 1 = bound to storage this VM can read (bind_target), 2 = bound to something
     * it cannot see (a pointer held in the instance) - reads through it are unknown. */
    std::vector<uint8_t> bind_state;
    std::vector<Operand> bind_target;

    /* Every slot offset the graph names, and the ones no record ever writes. A read
     * of a never-written slot is a read of the buffer's initial zero, and is answered
     * as known - see the note where these are filled. */
    std::set<uint32_t> named_slots;
    std::set<uint32_t> never_written;
    /* Per byte: whether ANY record's output span can reach it. */
    std::vector<uint8_t> writable;

    /* ARRAY HEAP. Expression arrays are an 8-byte data pointer with the element
     * count at data-4 (research: expression-lerp-array-iteration-and-context-nodes).
     * Offline a pointer is a TAGGED value naming a block here, so `it != end`
     * compares real bytes and a register bound to an iterator reads the element.
     * Per evaluation, like the slot file: the graphs rebuild their arrays each run. */
    struct Block {
        std::vector<uint8_t> bytes, initialized;
        uint32_t count = 0, stride = 0;
    };
    std::vector<Block> heap;
    /* Region-1 words this run wrote (the array constructor's output lands in the
     * instance image); read back ahead of the image. */
    std::map<uint32_t, Value> r1_overlay;

    /* Slot-to-slot copy that keeps knownness BYTE BY BYTE, so a 16-byte move of a
     * float slot does not make the float unknown just because the next 12 bytes are. */
    bool copy(uint32_t from, uint32_t to, uint32_t width) {
        if (!width || from > bytes.size() || width > bytes.size() - from ||
            to > bytes.size() || width > bytes.size() - to) return false;
        std::vector<uint8_t> b(bytes.begin() + from, bytes.begin() + from + width);
        std::vector<uint8_t> i(initialized.begin() + from, initialized.begin() + from + width);
        std::vector<uint8_t> t(tainted.begin() + from, tainted.begin() + from + width);
        std::copy(b.begin(), b.end(), bytes.begin() + to);
        std::copy(i.begin(), i.end(), initialized.begin() + to);
        std::copy(t.begin(), t.end(), tainted.begin() + to);
        widths[to] = width;
        refs.erase(to);
        return true;
    }

    Value read(uint32_t offset, uint32_t width) const {
        Value v;
        if (!width || offset > bytes.size() || width > bytes.size() - offset)
            return v;
        /* A never-written slot read as a value: the buffer's initial zero, known. The
         * width is the read's own, which is why this cannot be done up front. */
        if (width <= 16 && never_written.count(offset)) {
            bool untouched = true;
            for (uint32_t i = 0; i < width; ++i)
                if (initialized[offset + i]) { untouched = false; break; }
            if (untouched) {
                v.bytes.assign(width, 0);
                v.known_bytes.assign(width, 1);
                v.known = true;
                return v;
            }
        }
        v.bytes.assign(bytes.begin() + offset, bytes.begin() + offset + width);
        v.known_bytes.assign(initialized.begin() + offset, initialized.begin() + offset + width);
        v.known = true;
        /* AND THE SAME RULE PER BYTE. A Vec3 is often assembled lane by lane, and a
         * lane no record writes stays the buffer's zero - the boat builds three of its
         * force directions that way, and leaving those lanes unknown refused the
         * NormalizeFloat3 above them and took three applyForce calls with it. A byte
         * counts as unwritable only if it falls outside every record's output span,
         * which is bounded generously below, so this under-claims rather than over. */
        for (uint32_t i = 0; i < width; ++i)
            if (!initialized[offset + i] && offset + i < writable.size() &&
                !writable[offset + i])
                v.known_bytes[i] = 1;
        for (uint32_t i = 0; i < width; ++i) {
            const bool byte_known = v.known_bytes[i] != 0;
            v.known = v.known && byte_known;
            v.tainted = v.tainted || !byte_known;
            v.tainted = v.tainted || tainted[offset + i] != 0;
        }
        return v;
    }

    bool write(uint32_t offset, uint32_t width, const Value& value) {
        if (!width || offset > bytes.size() || width > bytes.size() - offset ||
            value.bytes.size() < width) return false;
        {
            static const char* watch = std::getenv("BF6_SLOT_WATCH");
            if (watch) {
                const uint32_t lo = (uint32_t)std::strtoul(watch, nullptr, 0);
                if (offset < lo + 40 && offset + width > lo) {
                    std::fprintf(stderr, "slot write %u..%u (known=%d):", offset,
                                 offset + width, (int)value.known);
                    for (uint32_t b = 0; b + 3 < width && b < 48; b += 4) {
                        uint32_t w = 0;
                        std::memcpy(&w, value.bytes.data() + b, 4);
                        std::fprintf(stderr, " %08X", w);
                    }
                    std::fprintf(stderr, "\n");
                }
            }
        }
        std::copy(value.bytes.begin(), value.bytes.begin() + width,
                  bytes.begin() + offset);
        std::fill(initialized.begin() + offset, initialized.begin() + offset + width,
                  (uint8_t)(value.known ? 1 : 0));
        std::fill(tainted.begin() + offset, tainted.begin() + offset + width,
                  (uint8_t)(value.tainted ? 1 : 0));
        widths[offset] = width;
        refs.erase(offset);
        return true;
    }
};

static size_t align16(size_t value) { return (value + 15u) & ~size_t(15u); }

/* Heap pointer encoding: tag in the top 16 bits, block in the next 24, byte
 * offset in the low 24. A heap block is addressed as region kHeapRegion + block. */
static const uint64_t kHeapTag = 0x5EA0ull << 48;
static const uint32_t kHeapRegion = 0xFE000000u;
static uint64_t heap_ptr(uint32_t block, uint32_t off) {
    return kHeapTag | ((uint64_t)(block & 0xFFFFFFu) << 24) | (off & 0xFFFFFFu);
}
static bool heap_decode(const Value& v, uint32_t& block, uint32_t& off) {
    if (!v.known || v.bytes.size() < 8) return false;
    uint64_t p = 0;
    std::memcpy(&p, v.bytes.data(), 8);
    if ((p & (0xFFFFull << 48)) != kHeapTag) return false;
    block = (uint32_t)((p >> 24) & 0xFFFFFFu);
    off = (uint32_t)(p & 0xFFFFFFu);
    return true;
}
static Value value_u64(uint64_t x) {
    Value v; v.bytes.resize(8); std::memcpy(v.bytes.data(), &x, 8); v.known = true; return v;
}

static Value unknown(uint32_t width, bool tainted = true) {
    Value v; v.bytes.resize(width); v.known = false; v.tainted = tainted; return v;
}

static Value materialize(const Graph& graph, const Instance* instance,
                         const Slots& slots,
                         const Operand& operand,
                         uint32_t width)
{
    if (!width) return Value{};
    if (operand.region == 0) {
        if (operand.offset > graph.constant_pool.size() ||
            width > graph.constant_pool.size() - operand.offset)
            return unknown(width);
        Value v;
        v.bytes.assign(graph.constant_pool.begin() + operand.offset,
                       graph.constant_pool.begin() + operand.offset + width);
        if (instance)
            for (auto it = instance->pool_patches.lower_bound(operand.offset >= 3 ? operand.offset - 3 : 0);
                 it != instance->pool_patches.end() && it->first < operand.offset + width; ++it)
                for (uint32_t b = 0; b < 4; ++b) {
                    const uint32_t at = it->first + b;
                    if (at >= operand.offset && at < operand.offset + width)
                        v.bytes[at - operand.offset] = (uint8_t)(it->second >> (8 * b));
                }
        v.known = true;
        return v;
    }
    if (operand.region == 1) {
        const auto ov = slots.r1_overlay.find(operand.offset);
        if (ov != slots.r1_overlay.end() && ov->second.bytes.size() >= width) {
            Value v = ov->second;
            v.bytes.resize(width);
            return v;
        }
        if (!instance) return unknown(width);
        const size_t base = align16(0x20u +
            (size_t)graph.header.external_bindings * 8u +
            (size_t)graph.header.instance_buffer_count * 4u);
        const size_t at = base + operand.offset;
        if (at > instance->image.size() || width > instance->image.size() - at)
            return unknown(width);
        Value v;
        v.bytes.assign(instance->image.begin() + at,
                       instance->image.begin() + at + width);
        v.known = true;
        // Constructor-owned ranges are deliberately withheld. The data type's
        // exact byte width is not present in the graph, so even the first byte
        // of a declared object is enough to make this read unknown.
        for (const TypedValueGroup& group : graph.instance_values)
            for (uint32_t off : group.offsets)
                if (operand.offset == off) return unknown(width);
        return v;
    }
    if (operand.region == 2) {
        /* A SLOT HOLDING A REFERENCE READS THROUGH IT. The engine passes addresses
         * where this VM passes values, so a slot that a field-address operator filled
         * holds a pointer and every consumer dereferences it: the struct builder that
         * copies a rotor config reads fourteen of them, and reading the pointer slots
         * themselves gave it fourteen zeros. A reference to a whole object (offset
         * zero into it) is left alone, because those are consumed by a kind 0x2E bind
         * and reading through them here would change what the bind sees. */
        const auto ref = slots.refs.find(operand.offset);
        if (ref != slots.refs.end() && ref->second.region == 2 &&
            ref->second.offset != 0)
            return slots.read(ref->second.offset, width);
        return slots.read(operand.offset, width);
    }
    if (operand.region >= kHeapRegion) {
        const uint32_t b = operand.region - kHeapRegion;
        if (b >= slots.heap.size()) return unknown(width);
        const auto& blk = slots.heap[b];
        if (operand.offset > blk.bytes.size() || width > blk.bytes.size() - operand.offset)
            return unknown(width);
        Value v;
        v.bytes.assign(blk.bytes.begin() + operand.offset, blk.bytes.begin() + operand.offset + width);
        v.known = true;
        for (uint32_t i = 0; i < width; ++i)
            if (!blk.initialized[operand.offset + i]) { v.known = false; v.tainted = true; }
        return v;
    }
    /* A BOUND register is a read THROUGH its reference. Reading it as an immediate
     * fabricated known zeros: two aim graphs were "sound" only because of that. */
    /* CONFIRMED (interpreter): every operand is table[region] + offset. A register
     * never bound has base 0, so an operator handed it receives the OFFSET itself -
     * which is why reading these as inline immediates worked. Bound to something
     * this VM cannot see: unknown. */
    if (operand.region >= 3 && operand.region - 3u < slots.bind_state.size() &&
        slots.bind_state[operand.region - 3u] != 0) {
        const size_t k = operand.region - 3u;
        if (slots.bind_state[k] != 1) return unknown(width);
        const Operand t = slots.bind_target[k];
        if (t.region >= 3 && t.region < kHeapRegion) return unknown(width);   /* no chains */
        return materialize(graph, instance, slots,
                           Operand{t.region, t.offset + operand.offset}, width);
    }
    // All 39,937 measured region-3+ operands in the shipped corpus lie past
    // the graph's declared binding population. They are inline immediates,
    // not host arguments; the carried value is the second dword. A wider
    // value needs operator/type knowledge and therefore remains unknown.
    if (width > 4) return unknown(width);
    Value v;
    v.bytes.resize(width);
    std::memcpy(v.bytes.data(), &operand.offset, width);
    v.known = true;
    return v;
}

static const Operand* last_slot(const Record& record) {
    for (auto it = record.operands.rbegin(); it != record.operands.rend(); ++it)
        if (it->region == 2) return &*it;
    return nullptr;
}

struct EffectiveCall {
    std::vector<const Operand*> inputs;
    const Operand* output = nullptr;
    std::vector<const Operand*> extra_outputs;
};

/* Reconstruct the call shape used by Frostbite's expression dispatcher.
 *
 * The serialized record keeps every (region, value) pair in one stream, but
 * the runtime does not pass that stream through verbatim.  Region 0/2 pairs
 * are the declared operands; non-storage regions are inline immediates and
 * only top a short declaration up to the operator arity.  When a record has
 * slot operands, its final slot is the result and is not an input.
 *
 * This is intentionally arity bounded.  Consuming every inline pair made a
 * call depend on padding/auxiliary values the game never passes and, when an
 * inline pair precedes a pool operand on disk, reversed non-commutative calls.
 */
static EffectiveCall effective_call(const Record& record,
                                    const OperatorSignature& signature,
                                    const Slots* slots = nullptr)
{
    EffectiveCall call;
    /* COUNTED LISTS (kind 0x23): the record states which operands are inputs,
     * outputs and context, so nothing is inferred. Context operands follow the
     * inputs; the primary output is the LAST output, the ones before it are the
     * extra outputs, in order. */
    if (record.counted_lists) {
        const size_t ni = record.n_in, no = record.n_out, nc = record.n_ctx;
        if (ni + no + nc != record.operands.size()) return call;
        for (size_t i = 0; i < ni; ++i) call.inputs.push_back(&record.operands[i]);
        for (size_t i = 0; i < nc; ++i) call.inputs.push_back(&record.operands[ni + no + i]);
        if (signature.output_width && no > 0) {
            call.output = &record.operands[ni + no - 1];
            const size_t want = signature.extra_output_widths.size();
            for (size_t i = 0; i + 1 < no && i < want; ++i)
                call.extra_outputs.push_back(&record.operands[ni + i]);
        }
        return call;
    }
    if (signature.output_width) call.output = last_slot(record);

    /* AN INSTANCE OPERAND IS AN ARGUMENT IN ITS OWN PLACE. Region 1 is the graph's
     * persistent memory and a call may take a value straight out of it: a tank reads
     * its wheel resistance from r1 as the second argument of the spin function. It
     * used to be set aside with the bound registers and appended AFTER the real
     * arguments, which silently shifted every argument past it by one - the config
     * landed in the resistance's place and the call was refused for a hundred and
     * twelve bytes of nothing. Only a bound register (region 3 and up), whose
     * position is not an argument position, is still set aside. */
    /* A BOUND REGISTER IS AN ARGUMENT IN ITS OWN PLACE TOO. The rule above was
     * applied to region 1 and not to the reference registers, and a boat pays for
     * it: its engine torque table is passed as a bound register in FIRST position,
     * so setting it aside made the 4-byte curve index argument 0 and the table was
     * read as four known bytes of a hundred and twenty - refused, and the boat had
     * no thrust. A register that is BOUND names storage and is passed like any
     * other operand; one that was never bound is still an inline immediate the game
     * does not pass, so those stay spares. */
    auto is_bound = [&](const Operand& op) {
        return slots && op.region >= 3 &&
               op.region - 3u < slots->bind_state.size() &&
               slots->bind_state[op.region - 3u] == 1;
    };
    std::vector<const Operand*> spares;
    for (const Operand& operand : record.operands) {
        if (call.output == &operand) break;
        if (operand.region <= 2 || is_bound(operand))
            call.inputs.push_back(&operand);
        else
            spares.push_back(&operand);
    }
    /* The last N slot operands before the output are second outputs, not inputs. */
    if (call.output && !signature.extra_output_widths.empty()) {
        const size_t n = signature.extra_output_widths.size();
        std::vector<const Operand*> tail;
        for (auto it = call.inputs.rbegin(); it != call.inputs.rend() && tail.size() < n; ++it)
            if ((*it)->region == 2) tail.push_back(*it);
        if (tail.size() == n) {
            for (auto it = tail.rbegin(); it != tail.rend(); ++it) {
                call.extra_outputs.push_back(*it);
                call.inputs.erase(std::find(call.inputs.begin(), call.inputs.end(), *it));
            }
        }
    }
    for (const Operand* spare : spares) {
        if (call.inputs.size() >= signature.input_widths.size()) break;
        call.inputs.push_back(spare);
    }
    return call;
}

static uint32_t move_width(const Record& record) {
    /* 0x25 carries a BYTE width (0x70 for a WheelConfig). 0x24 does not: it carries a
     * count, and its width is the destination type's size, which the typed-copy path
     * looks up - see the note there. Reading that count as a width copied five bytes
     * of a forty-byte curve handle on every boat. */
    if (record.kind == 0x25 && record.has_trailing_dword && record.trailing_dword > 0 &&
        record.trailing_dword <= 4096) return record.trailing_dword;
    if (record.kind == 0x1e) return 1;
    /* 0x22 is a 16-byte (Vec3) move, measured across all vehicle graphs by what
     * produces its source and consumes its destination: Vec3 on both sides
     * dominates (V3->V3 233, V3 sources 1,836 of the typed cases, V3 consumers
     * 1,464), and the suspension moves ray End into the point DistanceFloat3 reads
     * with it. 0x20 is float-dominant and 0x21 float-only; both stay 4. */
    if (record.kind == 0x22) return 16;
    /* CONFIRMED (interpreter cases 0x1e..0x22): 1, 2, 4, 8, 16 bytes. 0x21 was 4
     * here; it is 8. */
    if (record.kind == 0x1f) return 2;
    if (record.kind == 0x20) return 4;
    if (record.kind == 0x21) return 8;
    return 0;
}

static void add_unresolved(Evaluation& result, uint32_t key) {
    if (key && std::find(result.unresolved_keys.begin(),
                         result.unresolved_keys.end(), key) ==
               result.unresolved_keys.end())
        result.unresolved_keys.push_back(key);
}

} // namespace

Value Value::from_u32(uint32_t value) {
    Value v; v.bytes.resize(4); std::memcpy(v.bytes.data(), &value, 4);
    v.known = true; return v;
}

Value Value::from_bool(bool value) {
    Value v; v.bytes.push_back(value ? 1u : 0u); v.known = true; return v;
}

uint32_t Value::as_u32() const {
    uint32_t v = 0; if (!bytes.empty())
        std::memcpy(&v, bytes.data(), std::min<size_t>(4, bytes.size()));
    return v;
}

bool Value::as_bool() const { return !bytes.empty() && bytes[0] != 0; }

void NamedBuiltins::add(uint32_t key, const std::string& current_exe_name) {
    if (key && !current_exe_name.empty()) names_[key] = current_exe_name;
}

bool NamedBuiltins::describe(uint32_t key, OperatorSignature& out) {
    out = OperatorSignature{};
    const auto it = names_.find(key);
    if (it == names_.end()) return false;
    const std::string& n = it->second;
    if (n == "Not") { out.input_widths = {1}; out.output_width = 1; }
    else if (n == "And" || n == "Or") {
        out.input_widths = {1, 1}; out.output_width = 1;
    } else if (n == "AbsoluteFloat" || n == "FloorFloatFloat" ||
               n == "SinFloat" || n == "TanFloat" ||
               n == "SqrtFloat") {
        out.input_widths = {4}; out.output_width = 4;
    } else if (n == "ToInt32Float" || n == "ImplicitToFloatInt") {
        out.input_widths = {4}; out.output_width = 4;
    } else if (n == "AddFloat" || n == "MultiplyFloatFloatFloat" ||
               n == "DivideFloatFloatFloat" || n == "PowFloat") {
        out.input_widths = {4, 4}; out.output_width = 4;
    } else if (n == "AddInt" || n == "SubtractInt" ||
               n == "MultiplyIntIntInt") {
        out.input_widths = {4, 4}; out.output_width = 4;
    } else if (n == "ClampInt") {
        out.input_widths = {4, 4, 4}; out.output_width = 4;
    } else if (n == "EqualsBool") {
        out.input_widths = {1, 1}; out.output_width = 1;
    } else if (n == "GreaterThanFloat" || n == "GreaterThanOrEqualsFloat" ||
               n == "NotEqualsFloat" || n == "LessThanFloat" ||
               n == "LessThanOrEqualsFloat" || n == "EqualsInt" ||
               n == "NotEqualsInt" || n == "GreaterThanInt" ||
               n == "GreaterThanOrEqualsInt" || n == "LessThanInt" ||
               n == "LessThanOrEqualsInt" || n == "EnumEqualFunc") {
        out.input_widths = {4, 4}; out.output_width = 1;
    } else return false;
    return true;
}

bool NamedBuiltins::invoke(uint32_t key, const std::vector<Value>& a,
                           Value& out) {
    OperatorSignature signature;
    if (!describe(key, signature) || a.size() != signature.input_widths.size())
        return false;
    for (const Value& v : a) if (!v.known) return false;
    const std::string& n = names_[key];
    auto f32 = [](const Value& v) {
        float f = 0.f; std::memcpy(&f, v.bytes.data(), 4); return f;
    };
    auto i32 = [](const Value& v) { return (int32_t)v.as_u32(); };
    auto put_f32 = [](float f) {
        Value v; v.bytes.resize(4); std::memcpy(v.bytes.data(), &f, 4);
        v.known = true; return v;
    };
    auto put_i32 = [](int32_t i) { return Value::from_u32((uint32_t)i); };

    if (n == "Not") out = Value::from_bool(!a[0].as_bool());
    else if (n == "And") out = Value::from_bool(a[0].as_bool() && a[1].as_bool());
    else if (n == "Or") out = Value::from_bool(a[0].as_bool() || a[1].as_bool());
    else if (n == "AbsoluteFloat") out = put_f32(std::fabs(f32(a[0])));
    else if (n == "FloorFloatFloat") out = put_f32(std::floor(f32(a[0])));
    else if (n == "SinFloat") out = put_f32(std::sin(f32(a[0])));
    else if (n == "TanFloat") {
        const float value = std::tan(f32(a[0]));
        if (!std::isfinite(value)) return false;
        out = put_f32(value);
    }
    else if (n == "SqrtFloat") {
        if (f32(a[0]) < 0.f) return false;
        out = put_f32(std::sqrt(f32(a[0])));
    }
    else if (n == "ToInt32Float") {
        const float value = f32(a[0]);
        if (!std::isfinite(value) ||
            value < static_cast<float>(std::numeric_limits<int32_t>::min()) ||
            value > static_cast<float>(std::numeric_limits<int32_t>::max()))
            return false;
        out = put_i32(static_cast<int32_t>(value));
    }
    else if (n == "ImplicitToFloatInt") out = put_f32((float)i32(a[0]));
    else if (n == "AddFloat") out = put_f32(f32(a[0]) + f32(a[1]));
    else if (n == "MultiplyFloatFloatFloat") out = put_f32(f32(a[0]) * f32(a[1]));
    else if (n == "DivideFloatFloatFloat") {
        /* DIVIDING BY ZERO IS AN ANSWER HERE, not a refusal. The hardware gives an
         * infinity and the graphs are written expecting it: the tank's torque chain
         * divides by a zero on the very first tick and clamps the result two records
         * later, so refusing produced an unknown that poisoned everything downstream
         * of it and the tank never made any torque at all. Refusing is only right
         * where the original would not have computed a value; here it would. */
        out = put_f32(f32(a[0]) / f32(a[1]));
    } else if (n == "PowFloat") {
        const float value = std::pow(f32(a[0]), f32(a[1]));
        if (!std::isfinite(value)) return false;
        out = put_f32(value);
    } else if (n == "AddInt")
        out = Value::from_u32(a[0].as_u32() + a[1].as_u32());
    else if (n == "SubtractInt")
        out = Value::from_u32(a[0].as_u32() - a[1].as_u32());
    else if (n == "MultiplyIntIntInt")
        out = Value::from_u32(a[0].as_u32() * a[1].as_u32());
    else if (n == "ClampInt") {
        const int32_t value = i32(a[0]);
        const int32_t minimum = i32(a[1]);
        const int32_t maximum = i32(a[2]);
        if (minimum > maximum) return false;
        out = put_i32((std::max)(minimum, (std::min)(value, maximum)));
    }
    else if (n == "EqualsBool")
        out = Value::from_bool(a[0].as_bool() == a[1].as_bool());
    else if (n == "GreaterThanFloat") out = Value::from_bool(f32(a[0]) > f32(a[1]));
    else if (n == "GreaterThanOrEqualsFloat") out = Value::from_bool(f32(a[0]) >= f32(a[1]));
    else if (n == "NotEqualsFloat") out = Value::from_bool(f32(a[0]) != f32(a[1]));
    else if (n == "LessThanFloat") out = Value::from_bool(f32(a[0]) < f32(a[1]));
    else if (n == "LessThanOrEqualsFloat") out = Value::from_bool(f32(a[0]) <= f32(a[1]));
    else if (n == "EqualsInt" || n == "EnumEqualFunc")
        out = Value::from_bool(a[0].as_u32() == a[1].as_u32());
    else if (n == "NotEqualsInt") out = Value::from_bool(i32(a[0]) != i32(a[1]));
    else if (n == "GreaterThanInt") out = Value::from_bool(i32(a[0]) > i32(a[1]));
    else if (n == "GreaterThanOrEqualsInt") out = Value::from_bool(i32(a[0]) >= i32(a[1]));
    else if (n == "LessThanInt") out = Value::from_bool(i32(a[0]) < i32(a[1]));
    else if (n == "LessThanOrEqualsInt") out = Value::from_bool(i32(a[0]) <= i32(a[1]));
    else return false;
    return true;
}

bool make_instance(const Graph& graph, Instance& out, std::string& error) {
    error.clear(); out = Instance{};
    if (graph.instance_image.size() != graph.header.instance_header_size) {
        error = "instance image size does not match the graph header";
        return false;
    }
    out.graph = &graph;
    out.image = graph.instance_image;
    return true;
}

Evaluation evaluate(const Graph& graph, Instance* instance,
                    const std::vector<Value>& arguments, Host* host)
{
    Evaluation result;
    if (!graph.exact_record_tiling) {
        result.termination = Termination::UntiledGraph;
        result.diagnostics.push_back("record region is not an exact proven-kind tiling");
        return result;
    }
    if (instance && instance->graph != &graph) {
        result.termination = Termination::InvalidGraph;
        result.diagnostics.push_back("expression instance belongs to another graph");
        return result;
    }
    std::map<uint32_t, const Record*> records;
    for (const Record& record : graph.records) records[record.offset] = &record;
    if (records.empty() || records.find(0) == records.end()) {
        result.termination = Termination::Complete;
        return result;
    }
    Slots slots(graph.header.slot_file_size);
    /* THE HEAP, LENT TO THE HOSTS for this run and taken back at the end, so a host
     * that grows an engine vector (the tank's track sampler writes one contact per
     * road wheel) can hand the graph a pointer the array operators understand. The
     * guard takes it back on every exit path: a sink left behind would point into a
     * Slots that no longer exists. */
    struct Sink final : HeapSink {
        Sink(Slots& s, const Graph& g, const Instance* inst) : slots(s), graph(g), instance(inst) {}
        bool pool(uint32_t offset, uint32_t width, std::vector<uint8_t>& out) override {
            const Value v = materialize(graph, instance, slots, Operand{0, offset}, width);
            if (!v.known || v.bytes.size() < width) return false;
            out.assign(v.bytes.begin(), v.bytes.begin() + width);
            return true;
        }
        bool alloc(uint32_t count, uint32_t stride, const uint8_t* bytes, Value& out) override {
            if (!count || !stride || (uint64_t)count * stride > (1u << 20)) return false;
            Slots::Block blk;
            blk.count = count;
            blk.stride = stride;
            blk.bytes.assign(bytes, bytes + (size_t)count * stride);
            blk.initialized.assign(blk.bytes.size(), 1);
            slots.heap.push_back(std::move(blk));
            out = value_u64(heap_ptr((uint32_t)slots.heap.size() - 1, 0));
            return true;
        }
        bool read(const Value& ptr, uint32_t& count, uint32_t& stride,
                  const uint8_t*& bytes) override {
            uint32_t b = 0, o = 0;
            if (!heap_decode(ptr, b, o) || b >= slots.heap.size()) return false;
            const Slots::Block& blk = slots.heap[b];
            if (o >= blk.bytes.size() && !blk.bytes.empty()) return false;
            count = blk.count;
            stride = blk.stride;
            bytes = blk.bytes.data() + o;
            return true;
        }
        Slots& slots;
        const Graph& graph;
        const Instance* instance;
    } sink(slots, graph, instance);
    struct SinkGuard {
        ~SinkGuard() { if (h) h->set_heap_sink(nullptr); }
        Host* h;
    } guard{host};
    if (host) host->set_heap_sink(&sink);
    /* Caller-provided slot contents, applied before the first record runs - the
     * offline stand-in for the engine filling a callee's slot file. See
     * Instance::slot_seed. */
    if (instance)
        for (const auto& kv : instance->slot_seed)
            if (!kv.second.bytes.empty())
                slots.write(kv.first, (uint32_t)kv.second.bytes.size(), kv.second);
    size_t argument_index = 0;
    /* REFERENCE REGISTERS. CONFIRMED FROM THE INTERPRETER (case 0x2e):
     * table[u32 at +12] = the pointer stored at the operand, i.e. a 0x2E record binds
     * the region its TRAILING DWORD names. (Static order happened to agree on the
     * graphs first measured; the explicit index is the rule.) Regions 3+ exist up to
     * the header's pointer_table_entries; the first external_bindings of them are
     * preset from the instance header, which offline is none (0 on every measured
     * vehicle graph). */
    std::map<uint32_t, size_t> bind_ordinal;   /* record offset -> register - 3 */
    size_t bind_regs = graph.header.pointer_table_entries > 3
        ? graph.header.pointer_table_entries - 3 : 0;
    for (const Record& record : graph.records)
        if (record.kind == 0x2e && record.has_trailing_dword &&
            record.trailing_dword >= 3 && record.trailing_dword < 4096) {
            bind_ordinal[record.offset] = record.trailing_dword - 3;
            bind_regs = std::max(bind_regs, (size_t)(record.trailing_dword - 2));
        }
    slots.bind_state.assign(bind_regs, 0);
    slots.bind_target.assign(bind_regs, Operand{});
    std::map<uint32_t, uint32_t> slot_type;   /* slot offset -> data type id */
    /* HOW MANY BYTES A TYPED SLOT ACTUALLY HOLDS. A group's declared type gives the
     * size of the VALUE; the slot may hold the value inline or a reference to it,
     * and the graph says which by where it puts the next slot of the same group. A
     * boat's two keyed curves sit 8 bytes apart against a 40-byte type - they are
     * references, and copying 40 bytes over them buried the entity set that starts
     * 16 bytes later (the set's own 260-byte write then buried the curves back).
     * Eight 120-byte tables sit 128 apart and are inline. So the stride bounds the
     * copy; a lone slot in a group keeps the type's own size. */
    /* The bound is not the group's own stride but the next slot ANY record writes:
     * that is where the graph's own layout says this value ends, whichever group the
     * neighbour belongs to. A boat's curve slot is bounded at 8 bytes by its twin,
     * its engine config at 160 by the first table after it, and a 120-byte table
     * keeps its 120 because the next write is 128 further on. */
    /* WRITES only. A record that READS a field inside a struct names a slot offset
     * in the middle of it, and counting those bounds the struct at its first read
     * field - which truncated every car's typed copies. A record's output is its
     * last slot operand, or the ones its own counted lists name. */
    std::set<uint32_t> written_slots;
    for (const Record& rec : graph.records) {
        if (rec.counted_lists) {
            for (uint32_t o = rec.n_in; o < (uint32_t)rec.n_in + rec.n_out &&
                                        o < rec.operands.size(); ++o)
                if (rec.operands[o].region == 2) written_slots.insert(rec.operands[o].offset);
            continue;
        }
        if (const Operand* out = last_slot(rec)) written_slots.insert(out->offset);
    }
    std::map<uint32_t, uint32_t> slot_stride;
    for (const TypedValueGroup& group : graph.slot_values)
        for (uint32_t off : group.offsets) {
            slot_type[off] = group.data_type_id;
            const auto next = written_slots.upper_bound(off);
            if (next != written_slots.end()) slot_stride[off] = *next - off;
        }

    /* AN ACCUMULATOR SEED IS ZERO, AND KNOWN.
     *
     * A slot that NO record in the graph ever writes can hold only what the slot
     * buffer starts with, which is zero. Leaving those reads unknown is conservatism
     * with nothing behind it: there is no producer that could have run too late. A
     * boat pays for it - three of its force sums begin `acc = acc + term` against a
     * slot nothing writes, and with the seed unknown every sum after it is unknown.
     *
     * Only offsets that are never an OUTPUT qualify. One that is written somewhere
     * and merely has not been written yet stays unknown, because that is a real
     * ordering question rather than an empty one.
     *
     * The seed is given AT THE READ rather than up front, because only the read knows
     * how wide the value is. Seeding up front had to guess the width from the
     * distance to the next slot the graph names, and that distance is not the width:
     * it zeroed spans that were really structs - inventing a wheel with no radius and
     * no mass, which sent a boat's acceleration to 1e13 in two ticks - and then, once
     * capped, skipped the isolated accumulators it was added for, which is what kept
     * every helicopter on the ground. A read of no more than sixteen bytes is a value
     * with nowhere else to come from; a wider one is left alone. */
    for (const Record& rec : graph.records)
        for (const Operand& op : rec.operands)
            if (op.region == 2) slots.named_slots.insert(op.offset);
    for (uint32_t off : slots.named_slots)
        if (!written_slots.count(off)) slots.never_written.insert(off);
    /* The byte-level version of the same fact. A record's output span is taken as
     * sixteen bytes from its offset, or its move width where the record states one,
     * which over-covers rather than under-covers the bytes a write can reach - so a
     * byte left outside every span really is one nothing writes. */
    slots.writable.assign(slots.bytes.size(), 0);
    for (const Record& rec : graph.records) {
        const uint32_t mw = move_width(rec);
        auto cover = [&](uint32_t off, uint32_t w) {
            for (uint32_t i = 0; i < w && off + i < slots.writable.size(); ++i)
                slots.writable[off + i] = 1;
        };
        if (rec.counted_lists) {
            for (uint32_t o = rec.n_in; o < (uint32_t)rec.n_in + rec.n_out &&
                                        o < rec.operands.size(); ++o)
                if (rec.operands[o].region == 2) cover(rec.operands[o].offset, 272);
            continue;
        }
        for (const Operand& op : rec.operands)
            if (op.region == 2) cover(op.offset, mw > 16 ? mw : 272);
    }

    std::map<uint32_t, unsigned> back_edges;   /* record offset -> times its back-edge ran */
    const unsigned kMaxLoop = 1024;
    /* record offset -> guessed-branch count when the run first reached it: a loop
     * whose body guessed since its head was first reached is not re-entered */
    std::map<uint32_t, uint32_t> first_guess;
    std::vector<uint32_t> call_stack;     /* return successors for kind 0x2A calls */
    uint32_t returned_to = 0xFFFFFFFFu;   /* successor the last 0x2B popped */
    uint32_t cursor = 0;
    const uint32_t step_limit = std::max<uint32_t>(1u,
        (uint32_t)graph.records.size() * 4u);
    Value last_written;

    size_t trace_filled = instance ? instance->trace.size() : 0;
    while (result.steps++ < step_limit) {
        if (instance && instance->trace_records)
            for (; trace_filled < instance->trace.size(); ++trace_filled) {
                auto& tr = instance->trace[trace_filled];
                if ((size_t)tr.slot < slots.bytes.size() && slots.bytes.size() - (size_t)tr.slot >= 4) {
                    std::memcpy(&tr.bits, slots.bytes.data() + tr.slot, 4);
                    const size_t room = slots.bytes.size() - (size_t)tr.slot;
                    const size_t n = std::min<size_t>(16, std::min<size_t>(room, tr.width ? tr.width : 4));
                    std::memcpy(tr.lanes, slots.bytes.data() + tr.slot, n);
                }
            }
        const auto found = records.find(cursor);
        if (found == records.end()) {
            result.termination = Termination::Complete;
            result.result = last_written;
            return result;
        }
        const Record& record = *found->second;
        uint32_t next = record.next;
        first_guess.emplace(cursor, result.guessed_branches);

        result.last_record = record.offset;
        if (record.kind == 0x2c) {
            result.termination = Termination::Return;
            result.result = last_written;
            return result;
        }
        if (record.kind == 0x26 && !record.operands.empty()) {
            const Value condition = materialize(graph, instance, slots,
                                                record.operands[0], 1);
            if (condition.known) {
                if (!condition.as_bool()) next = record.control_target;
            } else {
                ++result.guessed_branches;
                result.diagnostics.push_back("unknown branch condition; followed fall-through @" + std::to_string(record.offset));
                last_written.tainted = true;
            }
        } else if (record.kind == 0x28 && !record.operands.empty()) {
            const Value selector = materialize(graph, instance, slots,
                                               record.operands[0], 4);
            if (selector.known) {
                const uint32_t label = selector.as_u32();
                for (size_t i = 0; i < record.dispatch_labels.size(); ++i)
                    if (record.dispatch_labels[i] == label) {
                        next = record.dispatch_targets[i];
                        break;
                    }
            } else {
                ++result.guessed_branches;
                result.diagnostics.push_back("unknown dispatch selector; followed merge successor @" + std::to_string(record.offset));
                last_written.tainted = true;
            }
        } else if (record.kind == 0x2a && !record.operands.empty()) {
            /* SUBROUTINE CALL. The first word of the record's pair is a record offset
             * (0x44C in an aim graph is a CrossVec3/Normalize routine ending in 0x2B;
             * 0x5B00 in the flyer60 suspension), and the callers read slots only that
             * routine writes - which is why those slots looked "filled by the engine".
             * Before this, 0x2A fell through and every such slot stayed unknown. */
            /* CONFIRMED FROM THE INTERPRETER (FUN_142484fc0, case 0x2a): a ONE-SHOT
             * CONDITIONAL call. Target u32 at +4; a flag operand at +8 (its region is
             * the parsed pair's second word, its offset the trailing dword). If the
             * flag byte is nonzero: push own-next, CLEAR the flag, jump. Otherwise
             * fall through. The unconditional call is kind 0x29. */
            const uint32_t target = record.operands[0].region;
            const Operand flag{record.operands[0].offset, record.trailing_dword};
            const Value f = materialize(graph, instance, slots, flag, 1);
            bool take = false;
            if (f.known) take = f.bytes[0] != 0;
            else {
                ++result.guessed_branches;
                result.diagnostics.push_back("unknown one-shot call flag; took the call @" + std::to_string(record.offset));
                last_written.tainted = true;
                take = true;
            }
            if (take) {
                if (records.find(target) != records.end() && call_stack.size() < 64) {
                    if (flag.region == 2) slots.write(flag.offset, 1, Value::from_bool(false));
                    call_stack.push_back(record.next);
                    next = target;
                } else {
                    result.diagnostics.push_back("call target is not a record; fell through");
                    last_written.tainted = true;
                }
            }
        } else if (record.kind == 0x2b) {
            /* RETURN from a subroutine. With no caller (a routine reached by falling
             * through) keep the stored successor, as before. */
            if (!call_stack.empty()) {
                next = call_stack.back();
                returned_to = next;
                call_stack.pop_back();
            }
        } else if (record.kind == 0x2e && !record.operands.empty()) {
            /* BIND this record's reference register. Only a reference this VM
             * recorded resolves; anything else is bound-but-opaque, so its reads
             * come back unknown rather than as a stale or invented base. */
            const auto bo = bind_ordinal.find(record.offset);
            if (bo != bind_ordinal.end() && bo->second < slots.bind_state.size()) {
                const size_t k = bo->second;
                const Operand& r = record.operands[0];
                const auto it = r.region == 2 ? slots.refs.find(r.offset) : slots.refs.end();
                uint32_t hb = 0, ho = 0;
                if (it != slots.refs.end()) {
                    slots.bind_state[k] = 1;
                    slots.bind_target[k] = it->second;
                } else if (heap_decode(materialize(graph, instance, slots, r, 8), hb, ho) &&
                           hb < slots.heap.size()) {
                    /* bound to an array element this VM allocated */
                    slots.bind_state[k] = 1;
                    slots.bind_target[k] = Operand{kHeapRegion + hb, ho};
                } else {
                    slots.bind_state[k] = 2;
                }
                if (std::getenv("BF6_MOVE_DEBUG"))
                    std::fprintf(stderr, "bind r%zu <- r%u+%u: state %d target r%u+%u\n",
                                 k + 3, r.region, r.offset, (int)slots.bind_state[k],
                                 slots.bind_target[k].region, slots.bind_target[k].offset);
            }
        } else if (record.kind == 0x27) {
            if (!record.operands.empty()) {
                const Value target = materialize(graph, instance, slots,
                                                 record.operands[0], 4);
                if (target.known) next = target.as_u32();
                else {
                    ++result.approximated_indirect_jumps;
                    result.diagnostics.push_back("unknown indirect jump; followed stored successor");
                }
            }
        } else if (record.operator_key == kParameterValue ||
                   record.operator_key == kParameterObject) {
            const Operand* output = last_slot(record);
            if (output) {
                Value value = argument_index < arguments.size()
                    ? arguments[argument_index++] : unknown(4);
                const uint32_t width = (uint32_t)std::max<size_t>(1, value.bytes.size());
                slots.write(output->offset, width, value);
                last_written = value;
            }
        } else if (record.operator_key == kSink) {
            if (!record.operands.empty()) {
                const Operand& source = record.operands.back();
                uint32_t width = 4;
                const auto wi = slots.widths.find(source.offset);
                if (source.region == 2 && wi != slots.widths.end()) width = wi->second;
                result.result = materialize(graph, instance, slots, source, width);
                last_written = result.result;
            }
        } else if (record.operator_key == kTerminator) {
            result.termination = Termination::Complete;
            result.result = last_written;
            return result;
        } else if (record.operator_key == kVariableStore) {
            if (instance && record.operands.size() >= 2) {
                const Operand* value_operand = nullptr;
                const Operand* tag = nullptr;
                for (const Operand& operand : record.operands) {
                    if (!value_operand && operand.region == 2) value_operand = &operand;
                    if (!tag && operand.region == 0) tag = &operand;
                }
                if (value_operand && tag) {
                    uint32_t width = 4;
                    const auto wi = slots.widths.find(value_operand->offset);
                    if (wi != slots.widths.end()) width = wi->second;
                    instance->variables[tag->offset] =
                        slots.read(value_operand->offset, width);
                }
            }
        } else if (record.kind >= 0x38 && record.kind <= 0x3d && record.operands.size() >= 4) {
            /* FLAG-SELECTED COPY (interpreter cases 0x38-0x3d): cond at +4, source
             * if cond != 0 at +12, source if cond == 0 at +20, destination at +28;
             * width 1/2/4/8/16 by kind, or the trailing dword for 0x38. An unknown
             * condition makes the destination unknown - never a guessed pick. */
            static const uint32_t kSelW[5] = {1, 2, 4, 8, 16};
            const uint32_t width = record.kind == 0x38
                ? (record.has_trailing_dword ? record.trailing_dword : 0u)
                : kSelW[record.kind - 0x39];
            const Operand& dst = record.operands[3];
            if (width && width <= 4096 && dst.region == 2) {
                const Value c = materialize(graph, instance, slots, record.operands[0], 1);
                if (c.known) {
                    const Operand& src = c.bytes[0] ? record.operands[1] : record.operands[2];
                    if (src.region == 2) slots.copy(src.offset, dst.offset, width);
                    else slots.write(dst.offset, width, materialize(graph, instance, slots, src, width));
                    last_written = slots.read(dst.offset, width);
                } else {
                    slots.write(dst.offset, width, unknown(width));
                    last_written.tainted = true;
                }
                if (instance && instance->trace_records)
                    instance->trace.push_back({record.offset, 0u, dst.offset, width, c.known});
                /* BF6_SELECT_DEBUG=<hex dst slot>: this record picks between two
                 * sources on a flag, so a destination stuck at one value means the
                 * FLAG never changed, not that the value was miscomputed. Printing the
                 * condition beside both candidate sources is the only way to tell
                 * those apart. */
                if (const char* want = std::getenv("BF6_SELECT_DEBUG")) {
                    const uint32_t s = (uint32_t)std::strtoul(want, nullptr, 16);
                    if (s == dst.offset) {
                        float got = 0.0f;
                        const Value dv = slots.read(dst.offset, width < 4 ? 4 : width);
                        if (dv.bytes.size() >= 4) std::memcpy(&got, dv.bytes.data(), 4);
                        std::fprintf(stderr,
                                     "select rec 0x%X -> slot 0x%X w %u cond %s%u"
                                     " true=r%u+0x%X false=r%u+0x%X got %g\n",
                                     record.offset, dst.offset, width,
                                     c.known ? "" : "UNKNOWN:", c.known ? c.bytes[0] : 0u,
                                     record.operands[1].region, record.operands[1].offset,
                                     record.operands[2].region, record.operands[2].offset, got);
                    }
                }
            }
        } else if (!record.has_operator && record.kind >= 0x1e && record.kind <= 0x25 &&
                   record.operands.size() >= 2 && record.operands.back().region == 1 &&
                   move_width(record)) {
            /* A MOVE INTO THE INSTANCE (region 1): the graph's persistent per-instance
             * memory. The flyer60 keeps its SpringK / SpringD there (r1+0x28 / +0x2C),
             * written each run and read back through a flag-selected copy. The slot-only
             * path treated the last SLOT operand (the source) as the destination, so
             * the write was lost and the springs read 0. Known values persist in the
             * image (as the engine's instance memory does); every value is also kept
             * for this run, knownness and all. */
            const uint32_t width = move_width(record);
            const Operand& dst = record.operands.back();
            Operand source = record.operands.front();
            if (source.region >= 3 && source.region - 3u < slots.bind_state.size() &&
                slots.bind_state[source.region - 3u] == 1 &&
                (slots.bind_target[source.region - 3u].region < 3 ||
                 slots.bind_target[source.region - 3u].region >= kHeapRegion)) {
                const Operand t = slots.bind_target[source.region - 3u];
                source = Operand{t.region, t.offset + source.offset};
            }
            const Value value = materialize(graph, instance, slots, source, width);
            slots.r1_overlay[dst.offset] = value;
            if (instance && value.known && value.bytes.size() >= width) {
                const size_t base = align16(0x20u + (size_t)graph.header.external_bindings * 8u +
                                            (size_t)graph.header.instance_buffer_count * 4u);
                const size_t at = base + dst.offset;
                if (at + width <= instance->image.size())
                    std::memcpy(instance->image.data() + at, value.bytes.data(), width);
            }
            last_written = value;
        } else if (!record.has_operator && record.kind >= 0x1e &&
                   record.kind <= 0x25) {
            uint32_t width = move_width(record);
            const Operand* output = last_slot(record);
            if (!width && instance && (record.kind == 0x24 || record.kind == 0x25) &&
                record.has_trailing_dword && output) {
                /* TYPED COPY: the destination's type from the slot table, its size
                 * from the caller's reflection.
                 *
                 * The trailing dword is NOT a byte width for this kind. Cars and
                 * tanks always carry zero there and copy whole structs; a boat's
                 * curves carry 5 and 10 against types of 40 and 120 bytes, and taking
                 * those for widths copied five bytes of a forty-byte curve handle, so
                 * the evaluator was handed a struct that was mostly unwritten. The
                 * type's own size is what gets copied either way. */
                const auto ty = slot_type.find(output->offset);
                if (ty != slot_type.end()) {
                    const auto sz = instance->type_sizes.find(ty->second);
                    if (sz != instance->type_sizes.end() && sz->second && sz->second <= 4096)
                        width = sz->second;
                    const auto st = slot_stride.find(output->offset);
                    if (st != slot_stride.end() && st->second && st->second < width)
                        width = st->second;
                    /* A TYPE THE REFLECTION DOES NOT CARRY still has a slot with a
                     * measured end. The boat's engine config is one: without a size
                     * nothing was copied, the reference bound to it read four known
                     * bytes of a hundred and twenty, and the engine's own torque
                     * table came back refused. The graph's next write is where the
                     * value ends, so that is what gets copied. */
                    if (!width && st != slot_stride.end() && st->second &&
                        st->second <= 4096)
                        width = st->second;
                }
                if (std::getenv("BF6_MOVE_DEBUG")) {
                    std::fprintf(stderr, "typed copy @%u src r%u+%u -> slot 0x%X: type %08X width %u\n",
                                 record.offset, record.operands.front().region,
                                 record.operands.front().offset, output->offset,
                                 ty != slot_type.end() ? ty->second : 0u, width);
                    if (width == 40) {
                        const Value probe = materialize(graph, instance, slots,
                                                        record.operands.front(), width);
                        std::fprintf(stderr, "   source:");
                        for (size_t b = 0; b + 3 < probe.bytes.size(); b += 4) {
                            uint32_t w = 0;
                            std::memcpy(&w, probe.bytes.data() + b, 4);
                            std::fprintf(stderr, " %08X", w);
                        }
                        std::fprintf(stderr, " known=%d\n", (int)probe.known);
                    }
                }
            }
            if (width && output && !record.operands.empty()) {
                Operand source = record.operands.front();
                /* Resolve a read through a bound reference to the storage it names,
                 * so a slot-to-slot copy keeps per-byte knownness. */
                if (source.region >= 3 && source.region - 3u < slots.bind_state.size() &&
                    slots.bind_state[source.region - 3u] == 1 &&
                    (slots.bind_target[source.region - 3u].region < 3 ||
                     slots.bind_target[source.region - 3u].region >= kHeapRegion)) {
                    const Operand t = slots.bind_target[source.region - 3u];
                    source = Operand{t.region, t.offset + source.offset};
                }
                Value value = materialize(graph, instance, slots, source, width);
                /* BF6_MOVE_FROM=<hex dst slot>: names the SOURCE a move copied from.
                 * A destination stuck at one value is either a source stuck at that
                 * value or a move reading the wrong place, and only the source's slot
                 * tells those apart. */
                if (const char* want = std::getenv("BF6_MOVE_FROM")) {
                    const uint32_t s = (uint32_t)std::strtoul(want, nullptr, 16);
                    if (s == output->offset) {
                        float g = 0.0f;
                        if (value.bytes.size() >= 4) std::memcpy(&g, value.bytes.data(), 4);
                        std::fprintf(stderr, "move rec 0x%X: slot 0x%X <- r%u+0x%X (w %u) = %g%s\n",
                                     record.offset, output->offset, source.region,
                                     source.offset, width, g, value.known ? "" : " UNKNOWN");
                    }
                }
                if (source.region == 2) slots.copy(source.offset, output->offset, width);
                else slots.write(output->offset, width, value);
                last_written = value;
                if (instance && instance->trace_records)
                    instance->trace.push_back({record.offset, 0u, output->offset, width,
                                               value.known});
            } else if (!width) {
                result.diagnostics.push_back("unmeasured move width withheld");
                last_written.tainted = true;
            }
        } else if (record.kind == 0x2f && record.operands.size() >= 2 &&
                   record.operands[1].region == 2) {
            /* MOVE AN 8-BYTE HANDLE (an array's data pointer) between regions: the
             * array constructor writes region 1, the iteration reads a slot. */
            const Value h = materialize(graph, instance, slots, record.operands[0], 8);
            slots.write(record.operands[1].offset, 8, h);
            last_written = h;
            if (instance && instance->trace_records)
                instance->trace.push_back({record.offset, 0u, record.operands[1].offset, 8, h.known});
        } else if (record.has_operator &&
                   (record.operator_key == 0x8AEFC858u || record.operator_key == 0xA49641E0u ||
                    record.operator_key == 0x013DC8C0u || record.operator_key == 0x75605C5Au)) {
            /* THE ARRAY FAMILY, on the heap above (semantics in the research finding
             * expression-lerp-array-iteration-and-context-nodes). The element type
             * operand is a type descriptor the engine patches in at load; offline the
             * stride is the one the constructor was given for its source. */
            const auto& ops = record.operands;
            const Operand* out_op = nullptr;
            Value out;
            bool ok = false;
            auto u32_of = [&](const Operand& o) -> std::pair<bool, uint32_t> {
                const Value v = materialize(graph, instance, slots, o, 4);
                uint32_t x = 0;
                if (v.known && v.bytes.size() >= 4) std::memcpy(&x, v.bytes.data(), 4);
                return {v.known, x};
            };
            if (record.operator_key == 0x8AEFC858u && ops.size() >= 5) {
                /* (element type, count, source stride, source base) -> data pointer */
                const auto cnt = u32_of(ops[1]);
                auto str = u32_of(ops[2]);
                out_op = &ops[4];
                /* A STRIDE OF ZERO IS THE ENGINE'S JOB, not a refusal. The stride
                 * operand is zero when the element size comes from the type the
                 * engine patches in at load; the width the VM itself recorded for the
                 * source slot is the same number and is measured rather than assumed,
                 * so it stands in when the operand is silent. */
                if (str.first && str.second == 0 && ops[3].region == 2) {
                    const auto w = slots.widths.find(ops[3].offset);
                    if (w != slots.widths.end() && w->second && w->second <= 4096)
                        str.second = w->second;
                }
                if (cnt.first && str.first && cnt.second <= 4096 && str.second && str.second <= 4096) {
                    Slots::Block blk;
                    blk.count = cnt.second;
                    blk.stride = str.second;
                    blk.bytes.assign((size_t)cnt.second * str.second, 0);
                    blk.initialized.assign(blk.bytes.size(), 0);
                    for (uint32_t i = 0; i < cnt.second; ++i) {
                        const Operand src{ops[3].region, ops[3].offset + i * str.second};
                        const Value e = materialize(graph, instance, slots, src, str.second);
                        for (uint32_t b = 0; b < str.second && b < e.bytes.size(); ++b) {
                            blk.bytes[(size_t)i * str.second + b] = e.bytes[b];
                            blk.initialized[(size_t)i * str.second + b] = e.known ? 1 : 0;
                        }
                    }
                    slots.heap.push_back(std::move(blk));
                    out = value_u64(heap_ptr((uint32_t)slots.heap.size() - 1, 0));
                    ok = true;
                }
            } else if (record.operator_key == 0xA49641E0u && ops.size() >= 4) {
                /* (array, element type) -> begin, end */
                uint32_t b = 0, o = 0;
                if (heap_decode(materialize(graph, instance, slots, ops[0], 8), b, o) &&
                    b < slots.heap.size() && ops[2].region == 2 && ops[3].region == 2) {
                    const auto& blk = slots.heap[b];
                    slots.write(ops[2].offset, 8, value_u64(heap_ptr(b, 0)));
                    out_op = &ops[3];
                    out = value_u64(heap_ptr(b, blk.count * blk.stride));
                    ok = true;
                } else if (ops.size() >= 4 && ops[2].region == 2) {
                    slots.write(ops[2].offset, 8, unknown(8));
                    out_op = &ops[3];
                }
            } else if (record.operator_key == 0x013DC8C0u && ops.size() >= 3) {
                /* (iterator, element type) -> iterator + stride */
                uint32_t b = 0, o = 0;
                out_op = &ops[2];
                if (heap_decode(materialize(graph, instance, slots, ops[0], 8), b, o) && b < slots.heap.size()) {
                    out = value_u64(heap_ptr(b, o + slots.heap[b].stride));
                    ok = true;
                }
            } else if (record.operator_key == 0x75605C5Au && ops.size() >= 2) {
                /* length: null -> 0, else the count */
                uint32_t b = 0, o = 0;
                out_op = &ops[1];
                const Value p = materialize(graph, instance, slots, ops[0], 8);
                uint64_t raw = 0;
                if (p.known && p.bytes.size() >= 8) std::memcpy(&raw, p.bytes.data(), 8);
                if (p.known && raw == 0) { out = Value::from_u32(0); ok = true; }
                else if (heap_decode(p, b, o) && b < slots.heap.size()) {
                    out = Value::from_u32(slots.heap[b].count);
                    ok = true;
                }
            }
            if (out_op) {
                const uint32_t w = record.operator_key == 0x75605C5Au ? 4u : 8u;
                if (!ok) { out = unknown(w); add_unresolved(result, record.operator_key); last_written.tainted = true; }
                if (out_op->region == 2) slots.write(out_op->offset, w, out);
                else if (out_op->region == 1) slots.r1_overlay[out_op->offset] = out;
                last_written = out;
                if (instance && instance->trace_records)
                    instance->trace.push_back({record.offset, record.operator_key,
                                               out_op->region == 2 ? out_op->offset : 0xFFFFFFFFu, w, out.known});
            } else {
                add_unresolved(result, record.operator_key);
                last_written.tainted = true;
            }
        } else if (record.operator_key) {
            OperatorSignature signature;
            /* PER-CALL DESCRIPTION. Some engine readers return a different WIDTH per
             * call, selected by a constant operand: 0x04BEFF62 returns a 64-byte
             * LinearTransform when its operand 0 is 0 and a 4-byte float when it is 1
             * (measured across every vehicle graph, by what each call's output is
             * consumed as). describe(key) cannot express that, so the call's region-0
             * operands are read from the constant pool and offered to the host;
             * 0xFFFFFFFF marks an operand that is not a pool constant. A host that
             * does not care inherits describe(key) unchanged.
             *
             * NOTE: this is a deliberate divergence from the research repo's
             * expression_vm.cpp, which was byte-identical until this change. */
            std::vector<uint32_t> call_consts;
            call_consts.reserve(record.operands.size());
            for (const Operand& op : record.operands) {
                uint32_t v = 0xFFFFFFFFu;
                if (op.region == 0 && op.offset + 4 <= graph.constant_pool.size()) {
                    std::memcpy(&v, graph.constant_pool.data() + op.offset, 4);
                    if (instance) {
                        const auto pp = instance->pool_patches.find(op.offset);
                        if (pp != instance->pool_patches.end()) v = pp->second;
                    }
                }
                call_consts.push_back(v);
            }
            const bool described = host &&
                host->describe_call(record.operator_key, call_consts, signature);
            const EffectiveCall call = described
                ? effective_call(record, signature, &slots) : EffectiveCall{};
            if (!described || call.inputs.size() != signature.input_widths.size() ||
                (signature.output_width && !call.output)) {
                add_unresolved(result, record.operator_key);
                last_written.tainted = true;
                /* A refused operator leaves its output slot UNWRITTEN, which is where
                 * an unknown is born - so trace it, against the slot it would have
                 * written (the last region-2 operand), as unknown. */
                if (instance && instance->trace_records) {
                    const Operand* ls = last_slot(record);
                    instance->trace.push_back({record.offset, record.operator_key,
                                               ls ? ls->offset : 0xFFFFFFFFu, 0u, false});
                    /* A counted-list call names its outputs: every one of them is
                     * left unwritten, not only the last. */
                    if (record.counted_lists)
                        for (uint32_t o = record.n_in; o < (uint32_t)record.n_in + record.n_out &&
                                                       o < record.operands.size(); ++o)
                            if (record.operands[o].region == 2 && &record.operands[o] != ls)
                                instance->trace.push_back({record.offset, record.operator_key,
                                                           record.operands[o].offset, 0u, false});
                }
            } else if (signature.output_is_reference_to_input0 && call.output &&
                       signature.output_width && !call.inputs.empty() &&
                       call.inputs[0]->region <= 2) {
                /* A reference, not a value: remember what it points at. The slot
                 * gets known zero bytes so width bookkeeping stays honest, but the
                 * meaning lives in slots.refs. */
                Value ref;
                ref.bytes.assign(signature.output_width, 0);
                ref.known = true;
                slots.write(call.output->offset, signature.output_width, ref);
                Operand target = *call.inputs[0];
                /* A reference to one FIELD: the byte offset comes from the second
                 * operand, which for the field-address operator is a pool word the
                 * caller has patched with that field's offset. An unknown offset
                 * leaves the reference pointing at the object's start, which is what
                 * it meant before this existed. */
                if (signature.reference_offset_from_input1 && call.inputs.size() > 1) {
                    const Value off = materialize(graph, instance, slots,
                                                  *call.inputs[1], 4);
                    if (off.known) target.offset += off.as_u32();
                }
                slots.refs[call.output->offset] = target;
                last_written = ref;
                if (instance && instance->trace_records)
                    instance->trace.push_back({record.offset, record.operator_key,
                                               call.output->offset,
                                               signature.output_width, true});
            } else {
                std::vector<Value> args;
                for (size_t i = 0; i < call.inputs.size(); ++i)
                    args.push_back(materialize(graph, instance, slots,
                                               *call.inputs[i],
                                               signature.input_widths[i]));
                /* BF6_OP_INPUTS=<hex key>: every call of that operator, with each
                 * input's region, slot, first float, and - the point of it - whether
                 * the slot is one NO record writes. A state input on such a slot reads
                 * a known zero for ever, which looks exactly like a physical result
                 * (a wheel that never turns) rather than a wiring fault. */
                if (const char* want = std::getenv("BF6_OP_INPUTS")) {
                    const uint32_t k = (uint32_t)std::strtoul(want, nullptr, 16);
                    if (k == record.operator_key) {
                        std::fprintf(stderr, "op %08X rec 0x%X inputs:", k, record.offset);
                        for (size_t i = 0; i < call.inputs.size(); ++i) {
                            const bool r2 = call.inputs[i]->region == 2;
                            const bool nw = r2 && slots.never_written.count(call.inputs[i]->offset);
                            /* BY WIDTH. Printing a float from anything narrower than
                             * four bytes reported every one-byte flag as 0, and a flag
                             * written TRUE then read as 0 sent a whole investigation
                             * the wrong way. A narrow value prints as its integer. */
                            char v[96] = "?";
                            /* A 16-byte operand is a state DESCRIPTOR, and its four
                             * dwords are what the cell key is built from, so print it
                             * whole: the first lane alone cannot identify a cell. */
                            if (args[i].bytes.size() == 16) {
                                uint32_t d[4];
                                std::memcpy(d, args[i].bytes.data(), 16);
                                std::snprintf(v, sizeof v, "desc[%08X %08X %08X %08X]",
                                              d[0], d[1], d[2], d[3]);
                            } else if (args[i].bytes.size() >= 4) {
                                float g = 0.0f;
                                std::memcpy(&g, args[i].bytes.data(), 4);
                                std::snprintf(v, sizeof v, "%g", g);
                            } else if (!args[i].bytes.empty()) {
                                uint32_t u = 0;
                                std::memcpy(&u, args[i].bytes.data(), args[i].bytes.size());
                                std::snprintf(v, sizeof v, "%u(w%zu)", u, args[i].bytes.size());
                            }
                            std::fprintf(stderr, " [%zu]r%u+0x%X=%s%s", i,
                                         call.inputs[i]->region, call.inputs[i]->offset, v,
                                         nw ? " NOBODY-WRITES" : "");
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
                if (record.operator_key == 0x9afb0561u && std::getenv("BF6_MOVE_DEBUG")) {
                    std::fprintf(stderr, "curve call @%u:", record.offset);
                    for (size_t i = 0; i < call.inputs.size(); ++i)
                        std::fprintf(stderr, " r%u+%u(%u)", call.inputs[i]->region,
                                     call.inputs[i]->offset, signature.input_widths[i]);
                    std::fprintf(stderr, "\n");
                }
                Value value;
                if (host->invoke(record.operator_key, args, value)) {
                    /* THE FIRST NON-FINITE ANSWER is the one worth seeing: everything
                     * after it is downstream of the same mistake. */
                    if (std::getenv("BF6_NAN_DEBUG") && value.known &&
                        value.bytes.size() >= 4) {
                        for (size_t w = 0; w + 4 <= value.bytes.size(); w += 4) {
                            float f = 0.0f;
                            std::memcpy(&f, value.bytes.data() + w, 4);
                            if ((std::isfinite(f) && std::fabs(f) < 1e7f) || f == 0.0f) continue;
                            std::fprintf(stderr, "non-finite: rec 0x%X key %08X lane %zu = %g, inputs:",
                                         record.offset, record.operator_key, w / 4, f);
                            for (size_t i = 0; i < args.size(); ++i) {
                                float g = 0.0f;
                                if (args[i].bytes.size() >= 4)
                                    std::memcpy(&g, args[i].bytes.data(), 4);
                                std::fprintf(stderr, " r%u+%u=%g%s", call.inputs[i]->region,
                                             call.inputs[i]->offset, g,
                                             args[i].known ? "" : "?");
                            }
                            std::fprintf(stderr, "\n");
                            break;
                        }
                    }
                    for (const Value& arg : args)
                        value.tainted = value.tainted || arg.tainted || !arg.known;
                    if (call.output && signature.output_width) {
                        uint32_t total = signature.output_width;
                        for (uint32_t w : signature.extra_output_widths) total += w;
                        /* A short answer: extras the host did not supply are unknown;
                         * a primary it did not fully supply makes everything unknown. */
                        const size_t got = value.bytes.size();
                        if (got < signature.output_width) value = unknown(total);
                        const bool extras_known = got >= total && value.known;
                        if (value.bytes.size() < total) value.bytes.resize(total, 0);
                        uint32_t at = signature.output_width;
                        for (size_t e = 0; e < call.extra_outputs.size(); ++e) {
                            const uint32_t w = signature.extra_output_widths[e];
                            Value part;
                            part.bytes.assign(value.bytes.begin() + at, value.bytes.begin() + at + w);
                            part.known = extras_known;
                            part.tainted = value.tainted || !extras_known;
                            slots.write(call.extra_outputs[e]->offset, w, part);
                            if (instance && instance->trace_records)
                                instance->trace.push_back({record.offset, record.operator_key,
                                                           call.extra_outputs[e]->offset, w,
                                                           part.known});
                            at += w;
                        }
                        value.bytes.resize(signature.output_width);
                        slots.write(call.output->offset, signature.output_width, value);
                        last_written = value;
                        if (instance && instance->trace_records)
                            instance->trace.push_back({record.offset, record.operator_key,
                                                       call.output->offset,
                                                       signature.output_width, value.known});
                    }
                } else {
                    if (instance && instance->trace_records) {
                        const Operand* ls = call.output ? call.output : last_slot(record);
                        instance->trace.push_back({record.offset, record.operator_key,
                                                   ls ? ls->offset : 0xFFFFFFFFu, 0u, false});
                        /* its second outputs are left unwritten too */
                        for (const Operand* eo : call.extra_outputs)
                            if (eo && eo->region == 2)
                                instance->trace.push_back({record.offset, record.operator_key,
                                                           eo->offset, 0u, false});
                    }
                    add_unresolved(result, record.operator_key);
                    std::string diagnostic = "host invoke failed at record " +
                        std::to_string(record.offset) + " key " +
                        std::to_string(record.operator_key) + " inputs";
                    for (const Value& arg : args) {
                        diagnostic += arg.known ? " known" : " unknown";
                        if (arg.tainted) diagnostic += "/tainted";
                        diagnostic += "/" + std::to_string(arg.bytes.size());
                    }
                    result.diagnostics.push_back(diagnostic);
                    last_written.tainted = true;
                }
            }
        }

        /* Successors run forward, EXCEPT a subroutine return, which goes back to its
         * caller by construction; the step limit still bounds the run. */
        const bool returning = record.kind == 0x2b && next == returned_to;
        /* LOOPS. A backward successor is a loop's back-edge (the flyer60 walks its
         * wheel array at 0x79C8-0x7ACC and only then reaches the wheel physics).
         * Ending the run there skipped a third of the graph. A back-edge is followed
         * while its loop's decisions are known; one taken after a GUESSED branch, or
         * more than kMaxLoop times, ends the run as before. */
        bool loop_ok = false;
        if (next && next <= cursor && !returning && records.find(next) != records.end()) {
            unsigned& n = back_edges[record.offset];
            const auto fg = first_guess.find(next);
            /* A backward jump to a record this run has NOT reached yet is out-of-order
             * layout, not a loop (the flyer60 branches forward to a test block at
             * 0x446C that jumps back to 0x2E8C): always followed. A jump back to a
             * record already run is a loop, followed under the guess rule and cap. */
            loop_ok = ++n <= kMaxLoop &&
                      (fg == first_guess.end() || result.guessed_branches == fg->second);
        }
        if (!next || (next <= cursor && !returning && !loop_ok) || records.find(next) == records.end()) {
            result.termination = Termination::Complete;
            if (!result.result.known && result.result.bytes.empty()) result.result = last_written;
            return result;
        }
        cursor = next;
    }
    result.termination = Termination::StepLimit;
    result.result = last_written;
    result.result.tainted = true;
    result.diagnostics.push_back("step limit reached");
    return result;
}

}} // namespace bf6::expression
