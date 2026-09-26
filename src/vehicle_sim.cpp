#include "vehicle_sim.h"
#include <chrono>

#include "expression_registry.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <cctype>
#include <cmath>
#include <algorithm>

extern "C" uint32_t bf6__type_field_offsets_by_hash(bf6_ctx*, uint32_t, uint32_t*, uint32_t);

namespace bf6 {

bool VehicleSim::open(bf6_ctx* ctx, const std::string& exe, const std::vector<std::string>& graphs,
                      const std::string& skeleton, expression::bf6_ray_trace_fn tracer,
                      void* tracer_user, std::string& err)
{
    graphs_.clear();
    channel_hash_.clear();
    bone_identity_.clear();
    presented_bones_.clear();
    host_set_.clear();
    host_values_.clear();
    primary_written_.clear();
    primary_written_ready_ = false;
    frame_ids_.clear();
    frame_ids_taken_.clear();
    frame_ids_presentation_.clear();
    bone_binds_.clear();
    rig_names_.clear();
    rig_parents_.clear();
    motion_.clear();
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
        g->inst.trace_records = std::getenv("BF6_NO_TRACE") == nullptr;

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
            if (b.kind == 1) bone_binds_[b.channel_hash] = b.channel_name;
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
            /* BF6_POOL_AT=<hex>: whether one pool word is among the authored values the
             * engine writes at load, and what it holds. A pool word nobody patches keeps
             * its on-disk placeholder, usually zero, and an unpatched zero is
             * indistinguishable from an authored zero without asking this question. */
            if (const char* at = std::getenv("BF6_POOL_AT")) {
                const uint32_t want = (uint32_t)std::strtoul(at, nullptr, 16);
                bool found = false;
                for (int i = 0; i < nv && i < 4096; ++i)
                    if (po[(size_t)i] == want) {
                        float f;
                        std::memcpy(&f, &pv[(size_t)i], 4);
                        std::fprintf(stderr, "pool 0x%X in %s: PATCHED to 0x%08X (%g)\n",
                                     want, name.c_str(), pv[(size_t)i], f);
                        found = true;
                    }
                if (!found)
                    std::fprintf(stderr, "pool 0x%X in %s: NOT among the %d authored values,"
                                         " keeps its on-disk placeholder\n", want, name.c_str(), nv);
            }
        }
        /* Relocated pointers (e.g. a PUSH's feature-path pointer) get their pool
         * target, so each pushed feature has its own state identity.
         *
         * ONE IDENTITY PER FEATURE ACROSS THE VEHICLE'S GRAPHS. The engine keeps
         * state per entity and feature; a derived graph (derivedex_*) reads the
         * drivetrain's replicated state and republishes it as channels. Keyed by
         * each graph's own pool offset, the F-16's derived graph pushed its control
         * surface feature under a different frame from the simex that wrote it, read
         * nothing, and wrote zeros over the elevators and rudder. A feature path now
         * keeps the id the first graph (the simex) gave it; a path new to a later
         * graph keeps its own offset unless another path already owns that id. */
        /* AN INSTANCE IS A PATH AND ITS OCCURRENCE. One graph holds a path once per
         * instance - simex_sub_carwheel four times, one per wheel, each its own state -
         * so the shared key is (path, the rank of this copy's pool offset among that
         * path's copies in this graph). The k-th wheel of the derived graph meets the
         * k-th wheel of the simex; the four wheels stay four. */
        std::map<std::string, std::vector<uint32_t>> copies;
        auto path_at = [&](uint32_t target) {
            const auto& pool = g->graph.constant_pool;
            std::string path;
            if (target < pool.size()) {
                size_t end = target;
                while (end < pool.size() && pool[end] >= 32 && pool[end] < 127) ++end;
                path.assign((const char*)pool.data() + target, end - target);
                for (char& ch : path) ch = (char)std::tolower((unsigned char)ch);
            }
            return path;
        };
        for (const auto& rl : g->graph.relocations) {
            const std::string p = path_at(rl.target);
            if (p.size() < 4) continue;
            auto& v = copies[p];
            if (std::find(v.begin(), v.end(), rl.target) == v.end()) v.push_back(rl.target);
        }
        for (auto& kv : copies) std::sort(kv.second.begin(), kv.second.end());
        for (const auto& rl : g->graph.relocations) {
            uint32_t id = rl.target;
            {
                const std::string base = path_at(rl.target);
                if (base.size() >= 4) {
                    const auto& v = copies[base];
                    const size_t rank = (size_t)(std::find(v.begin(), v.end(), rl.target) - v.begin());
                    /* TWO PRESENTATION GRAPHS ARE TWO INSTANCES. The CV90's primary and
                     * rocket moving-parts graphs each embed a struct of the same path, and
                     * sharing its identity let one graph reset the other's arm position
                     * every tick. A presentation graph reuses an identity only when the
                     * drivetrain or a derived graph made it. */
                    const bool presentation = name.find("/presex_") != std::string::npos;
                    std::string path = base + "#" + std::to_string(rank);
                    if (presentation && frame_ids_presentation_.count(path) &&
                        !std::getenv("BF6_PRESEX_SHARE_FRAMES"))
                        path += "@" + name;
                    const auto known = frame_ids_.find(path);
                    if (known != frame_ids_.end()) {
                        id = known->second;
                        if (std::getenv("BF6_FRAME_IDS"))
                            std::fprintf(stderr, "frame %04X shared by %s: %s (own offset %X)\n", id,
                                         name.substr(name.rfind('/') + 1).c_str(), path.c_str(), rl.target);
                    } else {
                        if (frame_ids_taken_.count(id)) {
                            id = 0xF000u;
                            while (frame_ids_taken_.count(id)) ++id;
                        }
                        frame_ids_[path] = id;
                        frame_ids_taken_.insert(id);
                        if (presentation) frame_ids_presentation_.insert(path);
                    }
                }
            }
            g->inst.pool_patches[rl.pointer_field] = id;
            /* A CONDITION CONFIG: a pool word that is both a channel binding and a
             * relocated pointer is an entity-query config (0xE88A04DB) whose condition
             * reads that channel - the RHIB's is HealthState. The pointer wins the pool
             * word, so the host is told which channel the pointed-at config tests. */
            for (const auto& b : g->binds)
                if (b.region == 0 && b.kind == 0 && b.pool_offset == rl.pointer_field)
                    state_.map_condition_channel(id, b.channel_hash);
        }
        /* Typed-copy sizes from the executable's reflection. */
        for (const auto& grp : g->graph.slot_values) {
            auto it = type_size_cache.find(grp.data_type_id);
            if (it == type_size_cache.end())
                it = type_size_cache.emplace(grp.data_type_id,
                                             bf6_type_size_by_hash(ctx, grp.data_type_id)).first;
            if (it->second) g->inst.type_sizes[grp.data_type_id] = it->second;
            if (!g->inst.type_fields.count(grp.data_type_id)) {
                uint32_t offs[256];
                const uint32_t n = bf6__type_field_offsets_by_hash(ctx, grp.data_type_id, offs, 256);
                if (n && n <= 256)
                    g->inst.type_fields[grp.data_type_id].assign(offs, offs + n);
            }
        }
        /* Operator names from the executable. */
        std::set<uint32_t> keyset;
        for (const auto& f : g->graph.fixups) keyset.insert(f.key);
        std::vector<uint32_t> keys(keyset.begin(), keyset.end());
        /* THE CLOSED SET FIRST. Resolving a key against every printable literal in
         * the executable withholds any key whose name collides with an unrelated
         * string, and three of a boat's plainest operators - AddFloat3,
         * GreaterThanFloat, LessThanFloat - were withheld for exactly that reason
         * while the graph could not get a number out of them. The named-builtin
         * descriptors give the operator names and nothing else, so a key resolved
         * against them is resolved for good. The literal scan still runs for
         * whatever the descriptors do not cover. */
        std::vector<expression::NamedOperator> names;
        std::string scan_why;
        {
            const auto t0 = std::chrono::steady_clock::now();
            expression::resolve_named_operators(exe, keys, names, scan_why);
            if (std::getenv("BF6_OPEN_TIMING"))
                std::fprintf(stderr, "  resolve_named_operators %s: %.1f ms (%zu keys)\n",
                             name.substr(name.rfind('/') + 1).c_str(),
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                             keys.size());
        }
        {
            static std::map<std::string, std::vector<expression::NamedBuiltin>> cache;
            auto it = cache.find(exe);
            if (it == cache.end()) {
                std::vector<expression::NamedBuiltin> rows;
                std::string berr;
                expression::read_named_builtins(exe, rows, berr);
                if (std::getenv("BF6_GRAPH_DEBUG"))
                    std::fprintf(stderr, "named builtins: %zu%s%s\n", rows.size(),
                                 berr.empty() ? "" : " - ", berr.c_str());
                it = cache.emplace(exe, std::move(rows)).first;
            }
            std::set<uint32_t> want(keys.begin(), keys.end());
            for (const auto& row : it->second) {
                if (!want.count(row.key)) continue;
                g->names[row.key] = row.name;
                g->builtins.add(row.key, row.name);
                g->pure.add(row.key, row.name);
                physics_.add(row.key, row.name);
            }
        }
        for (const auto& row : names) {
            if (row.match_count != 1) continue;
            if (g->names.count(row.key)) continue;   /* the closed set already named it */
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
            for (int32_t index = 0; index < sk->bone_count; ++index)
            {
                rig_names_.push_back(sk->bones[index].name ? sk->bones[index].name : "");
                rig_parents_.push_back(sk->bones[index].parent);
            }
            for (int32_t index = 0; index < sk->bone_count; ++index) {
                const float* m = sk->bones[index].model;
                const float* l = sk->bones[index].local;
                const float mr[16] = {m[0], m[1], m[2], 0, m[3], m[4], m[5], 0,
                                      m[6], m[7], m[8], 0, m[9], m[10], m[11], 0};
                const float lr[16] = {l[0], l[1], l[2], 0, l[3], l[4], l[5], 0,
                                      l[6], l[7], l[8], 0, l[9], l[10], l[11], 0};
                state_.set_skeleton_bone(index, sk->bones[index].parent, lr, mr);
            }
            for (const auto& mapped : bone_of) {
                const int32_t index = mapped.second;
                if (index < 0 || index >= sk->bone_count) continue;
                state_.map_skeleton_bone(mapped.first, index);
                bone_identity_[mapped.first] = BoneIdentity{index,
                    sk->bones[index].name ? sk->bones[index].name : ""};
            }
            bf6_free(ctx, sk);
        }
    }
    return true;
}

