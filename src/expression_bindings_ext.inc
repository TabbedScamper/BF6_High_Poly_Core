/* ------------------------------------ DiceExpression channel bindings
 *
 * WHAT A GRAPH'S "CONTEXT ACCESSOR" CONSTANTS ARE. The operators filed in
 * EngineNodes.tsv as "fetch the current thread's evaluation context" take, as
 * operand 0, a constant-pool entry that is ZERO ON DISK. The graph's own EBX
 * lists which PUBLIC CHANNEL each such entry is bound to: an array of
 * {import of a channel record, {0, pool byte offset}}. At load the engine writes
 * the channel's handle into the pool there. Measured on the flyer60 suspension:
 * pool 0xA98 IsSleeping (read by 0x59F02977), 0xAA0 AngularVelocity_Wheel_FL
 * (0x18B2987B), 0xAB0 RootTransform (0xABAAAD01), 0xAC8 SpringCompression_A1
 * (0x6D86C436), 0xB28 GearNumber (0x45C08E62) - every offset the table names is
 * exactly operand 0 of one of those operators.
 *
 * Field name hashes are the retail ones (names are stripped); each is checked
 * for shape before it is trusted, and anything that does not fit is skipped
 * rather than guessed:
 *   0xa2734e52  the binding array
 *   0x8cf424e7  import of the channel record
 *   0xfc636c1f  {0xbdf81e34 = 0, 0x4ff541e4 = pool byte offset}
 *   channel record: 0xcdc6f6a3 = channel hash, 0x0c59fa06 = channel name
 *
 * BONE BINDINGS, same shape, field 0xc939b436: {0xfc636c1f location, 0x8cf424e7
 * {0x731e0047 import of a bone channel record, 0xe77520d2}}. The pool entries they
 * name are the 24-byte-stride ExpressionBoneId constants the part-transform reader
 * 0x04BEFF62 takes, zero on disk; bone record: 0x0c59fa06 name, 0xc124c7d8 hash. */

namespace {

void bf6__find_binding_arrays(const bf6::EbxValue& v, std::vector<const bf6::EbxValue*>& out,
                              int depth)
{
    if (depth > 12) return;
    if (v.kind == bf6::EbxValue::Kind::Struct) {
        for (const auto& kv : v.fields) {
            if ((kv.first == 0xa2734e52u || kv.first == 0xc939b436u) &&
                kv.second.kind == bf6::EbxValue::Kind::Array)
                out.push_back(&kv.second);
            bf6__find_binding_arrays(kv.second, out, depth + 1);
        }
    } else if (v.kind == bf6::EbxValue::Kind::Array) {
        for (const auto& it : v.items) bf6__find_binding_arrays(it, out, depth + 1);
    }
}

} // namespace

