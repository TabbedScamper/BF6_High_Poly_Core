/* The disk-only symbolic animation binder. The key order is authored by the
 * channel-map array; the rig table and DofSet spans provide the slot record;
 * only the record's exact FNV-1 bone name may cross into render space. */

struct AnimBindingDofRecord {
    uint32_t component_type = 0, stride = 0, byte_offset = 0;
    std::string name;
};

struct AnimBindingDefault {
    float value[4] = {0.f, 0.f, 0.f, 0.f};
    int component = BF6_ANIM_DOF_SCALAR;
};

static uint32_t bf6__anim_fnv1(const std::string& value)
{
    uint32_t hash = 0x811c9dc5u;
    for (unsigned char c : value) { hash *= 0x01000193u; hash ^= c; }
    return hash;
}

static bool bf6__anim_ebx(bf6_ctx* c, const char* name, bf6::Ebx& ebx)
{
    std::string error;
    std::string asset = name ? name : "";
    std::vector<uint8_t> raw = c->src.get_ebx(asset, error);
    if (raw.empty() && asset.size() > 4 &&
        asset.compare(asset.size() - 4, 4, ".ebx") == 0) {
        asset.resize(asset.size() - 4);
        raw = c->src.get_ebx(asset, error);
    }
    if (raw.empty()) raw = c->src.get_ebx(asset + ".ebx", error);
    if (raw.empty()) return false;
    /* Animation maps and DofSets live under `_af/` and `animations/`, outside
     * the bounded armory index. Use the complete mounted GUID index here. */
    ebx.set_guid_index(&c->src.partition_index());
    return ebx.parse(std::move(raw), error);
}

static bool bf6__anim_u32_array(const std::vector<uint8_t>& bytes,
                                std::vector<uint32_t>& values)
{
    if (bytes.size() % 4) return false;
    values.resize(bytes.size() / 4);
    if (!bytes.empty()) std::memcpy(values.data(), bytes.data(), bytes.size());
    return true;
}

static bool bf6__anim_map_keys(bf6_ctx* c, const std::string& map_path,
                               std::vector<uint32_t>& keys)
{
    bf6::Ebx ebx(*c->types);
    if (!bf6__anim_ebx(c, map_path.c_str(), ebx)) return false;
    for (size_t instance = 0; instance < ebx.instance_count(); ++instance) {
        const int64_t base = ebx.payload() + ebx.instance_offset(instance);
        std::vector<uint8_t> bytes;
        if (!ebx.opaque_array_at(base + 0x28, 4, bytes)) continue;
        std::vector<uint32_t> candidate;
        if (!bf6__anim_u32_array(bytes, candidate) || candidate.empty())
            continue;
        keys.swap(candidate);
        return true;
    }
    return false;
}

static bool bf6__anim_dof_set(bf6_ctx* c, const std::string& path,
                              std::vector<AnimBindingDofRecord>& records)
{
    bf6::Ebx ebx(*c->types);
    if (!bf6__anim_ebx(c, path.c_str(), ebx)) return false;
    for (size_t instance = 0; instance < ebx.instance_count(); ++instance) {
        const int64_t base = ebx.payload() + ebx.instance_offset(instance);
        std::vector<uint8_t> bytes;
        /* The slot table is serialized as an opaque UInt64 array: EBXX count
         * is in eight-byte elements, while its interior is the byte-oriented
         * 32-byte header/record/name-pool format. */
        if (!ebx.opaque_array_at(base + 0x28, 8, bytes) || bytes.size() < 32)
            continue;
        uint32_t header[8] = {};
        std::memcpy(header, bytes.data(), sizeof(header));
        const uint64_t count = header[1] == 0xFFFFFFFFu ? 0 : (uint64_t)header[1] + 1;
        const uint64_t records_end = 32 + count * 32;
        if (records_end > bytes.size() || header[4] != records_end ||
            (uint64_t)header[4] + header[5] > bytes.size()) continue;
        std::vector<AnimBindingDofRecord> parsed((size_t)count);
        for (size_t i = 0; i < parsed.size(); ++i) {
            const uint8_t* source = bytes.data() + 32 + i * 32;
            uint32_t words[4] = {};
            std::memcpy(words, source, sizeof(words));
            if (!words[1] || words[2] % words[1] ||
                words[3] >= header[5]) return false;
            parsed[i].component_type = words[0];
            parsed[i].stride = words[1];
            parsed[i].byte_offset = words[2];
            const char* name = (const char*)bytes.data() + header[4] + words[3];
            const size_t available = (size_t)header[5] - words[3];
            const void* end = std::memchr(name, 0, available);
            if (!end) return false;
            parsed[i].name.assign(name, (const char*)end);
        }
        records.swap(parsed);
        return true;
    }
    return false;
}

