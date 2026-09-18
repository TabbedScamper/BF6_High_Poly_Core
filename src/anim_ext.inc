/* anim_ext.inc - AnimationAssetRelocResource, the container that holds 99.9%
 * of BF6's animation.
 *
 * A translation-unit fragment like the others.
 *
 * WHY THIS AND NOT THE OTHER TWO. The raw (0x185E69A4) and ACL (0x1190E82E)
 * clip containers are fully documented and between them hold a few hundred
 * payloads. This type holds 486,942, and no clip-player node in the game
 * reaches anything else. A consumer that decodes the other two reaches almost
 * no animation at all, which is why this is the one worth implementing.
 *
 * THE FRAMING RULE, which is the whole trick: region 0's OFFSET is the header
 * size, and the header holds as many 12-byte {count, offset, flags}
 * descriptors as fit, with the remainder as trailing dwords. That single rule
 * covers all three framings:
 *
 *     offset 32 -> 2 descriptors + 2 dwords   'DCT ' , 6,244 payloads
 *     offset 48 -> 4 descriptors + 0          'VBR ' ,   254
 *     offset 64 -> 5 descriptors + 1          'RAW ' , 1,697
 *
 * A two-descriptor parser reads a RAW payload as if it had two regions and
 * silently truncates it - the structure still "parses", which is the failure
 * mode this comment exists to prevent.
 */

typedef struct { uint32_t count, offset, flags; } bf6__anim_desc;

struct AnimRelocHandle {
    bf6_anim_reloc            r{};
    std::vector<uint8_t>      bytes;
    std::vector<bf6_anim_region> regions;
};

bf6_anim_reloc* bf6_anim_reloc_read(bf6_ctx* c, const char* res_name)
{
    if (!c || !res_name) return nullptr;
    const uint8_t* d = nullptr;
    const int64_t n = bf6_read_raw(c, BF6_RAW_RES, res_name, &d);
    if (n < 12 || !d) return nullptr;

    auto rd32 = [&](size_t o) { uint32_t v; std::memcpy(&v, d + o, 4); return v; };

    /* Region 0's offset IS the header size. Anything that is not 12n + 4m is
     * not this container - 14 payloads out of 8,209 are rejected this way
     * rather than being forced into a framing they do not have. */
    const uint32_t hdr = rd32(4);
    if (hdr < 12 || hdr > 256 || (size_t)hdr > (size_t)n) return nullptr;
    const uint32_t ndesc = hdr / 12u;
    const uint32_t trail = (hdr - ndesc * 12u);
    if (trail % 4u != 0u || ndesc == 0u) return nullptr;

    AnimRelocHandle* h = new AnimRelocHandle();
    h->bytes.assign(d, d + n);
    const uint8_t* b = h->bytes.data();
    auto r32 = [&](size_t o) { uint32_t v; std::memcpy(&v, b + o, 4); return v; };

    for (uint32_t i = 0; i < ndesc; i++)
    {
        bf6_anim_region g{};
        g.count  = r32((size_t)i * 12 + 0);
        g.offset = r32((size_t)i * 12 + 4);
        g.flags  = r32((size_t)i * 12 + 8);
        /* A TRAILING {0,0} DESCRIPTOR IS A TERMINATOR, NOT A REGION. The
         * 48-byte framing ends with one, and treating its zero offset as a real
         * region makes an otherwise correct chain look broken. */
        if (i > 0 && g.count == 0 && g.offset == 0) break;
        h->regions.push_back(g);
    }

    h->r.framing = (hdr == 32) ? BF6_ANIM_DCT
                 : (hdr == 48) ? BF6_ANIM_VBR
                 : (hdr == 64) ? BF6_ANIM_RAW : BF6_ANIM_UNKNOWN;
    h->r.header_size  = (int32_t)hdr;
    h->r.regions      = h->regions.data();
    h->r.region_count = (int32_t)h->regions.size();
    h->r.data         = h->bytes.data();
    h->r.size         = n;
    c->handles[&h->r] = bf6_ctx::HK_ANIMRELOC;
    return &h->r;
}

void bf6__animreloc_delete(void* p) { delete (AnimRelocHandle*)p; }

/* ------------------------------------------------- the DCT clip codec ----- */

/* Variant A: region 1 is the engine's DctAnimationAsset body. Eight samples
 * share one coefficient block, stored as quantised DCT rows.
 *
 * THE THREE THINGS THAT SILENTLY GO WRONG, all of which still "parse":
 *
 * 1. The descriptor run is only walkable WITH THE FLAGS IN HAND - descriptor i
 *    is 8 + 2*(flags[i] >> 4) bytes. A fixed stride desynchronises the run and
 *    every later channel reads another channel's base coefficients.
 * 2. ALL FOUR LANES are in the width accounting for EVERY channel kind. It is
 *    tempting to budget three lanes for a vector3 or one for a scalar group;
 *    doing so shifts the whole bitstream and still yields plausible numbers.
 * 3. The bitstream is LSB-FIRST. An MSB-first reader produces normalised
 *    four-vectors too, so within-frame checks stay green.
 *
 * The size identity is what catches 1 and 2: predicted stored bytes must equal
 * the bytes actually present, and it is a function of every lane width in every
 * descriptor, so one misresolved nibble moves the total.
 */

struct AnimDctChannel {
    int16_t  base[4];
    uint8_t  rows;                 /* N = flag >> 4 */
    std::vector<uint16_t> width;   /* N width words */
};

struct AnimVbrMetadata {
    float quat_min = 0.f, trajectory_min = 0.f, vec3_min = 0.f, float_min = 0.f;
    float quat_max = 0.f, trajectory_max = 0.f, vec3_max = 0.f, float_max = 0.f;
    float vector_offset_scale = 0.f, float_offset_scale = 0.f, dct = 0.f;
    uint16_t quat_count = 0, vec3_count = 0, float_count = 0;
    uint16_t const_quat_count = 0, const_vec3_count = 0, const_float_count = 0;
    uint16_t key_time_size = 0, num_keys = 0, const_channel_map_size = 0;
    uint16_t const_palette_size = 0, vector_offset_size = 0, float_offset_size = 0;
    uint16_t flags = 0;
    int animated_dofs() const {
        return (int)quat_count * 4 + (int)vec3_count * 3 + (int)float_count;
    }
    int constant_dofs() const {
        return (int)const_quat_count * 4 + (int)const_vec3_count * 3 +
               (int)const_float_count;
    }
    int channels() const {
        return (int)quat_count + vec3_count + float_count + const_quat_count +
               const_vec3_count + const_float_count;
    }
};

struct AnimVbrCurve {
    int scalar_dof = 0;
    std::vector<int> frames;
    std::vector<float> values;
};

struct AnimClipHandle {
    bf6_anim_clip               c{};
    std::vector<uint8_t>        bytes;
    std::vector<AnimDctChannel> ch;
    std::vector<uint16_t>       key_times;
    size_t                      bits_off = 0;   /* into bytes */
    size_t                      bits_len = 0;
    uint32_t                    full_block_bits = 0;
    uint32_t                    first_coef_bits = 0;
    uint16_t                    block_quant = 1;
    uint8_t                     sub_quant_tenths = 0;
    uint8_t                     coef_bit_width = 0;
    bool                        is_vbr = false;
    /* RAW single-frame pose: every channel constant, one float4 each, taken
     * straight from the ConstData region. No bitstream, no blocks, no decode. */
    bool                        is_raw_pose = false;
    /* An animated RAW clip: frames * raw_stride float4 records, frame-major,
     * with the stride VERIFIED by column purity rather than derived. */
    bool                        is_raw_anim = false;
    uint32_t                    raw_stride  = 0;
    std::vector<float>          raw_values;       /* frames * raw_stride * 4 */
    /* The channel -> slot permutation (region 2's content, rebuilt by
     * inverting region 1). A slot below raw_stride is a column of Data; one at
     * or above it is a record of raw_const. Empty means the clip exposes its
     * animated run alone, the way it did before the permutation was read. */
    std::vector<uint16_t>       raw_slot;
    std::vector<float>          raw_const;        /* ConstData, 4 per record */
    AnimVbrMetadata             vbr{};
    std::vector<float>          vbr_palette;
    std::vector<uint16_t>       vbr_block_sizes;
    std::vector<uint8_t>        vbr_widths;       /* scalar dof * 8 */
    std::vector<uint8_t>        vbr_constant;     /* symbolic channel order */
    std::vector<size_t>         vbr_block_offsets;/* one per block + end */
    std::vector<AnimVbrCurve>   vbr_vector_offsets, vbr_float_offsets;
    size_t                      vbr_constant_selector = 0;
    int                         vbr_constant_selector_size = 1;
};