int bf6_expression_channel_bindings(bf6_ctx* c, const char* ebx_name,
                                    bf6_channel_binding* out, int out_max)
{
    if (!c || !ebx_name) return -1;
    std::string err;
    if (!ensure_types(c, err)) return -1;
    std::vector<uint8_t> raw = c->src.get_ebx(std::string(ebx_name) + ".ebx", err);
    if (raw.empty()) raw = c->src.get_ebx(ebx_name, err);
    if (raw.empty()) return -1;
    bf6::Ebx e(*c->types);
    e.set_guid_index(&c->src.partition_index());
    if (!e.parse(std::move(raw), err)) return -1;

    /* Channel partitions are shared by every graph; parse each one once per call. */
    struct ChanRec { uint32_t hash = 0; std::string name; int32_t tweak = 0; uint32_t bits = 0; int32_t is_bool = 0; };
    std::map<std::string, std::map<std::string, ChanRec>> chan_cache;
    auto channel_of = [&](const std::string& path, const std::string& inst,
                          ChanRec& rec) -> bool {
        auto pit = chan_cache.find(path);
        if (pit == chan_cache.end()) {
            std::map<std::string, ChanRec> m;
            std::string e2;
            /* An import path carries ".ebx"; the mount is keyed without it. */
            std::string bare = path;
            if (bare.size() > 4 && bare.compare(bare.size() - 4, 4, ".ebx") == 0)
                bare.resize(bare.size() - 4);
            std::vector<uint8_t> r2 = c->src.get_ebx(bare, e2);
            if (r2.empty()) r2 = c->src.get_ebx(path, e2);
            bf6::Ebx ce(*c->types);
            const size_t r2n = r2.size();
            const bool parsed = !r2.empty() && ce.parse(std::move(r2), e2);
            if (std::getenv("BF6_BIND_DEBUG"))
                std::fprintf(stderr, "bind: channel partition %s bytes %zu parsed %d instances %zu err %s\n",
                             path.c_str(), r2n, (int)parsed, parsed ? ce.instance_count() : 0u, e2.c_str());
            if (parsed) {
                for (size_t i = 0; i < ce.instance_count(); ++i) {
                    const std::string g = ce.instance_guid(i);
                    if (g.empty()) continue;
                    const bf6::EbxValue v = ce.read_instance(i);
                    const bf6::EbxValue* h = v.field(0xcdc6f6a3u);   /* public channel */
                    if (!h) h = v.field(0xc124c7d8u);                 /* bone channel   */
                    const bf6::EbxValue* n = v.field(0x0c59fa06u);
                    if (!h || !n || n->kind != bf6::EbxValue::Kind::Str) continue;
                    ChanRec r;
                    r.hash = (uint32_t)(h->kind == bf6::EbxValue::Kind::Int ? h->i : h->u);
                    r.name = n->s;
                    /* A TWEAKABLE record (tweakables databases): no public-channel
                     * field, the hash in 0xc124c7d8, its default in 0x42c8b257
                     * (then 0xc06d0cbb min, 0x3ff76ddb max). Verified on
                     * Gauntlet_RB_Relay_BoostMultiplier: 1 in [1, 10]. */
                    const bf6::EbxValue* dv = v.field(0x42c8b257u);
                    if (!v.field(0xcdc6f6a3u) && dv) {
                        r.tweak = 1;
                        if (dv->kind == bf6::EbxValue::Kind::Bool) { r.bits = dv->b ? 1u : 0u; r.is_bool = 1; }
                        else {
                            const float fv = dv->kind == bf6::EbxValue::Kind::Real ? (float)dv->f
                                           : dv->kind == bf6::EbxValue::Kind::Int ? (float)dv->i : (float)dv->u;
                            std::memcpy(&r.bits, &fv, 4);
                        }
                    }
                    m[g] = r;
                }
            }
            pit = chan_cache.emplace(path, std::move(m)).first;
        }
        const auto it = pit->second.find(inst);
        if (it == pit->second.end()) return false;
        rec = it->second;
        return true;
    };

    int n = 0;
    for (size_t i = 0; i < e.instance_count(); ++i) {
        const bf6::EbxValue v = e.read_instance(i);
        std::vector<const bf6::EbxValue*> arrays;
        bf6__find_binding_arrays(v, arrays, 0);
        if (std::getenv("BF6_BIND_DEBUG") && !arrays.empty())
            std::fprintf(stderr, "bind: instance %zu has %zu binding arrays, first %zu items, item kind %d\n",
                         i, arrays.size(), arrays[0]->items.size(),
                         arrays[0]->items.empty() ? -1 : (int)arrays[0]->items[0].kind);
        for (const bf6::EbxValue* arr : arrays)
            for (const bf6::EbxValue& item : arr->items) {
                const bf6::EbxValue* imp = item.field(0x8cf424e7u);
                const bf6::EbxValue* loc = item.field(0xfc636c1fu);
                int32_t kind = 0;
                /* BONE bindings (0xc939b436): the import sits one level down, under
                 * 0x731e0047, and names a bone channel record in the vehicle's
                 * bd_<vehicle> partition. Entries of other shapes are skipped. */
                if (imp && imp->kind == bf6::EbxValue::Kind::Struct) {
                    imp = imp->field(0x731e0047u);
                    kind = 1;
                }
                if (!imp || imp->kind != bf6::EbxValue::Kind::ImportRef || !loc) continue;
                const bf6::EbxValue* region = loc->field(0xbdf81e34u);
                const bf6::EbxValue* off = loc->field(0x4ff541e4u);
                if (!off || !region) continue;
                ChanRec rec;
                if (!channel_of(imp->import_path, imp->import_instance, rec)) {
                    if (std::getenv("BF6_BIND_DEBUG"))
                        std::fprintf(stderr, "bind: no channel for %s #%s (%zu in cache) pool 0x%llX\n",
                                     imp->import_path.c_str(), imp->import_instance.c_str(),
                                     chan_cache[imp->import_path].size(),
                                     (unsigned long long)(off->kind == bf6::EbxValue::Kind::Int ? off->i : (int64_t)off->u));
                    continue;
                }
                if (std::getenv("BF6_BIND_DEBUG"))
                    std::fprintf(stderr, "bind: %s <- %s #%s\n", rec.name.c_str(),
                                 imp->import_path.c_str(), imp->import_instance.c_str());
                if (out && n < out_max) {
                    bf6_channel_binding& b = out[n];
                    b.kind = kind == 0 && rec.tweak ? 2 : kind;
                    b.default_bits = rec.bits;
                    b.default_is_bool = rec.is_bool;
                    b.region = (int32_t)(region->kind == bf6::EbxValue::Kind::Int ? region->i : (int64_t)region->u);
                    b.pool_offset = (uint32_t)(off->kind == bf6::EbxValue::Kind::Int ? off->i : (int64_t)off->u);
                    b.channel_hash = rec.hash;
                    std::snprintf(b.channel_name, sizeof b.channel_name, "%s", rec.name.c_str());
                }
                ++n;
            }
    }
    return n;
}

