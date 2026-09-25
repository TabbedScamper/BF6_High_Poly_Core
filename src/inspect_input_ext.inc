/* inspect_input_ext.inc - the weapon-inspect drag, as the soldier logic
 * computes it: camera-yaw input -> fb.camerainput.yaw.float.
 *
 * From the executable (PlayerInputTriggerEntity update FUN_140dd5280, start
 * FUN_140dd5900; the presex_clamprotator expression, evaluator
 * FUN_142484fc0) and the shipped prefab
 * common/gameplay/soldier/fl_soldier/animation/lpf_mpsoldier_animation_common,
 * whose values are READ here rather than typed in. */

namespace {
constexpr const char* kInspectPrefab = "common/gameplay/soldier/fl_soldier/animation/lpf_mpsoldier_animation_common";
constexpr uint32_t kEIACameraYaw = 0xA6AD4A3Eu;          /* djb2-exact("EIACameraYaw") */
constexpr int32_t  kPinBaseValue = 1681448475;            /* djb2-exact("BaseValue") */
constexpr int32_t  kPinClampMax  = 722785954;             /* djb2-exact("ClampMax") */
constexpr int32_t  kPinClampMin  = 722785724;             /* djb2-exact("ClampMin") */
constexpr int32_t  kPinClamped   = 761207907;             /* djb2-exact("ClampedRotation") */
constexpr int32_t  kPinYawState  = -2033173419;           /* the link id feeding fb.camerainput.yaw */

int64_t ii_iv(const bf6::EbxValue* v)
{
    if (!v) return 0;
    return v->kind == bf6::EbxValue::Kind::Int ? v->i : (int64_t)(int32_t)(uint32_t)v->u;
}
}  // namespace

int bf6_inspect_input_params_read(bf6_ctx* c, bf6_inspect_input_params* p, char* err, int err_len)
{
    auto fail = [&](const char* why) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", why);
        return 0;
    };
    if (!c || !p) return fail("missing argument");
    *p = bf6_inspect_input_params{};
    bf6ant::Graph g(c);
    const bf6ant::Obj* root = g.root(kInspectPrefab);
    if (!root) return fail("inspect prefab not in the mount");
    std::vector<const bf6ant::Obj*> objs;
    for (int i = 0; i < 1024; ++i) {
        bf6::EbxValue ref; ref.kind = bf6::EbxValue::Kind::InstanceRef; ref.instance = i;
        const bf6ant::Obj* o = g.resolve(&ref, root);
        if (!o) break;
        objs.push_back(o);
    }
    /* 1. the input trigger bound to EIACameraYaw */
    bool trig = false;
    for (const auto* o : objs) {
        const bf6::EbxValue* act = o->f(0x10E5F855u);
        if (!act || (uint32_t)ii_iv(act) != kEIACameraYaw) continue;
        p->accumulated_at_start = (float)bf6ant::numv(o->f(0x3F1BBC6Bu));
        p->value_modifier = (float)bf6ant::numv(o->f(0xF21E663Cu));
        p->value_modifier_mouse = (float)bf6ant::numv(o->f(0x41BE3949u));
        p->modifier_on_accumulated = bf6ant::boolv(o->f(0x4AABA459u)) ? 1 : 0;
        trig = true;
        break;
    }
    if (!trig) return fail("no EIACameraYaw input trigger in the prefab");
    /* 2. the clamp rotator whose ClampedRotation feeds the yaw state, and the
     *    interface values linked into its BaseValue / ClampMax / ClampMin */
    const bf6::EbxValue* links = root->f(0xc78408a3u);
    if (!links) return fail("prefab has no property links");
    int writer = -1;
    for (const auto& l : links->items)
        if ((int32_t)ii_iv(l.field(0xc41fb636u)) == kPinClamped && (int32_t)ii_iv(l.field(0x2d7314c9u)) == kPinYawState)
            writer = l.field(0x4676ba2cu) ? l.field(0x4676ba2cu)->instance : -1;
    if (writer < 0) return fail("no clamp rotator feeds the yaw state");
    bool have_base = false, have_max = false;
    for (const auto& l : links->items) {
        const bf6::EbxValue* dst = l.field(0x645b319cu);
        if (!dst || dst->instance != writer) continue;
        const int32_t dpin = (int32_t)ii_iv(l.field(0x2d7314c9u));
        if (dpin != kPinBaseValue && dpin != kPinClampMax && dpin != kPinClampMin) continue;
        const bf6::EbxValue* src = l.field(0x4676ba2cu);
        const int32_t spin = (int32_t)ii_iv(l.field(0xc41fb636u));
        if (!src || src->instance < 0 || src->instance >= (int)objs.size()) continue;
        const bf6::EbxValue* fields = objs[(size_t)src->instance]->f(0x21943083u);
        if (!fields) continue;
        for (const auto& fr : fields->items) {
            if ((int32_t)ii_iv(fr.field(0xc1ad37dau)) != spin) continue;
            const float v = (float)bf6ant::numv(fr.field(0x47f25917u));
            if (dpin == kPinBaseValue) { p->base = v; have_base = true; }
            else if (dpin == kPinClampMax) { p->clamp_max = v; have_max = true; }
            else { p->clamp_min = v; p->clamp_min_linked = 1; }
        }
    }
    if (!have_base || !have_max) return fail("clamp rotator base/max not linked from the interface");
    /* ClampMin is UNLINKED for yaw in the shipped prefab. Its effective value
     * is not established; 0 is used and clamp_min_linked = 0 says so. */
    if (!p->clamp_min_linked) p->clamp_min = 0.f;
    return 1;
}

