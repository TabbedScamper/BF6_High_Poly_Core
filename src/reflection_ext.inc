/* REFLECTION VOLUMES: the authored local IBL boxes and their baked cubemaps.
 *
 * A level's specular reflections come from boxes the artist placed, each
 * carrying a baked texture. Without them an editor has only the sky to reflect,
 * which is why interiors preview grey and metal reads as plastic. The data was
 * decoded and left in a test; this is that read promoted to the ABI so both
 * engines can place the same probes from the same answer.
 *
 * THE FIELD IS TYPELESS IN THIS BUILD. `BakedTexture` (0xA8285286) is reflected
 * with no type, so an ordinary schema walk truthfully reports Null and a reader
 * that trusts it finds nothing. `Ebx::import_ref` is the bounded escape hatch:
 * it reads the import table entry directly. The fake-field control below is
 * what keeps that honest - asking for a neighbouring hash that does not exist
 * must return nothing, or the escape hatch is matching anything it is handed.
 *
 * WHERE THEY LIVE. Not in a partition of their own: the volumes are instances
 * inside the level's lighting layer (measured on mp_isolated:
 * `game/glaciermp/levels/mp_isolated/_layers_content/lighting`, 35 volumes of
 * 62 instances). Their baked textures ARE separate partitions, under
 * `<area>/reflectionvolumetexture/<layer>/<guid>-tex`, and a few resolve
 * instead to `<level>/lighting/lrv/<guid>-tex`. So the search is over the
 * level's own lighting partitions, and the texture name is carried through as
 * the import path rather than rebuilt from a convention.
 *
 * THE TRANSFORM IS A FULL BASIS, not a quaternion: right, up, forward and
 * translation, exactly as a LinearTransform ships. A consumer wanting "is this
 * point inside the box" solves the point in that basis; the half-extent
 * convention is the basis length itself, so no separate size field is read. */

static const uint32_t kReflBakedTexture = 0xa8285286u;
static const uint32_t kReflFakeField    = 0xa8285287u;   /* the control */
static const uint32_t kReflTransform    = 0xd6351edeu;
static const uint32_t kReflRight        = 0xc478cc3bu;
static const uint32_t kReflUp           = 0xbf151ef9u;
static const uint32_t kReflForward      = 0x695d12a4u;
static const uint32_t kReflTrans        = 0xbc4b07b4u;
static const uint32_t kReflX            = 0x3901db14u;
static const uint32_t kReflY            = 0x42fc0f5eu;
static const uint32_t kReflZ            = 0x32a99b9cu;

static double refl_real(const bf6::EbxValue* v)
{
    if (!v) return 0.0;
    if (v->kind == bf6::EbxValue::Kind::Real) return v->f;
    if (v->kind == bf6::EbxValue::Kind::Int) return (double)v->i;
    if (v->kind == bf6::EbxValue::Kind::Uint) return (double)v->u;
    return 0.0;
}

static void refl_vec3(const bf6::EbxValue* v, float* out)
{
    out[0] = out[1] = out[2] = 0.f;
    if (!v || v->kind != bf6::EbxValue::Kind::Struct) return;
    out[0] = (float)refl_real(v->field(kReflX));
    out[1] = (float)refl_real(v->field(kReflY));
    out[2] = (float)refl_real(v->field(kReflZ));
}