/* A skeleton's BONE-CHANNEL table: which rig bone each bone channel (a record in
 * the vehicle's bd_<vehicle> partition, by its hash) drives. Field 0x05a9fb0c on
 * the skeleton asset: an array of {0xcdc6f6a3 channel hash, 0x2b6f2936 bone index}.
 * Verified on the flyer60: LeftRearWheelHub -> bone 84 WheelHub_RearLeft,
 * RightRearWheelHub -> 90 WheelHub_RearRight, SteeringRack -> 54 SteeringRack. */
int bf6_skeleton_channel_bones(bf6_ctx* c, const char* ske_ebx, uint32_t* hashes,
                               int32_t* bones, int out_max)
{
    if (!c || !ske_ebx) return -1;
    std::string err;
    if (!ensure_types(c, err)) return -1;
    std::vector<uint8_t> raw = c->src.get_ebx(std::string(ske_ebx) + ".ebx", err);
    if (raw.empty()) raw = c->src.get_ebx(ske_ebx, err);
    if (raw.empty()) return -1;
    bf6::Ebx e(*c->types);
    if (!e.parse(std::move(raw), err)) return -1;
    int n = 0;
    for (size_t i = 0; i < e.instance_count(); ++i) {
        const bf6::EbxValue v = e.read_instance(i);
        /* TWO tables of the same {hash, bone} shape: 0x05a9fb0c and 0xe47d33b9.
         * The flyer60's bd channels are in the first; the aav7a1's first is empty
         * and its channels are in the second. Both are read. */
        const bf6::EbxValue* arrs[2] = {v.field(0x05a9fb0cu), v.field(0xe47d33b9u)};
        bool any = false;
        for (const bf6::EbxValue* arr : arrs) {
        if (!arr || arr->kind != bf6::EbxValue::Kind::Array) continue;
        any = true;
        for (const bf6::EbxValue& it : arr->items) {
            const bf6::EbxValue* h = it.field(0xcdc6f6a3u);
            const bf6::EbxValue* b = it.field(0x2b6f2936u);
            if (!h || !b) continue;
            if (n < out_max) {
                if (hashes) hashes[n] = (uint32_t)(h->kind == bf6::EbxValue::Kind::Int ? h->i : (int64_t)h->u);
                if (bones) bones[n] = (int32_t)(b->kind == bf6::EbxValue::Kind::Int ? b->i : (int64_t)b->u);
            }
            ++n;
        }
        }
        if (any) break;
    }
    return n;
}

