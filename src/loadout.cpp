/* The equipment a LootSpawner drops, for every engine: the Unreal plugin's
 * loadout catalogue, attachment join and configured weapon assembly
 * (BF6HighPolyLoadoutDecode.cpp, BF6HighPolyLoadoutAttachments.cpp), moved
 * here so the Godot plugin offers the same choices and draws the same weapon.
 * See bf6_loadout_catalogue in bf6_core.h. */
#include "bf6_core.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

std::string leaf(std::string s)
{
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".ebx") == 0) s.resize(s.size() - 4);
    const size_t at = s.find_last_of('/');
    return at == std::string::npos ? s : s.substr(at + 1);
}

std::string dir_of(const std::string& s)
{
    const size_t at = s.find_last_of('/');
    return at == std::string::npos ? std::string() : s.substr(0, at);
}

bool starts(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
bool ends(const std::string& s, const std::string& p)
{
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

std::string fold(const std::string& s)
{
    std::string r;
    for (unsigned char c : s) if (std::isalnum(c)) r.push_back((char)std::tolower(c));
    return r;
}

std::vector<std::string> names(bf6_ctx* c, const char* query)
{
    std::vector<std::string> out;
    const int n = bf6_list_ebx(c, query, nullptr, 0);
    if (n < 1 || n > 1000000) return out;
    std::vector<bf6_asset> rows((size_t)n);
    const int got = std::clamp(bf6_list_ebx(c, query, rows.data(), n), 0, n);
    for (int i = 0; i < got; ++i) if (rows[(size_t)i].name) out.emplace_back(rows[(size_t)i].name);
    std::sort(out.begin(), out.end());
    return out;
}

std::string exact_leaf(const std::vector<std::string>& rows, const std::string& wanted, const std::string& within)
{
    std::string found;
    for (const std::string& p : rows) {
        if ((!within.empty() && !starts(p, within + "/")) || leaf(p) != wanted) continue;
        if (!found.empty() && found != p) return std::string();
        found = p;
    }
    return found;
}

void json_str(std::string& out, const std::string& s)
{
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
            else out.push_back((char)c);
        }
    }
    out.push_back('"');
}

int64_t give(const std::string& text, uint8_t** out)
{
    if (!out) return -1;
    *out = (uint8_t*)std::malloc(text.size() + 1);
    if (!*out) return -1;
    std::memcpy(*out, text.data(), text.size());
    (*out)[text.size()] = 0;
    return (int64_t)text.size();
}

std::vector<std::string> lines(const char* text)
{
    std::vector<std::string> out;
    if (!text) return out;
    std::string cur;
    for (const char* p = text;; ++p) {
        if (*p == '\n' || *p == '\r' || *p == 0) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            if (*p == 0) break;
        } else {
            cur.push_back(*p);
        }
    }
    return out;
}

struct Item { std::string id, label, group, asset; };

/* The equipment list: common/hardware/{weapons,gadgets}/<class>/<name>/..., no
 * leading underscores, labelled from the weapon metadata where it names one. */
std::vector<Item> read_items(bf6_ctx* c, const std::vector<std::string>& ebx)
{
    std::vector<Item> items;
    std::set<std::string> seen;
    for (const std::string& p : ebx) {
        std::vector<std::string> seg;
        size_t a = 0;
        while (true) {
            size_t b = p.find('/', a);
            seg.push_back(p.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        if (seg.size() < 6 || seg[0] != "common" || seg[1] != "hardware") continue;
        if (seg[2] != "weapons" && seg[2] != "gadgets") continue;
        if (starts(seg[3], "_") || starts(seg[4], "_")) continue;
        const std::string id = seg[3] + "/" + seg[4];
        if (!seen.insert(id).second) continue;
        Item it;
        it.id = id;
        it.label = seg[4];
        for (char& ch : it.label) ch = (char)std::toupper((unsigned char)ch);
        it.asset = seg[0] + "/" + seg[1] + "/" + seg[2] + "/" + seg[3] + "/" + seg[4];
        it.group = seg[2] == "weapons" ? "Weapon"
                 : (seg[3].find("grenade") != std::string::npos || seg[3].find("throw") != std::string::npos)
                     ? "Throwable" : "Gadget";
        items.push_back(std::move(it));
    }
    std::vector<bf6_weapon_name_row> rows(512);
    const int count = std::clamp(bf6_weapon_names(c, rows.data(), (int)rows.size()), 0, (int)rows.size());
    for (Item& it : items)
        for (int n = 0; n < count; ++n)
            if (leaf(it.id) == rows[(size_t)n].weapon && rows[(size_t)n].name[0]) {
                it.label = rows[(size_t)n].name;
                break;
            }
    std::stable_sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.label < y.label; });
    return items;
}

