/* PLACED SPATIAL SOUND EMITTERS: where the level puts its ambient bed.
 *
 * Neither editor read a single sound emitter, which made every rebuild silent
 * while the game's world is full of authored sound. This is the placeable half
 * of that: DiceSoundSpatialEntityData, present on 25 of 28 levels, about 100 on
 * a busy one (mp_aftermath 102, mp_abbasid 93).
 *
 * NOT the whole of "audio". The dense per-object audio types are cells of a
 * `<level>_soundshapegrid` - two uints and a bool, no transform, no sound - and
 * are an acoustics lookup structure rather than sources. Reading THOSE as
 * emitters would place thousands of silent markers; see MAP-PARITY-LEDGER.md.
 *
 * WHAT AN EMITTER CARRIES, measured across every instance rather than taken
 * from the type dump (which lists only the type's own fields and so omits the
 * transform that inheritance contributes):
 *
 *   d6351ede  LinearTransform   the SAME hash the reflection volumes use
 *   d56fbb5b  import            the sound CONFIG partition
 *   d6aac566  real              amplitude, 1.0 on every instance sampled
 *   2dbb6fb1  bool              play on creation
 *   5645e663  uint              a per-instance id
 *
 * All five appear on 100% of instances, so none is optional and none needs a
 * default invented for it.
 *
 * THE SOUND IS PLAYABLE, which is what makes this worth building rather than
 * decorating: bf6_ui_sound_decode already accepts a sound config and returns
 * PCM, verified on this level's three assets at 48 kHz. So a consumer can place
 * the emitter AND hear what the game hears there.
 *
 * The import path carries ".ebx" and every name the mount accepts is without
 * one - the same trap that made the reflection reader report 35 probes and 35
 * missing textures - so it is stripped here, once. */

static const uint32_t kSndTransform = 0xd6351edeu;   /* shared with reflection volumes */
static const uint32_t kSndAsset     = 0xd56fbb5bu;
static const uint32_t kSndAmplitude = 0xd6aac566u;
static const uint32_t kSndPlay      = 0x2dbb6fb1u;
static const uint32_t kSndFlags     = 0x5645e663u;   /* Flags, per the field dictionary */
static const uint32_t kSndFake      = 0xd56fbb5cu;   /* the control */

/* ON THE SOUND CONFIG, not on the emitter. The emitter has no falloff of its
 * own, so a consumer left to itself would invent one; these are read here once
 * per distinct config so both engines attenuate the same way. Names come from
 * the repo's field dictionary rather than from the values looking right. */
static const uint32_t kSndCfgRadius   = 0x5cda5a37u;   /* Radius   */
static const uint32_t kSndCfgLoudness = 0xb0fad94du;   /* Loudness */

static const uint32_t kSndRight = 0xc478cc3bu;
static const uint32_t kSndUp    = 0xbf151ef9u;
static const uint32_t kSndFwd   = 0x695d12a4u;
static const uint32_t kSndTrans = 0xbc4b07b4u;
static const uint32_t kSndX = 0x3901db14u;
static const uint32_t kSndY = 0x42fc0f5eu;
static const uint32_t kSndZ = 0x32a99b9cu;

/* DiceSoundSpatialEntityData. Matched on the type guid rather than a name,
 * because the name comes from a side table and the guid is in the partition. */
static const char* kSndTypePrefix = "4450a080";

static double snd_real(const bf6::EbxValue* v)
{
    if (!v) return 0.0;
    if (v->kind == bf6::EbxValue::Kind::Real) return v->f;
    if (v->kind == bf6::EbxValue::Kind::Int) return (double)v->i;
    if (v->kind == bf6::EbxValue::Kind::Uint) return (double)v->u;
    return 0.0;
}

static void snd_vec3(const bf6::EbxValue* v, float* out)
{
    out[0] = out[1] = out[2] = 0.f;
    if (!v || v->kind != bf6::EbxValue::Kind::Struct) return;
    out[0] = (float)snd_real(v->field(kSndX));
    out[1] = (float)snd_real(v->field(kSndY));
    out[2] = (float)snd_real(v->field(kSndZ));
}

