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