/* --------------------------------------- which weapon the graph sees --- */

/* THE ITEM'S SoldierWeaponData, through its blueprint - shared by bf6_inspect_ids
 * and bf6_inspect_zoom_sight_types. */
static const bf6ant::Obj* bf6__weapon_data(bf6_ctx* c, const char* item, bf6ant::Graph& g, std::string& why)
{
    /* The item's leaf names the blueprint: <leaf>_wb, the same rule
     * bf6_base_weapon_stats uses. */
    std::string bare = item;
    const size_t slash = bare.find_last_of('/');
    if (slash != std::string::npos) bare = bare.substr(slash + 1);
    const std::string want = bare + "_wb";
    const int n = bf6_list_ebx(c, want.c_str(), nullptr, 0);
    std::string wb;
    if (n > 0) {
        std::vector<bf6_asset> rows((size_t)n);
        const int got = bf6_list_ebx(c, want.c_str(), rows.data(), n);
        for (int i = 0; i < got; ++i) {
            const std::string p = rows[(size_t)i].name ? rows[(size_t)i].name : "";
            const size_t s = p.find_last_of('/');
            if ((s == std::string::npos ? p : p.substr(s + 1)) == want && (wb.empty() || p.size() < wb.size())) wb = p;
        }
    }
    /* NOT EVERY BLUEPRINT IS NAMED AFTER ITS FOLDER. Gadgets and melee weapons use their
     * own names - throwables/frag holds fraggrenade_wb, mines/atmine gadget_atmine_wb,
     * throwables/c4 wb_c4, melee/combatknife wb_melee_combatknife - so when <leaf>_wb is
     * absent, the blueprint is the asset DIRECTLY in the item's own folder named *_wb or
     * wb_*, the client-schematics companion excluded. */
    if (wb.empty()) {
        const std::string folder = "/" + std::string(item) + "/";
        const int m = bf6_list_ebx(c, folder.c_str(), nullptr, 0);
        if (m > 0) {
            std::vector<bf6_asset> all((size_t)m);
            const int had = bf6_list_ebx(c, folder.c_str(), all.data(), m);
            for (int i = 0; i < had; ++i) {
                const std::string p = all[(size_t)i].name ? all[(size_t)i].name : "";
                const size_t at = p.find(folder);
                if (at == std::string::npos) continue;
                const std::string leaf = p.substr(at + folder.size());
                if (leaf.find('/') != std::string::npos) continue;   /* a subfolder: art, skins */
                const bool named = (leaf.size() > 3 && leaf.compare(leaf.size() - 3, 3, "_wb") == 0) ||
                                   (leaf.compare(0, 3, "wb_") == 0 && leaf.find("schematic") == std::string::npos);
                if (named && (wb.empty() || p.size() < wb.size())) wb = p;
            }
        }
    }
    if (wb.empty()) { why = "no weapon blueprint named " + want; return nullptr; }
    const bf6ant::Obj* root = g.root(wb);
    if (!root) { why = "weapon blueprint unreadable: " + wb; return nullptr; }
    /* SoldierWeaponBlueprint 0x9e113e77 -> SoldierWeaponData; if that link is
     * not there, the data instance is found by type rather than assumed. */
    static const char* kSoldierWeaponData = "4c01cee3-9af9-0b6e-04d7-e0c7c562a393";
    const bf6ant::Obj* data = g.resolve(root->f(0x9e113e77u), root);
    if (!data || data->type != kSoldierWeaponData) {
        data = nullptr;
        for (int i = 0; i < 4096; ++i) {
            bf6::EbxValue ref; ref.kind = bf6::EbxValue::Kind::InstanceRef; ref.instance = i;
            const bf6ant::Obj* o = g.resolve(&ref, root);
            if (!o) break;
            if (o->type == kSoldierWeaponData) { data = o; break; }
        }
    }
    if (!data) why = "no SoldierWeaponData in " + wb;
    return data;
}