static bool bf6__anim_rig(bf6_ctx* c, const char* path,
                          std::vector<uint32_t>& keys,
                          std::vector<uint32_t>& offsets,
                          std::vector<std::string>& dof_paths,
                          std::vector<std::vector<AnimBindingDofRecord>>& dof_records,
                          std::map<uint32_t, AnimBindingDefault>& defaults,
                          int& span_matches)
{
    bf6::Ebx ebx(*c->types);
    if (!bf6__anim_ebx(c, path, ebx)) return false;
    for (size_t instance = 0; instance < ebx.instance_count(); ++instance) {
        const int64_t base = ebx.payload() + ebx.instance_offset(instance);
        std::vector<uint8_t> dof_bytes, key_bytes, offset_bytes;
        bf6::Ebx::OpaqueArrayDescriptor dof_desc;
        if (!ebx.opaque_array_at(base + 0x38, 8, dof_bytes, &dof_desc) ||
            !ebx.opaque_array_at(base + 0x48, 4, key_bytes) ||
            !ebx.opaque_array_at(base + 0x50, 4, offset_bytes) ||
            !bf6__anim_u32_array(key_bytes, keys) ||
            !bf6__anim_u32_array(offset_bytes, offsets) || keys.empty() ||
            offsets.size() != dof_desc.count || dof_bytes.size() != offsets.size() * 8)
            continue;
        bool monotonic = !offsets.empty() && offsets[0] == 0;
        for (size_t i = 0; i < offsets.size(); ++i)
            monotonic = monotonic && offsets[i] < keys.size() &&
                (!i || offsets[i] > offsets[i - 1]);
        if (!monotonic) continue;
        dof_paths.clear();
        for (size_t i = 0; i < offsets.size(); ++i) {
            std::string partition, imported;
            const int64_t pointer = ebx.payload() + dof_desc.offset + (int64_t)i * 8;
            if (!ebx.import_ref_at(pointer, partition, imported) || imported.empty())
                return false;
            dof_paths.push_back(imported);
        }
        dof_records.resize(dof_paths.size());
        span_matches = 0;
        for (size_t i = 0; i < dof_paths.size(); ++i) {
            if (!bf6__anim_dof_set(c, dof_paths[i], dof_records[i])) {
                std::fprintf(stderr, "animation binding: DofSet parse failed %s\n",
                             dof_paths[i].c_str()); return false;
            }
            const size_t end = i + 1 < offsets.size() ? offsets[i + 1] : keys.size();
            if (dof_records[i].size() != end - offsets[i]) {
                std::fprintf(stderr, "animation binding: DofSet span mismatch %s %zu/%zu\n",
                    dof_paths[i].c_str(), dof_records[i].size(), end - offsets[i]);
                return false;
            }
            ++span_matches;
        }
        /* The internal defaults object carries three key/value pairs. Values
         * are scalar float, padded float3 and float4 respectively; each key
         * array has the same count as its value array. */
        defaults.clear();
        for (size_t object = 0; object < ebx.instance_count(); ++object)
        {
            if (c->types->layout_full(ebx.instance_type(object)).name_hash !=
                0xC80950E0u) continue;
            const int64_t object_base = ebx.payload() + ebx.instance_offset(object);
            struct Pair { int value_offset, key_offset, stride, component, lanes; };
            const Pair pairs[] = {
                {0x38, 0x40, 4,  BF6_ANIM_DOF_SCALAR, 1},
                {0x48, 0x50, 16, BF6_ANIM_DOF_VECTOR3, 3},
                {0x58, 0x60, 16, BF6_ANIM_DOF_QUATERNION, 4},
            };
            int found_pairs = 0;
            std::map<uint32_t, AnimBindingDefault> candidate;
            for (const Pair& pair : pairs)
            {
                std::vector<uint8_t> value_bytes, typed_key_bytes;
                const bool value_ok = ebx.opaque_array_at(
                    object_base + pair.value_offset, pair.stride, value_bytes);
                const bool key_ok = ebx.opaque_array_at(
                    object_base + pair.key_offset, 4, typed_key_bytes);
                if (!value_ok || !key_ok) continue;
                std::vector<uint32_t> typed_keys;
                if (!bf6__anim_u32_array(typed_key_bytes, typed_keys) ||
                    value_bytes.size() != typed_keys.size() * (size_t)pair.stride)
                    continue;
                ++found_pairs;
                for (size_t typed = 0; typed < typed_keys.size(); ++typed) {
                    AnimBindingDefault value;
                    value.component = pair.component;
                    std::memcpy(value.value,
                        value_bytes.data() + typed * (size_t)pair.stride,
                        (size_t)pair.lanes * sizeof(float));
                    if (!candidate.emplace(typed_keys[typed], value).second) {
                        std::fprintf(stderr,
                            "animation binding: duplicate default key %08x object=%zu component=%d\n",
                            typed_keys[typed], object, pair.component);
                        return false;
                    }
                }
            }
            if (found_pairs == 3) { defaults.swap(candidate); break; }
        }
        return true;
    }
    return false;
}

