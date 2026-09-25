/* ant_ex_ext.inc - THE SCENE OPS AROUND THE GRAPH: the game's EX expression programs
 * (1p.preupdate.expsop before the controller update, 1p.postupdate.expsop after it,
 * as animations/common/sceneopmatrix1p orders them) run over this runtime's states
 * and pose through bf6ex (ex_vm.h).
 *
 * THE POSE ARENA is the rig's own DOF table. A program names a DOF by a compiled
 * symbolic key from its seed rows; the rig carries the same keys, one per DofSet
 * record, in span order (native/EX_DOF_HASH.md - 45 of 45 post-update seeds resolve on
 * soldier.rig). bf6__anim_rig already reads those arrays for clip binding, so the
 * key -> "Bone.q" association is the one the animation binding uses. Each DOF gets
 * 16 bytes: a quaternion x,y,z,w, a translation x,y,z,0, a scale x,y,z,0, or a scalar.
 *
 * STATES go through the same bf6ant::States the graph reads, so a state the program
 * writes is the state the graph sees. Vector3 and quaternion states have no home
 * there; they live in this host, starting from the state asset's authored default. */

struct AntExOps {
    struct StateHost : bf6ex::Host {
        bf6ant::Runtime* rt = nullptr;
        std::vector<const bf6ant::Obj*> obj;
        std::vector<std::string> path;
        std::vector<uint32_t> writes;
        std::set<std::string> owned;    /* states a program has written: callers may not override them */
        uint64_t bind_state(const std::string& p, uint32_t) override {
            for (size_t i = 0; i < path.size(); ++i) if (path[i] == p) return i;
            const bf6ant::Obj* o = p.empty() ? nullptr : rt->g.root(p.c_str());
            std::array<float, 4> v{ 0, 0, 0, 0 };
            if (o)
                if (const bf6::EbxValue* d = o->f(0x97ea1523u)) {
                    /* the authored default: a struct x,y,z(,w) for vector and quaternion states */
                    const uint32_t lanes[4] = { 0x3901db14u, 0x42fc0f5eu, 0x32a99b9cu, 0x7c8062f2u };
                    for (int k = 0; k < 4; ++k)
                        if (const bf6::EbxValue* f = d->field(lanes[k])) v[(size_t)k] = (float)f->f;
                }
            if (!rt->vec_states.count(p)) rt->vec_states[p] = v;
            obj.push_back(o); path.push_back(p); writes.push_back(0);
            return path.size() - 1;
        }
        void read_state(uint64_t h, void* out, Kind k) override {
            if (h >= obj.size()) return;
            bool ok = false;
            switch (k) {
            case Bool: { const uint8_t b = obj[h] && rt->st.as_bool(rt->g, obj[h], ok) ? 1 : 0; std::memcpy(out, &b, 1); break; }
            case Float: { const float f = obj[h] ? rt->st.as_float(rt->g, obj[h], ok) : 0.f; std::memcpy(out, &f, 4); break; }
            case Int: { const int32_t i = obj[h] ? rt->st.as_int(rt->g, obj[h], ok) : 0; std::memcpy(out, &i, 4); break; }
            default: std::memcpy(out, rt->vec_states[path[h]].data(), 16); break;
            }
        }
        void write_state(uint64_t h, const void* in, Kind k) override {
            if (h >= obj.size()) return;
            ++writes[h];
            owned.insert(path[h]);
            switch (k) {
            case Bool: rt->st.set_bool(path[h], *(const uint8_t*)in != 0); break;
            case Float: { float f; std::memcpy(&f, in, 4); rt->st.set_float(path[h], f); break; }
            case Int: { int32_t i; std::memcpy(&i, in, 4); rt->st.set_int(path[h], i); break; }
            default: std::memcpy(rt->vec_states[path[h]].data(), in, 16); break;
            }
        }
        /* a vector/quaternion state set by the caller (the weapon's md.* inputs) */
        void set_vec(const std::string& p, const float* v) {
            const uint64_t h = bind_state(p, 0);
            std::memcpy(rt->vec_states[path[h]].data(), v, 16);
        }