int bf6_inspect_ids(bf6_ctx* c, const char* item, int32_t* specific_weapon,
                    int32_t* weapon_type, char* err, int err_len)
{
    auto fail = [&](const std::string& why) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", why.c_str());
        return 0;
    };
    if (specific_weapon) *specific_weapon = -1;
    if (weapon_type) *weapon_type = -1;          /* not established; see the header */
    if (!c || !item) return fail("missing argument");
    bf6ant::Graph g(c);
    std::string why;
    const bf6ant::Obj* data = bf6__weapon_data(c, item, g, why);
    if (!data) return fail(why);
    const bf6::EbxValue* v = data->f(0xe287c7f3u);
    if (!v) return fail("this weapon's data carries no specific-weapon value");
    if (specific_weapon) *specific_weapon = (int32_t)ii_iv(v);
    /* fb.weapontype is a table lookup, not a field: the weapon's
     * WeaponAnimBaseSetEnum (0x668292c7) indexes the conversion list the
     * soldier's weapons component carries (SoldierWeaponsComponentData
     * 0x0ec7c74b -> AnimBaseSetToAntWeaponTypeList 0xc508f787, rows of
     * {0x668292c7 base set, 0xfb58aefa ant weapon type}). The component is
     * read first so the list comes from the soldier rather than a path typed
     * in here; its own asset is the fallback when that soldier is not in the
     * mount. */
    const bf6::EbxValue* base = data->f(0x668292c7u);
    if (weapon_type && base) {
        static const char* kSoldier = "common/gameplay/soldier/glacier_soldier";
        static const char* kWeaponsComponent = "38b92884-0967-6d56-6610-10fec2436912";
        static const char* kConversionList = "common/hardware/gadgets/_logic/animbasesettoweapontypeconversion";
        const bf6ant::Obj* list = nullptr;
        if (const bf6ant::Obj* sr = g.root(kSoldier)) {
            for (int i = 0; i < 4096 && !list; ++i) {
                bf6::EbxValue ref; ref.kind = bf6::EbxValue::Kind::InstanceRef; ref.instance = i;
                const bf6ant::Obj* o = g.resolve(&ref, sr);
                if (!o) break;
                if (o->type == kWeaponsComponent) list = g.resolve(o->f(0x0ec7c74bu), o);
            }
        }
        if (!list) {
            if (const bf6ant::Obj* cr = g.root(kConversionList)) {
                for (int i = 0; i < 64 && !list; ++i) {
                    bf6::EbxValue ref; ref.kind = bf6::EbxValue::Kind::InstanceRef; ref.instance = i;
                    const bf6ant::Obj* o = g.resolve(&ref, cr);
                    if (!o) break;
                    if (o->f(0xc508f787u)) list = o;
                }
            }
        }
        const bf6::EbxValue* rows = list ? list->f(0xc508f787u) : nullptr;
        if (rows) {
            const int32_t want_base = (int32_t)ii_iv(base);
            for (const auto& r : rows->items)
                if ((int32_t)ii_iv(r.field(0x668292c7u)) == want_base) { *weapon_type = (int32_t)ii_iv(r.field(0xfb58aefau)); break; }
        }
    }
    return 1;
}