/* LSB-FIRST: bit (pos & 7) of byte (pos >> 3) is result bit 0. */
static inline int32_t bf6__rdbits(const uint8_t* p, size_t len, uint64_t& pos, int w)
{
    if (w <= 0) return 0;
    uint32_t v = 0;
    for (int i = 0; i < w; i++) {
        const size_t byte = (size_t)(pos >> 3);
        const uint32_t bit = (byte < len) ? ((uint32_t)(p[byte] >> (pos & 7)) & 1u) : 0u;
        v |= bit << i;
        pos++;
    }
    if (w < 32 && (v & (1u << (w - 1)))) v |= ~((1u << w) - 1u);   /* sign extend */
    return (int32_t)v;
}

static bool bf6__anim_number(const bf6::EbxValue* value, double& out)
{
    if (!value) return false;
    switch (value->kind) {
    case bf6::EbxValue::Kind::Real: out = value->f; return true;
    case bf6::EbxValue::Kind::Int: out = (double)value->i; return true;
    case bf6::EbxValue::Kind::Uint:
    case bf6::EbxValue::Kind::Unknown:
    case bf6::EbxValue::Kind::ResRef: out = (double)value->u; return true;
    default: return false;
    }
}

static bool bf6__anim_vbr_metadata(bf6_ctx* c, const char* asset,
                                    AnimVbrMetadata& out)
{
    std::string type_error;
    if (!c || !asset || !ensure_types(c, type_error)) return false;
    std::string error;
    std::vector<uint8_t> raw = c->src.get_ebx(std::string(asset) + ".ebx", error);
    if (raw.empty()) raw = c->src.get_ebx(asset, error);
    if (raw.empty()) return false;
    bf6::Ebx ebx(*c->types);
    ebx.set_guid_index(&c->src.armory_partition_index());
    if (!ebx.parse(std::move(raw), error)) return false;

    static constexpr uint32_t kCodec = 0x8D0B6DC1u;
    static constexpr uint32_t kTag = 0x56425220u;
    struct Field { uint32_t hash; bool is_float; void* destination; };
    for (size_t i = 0; i < ebx.instance_count(); ++i)
    {
        const bf6::EbxValue row = ebx.read_instance(i);
        double codec = 0.0;
        if (!bf6__anim_number(row.field(kCodec), codec) ||
            (uint32_t)codec != kTag) continue;
        const Field fields[] = {
            {0x15F6CA30u, true, &out.quat_min},
            {0xFBA85648u, true, &out.trajectory_min},
            {0x24BB0420u, true, &out.vec3_min},
            {0x1FD46348u, true, &out.float_min},
            {0x7761EAE7u, true, &out.quat_max},
            {0xD89BBC9Cu, true, &out.trajectory_max},
            {0x79332024u, true, &out.vec3_max},
            {0x31B5B4FCu, true, &out.float_max},
            {0x41FFAF8Cu, true, &out.vector_offset_scale},
            {0xEDA566C4u, true, &out.float_offset_scale},
            {0x5A72B04Fu, true, &out.dct},
            {0x95A0D065u, false, &out.quat_count},
            {0x7D3FB5B1u, false, &out.vec3_count},
            {0x14B9E56Bu, false, &out.float_count},
            {0xB58F4E14u, false, &out.const_quat_count},
            {0x772F9B7Au, false, &out.const_vec3_count},
            {0xB3470F1Eu, false, &out.const_float_count},
            {0x0906095Fu, false, &out.key_time_size},
            {0xBC6393CFu, false, &out.num_keys},
            {0x76FAE186u, false, &out.const_channel_map_size},
            {0x2C7C3CBEu, false, &out.const_palette_size},
            {0x84BEE21Au, false, &out.vector_offset_size},
            {0x5F8DEFF1u, false, &out.float_offset_size},
            {0x5645E663u, false, &out.flags},
        };
        for (const Field& field : fields)
        {
            double value = 0.0;
            if (!bf6__anim_number(row.field(field.hash), value)) return false;
            if (field.is_float) *static_cast<float*>(field.destination) = (float)value;
            else {
                if (value < 0.0 || value > 65535.0) return false;
                *static_cast<uint16_t*>(field.destination) = (uint16_t)value;
            }
        }
        return out.num_keys > 0 && out.channels() > 0 && out.animated_dofs() > 0;
    }
    return false;
}

static bool bf6__anim_vbr_channel_map(AnimClipHandle& h, size_t at)
{
    const AnimVbrMetadata& m = h.vbr;
    h.vbr_constant.assign((size_t)m.channels(), 0);
    const int quat_end = (int)m.quat_count + m.const_quat_count;
    const int vec_end = quat_end + (int)m.vec3_count + m.const_vec3_count;
    int output = 0, aq = 0, cq = 0, av = 0, cv = 0, af = 0, cf = 0;
    if (!m.const_channel_map_size)
    {
        output += m.quat_count;
        for (int i = 0; i < m.const_quat_count; ++i) h.vbr_constant[(size_t)output++] = 1;
        output += m.vec3_count;
        for (int i = 0; i < m.const_vec3_count; ++i) h.vbr_constant[(size_t)output++] = 1;
        output += m.float_count;
        for (int i = 0; i < m.const_float_count; ++i) h.vbr_constant[(size_t)output++] = 1;
        return output == m.channels();
    }
    if (at + m.const_channel_map_size > h.bytes.size()) return false;
    for (int run = 0; run < m.const_channel_map_size; ++run)
    {
        const int length = h.bytes[at++];
        const bool constant = (run & 1) != 0;
        if (output + length > m.channels()) return false;
        for (int i = 0; i < length; ++i, ++output)
        {
            h.vbr_constant[(size_t)output] = constant ? 1u : 0u;
            if (output < quat_end) constant ? ++cq : ++aq;
            else if (output < vec_end) constant ? ++cv : ++av;
            else constant ? ++cf : ++af;
        }
    }
    return output == m.channels() && aq == m.quat_count && cq == m.const_quat_count &&
           av == m.vec3_count && cv == m.const_vec3_count &&
           af == m.float_count && cf == m.const_float_count;
}

static bool bf6__anim_vbr_curves(AnimClipHandle& h, size_t at, size_t bytes,
                                 float scale, int scalar_base, int scalar_count,
                                 bool padded, std::vector<AnimVbrCurve>& curves)
{
    curves.clear();
    if (!bytes) return true;
    const size_t end = at + bytes;
    if (end > h.bytes.size() || at >= end) return false;
    const int curve_count = h.bytes[at++];
    int stored_index = 0;
    curves.reserve((size_t)curve_count);
    for (int c = 0; c < curve_count; ++c)
    {
        if (at + 3 > end) return false;
        stored_index += h.bytes[at++];
        const int points = h.bytes[at++];
        if (!points) return false;
        int dense = stored_index;
        if (padded) {
            const int lane = stored_index & 3;
            if (lane == 3) return false;
            dense = (stored_index >> 2) * 3 + lane;
        }
        if (dense < 0 || dense >= scalar_count) return false;
        AnimVbrCurve curve;
        curve.scalar_dof = scalar_base + dense;
        curve.frames.resize((size_t)points);
        curve.values.resize((size_t)points);
        curve.values[0] = (float)(int8_t)h.bytes[at++] * scale / 127.f;
        int frame = 0;
        for (int p = 1; p < points; ++p) {
            if (at + 2 > end) return false;
            frame += (int)h.bytes[at++] * 8;
            curve.frames[(size_t)p] = frame;
            curve.values[(size_t)p] = (float)(int8_t)h.bytes[at++] * scale / 127.f;
        }
        curves.push_back(std::move(curve));
    }
    return at == end;
}

