#include "expression_graph.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace bf6 { namespace expression {
namespace {

static uint16_t u16(const uint8_t* p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t u32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t u64(const uint8_t* p)
{
    return (uint64_t)u32(p) | ((uint64_t)u32(p + 4) << 32);
}

static bool add_ok(size_t a, size_t b, size_t& out)
{
    if (b > std::numeric_limits<size_t>::max() - a) return false;
    out = a + b;
    return true;
}

static bool mul_ok(size_t a, size_t b, size_t& out)
{
    if (a && b > std::numeric_limits<size_t>::max() / a) return false;
    out = a * b;
    return true;
}

static size_t align16(size_t v)
{
    return (v + 15u) & ~size_t(15u);
}

static bool parse_typed_table(const uint8_t* data, size_t begin, size_t bytes,
                              uint32_t destination_size,
                              std::vector<TypedValueGroup>& out,
                              std::string& error)
{
    const size_t end = begin + bytes;
    size_t at = begin;
    while (at < end) {
        if (end - at < 0x28) {
            error = "typed-value table ends inside a group header";
            return false;
        }
        // Constructor/context and destructor/context are relocation holes on
        // disk. A non-zero pointer is evidence that this is not the raw image.
        if (u64(data + at + 4) || u64(data + at + 0x0c) ||
            u64(data + at + 0x14) || u64(data + at + 0x1c)) {
            error = "typed-value group contains a patched runtime pointer";
            return false;
        }
        const uint32_t count = u32(data + at + 0x24);
        size_t offset_bytes = 0;
        if (!mul_ok((size_t)count, 4, offset_bytes) ||
            offset_bytes > end - at - 0x28) {
            error = "typed-value offset count exceeds its declared table";
            return false;
        }
        TypedValueGroup group;
        group.data_type_id = u32(data + at);
        group.offsets.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t off = u32(data + at + 0x28 + (size_t)i * 4);
            if (off >= destination_size && destination_size != 0) {
                error = "typed-value offset exceeds its destination region";
                return false;
            }
            group.offsets.push_back(off);
        }
        out.push_back(std::move(group));
        at += 0x28 + offset_bytes;
    }
    if (at != end) {
        error = "typed-value table did not walk to its declared end";
        return false;
    }
    return true;
}

// Fixed lengths measured without exception in the published 379-graph
// independent corpus. Unmeasured kinds deliberately return zero.
//
// THE SECOND GROUP BELOW WAS MEASURED DIFFERENTLY, and the difference matters.
// These eight kinds appear ONLY in graphs that fail to tile, so no proven
// length existed for them - the usual ground truth was missing. When tiling
// fails the parser falls back to DISCOVERED record starts (control-flow targets
// and fixup sites), and a record's reported length becomes the gap to the next
// discovered start. A gap can span several records where discovery missed one,
// so it is an upper bound and never an under-estimate; the MINIMUM gap is
// therefore the candidate length. For every one of these, minimum and median
// agree across the whole shipped corpus:
//
//     kind  samples  min = median
//     0x14      59       28          0x1F    2316       20
//     0x16       3       44          0x33      16       24
//     0x17      38       52          0x37     201        4
//     0x1A      17       36          0x3C      27       36
//
// This is weaker evidence than the arity rule, which comes from the game's own
// reflection. It is self-checking though: the tiler must reach the region end
// exactly AND hit every mandatory start, so a wrong length here makes graphs
// FAIL to tile rather than silently mis-read - and the corpus test's corruption
// controls must keep rejecting at 100%.
static uint32_t proven_fixed_length(uint8_t k)
{
    /* 0x00..0x13: one operator pointer and k+1 operand pairs, 20 + 8k (interpreter
     * cases 0x00-0x13; 0x0a-0x13 were missing and are measured in the same table). */
    if (k <= 0x13) return 20u + 8u * (uint32_t)k;
    switch (k) {
    case 0x14: return 28; case 0x16: return 44; case 0x17: return 52;
    case 0x1a: return 36; case 0x1f: return 20; case 0x33: return 24;
    case 0x37: return 4;  case 0x3c: return 36;
    case 0x15: return 36; case 0x18: return 28; case 0x19: return 36;
    case 0x1b: return 44; case 0x1c: return 44; case 0x1d: return 52;
    case 0x1e: return 20; case 0x20: return 20; case 0x21: return 20;
    case 0x22: return 20;
    case 0x24: return 24; case 0x25: return 24; case 0x26: return 16;
    case 0x27: return 12; case 0x2a: return 16;
    case 0x2b: return 4;  case 0x2c: return 4;  case 0x2d: return 4;
    case 0x2e: return 16; case 0x2f: return 20; case 0x30: return 28;
    case 0x31: return 28; case 0x32: return 24; case 0x34: return 24;
    case 0x35: return 24; case 0x36: return 24; case 0x38: return 40;
    case 0x39: return 36; case 0x3b: return 36; case 0x3d: return 36;
    case 0x3e: return 40;
    case 0x3a: return 36;                         /* flag-selected 2-byte copy */
    case 0x29: return 8;                          /* unconditional call */
    default: return 0;
    }
}

static bool record_target_ok(uint32_t target, size_t region_bytes)
{
    return (target & 3u) == 0 && target < region_bytes;
}

static void add_control_targets(const uint8_t* region, size_t region_bytes,
                                uint32_t off, std::set<uint32_t>& pending)
{
    if (!record_target_ok(off, region_bytes) || region_bytes - off < 4) return;
    const uint32_t h = u32(region + off);
    const uint8_t k = (uint8_t)(h & 0xffu);
    const uint32_t next = h >> 8;
    /* A backward target is as much a record start as a forward one, so the old
     * `next > off` guard was discarding loop edges for no reason. MEASURED AS
     * NEUTRAL on the shipped corpus - every such target was already reachable
     * by another path - so this buys no coverage; it is kept only because the
     * restriction was not correct to begin with. */
    if (record_target_ok(next, region_bytes)) pending.insert(next);
    if (k == 0x26 && region_bytes - off >= 16) {
        const uint32_t target = u32(region + off + 12);
        if (record_target_ok(target, region_bytes)) pending.insert(target);
    } else if (k == 0x28 && region_bytes - off >= 16) {
        const uint32_t count = u32(region + off + 12);
        if (count == 0 || count >= 64) return;
        const size_t bytes = 16u + (size_t)count * 8u;
        if (bytes > region_bytes - off) return;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t target = u32(region + off + 16u +
                                        (size_t)count * 4u + (size_t)i * 4u);
            if (record_target_ok(target, region_bytes)) pending.insert(target);
        }
    }
}

