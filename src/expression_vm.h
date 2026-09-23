#ifndef BF6_EXPRESSION_VM_H
#define BF6_EXPRESSION_VM_H

#include "expression_graph.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bf6 { namespace expression {

struct Value {
    std::vector<uint8_t> bytes;
    bool known = false;
    bool tainted = false;
    /* PER-BYTE knownness, when the value was read from the slot file: a struct a
     * graph assembles field by field (a mirrored WheelConfig) leaves its padding
     * unwritten, and `known` is then false although every field a native reads is
     * set. Empty means "all bytes as `known` says". */
    std::vector<uint8_t> known_bytes;

    static Value from_u32(uint32_t value);
    static Value from_bool(bool value);
    uint32_t as_u32() const;
    bool as_bool() const;
};

struct OperatorSignature {
    std::vector<uint32_t> input_widths;
    uint32_t output_width = 0; // zero means a side-effect-only call
    /* SECOND OUTPUTS. Slot operands immediately BEFORE the primary output that the
     * operator writes rather than reads, in operand order. The host returns their
     * bytes after the primary output's in Value::bytes. Measured need: the wheel
     * raycast 0x040F4924 writes a hit bool (a byte-aligned slot the next branch
     * tests and nothing else writes) as well as its PhysicsQueryResult. */
    std::vector<uint32_t> extra_output_widths;
    /* A FIELD REFERENCE. The output is not a value but a reference to input 0's
     * storage; the VM records where it points (host invoke is not called) and a
     * later kind 0x2E record binds it, after which region-3 move sources read
     * relative to it. Measured need: 0x532B3BA9 (object, 0) -> 0x2E -> a 16-byte
     * move from r3+0x0 is how the suspension reads HitPoint out of the ray result. */
    bool output_is_reference_to_input0 = false;
    /* AND THE REFERENCE MAY BE INTO THE MIDDLE OF IT. 0x88030F01 is nine bytes of
     * machine code - `mov eax,[rdx]; add rax,rcx; mov [r8],rax` - so it returns its
     * object's address PLUS a byte offset it reads from its second operand. That is a
     * reference to one FIELD, where 0x532B3BA9 returns one to the whole object. */
    bool reference_offset_from_input1 = false;
};

/* HOW A HOST HANDS BACK AN ARRAY. Some natives return no value: they grow an engine
 * vector and leave a pointer to it. The tank's track sampler is one, writing a
 * contact per road wheel, which the graph then walks with the array operators. A
 * host has no access to the evaluator's heap, so the evaluator lends it this.
 *
 * Deliberately narrow: not a general allocator, and nothing frees. A run is one
 * tick and the heap goes with it. */
class HeapSink {
public:
    virtual ~HeapSink() = default;
    virtual bool alloc(uint32_t count, uint32_t stride, const uint8_t* bytes,
                       Value& out) = 0;
    /* And back the other way: a host handed a pointer operand reads the block it
     * points at. The bytes stay owned by the evaluator and are valid for the call. */
    virtual bool read(const Value& ptr, uint32_t& count, uint32_t& stride,
                      const uint8_t*& bytes) = 0;
    /* THE CONSTANT POOL, which a host cannot otherwise reach. A curve is a struct
     * whose key array lives elsewhere in the pool and is reached through a pointer
     * field the loader relocates; the host has the struct and needs the keys. The
     * offset is a pool offset, which is what a relocated pointer field holds
     * offline. */
    virtual bool pool(uint32_t offset, uint32_t width, std::vector<uint8_t>& out) = 0;
};

class Host {
public:
    virtual ~Host() = default;
    /* Lent by the evaluator for the length of a run, null outside one. A host that
     * never returns an array can ignore it. */
    virtual void set_heap_sink(HeapSink* sink) { (void)sink; }
    // Width and arity are part of the ABI. Refusing a description is safer
    // than reading every bare operand as a float-sized value.
    virtual bool describe(uint32_t key, OperatorSignature& out) = 0;
    virtual bool invoke(uint32_t key, const std::vector<Value>& args,
                        Value& out) = 0;
    /* The same question for ONE CALL, with that call's constant-pool operand values
     * (0xFFFFFFFF where an operand is not a pool constant). Needed for engine readers
     * whose output width depends on a constant operand. Defaults to describe(key),
     * so every host that does not need it behaves exactly as before. */
    virtual bool describe_call(uint32_t key, const std::vector<uint32_t>& consts,
                               OperatorSignature& out) {
        (void)consts;
        return describe(key, out);
    }
};