static bf6_anim_clip* bf6__anim_vbr_open(bf6_ctx* c, const char* res_name,
                                          bf6_anim_reloc* r)
{
    if (!r || r->region_count < 3) return nullptr;
    AnimVbrMetadata metadata;
    if (!bf6__anim_vbr_metadata(c, res_name, metadata)) return nullptr;
    if (r->regions[0].count != metadata.const_palette_size ||
        r->regions[1].count != (uint32_t)((metadata.num_keys + 7u) / 8u))
        return nullptr;

    AnimClipHandle* h = new AnimClipHandle();
    h->is_vbr = true;
    h->vbr = metadata;
    h->bytes.assign(r->data, r->data + r->size);
    auto fail = [&]() -> bf6_anim_clip* { delete h; return nullptr; };
    const size_t palette_at = r->regions[0].offset;
    const size_t block_size_at = r->regions[1].offset;
    const size_t data_at = r->regions[2].offset;
    const size_t data_bytes = r->regions[2].count;
    if (palette_at + (size_t)metadata.const_palette_size * 4 > h->bytes.size() ||
        block_size_at + (size_t)r->regions[1].count * 2 > h->bytes.size() ||
        data_at + data_bytes > h->bytes.size()) return fail();
    h->vbr_palette.resize(metadata.const_palette_size);
    for (size_t i = 0; i < h->vbr_palette.size(); ++i)
        std::memcpy(&h->vbr_palette[i], h->bytes.data() + palette_at + i * 4, 4);
    h->vbr_block_sizes.resize(r->regions[1].count);
    size_t stored_blocks = 0;
    for (size_t i = 0; i < h->vbr_block_sizes.size(); ++i) {
        std::memcpy(&h->vbr_block_sizes[i], h->bytes.data() + block_size_at + i * 2, 2);
        if (h->vbr_block_sizes[i] < 3) return fail();
        stored_blocks += h->vbr_block_sizes[i];
    }
    const size_t constant_dofs = (size_t)metadata.constant_dofs();
    h->vbr_constant_selector_size = metadata.const_palette_size <= 256 ? 1 : 2;
    const size_t prefix = metadata.key_time_size +
        constant_dofs * (size_t)h->vbr_constant_selector_size +
        metadata.const_channel_map_size + (size_t)metadata.animated_dofs() * 4 +
        metadata.vector_offset_size + metadata.float_offset_size;
    if (prefix + stored_blocks + 8 != data_bytes) return fail();
    const size_t constant_at = data_at + metadata.key_time_size;
    const size_t map_at = constant_at + constant_dofs *
        (size_t)h->vbr_constant_selector_size;
    const size_t width_at = map_at + metadata.const_channel_map_size;
    const size_t vector_at = width_at + (size_t)metadata.animated_dofs() * 4;
    const size_t float_at = vector_at + metadata.vector_offset_size;
    const size_t blocks_at = float_at + metadata.float_offset_size;
    h->vbr_constant_selector = constant_at;
    if (!bf6__anim_vbr_channel_map(*h, map_at)) return fail();
    h->vbr_widths.resize((size_t)metadata.animated_dofs() * 8);
    for (int dof = 0; dof < metadata.animated_dofs(); ++dof) {
        uint32_t word = 0;
        std::memcpy(&word, h->bytes.data() + width_at + (size_t)dof * 4, 4);
        for (int k = 0; k < 8; ++k)
            h->vbr_widths[(size_t)dof * 8 + k] = (uint8_t)((word >> (k * 4)) & 15u);
    }
    h->vbr_block_offsets.resize(h->vbr_block_sizes.size() + 1);
    size_t at = blocks_at;
    for (size_t i = 0; i < h->vbr_block_sizes.size(); ++i) {
        h->vbr_block_offsets[i] = at;
        at += h->vbr_block_sizes[i];
    }
    h->vbr_block_offsets.back() = at;
    if (at + 8 != data_at + data_bytes) return fail();
    for (size_t i = at; i < at + 8; ++i) if (h->bytes[i]) return fail();
    const int quat_dofs = (int)metadata.quat_count * 4;
    const int vec_dofs = (int)metadata.vec3_count * 3;
    if (!bf6__anim_vbr_curves(*h, vector_at, metadata.vector_offset_size,
            metadata.vector_offset_scale, quat_dofs, vec_dofs, true,
            h->vbr_vector_offsets) ||
        !bf6__anim_vbr_curves(*h, float_at, metadata.float_offset_size,
            metadata.float_offset_scale, quat_dofs + vec_dofs,
            metadata.float_count, false, h->vbr_float_offsets)) return fail();

    h->c.framing = BF6_ANIM_VBR;
    h->c.key_time_count = metadata.num_keys;
    h->c.quat_count = (int32_t)metadata.quat_count + metadata.const_quat_count;
    h->c.vec3_count = (int32_t)metadata.vec3_count + metadata.const_vec3_count;
    h->c.group_count = (int32_t)metadata.float_count + metadata.const_float_count;
    h->c.scalar_count = h->c.group_count;
    h->c.channel_count = metadata.channels();
    h->c.block_count = (int32_t)h->vbr_block_sizes.size();
    h->c.const_quat_count = metadata.const_quat_count;
    h->c.const_vec3_count = metadata.const_vec3_count;
    h->c.const_scalar_count = metadata.const_float_count;
    h->c.predicted_stream_bytes = (int64_t)stored_blocks;
    h->c.actual_stream_bytes = (int64_t)stored_blocks;
    h->c.constant_channels = h->vbr_constant.data();
    h->c.quat_min = metadata.quat_min;
    h->c.quat_max = metadata.quat_max;
    c->handles[&h->c] = bf6_ctx::HK_ANIMCLIP;
    return &h->c;
}

/* ------------------------------------------------- the RAW single frame ---
 *
 * WHAT THIS UNLOCKS. 1,020 of the 2,801 animation resources under the 1P tree
 * are RAW-framed and bf6_anim_clip_open refused every one, so each
 * `p_1p_<weapon>_stand_idle_01` - the authored rest pose for every gun in the
 * game - was unreadable. The 1P view worked around it by sampling frame 0 of a
 * turn-in-place transition, which is the same pose but not the asset the game
 * plays.
 *
 * THE FRAMING, measured across the RAW payloads under animations/.../1p/ and
 * confirmed by the offsets chaining exactly:
 *
 *   header 64 = five {count, offset, flags} descriptors + a 4-byte terminator
 *     region 0  KeyTimes        count 0 means implicit and dense
 *     region 1  MappingIndices  count u16
 *     region 2  ChannelIndices  count u16
 *     region 3  Data            count float4, the animated samples
 *     region 4  ConstData       count float4, the channels that never move
 *
 *   p_1p_rifle_crouch_idle_01    0,64 | 129,64 | 129,322 |    0,592 | 128,592
 *      128*16 = 2048;   592 + 2048 = 2640,  payload 2660  (20 trailing)
 *   tadtv_1p_rifle_crouchtostand_01
 *                                0,64 | 129,64 | 129,322 | 2091,592 | 87,34048
 *      2091*16 = 33456; 592 + 33456 = 34048 = ConstData's own offset, exactly
 *
 * A POSE IS THE CASE WHERE DATA IS EMPTY. Every channel sits in ConstData, one
 * value each, and both index regions are the identity run 0..count-1 because
 * nothing needs remapping when nothing is animated.
 *
 * THE ANIMATED CASE IS NOT DECODED HERE. Data divides by no consistent channel
 * count - ratios of 103.71, 59.80, 71.22 and 163.52 measured across the tree -
 * so those samples are per-channel runs addressed through the index regions
 * rather than a grid. Returning null for them leaves them exactly as they were
 * instead of inventing a layout that would decode into plausible nonsense.
 */
/* HOW MANY RECORDS ONE FRAME OCCUPIES, decided by a test the data answers.
 *
 * r1 is slot -> channel with the animated channels first, so the length of its
 * leading ascending run is the obvious candidate. It is also WRONG on most
 * clips: measured, the stride is that run on one clip and run - 1 on three
 * others, and no amount of staring at the counts explains which. A rule right
 * three times in four is worse than no rule.
 *
 * So the stride is not derived, it is VERIFIED. A quaternion is unit length
 * and a vector is not, so with the correct stride slot j is the same channel
 * in every frame and that column is unit in all frames or in none. A wrong
 * stride shears the grid, every column mixes the two kinds, and purity
 * collapses. Measured over the candidates:
 *
 *   p_..._stand_idle_phaseaim_yaw_01   run 21 -> 0.952   run-1 20 -> 0.000
 *   tadtv_..._crouchtostand_01         run 42 -> 0.000   run-1 41 -> 1.000
 *   tadtv_..._pronetocrouch_01         run 90 -> 0.000   run-1 89 -> 0.978
 *   c_..._prone_crawl_left_01          run 48 -> 0.000   run-1 47 -> 1.000
 *
 * The wrong candidate scores exactly zero every time, because there is no way
 * to slice a sheared grid and have every column come out pure. That is a
 * decision the data makes, not one the reader makes for it - and a clip where
 * no candidate is pure is REFUSED rather than decoded into plausible nonsense.
 */
