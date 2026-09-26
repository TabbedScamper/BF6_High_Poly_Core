#ifndef BF6_EXPRESSION_PURE_OPS_H
#define BF6_EXPRESSION_PURE_OPS_H

/* THE PURE OPERATORS NamedBuiltins DOES NOT IMPLEMENT.
 *
 * `NamedBuiltins` covers about 25 names. Measured across every vehicle graph in the
 * install, 99 more named operators are used and never described - 2,065 blocked uses
 * - and almost all of them are ordinary scalar and vector maths: SubtractFloat,
 * ClampFloat, MinFloat, MaxFloat, MagnitudeFloat3, NormalizeFloat3 and so on.
 *
 * This is a SEPARATE host rather than more branches inside NamedBuiltins, because
 * `expression_vm.cpp` is currently byte-identical to the research copy and keeping
 * it that way is worth more than the convenience of one if-chain.
 *
 * WHERE THE ARITY COMES FROM. Not from reading the names. Every record carries its
 * own operand count, and collecting (name -> operand counts) over all vehicle graphs
 * showed 154 of 155 names carry exactly ONE count, with `operands = inputs + 1`:
 * the output slot is an operand. So `SubtractFloat` has 3 operands and 2 inputs,
 * `NegateFloat` 2 and 1, `ClampFloat` 4 and 3. Every arity below is that measured
 * number, not a guess from the spelling.
 *
 * WHERE THE WIDTHS COME FROM. The name states its types in order, inputs first and
 * the result last: `MultiplyFloat3FloatFloat3` is (Float3, Float) -> Float3, and
 * `MultiplyFloatFloat3Float3` is (Float, Float3) -> Float3. That convention is
 * consistent with the ones NamedBuiltins already implements
 * (`MultiplyFloatFloatFloat`, `DivideIntIntInt`), which is the cross-check.
 *
 * WHAT IS DELIBERATELY ABSENT. The `__Dice...Prepare*Ex` feature-node prologues,
 * `__queueAsyncPhysicsRayQuery*` and `__consumeAsyncPhysicsQueryNode`, and
 * `__GetTweakableFloat`. Those are host SERVICES, not maths - the async physics pair
 * is the wheel raycast - and guessing them would be inventing behaviour rather than
 * implementing it. The LinearTransform family is also left out until its byte layout
 * is measured rather than assumed.
 */

#include "env_cache.h"
#include "expression_vm.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace bf6 { namespace expression {

class PureOps final : public Host {
public:
    /* Same contract as NamedBuiltins: a key is served only once its name has been
     * recovered from the executable and that match was unique. */
    void add(uint32_t key, const std::string& current_exe_name);

    bool describe(uint32_t key, OperatorSignature& out) override;
    bool invoke(uint32_t key, const std::vector<Value>& args,
                Value& out) override;

    /* How many distinct names this host actually served, for measurement. */
    const std::map<std::string, uint32_t>& served() const { return served_; }
    /* Which record is being served, so a rule can be bisected to one call site. */
    void set_current_record(uint32_t record_offset) override { cur_record_ = record_offset; }

private:
    const std::string* name_for(uint32_t key) const;

    std::map<uint32_t, std::string> names_;
    std::map<std::string, uint32_t> served_;
    uint32_t cur_record_ = 0;
};

/* OPERATORS RECOVERED FROM THE EXECUTABLE'S OWN CODE, keyed by operator key.
 *
 * These have no recoverable name, so PureOps cannot serve them. Each was read from
 * its implementation in the installed build (6a1c1b): the reflected registry's call
 * thunk (descriptor +0x28, thunk(ctx, outputs[], inputs[])) or the engine-node
 * table's code pointer (the qword BEFORE the key), decompiled or, where Ghidra made
 * no function, disassembled. Only definitions that are pure and exact are here; the
 * evidence for each is beside it in the .cpp. Record operand order for a reflected
 * operator is inputs then outputs, in thunk order. */
class RecoveredOps final : public Host {
public:
    bool describe(uint32_t key, OperatorSignature& out) override;
    /* Per-call widths for operators whose arity is a pool constant (0xBD059C86). */
    bool describe_call(uint32_t key, const std::vector<uint32_t>& consts,
                       OperatorSignature& out) override;
    bool invoke(uint32_t key, const std::vector<Value>& args, Value& out) override;
    const std::map<uint32_t, uint32_t>& served() const { return served_; }
    /* The clutch-and-gear machine's direction byte (0xAFFD9D93, FUN_1443E6CB0) from its
     * last call: 1 while the drivetrain is in reverse mode, 0 forward, -1 never called. */
    int reverse_mode() const { return reverse_mode_; }
private:
    std::map<uint32_t, uint32_t> served_;
    int reverse_mode_ = -1;
};

/* Chain several hosts. The VM takes one, and a vehicle graph needs the pure
 * operators, these extra ones, and the state family. First to describe a key wins. */
class ChainHost final : public Host {
public:
    void add(Host* host) { if (host) hosts_.push_back(host); }
    /* Passed straight through: the evaluator lends the sink to the chain, and the
     * chain is only a router. */
    void set_heap_sink(HeapSink* sink) override {
        for (Host* h : hosts_) h->set_heap_sink(sink);
    }
    /* Passed straight through for the same reason: the chain is only a router. */
    void set_current_record(uint32_t record_offset) override {
        for (Host* h : hosts_) h->set_current_record(record_offset);
    }
    bool describe(uint32_t key, OperatorSignature& out) override;
    bool describe_call(uint32_t key, const std::vector<uint32_t>& consts,
                       OperatorSignature& out) override {
        /* BF6_OPS_OFF=<hex>,<hex>: refuse these keys outright, to attribute a change
         * to the operator that caused it. */
        if (const char* off = bf6_env("BF6_OPS_OFF"))
            for (const char* p = off; *p;) {
                char* end = nullptr;
                const unsigned long k = std::strtoul(p, &end, 16);
                if (end == p) break;
                if ((uint32_t)k == key) { out = OperatorSignature{}; return false; }
                p = *end ? end + 1 : end;
            }
        for (size_t i = 0; i < hosts_.size(); ++i)
            if (hosts_[i]->describe_call(key, consts, out)) {
                /* BF6_WHO_DESCRIBES=<hex key>: which host in the chain claims it */
                if (const char* w = bf6_env("BF6_WHO_DESCRIBES"))
                    if (std::strtoul(w, nullptr, 16) == key)
                        std::fprintf(stderr, "key %08X described by host %zu: %zu inputs, out %u\n",
                                     key, i, out.input_widths.size(), out.output_width);
                return true;
            }
        out = OperatorSignature{};
        return false;
    }
    bool invoke(uint32_t key, const std::vector<Value>& args,
                Value& out) override;

private:
    std::vector<Host*> hosts_;
};

}} // namespace bf6::expression

#endif
