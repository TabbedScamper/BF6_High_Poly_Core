/* THE SOLDIER'S AUTHORED MOVEMENT VALUES, READ FROM THE INSTALL.
 *
 * common/gameplay/soldier/grx_glacier_soldier is a flat GameRemixer table: 260
 * named tweakables, each instance carrying its name in field 0x0c59fa06 and its
 * authored value in 0x42c8b257. This reads the ones the walker can use.
 *
 * WHY READ RATHER THAN BAKE. Every number here was decoded by hand once, and
 * pasting the results into a constants block would have been quicker. It would
 * also have been a second copy of the game's data that drifts the first time
 * DICE retunes anything, and a reader with no way to notice. The values come
 * out of the player's own install for the same reason every other value in this
 * library does.
 *
 * WHAT IS NOT HERE. There is no absolute walk or sprint speed in this table, or
 * in any other GameRemixer partition - the whole set is multipliers and
 * thresholds, because BF6's locomotion is motion-matched and the base is a
 * property of the animation set. So bf6_walk_tuning keeps the engine's own
 * speeds and takes from the game only what the game actually states.
 */

namespace {

const char* const kSoldierRemixer = "common/gameplay/soldier/grx_glacier_soldier";

/* The two fields that make a tweakable. Verified against five entries of
 * different kinds - a multiplier, a velocity cap, an impulse length, a height
 * and a strafe speed - rather than generalised from one. */
const uint32_t kTweakName  = 0x0c59fa06;
const uint32_t kTweakValue = 0x42c8b257;

/* The rig and skeleton a third-person locomotion cycle binds against. The pair
 * is not interchangeable with the first-person one. */
const char* const kRig3pMotion = "animations/glacier/global/rigging/soldier_3p.rig";
const char* const kSkeleton3pMotion = "common/characters/_soldier/ske_soldier_3p";

/* Name -> where it lands in the tuning. One table so that adding a value is one
 * line and cannot get out of step with the struct. */
struct TweakSlot { const char* name; float bf6_walk_tuning::*field; };

const TweakSlot kSlots[] = {
    {"StepHeight",                         &bf6_walk_tuning::step_height},
    {"Jump_HorizontalVelocityCap",         &bf6_walk_tuning::jump_horizontal_cap},
    {"Jump_MinSpeedForHorizontalImpulse",  &bf6_walk_tuning::jump_min_speed_for_impulse},
    {"Jump_StrafeSpeed",                   &bf6_walk_tuning::jump_strafe_speed},
    {"Jump_LandingPenalty_Strength",       &bf6_walk_tuning::landing_penalty_strength},
    {"Jump_LandingPenalty_MinScaleClamp",  &bf6_walk_tuning::landing_penalty_floor},
    {"Jump_LandingRecovery_IncreasePerSecond", &bf6_walk_tuning::landing_recovery_per_second},
    {"Slide.Threshold.NeededVelocity",     &bf6_walk_tuning::slide_needed_velocity},
    {"Slide.CooldownDuration",             &bf6_walk_tuning::slide_cooldown},
    {"Slide.ImpulseLength",                &bf6_walk_tuning::slide_impulse},
    {"Vault_StartHeightMax",               &bf6_walk_tuning::vault_max_height},
};

} // namespace

extern "C" void bf6_walk_tuning_defaults(bf6_walk_tuning* t)
{
    if (!t) return;
    *t = bf6_walk_tuning{};
    /* THE ENGINE'S OWN CHARACTER DEFAULTS, and they are the honest fallback
     * rather than a guess at BF6's: the game does not author these. */
    t->crouch_speed = 1.4f;
    t->walk_speed   = 2.6f;
    t->run_speed    = 6.0f;
    t->jump_speed   = 4.2f;
    t->gravity      = 9.8f;
    t->accel_ground = 24.0f;
    t->accel_air    = 7.0f;
    t->radius       = 0.35f;
    t->crouch_drop  = 0.72f;
    /* Authored values, used when the install cannot be read. Kept in step with
     * the table above; bf6_walk_tuning_read replaces them with the install's
     * own, which is what should normally happen. */
    t->step_height                 = 0.32f;
    t->jump_horizontal_cap         = 8.0f;
    t->jump_min_speed_for_impulse  = 3.6f;
    t->jump_strafe_speed           = 0.125f;
    t->landing_penalty_strength    = 9.0f;
    t->landing_penalty_floor       = 0.5f;
    t->landing_recovery_per_second = 0.5f;
    t->slide_needed_velocity       = 4.8f;
    t->slide_cooldown              = 1.0f;
    t->slide_impulse               = 25.0f;
    t->vault_max_height            = 1.97f;
    t->fov_vertical                = 59.0f;
    t->fov_vertical_min            = 55.0f;
    t->fov_vertical_max            = 89.0f;
}