static double bf6__raw_column_purity(const float* recs, uint32_t count, uint32_t stride)
{
    if (!stride || stride > count) return 0.0;
    const uint32_t frames = count / stride;
    if (frames < 3) return 0.0;
    uint32_t pure = 0;
    for (uint32_t slot = 0; slot < stride; slot++) {
        uint32_t unit = 0;
        for (uint32_t f = 0; f < frames; f++) {
            const float* v = &recs[((size_t)f * stride + slot) * 4u];
            const double m = std::sqrt((double)v[0]*v[0] + (double)v[1]*v[1]
                                     + (double)v[2]*v[2] + (double)v[3]*v[3]);
            if (m > 0.97 && m < 1.03) ++unit;
        }
        if (unit == 0u || unit == frames) ++pure;
    }
    return (double)pure / (double)stride;
}

/* ONE CHANNEL OF AN ANIMATED RAW CLIP, at one frame.
 *
 * The single place that knows how a channel index becomes storage, so the
 * quat/vec3 derivation at open time and the sampler cannot drift apart - they
 * did drift once already, the derivation reading slot order while a caller
 * indexed in channel order, and the result was a clip that bound cleanly and
 * posed wrongly.
 *
 * Returns nullptr when the channel has no storage. That happens for the
 * trailing packed-scalar keys: ConstData packs four scalars to a float4, so a
 * clip with 62 constant channels can ship 61 records. Those keys carry no bone
 * transform, so a caller that skips them loses nothing - but it must be told,
 * not handed a neighbouring channel's rotation.
 */
static const float* bf6__raw_channel_at(const AnimClipHandle* h, uint32_t frame, uint32_t ch)
{
    const uint32_t stride = h->raw_stride;
    if (!stride) return nullptr;
    /* No permutation: channels ARE the animated slots, as before. */
    const uint32_t slot = h->raw_slot.empty()
                        ? ch
                        : (ch < h->raw_slot.size() ? h->raw_slot[ch] : 0xFFFFu);
    if (slot == 0xFFFFu) return nullptr;
    if (slot < stride) {
        const size_t at = ((size_t)frame * stride + slot) * 4u;
        if (at + 4u > h->raw_values.size()) return nullptr;
        return &h->raw_values[at];
    }
    const size_t at = (size_t)(slot - stride) * 4u;
    if (at + 4u > h->raw_const.size()) return nullptr;
    return &h->raw_const[at];
}

/* The leading ascending run of the slot -> channel permutation. */
static uint32_t bf6__raw_animated_run(const bf6_anim_reloc* r)
{
    const bf6_anim_region& idx = r->regions[1];
    if (idx.count < 2 ||
        (size_t)idx.offset + (size_t)idx.count * 2u > (size_t)r->size) return 0;
    uint16_t prev = 0; std::memcpy(&prev, r->data + idx.offset, 2);
    uint32_t run = 1;
    for (uint32_t k = 1; k < idx.count; k++) {
        uint16_t v; std::memcpy(&v, r->data + idx.offset + (size_t)k * 2u, 2);
        if (v < prev) break;
        prev = v; ++run;
    }
    return run;
}

bf6_anim_clip* bf6__anim_raw_pose_open(bf6_ctx* c, bf6_anim_reloc* r)
{
    if (r->region_count < 5) return nullptr;
    const bf6_anim_region& data  = r->regions[3];
    const bf6_anim_region& konst = r->regions[4];
    if (data.count != 0u) return nullptr;      /* animated RAW: not this codec */
    if (konst.count == 0u || konst.count > 65535u) return nullptr;
    if ((size_t)konst.offset + (size_t)konst.count * 16u > (size_t)r->size) return nullptr;

    AnimClipHandle* h = new AnimClipHandle();
    h->is_raw_pose = true;
    h->raw_values.resize((size_t)konst.count * 4u);
    std::memcpy(h->raw_values.data(), r->data + konst.offset,
                (size_t)konst.count * 16u);

    /* THE CHANNEL KINDS, derived and then CHECKED rather than declared.
     *
     * Nothing in the 64-byte header states the quaternion / vector3 split the
     * clip struct reports, and the DCT framing's nq/nv/ng words have no
     * equivalent here. What is true of the values is that quaternions are unit
     * length and positions are not, so the leading unit-length run IS the
     * quaternion count.
     *
     * That is inference from data, so it carries its own control: everything
     * AFTER the run must be non-unit. If it is not - all unit, or an empty run
     * - the split is not trustworthy and the counts are left at zero rather
     * than reported wrongly. A caller needing it exactly should take it from
     * bf6_anim_bindings, whose stats carry quaternion_bones / vector_bones /
     * scalar_bones from the binding itself rather than from the values.
     */
    auto unit_like = [&](uint32_t i) {
        const float* v = &h->raw_values[(size_t)i * 4u];
        const double m = (double)v[0]*v[0] + (double)v[1]*v[1]
                       + (double)v[2]*v[2] + (double)v[3]*v[3];
        return m > 0.9801 && m < 1.0201;          /* magnitude 1 +/- 1% */
    };
    uint32_t nq = 0;
    while (nq < konst.count && unit_like(nq)) ++nq;
    uint32_t tail_unit = 0;
    for (uint32_t i = nq; i < konst.count; i++) if (unit_like(i)) ++tail_unit;
    const bool split_trusted = nq > 0 && nq < konst.count && tail_unit == 0;

    h->c.framing            = BF6_ANIM_RAW;
    h->c.key_time_count     = 1;
    h->c.channel_count      = (int32_t)konst.count;
    h->c.block_count        = 1;
    h->c.quat_count         = split_trusted ? (int32_t)nq : 0;
    h->c.vec3_count         = split_trusted ? (int32_t)(konst.count - nq) : 0;
    h->c.group_count        = 0;
    h->c.const_quat_count   = h->c.quat_count;
    h->c.const_vec3_count   = h->c.vec3_count;
    h->c.const_scalar_count = 0;
    h->c.sparse_times       = nullptr;
    h->c.sparse_time_count  = 0;
    h->c.predicted_stream_bytes = (int64_t)konst.count * 16;
    h->c.actual_stream_bytes    = (int64_t)konst.count * 16;
    c->handles[&h->c] = bf6_ctx::HK_ANIMCLIP;
    return &h->c;
}

/* ------------------------------------------------ the compact pose blob ---
 *
 * A SECOND POSE CONTAINER, and the one that matters most: it holds
 * p_1p_rifle_stand_idle_01, the authored standing rest pose the first-person
 * view is built around. bf6_anim_reloc_read refuses it before any codec sees
 * it - the dword at +4 reads 37, and 37 % 4 != 0 fails the "12n + 4m" shape
 * test that identifies the reloc container - so it was reported as "clip
 * unavailable" and the 1P view fell back to sampling frame 0 of a
 * turn-in-place transition instead.
 *
 * It is not a reloc payload at all. It is a flat header and a run of float4:
 *
 *   +0x00  u32  scalarCount
 *   +0x04  u32  vec3Count
 *   +0x08  u32  quatCount
 *   +0x0C  u32  channelCount   == quatCount + vec3Count + ceil(scalarCount/4)
 *   +0x10  u32  dataOffset     == 32
 *   +0x20       channelCount * float4, quaternions, then vector3s, then the
 *               packed scalar groups
 *   +end   4 trailing bytes
 *
 * On p_1p_rifle_stand_idle_01 that is 2 / 37 / 89 / 127 / 32 and
 * 32 + 127*16 + 4 = 2,068 bytes exactly, which is the file's own size. The
 * size identity is the check: a wrong field order still yields four plausible
 * counts, and only the total gives it away.
 */
bf6_anim_clip* bf6__anim_pose_blob_open(bf6_ctx* c, const char* res_name)
{
    const uint8_t* d = nullptr;
    const int64_t n = bf6_read_raw(c, BF6_RAW_RES, res_name, &d);
    if (n < 36 || !d) return nullptr;
    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, d + o, 4); return v; };

    const uint32_t ns = u32(0), nv = u32(4), nq = u32(8), nch = u32(12), off = u32(16);
    if (off != 32u || nch == 0u || nch > 65535u) return nullptr;
    /* The declared composition must add up, and the file must be exactly the
     * size that composition implies. Both, not either. */
    if (nq + nv + (ns + 3u) / 4u != nch) return nullptr;
    if ((int64_t)off + (int64_t)nch * 16 + 4 != n) return nullptr;

    AnimClipHandle* h = new AnimClipHandle();
    h->is_raw_pose = true;                 /* same one-frame sampling path */
    h->raw_values.resize((size_t)nch * 4u);
    std::memcpy(h->raw_values.data(), d + off, (size_t)nch * 16u);

    h->c.framing            = BF6_ANIM_RAW;
    h->c.key_time_count     = 1;
    h->c.channel_count      = (int32_t)nch;
    h->c.block_count        = 1;
    h->c.quat_count         = (int32_t)nq;
    h->c.vec3_count         = (int32_t)nv;
    h->c.group_count        = (int32_t)((ns + 3u) / 4u);
    h->c.scalar_count       = (int32_t)ns;
    h->c.const_quat_count   = (int32_t)nq;
    h->c.const_vec3_count   = (int32_t)nv;
    h->c.const_scalar_count = (int32_t)ns;
    h->c.sparse_times       = nullptr;
    h->c.sparse_time_count  = 0;
    h->c.predicted_stream_bytes = (int64_t)nch * 16;
    h->c.actual_stream_bytes    = (int64_t)nch * 16;
    c->handles[&h->c] = bf6_ctx::HK_ANIMCLIP;
    return &h->c;
}