static std::vector<uint32_t> discover_records(const uint8_t* region,
                                              size_t region_bytes,
                                              const std::vector<Fixup>& fixups)
{
    std::set<uint32_t> pending;
    std::set<uint32_t> seen;
    if (region_bytes >= 4) pending.insert(0);
    for (const Fixup& f : fixups) pending.insert(f.record_offset);
    while (!pending.empty()) {
        const uint32_t off = *pending.begin();
        pending.erase(pending.begin());
        if (!seen.insert(off).second) continue;
        add_control_targets(region, region_bytes, off, pending);
    }
    return std::vector<uint32_t>(seen.begin(), seen.end());
}

static bool mandatory_starts_ok(size_t off, size_t next,
                                const std::set<uint32_t>& mandatory)
{
    auto it = mandatory.upper_bound((uint32_t)off);
    return it == mandatory.end() || *it >= next;
}

/* KIND 0x23's LENGTH IS IN THE RECORD, read the way the interpreter reads it
 * (FUN_142484fc0 case 0x23): u32 n_in at +12, then n_in 8-byte operand pairs,
 * then u32 n_out, n_out pairs, then u32 n_ctx, n_ctx pairs. So
 *     length = 24 + 8 * (n_in + n_out + n_ctx)
 * with n_out and n_ctx found AFTER the preceding list - the earlier fit tried
 * fixed offsets (+16, +20) and topped out at 77.8% for exactly that reason.
 * 0 when the counts run past the region. */