/* EACH ZOOM LEVEL'S SIGHT TYPE, in the weapon's own order (SoldierWeaponData 0xD96E9943,
 * WeaponZoomLevelData.SightType 0xE5B5C7DB): level 0 is the hip, level 1 the first aim.
 * The engine writes the current level's value to fb.wep.sighttype.enumgs; the M4A1 is
 * [0 None, 3 IronSights]. Left unset, that state holds its authored default 1, Canted,
 * and the first-person scene ops roll the weapon for a canted sight. Returns the count. */
int bf6_inspect_zoom_sight_types(bf6_ctx* c, const char* item, int32_t* out, int out_max, char* err, int err_len)
{
    if (!c || !item) return 0;
    bf6ant::Graph g(c);
    std::string why;
    const bf6ant::Obj* data = bf6__weapon_data(c, item, g, why);
    if (!data) { if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", why.c_str()); return 0; }
    const bf6::EbxValue* levels = data->f(0xd96e9943u);
    if (!levels) { if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "no zoom levels"); return 0; }
    int n = 0;
    for (const auto& ref : levels->items) {
        const bf6ant::Obj* z = g.resolve(&ref, data);
        const int32_t t = z ? (int32_t)ii_iv(z->f(0xe5b5c7dbu)) : -1;
        if (out && n < out_max) out[n] = t;
        ++n;
    }
    return n;
}

/* ------------------------------------------------ the roll's phase ---- */

namespace {
constexpr const char* kPreupdateOp = "animations/glacier/global/expressions/1p.preupdate.expsop";

/* Kernel name hash: ~CRC32, MSB-first, poly 0x04C11DB7, init ~0 (the table
 * at 0x148644ff0; checked against the program's "Or" = 0xeabfec6f). */
uint32_t ex_kernel_hash(const char* s)
{
    static uint32_t T[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i << 24;
            for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
            T[i] = c;
        }
        built = true;
    }
    uint32_t c = 0xffffffffu;
    for (; *s; ++s) c = (c << 8) ^ T[((c >> 24) ^ (uint8_t)*s) & 0xff];
    return ~c;
}

struct ExOp { uint32_t pc = 0, op = 0, fn = 0; std::vector<std::pair<uint32_t, uint32_t>> ins, outs; };

/* The EX program image (research spec_expression 2) and a walk over its
 * reachable instructions, decoding the forms the roll's sub-program uses. */
