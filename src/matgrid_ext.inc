/* MATERIAL RELATION GRID: what happens when material A meets material B.
 *
 * Every impact in the game asks this question. A bullet hits a surface and the
 * game must choose a sound, an impact effect, a decal, a footprint, a
 * penetration rule. The answer is authored per level as a square matrix over
 * the game's material ids, and this is the read of it.
 *
 * WHY THE QUERY IS PER PAIR AND NOT A DUMP. The grid is the level's material
 * count squared - 198,025 cells on mp_subsurface, 248,004 on mp_isolated - and
 * a caller only ever wants one cell: the pair it just collided. Handing out the
 * whole matrix would move megabytes so the caller could index one row. The
 * partition is parsed once per level and cached on the context, so the first
 * query pays for the decode and the rest are a map lookup.
 *
 * THE DECLARED DIMENSION IS A TRAP. The root declares 128 on every level
 * measured while the real side is the level's own material count, 445 to 498.
 * A reader that sizes the grid from the declared value reads the wrong
 * rectangle and returns another pair's relations, which is worse than failing.
 * The side is taken from the grid array itself and cross-checked against the
 * relation-group array, which has matched on every level measured.
 *
 * THE ID SPACE IS GLOBAL, THE ROWS ARE LOCAL, AND THE MAP IS MANY-TO-ONE. The
 * id map is 785 entries on every level and every id resolves, but onto only 445
 * to 498 distinct rows - 287 to 340 ids alias onto another id's row. A level
 * therefore does not ship a subset of the materials, it draws fewer
 * distinctions between them. Callers pass GLOBAL ids and this does the
 * per-level row lookup; a caller that carried row indices between levels would
 * silently read the wrong material. The -2 "not shipped" answer below is a real
 * code path that no measured level exercises.
 *
 * THE PAIR IS ORDERED. Occupancy is symmetric except for exactly four authored
 * one-way pairs per level, so (a,b) is answered as the cell it names and is not
 * symmetrised. Folding the transpose in would invent relations on the four
 * pairs an artist deliberately left one way.
 *
 * WHAT A RELATION IS. The cell holds (kind, instance reference) pairs. The
 * instance's TYPE is the subsystem that responds: on mp_subsurface 20,001 of
 * 49,356 are the AudioLegacyGameplay relation, more than effects, decals and
 * everything else combined, then MaterialRelationEffectData (17,831),
 * MaterialRelationDecalData (6,027) and MaterialRelationTriggarableEffectData
 * (4,177). The type is handed out as its guid rather than a name because the
 * shipped reflection carries a name HASH and not a name, and that hash is not
 * derivable from the name - djb2, FNV-1, FNV-1a, sdbm and K&R reproduce 0 of 7
 * known names across three cases, six seeds and five multipliers. Naming is the
 * caller's join against a type table, not a guess made here.
 *
 * THE LEAVES STILL DO NOT DECODE, AND THAT IS FINE. 7,737 of 7,833
 * MaterialRelation*Data instances reflect every field at offset 0xFFFF, so a
 * schema walk into one truthfully returns nothing. This reader never walks into
 * them: the arrangement is the answer, and the arrangement is in the root. */

static const uint32_t kMgDim       = 0xe6c1e5adu;
static const uint32_t kMgDimValue  = 0x48288193u;
static const uint32_t kMgIdMap     = 0x8e6ca5fcu;
static const uint32_t kMgGroups    = 0xdb25cf83u;
static const uint32_t kMgGrid      = 0x24535855u;
static const uint32_t kMgGridRow   = 0x2c95d7b7u;
static const uint32_t kMgCellList  = 0x2f0bed21u;
static const uint32_t kMgCellItems = 0xa2a9e9a7u;
static const uint32_t kMgItemKind  = 0xddcb6879u;
static const uint32_t kMgItemRel   = 0x2adefb09u;
static const uint32_t kMgFakeField = 0x2adefb0au;   /* the control */

/* WHAT EACH RELATION TYPE IS, by type guid.
 *
 * The shipped reflection carries a name HASH and not a name, and that hash is
 * not derivable from the name, so the leaf names below come from the SDK type
 * dump and the categories from either the name or the type's runtime namespace.
 * Recording them here rather than in each editor is the point: a relation must
 * read as the same thing in Godot and in Unreal, and that is one decision.
 *
 * `name` is empty where no corpus carries the leaf name; the category is still
 * known from the namespace, and the namespace is what answers "what responds
 * when these two surfaces meet". 30 types cover four levels measured with no
 * remainder; anything else falls through to an empty category, which the UI
 * shows as the raw guid rather than guessing.
 *
 * NOTE the biggest row: 21888a08 is AUDIO and is the densest relation in the
 * game, 20,001 of 49,356 references on mp_subsurface. */