/* AN ANIMATED RAW CLIP: frames * stride float4 records, frame-major.
 *
 * The ordering was established by scanning for the record spacing at which
 * neighbours are nearly identical - the spacing that follows ONE bone through
 * time. It wins by 100x to 400x over spacing 1 on every clip measured, which
 * is not a margin a coincidence produces.
 *
 * The stride itself is chosen by bf6__raw_column_purity above: the candidates
 * are the permutation's animated run and one less than it, the right one
 * scores at or near 1.0, and the wrong one scores exactly 0.0. A clip where
 * neither is pure is refused.
 *
 * WHAT IS STILL OPEN, and why it does not stop this being correct: when the
 * stride is run - 1, ONE channel in the permutation's animated run has no
 * column in Data, and which one is not yet established. Callers get the frame
 * count, the stride and the records; mapping slots back to channel indices
 * through r1 is exact only for the clips where stride == run. The rest expose
 * their data without a channel map rather than with a guessed one.
 */
bf6_anim_clip* bf6__anim_raw_anim_open(bf6_ctx* c, bf6_anim_reloc* r)
{
    if (r->region_count < 5) return nullptr;
    const bf6_anim_region& data  = r->regions[3];
    const bf6_anim_region& konst = r->regions[4];
    if (data.count < 8u) return nullptr;
    if ((size_t)data.offset + (size_t)data.count * 16u > (size_t)r->size) return nullptr;

    const float* recs = (const float*)(const void*)(r->data + data.offset);
    const uint32_t run = bf6__raw_animated_run(r);
    if (run < 2u) return nullptr;

    uint32_t stride = 0; double best = 0.0;
    for (uint32_t cand : { run, run - 1u }) {
        if (!cand || data.count % cand) continue;
        const double p = bf6__raw_column_purity(recs, data.count, cand);
        if (p > best) { best = p; stride = cand; }
    }
    /* Not "the better of two" - good ENOUGH, or nothing. A sheared grid cannot
     * score here, so a low best means the layout is not what this believes. */
    if (!stride || best < 0.90) return nullptr;

    AnimClipHandle* h = new AnimClipHandle();
    h->is_raw_pose = false;
    h->is_raw_anim = true;
    h->raw_stride  = stride;
    h->raw_values.resize((size_t)data.count * 4u);
    std::memcpy(h->raw_values.data(), recs, (size_t)data.count * 16u);

    /* THE FULL CHANNEL SET, not just the animated run.
     *
     * This used to report channel_count = stride and drop ConstData on the
     * floor with a (void) cast, which made an animated RAW clip look like it
     * drove only the bones that move. Nothing binds against that: the rig's
     * channel map has an entry for every channel, animated or not, so
     * bf6_anim_bindings saw 128 keys against 66 channels and refused the clip
     * outright. 354 of the 788 first-person rifle clips failed exactly this
     * way - vaulting, sprint-to-crouch, stand-to-prone, mounted, parachute.
     *
     * The container states the layout; it did not need inferring. Region 1
     * (MappingIndices) is a permutation slot -> channel, whose leading
     * ascending run is the animated slots and whose tail is the constant ones,
     * and region 2 (ChannelIndices) is its inverse. Checked on
     * ladtv_1p_rifle_stand_idle_01: r1's ascending run is 66 entries long,
     * which is the stride the column-purity scan independently picked, and
     * r1[0]=9 with r2[9]=0, r1[66]=0 with r2[0]=66 - the two regions invert
     * each other in both directions.
     *
     * So: channel c lives at slot r2[c]; slots below stride are a column of
     * Data, slots at or above it are a record of ConstData. */
    const bf6_anim_region& map = r->regions[1];
    uint32_t nch = stride;
    if (map.count > stride && map.count <= 65535u &&
        (size_t)map.offset + (size_t)map.count * 2u <= (size_t)r->size) {
        nch = map.count;
        h->raw_slot.resize(nch, 0xFFFFu);
        for (uint32_t s = 0; s < nch; s++) {
            uint16_t ch = 0;
            std::memcpy(&ch, r->data + map.offset + (size_t)s * 2u, 2);
            if (ch < nch) h->raw_slot[ch] = (uint16_t)s;   /* invert r1 ourselves */
        }
        /* Derived from r1 rather than read from r2, then CHECKED against r2
         * where r2 is present. Inverting the permutation we already trust
         * cannot disagree with itself; trusting r2 blindly could. */
        const bf6_anim_region& inv = r->regions[2];
        if (inv.count == nch &&
            (size_t)inv.offset + (size_t)inv.count * 2u <= (size_t)r->size) {
            for (uint32_t ch = 0; ch < nch; ch++) {
                uint16_t s = 0;
                std::memcpy(&s, r->data + inv.offset + (size_t)ch * 2u, 2);
                if (h->raw_slot[ch] != s) { h->raw_slot.clear(); nch = stride; break; }
            }
        }
        if (h->raw_slot.empty()) nch = stride;
    }
    if (!h->raw_slot.empty() && konst.count &&
        (size_t)konst.offset + (size_t)konst.count * 16u <= (size_t)r->size) {
        h->raw_const.resize((size_t)konst.count * 4u);
        std::memcpy(h->raw_const.data(), r->data + konst.offset, (size_t)konst.count * 16u);
    }

    h->c.framing        = BF6_ANIM_RAW;
    h->c.key_time_count = (int32_t)(data.count / stride);
    h->c.channel_count  = (int32_t)nch;
    h->c.block_count    = (int32_t)(data.count / stride);
    /* Derived per frame 0 the same way the pose reader does, with the same
     * control: the trailing values must NOT be unit or the split is not
     * reported at all. Read through the permutation, so the split describes
     * CHANNEL order - the order a binding indexes in - and not slot order. */
    uint32_t nq = 0;
    auto unit0 = [&](uint32_t ch) {
        const float* v = bf6__raw_channel_at(h, 0, ch);
        if (!v) return false;
        const double m = (double)v[0]*v[0] + (double)v[1]*v[1]
                       + (double)v[2]*v[2] + (double)v[3]*v[3];
        return m > 0.9801 && m < 1.0201;
    };
    while (nq < nch && unit0(nq)) ++nq;
    uint32_t tail_unit = 0;
    for (uint32_t i = nq; i < nch; i++) if (unit0(i)) ++tail_unit;
    const bool trusted = nq > 0 && nq < nch && tail_unit == 0;
    h->c.quat_count  = trusted ? (int32_t)nq : 0;
    h->c.vec3_count  = trusted ? (int32_t)(nch - nq) : 0;
    h->c.group_count = 0;
    h->c.const_quat_count = h->c.const_vec3_count = h->c.const_scalar_count = 0;
    h->c.sparse_times      = nullptr;
    h->c.sparse_time_count = 0;
    h->c.predicted_stream_bytes = (int64_t)data.count * 16;
    h->c.actual_stream_bytes    = (int64_t)data.count * 16;
    c->handles[&h->c] = bf6_ctx::HK_ANIMCLIP;
    return &h->c;
}