int bf6_level_reflection_probes(bf6_ctx* c, const char* level,
                                bf6_reflection_probe* out, int out_max,
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

    int found = 0, controls = 0, partitions = 0;
    for (const auto& kv : c->src.ebx())
    {
        const std::string& name = kv.first;
        /* The level's own lighting layers only. A whole-level scan would parse
         * thousands of partitions to find one layer's worth of boxes, and the
         * volumes have never been seen outside it; a level whose lighting layer
         * is named differently reports zero rather than silently costing a
         * minute. */
        if (name.find("lighting") == std::string::npos) continue;
        /* level_dir_end is the shared answer to "is this partition this
         * level's", and it already knows about the group folders the 1.4.3.0
         * update added (levels/gr/, levels/mp/). Matching the level name as a
         * substring instead would pull in another level that contains this
         * one's name. */
        if (bf6::Source::level_dir_end(name, level) == std::string::npos) continue;
        /* The baked GI rasters sit beside them and are not volumes. */
        if (name.rfind("bakedgi/", 0) == 0) continue;

        std::string rerr;
        std::vector<uint8_t> raw = c->src.get_ebx(name, rerr);
        if (raw.empty()) continue;
        bf6::Ebx ebx(*c->types);
        if (!ebx.parse(std::move(raw), rerr)) continue;
        ebx.set_guid_index(&c->src.partition_index());
        partitions++;

        for (size_t i = 0; i < ebx.instance_count(); ++i)
        {
            std::string guid, path;
            if (!ebx.import_ref(i, kReflBakedTexture, guid, path)) {
                std::string g2, p2;
                if (ebx.import_ref(i, kReflFakeField, g2, p2)) controls++;
                continue;
            }
            std::string g2, p2;
            if (ebx.import_ref(i, kReflFakeField, g2, p2)) controls++;
            if (out && found < out_max)
            {
                bf6_reflection_probe& p = out[found];
                p = bf6_reflection_probe{};
                const bf6::EbxValue v = ebx.read_instance(i);
                const bf6::EbxValue* tr = v.field(kReflTransform);
                refl_vec3(tr ? tr->field(kReflRight) : nullptr, p.right);
                refl_vec3(tr ? tr->field(kReflUp) : nullptr, p.up);
                refl_vec3(tr ? tr->field(kReflForward) : nullptr, p.forward);
                refl_vec3(tr ? tr->field(kReflTrans) : nullptr, p.translation);
                /* WITHOUT THE .ebx SUFFIX. The import table names the partition
                 * with its extension; every other name in this ABI - and
                 * everything get_ebx or bf6_list_ebx will accept - is without
                 * one. Handing the raw import path out looks right and then
                 * resolves to nothing in the caller, which is how the first run
                 * of this reader reported 35 probes and 35 missing textures. */
                std::string tex = path;
                if (tex.size() > 4 && tex.compare(tex.size() - 4, 4, ".ebx") == 0)
                    tex.resize(tex.size() - 4);
                put_str(p.texture, sizeof(p.texture), tex);
                put_str(p.partition, sizeof(p.partition), name);
                put_str(p.guid, sizeof(p.guid), ebx.instance_guid(i));
            }
            found++;
        }
    }
    /* A control hit means import_ref answered for a field that does not exist,
     * which would make every probe above suspect. Reported as a failure rather
     * than folded into the count. */
    if (controls > 0)
        return fail("reflection control field resolved " + std::to_string(controls) +
                    " time(s); the import read is matching anything");
    if (found == 0 && partitions == 0)
        return fail("no lighting partition found for level '" + std::string(level) + "'");

    /* WHICH VOLUME IS THE LEVEL-WIDE FALLBACK. Decided here, once, so the two
     * editors cannot disagree about it. Compared against the MEDIAN and not the
     * mean: the outlier is exactly what is being looked for, and it would drag
     * a mean upward until it stopped looking like an outlier. */
    if (out && found > 0)
    {
        const int n = found < out_max ? found : out_max;
        auto box_volume = [](const bf6_reflection_probe& p) {
            auto len = [](const float* v) {
                return std::sqrt((double)v[0]*v[0] + (double)v[1]*v[1] + (double)v[2]*v[2]);
            };
            return len(p.right) * len(p.up) * len(p.forward);
        };
        std::vector<double> vols;
        vols.reserve((size_t)n);
        for (int i = 0; i < n; ++i) vols.push_back(box_volume(out[i]));
        std::vector<double> sorted = vols;
        std::sort(sorted.begin(), sorted.end());
        const double median = sorted.empty() ? 0.0
            : (sorted.size() % 2 ? sorted[sorted.size() / 2]
                                 : 0.5 * (sorted[sorted.size() / 2 - 1] + sorted[sorted.size() / 2]));
        for (int i = 0; i < n; ++i)
            out[i].is_global = (median > 0.0 && vols[(size_t)i] >= median * 100.0) ? 1u : 0u;
    }
    return found;
}
