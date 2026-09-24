/* soldier_sim_ext.inc - THE SOLDIER'S OWN MOTION-MACHINE GRAPHS, RUN FRAME BY FRAME.
 *
 * The soldier's traversal (vault, ladder, swim, slide, landing...) is decided by expression
 * graphs under common/gameplay/soldier/**, the same VM and the same kind of graph as a
 * vehicle's simex_*. So this is a VehicleSim over those graphs, prepared the same way -
 * bindings patched, typed values (curves) seeded from each graph's own EBX by
 * vehicle_typed_seed.inc - with the soldier's named fields (soldier_fields.h) as the state
 * the graphs read and store. The walker supplies fields by name before a tick and reads
 * back what the graphs stored after it.
 *
 * Included into bf6_core.cpp after vehicle_api_ext.inc (it reuses bf6__seed_typed_values)
 * and soldier_fields_ext.inc. The field table is a process-wide singleton, so this models
 * ONE soldier. */

struct bf6_soldier {
    bf6::VehicleSim sim;
    bf6_ray_scene* scene = nullptr;
    /* false when the scene is the caller's (bf6_soldier_open_scene): a walker's own
     * collision, which it frees itself - after closing this. */
    bool owns_scene = true;
    std::vector<std::string> graphs;
    double time = 0.0;
};

static int bf6__soldier_trace(void* user, const double* from, const double* to, double* hit, double* normal) {
    const bf6_soldier* s = (const bf6_soldier*)user;
    double out[7] = {};
    if (!s || !s->scene || bf6_ray_scene_trace(s->scene, from, to, out) < 0) return 0;
    for (int i = 0; i < 3; ++i) { hit[i] = out[i]; normal[i] = out[3 + i]; }
    return 1;
}

static bf6_soldier* bf6__soldier_open(bf6_ctx* c, const char* graphs_csv, const float* tris,
                                      int32_t vert_count, bf6_ray_scene* borrowed,
                                      char* err, int32_t err_len);

bf6_soldier* bf6_soldier_open(bf6_ctx* c, const char* graphs_csv, const float* tris, int32_t vert_count,
                              char* err, int32_t err_len)
{
    return bf6__soldier_open(c, graphs_csv, tris, vert_count, nullptr, err, err_len);
}

/* The same, tracing its rays against a scene the caller owns - the walker's collision, so
 * the motion machine's ledge and ground probes see exactly what the player stands on. The
 * scene must outlive the soldier. */
bf6_soldier* bf6_soldier_open_scene(bf6_ctx* c, const char* graphs_csv, bf6_ray_scene* scene,
                                    char* err, int32_t err_len)
{
    return bf6__soldier_open(c, graphs_csv, nullptr, 0, scene, err, err_len);
}

