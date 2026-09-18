/* ant_cdb_ext.inc - ANT's ContextDatabase lookup: "which entry, for THIS
 * weapon, stance, state". A translation-unit fragment of bf6_core.cpp.
 *
 * Every rule below is the game's, read from the analysed build and then
 * checked against the game's own machine code run under Unicorn:
 * 45,400 contexts over all 1,135 databases, 0 encoder / 0 score / 0 pick
 * mismatches (research: ant-context-database-matcher-decoded; the oracle,
 * scalar model and differential are impl/retools/cdb_*.py, and this file is a
 * port of cdb_model.py). ant_cdb_replay_test replays those same contexts
 * against THIS code.
 *
 *   values   FUN_1408a9e80 / FUN_1408a9fb0 - the caller packs game-state
 *            values at each key descriptor's offsets (see bf6_ant_cdb_key)
 *   encode   FUN_14088fa30 - the group's u8 array partitions the descriptors
 *            into nine passes; descriptor class c runs in pass c+1
 *   match    FUN_14089a730 pass 0 - compare methods in order over context
 *            SLOTS (bounded by the region's per-method slot prefix) and ROWS
 *            (a separate counter: BOOL_EQUAL spends two rows on one slot)
 *   pick     FUN_1408aab60 - highest score, earliest entry wins
 *
 * Only pass 0 exists here because the +0x58 multi-pass flag is clear in all
 * 1,135 databases the install ships; a database that sets it is refused
 * rather than half-evaluated. Likewise only the nine compare methods the game
 * uses are implemented, and an unknown one refuses the whole database.
 */

#include <array>