bf6_anim_clip* bf6_anim_clip_open(bf6_ctx* c, const char* res_name)
{
    bf6_anim_reloc* r = bf6_anim_reloc_read(c, res_name);
    /* Not a reloc payload at all - try the flat pose blob before giving up,
     * which is where the standing rest poses live. */
    if (!r) return bf6__anim_pose_blob_open(c, res_name);
    if (r->framing == BF6_ANIM_RAW) {
        bf6_anim_clip* clip = bf6__anim_raw_pose_open(c, r);
        if (!clip) clip = bf6__anim_raw_anim_open(c, r);
        bf6_free(c, r);
        return clip;
    }
    if (r->framing == BF6_ANIM_VBR) {
        bf6_anim_clip* clip = bf6__anim_vbr_open(c, res_name, r);
        bf6_free(c, r);
        return clip;
    }
    if (r->framing != BF6_ANIM_DCT || r->region_count < 2) { bf6_free(c, r); return nullptr; }

    const size_t   n  = (size_t)r->size;
    const uint32_t r1 = r->regions[1].offset;
    if ((size_t)r1 + 12 > n) { bf6_free(c, r); return nullptr; }

    AnimClipHandle* h = new AnimClipHandle();
    h->bytes.assign(r->data, r->data + n);
    const uint8_t* b = h->bytes.data();
    auto q16 = [&](size_t o) { uint16_t v; std::memcpy(&v, b + o, 2); return v; };

    const uint16_t keyTimes = q16(r1 + 0);
    const uint16_t nq       = q16(r1 + 2);
    const uint16_t nv       = q16(r1 + 4);
    const uint16_t ng       = q16(r1 + 6);
    h->block_quant      = q16(r1 + 8);
    h->sub_quant_tenths = b[r1 + 10];
    h->coef_bit_width   = b[r1 + 11];
    const uint32_t nch = (uint32_t)nq + nv + ng;
    if (!nch || nch > 65535u) { delete h; bf6_free(c, r); return nullptr; }

    /* Region 0 is the sparse key-time list. EMPTY means implicit and dense:
     * every integer 0..frameCount inclusive, so frameCount + 1 samples - the
     * EBX frame count is a last frame INDEX, not a sample count. */
    if (r->regions[0].count) {
        h->key_times.resize(r->regions[0].count);
        std::memcpy(h->key_times.data(), b + r->regions[0].offset,
                    (size_t)r->regions[0].count * 2);
    }

    size_t p = (size_t)r1 + 12;
    if (p + nch > n) { delete h; bf6_free(c, r); return nullptr; }
    std::vector<uint8_t> flags(b + p, b + p + nch);
    p += nch;
    if (p & 1u) p++;                       /* pad ONLY when the cursor is odd */

    h->ch.resize(nch);
    for (uint32_t i = 0; i < nch; i++) {
        AnimDctChannel& ac = h->ch[i];
        ac.rows = (uint8_t)(flags[i] >> 4);
        if (p + 8 + (size_t)ac.rows * 2 > n) { delete h; bf6_free(c, r); return nullptr; }
        for (int k = 0; k < 4; k++) {
            int16_t v; std::memcpy(&v, b + p + (size_t)k*2, 2); ac.base[k] = v;
        }
        p += 8;
        ac.width.resize(ac.rows);
        for (uint32_t k = 0; k < ac.rows; k++) { ac.width[k] = q16(p); p += 2; }
    }
    h->bits_off = p;
    /* THE STREAM ENDS AT REGION 1'S END, NOT AT EOF.
     *
     * region1.count is the body size in bytes, and the payload carries a small
     * relocation tail after it - 10 and 16 bytes on two sampled files, so it is
     * not even a constant to subtract. Measuring the stream as "everything left
     * in the file" silently overstates it by that tail, which breaks the one
     * check that validates the whole width model. */
    const uint64_t body_end = (uint64_t)r->regions[1].offset + r->regions[1].count;
    h->bits_len = (body_end > (uint64_t)p && body_end <= (uint64_t)n)
                ? (size_t)(body_end - (uint64_t)p)
                : (n - p);

    /* Resolve 0xF to the header default BEFORE totalling, or the size identity
     * is computed from nibbles the decoder will not actually use. */
    auto lane = [&](uint16_t w, int l) -> uint32_t {
        const uint32_t nib = (w >> (12 - 4*l)) & 0xFu;
        return (nib == 0xFu) ? (uint32_t)h->coef_bit_width : nib;
    };
    for (uint32_t i = 0; i < nch; i++) {
        const AnimDctChannel& ac = h->ch[i];
        for (uint32_t k = 0; k < ac.rows; k++) {
            uint32_t s = 0;
            for (int l = 0; l < 4; l++) s += lane(ac.width[k], l);
            h->full_block_bits += s;
            if (k == 0) h->first_coef_bits += s;    /* row 0 of every non-empty descriptor */
        }
    }

    h->c.key_time_count = keyTimes;
    h->c.framing        = BF6_ANIM_DCT;
    h->c.quat_count     = nq;
    h->c.vec3_count     = nv;
    h->c.group_count    = ng;
    h->c.channel_count  = (int32_t)nch;
    h->c.block_count    = (int32_t)(((uint32_t)keyTimes + 7u) / 8u);
    h->c.sparse_times   = h->key_times.empty() ? nullptr : h->key_times.data();
    h->c.sparse_time_count = (int32_t)h->key_times.size();
    h->c.full_block_bits = (int32_t)h->full_block_bits;
    h->c.first_coefficient_bits = (int32_t)h->first_coef_bits;

    /* The serializer's exact bound - a reader checking its own width model. */
    const uint64_t used = (uint64_t)h->c.block_count * h->full_block_bits
                        - (uint64_t)h->first_coef_bits;
    const uint64_t ub   = (used + 7ull) / 8ull;
    const uint64_t a16  = ((ub ? ub : 1ull) + 15ull) & ~15ull;
    h->c.predicted_stream_bytes = (int64_t)(a16 + 16ull);
    h->c.actual_stream_bytes    = (int64_t)h->bits_len;

    bf6_free(c, r);
    c->handles[&h->c] = bf6_ctx::HK_ANIMCLIP;
    return &h->c;
}

static bool bf6__anim_vbr_bits(const std::vector<uint8_t>& bytes, size_t at,
                               size_t count, int& bit, int width, int& value)
{
    value = 0;
    if (width < 0 || width > 15 || bit < 0 ||
        (uint64_t)bit + (uint64_t)width > (uint64_t)count * 8) return false;
    for (int i = 0; i < width; ++i) {
        const int logical = bit + i;
        value |= ((bytes[at + (size_t)(logical >> 3)] >> (logical & 7)) & 1) << i;
    }
    bit += width;
    return true;
}

static float bf6__anim_vbr_curve(const AnimVbrCurve& curve, int frame)
{
    if (curve.frames.empty()) return 0.f;
    if (frame <= curve.frames[0]) return curve.values[0];
    for (size_t i = 1; i < curve.frames.size(); ++i) {
        if (frame > curve.frames[i]) continue;
        const int span = curve.frames[i] - curve.frames[i - 1];
        if (span <= 0) return curve.values[i];
        const float t = (float)(frame - curve.frames[i - 1]) / (float)span;
        return curve.values[i - 1] + (curve.values[i] - curve.values[i - 1]) * t;
    }
    return curve.values.back();
}

