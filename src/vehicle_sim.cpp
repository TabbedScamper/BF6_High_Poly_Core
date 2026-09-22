#include "vehicle_sim.h"

#include "expression_registry.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <cctype>

namespace bf6 {

bool VehicleSim::open(bf6_ctx* ctx, const std::string& exe, const std::vector<std::string>& graphs,
                      const std::string& skeleton, expression::bf6_ray_trace_fn tracer,
                      void* tracer_user, std::string& err)
{
    graphs_.clear();
    channel_hash_.clear();
    state_.set_allow_writes(true);
    physics_.set_tracer(tracer, tracer_user);
    wheel_.set_tracer(tracer, tracer_user);
    wheel_err_.clear();
    if (!expression::wheel_ops_load_table(exe, wheel_err_) && wheel_err_.empty()) wheel_err_ = "tyre table not loaded";

    std::map<uint32_t, uint32_t> type_size_cache;
    std::vector<bf6_channel_binding> all_bone_binds;
    /* THE SUB-EXPRESSIONS A GRAPH PUSHES.
     *
     * A vehicle's drivetrain graph does not always carry its own engine. A boat's
     * simex lists only hull, drag and force operators; its propulsion lives in
     * SimEx_Sub_AdvancedBoatEngine, named by a relocated pool pointer and pushed as
     * a frame by the state operator. Opening the named graph alongside it is what
     * that push is for - it gives the sub-expression its own state identity, and the
     * two then run against the same channels and the same state. Loading only the
     * graph the vehicle names left the boat afloat with no thrust at all.
     *
     * Names are followed transitively; one the mount does not carry is skipped
     * rather than failing the open, because a graph may name an expression that
     * ships with a mode this vehicle is not in. */
    std::vector<std::string> todo(graphs);
    std::set<std::string> queued(graphs.begin(), graphs.end());
    for (size_t gi = 0; gi < todo.size(); ++gi) {
        const std::string name = todo[gi];
        const bool required = gi < graphs.size();
        std::unique_ptr<G> g(new G());
        g->name = name;
        const uint8_t* raw = nullptr;
        const int64_t n = bf6_read_raw(ctx, BF6_RAW_RES, name.c_str(), &raw);
        if ((n <= 0 || !raw) && !required) continue;
        if (n <= 0 || !raw) { err = "not in the mount: " + name; return false; }
        g->raw.assign(raw, raw + n);
        std::string why;
        if (!expression::parse(g->raw.data(), g->raw.size(), g->graph, why)) {
            if (!required) continue;
            err = name + ": " + why; return false;
        }
        if (!g->graph.exact_record_tiling) {
            if (!required) continue;
            err = name + ": record stream does not tile"; return false;
        }
        if (!expression::make_instance(g->graph, g->inst, why)) {
            if (!required) continue;
            err = name + ": " + why; return false;
        }
        g->inst.trace_records = true;

        /* Each relocated pool pointer that lands on a printable run is an asset
         * reference the loader resolves: "[[guid/guid|Path/To/Expression]]". The
         * path after the bar, lowercased, is the resource name. */
        for (const auto& rel : g->graph.relocations) {
            const std::vector<uint8_t>& pool = g->graph.constant_pool;
            if (rel.target + 4 >= pool.size() || pool[rel.target] != '[') continue;
            size_t end = rel.target;
            while (end < pool.size() && pool[end] >= 32 && pool[end] < 127) ++end;
            const std::string ref((const char*)pool.data() + rel.target, end - rel.target);
            const size_t bar = ref.find('|');
            const size_t close = ref.find("]]");
            if (bar == std::string::npos || close == std::string::npos || close <= bar + 1) continue;
            std::string path = ref.substr(bar + 1, close - bar - 1);
            for (char& ch : path) ch = (char)std::tolower((unsigned char)ch);
            if (path.empty() || !queued.insert(path).second) continue;
            todo.push_back(path);
        }

        /* Bindings, patched into the pool the way the engine does at load. */
        g->binds.assign(256, bf6_channel_binding{});
        const int nb = bf6_expression_channel_bindings(ctx, name.c_str(), g->binds.data(), 256);
        g->binds.resize(nb > 0 ? (size_t)std::min(nb, 256) : 0u);
        for (const auto& b : g->binds) {
            if (b.region == 0) g->inst.pool_patches[b.pool_offset] = b.channel_hash;
            if (b.kind == 0) channel_hash_[b.channel_name] = b.channel_hash;
            else if (b.kind == 2) world_.set_tweakable(b.channel_hash, b.default_bits);
            else all_bone_binds.push_back(b);
            /* every binding name addresses its channel (the moded Vec3 channels such as
             * LinearAcceleration are not kind 0, and were unreachable by name) */
            if (b.kind != 2 && b.channel_name[0]) channel_hash_.emplace(b.channel_name, b.channel_hash);
        }
        /* Inline authored values (curves, constants) the engine writes into the pool
         * at load - the on-disk pool holds placeholders for them. */
        {
            std::vector<uint32_t> po(4096), pv(4096);
            const int nv = bf6_expression_pool_values(ctx, name.c_str(), po.data(), pv.data(), 4096);
            for (int i = 0; i < nv && i < 4096; ++i) g->inst.pool_patches[po[(size_t)i]] = pv[(size_t)i];
        }
        /* Relocated pointers (e.g. a PUSH's feature-path pointer) get their pool
         * target, so each pushed feature has its own state identity. */
        for (const auto& rl : g->graph.relocations)
            g->inst.pool_patches[rl.pointer_field] = rl.target;
        /* Typed-copy sizes from the executable's reflection. */
        for (const auto& grp : g->graph.slot_values) {
            auto it = type_size_cache.find(grp.data_type_id);
            if (it == type_size_cache.end())
                it = type_size_cache.emplace(grp.data_type_id,
                                             bf6_type_size_by_hash(ctx, grp.data_type_id)).first;
            if (it->second) g->inst.type_sizes[grp.data_type_id] = it->second;
        }
        /* Operator names from the executable. */
        std::set<uint32_t> keyset;
        for (const auto& f : g->graph.fixups) keyset.insert(f.key);
        std::vector<uint32_t> keys(keyset.begin(), keyset.end());
        std::vector<expression::NamedOperator> names;
        std::string scan_why;
        expression::resolve_named_operators(exe, keys, names, scan_why);
        for (const auto& row : names) {
            if (row.match_count != 1) continue;
            g->names[row.key] = row.name;
            g->builtins.add(row.key, row.name);
            g->pure.add(row.key, row.name);
            physics_.add(row.key, row.name);
        }
        if (std::getenv("BF6_GRAPH_DEBUG")) {
            std::fprintf(stderr, "graph %s (%zu record(s))%s\n", name.c_str(),
                         g->graph.records.size(), required ? "" : " [sub-expression]");
            for (const auto& b : g->binds)
                if (b.channel_name[0])
                    std::fprintf(stderr, "   channel kind %d %08X %s\n", b.kind,
                                 b.channel_hash, b.channel_name);
        }
        graphs_.push_back(std::move(g));
    }

    /* The rig: bone channel -> rig bone -> pose, modes 0 LOCAL, 1 MODEL, 2 WORLD. */
    if (!skeleton.empty()) {
        std::vector<uint32_t> hs(1024);
        std::vector<int32_t> bs(1024);
        const int nc = bf6_skeleton_channel_bones(ctx, skeleton.c_str(), hs.data(), bs.data(), 1024);
        bf6_skeleton* sk = bf6_skeleton_read(ctx, skeleton.c_str());
        std::map<uint32_t, int32_t> bone_of;
        for (int i = 0; i < nc && i < 1024; ++i) bone_of[hs[(size_t)i]] = bs[(size_t)i];
        if (sk) {
            for (const auto& b : all_bone_binds) {
                const auto bo = bone_of.find(b.channel_hash);
                if (bo == bone_of.end() || bo->second < 0 || bo->second >= sk->bone_count) continue;
                const float* m = sk->bones[bo->second].model;
                const float* l = sk->bones[bo->second].local;
                const float mr[16] = {m[0], m[1], m[2], 0, m[3], m[4], m[5], 0,
                                      m[6], m[7], m[8], 0, m[9], m[10], m[11], 0};
                const float lr[16] = {l[0], l[1], l[2], 0, l[3], l[4], l[5], 0,
                                      l[6], l[7], l[8], 0, l[9], l[10], l[11], 0};
                state_.set_bone_pose(b.channel_hash, 0, lr);
                state_.set_bone_pose(b.channel_hash, 1, mr);
                state_.set_bone_pose(b.channel_hash, 2, mr);
            }
            bf6_free(ctx, sk);
        }
    }
    return true;
}

bool VehicleSim::hash_of(const std::string& name, uint32_t& h) const {
    const auto it = channel_hash_.find(name);
    if (it == channel_hash_.end()) return false;
    h = it->second;
    return true;
}

bool VehicleSim::set_float(const std::string& channel, float v, uint32_t mode) {
    uint32_t h;
    if (!hash_of(channel, h)) return false;
    std::vector<uint8_t> b(4);
    std::memcpy(b.data(), &v, 4);
    state_.set_channel(h, b, mode);
    return true;
}

bool VehicleSim::set_bool(const std::string& channel, bool v) {
    uint32_t h;
    if (!hash_of(channel, h)) return false;
    state_.set_channel(h, std::vector<uint8_t>(1, (uint8_t)(v ? 1 : 0)));
    return true;
}

bool VehicleSim::set_int(const std::string& channel, int32_t v) {
    uint32_t h;
    if (!hash_of(channel, h)) return false;
    std::vector<uint8_t> b(4);
    std::memcpy(b.data(), &v, 4);
    state_.set_channel(h, b);
    return true;
}

bool VehicleSim::set_vec3(const std::string& channel, const float v[3], uint32_t mode) {
    uint32_t h;
    if (!hash_of(channel, h)) return false;
    std::vector<uint8_t> b(16, 0);
    std::memcpy(b.data(), v, 12);
    state_.set_channel(h, b, mode);
    return true;
}

void VehicleSim::set_root(const float rows[16]) {
    state_.set_named_transform(0x5F9C8163u, rows);   /* the RootTransform channel */
}

bool VehicleSim::get_vec3(const std::string& channel, float out[3], uint32_t mode) const {
    out[0] = out[1] = out[2] = 0.0f;
    uint32_t h;
    if (!hash_of(channel, h)) return false;
    const auto& ch = state_.channels();
    const auto it = ch.find(((uint64_t)mode << 32) | h);
    if (it == ch.end() || it->second.size() < 12) return false;
    std::memcpy(out, it->second.data(), 12);
    return true;
}

float VehicleSim::get_float(const std::string& channel, bool* known, uint32_t mode) const {
    if (known) *known = false;
    uint32_t h;
    if (!hash_of(channel, h)) return 0.0f;
    const auto& ch = state_.channels();
    const auto it = ch.find(((uint64_t)mode << 32) | h);
    if (it == ch.end() || it->second.size() < 4) return 0.0f;
    float f;
    std::memcpy(&f, it->second.data(), 4);
    if (known) *known = true;
    return f;
}

std::string VehicleSim::sources(uint32_t offset, int max_depth) const {
    std::string out;
    for (const auto& g : graphs_) {
        std::map<uint32_t, const expression::Record*> by_off;
        for (const auto& rec : g->graph.records) by_off[rec.offset] = &rec;
        if (!by_off.count(offset)) continue;
        const auto& tr = g->inst.trace;
        std::map<uint32_t, std::string> chname;
        for (const auto& b : g->binds) chname[b.pool_offset] = b.channel_name;
        std::set<uint32_t> seen;
        std::function<void(uint32_t, size_t, int, int)> walk = [&](uint32_t roff, size_t before, int depth, int known) {
            const auto it = by_off.find(roff);
            if (it == by_off.end()) return;
            const expression::Record& r = *it->second;
            const auto nm = g->names.find(r.operator_key);
            char line[300];
            std::string what = !r.has_operator ? "MOVE/kind" : nm != g->names.end() ? nm->second : "";
            char kb[24]; std::snprintf(kb, sizeof kb, "0x%08X", r.operator_key);
            std::string extra;
            for (const auto& op : r.operands)
                if (op.region == 0 && chname.count(op.offset)) extra += " [" + chname[op.offset] + "]";
            std::snprintf(line, sizeof line, "%*s%srec 0x%X kind 0x%02X %s %s%s\n", depth * 2, "", known == 0 ? "UNKNOWN " : "", roff, r.kind,
                          r.has_operator ? kb : "", what.c_str(), extra.c_str());
            out += line;
            if (depth >= max_depth || !seen.insert(roff).second) return;
            /* find this record's first trace row index <= before */
            size_t idx = before;
            for (size_t i = before; i-- > 0;) if (tr[i].record_offset == roff) { idx = i; break; }
            for (size_t oi = 0; oi < r.operands.size(); ++oi) {
                const auto& op = r.operands[oi];
                if (op.region != 2) continue;
                long w = -1;
                for (size_t i = idx; i-- > 0;) {
                    const uint32_t wd = tr[i].width ? tr[i].width : 4u;
                    if (tr[i].slot <= op.offset && op.offset < tr[i].slot + wd) { w = (long)i; break; }
                }
                if (w < 0) continue;
                if (tr[(size_t)w].record_offset == roff) continue;   /* its own output */
                if (std::getenv("BF6_SRC_VALUES")) {
                    float fv; std::memcpy(&fv, &tr[(size_t)w].bits, 4);
                    char vb[96];
                    std::snprintf(vb, sizeof vb, "%*s  = slot 0x%X: %g (0x%08X)\n", (depth + 1) * 2, "", op.offset, fv, tr[(size_t)w].bits);
                    out += vb;
                }
                walk(tr[(size_t)w].record_offset, (size_t)w, depth + 1, tr[(size_t)w].known ? 1 : 0);
            }
        };
        walk(offset, tr.size(), 0, -1);
    }
    return out;
}

std::string VehicleSim::list(const std::string& gs, uint32_t lo, uint32_t hi) const {
    std::string out;
    for (const auto& g : graphs_) {
        if (g->name.find(gs) == std::string::npos) continue;
        std::set<uint32_t> ran;
        for (const auto& t : g->inst.trace) ran.insert(t.record_offset);
        for (const auto& r : g->graph.records) {
            if (r.offset < lo || r.offset >= hi) continue;
            const auto nm = g->names.find(r.operator_key);
            char line[160];
            std::snprintf(line, sizeof line, "0x%05X k%02X %-10s %-24s next 0x%X tgt 0x%X%s |", r.offset, r.kind,
                          r.has_operator ? (std::string("0x") + [&]{ char b[9]; std::snprintf(b, 9, "%08X", r.operator_key); return std::string(b); }()).c_str() : "",
                          nm != g->names.end() ? nm->second.c_str() : "", r.next, r.control_target,
                          ran.count(r.offset) ? " RAN" : "");
            out += line;
            for (const auto& op : r.operands) {
                std::snprintf(line, sizeof line, " r%u@0x%X", op.region, op.offset);
                out += line;
            }
            if (r.has_trailing_dword) { std::snprintf(line, sizeof line, " +%u", r.trailing_dword); out += line; }
            out += "\n";
        }
    }
    return out;
}

std::string VehicleSim::instance_layout() const {
    std::string out;
    char line[512];
    for (const auto& g : graphs_) {
        const auto& h = g->graph.header;
        std::snprintf(line, sizeof line, "%s: instance_header %u external_bindings %u buffers %u image %zu slot_file %u\n",
                      g->name.c_str(), h.instance_header_size, h.external_bindings, h.instance_buffer_count,
                      g->inst.image.size(), h.slot_file_size);
        out += line;
        for (const auto& grp : g->graph.instance_values) {
            std::snprintf(line, sizeof line, "  INST type 0x%08X offsets", grp.data_type_id);
            out += line;
            for (uint32_t o : grp.offsets) { std::snprintf(line, sizeof line, " 0x%X", o); out += line; }
            out += "\n";
        }
    }
    return out;
}

std::map<uint32_t, std::string> VehicleSim::channel_name_map() const {
    std::map<uint32_t, std::string> m;
    for (const auto& g : graphs_)
        for (const auto& b : g->binds) m[b.channel_hash] = b.channel_name;
    return m;
}

std::vector<std::string> VehicleSim::channel_names() const {
    std::vector<std::string> out;
    for (const auto& kv : channel_hash_) out.push_back(kv.first);
    return out;
}

std::string VehicleSim::why(const std::string& channel) const {
    uint32_t h;
    if (!hash_of(channel, h)) return channel + ": no graph binds it\n";
    /* The channel write operators (operand 0 = the bound pool entry). */
    static const uint32_t kSetOps[] = {0x6D86C436u, 0xC491CD96u, 0xCC162A96u, 0x95954635u};
    std::map<std::string, int> culprits;
    for (const auto& g : graphs_) {
        std::set<uint32_t> pool_offs;
        for (const auto& b : g->binds)
            if (b.region == 0 && b.channel_hash == h) pool_offs.insert(b.pool_offset);
        if (pool_offs.empty()) continue;
        std::map<uint32_t, const expression::Record*> by_off;
        for (const auto& rec : g->graph.records) by_off[rec.offset] = &rec;
        const auto& tr = g->inst.trace;
        auto last_writer = [&](size_t before, uint32_t slot) -> long {
            for (size_t i = before; i-- > 0;) {
                const uint32_t w = tr[i].width ? tr[i].width : 4u;
                if (tr[i].slot <= slot && slot < tr[i].slot + w) return (long)i;
            }
            return -1;
        };
        auto name_of = [&](uint32_t k) {
            char hb[16];
            std::snprintf(hb, sizeof hb, "0x%08X", k);
            if (k == 0) return std::string("MOVE");
            const auto f = g->names.find(k);
            return f == g->names.end() ? std::string(hb) : std::string(hb) + " " + f->second;
        };
        std::set<size_t> seen;
        std::vector<std::pair<size_t, uint32_t>> stack;   /* (search before, slot) */
        for (const auto& rec : g->graph.records) {
            bool is_set = false;
            for (uint32_t k : kSetOps) is_set = is_set || (rec.has_operator && rec.operator_key == k);
            if (!is_set || rec.operands.empty() || rec.operands[0].region != 0 ||
                !pool_offs.count(rec.operands[0].offset)) continue;
            for (size_t oi = 1; oi < rec.operands.size(); ++oi)
                if (rec.operands[oi].region == 2) stack.push_back({tr.size(), rec.operands[oi].offset});
        }
        int budget = 20000;
        while (!stack.empty() && budget-- > 0) {
            const auto cur = stack.back();
            stack.pop_back();
            const long w = last_writer(cur.first, cur.second);
            char sb[64];
            if (w < 0) {
                std::snprintf(sb, sizeof sb, "slot 0x%X never written (unseeded state)", cur.second);
                culprits[g->name.substr(g->name.rfind('/') + 1) + ": " + sb] += 1;
                continue;
            }
            if (tr[(size_t)w].known || !seen.insert((size_t)w).second) continue;
            const auto rit = by_off.find(tr[(size_t)w].record_offset);
            if (rit == by_off.end()) continue;
            const auto& ops = rit->second->operands;
            int pushed = 0;
            for (const auto& op : ops) {
                if (op.region != 2 || op.offset == tr[(size_t)w].slot) continue;
                const long w2 = last_writer((size_t)w, op.offset);
                if (w2 >= 0 && tr[(size_t)w2].known) continue;
                stack.push_back({(size_t)w, op.offset});
                ++pushed;
            }
            if (!pushed) {
                std::snprintf(sb, sizeof sb, " at rec 0x%X", rit->second->offset);
                culprits[g->name.substr(g->name.rfind('/') + 1) + ": " +
                         name_of(tr[(size_t)w].key) + sb] += 1;
            }
        }
    }
    std::string out;
    for (const auto& kv : culprits) out += "  " + kv.first + (kv.second > 1 ? " x" + std::to_string(kv.second) : "") + "\n";
    return out.empty() ? channel + ": no unknown culprit found\n" : channel + " culprits:\n" + out;
}

std::string VehicleSim::touch(uint32_t region, uint32_t lo, uint32_t hi) const {
    std::string out;
    for (const auto& g : graphs_) {
        std::set<uint32_t> ran;
        for (const auto& t : g->inst.trace) ran.insert(t.record_offset);
        for (const auto& rec : g->graph.records)
            for (size_t oi = 0; oi < rec.operands.size(); ++oi) {
                const auto& op = rec.operands[oi];
                if (op.region != region || op.offset < lo || op.offset >= hi) continue;
                const auto nm = g->names.find(rec.operator_key);
                char line[256];
                std::snprintf(line, sizeof line, "  %s rec 0x%X kind 0x%02X %s op%zu/%zu r%u@0x%X%s\n",
                              g->name.substr(g->name.rfind('/') + 1).c_str(), rec.offset, rec.kind,
                              !rec.has_operator ? "-" : nm != g->names.end() ? nm->second.c_str() : "?",
                              oi, rec.operands.size(), op.region, op.offset,
                              ran.count(rec.offset) ? "  RAN" : "");
                out += line;
                if (rec.has_operator && nm == g->names.end()) {
                    std::snprintf(line, sizeof line, "      key 0x%08X\n", rec.operator_key);
                    out += line;
                }
            }
    }
    return out;
}

std::string VehicleSim::record_info(uint32_t offset) const {
    std::string out;
    for (const auto& g : graphs_) {
        const expression::Record* r = nullptr;
        for (const auto& rec : g->graph.records) if (rec.offset == offset) r = &rec;
        if (!r) continue;
        const auto& tr = g->inst.trace;
        size_t first = tr.size();
        for (size_t i = 0; i < tr.size(); ++i) if (tr[i].record_offset == offset) { first = i; break; }
        char line[256];
        std::snprintf(line, sizeof line, "%s rec 0x%X kind 0x%02X key 0x%08X counted %d in %u out %u ctx %u ran %d\n",
                      g->name.c_str(), offset, r->kind, r->operator_key, (int)r->counted_lists,
                      r->n_in, r->n_out, r->n_ctx, first < tr.size() ? 1 : 0);
        out += line;
        for (size_t oi = 0; oi < r->operands.size(); ++oi) {
            const auto& op = r->operands[oi];
            std::string st = "";
            if (op.region == 2) {
                long w = -1;
                for (size_t i = first; i-- > 0;) {
                    const uint32_t wd = tr[i].width ? tr[i].width : 4u;
                    if (tr[i].slot <= op.offset && op.offset < tr[i].slot + wd) { w = (long)i; break; }
                }
                if (w < 0) st = "never written before";
                else {
                    float fv = 0.0f;
                    const uint32_t bits = tr[(size_t)w].bits;
                    std::memcpy(&fv, &bits, 4);
                    std::snprintf(line, sizeof line, "last write rec 0x%X key 0x%08X %s = %g (0x%08X%s)",
                                  tr[(size_t)w].record_offset, tr[(size_t)w].key,
                                  tr[(size_t)w].known ? "KNOWN" : "UNKNOWN", fv, bits,
                                  tr[(size_t)w].slot == op.offset ? "" : ", not the slot's first word");
                    st = line;
                }
            }
            std::snprintf(line, sizeof line, "  op%zu r%u@0x%X %s\n", oi, op.region, op.offset, st.c_str());
            out += line;
        }
    }
    return out;
}

void VehicleSim::tick() {
    report_.clear();
    for (auto& g : graphs_) {
        g->inst.trace.clear();
        expression::ChainHost chain;
        chain.add(&g->builtins);
        chain.add(&g->pure);
        chain.add(&recovered_);
        chain.add(&wheel_);
        chain.add(&world_);
        chain.add(&physics_);
        chain.add(&state_);
        const auto r = expression::evaluate(g->graph, &g->inst, {}, &chain);
        /* BF6_SLOT_WATCH=hex,hex: every write this run to those slots of the
         * drivetrain graph, with the value and the writing record. */
        if (const char* at = std::getenv("BF6_RAN_AT"))
            if (g->name.find("simex") != std::string::npos) {
                static int ticks = 0;
                if (++ticks == std::atoi(at)) {
                    std::set<uint32_t> ran;
                    for (const auto& t : g->inst.trace) ran.insert(t.record_offset);
                    uint32_t lo = 0, prev = 0;
                    bool open = false;
                    std::string o = "ran ranges:";
                    for (const auto& rec : g->graph.records) {
                        const bool r = ran.count(rec.offset) != 0;
                        if (r && !open) { lo = rec.offset; open = true; }
                        if (!r && open) {
                            char b[32]; std::snprintf(b, sizeof b, " %X-%X", lo, prev); o += b; open = false;
                        }
                        if (r) prev = rec.offset;
                    }
                    std::fprintf(stderr, "%s\n", o.c_str());
                }
            }
        if (const char* watch = std::getenv("BF6_SLOT_WATCH"))
            if (g->name.find("simex") != std::string::npos) {
                static int calls = 0;
                if (++calls % 60 == 0)
                    for (const char* p = watch; *p;) {
                        char* end = nullptr;
                        const unsigned long slot = std::strtoul(p, &end, 16);
                        if (end == p) break;
                        for (const auto& t : g->inst.trace)
                            if (t.slot == slot) {
                                float f; std::memcpy(&f, &t.bits, 4);
                                std::fprintf(stderr, "watch tick %d slot 0x%lX rec 0x%X key %08X %s %g\n", calls, slot,
                                             t.record_offset, t.key, t.known ? "known" : "UNKNOWN", f);
                            }
                        p = *end ? end + 1 : end;
                    }
            }
        char line[256];
        std::snprintf(line, sizeof line, "%s: termination %d steps %u guessed %u unresolved %zu\n",
                      g->name.c_str(), (int)r.termination, r.steps, r.guessed_branches,
                      r.unresolved_keys.size());
        report_ += line;
        if (!r.unresolved_keys.empty()) {
            std::map<uint32_t, int> uk;
            for (uint32_t k : r.unresolved_keys) ++uk[k];
            std::string ul = "  unresolved:";
            for (const auto& kv : uk) {
                char kb[32];
                std::snprintf(kb, sizeof kb, " %08X x%d", kv.first, kv.second);
                ul += kb;
            }
            report_ += ul + "\n";
        }
        for (const auto& d : r.diagnostics)
            if (d.find(" @") != std::string::npos) {
                const unsigned long off = std::stoul(d.substr(d.rfind('@') + 1));
                std::snprintf(line, sizeof line, "  guessed at rec 0x%lX: %s\n", off, d.substr(0, d.find(" @")).c_str());
                report_ += line;
            }
    }
}

} // namespace bf6
