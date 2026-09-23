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

/* WHERE EACH VALUE GOES, STATED BY THE DATA.
 *
 * The pairing of authored values to pool offsets never needed to be guessed: every
 * value is wrapped in an entry that carries its DESTINATION. A car's eleven curve
 * tables sit in one array, each entry holding a small descriptor and the table:
 *
 *     0xC939B436 [11]
 *        0: { 0xFC636C1F { 0xBDF81E34 0, 0x4FF541E4 1024 },   <- the pool offset
 *             0x8CF424E7 {15} ... }                           <- the table
 *
 * and those eleven offsets - 512, 640, 768 ... 1792 - are exactly the eleven pool
 * offsets that car's typed copies read, in no particular order. Two count-based
 * pairings were tried before this turned up and both broke working vehicles; neither
 * was needed.
 *
 * So the walk looks for the WRAPPER rather than for the value: any struct carrying an
 * offset descriptor and a struct-valued field is one placement, and the field's own
 * type decides how it is written. */
const uint32_t kPlacementDesc = 0xFC636C1Fu;   /* the descriptor that carries it */
const uint32_t kPlacementOff  = 0x4FF541E4u;   /* the pool offset itself */

struct bf6__placement {
    uint32_t pool_offset = 0;
    const bf6::EbxValue* value = nullptr;
};