struct MgTypeInfo { const char* prefix; const char* category; const char* name; };
static const MgTypeInfo kMgTypes[] = {
    {"21888a08", "audio",       ""},                                        /* ns AudioLegacyGameplay */
    {"310bdc0d", "effect",      "MaterialRelationEffectData"},
    {"8762ec89", "decal",       "MaterialRelationDecalData"},
    {"26ee2707", "effect",      "MaterialRelationTriggarableEffectData"},
    {"9054b17b", "shooter",     ""},                                        /* ns DiceShooterShared */
    {"cfd3812a", "audio",       ""},                                        /* ns AudioLegacyGameplay */
    {"0d16cb78", "footprint",   "MaterialRelationFootPrintData"},
    {"13b2a24a", "fire",        "MaterialRelationDynamicFireData"},
    {"1ac4a694", "physics",     "MaterialPropertyPhysicsData"},
    {"2d7a79b0", "motion",      ""},                                        /* ns MotionMachineShared */
    {"314e982c", "animation",   "MaterialPropertyAnimationData"},
    {"335e9c75", "drake",       ""},                                        /* ns DrakeShared */
    {"45b612a1", "terrain",     "MaterialPropertyTerrainData"},
    {"4f1bc065", "effect",      "MaterialPropertyEffectData"},
    {"52c80cda", "gameplay",    ""},                                        /* ns ExampleGameplayShared */
    {"56fa929a", "audio",       "DiceShooterMaterialPropertySoundData"},
    {"5e9b7b5c", "audio",       ""},                                        /* ns AudioLegacyGameplay */
    {"7df5ca5f", "battlefield", ""},                                        /* ns BattlefieldShared */
    {"8d3a0617", "gameplay",    ""},                                        /* ns ExampleGameplayShared */
    {"99037f92", "",            ""},                                        /* in no corpus here at all */
    {"a1b0f70d", "pathfinding", "PathfindingMaterialPropertyData"},
    {"a4428250", "destruction", "MaterialPropertyPhysicsDestructionData"},
    {"c8611458", "fire",        "MaterialPropertyFireData"},
    {"cda270d1", "weapon",      ""},                                        /* ns BattlefieldWeaponShared */
    {"d605e78b", "destruction", ""},                                        /* ns BattlefieldDestructionShared */
    {"d818121c", "battlefield", ""},                                        /* ns BattlefieldShared */
    {"df321576", "weathering",  "MaterialPropertyWeatheringData"},
    {"e257ec8d", "audio",       ""},                                        /* ns AudioLegacyGameplay */
    {"e6c963d3", "vehicle",     "MaterialRelationVehicleData"},
    {"e7f7c744", "gameplay",    ""},                                        /* ns GameplaySim */
};

/* The canonical category order. Index 0 is the uncategorised bucket, so a type
 * this table does not know is COUNTED rather than dropped: an editor showing
 * "3 uncategorised" is telling the truth, one showing nothing is not. */
static const char* const kMgCategories[BF6_MATERIAL_CATEGORIES] = {
    "uncategorised", "audio", "effect", "decal", "footprint", "fire", "physics",
    "destruction", "vehicle", "terrain", "animation", "pathfinding", "weathering",
    "motion", "shooter", "weapon", "gameplay", "battlefield", "drake", ""
};

static int mg_category_index(const char* cat)
{
    if (!cat || !*cat) return 0;
    for (int i = 0; i < BF6_MATERIAL_CATEGORIES; ++i)
        if (kMgCategories[i][0] && std::strcmp(kMgCategories[i], cat) == 0) return i;
    return 0;
}

static const MgTypeInfo* mg_type_info(const std::string& guid)
{
    for (const MgTypeInfo& t : kMgTypes)
        if (guid.compare(0, 8, t.prefix) == 0) return &t;
    return nullptr;
}

static uint64_t mg_cell_key(int row, int col)
{
    return ((uint64_t)(uint32_t)row << 32) | (uint32_t)col;
}