static uint32_t k23_length(const uint8_t* region, size_t off, size_t limit)
{
    size_t at = off + 12;
    uint32_t total = 0;
    for (int list = 0; list < 3; ++list) {
        if (at + 4 > limit) return 0;
        const uint32_t n = u32(region + at);
        if (n > 64) return 0;
        total += n;
        at += 4 + (size_t)n * 8;
    }
    if (at > limit) return 0;
    return 24u + total * 8u;
}

/* (Superseded for length by k23_length above; kept for the record.)
 * KIND 0x23's LENGTH IS ITS OPERATOR'S ARITY, and it is not in the record.
 *
 * Fitting the length to the record's own fields tops out at 77.8%: the shape
 * (dword3=0, dword4=0) occurs 6,977 times at 24 bytes and 1,387 times at 32,
 * and nothing inside the record separates them. Grouping proven lengths by
 * OPERATOR KEY instead, 601 of 632 keys map to exactly one length - so the
 * length belongs to the operator, not to the record.
 *
 * The executable's reflected registry carries a parameter_count per operator,
 * and against every proven length it is exact, with no exceptions:
 *
 *     arity  0 -> 24    arity 13 -> 128    arity 16 -> 152
 *     arity 11 -> 112   arity 15 -> 144    arity 17 -> 160
 *     arity 12 -> 120
 *
 *     length = 24 + parameter_count * 8
 *
 * A 24-byte header and one 8-byte operand per declared parameter - the same
 * shape 0x28 has, with the count in the game's own reflection rather than in
 * the record. READ FROM THE GAME, not fitted to a corpus, which is why using
 * it does not break the promise that no exported corpus table is a runtime
 * input.
 *
 * Entirely optional: with no arity supplied, or for a key the registry does not
 * carry, the search below runs exactly as it did before. */
static uint32_t arity_length(const ArityMap* arity,
                             const std::map<uint32_t, uint32_t>& key_by_offset,
                             size_t off)
{
    if (!arity || arity->empty()) return 0;
    const auto k = key_by_offset.find((uint32_t)off);
    if (k == key_by_offset.end()) return 0;
    const auto a = arity->find(k->second);
    if (a == arity->end()) return 0;
    const uint32_t len = 24u + (uint32_t)a->second * 8u;
    // Outside the measured envelope this is not the record shape assumed here.
    return (len >= 24u && len <= 232u && (len % 8u) == 0u) ? len : 0u;
}