static bool bf6__anim_vbr_sample(AnimClipHandle* h, int ordinal, float* out,
                                 float* out_quat_magnitude)
{
    if (!h || !out || ordinal < 0 || ordinal >= h->vbr.num_keys) return false;
    const int block = ordinal >> 3;
    const int phase = ordinal & 7;
    if ((size_t)block + 1 >= h->vbr_block_offsets.size()) return false;
    const int dofs = h->vbr.animated_dofs();
    std::vector<int> coefficients((size_t)dofs * 8, 0);
    std::vector<float> samples((size_t)dofs * 8, 0.f);
    const size_t block_at = h->vbr_block_offsets[(size_t)block];
    const size_t block_end = h->vbr_block_offsets[(size_t)block + 1];
    if (block_end < block_at + 3 || block_end > h->bytes.size()) return false;
    int bit = 0;
    const size_t bit_at = block_at + 3;
    const size_t bit_bytes = block_end - bit_at;
    bool present[8], nonnegative[8];
    for (int dof = 0; dof < dofs; ++dof)
    {
        std::memset(present, 0, sizeof(present));
        std::memset(nonnegative, 0, sizeof(nonnegative));
        for (int k = 0; k < 8; ++k) {
            if (!h->vbr_widths[(size_t)dof * 8 + k]) continue;
            int value = 0;
            if (!bf6__anim_vbr_bits(h->bytes, bit_at, bit_bytes, bit, 1, value))
                return false;
            present[k] = value != 0;
        }
        for (int k = 0; k < 8; ++k) if (present[k]) {
            int value = 0;
            if (!bf6__anim_vbr_bits(h->bytes, bit_at, bit_bytes, bit, 1, value))
                return false;
            nonnegative[k] = value != 0;
        }
        for (int k = 0; k < 8; ++k) if (present[k]) {
            int magnitude = 0;
            const int width = h->vbr_widths[(size_t)dof * 8 + k];
            if (!bf6__anim_vbr_bits(h->bytes, bit_at, bit_bytes, bit, width, magnitude))
                return false;
            coefficients[(size_t)dof * 8 + k] = nonnegative[k] ? magnitude : -magnitude;
        }
    }

    const int quat_dofs = (int)h->vbr.quat_count * 4;
    const int vec_dofs = (int)h->vbr.vec3_count * 3;
    const bool trajectory = !(h->vbr.flags & 0x80u) && h->vbr.vec3_count > 1;
    for (int dof = 0; dof < dofs; ++dof)
    {
        uint8_t rate = 0;
        float range_min = 0.f, range_max = 0.f;
        if (dof < quat_dofs) {
            rate = h->bytes[block_at];
            range_min = h->vbr.quat_min; range_max = h->vbr.quat_max;
        } else if (trajectory && dof < quat_dofs + 3) {
            rate = h->bytes[block_at + 1];
            range_min = h->vbr.trajectory_min; range_max = h->vbr.trajectory_max;
        } else if (dof < quat_dofs + vec_dofs) {
            rate = h->bytes[block_at + 2];
            range_min = h->vbr.vec3_min; range_max = h->vbr.vec3_max;
        } else {
            rate = h->bytes[block_at + 2];
            range_min = h->vbr.float_min; range_max = h->vbr.float_max;
        }
        const float type_scale = ((float)rate + 1.f) * h->vbr.dct / 5.f;
        for (int p = 0; p < 8; ++p) {
            float value = 0.f;
            for (int k = 0; k < 8; ++k) {
                const float dequant = (std::log((float)k + 2.f) * type_scale + 1.f) *
                                      (1.f / 32768.f) * h->vbr.dct;
                float basis = std::cos((float)(2 * p + 1) * (float)k *
                                       3.14159265358979323846f / 16.f);
                if (!k) basis *= 1.f / std::sqrt(2.f);
                value += (float)coefficients[(size_t)dof * 8 + k] * dequant * basis;
            }
            const float normalized = value * 0.25f;
            /* VBR block coefficients encode a residual around zero. The
             * encoder subtracts its predictor before clamping to the authored
             * min/max span, so adding range_min here biases every zero
             * residual toward -1 (and collapses animated quaternions after
             * normalization). Palette constants are absolute and continue to
             * use min + selector*span below. */
            samples[(size_t)dof * 8 + p] =
                normalized * (range_max - range_min);
        }
    }
    const int first_frame = block * 8;
    auto add_curves = [&](const std::vector<AnimVbrCurve>& curves) {
        for (const AnimVbrCurve& curve : curves)
            for (int p = 0; p < 8; ++p)
                samples[(size_t)curve.scalar_dof * 8 + p] +=
                    bf6__anim_vbr_curve(curve, first_frame + p);
    };
    add_curves(h->vbr_vector_offsets);
    add_curves(h->vbr_float_offsets);

    std::vector<float> constants((size_t)h->vbr.constant_dofs(), 0.f);
    size_t selector_at = h->vbr_constant_selector;
    auto selector = [&]() -> int {
        if (h->vbr_constant_selector_size == 1) return h->bytes[selector_at++];
        uint16_t value = 0; std::memcpy(&value, h->bytes.data() + selector_at, 2);
        selector_at += 2; return value;
    };
    auto constant_value = [&](float lo, float hi, float& value) -> bool {
        const int index = selector();
        if (index < 0 || (size_t)index >= h->vbr_palette.size()) return false;
        value = h->vbr_palette[(size_t)index] * (hi - lo) + lo;
        return true;
    };
    int next = 0;
    for (int q = 0; q < h->vbr.const_quat_count; ++q) {
        const int base = next;
        for (int lane = 0; lane < 4; ++lane)
            if (!constant_value(h->vbr.quat_min, h->vbr.quat_max,
                                constants[(size_t)next++])) return false;
        float m2 = 0.f; for (int lane = 0; lane < 4; ++lane)
            m2 += constants[(size_t)base + lane] * constants[(size_t)base + lane];
        if (m2 > 1e-20f) { const float inv = 1.f / std::sqrt(m2);
            for (int lane = 0; lane < 4; ++lane) constants[(size_t)base + lane] *= inv; }
    }
    for (int v = 0; v < h->vbr.const_vec3_count; ++v)
        for (int lane = 0; lane < 3; ++lane)
            if (!constant_value(h->vbr.vec3_min, h->vbr.vec3_max,
                                constants[(size_t)next++])) return false;
    for (int f = 0; f < h->vbr.const_float_count; ++f)
        if (!constant_value(h->vbr.float_min, h->vbr.float_max,
                            constants[(size_t)next++])) return false;

    const int total_quat = (int)h->vbr.quat_count + h->vbr.const_quat_count;
    const int total_vec = (int)h->vbr.vec3_count + h->vbr.const_vec3_count;
    int aq = 0, cq = 0, av = 0, cv = 0, af = 0, cf = 0;
    const int const_vec_base = (int)h->vbr.const_quat_count * 4;
    const int const_float_base = const_vec_base + (int)h->vbr.const_vec3_count * 3;
    const int anim_vec_base = (int)h->vbr.quat_count * 4;
    const int anim_float_base = anim_vec_base + (int)h->vbr.vec3_count * 3;
    for (int channel = 0; channel < h->vbr.channels(); ++channel)
    {
        float* destination = out + (size_t)channel * 4;
        destination[0] = destination[1] = destination[2] = destination[3] = 0.f;
        const bool constant = h->vbr_constant[(size_t)channel] != 0;
        if (channel < total_quat) {
            if (constant) {
                const int base = cq++ * 4;
                for (int lane = 0; lane < 4; ++lane)
                    destination[lane] = constants[(size_t)base + lane];
                if (out_quat_magnitude) out_quat_magnitude[channel] = 1.f;
            } else {
                const int base = aq++ * 4;
                float m2 = 0.f;
                for (int lane = 0; lane < 4; ++lane) {
                    destination[lane] = samples[(size_t)(base + lane) * 8 + phase];
                    m2 += destination[lane] * destination[lane];
                }
                const float magnitude = std::sqrt(m2);
                if (out_quat_magnitude) out_quat_magnitude[channel] = magnitude;
                if (m2 > 1e-20f) { const float inv = 1.f / magnitude;
                    for (int lane = 0; lane < 4; ++lane) destination[lane] *= inv; }
            }
        } else if (channel < total_quat + total_vec) {
            if (constant) {
                const int base = const_vec_base + cv++ * 3;
                for (int lane = 0; lane < 3; ++lane)
                    destination[lane] = constants[(size_t)base + lane];
            } else {
                const int base = anim_vec_base + av++ * 3;
                for (int lane = 0; lane < 3; ++lane)
                    destination[lane] = samples[(size_t)(base + lane) * 8 + phase];
            }
        } else {
            destination[0] = constant
                ? constants[(size_t)const_float_base + cf++]
                : samples[(size_t)(anim_float_base + af++) * 8 + phase];
        }
    }
    return true;
}