int VehicleSim::add_skeleton(bf6_ctx* ctx, const std::string& skeleton) {
    bf6_skeleton* sk = bf6_skeleton_read(ctx, skeleton.c_str());
    if (!sk) return 0;
    std::vector<uint32_t> hs(1024);
    std::vector<int32_t> bs(1024);
    const int nc = bf6_skeleton_channel_bones(ctx, skeleton.c_str(), hs.data(), bs.data(), 1024);
    const int32_t offset = (int32_t)rig_names_.size();
    for (int32_t index = 0; index < sk->bone_count; ++index) {
        const int32_t parent = sk->bones[index].parent >= 0 ? sk->bones[index].parent + offset : -1;
        rig_names_.push_back(sk->bones[index].name ? sk->bones[index].name : "");
        rig_parents_.push_back(parent);
        const float* m = sk->bones[index].model;
        const float* l = sk->bones[index].local;
        const float mr[16] = {m[0], m[1], m[2], 0, m[3], m[4], m[5], 0,
                              m[6], m[7], m[8], 0, m[9], m[10], m[11], 0};
        const float lr[16] = {l[0], l[1], l[2], 0, l[3], l[4], l[5], 0,
                              l[6], l[7], l[8], 0, l[9], l[10], l[11], 0};
        state_.set_skeleton_bone(offset + index, parent, lr, mr);
    }
    for (int i = 0; i < nc && i < 1024; ++i) {
        const int32_t index = bs[(size_t)i];
        if (index < 0 || index >= sk->bone_count || bone_identity_.count(hs[(size_t)i])) continue;
        state_.map_skeleton_bone(hs[(size_t)i], offset + index);
        bone_identity_[hs[(size_t)i]] = BoneIdentity{offset + index,
            sk->bones[index].name ? sk->bones[index].name : ""};
    }
    const int added = sk->bone_count;
    bf6_free(ctx, sk);
    return added;
}