        /* TUNING CURVES (TuningCurveExp, EX_KERNELS_SPEC.md 0x140743520): channel 0's
         * keys, each a point 0xA4862C49 (x,y), an in-control 0x27C8EF1E, an out-control
         * 0xF4F24BD2 and a segment mode 0x0211E93A (0 step, 1 linear, 2 bezier). The
         * native's x-bezier inversion (0x14067e7b0) is replaced by bisection here. */
        struct Key { float x, y, ix, iy, ox, oy; int mode; };
        std::map<uint64_t, std::vector<Key>> curves;
        const std::vector<Key>& curve_keys(uint64_t h) {
            auto it = curves.find(h);
            if (it != curves.end()) return it->second;
            std::vector<Key>& ks = curves[h];
            const bf6ant::Obj* o = h < obj.size() ? obj[h] : nullptr;
            if (const bf6::EbxValue* arr = o ? o->f(0x32E3F0CEu) : nullptr)
                for (const auto& k : arr->items) {
                    auto xy = [&](uint32_t f, float& x, float& y) {
                        const bf6::EbxValue* v = k.field(f);
                        x = v && v->field(0x3901db14u) ? (float)v->field(0x3901db14u)->f : 0.f;
                        y = v && v->field(0x42fc0f5eu) ? (float)v->field(0x42fc0f5eu)->f : 0.f;
                    };
                    Key key{};
                    xy(0xA4862C49u, key.x, key.y); xy(0x27C8EF1Eu, key.ix, key.iy); xy(0xF4F24BD2u, key.ox, key.oy);
                    const bf6::EbxValue* m = k.field(0x0211E93Au);
                    key.mode = m ? (int)(m->kind == bf6::EbxValue::Kind::Int ? m->i : (int64_t)m->u) : 0;
                    ks.push_back(key);
                }
            return ks;
        }
        float curve(uint64_t h, float x) override {
            const std::vector<Key>& k = curve_keys(h);
            if (k.empty()) return 0.f;
            if (k.size() == 1) return k[0].y;
            size_t i = 0;
            while (i + 2 < k.size() && x >= k[i + 1].x) ++i;
            const Key &a = k[i], &b = k[i + 1];
            switch (a.mode) {
            case 0: return a.y;
            case 1: { float t = (x - a.x) / (b.x - a.x); t = std::fmin(std::fmax(t, 0.f), 1.f); return (a.y - a.y * t) + b.y * t; }
            case 2: {
                if (x <= a.x) return a.y;
                if (x >= b.x) return b.y;
                auto bez = [](float p0, float p1, float p2, float p3, float u) {
                    const float v = 1.f - u; return v * v * v * p0 + 3 * v * v * u * p1 + 3 * v * u * u * p2 + u * u * u * p3;
                };
                float lo = 0.f, hi = 1.f;
                for (int it = 0; it < 40; ++it) {
                    const float mid = 0.5f * (lo + hi);
                    if (bez(a.x, a.ox, b.ix, b.x, mid) < x) lo = mid; else hi = mid;
                }
                return bez(a.y, a.oy, b.iy, b.y, 0.5f * (lo + hi));
            }
            default: return 0.f;
            }
        }
    };

    struct Stage { bf6ex::Program prog; bf6ex::Instance inst; bool on = false; bool seeded = false; };
    Stage pre, post;
    StateHost host;
    bf6ex::PoseArena arena;
    std::vector<int> dof_bone;       /* per DOF: skeleton bone, or -1 */
    std::vector<char> dof_comp;      /* 'q', 't', 's', or 0 for a scalar / non-bone DOF */
    std::vector<std::string> dof_name;
    std::vector<std::array<float, 4>> dof_default;   /* resting value of each DOF */
    int seeds_total = 0, seeds_bound = 0;
    /* THE WEAPON'S ZOOM LEVELS' SIGHT TYPES (bf6_inspect_zoom_sight_types), level 0 the hip.
     * Before each pre-update the current level's value is written to
     * fb.wep.sighttype.enumgs, as the engine does. WHICH level is current is ours: the
     * engine switches levels as the zoom engages; here fb.ads.bool picks level 1. */
    std::vector<int32_t> zoom_sight;
    /* each zoom level's procedural weapon states (bf6_inspect_zoom_level_states), applied
     * when the current level changes */
    std::vector<std::string> zoom_states;
    int zoom_level_applied = -1;
    /* the clips' own programs, one instance each, loaded on first use */
    struct ClipProgram { std::unique_ptr<Stage> st; bool failed = false; };
    std::map<std::string, ClipProgram> clip_programs;
    uint32_t dofs_written = 0;
    std::vector<std::string> errors;
};