/* Build the level's grid once. Returns null and fills err on failure. */
static const bf6_matgrid_cache* mg_load(bf6_ctx* c, const char* level, std::string& e)
{
    if (c->matgrid.level == level && c->matgrid.side > 0) return &c->matgrid;
    if (!ensure_mounted(c, level, e)) return nullptr;
    if (!ensure_types(c, e)) return nullptr;

    /* Found by scanning the mount, not by rebuilding a path: the 1.4.3.0 update
     * moved Portal levels under group folders and a rebuilt path misses them
     * without saying so. */
    std::string want;
    for (const auto& kv : c->src.ebx()) {
        if (kv.first.find("materialgrid") == std::string::npos) continue;
        if (bf6::Source::level_dir_end(kv.first, level) == std::string::npos) continue;
        want = kv.first;
        break;
    }
    if (want.empty()) { e = "no material grid for level '" + std::string(level) + "'"; return nullptr; }

    std::vector<uint8_t> raw = c->src.get_ebx(want, e);
    if (raw.empty()) return nullptr;
    bf6::Ebx ebx(*c->types);
    if (!ebx.parse(std::move(raw), e)) return nullptr;
    ebx.set_guid_index(&c->src.partition_index());

    const bf6::EbxValue root = ebx.read_instance(0);
    const bf6::EbxValue* dim    = root.field(kMgDim);
    const bf6::EbxValue* idmap  = root.field(kMgIdMap);
    const bf6::EbxValue* groups = root.field(kMgGroups);
    const bf6::EbxValue* grid   = root.field(kMgGrid);
    if (!idmap || !groups || !grid) { e = "root of " + want + " is not a material grid"; return nullptr; }

    bf6_matgrid_cache g;
    g.level     = level;
    g.partition = want;
    g.side      = (int)grid->items.size();
    g.declared  = dim && dim->field(kMgDimValue) ? (int)dim->field(kMgDimValue)->u : 0;

    /* The side must agree with the relation-group array. This is the check that
     * catches sizing the grid from the declared dimension, and it has held on
     * every level measured. */
    if ((int)groups->items.size() != g.side) {
        e = "grid side " + std::to_string(g.side) + " disagrees with " +
            std::to_string(groups->items.size()) + " relation groups in " + want;
        return nullptr;
    }

    /* THE MAP IS INJECTIVE PLUS A FALLBACK, which is not what it looks like.
     * On mp_subsurface 341 of the 785 ids map to row 0 and NO nonzero row is
     * shared by two ids. Row 0 is the DEFAULT material, so "maps to row 0" is
     * how the data says "this level does not ship this material, use the
     * default" - it is not a material sharing a behaviour. Reading those as
     * real rows makes 341 unshipped materials report the default's 30 relations
     * as their own, which is a confident wrong answer.
     *
     * So an id other than 0 that lands on row 0 is recorded as NOT SHIPPED, and
     * a caller that wants the fallback behaviour asks about material 0. */
    g.row_of.reserve(idmap->items.size());
    for (size_t id = 0; id < idmap->items.size(); ++id) {
        const bf6::EbxValue& v = idmap->items[id];
        long long row = (v.kind == bf6::EbxValue::Kind::Uint) ? (long long)v.u : (long long)v.i;
        if (row < 0 || row >= g.side) { g.row_of.push_back(-1); continue; }
        if (row == 0 && id != 0) { g.row_of.push_back(-1); g.fallback_ids++; continue; }
        g.row_of.push_back((int32_t)row);
    }

    std::map<std::string, int32_t> type_idx, inst_idx;
    size_t controls = 0;
    for (int r = 0; r < g.side; ++r) {
        const bf6::EbxValue* cols = grid->items[(size_t)r].field(kMgGridRow);
        if (!cols) continue;
        for (int cc = 0; cc < (int)cols->items.size() && cc < g.side; ++cc) {
            const bf6::EbxValue& cell = cols->items[(size_t)cc];
            const bf6::EbxValue* list = cell.field(kMgCellList);
            if (!list || list->items.empty()) continue;
            std::vector<bf6_matgrid_item> flat;
            for (const bf6::EbxValue& entry : list->items) {
                const bf6::EbxValue* arr = entry.field(kMgCellItems);
                if (!arr) continue;
                for (const bf6::EbxValue& it : arr->items) {
                    /* A neighbouring hash that does not exist must stay null, or
                     * the field read is matching anything it is handed and every
                     * relation above is suspect. */
                    if (it.field(kMgFakeField)) controls++;
                    const bf6::EbxValue* rel = it.field(kMgItemRel);
                    if (!rel || rel->kind != bf6::EbxValue::Kind::InstanceRef) continue;
                    if (rel->instance < 0 || (size_t)rel->instance >= ebx.instance_count()) continue;
                    const bf6::EbxValue* kind = it.field(kMgItemKind);
                    bf6_matgrid_item row{};
                    row.kind = kind ? (uint32_t)kind->u : 0u;

                    const std::string tg = bf6::TypeDb::guid_str(ebx.instance_type((size_t)rel->instance));
                    auto ti = type_idx.find(tg);
                    if (ti == type_idx.end()) {
                        ti = type_idx.emplace(tg, (int32_t)g.type_guids.size()).first;
                        g.type_guids.push_back(tg);
                    }
                    row.type_idx = ti->second;

                    const std::string ig = ebx.instance_guid((size_t)rel->instance);
                    auto ii = inst_idx.find(ig);
                    if (ii == inst_idx.end()) {
                        ii = inst_idx.emplace(ig, (int32_t)g.inst_guids.size()).first;
                        g.inst_guids.push_back(ig);
                    }
                    row.inst_idx = ii->second;
                    flat.push_back(row);
                }
            }
            if (flat.empty()) continue;
            g.relations += flat.size();
            g.cells.emplace(mg_cell_key(r, cc), std::move(flat));
        }
    }
    if (controls > 0) {
        e = "material grid control field resolved " + std::to_string(controls) +
            " time(s); the cell read is matching anything";
        return nullptr;
    }
    c->matgrid = std::move(g);
    return &c->matgrid;
}