void bf6__collect_placements(const bf6::EbxValue& v,
                             std::vector<bf6__placement>& out, int depth)
{
    if (depth > 24) return;
    const bf6::EbxValue* desc = v.field(kPlacementDesc);
    if (desc && desc->kind == bf6::EbxValue::Kind::Struct) {
        const bf6::EbxValue* off = desc->field(kPlacementOff);
        if (off && (off->kind == bf6::EbxValue::Kind::Uint ||
                    off->kind == bf6::EbxValue::Kind::Int)) {
            const uint32_t at = off->kind == bf6::EbxValue::Kind::Uint
                                    ? (uint32_t)off->u : (uint32_t)off->i;
            /* The placed value is the entry's OTHER field - a struct, not the
             * descriptor. An entry with none places nothing. */
            for (const auto& kv : v.fields)
                if (kv.first != kPlacementDesc &&
                    kv.second.kind == bf6::EbxValue::Kind::Struct)
                    out.push_back({at, &kv.second});
        }
    }
    for (const auto& kv : v.fields) bf6__collect_placements(kv.second, out, depth + 1);
    for (const auto& it : v.items) bf6__collect_placements(it, out, depth + 1);
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
    const bool dbg = std::getenv("BF6_SEED_DEBUG") != nullptr;
    /* The instances are KEPT: a collected value points into the tree the reader built,
     * so the tree has to outlive the walk. */
    std::vector<bf6::EbxValue> kept;
    kept.reserve(e.instance_count());
    for (size_t i = 0; i < e.instance_count(); ++i) kept.push_back(e.read_instance(i));
    std::vector<bf6__placement> places;
    for (const auto& v : kept) bf6__collect_placements(v, places, 0);
    if (dbg) std::fprintf(stderr, "seed: %zu placement(s) declared by %s\n",
                          places.size(), name.c_str());
    /* The reflected layout of any type this graph names. A struct BUILDER writes its
     * i-th argument at the i-th entry of an offsets array the loader hands it, and for
     * a struct that array is the type's own field table - so the field order here is
     * what such a builder can be checked against. */
    if (std::getenv("BF6_TYPE_LAYOUT"))
        for (const auto& grp : gr->slot_values) {
            bf6::TypeGuid g{};
            if (!ctx->types->guid_by_name_hash(grp.data_type_id, g)) continue;
            const bf6::TypeLayout& l = ctx->types->layout_full(g);
            std::fprintf(stderr, "type %08X size %u, %zu field(s):", grp.data_type_id,
                         l.size, l.fields.size());
            for (const auto& f : l.fields) std::fprintf(stderr, " %08X@%u", f.name_hash, f.offset);
            std::fprintf(stderr, "\n");
        }

    /* Which pool offsets this graph's typed copies actually read, and the slot each
     * one feeds. A placement naming an offset no copy reads is for a region this
     * graph does not use, and is skipped rather than written blindly. */
    std::map<uint32_t, uint32_t> slot_of_pool;      /* pool offset -> slot offset */
    for (const auto& rec : gr->records) {
        if (rec.has_operator || rec.kind < 0x24 || rec.kind > 0x25 || rec.operands.size() < 2) continue;
        const auto& src = rec.operands.front();
        const auto& dst = rec.operands.back();
        if (src.region == 0 && dst.region == 2) slot_of_pool[src.offset] = dst.offset;
    }
    /* Each typed slot's declared type and its spacing, so a value written by reference
     * is told from one written inline. */
    std::map<uint32_t, uint32_t> slot_type;
    std::map<uint32_t, uint32_t> slot_stride;
    for (const auto& grp : gr->slot_values) {
        std::vector<uint32_t> sorted(grp.offsets.begin(), grp.offsets.end());
        std::sort(sorted.begin(), sorted.end());
        for (size_t i = 0; i < sorted.size(); ++i) {
            slot_type[sorted[i]] = grp.data_type_id;
            if (sorted.size() > 1)
                slot_stride[sorted[i]] = i + 1 < sorted.size() ? sorted[i + 1] - sorted[i]
                                                               : sorted[i] - sorted[i - 1];
        }
    }

    /* THE FIELD OFFSET EACH ADDRESS CALL WANTS.
     *
     * 0x88030F01 is `object + *(dword*)selector`, and the selector is a pool word the
     * loader fills from the reflected field table. Offline it reads zero, so every one
     * of these calls returns the object's start and the graph reads the same field
     * fifteen times - which is why a helicopter's rotor config came out empty.
     *
     * The calls that share a base object address that object's fields IN ORDER: their
     * selectors sit at consecutive pool offsets, and the struct builder downstream
     * writes the i-th of them to the i-th field of the same type. So the i-th call by
     * selector offset takes the i-th field, and patching its pool word is all that is
     * needed - the operator itself reads the offset from there, exactly as the native
     * does. Where the base slot's type has no reflected layout, nothing is patched and
     * the calls stay as they were. */
    {
        struct Call { uint32_t selector; uint32_t base; };
        std::map<uint32_t, std::vector<Call>> by_base;
        for (const auto& rec : gr->records) {
            if (!rec.has_operator || rec.operator_key != 0x88030F01u ||
                rec.operands.size() < 3) continue;
            if (rec.operands[0].region != 2 || rec.operands[1].region != 0) continue;
            by_base[rec.operands[0].offset].push_back({rec.operands[1].offset,
                                                      rec.operands[0].offset});
        }
        std::map<uint32_t, uint32_t> sel_patch;
        for (auto& kv : by_base) {
            const auto ty = slot_type.find(kv.first);
            if (ty == slot_type.end()) continue;
            bf6::TypeGuid g{};
            if (!ctx->types->guid_by_name_hash(ty->second, g)) continue;
            const bf6::TypeLayout& lay = ctx->types->layout_full(g);
            if (!lay.valid || lay.fields.empty()) continue;
            std::sort(kv.second.begin(), kv.second.end(),
                      [](const Call& a, const Call& b) { return a.selector < b.selector; });
            /* More calls than the type has fields would mean the order is not what it
             * looks like, so nothing is patched rather than some of it. */
            if (kv.second.size() > lay.fields.size()) {
                if (dbg)
                    std::fprintf(stderr, "seed: slot 0x%X has %zu field calls for %zu"
                                         " field(s): left alone\n",
                                 kv.first, kv.second.size(), lay.fields.size());
                continue;
            }
            for (size_t i = 0; i < kv.second.size(); ++i)
                sel_patch[kv.second[i].selector] = lay.fields[i].offset;
            if (dbg)
                std::fprintf(stderr, "seed: slot 0x%X type %08X, %zu field address(es)\n",
                             kv.first, ty->second, kv.second.size());
        }
        if (!sel_patch.empty()) {
            sim.add_pool_patches(name, sel_patch);
            if (dbg)
                std::fprintf(stderr, "seed: %zu field selector(s) patched\n", sel_patch.size());
        }
    }

    /* THE FIELD TABLE A STRUCT BUILDER NEEDS. 0x8B226FBB writes its i-th argument at
     * the i-th field of the type it produces, and only this caller can see the type
     * database, so the table is measured here and handed to the host. The type is the
     * one declared for the slot the builder writes; the width of each field is the gap
     * to the next field in OFFSET order, which is what a field occupies. */
    for (const auto& rec : gr->records) {
        if (!rec.has_operator || rec.operator_key != 0x8B226FBBu || rec.operands.empty()) continue;
        uint32_t out_slot = 0;
        bool have = false;
        for (auto it = rec.operands.rbegin(); it != rec.operands.rend(); ++it)
            if (it->region == 2) { out_slot = it->offset; have = true; break; }
        if (!have) continue;
        const auto ty = slot_type.find(out_slot);
        if (ty == slot_type.end()) continue;
        bf6::TypeGuid g{};
        if (!ctx->types->guid_by_name_hash(ty->second, g)) continue;
        const bf6::TypeLayout& lay = ctx->types->layout_full(g);
        if (!lay.valid || !lay.size || lay.fields.empty()) continue;
        std::vector<uint32_t> sorted;
        for (const auto& f : lay.fields) sorted.push_back(f.offset);
        std::sort(sorted.begin(), sorted.end());
        bf6::expression::WheelOps::BuilderLayout b;
        b.size = lay.size;
        for (const auto& f : lay.fields) {
            const auto nx = std::upper_bound(sorted.begin(), sorted.end(), f.offset);
            const uint32_t end = nx == sorted.end() ? lay.size : *nx;
            b.offsets.push_back(f.offset);
            b.widths.push_back(end > f.offset ? end - f.offset : 4u);
        }
        sim.wheel_ops().set_builder(b);
        if (dbg)
            std::fprintf(stderr, "seed: builder for type %08X, %zu field(s), %u bytes\n",
                         ty->second, b.offsets.size(), b.size);
    }

    size_t written = 0;
    for (const bf6__placement& pl : places) {
        const auto slot = slot_of_pool.find(pl.pool_offset);
        if (slot == slot_of_pool.end()) continue;      /* this graph does not read it */
        const bf6::TypeLayout& lay = ctx->types->layout_full(pl.value->guid);
        if (!lay.valid || !lay.size) {
            if (dbg)
                std::fprintf(stderr, "  pool 0x%X: the placed value's type has no layout\n",
                             pl.pool_offset);
            continue;
        }
        const auto st = slot_stride.find(slot->second);
        const bool by_reference = st != slot_stride.end() && st->second && st->second < lay.size;
        std::map<uint32_t, uint32_t> patches;
        if (by_reference) {
            /* The slot holds a handle, so only the curve handle goes in the pool. */
            std::map<uint32_t, uint32_t> fields;
            bf6__seed_value(*ctx->types, lay, *pl.value, 0, fields, 0, &sim.wheel_ops());
            for (const auto& f : fields)
                if ((f.second & 0xFFFF0000u) == 0xC0DE0000u) patches[pl.pool_offset] = f.second;
        } else {
            bf6__seed_value(*ctx->types, lay, *pl.value, pl.pool_offset, patches, 0,
                            &sim.wheel_ops());
        }
        if (patches.empty()) continue;
        sim.add_pool_patches(name, patches);
        ++written;
        if (dbg) {
            std::fprintf(stderr, "  pool 0x%X -> slot 0x%X%s, %zu word(s)\n",
                         pl.pool_offset, slot->second,
                         by_reference ? " (by reference)" : "", patches.size());
        }
    }
    if (dbg) std::fprintf(stderr, "seed: %zu placement(s) written\n", written);
}