struct Choice { std::string id, label, slot, bundle, description, icon_atlas; int icon_index = -1; };

const std::map<std::string, std::string>& slot_prefixes()
{
    static const std::map<std::string, std::string> m = {
        {"scp", "Scope"}, {"sca", "Scope"}, {"brl", "Barrel"}, {"mzl", "Muzzle"}, {"mag", "Magazine"},
        {"amo", "Ammo"}, {"erg", "Ergonomic"}, {"btm", "Bottom"}, {"top", "Top"}, {"lft", "Left"}, {"rgt", "Right"}};
    return m;
}

/* A weapon's fittable attachments that join, uniquely and exactly, a part
 * file token and a Portal attachment enum. */
std::vector<Choice> read_attachments(bf6_ctx* c, const Item& item, const std::vector<std::string>& ebx,
                                     const std::vector<std::string>& enums)
{
    std::vector<Choice> out;
    if (item.group != "Weapon") return out;
    const std::string weapon = leaf(item.id);
    const int count = bf6_weapon_attachment_catalogue(c, weapon.c_str(), nullptr, 0);
    if (count < 1 || count > 8192) return out;
    std::vector<bf6_attachment_catalogue_row> rows((size_t)count);
    if (bf6_weapon_attachment_catalogue(c, weapon.c_str(), rows.data(), count) != count) return out;
    const auto& prefixes = slot_prefixes();
    std::map<std::string, std::set<std::string>> tokens;
    const std::string stem = "attachment_" + weapon + "_";
    for (const std::string& p : ebx) {
        if (!starts(p, item.asset + "/")) continue;
        const std::string l = leaf(p);
        if (!starts(l, stem)) continue;
        const std::string rest = l.substr(stem.size());
        const size_t us = rest.find('_');
        if (us == std::string::npos) continue;
        const std::string slot = rest.substr(0, us), token = rest.substr(us + 1);
        if (!prefixes.count(slot)) continue;
        tokens[slot].insert(token);
    }
    std::map<std::string, Choice> candidates;
    std::set<std::string> ambiguous;
    for (const auto& row : rows) {
        std::string slot = row.slot;
        if (slot == "opt") slot = "sca";
        auto pre = prefixes.find(slot);
        auto tok = tokens.find(slot);
        if (pre == prefixes.end() || tok == tokens.end()) continue;
        std::vector<std::string> matches;
        for (const std::string& t : tok->second)
            if (fold(t) == row.name_key || fold(weapon + slot + t) == row.ad_stem) matches.push_back(t);
        if (matches.size() != 1) continue;
        std::vector<std::string> pub;
        const std::string want = fold(row.name);
        for (const std::string& e : enums)
            if (starts(e, pre->second + "_") && fold(e.substr(pre->second.size() + 1)) == want) pub.push_back(e);
        if (pub.size() != 1) continue;
        const std::string key = slot + ":" + pub[0];
        Choice ch;
        ch.id = pub[0];
        ch.label = row.name;
        ch.slot = slot;
        ch.bundle = matches[0];
        ch.description = row.description;
        ch.icon_atlas = row.icon_atlas;
        ch.icon_index = row.icon_index;
        auto prev = candidates.find(key);
        if (prev != candidates.end()) { if (prev->second.bundle != ch.bundle) ambiguous.insert(key); }
        else candidates.emplace(key, std::move(ch));
    }
    for (auto& kv : candidates) if (!ambiguous.count(kv.first)) out.push_back(kv.second);
    std::stable_sort(out.begin(), out.end(), [](const Choice& x, const Choice& y) { return x.label < y.label; });
    return out;
}