/* HOW FAST A GAIT ACTUALLY TRAVELS, measured from the game's own animation.
 *
 * BF6 authors no absolute walk or sprint speed, and for a while that read as
 * "the number is missing". It is not missing - it is the animation. Locomotion
 * is motion-matched, so the character moves because the ROOT MOVES, and the
 * speed of a gait is the distance its cycle covers divided by its duration.
 *
 * Measured rather than pasted in, for the same reason the tweakables are read
 * rather than baked: a retuned game retunes this with it.
 *
 * Returns 0 when the clip cannot be read - several locomotion clips will not
 * bind, which is a known and separate gap - and the caller keeps its default. */
float measure_gait_speed(bf6_ctx* c, const char* clip)
{
    bf6_anim_binding_stats st{};
    const int n = bf6_anim_bindings(c, clip, kRig3pMotion, kSkeleton3pMotion, nullptr, 0, &st);
    if (n < 1 || n > 4096) return 0.f;
    std::vector<bf6_anim_binding> b((size_t)n);
    if (bf6_anim_bindings(c, clip, kRig3pMotion, kSkeleton3pMotion, b.data(), n, &st) != n)
        return 0.f;
    /* The root translation: the lowest bone index carrying a VECTOR3 channel,
     * which on this rig is AITrajectory. Taking the lowest rather than naming
     * the bone means a renamed rig still measures. */
    int best = 1 << 30, channel = -1;
    for (const bf6_anim_binding& x : b)
        if (x.component == BF6_ANIM_DOF_VECTOR3 && x.bone >= 0 && x.bone < best && x.channel >= 0) {
            best = x.bone;
            channel = x.channel;
        }
    if (channel < 0) return 0.f;
    bf6_anim_clip* h = bf6_anim_clip_open(c, clip);
    if (!h || h->key_time_count < 2) { if (h) bf6_free(c, h); return 0.f; }
    const int frames = h->key_time_count;
    std::vector<float> s((size_t)h->channel_count * 4, 0.f);
    double first[3] = {0, 0, 0}, last[3] = {0, 0, 0};
    bool ok = true;
    for (int f = 0; f < frames; ++f) {
        if (!bf6_anim_clip_sample(c, h, f, s.data(), nullptr)) { ok = false; break; }
        const float* v = s.data() + (size_t)channel * 4;
        if (f == 0) for (int k = 0; k < 3; ++k) first[k] = v[k];
        for (int k = 0; k < 3; ++k) last[k] = v[k];
    }
    bf6_free(c, h);
    if (!ok) return 0.f;
    double d = 0.0;
    for (int k = 0; k < 3; ++k) d += (last[k] - first[k]) * (last[k] - first[k]);
    /* THE FRAME RATE IS AN ASSUMPTION and every speed scales with it. 60 is
     * what the front-end ClipPlayer owners and the 1P pose assets author. */
    const double seconds = (double)(frames - 1) / 60.0;
    return seconds > 0.0 ? (float)(std::sqrt(d) / seconds) : 0.f;
}

/* One clip per gait. Several plain cycles (stand_walk_fwd, stand_run_fwd) do
 * not bind at all, so these are the spellings that measured - the lowlodcopy is
 * the same motion at a lower mesh LOD, not a different gait. */
const char* const kGaitCrouch =
    "animations/glacier/assets/3p/common/rifle/loco/c_3p_rifle_crouch_sneak_fwd_01";
const char* const kGaitWalk =
    "animations/glacier/assets/3p/common/rifle/loco/c_3p_rifle_stand_run_fwd_lowlodcopy_01";
const char* const kGaitRun =
    "animations/glacier/assets/3p/common/rifle/loco/c_3p_rifle_stand_sprint_fwd_01";