const char* bf6_material_category_name(int index)
{
    if (index < 0 || index >= BF6_MATERIAL_CATEGORIES) return nullptr;
    return kMgCategories[index][0] ? kMgCategories[index] : nullptr;
}

int bf6_material_profile_get(bf6_ctx* c, const char* level, int material_id,
                             bf6_material_profile* out, char* err, int err_len)
{
    auto fail = [&](const std::string& m) {
        if (err && err_len > 0) put_str(err, (size_t)err_len, m);
        return -1;
    };
    if (!c || !level || !*level || !out) return fail("bad arguments");
    std::string e;
    const bf6_matgrid_cache* g = mg_load(c, level, e);
    if (!g) return fail(e);
    *out = bf6_material_profile{};
    if (material_id < 0 || (size_t)material_id >= g->row_of.size())
        return fail("material id out of the global id space");
    const int32_t r = g->row_of[(size_t)material_id];
    if (r < 0) return 0;   /* the level does not ship it: a real empty answer */

    for (int col = 0; col < g->side; ++col) {
        auto it = g->cells.find(mg_cell_key(r, col));
        if (it == g->cells.end()) continue;
        out->partners++;
        for (const bf6_matgrid_item& item : it->second) {
            out->relations++;
            const MgTypeInfo* ti = mg_type_info(g->type_guids[(size_t)item.type_idx]);
            out->by_category[mg_category_index(ti ? ti->category : nullptr)]++;
        }
    }
    return out->partners;
}

int bf6_material_busiest(bf6_ctx* c, const char* level,
                         bf6_material_rank* out, int out_max,
                         char* err, int err_len)
{
    auto fail = [&](const std::string& m) {
        if (err && err_len > 0) put_str(err, (size_t)err_len, m);
        return -1;
    };
    if (!c || !level || !*level) return fail("bad arguments");
    std::string e;
    const bf6_matgrid_cache* g = mg_load(c, level, e);
    if (!g) return fail(e);

    /* Per ROW, then attributed back to the id that owns it. Walking ids instead
     * would rescan the same row for every id pointing at it. */
    std::vector<bf6_material_rank> rows;
    rows.reserve(g->row_of.size());
    for (size_t id = 0; id < g->row_of.size(); ++id) {
        const int32_t r = g->row_of[id];
        if (r < 0) continue;              /* falls back to the default */
        bf6_material_rank k{};
        k.material = (int)id;
        k.row = r;
        for (int col = 0; col < g->side; ++col) {
            auto it = g->cells.find(mg_cell_key(r, col));
            if (it == g->cells.end()) continue;
            k.partners++;
            k.relations += (int)it->second.size();
        }
        rows.push_back(k);
    }
    std::sort(rows.begin(), rows.end(), [](const bf6_material_rank& a, const bf6_material_rank& b) {
        if (a.relations != b.relations) return a.relations > b.relations;
        return a.material < b.material;   /* stable between runs */
    });
    if (out) {
        const int n = (int)rows.size() < out_max ? (int)rows.size() : out_max;
        for (int i = 0; i < n; ++i) out[i] = rows[(size_t)i];
    }
    return (int)rows.size();
}