/* Pure named operators keyed by names recovered from the current executable.
 * Registering a name does not assert support: describe() returns false for
 * state-backed names such as tweakable reads. No copied key table is used.
 */
class NamedBuiltins final : public Host {
public:
    void add(uint32_t key, const std::string& current_exe_name);
    bool describe(uint32_t key, OperatorSignature& out) override;
    bool invoke(uint32_t key, const std::vector<Value>& args,
                Value& out) override;
private:
    std::map<uint32_t, std::string> names_;
};

struct Instance {
    const Graph* graph = nullptr;
    std::vector<uint8_t> image;
    std::map<uint32_t, Value> variables;
    /* SLOT CONTENTS THE CALLER PROVIDES BEFORE THE GRAPH RUNS, keyed by slot offset.
     *
     * Measured on the vehicle suspension graphs: the wheel point that the ray-origin
     * multiply reads is written by NO record in the graph - not an operator, not a
     * move, not the VM's own Parameter nodes, and not later in a loop (26 of 26
     * "written nowhere"). The engine puts it in the slot file before execution. This is
     * where an offline caller does the same. Empty by default, so nothing changes for
     * a caller that does not use it. */
    std::map<uint32_t, Value> slot_seed;
    /* OPT-IN EXECUTION TRACE, for finding the first unknown value at RUNTIME.
     *
     * Static tracing over record order cannot say what wrote a slot before it was read,
     * because execution follows jumps and loops rather than record order. When set,
     * evaluate() appends one row per record that writes a slot: which record, which
     * operator, which slot, how wide, and whether the value written was known. Off by
     * default and costs nothing when off. */
    struct TraceRow {
        uint32_t record_offset = 0;
        uint32_t key = 0;
        uint32_t slot = 0;
        uint32_t width = 0;
        bool known = false;
        uint32_t bits = 0;      /* first 4 bytes written, filled before the next record */
        /* ALL FOUR LANES, for the same reason a whole-frame aggregate is useless: a
         * vehicle accelerating along Z writes zero into lane 0, so a trace that shows
         * only `bits` reports every force on the forward axis as nothing. */
        uint32_t lanes[4] = {0, 0, 0, 0};
    };
    /* DATA TYPE SIZES by type id (a reflected name hash), from the caller's own
     * reading of the game executable (bf6_type_size_by_hash). A kind 0x24/0x25 move
     * whose trailing width is 0 is a TYPED copy: its width is the size of the type
     * the graph's slot table gives its destination (1,985 of 2,102 such moves in
     * the vehicle graphs; ExpressionBoneId 0x444908CB = 16 is most of them). A type
     * missing here leaves that copy unperformed, as before. */
    std::map<uint32_t, uint32_t> type_sizes;
    /* CONSTANT-POOL PATCHES, byte offset -> u32, laid over the pool for every read.
     * The engine writes channel handles into zero-on-disk pool entries at load
     * (bf6_expression_channel_bindings names which channel goes where); offline the
     * caller writes the channel HASH there, so a channel operator's operand 0 says
     * which channel it means. */
    std::map<uint32_t, uint32_t> pool_patches;
    bool trace_records = false;
    std::vector<TraceRow> trace;
};

enum class Termination {
    Complete,
    Return,
    StepLimit,
    UntiledGraph,
    InvalidGraph
};

struct Evaluation {
    Termination termination = Termination::InvalidGraph;
    Value result;
    uint32_t steps = 0;
    /* Where the run ended: the record that returned, or the last one reached. */
    uint32_t last_record = 0;
    uint32_t guessed_branches = 0;
    uint32_t approximated_indirect_jumps = 0;
    std::vector<uint32_t> unresolved_keys;
    std::vector<std::string> diagnostics;
};

bool make_instance(const Graph& graph, Instance& out, std::string& error);

/* Execute only a graph whose record region was tiled exactly. Operator calls
 * require a host-supplied exact signature; unknown operators/widths are
 * externalized in Evaluation and taint forwarded values. No neutral defaults
 * are fabricated by this core.
 */
Evaluation evaluate(const Graph& graph, Instance* instance,
                    const std::vector<Value>& arguments, Host* host);

}} // namespace bf6::expression

#endif
