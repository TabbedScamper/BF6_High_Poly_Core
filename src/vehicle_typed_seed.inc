/* THE TYPED VALUES A GRAPH DOES NOT CARRY IN ITS BLOB.
 *
 * A vehicle graph's curves live in the same resource read as EBX, not in the
 * expression blob: the blob's constant pool holds an empty descriptor whose pointer
 * fields are 0x000FFFFF, the engine's unbound-handle placeholder, and the loader
 * fills it. Three things have to line up to do that offline, and all three are
 * measured rather than assumed:
 *
 *   1. WHICH SLOTS. The graph's own typed value groups say "these slot offsets hold
 *      a value of this reflected type" - a boat declares two 40-byte keyed curves
 *      and eight 120-byte tables.
 *   2. WHERE THE COPY READS FROM. Each of those slots is filled every tick by a
 *      typed copy out of the pool, so patching the pool is what sticks. The source
 *      is read off the copying record itself.
 *   3. WHAT GOES IN. The resource's EBX carries values of that type; the
 *      executable's reflection gives each field's offset, so a field lands where the
 *      native reads it instead of where it happened to appear in the file.
 *
 * The k-th slot of a type takes the k-th value of that type in the resource, which
 * is the only ordering either side offers.
 */

namespace {

/* Every value of `guid` in a partition, in document order. */
void bf6__collect_typed(const bf6::EbxValue& v, const bf6::TypeGuid& guid,
                        std::vector<const bf6::EbxValue*>& out, int depth)
{
    if (depth > 24) return;
    if (v.kind == bf6::EbxValue::Kind::Struct && v.guid == guid) out.push_back(&v);
    for (const auto& kv : v.fields) bf6__collect_typed(kv.second, guid, out, depth + 1);
    for (const auto& it : v.items) bf6__collect_typed(it, guid, out, depth + 1);
}

/* One number, as the four bytes the field holds.
 *
 * AN INTEGER STAYS AN INTEGER. A curve key's interpolation mode is an integer in a
 * four-byte field the native compares against the BIT PATTERNS 1, 2 and 3 (the
 * decompiler shows them as 1.4e-45 and friends, which is what those bits are as a
 * float). Converting it to 2.0f wrote 0x40000000 where the engine wanted 2, and
 * every key fell through to the arm for mode 0. */
bool bf6__as_bits(const bf6::EbxValue& v, uint32_t& bits)
{
    switch (v.kind) {
    case bf6::EbxValue::Kind::Real: {
        const float f = (float)v.f;
        std::memcpy(&bits, &f, 4);
        return true;
    }
    case bf6::EbxValue::Kind::Int:
        bits = (uint32_t)(int32_t)v.i; return true;
    case bf6::EbxValue::Kind::Uint:
        bits = (uint32_t)v.u; return true;
    case bf6::EbxValue::Kind::Bool: bits = v.b ? 1u : 0u; return true;
    default: return false;
    }
}

/* Write one EBX value into the pool at `base`, field by field, through the layout.
 * A field the layout does not name is left alone rather than guessed at. */
void bf6__seed_value(bf6::TypeDb& types, const bf6::TypeLayout& lay, const bf6::EbxValue& val,
                     uint32_t base, std::map<uint32_t, uint32_t>& patches, int depth,
                     bf6::expression::WheelOps* curves = nullptr)
{
    if (!lay.valid || depth > 8) return;
    for (const auto& f : lay.fields) {
        const bf6::EbxValue* fv = val.field(f.name_hash);
        if (!fv) continue;
        uint32_t bits = 0;
        if (bf6__as_bits(*fv, bits)) {
            patches[base + f.offset] = bits;
            continue;
        }
        if (fv->kind == bf6::EbxValue::Kind::Struct) {
            const bf6::TypeLayout& sub = types.layout_full(fv->guid);
            bf6__seed_value(types, sub, *fv, base + f.offset, patches, depth + 1, curves);
            continue;
        }
        if (fv->kind == bf6::EbxValue::Kind::Array && !fv->items.empty()) {
            /* An array that does NOT fit inside its own struct is held behind a
             * pointer at runtime - a keyed curve's keys are, a fixed table's entries
             * are not. There is nowhere in an offline pool to put the keys, so the
             * field gets a handle for the curve the host was given instead. */
            /* An INLINE array: the runtime type is a fixed-size table and the file
             * carries only the entries that are used, which is exactly the shape the
             * curve evaluators expect (they stop at the first x that does not rise).
             * The element stride comes from the element type's own layout. */
            const bf6::EbxValue& first = fv->items.front();
            if (first.kind != bf6::EbxValue::Kind::Struct) continue;
            const bf6::TypeLayout& el = types.layout_full(first.guid);
            if (!el.valid || !el.size) continue;
            if ((uint64_t)fv->items.size() * el.size + f.offset > lay.size) {
                std::vector<float> keys((size_t)fv->items.size() * (el.size / 4), 0.0f);
                for (size_t i = 0; i < fv->items.size(); ++i) {
                    std::map<uint32_t, uint32_t> one;
                    bf6__seed_value(types, el, fv->items[i], 0, one, depth + 1);
                    for (const auto& kv : one)
                        if (kv.first / 4 < el.size / 4)
                            std::memcpy(&keys[i * (el.size / 4) + kv.first / 4], &kv.second, 4);
                }
                if (curves) {
                    const uint32_t handle =
                        bf6::expression::WheelOps::curve_handle(curves->add_curve(std::move(keys)));
                    patches[base + f.offset] = handle;
                }
                continue;
            }
            for (size_t i = 0; i < fv->items.size(); ++i)
                bf6__seed_value(types, el, fv->items[i], base + f.offset + (uint32_t)i * el.size,
                                patches, depth + 1);
        }
    }
}

} // namespace