int bf6_anim_bindings(bf6_ctx* c, const char* clip_ebx,
                      const char* rig_ebx, const char* skeleton_ebx,
                      bf6_anim_binding* out, int out_max,
                      bf6_anim_binding_stats* stats)
{
    if (stats) *stats = bf6_anim_binding_stats{};
    if (!c || !clip_ebx || !rig_ebx || !skeleton_ebx || out_max < 0) return -1;
    std::string type_error;
    if (!ensure_types(c, type_error)) {
        std::fprintf(stderr, "animation binding: type schema unavailable: %s\n",
                     type_error.c_str()); return -1;
    }
    bf6_anim_clip* clip = bf6_anim_clip_open(c, clip_ebx);
    if (!clip) { std::fprintf(stderr, "animation binding: clip unavailable %s\n", clip_ebx); return -1; }
    bf6::Ebx clip_data(*c->types);
    if (!bf6__anim_ebx(c, clip_ebx, clip_data)) {
        std::fprintf(stderr, "animation binding: clip EBX unavailable %s\n", clip_ebx);
        bf6_free(c, clip); return -1;
    }
    std::string map_path;
    for (size_t i = 0; i < clip_data.instance_count(); ++i) {
        const bf6::EbxValue row = clip_data.read_instance(i);
        std::string partition, instance, candidate;
        if (!clip_data.import_ref_field(row, 0x1B386E8Au, partition,
                                        instance, candidate) || candidate.empty()) continue;
        if (!map_path.empty() && map_path != candidate) {
            bf6_free(c, clip); return -1;
        }
        map_path = candidate;
    }
    std::vector<uint32_t> map_keys;
    if (map_path.empty() || !bf6__anim_map_keys(c, map_path, map_keys)) {
        std::fprintf(stderr, "animation binding: channel map unavailable path=%s expected=%d\n",
                     map_path.c_str(), clip->channel_count);
        bf6_free(c, clip); return -1;
    }
    const int transformKeys = clip->quat_count + clip->vec3_count;
    const int scalarKeys = (int)map_keys.size() - transformKeys;
    /* THE TEST IS THE LAYOUT, NOT THE CODEC THAT PRODUCED IT.
     *
     * These were gated on `framing == BF6_ANIM_DCT` and `!= BF6_ANIM_DCT`, as
     * though packing scalars four to a float4 were a property of the DCT codec.
     * It is not - it is a property of the payload. The flat pose blob packs the
     * same way: p_1p_rifle_stand_idle_01 declares 89 quaternions, 37 vectors
     * and 2 scalars, stores 127 float4 records, and its channel map carries 128
     * keys because the map counts scalars INDIVIDUALLY while the payload packs
     * them. Both numbers are right and they are not equal.
     *
     * Gated on the framing that clip could only take the direct branch, where
     * 128 != 127 rejected it and a perfectly good pose came back as "layout
     * mismatch". Ungated, the packed branch recognises it - and this is not a
     * weaker test: the group arithmetic still has to hold exactly, so counts
     * that do not add up are refused as firmly as before.
     *
     * A clip with no scalars satisfies both branches identically, which is why
     * dropping the framing condition changes no verdict that used to pass. */
    bool packedLayout = scalarKeys >= 0 &&
        clip->group_count == (scalarKeys + 3) / 4 &&
        clip->channel_count == transformKeys + clip->group_count;
    const bool directLayout = (int)map_keys.size() == clip->channel_count;

    /* WHEN THE PAYLOAD CANNOT STATE ITS OWN VECTOR / SCALAR SPLIT.
     *
     * The RAW framing declares no channel kinds at all, so its reader derives
     * the quaternion count from unit length - which is sound, quaternions are
     * unit and nothing else is - and then cannot tell a vector3 from a float4
     * holding four packed scalars, because neither is unit. It reports every
     * non-quaternion as a vector3 and no groups, and the check above then
     * fails by exactly the number of scalars that were packed.
     *
     * The two counts determine the split, so solve rather than guess. With
     * Q quaternions, C payload channels and K map keys:
     *
     *     v + g = C - Q        the payload stores vectors and groups
     *     v + s = K - Q        the map names vectors and scalars
     *     g = ceil(s / 4)      four scalars to a float4
     *  => s - ceil(s/4) = K - C
     *
     * which is strictly increasing in s, so there is at most one answer and
     * finding it confirms the reading rather than assuming it. p_1p_rifle_
     * crouch_idle_01: K-C = 1, s = 2, g = 1, v = 37, and 90 + 37 + 2 = 129
     * keys against 90 + 37 + 1 = 128 stored channels.
     *
     * A clip that already stated a consistent split never reaches this. */
    int solved_vec3 = clip->vec3_count;
    if (!packedLayout && !directLayout && clip->group_count == 0 &&
        clip->quat_count > 0 && scalarKeys >= 0) {
        const int d = (int)map_keys.size() - clip->channel_count;
        /* BOUND THE SEARCH BY THE MAP, NOT BY scalarKeys. scalarKeys is
         * derived from the clip's own vec3 count, which is precisely the
         * number this loop exists because the payload got wrong - on the
         * crouch pose it reports 38 where the truth is 37, capping the search
         * at s = 1 when the answer is s = 2. The most scalars there can be is
         * every non-quaternion key. */
        const int max_s = (int)map_keys.size() - clip->quat_count;
        for (int s = 0; s <= max_s && d >= 0; s++) {
            const int g = (s + 3) / 4;
            if (s - g != d) continue;
            const int v = clip->channel_count - clip->quat_count - g;
            if (v < 0 || clip->quat_count + v + s != (int)map_keys.size()) break;
            solved_vec3 = v;
            packedLayout = true;
            std::fprintf(stderr,
                "animation binding: derived the packed split for %s - "
                "%d quat, %d vec3, %d scalar in %d group(s)\n",
                clip_ebx, clip->quat_count, v, s, g);
            break;
        }
    }
    if (!packedLayout && !directLayout) {
        std::fprintf(stderr,
            "animation binding: channel/map layout mismatch path=%s "
            "keys=%zu decoded=%d q=%d v=%d groups=%d framing=%d\n",
            map_path.c_str(), map_keys.size(), clip->channel_count,
            clip->quat_count, clip->vec3_count, clip->group_count,
            clip->framing);
        bf6_free(c, clip); return -1;
    }

    std::vector<uint32_t> rig_keys, offsets;
    std::vector<std::string> dof_paths;
    std::vector<std::vector<AnimBindingDofRecord>> dof_records;
    std::map<uint32_t, AnimBindingDefault> defaults;
    int span_matches = 0;
    if (!bf6__anim_rig(c, rig_ebx, rig_keys, offsets, dof_paths,
                       dof_records, defaults, span_matches)) {
        std::fprintf(stderr, "animation binding: rig/DofSet graph unavailable %s\n", rig_ebx);
        bf6_free(c, clip); return -1;
    }
    std::map<uint32_t, size_t> rig_by_key;
    for (size_t i = 0; i < rig_keys.size(); ++i)
        if (!rig_by_key.emplace(rig_keys[i], i).second) {
            bf6_free(c, clip); return -1;
        }
    bf6_skeleton* skeleton = bf6_skeleton_read(c, skeleton_ebx);
    if (!skeleton) {
        std::fprintf(stderr, "animation binding: render skeleton unavailable %s\n", skeleton_ebx);
        bf6_free(c, clip); return -1;
    }
    std::map<uint32_t, int> bone_by_hash;
    for (int i = 0; i < skeleton->bone_count; ++i)
        bone_by_hash.emplace(skeleton->bones[i].name_hash, i);

    bf6_anim_binding_stats report{};
    report.map_keys = (int32_t)map_keys.size();
    report.rig_keys = (int32_t)rig_keys.size();
    report.dof_sets = (int32_t)dof_paths.size();
    report.exact_span_matches = span_matches;
    for (const auto& records : dof_records) report.dof_records += (int32_t)records.size();
    std::vector<bf6_anim_binding> rows(map_keys.size());
    const int quat_end = clip->quat_count;
    /* The SOLVED vector count, which equals clip->vec3_count for every payload
     * that stated its own split and differs only for the RAW poses that could
     * not. Using the clip's own number here would put the scalar boundary one
     * channel too far along and silently mis-lane every packed scalar. */
    const int vec_end = quat_end + solved_vec3;
    for (size_t channel = 0; channel < map_keys.size(); ++channel)
    {
        bf6_anim_binding& row = rows[channel];
        row = bf6_anim_binding{};
        row.key = map_keys[channel];
        row.channel = (int32_t)channel;
        row.lane = 0;
        row.rig_key = row.dof_set = row.dof_record = row.storage_index = row.bone = -1;
        row.component = (int)channel < quat_end ? BF6_ANIM_DOF_QUATERNION
                      : (int)channel < vec_end ? BF6_ANIM_DOF_VECTOR3
                                              : BF6_ANIM_DOF_SCALAR;
        /* Packed scalars: the map names them one at a time, the payload stores
         * four to a float4, so a scalar's channel is its group and its lane is
         * its position in that group. Keyed on the LAYOUT, which is why the
         * flag above is no longer named for the DCT codec. */
        if (row.component == BF6_ANIM_DOF_SCALAR && packedLayout) {
            const int scalar = (int)channel - vec_end;
            row.channel = vec_end + scalar / 4;
            row.lane = scalar % 4;
        }
        const auto key = rig_by_key.find(row.key);
        if (key == rig_by_key.end()) { ++report.unresolved_keys; continue; }
        row.rig_key = (int32_t)key->second; ++report.resolved_rig;
        const auto upper = std::upper_bound(offsets.begin(), offsets.end(),
                                             (uint32_t)key->second);
        if (upper == offsets.begin()) { ++report.unresolved_keys; continue; }
        const size_t set = (size_t)(upper - offsets.begin() - 1);
        const size_t local = key->second - offsets[set];
        if (set >= dof_records.size() || local >= dof_records[set].size()) {
            ++report.unresolved_keys; continue;
        }
        const AnimBindingDofRecord& record = dof_records[set][local];
        row.dof_set = (int32_t)set; row.dof_record = (int32_t)local;
        row.storage_index = (int32_t)(record.byte_offset / record.stride);
        std::snprintf(row.dof_name, sizeof(row.dof_name), "%s", record.name.c_str());
        ++report.resolved_storage;
        const auto default_value = defaults.find(row.key);
        if (default_value != defaults.end() &&
            default_value->second.component == row.component) {
            std::memcpy(row.default_value, default_value->second.value,
                        sizeof(row.default_value));
            row.has_default_value = 1;
            ++report.resolved_defaults;
        }
        std::string bone = record.name;
        if (bone.size() > 2 && bone[bone.size() - 2] == '.' &&
            (bone.back() == 'q' || bone.back() == 't' || bone.back() == 's'))
            bone.resize(bone.size() - 2);
        const auto found_bone = bone_by_hash.find(bf6__anim_fnv1(bone));
        if (found_bone == bone_by_hash.end()) { ++report.unresolved_bones; continue; }
        row.bone = found_bone->second; ++report.resolved_bones;
        if (row.component == BF6_ANIM_DOF_QUATERNION) ++report.quaternion_bones;
        else if (row.component == BF6_ANIM_DOF_VECTOR3) ++report.vector_bones;
        else ++report.scalar_bones;
    }
    report.fake_control_hits = rig_by_key.count(0xDEADBEEFu) ? 1 : 0;
    if (stats) *stats = report;
    if (out && out_max > 0)
        for (int i = 0; i < (int)rows.size() && i < out_max; ++i) out[i] = rows[(size_t)i];
    bf6_free(c, skeleton);
    bf6_free(c, clip);
    return (int)rows.size();
}