struct ExProg {
    std::vector<uint8_t> b;
    uint32_t cbase = 0, code = 0, clen = 0;
    std::map<uint32_t, uint32_t> rel;        /* pc -> kernel hash (table C) */
    uint32_t u32(size_t o) const { uint32_t v = 0; if (o + 4 <= b.size()) std::memcpy(&v, &b[o], 4); return v; }
    uint16_t u16(size_t o) const { uint16_t v = 0; if (o + 2 <= b.size()) std::memcpy(&v, &b[o], 2); return v; }
    bool open() {
        if (b.size() < 0x50) return false;
        cbase = ((uint32_t)(u16(0x3e) + u16(0x40) + u16(0x42)) * 4 + 0x5f) & ~0xfu;
        code = (u32(0x24) + 3 + cbase) & ~3u;
        clen = u32(0x2c) * 4;
        const uint32_t n38 = u16(0x38), n3a = u16(0x3a), n3c = u16(0x3c);
        const size_t relC = (size_t)code + clen + 8u * (n38 + n3a);
        if (relC + 8ull * n3c > b.size()) return false;
        for (uint32_t i = 0; i < n3c; ++i) rel[u32(relC + 8 * i + 4)] = u32(relC + 8 * i);
        return true;
    }
    float kf(uint32_t off) const { float f = 0; if ((size_t)cbase + off + 4 <= b.size()) std::memcpy(&f, &b[cbase + off], 4); return f; }
    uint8_t kb(uint32_t off) const { return (size_t)cbase + off < b.size() ? b[cbase + off] : 0; }
    std::pair<uint32_t, uint32_t> opnd(size_t o) const { return { u32(o), u32(o + 4) }; }
    bool decode(uint32_t pc, ExOp& d, uint32_t& next, std::vector<uint32_t>& targets) const {
        const size_t o = (size_t)code + pc;
        if (o + 4 > b.size()) return false;
        const uint32_t w = u32(o);
        d = ExOp{}; d.pc = pc; d.op = w & 0xff; next = w >> 8;
        auto it = rel.find(pc);
        d.fn = it == rel.end() ? 0 : it->second;
        const uint32_t op = d.op;
        if (op <= 0x13) { for (uint32_t i = 0; i <= op; ++i) d.ins.push_back(opnd(o + 0xc + 8 * i)); }
        else if (op >= 0x14 && op <= 0x1d) {
            static const int m[10][2] = { {1,1},{1,2},{1,3},{1,4},{2,0},{2,1},{3,0},{3,1},{4,0},{4,1} };
            const int ni = m[op - 0x14][0], no = m[op - 0x14][1];
            for (int i = 0; i < ni; ++i) d.ins.push_back(opnd(o + 0xc + 8 * (size_t)i));
            for (int i = 0; i < no; ++i) d.outs.push_back(opnd(o + 0xc + 8 * (size_t)(ni + i)));
        } else if (op == 0x23) {
            const uint32_t ni = u32(o + 0xc);
            size_t p = o + 0x10;
            for (uint32_t i = 0; i < ni && i < 64; ++i, p += 8) d.ins.push_back(opnd(p));
            const uint32_t no = u32(p); p += 4;
            for (uint32_t i = 0; i < no && i < 64; ++i, p += 8) d.outs.push_back(opnd(p));
        } else if (op == 0x26) targets.push_back(u32(o + 0xc));
        else if (op == 0x28) { const uint32_t n = u32(o + 0xc); for (uint32_t i = 0; i < n && i < 4096; ++i) targets.push_back(u32(o + 0x10 + 4 * (size_t)(n + i))); }
        else if (op == 0x29 || op == 0x2a) targets.push_back(u32(o + 4));
        return true;
    }
    std::vector<ExOp> walk() const {
        std::vector<ExOp> out;
        std::vector<uint32_t> todo{0};
        std::set<uint32_t> seen;
        while (!todo.empty()) {
            const uint32_t pc = todo.back(); todo.pop_back();
            if (pc >= clen || !seen.insert(pc).second) continue;
            ExOp d; uint32_t next = 0; std::vector<uint32_t> tg;
            if (!decode(pc, d, next, tg)) continue;
            out.push_back(d);
            if (d.op == 0x2b || d.op == 0x2c) continue;
            for (uint32_t t : tg) todo.push_back(t);
            todo.push_back(next);
        }
        return out;
    }
};
}  // namespace