/* INLINE POOL VALUES. The same binding arrays (0xa2734e52 and 0xc939b436) also hold
 * entries whose 0x8cf424e7 is an INLINE VALUE rather than an import: the engine
 * writes that value into the constant pool at the location's offset when the graph
 * loads. Measured on simex_car_flyer60: 11 SimpleCurveFixed curves (type
 * 0xECC62DCB, 15 {x, y} points of 8 bytes, unused points -666666) at pool 0x200 ..
 * 0x700, among them the steering curves at 0x480 and 0x580, whose on-disk pool copy
 * is a placeholder (0, 0) + sentinels. Flattened field by field in declaration
 * order: a {x, y} struct is 2 floats, {x, y, z} 3 floats + 4 bytes of padding (Vec3
 * is 16 bytes), {x, y, z, w} 4 floats; scalars 4 bytes. Returns the dword count. */
namespace {
bool bf6__flatten_value(const bf6::EbxValue& v, std::vector<uint32_t>& out, int depth) {
    if (depth > 6) return false;
    switch (v.kind) {
    case bf6::EbxValue::Kind::Real: { const float f = (float)v.f; uint32_t b; std::memcpy(&b, &f, 4); out.push_back(b); return true; }
    case bf6::EbxValue::Kind::Int:  out.push_back((uint32_t)(int32_t)v.i); return true;
    case bf6::EbxValue::Kind::Uint: out.push_back((uint32_t)v.u); return true;
    case bf6::EbxValue::Kind::Bool: out.push_back(v.b ? 1u : 0u); return true;
    case bf6::EbxValue::Kind::Struct: {
        const bool is_vec3 = v.fields.size() == 3 && v.field(0x3901db14u) && v.field(0x42fc0f5eu) && v.field(0x32a99b9cu);
        for (const auto& kv : v.fields)
            if (!bf6__flatten_value(kv.second, out, depth + 1)) return false;
        if (is_vec3) out.push_back(0u);
        return true;
    }
    default: return false;
    }
}
}

int bf6_expression_pool_values(bf6_ctx* c, const char* ebx_name, uint32_t* offsets, uint32_t* values, int out_max)
{
    if (!c || !ebx_name) return -1;
    std::string err;
    if (!ensure_types(c, err)) return -1;
    std::vector<uint8_t> raw = c->src.get_ebx(std::string(ebx_name) + ".ebx", err);
    if (raw.empty()) raw = c->src.get_ebx(ebx_name, err);
    if (raw.empty()) return -1;
    bf6::Ebx e(*c->types);
    e.set_guid_index(&c->src.partition_index());
    if (!e.parse(std::move(raw), err)) return -1;
    int n = 0;
    for (size_t i = 0; i < e.instance_count(); ++i) {
        const bf6::EbxValue v = e.read_instance(i);
        std::vector<const bf6::EbxValue*> arrays;
        bf6__find_binding_arrays(v, arrays, 0);
        for (const bf6::EbxValue* arr : arrays)
            for (const bf6::EbxValue& item : arr->items) {
                const bf6::EbxValue* val = item.field(0x8cf424e7u);
                const bf6::EbxValue* loc = item.field(0xfc636c1fu);
                if (!val || !loc || val->kind != bf6::EbxValue::Kind::Struct) continue;
                if (val->field(0x731e0047u)) continue;          /* a bone binding, not a value */
                const bf6::EbxValue* region = loc->field(0xbdf81e34u);
                const bf6::EbxValue* off = loc->field(0x4ff541e4u);
                if (!region || !off) continue;
                const int64_t reg = region->kind == bf6::EbxValue::Kind::Int ? region->i : (int64_t)region->u;
                if (reg != 0) continue;
                const uint32_t base = (uint32_t)(off->kind == bf6::EbxValue::Kind::Int ? off->i : (int64_t)off->u);
                std::vector<uint32_t> dw;
                if (!bf6__flatten_value(*val, dw, 0)) continue;
                for (size_t k = 0; k < dw.size(); ++k) {
                    if (offsets && values && n < out_max) {
                        offsets[n] = base + 4u * (uint32_t)k;
                        values[n] = dw[k];
                    }
                    ++n;
                }
            }
    }
    return n;
}