/* The arena from the rig's key table; bone DOFs mapped onto this skeleton. */
static bool bf6__ex_arena(bf6ant::Runtime& rt, AntExOps& ex, std::string& err)
{
    std::vector<uint32_t> keys, offsets;
    std::vector<std::string> dof_paths;
    std::vector<std::vector<AnimBindingDofRecord>> records;
    std::map<uint32_t, AnimBindingDefault> defaults;
    int spans = 0;
    if (!bf6__anim_rig(rt.c, rt.rig.c_str(), keys, offsets, dof_paths, records, defaults, spans)) {
        err = "rig key table unreadable: " + rt.rig; return false;
    }
    bf6ex::PoseArena& a = ex.arena;
    a = bf6ex::PoseArena{};
    ex.dof_bone.clear(); ex.dof_comp.clear(); ex.dof_name.clear(); ex.dof_default.clear();
    size_t k = 0;
    for (size_t s = 0; s < records.size(); ++s)
        for (const AnimBindingDofRecord& r : records[s]) {
            if (k >= keys.size()) break;
            const int32_t idx = (int32_t)a.offset.size();
            a.by_hash[keys[k++]] = idx;
            a.offset.push_back((uint32_t)(idx * 16));
            a.width.push_back(16);
            ex.dof_name.push_back(r.name);
            int bone = -1; char comp = 0;
            const size_t dot = r.name.rfind('.');
            if (dot != std::string::npos && dot + 2 == r.name.size()) {
                auto it = rt.bone_index.find(r.name.substr(0, dot));
                if (it != rt.bone_index.end()) { bone = it->second; comp = r.name[dot + 1]; }
            }
            ex.dof_bone.push_back(bone); ex.dof_comp.push_back(comp);
            /* THE DOF'S RESTING VALUE, for DOFs no bone carries (deltaRotation, the
             * trajectory ...): the rig's own default when it authors one, else identity
             * for a quaternion (dof-set component tag 14), one for a scale, zero. A zero
             * quaternion there made RootJointReader's normalize NaN. */
            std::array<float, 4> dv{ 0, 0, 0, 0 };
            auto dit = defaults.find(keys[k - 1]);
            if (dit != defaults.end()) for (int c = 0; c < 4; ++c) dv[(size_t)c] = dit->second.value[c];
            else if (r.component_type == 14u) dv = { 0, 0, 0, 1 };
            else if (comp == 's') dv = { 1, 1, 1, 0 };
            ex.dof_default.push_back(dv);
        }
    a.bytes.assign(a.offset.size() * 16, 0);
    a.valid.assign(a.offset.size(), 0);
    return true;
}

/* pose -> arena: every bone DOF live (validity 1, so a DofWriter's 0xff is visible) */
static void bf6__ex_fill(AntExOps& ex, const bf6ant::Pose& p)
{
    bf6ex::PoseArena& a = ex.arena;
    for (size_t i = 0; i < ex.dof_bone.size(); ++i) {
        const int b = ex.dof_bone[i];
        float* d = (float*)(a.bytes.data() + a.offset[i]);
        a.valid[i] = 0;
        if (b < 0 || (size_t)b >= p.q.size()) {
            if (i < ex.dof_default.size()) std::memcpy(d, ex.dof_default[i].data(), 16);
            continue;
        }
        switch (ex.dof_comp[i]) {
        case 'q': std::memcpy(d, p.q[(size_t)b].data(), 16); a.valid[i] = 1; break;
        case 't': d[0] = p.t[(size_t)b][0]; d[1] = p.t[(size_t)b][1]; d[2] = p.t[(size_t)b][2]; d[3] = 0; a.valid[i] = 1; break;
        case 's': d[0] = d[1] = d[2] = 1; d[3] = 0; a.valid[i] = 1; break;
        default: break;
        }
    }
}

