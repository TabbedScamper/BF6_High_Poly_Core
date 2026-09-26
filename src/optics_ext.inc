/* Read-only optics evidence. Included at the end of armory_ext.inc.
 * Roots are enumerated from the install; imports retain their instance identity.
 * Unknown fields remain hashes, so consumers cannot mistake guesses for data. */
namespace bf6optics {
static std::string path(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (s.size() >= 4 && s.substr(s.size() - 4) == ".ebx") s.resize(s.size() - 4);
    return s;
}
static std::string quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char ch : s) {
        if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
        else if (ch < 32) { char b[7]; std::snprintf(b, sizeof(b), "\\u%04x", ch); out += b; }
        else out += ch;
    }
    return out + "\"";
}
static std::string hash(uint32_t h) {
    char b[9]; std::snprintf(b, sizeof(b), "%08x", h); return b;
}
static std::string value(const bf6::EbxValue& v) {
    using K = bf6::EbxValue::Kind;
    switch (v.kind) {
    case K::Bool: return v.b ? "true" : "false";
    case K::Int: return std::to_string(v.i);
    case K::Uint: return std::to_string(v.u);
    case K::Real: {
        if (!std::isfinite(v.f)) return "null";
        char b[64]; std::snprintf(b, sizeof(b), "%.17g", v.f); return b;
    }
    case K::Str: return quote(v.s);
    case K::InstanceRef: return "{\"instance\":" + std::to_string(v.instance) + "}";
    case K::ImportRef: return "{\"path\":" + quote(v.import_path) +
        ",\"guid\":" + quote(v.import_instance) + "}";
    case K::Struct: {
        std::string s = "{";
        for (const auto& f : v.fields) { if (s.size() > 1) s += ','; s += quote(hash(f.first)) + ':' + value(f.second); }
        return s + '}';
    }
    case K::Array: {
        std::string s = "[";
        for (const auto& a : v.items) { if (s.size() > 1) s += ','; s += value(a); }
        return s + ']';
    }
    default: return "null";
    }
}
static void imports(const bf6::EbxValue& v, std::vector<std::string>& pending) {
    if (v.kind == bf6::EbxValue::Kind::ImportRef) {
        const std::string p = path(v.import_path);
        if (p.find("/aimingcontrollers/") != std::string::npos ||
            p.find("/_zoomlevels/") != std::string::npos ||
            p.find("/ve_") != std::string::npos) pending.push_back(p);
    }
    for (const auto& f : v.fields) imports(f.second, pending);
    for (const auto& a : v.items) imports(a, pending);
}
}

int64_t bf6_optics_read(bf6_ctx* c, const char* item, const char* mesh,
    const char* bundle, const char* state_key, char* out, int out_len)
{
    if (!c || !item || !mesh || !bundle || !state_key) return -1;
    std::string error;
    if (!ensure_types(c, error)) return -1;
    using namespace bf6optics;
    const std::string m = path(mesh);
    const size_t art = m.find("/art/");
    const std::string folder = art == std::string::npos ? "" : m.substr(0, art + 1);
    std::vector<std::string> pending, modifiers;
    auto enumerate = [&](const std::string& search, bool optic) {
        const int n = bf6_list_ebx(c, search.c_str(), nullptr, 0);
        if (n <= 0) return;
        std::vector<bf6_asset> rows((size_t)n);
        const int got = bf6_list_ebx(c, search.c_str(), rows.data(), n);
        for (int i = 0; i < got; ++i) {
            const std::string p = rows[(size_t)i].name;
            const std::string leaf = bf6__aam_stem(p);
            if (optic) {
                if (p.substr(0, p.find_last_of('/') + 1) == folder && leaf.find("wpm_") == 0)
                    modifiers.push_back(p);
            } else if (leaf.size() > 3 && leaf.substr(leaf.size() - 3) == "_wb") pending.push_back(p);
        }
    };
    enumerate("/" + std::string(item) + "/", false);
    if (!folder.empty()) enumerate(folder, true);
    if (modifiers.size() == 1) pending.push_back(modifiers[0]);
    std::string json = "{\"modifier\":" + quote(modifiers.size() == 1 ? modifiers[0] : "") +
        ",\"modifier_candidates\":" + std::to_string(modifiers.size()) + ",\"assets\":[";
    std::set<std::string> seen;
    bool first = true;
    // Ours: a defensive graph size bound, not a game limit.
    for (size_t at = 0; at < pending.size() && seen.size() < 128; ++at) {
        const std::string p = pending[at];
        if (!seen.insert(p).second) continue;
        auto raw = c->src.get_ebx(p + ".ebx", error);
        if (raw.empty()) raw = c->src.get_ebx(p, error);
        bf6::Ebx e(*c->types);
        e.set_guid_index(&c->src.armory_partition_index());
        if (raw.empty() || !e.parse(std::move(raw), error)) continue;
        if (!first) json += ',';
        first = false;
        json += "{\"path\":" + quote(p) + ",\"instances\":[";
        for (size_t i = 0; i < e.instance_count(); ++i) {
            if (i) json += ',';
            const auto v = e.read_instance(i);
            imports(v, pending);
            json += "{\"type\":" + quote(bf6::TypeDb::guid_str(e.instance_type(i))) +
                ",\"guid\":" + quote(e.instance_guid(i)) + ",\"fields\":" + value(v) + "}";
        }
        json += "]}";
    }
    json += "],\"constants\":{";
    bf6::MaterialBinding binding;
    const uint64_t key = std::strtoull(state_key, nullptr, 16);
    if (!m.empty() && key) for (const auto& scope : material_scopes(c, m, bundle)) {
        const auto b = scope.depot->textures_for(key, *scope.bytes);
        if (b.valid) for (const auto& kv : b.constants) binding.constants.emplace(kv);
    }
    first = true;
    for (const auto& kv : binding.constants) {
        if (!first) json += ',';
        first = false;
        json += quote(hash(kv.first)) + ":[";
        for (size_t i = 0; i + 4 <= kv.second.size(); i += 4) {
            if (i) json += ',';
            float f; std::memcpy(&f, kv.second.data() + i, 4);
            bf6::EbxValue v; v.kind = bf6::EbxValue::Kind::Real; v.f = f;
            json += value(v);
        }
        json += ']';
    }
    float in_s = 0, out_s = 0;
    const int tier = bf6_inspect_zoom_transition(c, item, &in_s, &out_s);
    json += "},\"transition_tier\":" + std::to_string(tier);
    if (tier >= 0) {
        bf6::EbxValue v; v.kind = bf6::EbxValue::Kind::Real;
        v.f = in_s; json += ",\"in_seconds\":" + value(v);
        v.f = out_s; json += ",\"out_seconds\":" + value(v);
    }
    json += '}';
    if (out && out_len > 0) {
        const size_t n = std::min(json.size(), (size_t)out_len - 1);
        std::memcpy(out, json.data(), n); out[n] = 0;
    }
    return (int64_t)json.size();
}