namespace {

/* Field hashes: ContextDatabase (aa2343e0) and its structs, from the
 * executable's reflection (field_type_probe). */
constexpr uint32_t kCdb_Entries   = 0x1a054761u;  /* +0x30 AntAsset[] */
constexpr uint32_t kCdb_Groups    = 0x2b123790u;  /* +0x40 */
constexpr uint32_t kCdb_Keys      = 0x03f14c46u;  /* +0x48 48-byte descriptors */
constexpr uint32_t kCdb_Multi     = 0xf834e6ecu;  /* +0x58 struct, first field the multi-pass flag */
constexpr uint32_t kCdb_Multi_On  = 0xc85c7364u;
constexpr uint32_t kCdb_Regions   = 0xadee1c58u;  /* +0x78 */
constexpr uint32_t kCdb_Rows      = 0x9f6586cdu;  /* +0x80 Vec4[] */
constexpr uint32_t kCdb_Seed      = 0xcefcf711u;  /* +0x88 u8[] */
constexpr uint32_t kCdb_KeyNames  = 0xdb58cdd1u;  /* +0x98 CString[] */
constexpr uint32_t kCdb_NEntries  = 0x77e41430u;  /* +0xA8 */
constexpr uint32_t kCdb_NRows     = 0x33b68e77u;  /* +0xAC rows per 16-entry block */
constexpr uint32_t kCdb_NSlots    = 0x8cdd2d61u;  /* +0xB0 */
constexpr uint32_t kGrp_Hash      = 0x58ac9cecu;
constexpr uint32_t kGrp_Pass      = 0x0424c3f9u;  /* u8[10] */
constexpr uint32_t kGrp_SkipLo    = 0xe58491f6u;
constexpr uint32_t kGrp_SkipHi    = 0x16e8e080u;
constexpr uint32_t kReg_Prefix    = 0x9ecf87d8u;  /* i32[33] */
/* ContextDatabaseAsset (6fa5a6cc) */
constexpr uint32_t kAsset_Rows    = 0x0cd0a1f8u;  /* Struct_0cd0a1f8[] */
constexpr uint32_t kAsset_Mirror  = 0x5a8e5a87u;  /* +0x48 mirror game state */
constexpr uint32_t kRow_State     = 0xf365b7f1u;
constexpr uint32_t kRow_Mirror    = 0x76af5b45u;
constexpr uint32_t kRow_Index     = 0xca9a686eu;
constexpr uint32_t kRow_Context   = 0x23f3ecb1u;
constexpr uint32_t kRow_Bitflag   = 0xc948e596u;
constexpr uint32_t kGroupHash     = 0x15A9AE49u;  /* the one group, 1135 of 1135 */

/* Engine constants, all .rdata in the analysed build. */
constexpr float kEps        = 1.52587890625e-05f;   /* 0x37800000 */
constexpr float kRangeScale = 253.0f;               /* 0x437d0000 */
constexpr float kRangeBias  = 1.4900000095367432f;  /* 0x3fbeb852 */
constexpr float kRankWidth  = 0.99609375f;          /* 0x3f7f0000 */
constexpr float kRankScale  = 255.0f;               /* 0x437f0000 */
constexpr float kRankWScale = 128.0f;               /* 0x43000000 */
constexpr float kRankBias   = 0.49000000953674316f; /* 0x3efae148 */

struct AntCdbHandle {
    bf6_ant_cdb_info info{};
    std::vector<uint8_t>  seed;            /* per slot */
    uint8_t               pass[10]{};       /* encoder pass prefix */
    std::vector<bf6_ant_cdb_key> keys;
    std::vector<std::array<uint8_t, 16>> rows;  /* blocks * rows_per_block */
    std::vector<int32_t>  prefix;           /* per compare method, SLOT boundary */
    std::vector<std::string> entries;       /* entry asset paths */
    std::vector<std::string> key_names;
    struct AssetRow { std::string state; std::string mirror; int descriptor; int context; bool bitflag; };
    std::vector<AssetRow> asset_rows;
    std::string mirror_state;
    int skip_lo = 0, skip_hi = 0;
    std::string name;
};

inline uint8_t cdb_i8(float x)
{
    /* (char)(int)x: truncating float->int, then the low byte. */
    return (uint8_t)((int32_t)x & 0xFF);
}

inline uint8_t cdb_quant_range(float v, float lo, float hi)
{
    if (hi < v) return 0xFF;
    if (v < lo) return 0x00;
    const float span = hi - lo;
    float t = (std::fabs(span) < kEps) ? 0.0f : (v - lo) / span;
    if (!(t > 0.0f)) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return cdb_i8(t * kRangeScale + kRangeBias);
}

inline uint32_t cdb_rd32(const uint8_t* v, int n, uint64_t off)
{
    if (off + 4 > (uint64_t)n) return 0;
    uint32_t x; std::memcpy(&x, v + off, 4); return x;
}
inline float cdb_rdf(const uint8_t* v, int n, uint64_t off)
{
    if (off + 4 > (uint64_t)n) return 0.0f;
    float x; std::memcpy(&x, v + off, 4); return x;
}

/* FUN_14088fa30: game-state values -> one byte per context slot (the engine
 * broadcasts each to 16 lanes; every lane of a slot is equal). */
bool cdb_encode(const AntCdbHandle& h, const uint8_t* values, int n, std::vector<uint8_t>& ctx)
{
    const int slots = h.info.slots;
    ctx.assign((size_t)slots, 0);
    for (int i = 0; i < slots && i < (int)h.seed.size(); ++i) ctx[(size_t)i] = h.seed[(size_t)i];
    auto put = [&](int64_t s, uint8_t b) -> bool {
        if (s < 0 || s >= slots) return false;
        ctx[(size_t)s] = b; return true;
    };
    int k = h.pass[0];
    /* pass 1: bool bits */
    for (; k < h.pass[1]; ++k) {
        const bf6_ant_cdb_key& d = h.keys[(size_t)k];
        const uint8_t mask = (uint8_t)(d.second & 0xFF);
        const bool on = d.value_offset < (uint64_t)n && values[d.value_offset] != 0;
        if (d.slot < 0 || d.slot >= slots) return false;
        if (on) ctx[(size_t)d.slot] |= mask; else ctx[(size_t)d.slot] &= (uint8_t)~mask;
    }
    /* passes 2 and 3: little-endian integer bytes, each followed by its valid mask */
    for (int p = 2; p <= 3; ++p)
        for (; k < h.pass[p]; ++k) {
            const bf6_ant_cdb_key& d = h.keys[(size_t)k];
            const int w = (int)d.second;
            const uint32_t v = cdb_rd32(values, n, d.value_offset);
            bool unset;
            switch (w) {
            case 1: unset = (v & 0xFFu) == 0xFFu; break;
            case 2: unset = (v & 0xFFFFu) == 0xFFFFu; break;
            case 3: unset = (v & 0xFFFFFFu) == 0xFFFFFFu; break;
            case 4: unset = v == 0xFFFFFFFFu; break;
            default: unset = true; break;
            }
            const uint8_t valid = unset ? 0x00 : 0xFF;
            for (int i = 0; i < w; ++i)
                if (!put(d.slot + 2 * i, (uint8_t)(v >> (8 * i))) ||
                    !put(d.slot + 2 * i + 1, valid)) return false;
        }
    /* pass 4: raw bitmap bytes */
    for (; k < h.pass[4]; ++k) {
        const bf6_ant_cdb_key& d = h.keys[(size_t)k];
        for (int i = 0; i < (int)d.second; ++i) {
            const uint64_t o = d.value_offset + (uint64_t)i;
            if (!put(d.slot + i, o < (uint64_t)n ? values[o] : 0)) return false;
        }
    }
    /* passes 5..8: float windows. 5 and 7 are value pairs; 6 and 8 value+weight */
    for (int p = 5; p <= 8; ++p)
        for (; k < h.pass[p]; ++k) {
            const bf6_ant_cdb_key& d = h.keys[(size_t)k];
            if (p == 5 || p == 7) {
                const float a = cdb_rdf(values, n, d.value_offset);
                const float b = cdb_rdf(values, n, d.second);
                if (!put(d.slot, cdb_quant_range(a, d.lo, d.hi)) ||
                    !put(d.slot + 1, cdb_quant_range(b, d.lo, d.hi))) return false;
            } else {
                float w = d.second == 0xFFFFFFFFull ? 0.0f : cdb_rdf(values, n, d.second);
                const float span = (d.hi - d.lo) * kRankWidth;
                float t = std::fabs(span) < kEps ? 0.0f
                        : (cdb_rdf(values, n, d.value_offset) - d.lo) / span;
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                w = w < 0.0f ? 0.0f : (w > span ? span : w);
                if (!put(d.slot, cdb_i8(t * kRankScale + kRankBias)) ||
                    !put(d.slot + 1, cdb_i8((w / span) * kRankWScale + kRankBias))) return false;
            }
        }
    /* pass 9 (vector angle) is not used by any shipped database; refused at open. */
    return true;
}

/* FUN_14089a730 pass 0, per entry. Filters AND a 0/0xFF mask; RANKED_HALF
 * subtracts half the interval gap from what survives. */
bool cdb_score(const AntCdbHandle& h, const std::vector<uint8_t>& ctx, uint8_t* scores)
{
    const int n = h.info.entries, rows_n = h.info.rows_per_block;
    for (int e = 0; e < n; ++e) {
        const int blk = e / 16, lane = e % 16;
        auto R = [&](int r) -> uint8_t {
            const size_t at = (size_t)blk * (size_t)rows_n + (size_t)r;
            return at < h.rows.size() ? h.rows[at][(size_t)lane] : 0;
        };
        auto C = [&](int s) -> uint8_t { return s < (int)ctx.size() ? ctx[(size_t)s] : 0; };
        bool alive = true;
        int penalty = 0, s = 0, r = 0;
        for (int m = 0; m < 32; ++m) {
            const int end = m < (int)h.prefix.size() ? h.prefix[(size_t)m] : 0;
            while (s < end) {
                switch (m) {
                case 0: case 1: case 2: case 3: {                    /* BITMAP_8..32 */
                    const int w = m + 1;
                    bool any = false;
                    for (int i = 0; i < w; ++i) if (R(r + i) & C(s + i)) any = true;
                    if (!any) alive = false;
                    s += w; r += w;
                    break;
                }
                case 10: case 12: {                                  /* RANGE / RANKED_HALF */
                    const int a = C(s), b = C(s + 1);
                    const int clo = a < b ? a : b, chi = a < b ? b : a;
                    const int elo = R(r), ehi = R(r + 1);
                    if (m == 10) {
                        if (!((clo > elo ? clo : elo) <= (chi < ehi ? chi : ehi))) alive = false;
                    } else {
                        const int gap = chi < elo ? elo - chi : (ehi < clo ? clo - ehi : 0);
                        penalty += gap / 2;
                    }
                    s += 2; r += 2;
                    break;
                }
                case 20:                                             /* BOOL_EQUAL */
                    if (R(r + 1) & (R(r) ^ C(s))) alive = false;
                    s += 1; r += 2;
                    break;
                case 21:                                             /* ENUM_EQUAL_8 */
                    if (R(r + 1) & C(s + 1) & (R(r) ^ C(s))) alive = false;
                    s += 2; r += 2;
                    break;
                case 22: {                                           /* ENUM_EQUAL_16 */
                    const uint8_t care = R(r + 1) & C(s + 1);
                    if ((care & (R(r) ^ C(s))) || (care & (R(r + 2) ^ C(s + 2)))) alive = false;
                    s += 4; r += 4;
                    break;
                }
                default:
                    return false;
                }
            }
        }
        const int sc = 255 - penalty;
        scores[e] = alive ? (uint8_t)(sc < 0 ? 0 : sc) : 0;
    }
    return true;
}

int cdb_pick(const uint8_t* scores, int n)
{
    int best = -1, idx = -10000000;
    for (int i = 0; i < n; ++i)
        if ((int)scores[i] > best) { best = scores[i]; idx = i; }
    return idx;
}

}  // namespace