int bf6_inspect_aim_params_read(bf6_ctx* c, bf6_inspect_aim_params* p, char* err, int err_len)
{
    auto fail = [&](const char* why) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", why);
        return 0;
    };
    if (!c || !p) return fail("missing argument");
    *p = bf6_inspect_aim_params{};
    bf6ant::Graph g(c);
    const bf6ant::Obj* op = g.root(kPreupdateOp);
    if (!op) return fail("1P pre-update expression op not in the mount");
    const bf6ant::Obj* ex = g.resolve(op->f(0x20280736u), op);
    if (!ex) return fail("expression op has no program asset");
    ExProg P;
    if (const bf6::EbxValue* img = ex->f(0x5db2cfc4u))
        for (const auto& r : img->items)
            for (uint32_t h : { 0x3901db14u, 0x42fc0f5eu, 0x32a99b9cu, 0x7c8062f2u }) {
                const uint32_t v = (uint32_t)ii_iv(r.field(h));
                for (int k = 0; k < 4; ++k) P.b.push_back((uint8_t)(v >> (8 * k)));
            }
    if (!P.open()) return fail("expression program image unreadable");
    /* input slots of the two game states */
    const bf6::EbxValue* in = ex->f(0x202e54f7u);
    const bf6::EbxValue* sl = ex->f(0x246d8578u);
    if (!in || !sl) return fail("expression inputs missing");
    std::pair<uint32_t, uint32_t> aim{~0u, ~0u}, yaw{~0u, ~0u};
    auto ends = [](const std::string& s, const char* t) { const size_t n = std::strlen(t); return s.size() >= n && s.compare(s.size() - n, n, t) == 0; };
    for (size_t i = 0; i < in->items.size() && 2 * i + 1 < sl->items.size(); ++i) {
        const bf6ant::Obj* o = g.resolve(&in->items[i], ex);
        if (!o) continue;
        const std::pair<uint32_t, uint32_t> s{ (uint32_t)ii_iv(&sl->items[2 * i]), (uint32_t)ii_iv(&sl->items[2 * i + 1]) };
        if (ends(o->path, "1p.aimleftright.phased.float")) aim = s;
        else if (ends(o->path, "fb.camerainput.yaw.float")) yaw = s;
    }
    if (aim.first == ~0u || yaw.first == ~0u) return fail("aimleftright / camera-yaw inputs not in the program");
    const std::pair<uint32_t, uint32_t> dt_slot{ (uint32_t)ii_iv(ex->f(0x5d4fe63fu)), (uint32_t)ii_iv(ex->f(0xd4c91e35u)) };
    const auto ops = P.walk();
    const uint32_t hW = ex_kernel_hash("IFloatWriter"), hR = ex_kernel_hash("IFloatReader"),
                   hS = ex_kernel_hash("SpringDampenFloat"), hC = ex_kernel_hash("ClampFloat"),
                   hM = ex_kernel_hash("MultiplyFloatFloatFloat");
    auto find = [&](uint32_t fn, auto pred) -> const ExOp* { for (const auto& d : ops) if (d.fn == fn && pred(d)) return &d; return nullptr; };
    const ExOp* w = find(hW, [&](const ExOp& d) { return d.op == 0x1a && d.ins.size() == 3 && d.ins[2] == aim; });
    if (!w) return fail("no IFloatWriter for 1p.aimleftright");
    const ExOp* s = find(hS, [&](const ExOp& d) { return d.op == 0x23 && d.ins.size() == 9 && !d.outs.empty() && d.outs[0] == w->ins[0]; });
    if (!s) return fail("aimleftright is not written from a SpringDampenFloat");
    const ExOp* cl = find(hC, [&](const ExOp& d) { return d.op == 0x3 && d.ins.size() == 4 && d.ins[3] == s->ins[0]; });
    if (!cl) return fail("spring target is not a ClampFloat");
    const ExOp* mu = find(hM, [&](const ExOp& d) { return d.op == 0x2 && d.ins.size() == 3 && d.ins[2] == cl->ins[0]; });
    if (!mu) return fail("clamp input is not a MultiplyFloatFloatFloat");
    int kside = -1;
    for (int k = 0; k < 2; ++k)
        if (find(hR, [&](const ExOp& d) { return d.op == 0x19 && d.ins.size() == 2 && d.outs.size() == 1 && d.outs[0] == mu->ins[(size_t)k] && d.ins[1] == yaw; }))
            kside = 1 - k;
    if (kside < 0) return fail("multiply does not read fb.camerainput.yaw");
    auto isK = [](const std::pair<uint32_t, uint32_t>& o) { return o.first == 0; };
    if (!isK(mu->ins[(size_t)kside]) || !isK(cl->ins[1]) || !isK(cl->ins[2]))
        return fail("multiply/clamp operands are not constants");
    for (int i : { 1, 2, 3, 5, 6, 7, 8 }) if (!isK(s->ins[(size_t)i])) return fail("spring parameters are not constants");
    if (s->ins[4] != dt_slot) return fail("spring dt is not the program's tick slot");
    p->scale = P.kf(mu->ins[(size_t)kside].second);
    p->clamp_lo = P.kf(cl->ins[1].second);
    p->clamp_hi = P.kf(cl->ins[2].second);
    p->stiffness = P.kf(s->ins[1].second);
    p->damping = P.kf(s->ins[2].second);
    p->max_step = P.kf(s->ins[3].second);
    p->lo = P.kf(s->ins[5].second);
    p->hi = P.kf(s->ins[6].second);
    p->reset_input = P.kb(s->ins[7].second) ? 1 : 0;
    p->wrap = P.kb(s->ins[8].second) ? 1 : 0;
    if (p->wrap) return fail("spring wrap mode is on; its branch (FUN_1405dc0b0) is not transcribed");
    return 1;
}