extern "C" int bf6_walk_tuning_read(bf6_ctx* c, bf6_walk_tuning* t)
{
    if (!t) return 0;
    bf6_walk_tuning_defaults(t);
    if (!c) return 0;
    /* THE TYPE DATABASE IS WHAT DECODES A FIELD BY NAME, and it is loaded by
     * opening a LEVEL - which this has no business doing. ensure_types is the
     * same helper the armory reads through, and without it c->types is null:
     * the first version dereferenced it and segfaulted rather than falling back
     * to the defaults it had just filled in. */
    std::string types_err;
    if (!ensure_types(c, types_err) || !c->types) return 0;
    char why[512] = {0};
    bf6_mount_frontend(c, why, sizeof(why));

    int found = 0;
    std::string err;
    std::vector<uint8_t> raw = c->src.get_ebx(kSoldierRemixer, err);
    if (raw.empty()) {
        /* The soldier remixer is not in the front-end mount on every install. */
        bf6_mount_all(c, 1, why, sizeof(why));
        raw = c->src.get_ebx(kSoldierRemixer, err);
    }
    if (!raw.empty()) {
        bf6::Ebx e(*c->types);
        if (e.parse(std::move(raw), err)) {
            const std::vector<uint32_t> want = {kTweakName, kTweakValue};
            for (size_t i = 0; i < e.instance_count(); ++i) {
                const bf6::EbxValue v = e.read_instance(i, &want);
                if (v.kind != bf6::EbxValue::Kind::Struct) continue;
                const bf6::EbxValue* n = v.field(kTweakName);
                const bf6::EbxValue* x = v.field(kTweakValue);
                if (!n || !x || n->kind != bf6::EbxValue::Kind::Str) continue;
                /* The partition names ITSELF in the same field on one row. It
                 * contains a slash and no tweakable does, which is how that row
                 * is told apart from a real entry. */
                if (n->s.find('/') != std::string::npos) continue;
                double value = 0.0;
                if (x->kind == bf6::EbxValue::Kind::Real) value = x->f;
                else if (x->kind == bf6::EbxValue::Kind::Int) value = (double)x->i;
                else if (x->kind == bf6::EbxValue::Kind::Uint) value = (double)x->u;
                else continue;
                for (const TweakSlot& slot : kSlots)
                    if (n->s == slot.name) { t->*(slot.field) = (float)value; ++found; break; }
            }
        }
    }

    /* THE SPEEDS, MEASURED FROM THE GAME'S OWN GAITS.
     *
     * These were the engine's character defaults because no partition states
     * them. They are stated - as animation. Each gait's cycle is sampled and
     * its root displacement divided by its duration, which is what a
     * motion-matched character's speed IS. A clip that will not bind leaves its
     * default alone rather than reporting zero. */
    const float crouch = measure_gait_speed(c, kGaitCrouch);
    const float walk = measure_gait_speed(c, kGaitWalk);
    const float run = measure_gait_speed(c, kGaitRun);
    if (crouch > 0.1f && crouch < 20.f) { t->crouch_speed = crouch; ++found; }
    if (walk > 0.1f && walk < 20.f) { t->walk_speed = walk; ++found; }
    if (run > 0.1f && run < 20.f) { t->run_speed = run; ++found; }

    /* Field of view is a player OPTION, not a soldier tweakable: a default with
     * a range the player moves inside. Vertical degrees. */
    std::vector<uint8_t> fov = c->src.get_ebx("common/gamesetup/options/render/optionfieldofview", err);
    if (!fov.empty()) {
        bf6::Ebx e(*c->types);
        if (e.parse(std::move(fov), err) && e.instance_count() > 0) {
            const bf6::EbxValue v = e.read_instance(0);
            const bf6::EbxValue* lo = v.field(0x8ee32a15);
            const bf6::EbxValue* hi = v.field(0xe2253f2b);
            const bf6::EbxValue* def = v.field(0x8cf424e7);
            auto as_f = [](const bf6::EbxValue* x, float fallback) -> float {
                if (!x) return fallback;
                if (x->kind == bf6::EbxValue::Kind::Real) return (float)x->f;
                if (x->kind == bf6::EbxValue::Kind::Int) return (float)x->i;
                if (x->kind == bf6::EbxValue::Kind::Uint) return (float)x->u;
                return fallback;
            };
            t->fov_vertical_min = as_f(lo, t->fov_vertical_min);
            t->fov_vertical_max = as_f(hi, t->fov_vertical_max);
            t->fov_vertical     = as_f(def, t->fov_vertical);
            ++found;
        }
    }
    return found;
}