int bf6_anim_clip_sample(bf6_ctx* c, bf6_anim_clip* clip, int ordinal, float* out,
                         float* out_quat_magnitude)
{
    if (!c || !clip || !out || ordinal < 0) return 0;
    AnimClipHandle* h = (AnimClipHandle*)clip;
    /* A POSE HAS ONE FRAME, and every ordinal is that frame. Refusing anything
     * past ordinal 0 would be technically right and practically wrong: a caller
     * stepping a clip it believes is animated should stand still on the pose,
     * not stop dead partway through. */
    /* An animated RAW clip indexes straight in: frame-major, stride records
     * per frame, no bitstream and no block decode. Out of range is a refusal
     * rather than a clamp - a caller stepping past the end should be told. */
    if (h->is_raw_anim) {
        const uint32_t stride = h->raw_stride;
        if (!stride) return 0;
        const size_t frames = h->raw_values.size() / ((size_t)stride * 4u);
        if ((size_t)ordinal >= frames) return 0;
        /* Channel by channel through the permutation, because the animated
         * channels are scattered across the map rather than being its first
         * `stride` entries - a straight memcpy of the frame's Data row wrote
         * every constant channel's value into the wrong place. Identity for a
         * channel with no storage, so an unresolved packed scalar reads as a
         * no-op instead of as a neighbour's rotation. */
        const int32_t nch = clip->channel_count;
        for (int32_t ch = 0; ch < nch; ch++) {
            const float* v = bf6__raw_channel_at(h, (uint32_t)ordinal, (uint32_t)ch);
            float* dst = out + (size_t)ch * 4u;
            if (v) { dst[0] = v[0]; dst[1] = v[1]; dst[2] = v[2]; dst[3] = v[3]; }
            else   { dst[0] = dst[1] = dst[2] = 0.f; dst[3] = 1.f; }
        }
        if (out_quat_magnitude) {
            const float* v = bf6__raw_channel_at(h, (uint32_t)ordinal, 0);
            *out_quat_magnitude = v ? (float)std::sqrt((double)v[0]*v[0] + (double)v[1]*v[1]
                                                     + (double)v[2]*v[2] + (double)v[3]*v[3])
                                    : 0.f;
        }
        return 1;
    }
    if (h->is_raw_pose) {
        const size_t n = h->raw_values.size();
        if (!n) return 0;
        std::memcpy(out, h->raw_values.data(), n * sizeof(float));
        if (out_quat_magnitude) {
            /* The first channel's magnitude, the same self-check the other
             * codecs report - 1.0 here means the values arrived undamaged. */
            const float* v = h->raw_values.data();
            *out_quat_magnitude = (float)std::sqrt((double)v[0]*v[0] + (double)v[1]*v[1]
                                                 + (double)v[2]*v[2] + (double)v[3]*v[3]);
        }
        return 1;
    }
    if (h->is_vbr)
        return bf6__anim_vbr_sample(h, ordinal, out, out_quat_magnitude) ? 1 : 0;
    const uint32_t block = (uint32_t)ordinal >> 3;
    const uint32_t phase = (uint32_t)ordinal & 7u;
    if ((int32_t)block >= clip->block_count) return 0;

    const uint8_t* bits = h->bytes.data() + h->bits_off;
    auto lane = [&](uint16_t w, int l) -> int {
        const uint32_t nib = (w >> (12 - 4*l)) & 0xFu;
        return (nib == 0xFu) ? (int)h->coef_bit_width : (int)nib;
    };

    /* Block zero OMITS coefficient row zero - the descriptor base supplies it -
     * so every later block's offset is short by exactly that one row. */
    uint64_t pos = (block == 0)
                 ? 0ull
                 : (uint64_t)block * h->full_block_bits - (uint64_t)h->first_coef_bits;

    const float t  = (float)h->sub_quant_tenths * 0.1f;
    const float bq = h->block_quant ? (float)h->block_quant : 1.0f;
    float basis[8];
    basis[0] = 0.25f;
    for (int k = 1; k < 8; k++)
        basis[k] = 0.5f * (float)std::cos((2.0 * (double)phase + 1.0) * (double)k
                                          * 3.14159265358979323846 / 16.0);

    for (size_t i = 0; i < h->ch.size(); i++) {
        const AnimDctChannel& ac = h->ch[i];
        int16_t coef[8][4];
        std::memset(coef, 0, sizeof(coef));
        const uint32_t start = (block == 0) ? 1u : 0u;
        for (uint32_t k = start; k < ac.rows; k++)
            for (int l = 0; l < 4; l++)
                coef[k][l] = (int16_t)bf6__rdbits(bits, h->bits_len, pos, lane(ac.width[k], l));
        /* Unchecked int16 wrap, exactly as the engine does it. */
        for (int l = 0; l < 4; l++)
            coef[0][l] = (int16_t)((int32_t)coef[0][l] + (int32_t)ac.base[l]);

        float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (int k = 0; k < 8; k++) {
            const float qs = (1.0f + (float)k * t) / bq;
            for (int l = 0; l < 4; l++) v[l] += (float)coef[k][l] * qs * basis[k];
        }
        /* Quaternion channels normalise; vector channels do NOT. */
        if (i < (size_t)clip->quat_count) {
            const float d2 = v[0]*v[0] + v[1]*v[1] + v[2]*v[2] + v[3]*v[3];
            /* The PRE-normalisation magnitude is the decode-health signal, and
             * it is the only quaternion check that can fail. The encoder stored
             * a unit quaternion and quantised it, so a correct unpack arrives
             * near 1 before normalising; a wrong-but-consistent bit unpack
             * arrives anywhere. Normalising first and then checking the result
             * is unit is vacuous - it is unit by construction. */
            if (out_quat_magnitude) out_quat_magnitude[i] = std::sqrt(d2);
            if (d2 > 1e-20f) {
                const float s = 1.0f / std::sqrt(d2);
                v[0] *= s; v[1] *= s; v[2] *= s; v[3] *= s;
            }
        }
        std::memcpy(out + i*4, v, sizeof(v));
    }
    return 1;
}

void bf6__animclip_delete(void* p) { delete (AnimClipHandle*)p; }

/* ------------------------------------------------------- sampling by time - */

/* The DCT decoder takes a SAMPLE ORDINAL. Region 0 is what converts a playback
 * time into the surrounding ordinals and the fraction between them:
 *   - empty region 0 means times are implicit and DENSE - every integer from 0
 *     to the owning frame count inclusive, so the ordinal IS the time;
 *   - a non-empty region 0 is a sparse, strictly increasing key-time table, and
 *     two adjacent ordinals can be several frames apart.
 *
 * That distinction matters beyond convenience: on a sparse clip a large change
 * between adjacent ORDINALS is correct rather than a decode fault, because they
 * are not adjacent in TIME.
 */
static void bf6__clip_bracket(const bf6_anim_clip* c, float time, int loop,
                              int* lo, int* hi, float* frac)
{
    const int last = c->key_time_count > 0 ? c->key_time_count - 1 : 0;
    if (c->sparse_times && c->sparse_time_count > 1) {
        /* Sparse: find the bracketing pair by key TIME. */
        const int n = c->sparse_time_count;
        const float t0 = (float)c->sparse_times[0];
        const float tN = (float)c->sparse_times[n - 1];
        float t = time;
        if (loop && tN > t0) {
            const float period = tN - t0;
            t = t0 + std::fmod(std::fmod(t - t0, period) + period, period);
        }
        if (t <= t0)  { *lo = 0;     *hi = 0;     *frac = 0.0f; return; }
        if (t >= tN)  { *lo = n - 1; *hi = n - 1; *frac = 0.0f; return; }
        int a = 0, b = n - 1;
        while (b - a > 1) { const int m = (a + b) / 2;
                            if ((float)c->sparse_times[m] <= t) a = m; else b = m; }
        const float ta = (float)c->sparse_times[a], tb = (float)c->sparse_times[b];
        *lo = a; *hi = b;
        *frac = (tb > ta) ? (t - ta) / (tb - ta) : 0.0f;
        return;
    }
    /* Dense: the ordinal is the time. */
    float t = time;
    if (loop && last > 0) {
        const float period = (float)(last + 1);
        t = std::fmod(std::fmod(t, period) + period, period);
        const int l = (int)std::floor(t);
        *lo = l;
        *hi = (l + 1) % (last + 1);
        *frac = t - (float)l;
        return;
    }
    if (t <= 0.0f)          { *lo = 0;    *hi = 0;    *frac = 0.0f; return; }
    if (t >= (float)last)   { *lo = last; *hi = last; *frac = 0.0f; return; }
    const int l = (int)std::floor(t);
    *lo = l; *hi = l + 1; *frac = t - (float)l;
}

int bf6_anim_clip_sample_time(bf6_ctx* c, bf6_anim_clip* clip, float time, int loop,
                              float* out)
{
    if (!c || !clip || !out) return 0;
    int lo = 0, hi = 0; float f = 0.0f;
    bf6__clip_bracket(clip, time, loop, &lo, &hi, &f);

    if (!bf6_anim_clip_sample(c, clip, lo, out, nullptr)) return 0;
    if (hi == lo || f <= 0.0f) return 1;

    std::vector<float> b((size_t)clip->channel_count * 4);
    if (!bf6_anim_clip_sample(c, clip, hi, b.data(), nullptr)) return 1;

    for (int i = 0; i < clip->channel_count; i++) {
        float* A = out + (size_t)i * 4;
        const float* B = &b[(size_t)i * 4];
        if (i < clip->quat_count) {
            /* Shortest arc, then renormalise. NOTE: the format spec records
             * THAT the engine interpolates, not WHICH interpolation it uses, so
             * this is nlerp with a sign fix rather than a reproduction of the
             * engine's arithmetic. It is correct to within the usual nlerp/slerp
             * difference and is flagged so nobody reads it as measured. */
            float d = A[0]*B[0] + A[1]*B[1] + A[2]*B[2] + A[3]*B[3];
            const float s = (d < 0.0f) ? -1.0f : 1.0f;
            float q[4];
            for (int k = 0; k < 4; k++) q[k] = A[k] + f * (s * B[k] - A[k]);
            const float m2 = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
            if (m2 > 1e-20f) { const float r = 1.0f / std::sqrt(m2);
                               for (int k = 0; k < 4; k++) A[k] = q[k] * r; }
        } else {
            for (int k = 0; k < 4; k++) A[k] = A[k] + f * (B[k] - A[k]);
        }
    }
    return 1;
}