float bf6_inspect_aim_step(const bf6_inspect_aim_params* p, bf6_inspect_aim_state* s, float yaw, float dt)
{
    if (!p || !s) return 0.f;
    /* MultiplyFloatFloatFloat, ClampFloat (max then min) */
    const float target = std::fmin(std::fmax(yaw * p->scale, p->clamp_lo), p->clamp_hi);
    /* SpringDampenFloat, 0x1405dc4c0 + FUN_1405dc110 (x lane) */
    if (!s->init || p->reset_input) { s->pos = target; s->vel = 0.f; s->init = 1; }
    if (!(dt > 1.52587890625e-05f || dt < -1.52587890625e-05f)) return s->pos;
    const bool ranged = std::fabs(p->hi - p->lo) > 1.1920928955078125e-07f;
    const float tc = ranged ? std::fmin(p->hi, std::fmax(p->lo, target)) : target;
    const float h = dt * 0.01666666753590107f;
    const float k = (h * p->stiffness) * h;
    const float sq = std::sqrt(k);
    const float a = std::fmax(1.f - k, 0.f);
    const float b = std::fmax(1.f - (p->damping + p->damping) * sq, 0.f);
    const float pos = s->pos, vel = s->vel;
    const float d = pos - tc;
    const float bv = b * vel;
    const float vel_new = ((a - 1.f) * d) / h + bv;
    float delta = ((a * d + h * bv) + tc) - pos;
    delta = std::fmin(p->max_step, std::fmax(-1.f * p->max_step, delta));
    float np = delta + pos;
    if (ranged) np = std::fmin(p->hi, std::fmax(p->lo, np));
    s->pos = np; s->vel = vel_new;
    return np;
}

float bf6_inspect_input_step(const bf6_inspect_input_params* p, bf6_inspect_input_state* s,
                             int inspecting, float raw, int from_mouse)
{
    if (!p || !s) return 0.f;
    if (inspecting && !s->active) {
        /* Enable (FUN_140dd5900): accumulator := AccumulatedValueAtStart,
         * and the graph's Reset? goes true on the inspection-active edge. */
        s->accumulated = p->accumulated_at_start;
        s->reset = 1;
    }
    s->active = inspecting ? 1 : 0;
    if (!inspecting) return s->yaw;           /* Disable keeps values; the state is not written */
    /* FUN_140dd5280: A = clamp(A + d, -1, 1), d = raw * modifier */
    const float m = from_mouse ? p->value_modifier_mouse : p->value_modifier;
    const float r = std::fmin(std::fmax(raw, -1.f), 1.f);
    const float d = p->modifier_on_accumulated ? r * m : r;
    s->accumulated = std::fmin(std::fmax(s->accumulated + d, -1.f), 1.f);
    /* presex_clamprotator */
    if (s->reset) { s->start = s->accumulated; s->last_frame = p->base; s->rotation = p->base; s->reset = 0; }
    const float t = s->accumulated - s->start + p->base;
    const float out = std::fmin(std::fmax(t - s->rotation + s->last_frame, p->clamp_min), p->clamp_max);
    s->last_frame = out;
    s->rotation = t;
    s->yaw = out;
    return out;
}