void bf6__antcdb_delete(void* p) { delete (AntCdbHandle*)p; }

bf6_ant_cdb* bf6_ant_cdb_open(bf6_ctx* c, const char* name, char* err, int err_len)
{
    auto fail = [&](const std::string& why) -> bf6_ant_cdb* {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", why.c_str());
        return nullptr;
    };
    if (!c || !name) return fail("no context or name");
    std::string terr;
    if (!ensure_types(c, terr)) return fail("no type schema: " + terr);
    std::string e2;
    std::vector<uint8_t> raw = c->src.get_ebx(std::string(name) + ".ebx", e2);
    if (raw.empty()) raw = c->src.get_ebx(name, e2);
    if (raw.empty()) return fail(std::string("not in the mount: ") + name);

    bf6::Ebx e(*c->types);
    e.set_guid_index(&c->src.partition_index());
    if (!e.parse(std::move(raw), e2)) return fail("parse failed: " + e2);

    auto h = std::make_unique<AntCdbHandle>();
    h->name = name;
    bool have_db = false;
    auto I = [](const bf6::EbxValue* x) -> int64_t {
        if (!x) return 0;
        return x->kind == bf6::EbxValue::Kind::Int ? x->i : (int64_t)x->u;
    };
    auto path_of = [](const bf6::EbxValue& v) -> std::string {
        if (v.kind != bf6::EbxValue::Kind::ImportRef) return std::string();
        if (v.import_path.empty() || v.import_path == "<not indexed>") return std::string();
        std::string p = v.import_path;
        if (p.size() > 4 && p.compare(p.size() - 4, 4, ".ebx") == 0) p.resize(p.size() - 4);
        return p;
    };
    const std::vector<uint8_t>& bytes = e.raw();
    for (size_t i = 0; i < e.instance_count(); ++i) {
        const bf6::TypeGuid tg = e.instance_type(i);
        const std::string g = bf6::TypeDb::guid_str(tg);
        if (g == "6fa5a6cc-ab97-ef80-9a98-59b5bd2edbda") {            /* ContextDatabaseAsset */
            bf6::EbxValue v = e.read_instance(i);
            if (const bf6::EbxValue* m = v.field(kAsset_Mirror)) h->mirror_state = path_of(*m);
            if (const bf6::EbxValue* rows = v.field(kAsset_Rows))
                for (const auto& r : rows->items) {
                    AntCdbHandle::AssetRow ar;
                    if (const bf6::EbxValue* s = r.field(kRow_State)) ar.state = path_of(*s);
                    if (const bf6::EbxValue* s = r.field(kRow_Mirror)) ar.mirror = path_of(*s);
                    ar.descriptor = (int)I(r.field(kRow_Index));
                    ar.context = (int)I(r.field(kRow_Context));
                    const bf6::EbxValue* bf = r.field(kRow_Bitflag);
                    ar.bitflag = bf && bf->b;
                    h->asset_rows.push_back(std::move(ar));
                }
        } else if (g == "aa2343e0-3bd2-7f14-cd07-54ccd188c0a7") {     /* ContextDatabase */
            have_db = true;
            bf6::EbxValue v = e.read_instance(i);
            if (const bf6::EbxValue* mp = v.field(kCdb_Multi))
                if (const bf6::EbxValue* on = mp->field(kCdb_Multi_On))
                    if (on->b) return fail("multi-pass ContextDatabase (+0x58 set): no shipped database uses it");
            h->info.entries = (int32_t)I(v.field(kCdb_NEntries));
            h->info.rows_per_block = (int32_t)I(v.field(kCdb_NRows));
            h->info.slots = (int32_t)I(v.field(kCdb_NSlots));
            if (const bf6::EbxValue* s = v.field(kCdb_Seed))
                for (const auto& it : s->items) h->seed.push_back((uint8_t)it.u);
            if (const bf6::EbxValue* en = v.field(kCdb_Entries))
                for (const auto& it : en->items) h->entries.push_back(path_of(it));
            if (const bf6::EbxValue* kn = v.field(kCdb_KeyNames))
                for (const auto& it : kn->items) h->key_names.push_back(it.s);
            /* the one group */
            bool have_group = false;
            if (const bf6::EbxValue* gs = v.field(kCdb_Groups))
                for (const auto& grp : gs->items) {
                    if ((uint32_t)I(grp.field(kGrp_Hash)) != kGroupHash) continue;
                    have_group = true;
                    const bf6::EbxValue* pb = grp.field(kGrp_Pass);
                    if (!pb || pb->items.size() < 10) return fail("encoder pass table short");
                    for (int k = 0; k < 10; ++k) h->pass[k] = (uint8_t)pb->items[(size_t)k].u;
                    h->skip_lo = (int)I(grp.field(kGrp_SkipLo));
                    h->skip_hi = (int)I(grp.field(kGrp_SkipHi));
                }
            if (!have_group) return fail("no group 0x15A9AE49");
            /* key descriptors: their own 48 bytes each, as the encoder reads them */
            if (const bf6::EbxValue* ks = v.field(kCdb_Keys))
                for (const auto& kd : ks->items) {
                    if (kd.source_pos < 0 || (size_t)kd.source_pos + 48 > bytes.size())
                        return fail("key descriptor has no source bytes");
                    const uint8_t* p = bytes.data() + kd.source_pos;
                    bf6_ant_cdb_key k{};
                    std::memcpy(&k.key_class, p + 0, 4);
                    std::memcpy(&k.key_index, p + 4, 4);
                    std::memcpy(&k.slot, p + 8, 4);
                    std::memcpy(&k.first_row, p + 12, 4);
                    std::memcpy(&k.value_offset, p + 16, 8);
                    std::memcpy(&k.second, p + 24, 8);
                    std::memcpy(&k.lo, p + 40, 4);
                    std::memcpy(&k.hi, p + 44, 4);
                    if (k.key_class < 0 || k.key_class > 7)
                        return fail("key class outside the eight the encoder handles");
                    h->keys.push_back(k);
                }
            if (h->pass[9] > h->keys.size() || h->pass[8] != h->pass[9])
                return fail("vector-angle keys (encoder pass 9): no shipped database uses them");
            if (const bf6::EbxValue* rs = v.field(kCdb_Regions)) {
                if (rs->items.size() != 1) return fail("expected exactly one region");
                if (const bf6::EbxValue* pre = rs->items[0].field(kReg_Prefix))
                    for (const auto& it : pre->items) h->prefix.push_back((int32_t)I(&it));
            }
            for (size_t m = 0; m < h->prefix.size() && m < 32; ++m) {
                const int32_t prev = m ? h->prefix[m - 1] : 0;
                const bool used = h->prefix[m] > prev;
                const bool known = m <= 3 || m == 10 || m == 12 || (m >= 20 && m <= 22);
                /* A method is USED when its slot boundary advances. Prefix
                 * entries of unused methods repeat the previous value. */
                if (used && !known) return fail("compare method " + std::to_string(m) +
                                                " not implemented (no shipped database uses it)");
            }
            if (const bf6::EbxValue* rows = v.field(kCdb_Rows))
                for (const auto& r : rows->items) {
                    if (r.source_pos < 0 || (size_t)r.source_pos + 16 > bytes.size())
                        return fail("match row has no source bytes");
                    std::array<uint8_t, 16> a{};
                    std::memcpy(a.data(), bytes.data() + r.source_pos, 16);
                    h->rows.push_back(a);
                }
        }
    }
    if (!have_db) return fail("no ContextDatabase instance in the partition");
    h->info.keys = (int32_t)h->keys.size();
    h->info.asset_keys = (int32_t)h->asset_rows.size();
    /* FUN_14088afe0: the packed values buffer's size */
    int64_t need = 0;
    for (int k = h->pass[0]; k < h->pass[9] && k < (int)h->keys.size(); ++k) {
        const bf6_ant_cdb_key& d = h->keys[(size_t)k];
        int64_t end = 0;
        switch (d.key_class) {
        case 0: case 1: case 2: end = (int64_t)d.value_offset; break;
        case 3: end = (int64_t)d.value_offset + 12; break;
        case 4: case 6: end = (int64_t)std::max(d.value_offset, d.second); break;
        case 5: case 7: end = d.second != 0xFFFFFFFFull
                            ? (int64_t)std::max(d.value_offset, d.second) : (int64_t)d.value_offset; break;
        }
        need = std::max(need, end);
    }
    h->info.value_bytes = (int32_t)(need + 4);
    h->info.has_mirror = h->mirror_state.empty() ? 0 : 1;

    AntCdbHandle* out = h.release();
    c->handles[out] = bf6_ctx::HK_ANTCDB;
    return reinterpret_cast<bf6_ant_cdb*>(out);
}

