/* Soldier-only registry contracts, corpus 6a1c1b. Included inside expression.
 * OURS: the one offline player's opaque handle is 1 and soldier entity index is 0.
 * These are transport identities, not gameplay tuning or native pointer addresses.
 * PlayerId comes from the motion-machine field supplied by the caller. */
static bool soldier_registry_describe(uint32_t key, OperatorSignature& out) {
    if (!bf6::SoldierFields::evaluating()) return false;
    out = OperatorSignature{};
    switch (key) {
    case 0x17877ACCu: out.input_widths = {12, 16}; out.output_width = 16; return true;
    case 0x2948B3E1u: out.input_widths = {8}; out.output_width = 4; return true;
    case 0x94DB0BEEu: out.input_widths = {4}; out.output_width = 8; return true;
    case 0xFC25F6A7u:
        out.input_widths = {4, 1}; out.extra_output_widths = {8};
        out.output_width = 1; return true;
    case 0x82CC1F7Bu: out.input_widths = {8, 1}; out.output_width = 260; return true;
    default: return false;
    }
}

static bool soldier_registry_invoke(uint32_t key, const std::vector<Value>& args, Value& out) {
    OperatorSignature sig;
    if (!soldier_registry_describe(key, sig) || args.size() != sig.input_widths.size()) return false;
    for (size_t i = 0; i < args.size(); ++i)
        if (!args[i].known || args[i].bytes.size() < sig.input_widths[i]) return false;
    if (key == 0x17877ACCu) {
        /* Reflected (Source Vec3, Rotation Quat), native 147EDE9F0. This is
         * q*v rotation, with no normalization. Source's pad does not affect xyz.
         * OURS: zero Vec3 ABI padding in the result. */
        float v[3], q[4], result[4] = {};
        std::memcpy(v, args[0].bytes.data(), 12);
        std::memcpy(q, args[1].bytes.data(), 16);
        const float a = v[0]*q[3] + q[1]*v[2] - v[1]*q[2];
        const float b = v[1]*q[3] + q[2]*v[0] - v[2]*q[0];
        const float c = v[2]*q[3] + q[0]*v[1] - v[0]*q[1];
        result[0] = v[0] + 2.0f*(q[1]*c - q[2]*b);
        result[1] = v[1] + 2.0f*(q[2]*a - q[0]*c);
        result[2] = v[2] + 2.0f*(q[0]*b - q[1]*a);
        out.bytes.resize(16);
        std::memcpy(out.bytes.data(), result, 16);
        out.known = true;
        return true;
    }
    float id[4];
    const auto& sf = bf6::SoldierFields::get();
    const auto* player = sf.by_name("PlayerId");
    if (!player || !sf.known(*player)) return false;
    sf.value(*player, id);
    if (!std::isfinite(id[0]) || id[0] < 0) return false;
    const uint64_t handle = 1; // OURS: opaque local-player token, see above.
    if (key == 0x2948B3E1u) {
        /* FUN_1475F4920: null returns zero, live player reads +0x60 TeamId.
         * Do not let the generic host's null-only fallback answer a live token. */
        uint64_t input = 0;
        std::memcpy(&input, args[0].bytes.data(), 8);
        if (input != 0 && input != handle) return false;
        float team[4] = {};
        const auto* field = sf.by_name("TeamId");
        if (input == handle) {
            if (!field || !sf.known(*field)) return false;
            sf.value(*field, team);
        }
        out = Value::from_u32((uint32_t)(int32_t)team[0]);
        return true;
    }
    if (key == 0x94DB0BEEu) {
        /* FUN_14173FF40: index outside the table or empty entry returns null. */
        out.bytes.assign(8, 0);
        if (args[0].as_u32() == (uint32_t)id[0]) std::memcpy(out.bytes.data(), &handle, 8);
    } else if (key == 0xFC25F6A7u) {
        /* Thunk 147B70910: two inputs, handle then found outputs. Native 147B707E0
         * matches Player +0x40; mode true uses 143C96720 (same id, optional proxy).
         * Our player has no proxy. MISS LEAVES THE HANDLE UNCHANGED. The VM has no
         * in/out slot contract, so refuse that path rather than clear the handle. */
        if (args[0].as_u32() != (uint32_t)id[0]) return false;
        out.bytes.assign(9, 0);
        out.bytes[0] = 1;
        std::memcpy(out.bytes.data() + 1, &handle, 8);
    } else {
        /* FUN_14432B8E0 clears GameQueryResult then selects controlled soldier
         * (+D8/+E0); spectator fallback (+E8/+F0) only if no controlled soldier.
         * Both flag values therefore return our soldier for our player. */
        uint64_t input = 0;
        std::memcpy(&input, args[0].bytes.data(), 8);
        if (input != 0 && input != handle) return false;
        const uint32_t none = 0x000FFFFFu; // Native collection constructor 147F13AB0.
        out.bytes.assign(260, 0);
        for (size_t i = 1; i <= 64; ++i) std::memcpy(out.bytes.data() + i * 4, &none, 4);
        if (input == handle) {
            const uint32_t count = 1, entity = 0; // OURS: same index as C8364385.
            std::memcpy(out.bytes.data(), &count, 4);
            std::memcpy(out.bytes.data() + 4, &entity, 4);
        }
    }
    out.known = true;
    return true;
}