/* arena -> pose: only the DOFs a DofWriter touched (validity 0xff) */
static void bf6__ex_apply(AntExOps& ex, bf6ant::Pose& p)
{
    const bf6ex::PoseArena& a = ex.arena;
    for (size_t i = 0; i < ex.dof_bone.size(); ++i) {
        if (a.valid[i] != 0xff) continue;
        ++ex.dofs_written;
        /* BF6_EX_DOFS=N: each written DOF and its value, for the first N writes */
        static const long trace_n = std::getenv("BF6_EX_DOFS") ? std::atol(std::getenv("BF6_EX_DOFS")) : 0;
        const int b = ex.dof_bone[i];
        if ((long)ex.dofs_written <= trace_n) {
            /* only the writes that CHANGE the joint, with the value the pose had */
            const float* v = (const float*)(a.bytes.data() + a.offset[i]);
            float was[4] = { 0, 0, 0, 0 };
            if (b >= 0 && (size_t)b < p.q.size()) {
                if (ex.dof_comp[i] == 'q') std::memcpy(was, p.q[(size_t)b].data(), 16);
                else if (ex.dof_comp[i] == 't') std::memcpy(was, p.t[(size_t)b].data(), 12);
            }
            float d = 0;
            for (int k = 0; k < 3; ++k) d = std::fmax(d, std::fabs(v[k] - was[k]));
            if (d > 1e-4f || !std::isfinite(v[0] + v[1] + v[2] + v[3]))
                std::fprintf(stderr, "ex dof %-28s (%.4f %.4f %.4f %.4f) was (%.4f %.4f %.4f %.4f)\n", ex.dof_name[i].c_str(),
                             v[0], v[1], v[2], v[3], was[0], was[1], was[2], was[3]);
        }
        if (b < 0 || (size_t)b >= p.q.size()) continue;
        const float* s = (const float*)(a.bytes.data() + a.offset[i]);
        if (ex.dof_comp[i] == 'q') { p.q[(size_t)b] = { s[0], s[1], s[2], s[3] }; if ((size_t)b < p.vq.size()) p.vq[(size_t)b] = 1; }
        else if (ex.dof_comp[i] == 't') { p.t[(size_t)b] = { s[0], s[1], s[2] }; if ((size_t)b < p.vt.size()) p.vt[(size_t)b] = 1; }
    }
}

/* A clip's own expression over that clip's sampled pose (Runtime::clip_expression).
 * Its seed rows copy the pose's DOFs in at binding (default slots), so the arena is
 * filled first and the seeds re-bound; it writes states only. */
static void bf6__ex_clip(AntExOps& ex, bf6ant::Runtime& rt, const bf6ant::Obj* program, const bf6ant::Pose& pose);
static bool bf6__ex_stage(bf6ant::Runtime& rt, AntExOps& ex, AntExOps::Stage& st, const char* asset, std::string& err)
{
    st.on = false;
    if (!asset || !*asset) return true;
    if (!bf6ex::load_program(rt.c, asset, st.prog, err)) return false;
    if (!st.inst.init(st.prog, &ex.host, &ex.arena, err)) return false;
    for (const auto& s : st.prog.seeds) { ++ex.seeds_total; if (ex.arena.by_hash.count(s.hash)) ++ex.seeds_bound; }
    st.on = true;
    return true;
}

static void bf6__ex_clip(AntExOps& ex, bf6ant::Runtime& rt, const bf6ant::Obj* program, const bf6ant::Pose& pose)
{
    AntExOps::ClipProgram& cp = ex.clip_programs[program->path];
    if (cp.failed) return;
    if (!cp.st) {
        if (std::getenv("BF6_EX_CLIPS")) std::fprintf(stderr, "ex clip program %s\n", program->path.c_str());
        cp.st = std::make_unique<AntExOps::Stage>();
        std::string e;
        if (!bf6__ex_stage(rt, ex, *cp.st, program->path.c_str(), e)) {
            cp.failed = true;
            ex.errors.push_back("clip expression " + program->path + ": " + e);
            return;
        }
    }
    bf6__ex_fill(ex, pose);
    cp.st->inst.bind_seeds();          /* the default slots take this pose's values */
    std::string e;
    if (!cp.st->inst.run(0.f, e)) ex.errors.push_back("clip expression " + program->path + ": " + e);
}