void VehicleSim::isolate_graphs(const std::vector<std::string>& names) {
    uint32_t next = 0xE000u;
    for (auto& g : graphs_)
        if (std::find(names.begin(), names.end(), g->name) != names.end()) g->owner = next++;
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
    host_set_.insert(channel);
    std::vector<uint8_t> b(4);
    std::memcpy(b.data(), &v, 4);
    state_.set_channel(h, b, mode);
    host_values_[{h, mode}] = b;
    return true;
}

bool VehicleSim::set_bool(const std::string& channel, bool v) {
    uint32_t h;
    if (!hash_of(channel, h)) return false;
    host_set_.insert(channel);
    state_.set_channel(h, std::vector<uint8_t>(1, (uint8_t)(v ? 1 : 0)));
    host_values_[{h, 0u}] = std::vector<uint8_t>(1, (uint8_t)(v ? 1 : 0));
    return true;
}

bool VehicleSim::set_int(const std::string& channel, int32_t v) {
    uint32_t h;
    if (!hash_of(channel, h)) return false;
    host_set_.insert(channel);
    std::vector<uint8_t> b(4);
    std::memcpy(b.data(), &v, 4);
    state_.set_channel(h, b);
    host_values_[{h, 0u}] = b;
    return true;
}

bool VehicleSim::set_vec3(const std::string& channel, const float v[3], uint32_t mode) {
    uint32_t h;
    if (!hash_of(channel, h)) return false;
    host_set_.insert(channel);
    std::vector<uint8_t> b(16, 0);
    std::memcpy(b.data(), v, 12);
    state_.set_channel(h, b, mode);
    host_values_[{h, mode}] = b;
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

/* WHO WRITES A CHANNEL: every channel-set record bound to it, in every graph, with
 * the tree of records that fed its value on the last tick (sources). */
std::string VehicleSim::writers(const std::string& channel, int depth) const {
    uint32_t h;
    if (!hash_of(channel, h)) return channel + ": no graph binds it\n";
    static const uint32_t kSetOps[] = {0x6D86C436u, 0xC491CD96u, 0xCC162A96u, 0x95954635u};
    std::string out;
    for (const auto& g : graphs_) {
        std::set<uint32_t> pool_offs;
        for (const auto& b : g->binds)
            if (b.region == 0 && b.channel_hash == h) pool_offs.insert(b.pool_offset);
        if (pool_offs.empty()) continue;
        for (const auto& rec : g->graph.records) {
            bool is_set = false;
            for (uint32_t k : kSetOps) is_set = is_set || (rec.has_operator && rec.operator_key == k);
            if (!is_set || rec.operands.empty() || rec.operands[0].region != 0 ||
                !pool_offs.count(rec.operands[0].offset)) continue;
            char hb[160];
            std::snprintf(hb, sizeof hb, "== %s writes %s at record 0x%X\n",
                          g->name.substr(g->name.rfind('/') + 1).c_str(), channel.c_str(), rec.offset);
            out += hb;
            out += sources(rec.offset, depth);
        }
    }
    return out.empty() ? channel + ": bound, but no graph writes it\n" : out;
}

/* WHO READS A CHANNEL: every record in every graph with an operand on one of the
 * channel's bound pool entries, plus the records that follow it (the use). */
std::string VehicleSim::readers(const std::string& channel, int follow) const {
    uint32_t h;
    if (!hash_of(channel, h)) return channel + ": no graph binds it" + std::string(1, (char)10);
    std::string out;
    for (const auto& g : graphs_) {
        std::set<uint32_t> pool_offs;
        for (const auto& b : g->binds)
            if (b.region == 0 && b.channel_hash == h) pool_offs.insert(b.pool_offset);
        if (pool_offs.empty()) continue;
        const std::string gshort = g->name.substr(g->name.rfind('/') + 1);
        for (const auto& rec : g->graph.records) {
            bool hit = false;
            for (const auto& op : rec.operands) hit = hit || (op.region == 0 && pool_offs.count(op.offset));
            if (!hit) continue;
            out += "== " + gshort + " uses " + channel + ":" + std::string(1, (char)10);
            out += list(gshort, rec.offset, rec.offset + (uint32_t)follow);
        }
    }
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
    state_.begin_bone_tick();
    for (auto& g : graphs_) {
        g->inst.trace.clear();
        if (std::getenv("BF6_FRAME_LEAK") && state_.frame_depth())
            std::fprintf(stderr, "frame stack depth %d leaked into %s\n", state_.frame_depth(), g->name.c_str());
        if (!std::getenv("BF6_KEEP_FRAMES")) state_.reset_frames();
        state_.set_owner(g->owner);
        expression::ChainHost chain;
        chain.add(&g->builtins);
        chain.add(&g->pure);
        chain.add(&recovered_);
        chain.add(&wheel_);
        chain.add(&world_);
        chain.add(&physics_);
        chain.add(&state_);
        const auto r = expression::evaluate(g->graph, &g->inst, {}, &chain);
        /* THE HOST PLAYS THE NATIVE SIDE. A derived graph republishes native part state
         * (an aircraft's control-surface part becomes Flap_Elevator, its gear wheels the
         * wheel speeds); where this host supplies such a channel itself, the channel is
         * what the part holds, so the derived graph's copy - read from part cells the
         * host does not keep - must not replace it. */
        if (g->name.find("/derivedex_") != std::string::npos) {
            if (!primary_written_ready_) {
                /* channels a non-derived graph writes: graph outputs, never restored */
                static const uint32_t kSetOps[] = {0x6D86C436u, 0xC491CD96u, 0xCC162A96u, 0x95954635u};
                for (const auto& og : graphs_) {
                    if (og->name.find("/derivedex_") != std::string::npos) continue;
                    std::map<uint32_t, uint32_t> hash_at;
                    for (const auto& b : og->binds)
                        if (b.region == 0 && b.kind != 2) hash_at[b.pool_offset] = b.channel_hash;
                    for (const auto& rec : og->graph.records) {
                        bool is_set = false;
                        for (uint32_t k : kSetOps) is_set = is_set || (rec.has_operator && rec.operator_key == k);
                        if (!is_set || rec.operands.empty() || rec.operands[0].region != 0) continue;
                        const auto it = hash_at.find(rec.operands[0].offset);
                        if (it != hash_at.end()) primary_written_.insert(it->second);
                    }
                }
                primary_written_ready_ = true;
            }
            for (const auto& hv : host_values_)
                if (!primary_written_.count(hv.first.first))
                    state_.set_channel(hv.first.first, hv.second, hv.first.second);
        }
        /* BF6_SLOT_WATCH=hex,hex: every write this run to those slots of the
         * drivetrain graph, with the value and the writing record. */
        if (const char* at = std::getenv("BF6_RAN_AT"))
            if (g->name.find(std::getenv("BF6_RAN_GRAPH") ? std::getenv("BF6_RAN_GRAPH") : "simex") != std::string::npos) {
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
        /* BF6_TRACE_AT=<tick>[,<value>]: every slot write of that tick, in order, with
         * the record and operator that made it. Given a value, only the writes within
         * a thousandth of it - which is how you find the operator that produced a
         * number you can see in the output but cannot place. An operator that writes
         * nothing does not appear, so a force that is missing and a force that is
         * cancelled look different here, which is the whole point. */
        if (const char* at = std::getenv("BF6_TRACE_AT")) {
            /* Counted PER GRAPH: a vehicle runs several, so a single counter would
             * land the dump on whichever graph happened to be that many calls in. */
            static std::map<std::string, int> ticks;
            const int tick = ++ticks[g->name];
            char* end = nullptr;
            const long want = std::strtol(at, &end, 10);
            const bool has_v = end && *end == ',';
            const float v = has_v ? (float)std::atof(end + 1) : 0.0f;
            if (tick == want)
                for (const auto& t : g->inst.trace) {
                    float l[4];
                    std::memcpy(l, t.lanes, 16);
                    const int n = t.width >= 16 ? 4 : (t.width >= 8 ? 2 : 1);
                    bool hit = !has_v;
                    for (int i = 0; i < n && !hit; ++i)
                        hit = std::fabs(l[i] - v) <= std::fabs(v) * 1e-3f + 1e-6f;
                    if (!hit) continue;
                    std::fprintf(stderr, "trace %s rec 0x%X key %08X -> slot 0x%X w %u %s",
                                 g->name.c_str(), t.record_offset, t.key, t.slot, t.width,
                                 t.known ? "known" : "UNKNOWN");
                    for (int i = 0; i < n; ++i) std::fprintf(stderr, " %g", l[i]);
                    std::fprintf(stderr, "\n");
                }
        }
        /* ANSWER THE WHEEL STATUS CELL, which is what the header of
         * unseeded_frame_reads() describes: "the cell the engine would have written",
         * kept in read order so a caller can answer it next tick, with set_cell_raw
         * provided for exactly that. Nothing had ever answered one.
         *
         * WHICH CELL AND WHY 1 IS NOT A GUESS. The airplane graphs read kind 0 field 0
         * of a bound part and test it against zero, treating equality as the wheel
         * brake being applied; that pinned four aircraft to about 1.6 m/s. Reading the
         * f22's own StateListDescriptor names that field: its per-wheel entries are
         * Wheel Status at offset 1, AverageSlipRatioSum at 6, AverageSlipAngleSum at
         * 10, Wheel Angular Velocity at 14 and SpringCompression at 18, so ranked by
         * offset field 0 is **Wheel Status** - and the ranking is corroborated because
         * field 3 lands on Wheel Angular Velocity, which is what this host already
         * serves for field 3.
         *
         * The VALUE comes from the game too, not from taste: the tyre operator
         * 45A17BD8 takes a status operand and returns one, and on every gear of every
         * tick both are exactly 1. So the cell should hold 1, and the graph already
         * carries that 1 in its own slots - it is only the state cell that nothing
         * seeded.
         *
         * Narrow on purpose: kind 0, field 0, and a path whose low byte names a
         * particular part. A car's equivalent reads carry the all-ones sentinel and are
         * untouched. */
        for (const auto& fr : state_.unseeded_frame_reads())
            if (fr.kind == 0 && fr.field == 0 && (fr.path & 0xFFu) != 0xFFu)
                state_.set_cell_raw(fr.key, 1);
        /* BF6_SEED_CELL=<hex key>[:<value>][,...]: seed named state cells every tick.
         * The graph's own state list declares cells nothing offline ever writes - a
         * helicopter's Autopilot struct carries a Pitch Angle PID, a Roll PID, HasPilot
         * and AutohoverEnabled, and an unwritten HasPilot reads false so the attitude
         * hold never runs. Which cell is which cannot be read off the descriptors,
         * because the frame in a cell key is a small runtime id and not the struct's
         * HashName, so the way to find out is to seed one at a time and measure. Value
         * defaults to 1. */
        if (const char* sc = std::getenv("BF6_SEED_CELL"))
            for (const char* p = sc; *p;) {
                char* e = nullptr;
                const unsigned long long k = std::strtoull(p, &e, 16);
                if (e == p) break;
                uint32_t v = 1;
                if (*e == ':') { char* e2 = nullptr; v = (uint32_t)std::strtoul(e + 1, &e2, 0); e = e2; }
                state_.set_cell_raw((uint64_t)k, v);
                p = *e ? e + 1 : e;
            }
        /* BF6_LIST_CELLS=1: every unseeded cell this tick, as a key the sweep can feed
         * back to BF6_SEED_CELL. */
        if (std::getenv("BF6_LIST_CELLS"))
            for (const auto& fr : state_.unseeded_frame_reads())
                std::fprintf(stderr, "cell %016llX frame %06X path %08X kind %u field %u\n",
                             (unsigned long long)fr.key, fr.frame, fr.path, fr.kind, fr.field);
        /* BF6_WHY=<hex slot>[,tick[,depth]]: explain a value by walking the dataflow
         * BACKWARD from the slot that holds it - what wrote it, what that read, and so on
         * - printing the record, operator, value and knownness at each level.
         *
         * This exists because every diagnosis in this file's history was that same walk
         * done by hand, ten or twenty commands deep, one grep per level. Worse, the
         * manual walks were stitched together by matching VALUES between levels, and
         * three of them reached confident wrong conclusions that way: a frozen state cell
         * blamed for a roll it had no part in, an integrator accused of accumulating its
         * own delta when it was tracking a channel, and a wing axis deduced from the one
         * surface that happened to be a rudder. Identity is in the trace; matching values
         * was never necessary. One command now does the whole walk. */
        if (const char* want = std::getenv("BF6_WHY")) {
            static std::map<std::string, int> ticks;
            const int tick = ++ticks[g->name];
            char* end = nullptr;
            const uint32_t slot = (uint32_t)std::strtoul(want, &end, 16);
            long at_tick = 120, max_depth = 12;
            if (end && *end == ',') { at_tick = std::strtol(end + 1, &end, 10);
                                      if (end && *end == ',') max_depth = std::strtol(end + 1, nullptr, 10); }
            if (tick == at_tick) {
                std::fprintf(stderr, "why slot 0x%X in %s at tick %ld:\n",
                             slot, g->name.c_str(), at_tick);
                /* CAUSALLY ORDERED, not last-writer-wins. The trace is in execution
                 * order, so the write that a consumer at index i actually read is the
                 * last write to that slot BEFORE i. A global last-writer map instead
                 * reports each slot's end-of-tick value, which produced chains that
                 * cannot be true - a move whose source and destination disagree - and
                 * sent one investigation down a vector that had nothing to do with the
                 * value being explained. A tool that invents plausible chains is worse
                 * than no tool, so the walk carries the index it is allowed to look
                 * before. */
                const auto& tr = g->inst.trace;
                auto writer_before = [&tr](uint32_t s, size_t before) -> size_t {
                    for (size_t i = before; i-- > 0;)
                        if (tr[i].slot == s) return i;
                    return (size_t)-1;
                };
                std::set<uint32_t> seen;
                struct Walk {
                    const std::vector<expression::Instance::TraceRow>& tr;
                    const std::function<size_t(uint32_t, size_t)>& writer_before;
                    std::set<uint32_t>& seen;
                    long max_depth;
                    void go(uint32_t s, size_t before, long depth) const {
                        std::string pad((size_t)depth * 2, ' ');
                        const size_t idx = writer_before(s, before);
                        if (idx == (size_t)-1) {
                            std::fprintf(stderr, "%s0x%X <- NOTHING WROTE IT this tick"
                                                 " (a seeded constant, or an unseeded cell"
                                                 " read as zero)\n", pad.c_str(), s);
                            return;
                        }
                        const auto& t = tr[idx];
                        float l[4];
                        std::memcpy(l, t.lanes, 16);
                        const int n = t.width >= 16 ? 4 : 1;
                        std::fprintf(stderr, "%s0x%X = ", pad.c_str(), s);
                        for (int i = 0; i < n; ++i) std::fprintf(stderr, "%g ", l[i]);
                        std::fprintf(stderr, "%s <- rec 0x%X %s", t.known ? "" : "UNKNOWN",
                                     t.record_offset, t.key ? "op" : "move/select");
                        if (t.key) std::fprintf(stderr, " %08X", t.key);
                        if (t.inputs_truncated) std::fprintf(stderr, " (inputs truncated)");
                        std::fprintf(stderr, "\n");
                        if (depth + 1 > max_depth || !seen.insert(s).second) return;
                        for (uint8_t i = 0; i < t.n_inputs; ++i) {
                            /* ONLY SLOTS ARE IN THE TRACE. Region 0 is the constant pool
                             * and shares the number space with the slots, so following a
                             * pool offset into the slot trace reports an unrelated value -
                             * which it did, and the giveaway was a move whose source and
                             * destination disagreed. Name the region and stop there. */
                            if (t.in_region[i] != 2) {
                                std::string p2((size_t)(depth + 1) * 2, ' ');
                                std::fprintf(stderr, "%sr%u+0x%X (a %s, not a slot)\n",
                                             p2.c_str(), t.in_region[i], t.inputs[i],
                                             t.in_region[i] == 0 ? "POOL CONSTANT" : "bound operand");
                                continue;
                            }
                            go(t.inputs[i], idx, depth + 1);   /* only writes BEFORE this one */
                        }
                    }
                };
                Walk{tr, writer_before, seen, max_depth}.go(slot, tr.size(), 1);
            }
        }
        /* BF6_UNSEEDED_REPORT=1: the per-part state cells this graph READ and nothing
         * ever wrote, which the host is meant to answer - the header calls each one
         * "the cell the engine would have written", and set_cell_raw exists for
         * exactly that. The inventory was already being collected and nothing read it,
         * so these gaps could only be found by hand-tracing one slot at a time. They
         * are not harmless: an unseeded read returns ZERO, and an airplane graph tests
         * one of these against zero and takes it as the wheel brake being applied,
         * which pins four aircraft to a standstill. */
        if (std::getenv("BF6_UNSEEDED_REPORT")) {
            const auto& u = state_.unseeded_frame_reads();
            std::map<uint64_t, expression::StateHost::FrameRead> once;
            for (const auto& fr : u) once.emplace(fr.key, fr);
            if (!once.empty()) {
                std::fprintf(stderr, "unseeded state cells in %s: %zu distinct\n",
                             g->name.c_str(), once.size());
                for (const auto& kv : once)
                    std::fprintf(stderr, "  key 0x%016llX frame %06X path %08X kind %u field %u\n",
                                 (unsigned long long)kv.first, kv.second.frame,
                                 kv.second.path, kv.second.kind, kv.second.field);
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
        std::snprintf(line, sizeof line,
                      "%s: termination %d steps %u ended at rec 0x%X guessed %u unresolved %zu\n",
                      g->name.c_str(), (int)r.termination, r.steps, r.last_record,
                      r.guessed_branches, r.unresolved_keys.size());
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
        /* REFUSED IS NOT UNRESOLVED. A refused call means the operator is known and
         * described but its INPUTS were not, so it is a knownness frontier rather than a
         * missing capability. Reported as one list the two were indistinguishable, and
         * MultiplyFloatFloatFloat, SubtractFloat and logical And sitting in "unresolved"
         * read as missing operators - which sent more than one investigation after a
         * capability that was already implemented. */
        if (!r.refused_keys.empty()) {
            std::map<uint32_t, int> rk;
            for (uint32_t k : r.refused_keys) ++rk[k];
            std::string rl = "  refused (known op, unknown input):";
            for (const auto& kv : rk) {
                char kb[32];
                std::snprintf(kb, sizeof kb, " %08X x%d", kv.first, kv.second);
                rl += kb;
            }
            report_ += rl + "\n";
        }
        /* BF6_REFUSED_DETAIL=1: every refused call with its record and which inputs were
         * unknown. The VM always wrote this diagnostic and nothing ever printed it, so a
         * refusal could only be located by walking the graph by hand. */
        if (std::getenv("BF6_REFUSED_DETAIL"))
            for (const auto& d : r.diagnostics)
                if (d.rfind("host invoke failed", 0) == 0 || d.rfind("unresolved at record", 0) == 0) report_ += "  " + d + "\n";
        for (const auto& d : r.diagnostics)
            if (d.find(" @") != std::string::npos) {
                const unsigned long off = std::stoul(d.substr(d.rfind('@') + 1));
                std::snprintf(line, sizeof line, "  guessed at rec 0x%lX: %s\n", off, d.substr(0, d.find(" @")).c_str());
                report_ += line;
            }
    }
    publish_bones();
}

/* Write a bone's local pose from outside the graphs (an animation clip layer): the
 * skeleton, the subtree and the tick's bone writes, as a graph's SetPartTransform. */
bool VehicleSim::write_bone_local(uint32_t channel_hash, const float local[16]) {
    const auto identity = bone_identity_.find(channel_hash);
    if (identity == bone_identity_.end()) return false;
    state_.commit_local(identity->second.index, local);
    state_.record_bone_write(channel_hash, local);
    return true;
}

void VehicleSim::write_rig_local(int32_t index, const float local[16]) {
    state_.commit_local(index, local);
    uint32_t key = 0;
    for (const auto& kv : bone_identity_) if (kv.second.index == index) { key = kv.first; break; }
    if (!key) {
        /* a bone no graph binds (a FakeHinge door, an antenna): a synthetic key in the
         * top range, registered so it is presented and tracked like any other */
        key = 0xFE000000u | (uint32_t)index;
        if (!bone_identity_.count(key) && (size_t)index < rig_names_.size())
            bone_identity_[key] = BoneIdentity{index, rig_names_[(size_t)index]};
        if (!bone_binds_.count(key) && (size_t)index < rig_names_.size())
            bone_binds_[key] = rig_names_[(size_t)index];
    }
    state_.record_bone_write(key, local);
}

bool VehicleSim::bone_rest_local(uint32_t channel_hash, float out[16]) const {
    const auto identity = bone_identity_.find(channel_hash);
    if (identity == bone_identity_.end()) return false;
    return state_.rest_local(identity->second.index, out);
}

bool VehicleSim::maps_bone(uint32_t channel_hash) const {
    return bone_identity_.count(channel_hash) != 0;
}

/* The tick's bone writes as presented bones, and the motion scoreboard's tracking. */
void VehicleSim::publish_bones() {
    presented_bones_.clear();
    for (const auto& written : state_.bone_writes()) {
        const auto identity = bone_identity_.find(written.first);
        if (identity == bone_identity_.end() || written.second.size() < 64) continue;
        PresentedBone out;
        out.channel_hash = written.first;
        out.bone_index = identity->second.index;
        out.name = identity->second.name;
        std::memcpy(out.local.data(), written.second.data(), 64);
        presented_bones_.push_back(std::move(out));
    }
    /* the motion scoreboard: how far each written bone has moved since its first write */
    for (const auto& written : state_.bone_writes()) {
        if (written.second.size() < 64) continue;
        float m[16];
        std::memcpy(m, written.second.data(), 64);
        auto it = motion_.find(written.first);
        if (it == motion_.end()) {
            MotionTrack t;
            std::memcpy(t.first.data(), m, 64);
            t.writes = 1;
            motion_[written.first] = t;
            continue;
        }
        MotionTrack& t = it->second;
        ++t.writes;
        /* rows 0..2 are the basis (stride 4), row 3 the translation */
        auto row = [](const float* r, int i, float out[3]) {
            float n = std::sqrt(r[i * 4] * r[i * 4] + r[i * 4 + 1] * r[i * 4 + 1] + r[i * 4 + 2] * r[i * 4 + 2]);
            if (n < 1e-6f) n = 1.0f;
            for (int k = 0; k < 3; ++k) out[k] = r[i * 4 + k] / n;
        };
        float tr = 0.0f;
        for (int i = 0; i < 3; ++i) {
            float a[3], b[3];
            row(t.first.data(), i, a);
            row(m, i, b);
            tr += a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }
        const float c = std::max(-1.0f, std::min(1.0f, (tr - 1.0f) * 0.5f));
        const float deg = std::acos(c) * 57.2957795f;
        const float dx = m[12] - t.first[12], dy = m[13] - t.first[13], dz = m[14] - t.first[14];
        t.rot_deg = std::max(t.rot_deg, deg);
        t.move_m = std::max(t.move_m, std::sqrt(dx * dx + dy * dy + dz * dz));
        /* A SPRING COMPRESSES BY SCALE: the largest relative change of a basis row's
         * length (a coil spring or a damper stretched along its axis) */
        for (int i = 0; i < 3; ++i) {
            const float* a = t.first.data() + i * 4;
            const float* b = m + i * 4;
            const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
            const float lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
            if (la > 1e-6f) t.scale = std::max(t.scale, std::fabs(lb - la) / la);
        }
    }
}

std::string VehicleSim::motion_json() const {
    auto esc = [](const std::string& v) {
        std::string o;
        for (char ch : v) {
            if (ch == '"' || ch == '\\') o += '\\';
            if ((unsigned char)ch >= 0x20) o += ch;
        }
        return "\"" + o + "\"";
    };
    const std::map<uint32_t, std::string> names = channel_name_map();
    auto name_of = [&](uint32_t h) {
        const auto it = names.find(h);
        if (it != names.end() && !it->second.empty()) return it->second;
        char b[16];
        std::snprintf(b, sizeof b, "0x%08X", h);
        return std::string(b);
    };
    std::set<std::string> written, unsupplied;
    for (const auto& kv : state_.channel_writes()) written.insert(name_of((uint32_t)kv.first));
    /* GRAPH-OWNED: a channel some graph has a write record for, whether or not it ran.
     * The F-16's simex sets Airborne State itself (true past an altitude test, false on
     * touchdown); a read before its first write is the grounded default, not a missing
     * engine input. Only a channel NOTHING can write is unsupplied. */
    {
        static const uint32_t kSetOps[] = {0x6D86C436u, 0xC491CD96u, 0xCC162A96u, 0x95954635u};
        for (const auto& g : graphs_) {
            std::map<uint32_t, uint32_t> hash_at;
            for (const auto& b : g->binds)
                if (b.region == 0 && b.kind != 2) hash_at[b.pool_offset] = b.channel_hash;
            for (const auto& rec : g->graph.records) {
                bool is_set = false;
                for (uint32_t k : kSetOps) is_set = is_set || (rec.has_operator && rec.operator_key == k);
                if (!is_set || rec.operands.empty() || rec.operands[0].region != 0) continue;
                const auto it = hash_at.find(rec.operands[0].offset);
                if (it != hash_at.end()) written.insert(name_of(it->second));
            }
        }
    }
    for (const auto& kv : state_.unsupplied_channels()) {
        const std::string n = name_of((uint32_t)kv.first);
        if (!host_set_.count(n) && !written.count(n)) unsupplied.insert(n);
    }
    std::string o = "{\"graphs\":[";
    for (size_t i = 0; i < graphs_.size(); ++i) o += (i ? "," : "") + esc(graphs_[i]->name);
    o += "],\"host_set\":[";
    bool first = true;
    for (const auto& n : host_set_) { o += (first ? "" : ",") + esc(n); first = false; }
    o += "],\"graph_written\":[";
    first = true;
    for (const auto& n : written) { o += (first ? "" : ",") + esc(n); first = false; }
    o += "],\"unsupplied\":[";
    first = true;
    for (const auto& n : unsupplied) { o += (first ? "" : ",") + esc(n); first = false; }
    o += "],\"parts_unsupplied\":" + std::to_string(state_.unsupplied_parts().size());
    o += ",\"rig\":[";
    for (size_t i = 0; i < rig_names_.size(); ++i) o += (i ? "," : "") + esc(rig_names_[i]);
    o += "],\"rig_parent\":[";
    for (size_t i = 0; i < rig_parents_.size(); ++i) o += (i ? "," : "") + std::to_string(rig_parents_[i]);
    o += "],\"bones\":[";
    first = true;
    for (const auto& kv : bone_binds_) {
        const auto id = bone_identity_.find(kv.first);
        const auto mt = motion_.find(kv.first);
        char b[200];
        std::snprintf(b, sizeof b, ",\"rig_index\":%d,\"writes\":%u,\"rot_deg\":%.3f,\"move_m\":%.4f,\"scale\":%.4f}",
                      id == bone_identity_.end() ? -1 : id->second.index,
                      mt == motion_.end() ? 0u : mt->second.writes,
                      mt == motion_.end() ? 0.0f : mt->second.rot_deg,
                      mt == motion_.end() ? 0.0f : mt->second.move_m,
                      mt == motion_.end() ? 0.0f : mt->second.scale);
        o += (first ? "{" : ",{");
        o += "\"channel\":" + esc(kv.second) + ",\"bone\":" +
             esc(id == bone_identity_.end() ? std::string() : id->second.name) + b;
        first = false;
    }
    o += "]}";
    return o;
}

} // namespace bf6