const Item* find_item(const std::vector<Item>& items, const std::string& id)
{
    for (const Item& it : items) if (it.id == id) return &it;
    return nullptr;
}

std::string model_definition(bf6_ctx* c, const std::vector<std::string>& ebx, const Item& item)
{
    std::set<std::string> targets;
    for (const std::string& p : ebx) {
        if (dir_of(p) != item.asset || !starts(leaf(p), "cust_")) continue;
        bf6_ebx_instance_import rows[64]{};
        const int n = bf6_armory_ebx_instance_imports(c, p.c_str(), 0, rows, 64);
        for (int i = 0; i < std::min(n, 64); ++i)
            if (rows[i].field_hash == 0xff2bbdd1 && rows[i].path[0]) {
                std::string t = rows[i].path;
                if (ends(t, ".ebx")) t.resize(t.size() - 4);
                targets.insert(t);
            }
    }
    if (targets.size() == 1) return *targets.begin();
    if (targets.size() > 1) return std::string();
    return exact_leaf(ebx, "md_" + leaf(item.id), item.asset);
}

/* A 3x4 row-major transform (right, up, forward, translation) applied the way
 * the Unreal plugin's FMatrix44f does: p' = p.x*R + p.y*U + p.z*F + T. */
void xform_point(const float* m, const float* p, float* out)
{
    for (int k = 0; k < 3; ++k) out[k] = p[0] * m[k] + p[1] * m[3 + k] + p[2] * m[6 + k] + m[9 + k];
}
void xform_vector(const float* m, const float* v, float* out)
{
    for (int k = 0; k < 3; ++k) out[k] = v[0] * m[k] + v[1] * m[3 + k] + v[2] * m[6 + k];
}

/* Normals under a 3x4 transform: the inverse transpose of its linear part. */
bool normal_matrix(const float* m, double out[9])
{
    const double a = m[0], b = m[1], cc = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + cc * (d * h - e * g);
    if (std::fabs(det) < 1e-12) return false;
    /* Rows of the inverse transpose, in the same row-vector convention. */
    out[0] = (e * i - f * h) / det; out[1] = -(d * i - f * g) / det; out[2] = (d * h - e * g) / det;
    out[3] = -(b * i - cc * h) / det; out[4] = (a * i - cc * g) / det; out[5] = -(a * h - b * g) / det;
    out[6] = (b * f - cc * e) / det; out[7] = -(a * f - cc * d) / det; out[8] = (a * e - b * d) / det;
    return true;
}

bool any_bound(const bf6_mesh* m)
{
    for (int i = 0; i < m->material_count; ++i) if (m->materials[i].texture_count > 0) return true;
    return false;
}

} // namespace

extern "C" int64_t bf6_loadout_catalogue(bf6_ctx* c, uint8_t** out)
{
    if (!c || !out) return -1;
    char why[512] = {0};
    if (!bf6_mount_frontend(c, why, sizeof(why))) return -1;
    const std::vector<Item> items = read_items(c, names(c, "common/hardware/"));
    std::string j = "{\"items\":[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) j += ',';
        j += "{\"id\":"; json_str(j, items[i].id);
        j += ",\"label\":"; json_str(j, items[i].label);
        j += ",\"group\":"; json_str(j, items[i].group);
        j += ",\"asset\":"; json_str(j, items[i].asset);
        j += '}';
    }
    j += "]}";
    return give(j, out);
}