int bf6_level_sound_emitters(bf6_ctx* c, const char* level,
                             bf6_sound_emitter* out, int out_max,
                             char* err, int err_len)
{
    auto fail = [&](const std::string& m) {
        if (err && err_len > 0) put_str(err, (size_t)err_len, m);
        return -1;
    };
    if (!c || !level || !*level) return fail("bad arguments");
    std::string e;
    if (!ensure_mounted(c, level, e)) return fail(e);
    if (!ensure_types(c, e)) return fail(e);

    int found = 0, controls = 0;
    /* One read per distinct config, not per emitter: a level reuses the same
     * ambience many times (35 distinct sounds over 102 emitters on
     * mp_aftermath), and re-parsing it per placement would be the bulk of the
     * cost for no new answer. */
    std::map<std::string, std::pair<float, float>> cfg_cache;
    auto config_falloff = [&](const std::string& path) -> std::pair<float, float> {
        auto it = cfg_cache.find(path);
        if (it != cfg_cache.end()) return it->second;
        std::pair<float, float> rl(0.f, 0.f);
        std::string ce;
        std::vector<uint8_t> craw = c->src.get_ebx(path, ce);
        if (!craw.empty()) {
            bf6::Ebx cebx(*c->types);
            if (cebx.parse(std::move(craw), ce)) {
                cebx.set_guid_index(&c->src.partition_index());
                for (size_t i = 0; i < cebx.instance_count(); ++i) {
                    const bf6::EbxValue cv = cebx.read_instance(i);
                    const bf6::EbxValue* r = cv.field(kSndCfgRadius);
                    const bf6::EbxValue* l = cv.field(kSndCfgLoudness);
                    if (r || l) {
                        rl.first = (float)snd_real(r);
                        rl.second = (float)snd_real(l);
                        break;   /* the config root carries them */
                    }
                }
            }
        }
        cfg_cache[path] = rl;
        return rl;
    };

    for (const auto& kv : c->src.ebx())
    {
        /* level_dir_end is the shared answer to "is this partition this
         * level's", and it already knows the 1.4.3.0 group folders. */
        if (bf6::Source::level_dir_end(kv.first, level) == std::string::npos) continue;
        std::string rerr;
        std::vector<uint8_t> raw = c->src.get_ebx(kv.first, rerr);
        if (raw.empty()) continue;
        bf6::Ebx ebx(*c->types);
        if (!ebx.parse(std::move(raw), rerr)) continue;
        ebx.set_guid_index(&c->src.partition_index());

        for (size_t i = 0; i < ebx.instance_count(); ++i)
        {
            const std::string tg = bf6::TypeDb::guid_str(ebx.instance_type(i));
            if (tg.compare(0, std::strlen(kSndTypePrefix), kSndTypePrefix) != 0) continue;
            const bf6::EbxValue v = ebx.read_instance(i);
            /* A neighbouring hash that does not exist must stay null, or the
             * field read is matching anything and every row is suspect. */
            if (v.field(kSndFake)) controls++;
            if (out && found < out_max)
            {
                bf6_sound_emitter& s = out[found];
                s = bf6_sound_emitter{};
                const bf6::EbxValue* tr = v.field(kSndTransform);
                float right[3], up[3], fwd[3], trans[3];
                snd_vec3(tr ? tr->field(kSndRight) : nullptr, right);
                snd_vec3(tr ? tr->field(kSndUp) : nullptr, up);
                snd_vec3(tr ? tr->field(kSndFwd) : nullptr, fwd);
                snd_vec3(tr ? tr->field(kSndTrans) : nullptr, trans);
                /* 3x4 row-major, the same layout every other placement uses. */
                for (int k = 0; k < 3; ++k) {
                    s.xform[k]     = right[k];
                    s.xform[3 + k] = up[k];
                    s.xform[6 + k] = fwd[k];
                    s.xform[9 + k] = trans[k];
                }
                const bf6::EbxValue* a = v.field(kSndAsset);
                if (a && a->kind == bf6::EbxValue::Kind::ImportRef) {
                    std::string p = a->import_path;
                    if (p.size() > 4 && p.compare(p.size() - 4, 4, ".ebx") == 0)
                        p.resize(p.size() - 4);
                    put_str(s.sound, sizeof(s.sound), p);
                    const std::pair<float, float> rl = config_falloff(p);
                    s.radius = rl.first;
                    s.loudness = rl.second;
                }
                s.amplitude = (float)snd_real(v.field(kSndAmplitude));
                const bf6::EbxValue* pl = v.field(kSndPlay);
                s.play_on_creation = (pl && pl->kind == bf6::EbxValue::Kind::Bool && pl->b) ? 1u : 0u;
                const bf6::EbxValue* fl = v.field(kSndFlags);
                s.flags = fl ? (unsigned int)fl->u : 0u;
                put_str(s.partition, sizeof(s.partition), kv.first);
            }
            found++;
        }
    }
    if (controls > 0)
        return fail("sound emitter control field resolved " + std::to_string(controls) +
                    " time(s); the field read is matching anything");
    return found;
}