int bf6_ant_cdb_get_info(const bf6_ant_cdb* h, bf6_ant_cdb_info* out)
{
    if (!h || !out) return 0;
    *out = reinterpret_cast<const AntCdbHandle*>(h)->info;
    return 1;
}

const char* bf6_ant_cdb_entry(const bf6_ant_cdb* h, int i)
{
    const AntCdbHandle* x = reinterpret_cast<const AntCdbHandle*>(h);
    if (!x || i < 0 || i >= (int)x->entries.size()) return nullptr;
    return x->entries[(size_t)i].c_str();
}

int bf6_ant_cdb_key_at(const bf6_ant_cdb* h, int i, bf6_ant_cdb_key* out)
{
    const AntCdbHandle* x = reinterpret_cast<const AntCdbHandle*>(h);
    if (!x || !out || i < 0 || i >= (int)x->keys.size()) return 0;
    *out = x->keys[(size_t)i];
    return 1;
}

const char* bf6_ant_cdb_asset_key(const bf6_ant_cdb* h, int i, int* descriptor, int* flags)
{
    const AntCdbHandle* x = reinterpret_cast<const AntCdbHandle*>(h);
    if (!x || i < 0 || i >= (int)x->asset_rows.size()) return nullptr;
    const auto& r = x->asset_rows[(size_t)i];
    if (descriptor) *descriptor = r.descriptor;
    if (flags) {
        int f = 0;
        if (r.bitflag) f |= BF6_ANT_CDB_ROW_BITFLAG;
        /* FUN_14088c7a0: rows whose descriptor index is claimed by the
         * group's [lo,hi) descriptor range are filled elsewhere, not here. */
        for (int k = x->skip_lo; k < x->skip_hi && k < (int)x->keys.size(); ++k)
            if (x->keys[(size_t)k].key_index == i) f |= BF6_ANT_CDB_ROW_SKIPPED;
        if (!r.mirror.empty()) f |= BF6_ANT_CDB_ROW_MIRRORABLE;
        *flags = f;
    }
    return r.state.c_str();
}

int bf6_ant_cdb_match(const bf6_ant_cdb* h, const uint8_t* values, int values_len,
                      uint8_t* ctx_out, int ctx_max, uint8_t* scores_out, int scores_max)
{
    const AntCdbHandle* x = reinterpret_cast<const AntCdbHandle*>(h);
    if (!x || (!values && values_len > 0)) return BF6_ANT_CDB_ERROR;
    std::vector<uint8_t> ctx;
    if (!cdb_encode(*x, values, values_len, ctx)) return BF6_ANT_CDB_ERROR;
    std::vector<uint8_t> sc((size_t)std::max(0, x->info.entries), 0);
    if (!cdb_score(*x, ctx, sc.data())) return BF6_ANT_CDB_ERROR;
    if (ctx_out) std::memcpy(ctx_out, ctx.data(), (size_t)std::min<int>(ctx_max, (int)ctx.size()));
    if (scores_out) std::memcpy(scores_out, sc.data(), (size_t)std::min<int>(scores_max, (int)sc.size()));
    return cdb_pick(sc.data(), (int)sc.size());
}