// The exact tiler is intentionally conservative: it uses only lengths whose
// evidence is published as measured. It never guesses a length for a rare
// kind. A false result is therefore a named coverage gap, not parse failure.
static bool tile_records(const uint8_t* region, size_t limit,
                         const std::set<uint32_t>& mandatory,
                         std::vector<uint32_t>& starts,
                         const ArityMap* arity,
                         const std::map<uint32_t, uint32_t>& key_by_offset,
                         uint8_t* ways_out = nullptr)
{
    if (limit == 0) return true;
    std::vector<uint8_t> ways(limit + 1, 0);
    std::vector<uint32_t> choice(limit + 1, 0);
    ways[limit] = 1;
    for (size_t off = limit; off-- > 0;) {
        if ((off & 3u) || limit - off < 4) continue;
        const uint8_t kind = region[off];
        if (kind == 0x28) {
            if (limit - off < 16) continue;
            const uint32_t count = u32(region + off + 12);
            if (count == 0 || count >= 64) continue;
            const size_t len = 16u + (size_t)count * 8u;
            const size_t next = off + len;
            if (next <= limit && ways[next] && mandatory_starts_ok(off, next, mandatory)) {
                ways[off] = ways[next]; choice[off] = (uint32_t)len;
            }
        } else if (kind == 0x23) {
            /* Exact, from the record's own counts (see k23_length). */
            const uint32_t exact = k23_length(region, off, limit);
            if (exact) {
                const size_t next = off + exact;
                if (next <= limit && ways[next] && mandatory_starts_ok(off, next, mandatory)) {
                    ways[off] = ways[next];
                    choice[off] = exact;
                }
                continue;
            }
            // The operator's own arity settles it where the registry knows the
            // key; that is one length, so no ambiguity can arise.
            const uint32_t pinned = arity_length(arity, key_by_offset, off);
            if (pinned) {
                const size_t next = off + pinned;
                if (next <= limit && ways[next] && mandatory_starts_ok(off, next, mandatory)) {
                    ways[off] = ways[next];
                    choice[off] = pinned;
                }
                continue;
            }
            // Otherwise as before: observed range 24..232, in 8-byte steps.
            for (uint32_t len = 24; len <= 232 && off + len <= limit; len += 8) {
                const size_t next = off + len;
                if (!ways[next] || !mandatory_starts_ok(off, next, mandatory)) continue;
                const unsigned sum = (unsigned)ways[off] + (unsigned)ways[next];
                ways[off] = (uint8_t)std::min(sum, 2u); // 2 means ambiguous
                if (choice[off] == 0) choice[off] = len;
            }
        } else {
            const uint32_t len = proven_fixed_length(kind);
            const size_t next = off + len;
            if (len && next <= limit && ways[next] && mandatory_starts_ok(off, next, mandatory)) {
                ways[off] = ways[next]; choice[off] = len;
            }
        }
    }
    /* WHY it failed, not just that it did. "No tiling exists" and "several
     * tilings fit" are opposite problems - the first means a length is wrong or
     * missing, the second means a constraint is missing - and treating them as
     * one failure hides which fix is needed. */
    if (ways_out) *ways_out = ways[0];
    if (ways[0] != 1) return false;
    size_t off = 0;
    while (off < limit) {
        const uint32_t len = choice[off];
        if (!len) return false;
        starts.push_back((uint32_t)off);
        off += len;
    }
    return off == limit;
}

static bool has_trailing_dword(uint8_t k)
{
    switch (k) {
    case 0x23: case 0x24: case 0x25: case 0x2a: case 0x2e:
    case 0x32: case 0x33: case 0x34: case 0x35: case 0x36:
    case 0x38: case 0x3e: return true;
    default: return false;
    }
}