extern "C" int64_t bf6_loadout_attachments(bf6_ctx* c, const char* item_id, const char* portal_enums, uint8_t** out)
{
    if (!c || !item_id || !out) return -1;
    char why[512] = {0};
    if (!bf6_mount_frontend(c, why, sizeof(why))) return -1;
    const std::vector<std::string> ebx = names(c, "common/hardware/");
    const std::vector<Item> items = read_items(c, ebx);
    const Item* item = find_item(items, item_id);
    std::string j = "{\"item\":";
    json_str(j, item_id);
    j += ",\"attachments\":[";
    if (item) {
        const std::vector<Choice> choices = read_attachments(c, *item, ebx, lines(portal_enums));
        for (size_t i = 0; i < choices.size(); ++i) {
            const Choice& ch = choices[i];
            if (i) j += ',';
            j += "{\"id\":"; json_str(j, ch.id);
            j += ",\"label\":"; json_str(j, ch.label);
            j += ",\"slot\":"; json_str(j, ch.slot);
            j += ",\"bundle\":"; json_str(j, ch.bundle);
            j += ",\"description\":"; json_str(j, ch.description);
            j += ",\"icon_atlas\":"; json_str(j, ch.icon_atlas);
            j += ",\"icon_index\":" + std::to_string(ch.icon_index) + "}";
        }
    }
    j += "]}";
    return give(j, out);
}