static bf6_soldier* bf6__soldier_open(bf6_ctx* c, const char* graphs_csv, const float* tris,
                                      int32_t vert_count, bf6_ray_scene* borrowed,
                                      char* err, int32_t err_len)
{
    auto fail = [&](const std::string& m) -> bf6_soldier* {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", m.c_str());
        return nullptr;
    };
    if (!c || !graphs_csv || !*graphs_csv) return fail("no context or graphs");
    if (bf6_soldier_fields_load(c) <= 0) return fail("the soldier field table did not load");
    std::string exe;
    for (const std::string& cand : bf6::TypeDb::exe_candidates(c->src.game_dir())) { exe = cand; break; }
    if (exe.empty()) return fail("no executable for operator names");
    std::unique_ptr<bf6_soldier> s(new bf6_soldier());
    std::string list = graphs_csv;
    for (size_t p = 0; p <= list.size();) {
        size_t q = list.find(',', p);
        if (q == std::string::npos) q = list.size();
        std::string g = list.substr(p, q - p);
        while (!g.empty() && g.front() == ' ') g.erase(g.begin());
        while (!g.empty() && g.back() == ' ') g.pop_back();
        if (!g.empty()) s->graphs.push_back(g);
        p = q + 1;
    }
    if (s->graphs.empty()) return fail("no graphs named");
    /* The soldier graphs live in shared gameplay packages, and a few only in level packages
     * (stance transitions, swimming physics); widen once, levels included, which only ever
     * adds names. */
    {
        const uint8_t* probe = nullptr;
        if (bf6_read_raw(c, BF6_RAW_RES, s->graphs[0].c_str(), &probe) <= 0) {
            char merr[256] = {};
            bf6_mount_all(c, 1, merr, (int)sizeof(merr));
        }
    }
    s->owns_scene = borrowed == nullptr;
    s->scene = borrowed ? borrowed : bf6_ray_scene_create();
    if (!borrowed && tris && vert_count >= 3) {
        std::vector<int32_t> idx((size_t)(vert_count - vert_count % 3));
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = (int32_t)i;
        const int32_t mesh = bf6_ray_scene_add_mesh(s->scene, tris, vert_count, idx.data(), (int32_t)idx.size());
        const double identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        if (mesh >= 0) bf6_ray_scene_add_instance(s->scene, mesh, identity);
    }
    std::string e;
    if (!s->sim.open(c, exe, s->graphs, "", bf6__soldier_trace, s.get(), e)) {
        if (s->owns_scene) bf6_ray_scene_free(s->scene);
        return fail(e);
    }
    for (const std::string& g : s->graphs) bf6__seed_typed_values(c, g, s->sim);
    return s.release();
}

/* Run every graph once, in the order given. dt only advances the clock the graphs read
 * (0xE2EEC2BE); the per-frame delta operator is still the host's fixed 1/60. */
void bf6_soldier_tick(bf6_soldier* s, float dt) {
    if (!s) return;
    s->time += dt > 0.0f ? dt : 1.0f / 60.0f;
    s->sim.set_time(s->time);
    s->sim.tick();
}

/* A field's current value by name: 1 known (out gets up to four lanes), 0 when the last
 * store to it had an unknown value, -1 when there is no such field. */
int32_t bf6_soldier_field_get(const char* name, float* out4) {
    if (!name || !out4) return -1;
    const bf6::SoldierFields& f = bf6::SoldierFields::get();
    const bf6::SoldierFields::Field* fl = f.by_name(name);
    if (!fl) return -1;
    if (!f.known(*fl)) return 0;
    f.value(*fl, out4);
    return 1;
}

/* Forget every live value and unknown store: the soldier reads its authored defaults. */
void bf6_soldier_fields_reset(void) { bf6::SoldierFields::get().clear_live(); }

/* The names of every field written so far, newline-separated; returns the full length. */
int32_t bf6_soldier_written(char* buf, int32_t cap) {
    std::string all;
    for (const std::string& n : bf6::SoldierFields::get().written()) { all += n; all += '\n'; }
    if (buf && cap > 0) std::snprintf(buf, (size_t)cap, "%s", all.c_str());
    return (int32_t)all.size();
}

/* Every field the graphs READ so far, one per line as "name<TAB>stored": stored 1 when a
 * graph also stores it, 0 when nothing but the caller supplies it - the graphs' inputs. */
int32_t bf6_soldier_reads(char* buf, int32_t cap) {
    const bf6::SoldierFields& f = bf6::SoldierFields::get();
    std::string all;
    for (const std::string& n : f.reads()) { all += n; all += f.was_stored(n) ? "\t1\n" : "\t0\n"; }
    if (buf && cap > 0) std::snprintf(buf, (size_t)cap, "%s", all.c_str());
    return (int32_t)all.size();
}