static void materialize_records(const uint8_t* region, size_t region_bytes,
                                const std::vector<uint32_t>& starts,
                                const std::map<uint32_t, uint32_t>& key_by_offset,
                                std::vector<Record>& records,
                                size_t& covered)
{
    records.clear();
    covered = 0;
    for (size_t i = 0; i < starts.size(); ++i) {
        const size_t off = starts[i];
        const size_t end = i + 1 < starts.size() ? starts[i + 1] : region_bytes;
        if (end <= off || end > region_bytes || end - off < 4) continue;
        const uint32_t h = u32(region + off);
        Record r;
        r.offset = (uint32_t)off;
        r.byte_length = (uint32_t)(end - off);
        r.kind = (uint8_t)(h & 0xffu);
        r.next = h >> 8;
        r.has_operator = has_operator_pointer(r.kind);
        const auto key = key_by_offset.find(r.offset);
        if (key != key_by_offset.end()) r.operator_key = key->second;
        if (r.kind == 0x26 && end - off >= 16) {
            r.operands.push_back({u32(region + off + 4),
                                  u32(region + off + 8)});
            r.control_target = u32(region + off + 12);
            covered += end - off;
            records.push_back(std::move(r));
            continue;
        }
        if (r.kind == 0x28 && end - off >= 16) {
            r.operands.push_back({u32(region + off + 4),
                                  u32(region + off + 8)});
            const uint32_t count = u32(region + off + 12);
            if (count > 0 && count < 64 &&
                16u + (size_t)count * 8u <= end - off) {
                for (uint32_t j = 0; j < count; ++j)
                    r.dispatch_labels.push_back(u32(region + off + 16u +
                                                     (size_t)j * 4u));
                for (uint32_t j = 0; j < count; ++j)
                    r.dispatch_targets.push_back(u32(region + off + 16u +
                        (size_t)count * 4u + (size_t)j * 4u));
            }
            covered += end - off;
            records.push_back(std::move(r));
            continue;
        }
        if (r.kind == 0x23 && k23_length(region, off, end) == end - off) {
            /* Three counted lists: inputs, outputs, context. */
            size_t at = off + 12;
            uint16_t ns[3] = {0, 0, 0};
            std::vector<Operand> lists[3];
            for (int list = 0; list < 3; ++list) {
                const uint32_t n = u32(region + at);
                at += 4;
                for (uint32_t j = 0; j < n; ++j, at += 8)
                    lists[list].push_back({u32(region + at), u32(region + at + 4)});
                ns[list] = (uint16_t)n;
            }
            for (int list = 0; list < 3; ++list)
                r.operands.insert(r.operands.end(), lists[list].begin(), lists[list].end());
            r.counted_lists = true;
            r.n_in = ns[0]; r.n_out = ns[1]; r.n_ctx = ns[2];
            covered += end - off;
            records.push_back(std::move(r));
            continue;
        }
        /* SPLIT-LIST CALLS 0x14..0x1D (interpreter): the pairs after the operator
         * pointer go to an INPUT array and an OUTPUT array in a fixed split per
         * kind, the same (inputs, outputs) calling convention as 0x23. */
        {
            static const uint8_t kSplit[10][2] = {
                {1,1},{1,2},{1,3},{1,4},{2,0},{2,1},{3,0},{3,1},{4,0},{4,1}};
            if (r.kind >= 0x14 && r.kind <= 0x1d) {
                const uint8_t ni = kSplit[r.kind - 0x14][0], no = kSplit[r.kind - 0x14][1];
                if (end - off == 12u + 8u * (size_t)(ni + no)) {
                    for (size_t at = off + 12; at + 8 <= end; at += 8)
                        r.operands.push_back({u32(region + at), u32(region + at + 4)});
                    r.counted_lists = true;
                    r.n_in = ni; r.n_out = no; r.n_ctx = 0;
                    covered += end - off;
                    records.push_back(std::move(r));
                    continue;
                }
            }
        }
        size_t body = off + (r.has_operator ? 12u : 4u);
        size_t body_end = end;
        if (has_trailing_dword(r.kind) && body_end >= body + 4) {
            body_end -= 4;
            r.has_trailing_dword = true;
            r.trailing_dword = u32(region + body_end);
        }
        while (body + 8 <= body_end) {
            r.operands.push_back({u32(region + body), u32(region + body + 4)});
            body += 8;
        }
        covered += end - off;
        records.push_back(std::move(r));
    }
}

} // namespace

bool has_operator_pointer(uint8_t kind)
{
    return kind <= 0x1d || kind == 0x23;
}

uint32_t proven_record_length(uint8_t kind)
{
    return proven_fixed_length(kind);
}