void bf6_material_unpack(unsigned int packed, bf6_surface_material* out)
{
    if (!out) return;
    *out = bf6_surface_material{};
    out->flags      = packed & 0xFFu;
    out->material_a = (int)((packed >> 8) & 0x3FFu);
    out->material_b = (int)((packed >> 20) & 0x3FFu);
    /* The reserved gaps are what make this a layout rather than a slicing of
     * arbitrary bits, so a caller is told when they are not clear instead of
     * being handed two numbers that mean nothing. */
    out->reserved_clear = (((packed >> 18) & 0x3u) == 0u && ((packed >> 30) & 0x3u) == 0u) ? 1 : 0;
}

int bf6_material_grid_info(bf6_ctx* c, const char* level,
                           bf6_material_grid_stats* out, char* err, int err_len)
{
    auto fail = [&](const std::string& m) {
        if (err && err_len > 0) put_str(err, (size_t)err_len, m);
        return -1;
    };
    if (!c || !level || !*level || !out) return fail("bad arguments");
    std::string e;
    const bf6_matgrid_cache* g = mg_load(c, level, e);
    if (!g) return fail(e);
    *out = bf6_material_grid_stats{};
    out->side           = g->side;
    out->declared_dim   = g->declared;
    out->id_map_len     = (int32_t)g->row_of.size();
    out->occupied_cells = (int32_t)g->cells.size();
    out->relations      = (int32_t)g->relations;
    out->relation_types = (int32_t)g->type_guids.size();
    out->fallback_ids   = g->fallback_ids;
    put_str(out->partition, sizeof(out->partition), g->partition);
    return 0;
}

int bf6_material_row(bf6_ctx* c, const char* level, int material_id,
                     char* err, int err_len)
{
    auto fail = [&](const std::string& m) {
        if (err && err_len > 0) put_str(err, (size_t)err_len, m);
        return -1;
    };
    if (!c || !level || !*level) return fail("bad arguments");
    std::string e;
    const bf6_matgrid_cache* g = mg_load(c, level, e);
    if (!g) return fail(e);
    /* Out of the global space, or shipped by another level: a real answer of
     * "this level does not carry that material", not an error. */
    if (material_id < 0 || (size_t)material_id >= g->row_of.size()) return -2;
    const int32_t row = g->row_of[(size_t)material_id];
    return row < 0 ? -2 : row;
}

int bf6_material_relations(bf6_ctx* c, const char* level,
                           int material_a, int material_b,
                           bf6_material_relation* out, int out_max,
                           char* err, int err_len)
{
    auto fail = [&](const std::string& m) {
        if (err && err_len > 0) put_str(err, (size_t)err_len, m);
        return -1;
    };
    if (!c || !level || !*level) return fail("bad arguments");
    std::string e;
    const bf6_matgrid_cache* g = mg_load(c, level, e);
    if (!g) return fail(e);
    if (material_a < 0 || (size_t)material_a >= g->row_of.size() ||
        material_b < 0 || (size_t)material_b >= g->row_of.size())
        return fail("material id out of the global id space (0.." +
                    std::to_string(g->row_of.size() ? g->row_of.size() - 1 : 0) + ")");
    const int32_t ra = g->row_of[(size_t)material_a];
    const int32_t rb = g->row_of[(size_t)material_b];
    /* Either material is not shipped by this level, so the pair cannot occur
     * here. Zero relations is the truthful answer. */
    if (ra < 0 || rb < 0) return 0;

    auto it = g->cells.find(mg_cell_key(ra, rb));
    if (it == g->cells.end()) return 0;
    const std::vector<bf6_matgrid_item>& items = it->second;
    if (out) {
        int n = (int)items.size();
        if (n > out_max) n = out_max;
        for (int i = 0; i < n; ++i) {
            out[i] = bf6_material_relation{};
            out[i].kind = items[(size_t)i].kind;
            const std::string& tg = g->type_guids[(size_t)items[(size_t)i].type_idx];
            put_str(out[i].type, sizeof(out[i].type), tg);
            put_str(out[i].guid, sizeof(out[i].guid), g->inst_guids[(size_t)items[(size_t)i].inst_idx]);
            const MgTypeInfo* ti = mg_type_info(tg);
            /* NEVER BLANK. A UI that groups by category drops an empty one
             * without saying so, and the one type in no corpus here would
             * vanish from the panel entirely. It goes in the same bucket the
             * profile counts it in, so the two views agree. */
            put_str(out[i].category, sizeof(out[i].category),
                    (ti && ti->category[0]) ? ti->category : kMgCategories[0]);
            put_str(out[i].name, sizeof(out[i].name), ti ? ti->name : "");
        }
    }
    return (int)items.size();
}