/* The last tick's per-graph report (termination, unresolved operators). */
int32_t bf6_soldier_report(bf6_soldier* s, char* buf, int32_t cap) {
    if (!s) return 0;
    std::string r = s->sim.report();
    /* BF6_SOLDIER_LAYOUT=1: each graph's typed slot groups too - which slot holds which
     * reflected type, the way to confirm an operator's result type from data. */
    if (std::getenv("BF6_SOLDIER_LAYOUT")) r += "\n" + s->sim.instance_layout();
    /* BF6_SOLDIER_CHANNELS=1: every public channel the loaded graphs bind, by name. */
    if (std::getenv("BF6_SOLDIER_CHANNELS"))
        for (const auto& kv : s->sim.channel_name_map()) r += "\nchannel " + kv.second;
    /* BF6_SOLDIER_LIST=<graph substring>:<lo hex>:<hi hex> - those records, for reading
     * what a graph does with a value. */
    if (const char* want = std::getenv("BF6_SOLDIER_LIST")) {
        const std::string w = want;
        const size_t a = w.find(':'), b = w.find(':', a == std::string::npos ? 0 : a + 1);
        if (a != std::string::npos && b != std::string::npos)
            r += "\n" + s->sim.list(w.substr(0, a), (uint32_t)std::strtoul(w.substr(a + 1, b - a - 1).c_str(), nullptr, 16),
                                    (uint32_t)std::strtoul(w.substr(b + 1).c_str(), nullptr, 16));
    }
    /* BF6_SLOT_TYPE_OF=<hex key>: every call of that operator, its output slot and the
     * data type the graph DECLARES for that slot (slot_values) - an operator's result
     * type read from data rather than from its consumers. */
    if (const char* want = std::getenv("BF6_SLOT_TYPE_OF")) {
        const uint32_t k = (uint32_t)std::strtoul(want, nullptr, 16);
        char line[256];
        for (const std::string& gname : s->graphs) {
            const bf6::expression::Graph* g = s->sim.graph_named(gname);
            if (!g) continue;
            std::map<uint32_t, uint32_t> type_at;
            for (const auto& grp : g->slot_values)
                for (uint32_t off : grp.offsets) type_at[off] = grp.data_type_id;
            for (const auto& rec : g->records) {
                if (rec.operator_key != k) continue;
                uint32_t outslot = 0xFFFFFFFFu;
                for (auto it = rec.operands.rbegin(); it != rec.operands.rend(); ++it)
                    if (it->region == 2) { outslot = it->offset; break; }
                const auto t = type_at.find(outslot);
                std::snprintf(line, sizeof line, "slottype %08X rec 0x%X in %s: out slot 0x%X type %s%08X\n",
                              k, rec.offset, gname.c_str(), outslot,
                              t == type_at.end() ? "none " : "0x", t == type_at.end() ? 0u : t->second);
                r += line;
                /* and every field reference taken on that result (0x88030F01 / 0x532B3BA9
                 * with it as operand 0), with each operand's constant-pool word on disk */
                for (const auto& use : g->records) {
                    if (use.operator_key != 0x88030F01u && use.operator_key != 0x532B3BA9u) continue;
                    if (use.operands.empty() || use.operands[0].region != 2 || use.operands[0].offset != outslot)
                        continue;
                    std::string w;
                    for (const auto& op : use.operands) {
                        uint32_t v = 0xFFFFFFFFu;
                        if (op.region == 0 && op.offset + 4 <= g->constant_pool.size())
                            std::memcpy(&v, g->constant_pool.data() + op.offset, 4);
                        uint32_t pv = 0;
                        const bool patched = op.region == 0 && s->sim.pool_patch(gname, op.offset, pv);
                        char t2[40];
                        if (patched) std::snprintf(t2, sizeof t2, " r0:%08X->%X", v, pv);
                        else std::snprintf(t2, sizeof t2, " r%u:%08X", op.region, v);
                        w += t2;
                    }
                    std::snprintf(line, sizeof line, "    fieldref %08X rec 0x%X%s\n", use.operator_key, use.offset, w.c_str());
                    r += line;
                }
            }
        }
    }
    if (buf && cap > 0) std::snprintf(buf, (size_t)cap, "%s", r.c_str());
    return (int32_t)r.size();
}

void bf6_soldier_close(bf6_soldier* s) {
    if (!s) return;
    if (s->owns_scene) bf6_ray_scene_free(s->scene);
    delete s;
}