bool parse(const uint8_t* data, size_t size, Graph& out, std::string& error,
           const ArityMap* arity)
{
    out = Graph{};
    error.clear();
    if (!data || size < 0x50) {
        error = "payload is smaller than the 0x50-byte graph header";
        return false;
    }
    for (size_t i = 0; i < 0x10; ++i) {
        if (data[i] != 0) {
            error = "leading relocation-pointer holes are not zero";
            return false;
        }
    }

    Header& h = out.header;
    h.content_hash = u32(data + 0x10);
    h.instance_header_size = u32(data + 0x20);
    h.constant_pool_size = u32(data + 0x24);
    h.slot_file_size = u32(data + 0x28);
    h.record_dwords = u32(data + 0x2c);
    h.pointer_table_entries = u32(data + 0x30);
    h.external_bindings = u32(data + 0x34);
    h.relocation_count = u16(data + 0x38);
    h.secondary_relocation_count = u16(data + 0x3a);
    h.fixup_count = u16(data + 0x3c);
    h.type_table_dwords = u16(data + 0x3e);
    h.instance_value_dwords = u16(data + 0x40);
    h.slot_value_dwords = u16(data + 0x42);
    h.instance_buffer_count = data[0x44];
    h.register_groups[0] = data[0x45];
    h.register_groups[1] = data[0x46];
    h.register_groups[2] = data[0x47];

    const size_t typed_dwords = (size_t)h.type_table_dwords +
                                h.instance_value_dwords + h.slot_value_dwords;
    size_t typed_bytes = 0;
    if (!mul_ok(typed_dwords, 4, typed_bytes)) {
        error = "typed-table size overflow"; return false;
    }
    size_t typed_span = 0;
    if (!add_ok(typed_bytes, 0x5f, typed_span)) {
        error = "typed-table span overflow"; return false;
    }
    typed_span &= ~size_t(0x0f);
    size_t rb0 = 0;
    if (!add_ok((size_t)h.constant_pool_size, 3, rb0) ||
        !add_ok(rb0, typed_span, rb0)) {
        error = "record-base overflow"; return false;
    }
    out.region_base = rb0 & ~size_t(3);
    if (out.region_base < h.constant_pool_size) {
        error = "constant pool precedes payload"; return false;
    }
    out.constant_base = out.region_base - h.constant_pool_size;

    size_t record_bytes = 0, relocation_count = 0, relocation_bytes = 0;
    if (!mul_ok((size_t)h.record_dwords, 4, record_bytes) ||
        !add_ok((size_t)h.relocation_count, h.secondary_relocation_count,
                relocation_count) ||
        !mul_ok(relocation_count, 8, relocation_bytes) ||
        !add_ok(out.region_base, record_bytes, out.relocation_table) ||
        !add_ok(out.relocation_table, relocation_bytes, out.fixup_table)) {
        error = "record/table offset overflow"; return false;
    }
    size_t fixup_bytes = 0, after_fixups = 0;
    if (!mul_ok((size_t)h.fixup_count, 8, fixup_bytes) ||
        !add_ok(out.fixup_table, fixup_bytes, after_fixups)) {
        error = "fixup-table offset overflow"; return false;
    }
    out.image_at = align16(after_fixups);
    size_t image_end = 0;
    if (!add_ok(out.image_at, h.instance_header_size, image_end) || image_end > size) {
        error = "instance image exceeds payload"; return false;
    }
    if (after_fixups > size || out.region_base > size ||
        out.constant_base > out.region_base) {
        error = "derived graph table lies outside payload"; return false;
    }

    const size_t type_begin = 0x50;
    size_t type_bytes = (size_t)h.type_table_dwords * 4;
    const size_t instance_begin = type_begin + type_bytes;
    const size_t instance_bytes = (size_t)h.instance_value_dwords * 4;
    const size_t slot_begin = instance_begin + instance_bytes;
    const size_t slot_bytes = (size_t)h.slot_value_dwords * 4;
    if (slot_begin + slot_bytes > out.constant_base) {
        error = "typed tables overlap the constant pool"; return false;
    }
    if (type_bytes % 20u) {
        error = "type table is not an exact sequence of 20-byte records";
        return false;
    }
    for (size_t at = type_begin; at < type_begin + type_bytes; at += 20) {
        TypeRecord t;
        t.type_id = u32(data + at);
        for (int i = 0; i < 4; ++i) t.carried[i] = u32(data + at + 4 + i * 4);
        out.types.push_back(t);
    }
    if (!parse_typed_table(data, instance_begin, instance_bytes,
                           h.instance_header_size, out.instance_values, error) ||
        !parse_typed_table(data, slot_begin, slot_bytes,
                           h.slot_file_size, out.slot_values, error)) return false;

    out.constant_pool.assign(data + out.constant_base, data + out.region_base);
    for (size_t i = 0; i < relocation_count; ++i) {
        const size_t at = out.relocation_table + i * 8;
        Relocation rel{u32(data + at), u32(data + at + 4)};
        if ((rel.pointer_field & 3u) || rel.pointer_field > h.constant_pool_size ||
            h.constant_pool_size - rel.pointer_field < 8 ||
            rel.target >= h.constant_pool_size) {
            error = "pool relocation points outside the constant pool";
            return false;
        }
        if (u64(data + out.constant_base + rel.pointer_field) != 0) {
            error = "pool relocation pointer field is already patched";
            return false;
        }
        out.relocations.push_back(rel);
    }

    std::map<uint32_t, uint32_t> key_by_offset;
    const uint8_t* region = data + out.region_base;
    for (uint32_t i = 0; i < h.fixup_count; ++i) {
        const size_t at = out.fixup_table + (size_t)i * 8;
        Fixup f{u32(data + at), u32(data + at + 4)};
        if (!record_target_ok(f.record_offset, record_bytes) ||
            record_bytes - f.record_offset < 12) {
            error = "operator fixup does not name a complete record";
            return false;
        }
        const uint8_t kind = region[f.record_offset];
        if (!has_operator_pointer(kind)) {
            error = "operator fixup targets a kind with no operator pointer";
            return false;
        }
        if (u64(region + f.record_offset + 4) != 0) {
            error = "operator pointer field is already patched";
            return false;
        }
        if (!key_by_offset.emplace(f.record_offset, f.key).second) {
            error = "two operator fixups target the same record";
            return false;
        }
        out.fixups.push_back(f);
    }

    std::set<uint32_t> mandatory;
    mandatory.insert(0);
    for (const Fixup& f : out.fixups) mandatory.insert(f.record_offset);
    // Control targets are constraints too. The discovery pass is complete
    // enough to supply them even when a rare kind prevents exact tiling.
    const std::vector<uint32_t> discovered =
        discover_records(region, record_bytes, out.fixups);
    mandatory.insert(discovered.begin(), discovered.end());

    std::vector<uint32_t> starts;
    out.exact_record_tiling =
        tile_records(region, record_bytes, mandatory, starts, arity, key_by_offset,
                     &out.tiling_ways);
    /* A PIN THAT CONTRADICTS THE GRAPH IS A WRONG PIN, not a broken graph.
     *
     * Pinning 0x23 to its operator's arity turns ambiguity into certainty when
     * the arity is right. When it is WRONG - a variadic operator whose other
     * appearances all shared one length - it removes the only valid tiling and
     * the graph reports no tiling at all, which is strictly worse than the
     * ambiguity it replaced. Five graphs did exactly that.
     *
     * So an empty result with pins in play is retried without them. The worst
     * case is then the un-pinned answer, and pinning can only ever help. */
    if (!out.exact_record_tiling && out.tiling_ways == 0 && arity && !arity->empty()) {
        starts.clear();
        out.arity_pin_rejected = 1;
        out.exact_record_tiling =
            tile_records(region, record_bytes, mandatory, starts, nullptr, key_by_offset,
                         &out.tiling_ways);
    }
    if (!out.exact_record_tiling) starts = discovered;
    materialize_records(region, record_bytes, starts, key_by_offset,
                        out.records, out.discovered_record_bytes);

    out.instance_image.assign(data + out.image_at, data + image_end);
    return true;
}

}} // namespace bf6::expression