extern "C" int64_t bf6_loadout_weapon(bf6_ctx* c, const char* item_id, const char* fits_text,
                                      const char* portal_enums, uint8_t** out)
{
    if (!c || !item_id || !out) return -1;
    char why[512] = {0};
    std::string error;
    std::string sections_json;
    std::vector<float> body;
    std::string anchors_json;
    auto finish = [&]() -> int64_t {
        std::string j = "{\"item\":";
        json_str(j, item_id);
        j += ",\"error\":";
        json_str(j, error);
        j += ",\"sections\":[" + sections_json + "],\"anchors\":{" + anchors_json + "}}";
        while ((j.size() + 12) % 4) j.push_back(' ');
        const uint32_t head[3] = {0x50574C42u /* BLWP */, 1u, (uint32_t)j.size()};
        const size_t total = 12 + j.size() + body.size() * sizeof(float);
        *out = (uint8_t*)std::malloc(total);
        if (!*out) return -1;
        std::memcpy(*out, head, 12);
        std::memcpy(*out + 12, j.data(), j.size());
        if (!body.empty()) std::memcpy(*out + 12 + j.size(), body.data(), body.size() * sizeof(float));
        return (int64_t)total;
    };
    if (!bf6_mount_frontend(c, why, sizeof(why))) { error = why[0] ? why : "The reader cannot mount front-end equipment."; return finish(); }
    const std::vector<std::string> ebx = names(c, "common/hardware/");
    const std::vector<Item> items = read_items(c, ebx);
    const Item* item = find_item(items, item_id);
    if (!item) { error = "This item is absent from the installed equipment catalogue."; return finish(); }
    const std::string md = model_definition(c, ebx, *item);
    if (md.empty()) { error = "This item has no unambiguous model definition."; return finish(); }
    const std::string equipment = exact_leaf(ebx, "equipment_" + leaf(item->id), item->asset);

    bf6_weapon_fit fits[64]{};
    int nfits = 0;
    std::vector<std::string> fit_strings;
    fit_strings.reserve(256);
    if (!equipment.empty()) nfits = bf6_weapon_factory_fits(c, equipment.c_str(), fits, 64);
    if (nfits < 0 || nfits > 64) { error = "The item's factory configuration is unreadable."; return finish(); }
    /* The factory strings are context-owned and live until the next factory
     * call; keep copies so the user's choices can be mixed in safely. */
    for (int i = 0; i < nfits; ++i) {
        fit_strings.emplace_back(fits[i].slot ? fits[i].slot : "");
        fit_strings.emplace_back(fits[i].attachment ? fits[i].attachment : "");
    }
    const std::vector<std::string> wanted = lines(fits_text);
    std::vector<Choice> available;
    if (!wanted.empty()) available = read_attachments(c, *item, ebx, lines(portal_enums));
    std::vector<std::pair<std::string, std::string>> fit_pairs;
    for (int i = 0; i < nfits; ++i) fit_pairs.emplace_back(fit_strings[(size_t)i * 2], fit_strings[(size_t)i * 2 + 1]);
    for (const std::string& w : wanted) {
        const size_t eq = w.find('=');
        if (eq == std::string::npos) continue;
        const std::string slot = w.substr(0, eq), id = w.substr(eq + 1);
        const Choice* ch = nullptr;
        for (const Choice& a : available) if (a.slot == slot && a.id == id) { ch = &a; break; }
        if (!ch) { error = "An attachment is unavailable for this weapon. Choose it again in Loadout."; return finish(); }
        bool replaced = false;
        for (auto& fp : fit_pairs) if (fp.first == slot) { fp.second = ch->bundle; replaced = true; break; }
        if (!replaced) {
            if (fit_pairs.size() == 64) { error = "Too many configured attachments."; return finish(); }
            fit_pairs.emplace_back(slot, ch->bundle);
        }
    }
    nfits = (int)fit_pairs.size();
    for (int i = 0; i < nfits; ++i) {
        fits[i].slot = fit_pairs[(size_t)i].first.c_str();
        fits[i].attachment = fit_pairs[(size_t)i].second.c_str();
    }

    std::vector<bf6_weapon_part_pose> parts(256);
    std::vector<bf6_bone_xform> bones(1024);
    int bone_count = 0;
    const int count = bf6_weapon_configured_assembly(c, md.c_str(), fits, nfits, parts.data(), 256,
                                                     bones.data(), 1024, &bone_count);
    if (count < 1 || count > 256 || bone_count < 0 || bone_count > 1024) {
        error = "No complete configured item assembly was returned.";
        return finish();
    }
    struct Part { std::string mesh, bundle; float attach[12]; bool has_attach; };
    std::vector<Part> copy;
    for (int i = 0; i < count; ++i) {
        if (!parts[(size_t)i].mesh) continue;
        Part p;
        p.mesh = parts[(size_t)i].mesh;
        p.bundle = parts[(size_t)i].bundle ? parts[(size_t)i].bundle : "";
        std::memcpy(p.attach, parts[(size_t)i].attach_transform, sizeof(p.attach));
        p.has_attach = parts[(size_t)i].has_attach_transform != 0;
        copy.push_back(std::move(p));
    }

    /* Slot anchors: each attachment bone's socket under the configured skin. */
    if (bone_count > 0) {
        if (bf6_skeleton* s = bf6_skeleton_read(c, "common/characters/_soldier/_weaponskeleton")) {
            static const std::pair<const char*, const char*> kSlots[] = {
                {"Wep_Scope_ATT", "scp"}, {"Wep_SecondarySight_ATT", "sca"}, {"Wep_Barrel_ATT", "brl"},
                {"Wep_Muzzle_ATT", "mzl"}, {"Wep_MGZ_ATT", "mag"}, {"Wep_UnderBarrel_ATT", "btm"}};
            for (int i = 0; i < std::min(s->bone_count, bone_count); ++i) {
                if (!s->bones[i].name) continue;
                for (const auto& kv : kSlots) {
                    if (std::strcmp(s->bones[i].name, kv.first) != 0) continue;
                    const float origin[3] = {s->bones[i].model[9], s->bones[i].model[10], s->bones[i].model[11]};
                    float p[3];
                    xform_point(bones[(size_t)i].m, origin, p);
                    if (std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2])) {
                        char b[160];
                        std::snprintf(b, sizeof(b), "%s\"%s\":[%.6g,%.6g,%.6g]", anchors_json.empty() ? "" : ",",
                                      kv.second, p[0], p[1], p[2]);
                        anchors_json += b;
                    }
                }
            }
            bf6_free(c, s);
        }
    }

    bool first_section = true;
    for (const Part& part : copy) {
        bf6_mesh* m = bf6_read_armory_mesh_scoped(c, part.mesh.c_str(), 0,
                                                  part.bundle.empty() ? nullptr : part.bundle.c_str(), nullptr);
        if (!m) { error = "no mesh at " + part.mesh; sections_json.clear(); body.clear(); return finish(); }
        std::string scope = part.bundle;
        /* A scoped read that bound nothing in a bundle with no depot retries in
         * the mesh's own scope, as the Unreal plugin's ReadMesh does. */
        if (!part.bundle.empty() && !any_bound(m) && bf6_material_scope_exists(c, part.bundle.c_str()) != 1) {
            if (bf6_mesh* plain = bf6_read_armory_mesh_scoped(c, part.mesh.c_str(), 0, nullptr, nullptr)) {
                if (any_bound(plain)) { bf6_free(c, m); m = plain; scope.clear(); }
                else bf6_free(c, plain);
            }
        }
        double nm[9];
        const bool attach_normals = part.has_attach && normal_matrix(part.attach, nm);
        for (int si = 0; si < m->section_count; ++si) {
            const bf6_section& s = m->sections[si];
            if (s.vertex_count <= 0 || s.index_count <= 0 || !s.positions) continue;
            std::vector<float> pos(s.positions, s.positions + (size_t)s.vertex_count * 3);
            std::vector<float> nrm;
            if (s.normals) nrm.assign(s.normals, s.normals + (size_t)s.vertex_count * 3);
            if (bone_count > 0 && m->mesh_type == 1 && s.skin_bones && s.skin_weights) {
                for (int v = 0; v < s.vertex_count; ++v) {
                    float P[3] = {0, 0, 0}, N[3] = {0, 0, 0};
                    float accepted = 0.f;
                    for (int lane = 0; lane < s.skin_influences; ++lane) {
                        const int idx = v * s.skin_influences + lane;
                        const uint16_t raw = s.skin_bones[idx];
                        /* A weapon skeleton has no appended renderbones (rig count 0). */
                        const int bone = (raw & 0x8000) ? ((raw & 0x7fff) >> 1) : raw;
                        const float w = s.skin_weights[idx];
                        if (w <= 0.f) continue;
                        if (bone < 0 || bone >= bone_count) {
                            bf6_free(c, m);
                            error = "preview skin references an unavailable bone";
                            sections_json.clear(); body.clear();
                            return finish();
                        }
                        float tp[3];
                        xform_point(bones[(size_t)bone].m, &pos[(size_t)v * 3], tp);
                        for (int k = 0; k < 3; ++k) P[k] += tp[k] * w;
                        if (!nrm.empty()) {
                            float tn[3];
                            xform_vector(bones[(size_t)bone].m, &nrm[(size_t)v * 3], tn);
                            for (int k = 0; k < 3; ++k) N[k] += tn[k] * w;
                        }
                        accepted += w;
                    }
                    if (accepted > 1e-8f) {
                        for (int k = 0; k < 3; ++k) pos[(size_t)v * 3 + k] = P[k] / accepted;
                        if (!nrm.empty()) {
                            const float len = std::sqrt(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
                            if (len > 1e-8f) for (int k = 0; k < 3; ++k) nrm[(size_t)v * 3 + k] = N[k] / len;
                        }
                    }
                }
            }
            if (part.has_attach) {
                for (int v = 0; v < s.vertex_count; ++v) {
                    float tp[3];
                    xform_point(part.attach, &pos[(size_t)v * 3], tp);
                    std::memcpy(&pos[(size_t)v * 3], tp, sizeof(tp));
                    if (!nrm.empty() && attach_normals) {
                        const float* n = &nrm[(size_t)v * 3];
                        double o[3];
                        for (int k = 0; k < 3; ++k) o[k] = n[0] * nm[k] + n[1] * nm[3 + k] + n[2] * nm[6 + k];
                        const double len = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
                        if (len > 1e-12) for (int k = 0; k < 3; ++k) nrm[(size_t)v * 3 + k] = (float)(o[k] / len);
                    }
                }
            }
            char buf[256];
            if (!first_section) sections_json += ',';
            first_section = false;
            sections_json += "{\"mesh\":"; json_str(sections_json, part.mesh);
            sections_json += ",\"bundle\":"; json_str(sections_json, scope);
            std::snprintf(buf, sizeof(buf), ",\"state_key\":\"%016llx\",\"vertex_count\":%d,\"index_count\":%d",
                          (unsigned long long)s.state_key, s.vertex_count, s.index_count);
            sections_json += buf;
            std::snprintf(buf, sizeof(buf), ",\"positions\":%zu", body.size());
            sections_json += buf;
            body.insert(body.end(), pos.begin(), pos.end());
            std::snprintf(buf, sizeof(buf), ",\"normals\":%lld", nrm.empty() ? -1LL : (long long)body.size());
            sections_json += buf;
            body.insert(body.end(), nrm.begin(), nrm.end());
            std::snprintf(buf, sizeof(buf), ",\"uvs\":%lld", s.uv0 ? (long long)body.size() : -1LL);
            sections_json += buf;
            if (s.uv0) body.insert(body.end(), s.uv0, s.uv0 + (size_t)s.vertex_count * 2);
            /* Indices and colours ride in the float body as exact bit patterns. */
            std::snprintf(buf, sizeof(buf), ",\"indices\":%zu", body.size());
            sections_json += buf;
            const size_t at = body.size();
            body.resize(at + (size_t)s.index_count);
            std::memcpy(&body[at], s.indices, (size_t)s.index_count * sizeof(uint32_t));
            std::snprintf(buf, sizeof(buf), ",\"colors\":%lld,\"decal\":%d", s.colors ? (long long)body.size() : -1LL, s.is_decal);
            sections_json += buf;
            if (s.colors) {
                const size_t cat = body.size();
                body.resize(cat + (size_t)s.vertex_count);
                std::memcpy(&body[cat], s.colors, (size_t)s.vertex_count * sizeof(uint32_t));
            }
            if (m->materials && s.material >= 0 && s.material < m->material_count) {
                const bf6_material_desc& d = m->materials[s.material];
                std::snprintf(buf, sizeof(buf),
                              ",\"alpha_test\":%d,\"translucent\":%d,\"alpha_from_albedo\":%d,\"nsm\":%d,"
                              "\"terrain_decal_receiver\":%d,\"base_color\":[%.9g,%.9g,%.9g],\"roughness\":%.9g,\"textures\":[",
                              d.alpha_test, d.translucent, d.alpha_from_albedo, d.normal_is_nsm, d.terrain_decal_receiver,
                              d.base_color[0], d.base_color[1], d.base_color[2], d.roughness);
                sections_json += buf;
                for (int b = 0; b < d.texture_count; ++b) {
                    const char* tn = bf6_texture_name_at(c, d.textures[b].texture);
                    std::snprintf(buf, sizeof(buf), "%s[%d,%d,", b ? "," : "", (int)d.textures[b].slot, d.textures[b].texture);
                    sections_json += buf;
                    json_str(sections_json, tn ? tn : "");
                    sections_json += ']';
                }
                sections_json += "],\"shader_textures\":[";
                for (int b = 0; b < d.shader_texture_count; ++b) {
                    const char* tn = bf6_texture_name_at(c, d.shader_textures[b].texture);
                    std::snprintf(buf, sizeof(buf), "%s[%u,%d,", b ? "," : "", d.shader_textures[b].name32,
                                  d.shader_textures[b].texture);
                    sections_json += buf;
                    json_str(sections_json, tn ? tn : "");
                    sections_json += ']';
                }
                sections_json += ']';
            }
            sections_json += '}';
        }
        bf6_free(c, m);
    }
    if (first_section) error = "The configured item has no drawable geometry.";
    return finish();
}