void bf6__seed_typed_values(bf6_ctx* ctx, const std::string& name, bf6::VehicleSim& sim)
{
    std::string err;
    if (!ensure_types(ctx, err)) return;
    std::vector<uint8_t> raw = ctx->src.get_ebx(name, err);
    if (raw.empty()) return;
    bf6::Ebx e(*ctx->types);
    if (!e.parse(std::move(raw), err)) return;

    const bf6::expression::Graph* gr = sim.graph_named(name);
    if (!gr) return;
    /* Which pool offset each typed slot is copied from: a kind 0x24 or 0x25 record
     * whose source is the pool and whose destination is that slot. */
    std::map<uint32_t, uint32_t> from_pool;
    for (const auto& rec : gr->records) {
        if (rec.has_operator || rec.kind < 0x24 || rec.kind > 0x25 || rec.operands.size() < 2) continue;
        const auto& src = rec.operands.front();
        const auto& dst = rec.operands.back();
        if (src.region == 0 && dst.region == 2) from_pool[dst.offset] = src.offset;
    }

    const bool dbg = std::getenv("BF6_SEED_DEBUG") != nullptr;
    for (const auto& grp : gr->slot_values) {
        bf6::TypeGuid guid{};
        if (!ctx->types->guid_by_name_hash(grp.data_type_id, guid)) {
            if (dbg) std::fprintf(stderr, "seed: type %08X has no guid\n", grp.data_type_id);
            continue;
        }
        const bf6::TypeLayout& lay = ctx->types->layout_full(guid);
        /* The instances are KEPT: a collected value is a pointer into the tree the
         * reader built, so the tree has to outlive the walk. */
        std::vector<bf6::EbxValue> kept;
        kept.reserve(e.instance_count());
        for (size_t i = 0; i < e.instance_count(); ++i) kept.push_back(e.read_instance(i));
        std::vector<const bf6::EbxValue*> vals;
        for (const auto& v : kept) bf6__collect_typed(v, guid, vals, 0);
        if (dbg) {
            std::fprintf(stderr, "seed: type %08X size %u, %zu slot(s), %zu value(s) in the resource\n",
                         grp.data_type_id, lay.size, grp.offsets.size(), vals.size());
            std::fprintf(stderr, "  layout fields %zu:", lay.fields.size());
            for (size_t i = 0; i < lay.fields.size() && i < 8; ++i)
                std::fprintf(stderr, " %08X@%u", lay.fields[i].name_hash, lay.fields[i].offset);
            std::fprintf(stderr, "\n");
            if (!vals.empty()) {
                std::fprintf(stderr, "  value fields %zu:", vals[0]->fields.size());
                for (const auto& kv : vals[0]->fields) std::fprintf(stderr, " %08X", kv.first);
                std::fprintf(stderr, "\n");
            }
        }
        /* INLINE OR BY REFERENCE. The graph puts consecutive slots of a group one
         * VALUE apart when the slot holds the value and one POINTER apart when it
         * holds a handle the loader fills: a boat's eight 120-byte tables sit 128
         * apart, its two keyed curves 8 apart against a 40-byte type. A handle is
         * seeded as a handle - writing the struct into the pool would be writing it
         * where the graph keeps something else. */
        /* ONLY WHERE THE PAIRING IS FORCED. The k-th slot taking the k-th value of
         * its type is the only ordering either side offers, and it is sound only
         * when there are as many values as slots. A car declares thirteen tables and
         * ships eleven, a tank eleven and nine: pairing those by index handed each
         * one somebody else's gear and torque curves, and both lost a third of their
         * top speed while the tank stopped leaving the start line. Where the counts
         * disagree the graph's own pool is left exactly as authored, which is what
         * the vehicles had before any of this. */
        if (vals.size() != grp.offsets.size()) {
            if (dbg)
                std::fprintf(stderr, "  %zu value(s) for %zu slot(s): the pool is left alone\n",
                             vals.size(), grp.offsets.size());
            continue;
        }
        /* The spacing is measured on the SORTED offsets; the pairing keeps the
         * graph's own order, because the k-th slot as the graph lists it takes the
         * k-th value of that type in the resource, and sorting the pairing handed a
         * car's gear tables to each other's slots. */
        std::vector<uint32_t> sorted(grp.offsets.begin(), grp.offsets.end());
        std::sort(sorted.begin(), sorted.end());
        const uint32_t stride = sorted.size() > 1 ? sorted[1] - sorted[0] : lay.size;
        const bool by_reference = stride && stride < lay.size;
        for (size_t k = 0; k < grp.offsets.size() && k < vals.size(); ++k) {
            const auto at = from_pool.find(grp.offsets[k]);
            if (at == from_pool.end()) {
                if (dbg) std::fprintf(stderr, "  slot 0x%X is not copied from the pool\n", grp.offsets[k]);
                continue;
            }
            std::map<uint32_t, uint32_t> patches;
            if (by_reference) {
                /* The value's own array field carries the keys; the pool word gets a
                 * handle to the curve the host now holds. */
                std::map<uint32_t, uint32_t> fields;
                bf6__seed_value(*ctx->types, lay, *vals[k], 0, fields, 0, &sim.wheel_ops());
                for (const auto& f : lay.fields) {
                    const auto h = fields.find(f.offset);
                    if (h == fields.end()) continue;
                    if ((h->second & 0xFFFF0000u) == 0xC0DE0000u) patches[at->second] = h->second;
                }
            } else {
                bf6__seed_value(*ctx->types, lay, *vals[k], at->second, patches, 0, &sim.wheel_ops());
            }
            sim.add_pool_patches(name, patches);
            if (dbg) {
                std::fprintf(stderr, "  slot 0x%X <- pool 0x%X%s, %zu word(s):",
                             grp.offsets[k], at->second, by_reference ? " (by reference)" : "",
                             patches.size());
                size_t shown = 0;
                for (const auto& kv : patches) {
                    if (shown++ >= 6) break;
                    std::fprintf(stderr, " 0x%X=%08X", kv.first, kv.second);
                }
                std::fprintf(stderr, "\n");
            }
        }
    }
}
