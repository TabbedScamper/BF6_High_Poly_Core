/* The equipment a LootSpawner drops, for every engine: the Unreal plugin's
 * loadout catalogue, attachment join and configured weapon assembly
 * (BF6HighPolyLoadoutDecode.cpp, BF6HighPolyLoadoutAttachments.cpp), moved
 * here so the Godot plugin offers the same choices and draws the same weapon,
 * and the posed soldier a PlayerSpawner, HQ, AI spawner or spawn point stands for.
 * See bf6_loadout_catalogue in bf6_core.h. */
#include "bf6_core.h"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
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

/* ---------------------------------------------------------------- sections
 *
 * One drawable section as both records carry it: game-space geometry and the
 * material the reader bound, serialized the same way for a weapon and a
 * soldier. */

/* THE BIND RIG, for an engine that means to pose the mesh itself.
 *
 * `local` is the bind pose relative to the parent - a rest pose - and `inverse`
 * is the bind model pose inverted, which is what a skin needs.
 *
 * `posed` is the SAME parent-relative slot with the sampled idle frame written
 * into it, which is what a skeleton's animatable pose is. It is here so that a
 * skinned soldier can be shown standing the way the static one already stands;
 * without it an engine can only draw the character in its bind stance, and
 * "looks wrong" would be indistinguishable from "skinned wrong".
 *
 * The POSED MODEL transforms are still deliberately absent. Those are the
 * flattened product of the whole chain, and an engine that has `local`,
 * `parent` and `posed` recomputes them itself; shipping them would invite
 * someone to use a composed frame as though it were a rest pose. */
struct BindBone {
    std::string name;
    int32_t parent = -1;
    float local[12]{};
    float inverse[12]{};
    float posed[12]{};
};

struct Sec {
    std::string mesh, bundle;
    uint64_t state_key = 0;
    int decal = 0;
    std::vector<float> pos, nrm, uv;
    std::vector<uint32_t> idx, colors;
    bool has_material = false;
    int alpha_test = 0, translucent = 0, alpha_from_albedo = 0, nsm = 0, terrain_decal_receiver = 0;
    float base_color[3] = {0, 0, 0};
    float roughness = 0;
    std::vector<std::pair<int, int>> textures;          /* slot, texture id */
    std::vector<std::pair<uint32_t, int>> shader_textures; /* name32, texture id */
    /* PER-VERTEX SKIN BINDING, kept only when the caller asks to skin the
     * soldier itself. `sbones` is already RESOLVED through the composed
     * skeleton, so a consumer never repeats the 0x8000 renderbone rule - that
     * rule is exactly the sort of thing two engines get subtly different.
     * When these are present `pos`/`nrm` are the BIND pose, not the posed
     * mesh, because a posed vertex cannot be re-posed. */
    int influences = 0;
    std::vector<uint16_t> sbones;
    std::vector<float>    sweights;
    /* The bone this section hangs off rigidly, or -1. Only the weapon uses it,
     * and only in skinned mode: the rifle has no skin binding of its own, so
     * without a bone to follow it would stay where one sampled frame put it
     * while the soldier holding it moved. */
    int attach_bone = -1;
    /* RENDERBONES ARE PER MESH, not per character.
     *
     * The base rig is shared - 291 bones on the soldier - but each mesh appends
     * its own renderbones on top, so a skin index at or above the base count
     * means "renderbone k OF THIS MESH". Exporting one composed rig for the
     * whole soldier looked right and was not: the test found indices up to 297
     * against a 291-bone rig. These are the appended ones for this section's
     * mesh, and they are few - under ten. */
    std::vector<BindBone> rbones;
};

/* Row-vector 4x3 affine matrices, the convention bf6_bone and the Unreal
 * plugin's FMatrix44f share: p' = p.x*R + p.y*U + p.z*F + T. */
struct M43 { float m[12]; };

M43 identity43()
{
    M43 r{};
    r.m[0] = r.m[4] = r.m[8] = 1;
    return r;
}

M43 from12(const float* p)
{
    M43 r;
    std::memcpy(r.m, p, sizeof(r.m));
    return r;
}

/* a then b: the Unreal plugin's A * B. */
M43 mul(const M43& a, const M43& b)
{
    M43 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 3; ++j) {
            double s = i == 3 ? b.m[9 + j] : 0.0;
            for (int k = 0; k < 3; ++k) s += (double)a.m[i * 3 + k] * b.m[k * 3 + j];
            r.m[i * 3 + j] = (float)s;
        }
    return r;
}

bool inverse43(const M43& a, M43& out)
{
    const double m00 = a.m[0], m01 = a.m[1], m02 = a.m[2], m10 = a.m[3], m11 = a.m[4], m12 = a.m[5],
                 m20 = a.m[6], m21 = a.m[7], m22 = a.m[8];
    const double det = m00 * (m11 * m22 - m12 * m21) - m01 * (m10 * m22 - m12 * m20) + m02 * (m10 * m21 - m11 * m20);
    if (std::fabs(det) < 1e-20) return false;
    const double inv[9] = {
        (m11 * m22 - m12 * m21) / det, -(m01 * m22 - m02 * m21) / det, (m01 * m12 - m02 * m11) / det,
        -(m10 * m22 - m12 * m20) / det, (m00 * m22 - m02 * m20) / det, -(m00 * m12 - m02 * m10) / det,
        (m10 * m21 - m11 * m20) / det, -(m00 * m21 - m01 * m20) / det, (m00 * m11 - m01 * m10) / det};
    for (int k = 0; k < 9; ++k) out.m[k] = (float)inv[k];
    for (int j = 0; j < 3; ++j)
        out.m[9 + j] = (float)-(a.m[9] * inv[0 + j] + a.m[10] * inv[3 + j] + a.m[11] * inv[6 + j]);
    return true;
}

/* The Unreal plugin's FQuatRotationTranslationMatrix44f rows. */
void quat_rows(float x, float y, float z, float w, float* m9)
{
    const float len = std::sqrt(x * x + y * y + z * z + w * w);
    if (len > 1e-8f) { x /= len; y /= len; z /= len; w /= len; }
    const float x2 = x + x, y2 = y + y, z2 = z + z;
    const float xx = x * x2, xy = x * y2, xz = x * z2, yy = y * y2, yz = y * z2, zz = z * z2;
    const float wx = w * x2, wy = w * y2, wz = w * z2;
    m9[0] = 1 - (yy + zz); m9[1] = xy + wz;       m9[2] = xz - wy;
    m9[3] = xy - wz;       m9[4] = 1 - (xx + zz); m9[5] = yz + wx;
    m9[6] = xz + wy;       m9[7] = yz - wx;       m9[8] = 1 - (xx + yy);
}

/* Place every section through a transform, normals through its inverse
 * transpose (the Unreal plugin's Transform()). */
void transform_sections(std::vector<Sec>& secs, const M43& t)
{
    double nm[9];
    const bool normals = normal_matrix(t.m, nm);
    for (Sec& s : secs) {
        for (size_t v = 0; v + 2 < s.pos.size(); v += 3) {
            float p[3];
            xform_point(t.m, &s.pos[v], p);
            std::memcpy(&s.pos[v], p, sizeof(p));
        }
        if (!normals) continue;
        for (size_t v = 0; v + 2 < s.nrm.size(); v += 3) {
            const float* n = &s.nrm[v];
            double o[3];
            for (int k = 0; k < 3; ++k) o[k] = n[0] * nm[k] + n[1] * nm[3 + k] + n[2] * nm[6 + k];
            const double len = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
            if (len > 1e-12) for (int k = 0; k < 3; ++k) s.nrm[v + k] = (float)(o[k] / len);
        }
    }
}
/* SOME of a weapon's sections, picked by mesh name. A weapon part that hangs off
 * its own bone has to be re-expressed in that bone's frame, and only those
 * sections may move: transforming the whole gun would take the receiver with it. */
void transform_sections_matching(std::vector<Sec>& secs, const M43& t,
                                const char* mesh_contains)
{
    std::vector<Sec> picked;
    std::vector<Sec*> from;
    for (Sec& s : secs)
        if (s.mesh.find(mesh_contains) != std::string::npos) from.push_back(&s);
    if (from.empty()) return;
    /* transform_sections owns the arithmetic, including the normal matrix, so it
     * is reused rather than copied: the picked sections are moved through it and
     * moved back, which keeps ONE implementation of the transform. */
    picked.reserve(from.size());
    for (Sec* s : from) picked.push_back(std::move(*s));
    transform_sections(picked, t);
    for (size_t i = 0; i < from.size(); ++i) *from[i] = std::move(picked[i]);
}


/* A mesh through the armory reader, skinned by a palette and optionally
 * placed, as sections. `rig_bones` resolves a flagged (0x8000) skin index to an
 * appended renderbone; a weapon skeleton has none. False with `error` set. */
bool read_sections(bf6_ctx* c, const std::string& mesh, const std::string& bundle, const std::string& variation,
                   const std::vector<M43>* skin, int rig_bones, std::vector<Sec>& out,
                   std::string& error, bool want_skin = false)
{
    const char* var = variation.empty() ? nullptr : variation.c_str();
    bf6_mesh* m = bf6_read_armory_mesh_scoped(c, mesh.c_str(), 0, bundle.empty() ? nullptr : bundle.c_str(), var);
    if (!m) { error = "no mesh at " + mesh; return false; }
    std::string scope = bundle;
    /* A scoped read that bound nothing in a bundle with no depot retries in the
     * mesh's own scope, keeping its variation, as the Unreal plugin's ReadMesh
     * does. */
    if (!bundle.empty() && !any_bound(m) && bf6_material_scope_exists(c, bundle.c_str()) != 1) {
        if (bf6_mesh* plain = bf6_read_armory_mesh_scoped(c, mesh.c_str(), 0, nullptr, var)) {
            if (any_bound(plain)) { bf6_free(c, m); m = plain; scope.clear(); }
            else bf6_free(c, plain);
        }
    }
    for (int si = 0; si < m->section_count; ++si) {
        const bf6_section& s = m->sections[si];
        if (s.vertex_count <= 0 || s.index_count <= 0 || !s.positions) continue;
        Sec o;
        o.mesh = mesh;
        o.bundle = scope;
        o.state_key = s.state_key;
        o.decal = s.is_decal;
        o.pos.assign(s.positions, s.positions + (size_t)s.vertex_count * 3);
        if (s.normals) o.nrm.assign(s.normals, s.normals + (size_t)s.vertex_count * 3);
        if (s.uv0) o.uv.assign(s.uv0, s.uv0 + (size_t)s.vertex_count * 2);
        o.idx.assign(s.indices, s.indices + (size_t)s.index_count);
        if (s.colors) o.colors.assign(s.colors, s.colors + (size_t)s.vertex_count);
        /* THE SKIN BINDING, when the caller means to pose the mesh itself.
         *
         * Captured BEFORE the CPU skinning below and resolved here, so no
         * consumer has to repeat the 0x8000 renderbone rule - a rule two
         * engines would eventually implement differently. Passing `skin` as
         * null is what leaves `pos`/`nrm` at the BIND pose, which is the only
         * pose a skinned mesh can be re-posed from.
         *
         * `bf6_skin_index_to_bone` is not used here because the caller has
         * already handed us `rig_bones`, which is the same resolution without
         * needing the skeleton handle to still be alive. */
        if (want_skin && m->mesh_type == 1 && s.skin_bones && s.skin_weights
            && s.skin_influences > 0) {
            const size_t lanes = (size_t)s.vertex_count * (size_t)s.skin_influences;
            o.influences = s.skin_influences;
            o.sbones.resize(lanes);
            o.sweights.assign(s.skin_weights, s.skin_weights + lanes);
            for (size_t k = 0; k < lanes; ++k) {
                const uint16_t raw = s.skin_bones[k];
                const int bone = (raw & 0x8000) ? rig_bones + ((raw & 0x7fff) >> 1) : raw;
                o.sbones[k] = (uint16_t)(bone < 0 ? 0 : bone);
            }
        }
        if (skin && !skin->empty() && m->mesh_type == 1 && s.skin_bones && s.skin_weights) {
            const int bones = (int)skin->size();
            for (int v = 0; v < s.vertex_count; ++v) {
                float P[3] = {0, 0, 0}, N[3] = {0, 0, 0};
                float accepted = 0.f;
                for (int lane = 0; lane < s.skin_influences; ++lane) {
                    const int at = v * s.skin_influences + lane;
                    const uint16_t raw = s.skin_bones[at];
                    const int bone = (raw & 0x8000) ? rig_bones + ((raw & 0x7fff) >> 1) : raw;
                    const float w = s.skin_weights[at];
                    if (w <= 0.f) continue;
                    if (bone < 0 || bone >= bones) {
                        bf6_free(c, m);
                        error = "preview skin references an unavailable bone";
                        out.clear();
                        return false;
                    }
                    float tp[3];
                    xform_point((*skin)[(size_t)bone].m, &o.pos[(size_t)v * 3], tp);
                    for (int k = 0; k < 3; ++k) P[k] += tp[k] * w;
                    if (!o.nrm.empty()) {
                        float tn[3];
                        xform_vector((*skin)[(size_t)bone].m, &o.nrm[(size_t)v * 3], tn);
                        for (int k = 0; k < 3; ++k) N[k] += tn[k] * w;
                    }
                    accepted += w;
                }
                if (accepted > 1e-8f) {
                    for (int k = 0; k < 3; ++k) o.pos[(size_t)v * 3 + k] = P[k] / accepted;
                    if (!o.nrm.empty()) {
                        const float len = std::sqrt(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
                        if (len > 1e-8f) for (int k = 0; k < 3; ++k) o.nrm[(size_t)v * 3 + k] = N[k] / len;
                    }
                }
            }
        }
        if (m->materials && s.material >= 0 && s.material < m->material_count) {
            const bf6_material_desc& d = m->materials[s.material];
            o.has_material = true;
            o.alpha_test = d.alpha_test;
            o.translucent = d.translucent;
            o.alpha_from_albedo = d.alpha_from_albedo;
            o.nsm = d.normal_is_nsm;
            o.terrain_decal_receiver = d.terrain_decal_receiver;
            for (int k = 0; k < 3; ++k) o.base_color[k] = d.base_color[k];
            o.roughness = d.roughness;
            for (int b = 0; b < d.texture_count; ++b) o.textures.push_back({(int)d.textures[b].slot, d.textures[b].texture});
            for (int b = 0; b < d.shader_texture_count; ++b)
                o.shader_textures.push_back({d.shader_textures[b].name32, d.shader_textures[b].texture});
        }
        out.push_back(std::move(o));
    }
    bf6_free(c, m);
    return true;
}

void place_tail(std::vector<Sec>& out, size_t from, const M43& t)
{
    std::vector<Sec> tail(std::make_move_iterator(out.begin() + (long)from), std::make_move_iterator(out.end()));
    out.erase(out.begin() + (long)from, out.end());
    transform_sections(tail, t);
    for (Sec& s : tail) out.push_back(std::move(s));
}

/* sections_json and the float body, in the record layout bf6_core.h
 * documents. */
void serialize(bf6_ctx* c, const std::vector<Sec>& secs, std::string& sections_json, std::vector<float>& body)
{
    char buf[256];
    bool first = true;
    for (const Sec& s : secs) {
        const int vc = (int)(s.pos.size() / 3), ic = (int)s.idx.size();
        if (!first) sections_json += ',';
        first = false;
        sections_json += "{\"mesh\":"; json_str(sections_json, s.mesh);
        sections_json += ",\"bundle\":"; json_str(sections_json, s.bundle);
        std::snprintf(buf, sizeof(buf), ",\"state_key\":\"%016llx\",\"vertex_count\":%d,\"index_count\":%d",
                      (unsigned long long)s.state_key, vc, ic);
        sections_json += buf;
        std::snprintf(buf, sizeof(buf), ",\"positions\":%zu", body.size());
        sections_json += buf;
        body.insert(body.end(), s.pos.begin(), s.pos.end());
        std::snprintf(buf, sizeof(buf), ",\"normals\":%lld", s.nrm.empty() ? -1LL : (long long)body.size());
        sections_json += buf;
        body.insert(body.end(), s.nrm.begin(), s.nrm.end());
        std::snprintf(buf, sizeof(buf), ",\"uvs\":%lld", s.uv.empty() ? -1LL : (long long)body.size());
        sections_json += buf;
        body.insert(body.end(), s.uv.begin(), s.uv.end());
        /* Indices and colours ride in the float body as exact bit patterns. */
        std::snprintf(buf, sizeof(buf), ",\"indices\":%zu", body.size());
        sections_json += buf;
        size_t at = body.size();
        body.resize(at + s.idx.size());
        if (!s.idx.empty()) std::memcpy(&body[at], s.idx.data(), s.idx.size() * sizeof(uint32_t));
        std::snprintf(buf, sizeof(buf), ",\"colors\":%lld,\"decal\":%d", s.colors.empty() ? -1LL : (long long)body.size(), s.decal);
        sections_json += buf;
        if (!s.colors.empty()) {
            at = body.size();
            body.resize(at + s.colors.size());
            std::memcpy(&body[at], s.colors.data(), s.colors.size() * sizeof(uint32_t));
        }
        /* THE SKIN BINDING, present only in skinned mode. `skin_bones` rides in
         * the float body as exact uint32 bit patterns, the same way indices and
         * colours already do, so the body stays one float array and a consumer
         * that ignores these fields is unaffected. The ids are ALREADY resolved
         * against the composed rig - no consumer repeats the 0x8000 rule. */
        std::snprintf(buf, sizeof(buf), ",\"influences\":%d,\"attach_bone\":%d",
                      s.influences, s.attach_bone);
        sections_json += buf;
        if (s.influences > 0 && !s.sbones.empty()) {
            std::snprintf(buf, sizeof(buf), ",\"skin_bones\":%zu", body.size());
            sections_json += buf;
            at = body.size();
            body.resize(at + s.sbones.size());
            for (size_t k = 0; k < s.sbones.size(); ++k) {
                const uint32_t v = s.sbones[k];
                std::memcpy(&body[at + k], &v, sizeof(uint32_t));
            }
            std::snprintf(buf, sizeof(buf), ",\"skin_weights\":%zu", body.size());
            sections_json += buf;
            body.insert(body.end(), s.sweights.begin(), s.sweights.end());
        } else {
            sections_json += ",\"skin_bones\":-1,\"skin_weights\":-1";
        }
        /* THIS MESH'S OWN APPENDED RENDERBONES. A skin index at or above
         * `rig_bones` addresses this list, so it belongs to the SECTION and not
         * to the record - exporting one composed rig for the whole soldier is
         * what produced indices past the end of it. */
        if (!s.rbones.empty()) {
            sections_json += ",\"renderbones\":[";
            for (size_t i = 0; i < s.rbones.size(); ++i) {
                const BindBone& b = s.rbones[i];
                if (i) sections_json += ',';
                sections_json += "{\"name\":";
                json_str(sections_json, b.name);
                std::snprintf(buf, sizeof(buf), ",\"parent\":%d,\"local\":[", b.parent);
                sections_json += buf;
                for (int k = 0; k < 12; ++k) {
                    std::snprintf(buf, sizeof(buf), "%s%.9g", k ? "," : "", b.local[k]);
                    sections_json += buf;
                }
                sections_json += "],\"inverse\":[";
                for (int k = 0; k < 12; ++k) {
                    std::snprintf(buf, sizeof(buf), "%s%.9g", k ? "," : "", b.inverse[k]);
                    sections_json += buf;
                }
                sections_json += "],\"posed\":[";
                for (int k = 0; k < 12; ++k) {
                    std::snprintf(buf, sizeof(buf), "%s%.9g", k ? "," : "", b.posed[k]);
                    sections_json += buf;
                }
                sections_json += "]}";
            }
            sections_json += ']';
        }
        if (s.has_material) {
            std::snprintf(buf, sizeof(buf),
                          ",\"alpha_test\":%d,\"translucent\":%d,\"alpha_from_albedo\":%d,\"nsm\":%d,"
                          "\"terrain_decal_receiver\":%d,\"base_color\":[%.9g,%.9g,%.9g],\"roughness\":%.9g,\"textures\":[",
                          s.alpha_test, s.translucent, s.alpha_from_albedo, s.nsm, s.terrain_decal_receiver,
                          s.base_color[0], s.base_color[1], s.base_color[2], s.roughness);
            sections_json += buf;
            for (size_t b = 0; b < s.textures.size(); ++b) {
                const char* tn = bf6_texture_name_at(c, s.textures[b].second);
                std::snprintf(buf, sizeof(buf), "%s[%d,%d,", b ? "," : "", s.textures[b].first, s.textures[b].second);
                sections_json += buf;
                json_str(sections_json, tn ? tn : "");
                sections_json += ']';
            }
            sections_json += "],\"shader_textures\":[";
            for (size_t b = 0; b < s.shader_textures.size(); ++b) {
                const char* tn = bf6_texture_name_at(c, s.shader_textures[b].second);
                std::snprintf(buf, sizeof(buf), "%s[%u,%d,", b ? "," : "", s.shader_textures[b].first, s.shader_textures[b].second);
                sections_json += buf;
                json_str(sections_json, tn ? tn : "");
                sections_json += ']';
            }
            sections_json += ']';
        }
        sections_json += '}';
    }
}

/* The 'BLWP' record around a JSON head and a float body. `magic` names the
 * record kind; the layout is the same for all of them, so a reader that knows
 * one knows them all. */
int64_t record(const std::string& head, const std::vector<float>& body, uint8_t** out,
               uint32_t magic = 0x50574C42u /* BLWP */)
{
    std::string j = head;
    while ((j.size() + 12) % 4) j.push_back(' ');
    const uint32_t hdr[3] = {magic, 1u, (uint32_t)j.size()};
    const size_t total = 12 + j.size() + body.size() * sizeof(float);
    *out = (uint8_t*)std::malloc(total);
    if (!*out) return -1;
    std::memcpy(*out, hdr, 12);
    std::memcpy(*out + 12, j.data(), j.size());
    if (!body.empty()) std::memcpy(*out + 12 + j.size(), body.data(), body.size() * sizeof(float));
    return (int64_t)total;
}

/* ---------------------------------------------------------------- the weapon */

/* THE WHOLE INSTALL, not just the front-end families.
 *
 * bf6_mount_frontend selects the shared UI/weapons/characters archive families.
 * That is not the whole install: bf6_open mounts Data/Win32 only, and this
 * header's own note on bf6_mount_all puts the shortfall exactly - "a weapon
 * folder that holds 250 assets holds 4 of them in the Data/Win32 mount - every
 * per-attachment record is in the Update packages."
 *
 * Measured on 2026-09-20: the M4A1's 1P animation folder answers with 7 assets
 * under the front-end mount and carries its reloads, its firing parts additives
 * and its whole inspect set under this one. Across all 1P animation assets it is
 * ~200 against 3285, and for context databases 165 against 877. Every "the game
 * does not ship that animation" conclusion this project has drawn came from the
 * narrow mount.
 *
 * include_levels stays 0: level archives are much slower to mount and nothing
 * here has needed them. First mount wins on a name collision, so this only ever
 * ADDS names - anything already resolving keeps resolving, unchanged. Measured
 * cost of the wider mount is about 70 ms, against a ~2.4 s walk-mode takeover. */
static bool mount_full(bf6_ctx* c, char* why, int why_len)
{
    if (!bf6_mount_frontend(c, why, why_len)) return false;
    char ignored[512] = {0};
    /* A failure here is not fatal: the front-end families are mounted and the
     * narrower answer is still a usable one. It must not turn a working loadout
     * into an error. */
    bf6_mount_all(c, 0, ignored, (int)sizeof(ignored));
    return true;
}

/* The configured weapon's sections and slot anchors: the factory fits with the
 * user's choices mixed in, the configured assembly, each part skinned by the
 * configured palette and placed by its attach transform. */
bool assemble_weapon(bf6_ctx* c, const char* item_id, const char* fits_text, const char* portal_enums,
                     std::vector<Sec>& secs, std::string& anchors_json, std::string& error)
{
    char why[512] = {0};
    if (!mount_full(c, why, sizeof(why))) { error = why[0] ? why : "The reader cannot mount front-end equipment."; return false; }
    const std::vector<std::string> ebx = names(c, "common/hardware/");
    const std::vector<Item> items = read_items(c, ebx);
    const Item* item = find_item(items, item_id);
    if (!item) { error = "This item is absent from the installed equipment catalogue."; return false; }
    const std::string md = model_definition(c, ebx, *item);
    if (md.empty()) { error = "This item has no unambiguous model definition."; return false; }
    const std::string equipment = exact_leaf(ebx, "equipment_" + leaf(item->id), item->asset);

    bf6_weapon_fit fits[64]{};
    int nfits = 0;
    std::vector<std::string> fit_strings;
    fit_strings.reserve(256);
    if (!equipment.empty()) nfits = bf6_weapon_factory_fits(c, equipment.c_str(), fits, 64);
    if (nfits < 0 || nfits > 64) { error = "The item's factory configuration is unreadable."; return false; }
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
        if (!ch) { error = "An attachment is unavailable for this weapon. Choose it again in Loadout."; return false; }
        bool replaced = false;
        for (auto& fp : fit_pairs) if (fp.first == slot) { fp.second = ch->bundle; replaced = true; break; }
        if (!replaced) {
            if (fit_pairs.size() == 64) { error = "Too many configured attachments."; return false; }
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
        return false;
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
    std::vector<M43> skin;
    for (int i = 0; i < bone_count; ++i) skin.push_back(from12(bones[(size_t)i].m));

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

    for (const Part& part : copy) {
        const size_t from = secs.size();
        if (!read_sections(c, part.mesh, part.bundle, std::string(), bone_count > 0 ? &skin : nullptr, 0, secs, error)) {
            secs.clear();
            return false;
        }
        if (part.has_attach) place_tail(secs, from, from12(part.attach));
    }
    if (secs.empty()) { error = "The configured item has no drawable geometry."; return false; }
    return true;
}

/* ---------------------------------------------------------------- the soldier */

struct Character { std::string id, label, root; };
struct Outfit { std::string character, id, label, bundle; };

std::vector<std::string> res_names(bf6_ctx* c, const char* query)
{
    std::vector<std::string> out;
    const int n = bf6_list_res(c, query, nullptr, 0);
    if (n < 1 || n > 1000000) return out;
    std::vector<bf6_asset> rows((size_t)n);
    const int got = std::clamp(bf6_list_res(c, query, rows.data(), n), 0, n);
    for (int i = 0; i < got; ++i) if (rows[(size_t)i].name) out.emplace_back(rows[(size_t)i].name);
    std::sort(out.begin(), out.end());
    return out;
}

/* The operators and their outfits: every pf_cha<name>_set_<nnn>_*_bundle_3p
 * whose character ships the multiplayer set_001 body (the Unreal plugin's
 * ReadCatalogue). */
void read_characters(const std::vector<std::string>& bundles, const std::set<std::string>& resources,
                     std::vector<Character>& characters, std::vector<Outfit>& outfits)
{
    std::set<std::string> seen_char, seen_outfit;
    for (const std::string& p : bundles) {
        if (p.find('/') != std::string::npos || !starts(p, "pf_cha") || !ends(p, "_bundle_3p")) continue;
        const std::string tail = p.substr(3);
        const size_t split = tail.find("_set_");
        if (split == std::string::npos) continue;
        const std::string ch = tail.substr(0, split), rest = tail.substr(split + 5);
        if (rest.size() < 4) continue;
        const std::string set = rest.substr(0, 3);
        const std::string root = "common/characters/mp/main/" + ch;
        if (!resources.count(root + "/set/set_001/" + ch + "_set_001_mesh")) continue;
        if (seen_char.insert(ch).second) {
            std::string label = ch.size() > 7 ? ch.substr(7) : ch;
            for (char& x : label) x = (char)std::toupper((unsigned char)x);
            characters.push_back({ch, label, root});
        }
        if (seen_outfit.insert(ch + "/" + set).second)
            outfits.push_back({ch, set, "Outfit " + set, "win32/" + p});
    }
    std::stable_sort(characters.begin(), characters.end(), [](const Character& a, const Character& b) { return a.label < b.label; });
}

std::string replace_all(std::string s, const std::string& from, const std::string& to)
{
    if (from.empty()) return s;
    size_t at = 0;
    while ((at = s.find(from, at)) != std::string::npos) { s.replace(at, from.size(), to); at += to.size(); }
    return s;
}

const char* const kRig = "animations/glacier/global/rigging/soldier_3p.rig";
const char* const kSkeleton = "common/characters/_soldier/ske_soldier_3p";
/* THE FIRST-PERSON PAIR. Not interchangeable with the third-person one: both
 * resolve the same NUMBER of channels for a 1P clip (129 either way), and 119
 * of those 129 land on a different bone. Pairing a 1P clip with the 3P rig
 * therefore produces a fully-bound and entirely wrong pose, which is why the
 * two travel together everywhere below rather than being picked separately. */
const char* const kRig1p = "animations/glacier/global/rigging/soldier_1p.rig";
const char* const kSkeleton1p = "common/characters/_soldier/ske_soldier_1p";

/* The neutral standing hold a first-person view rests in.
 *
 * THIS IS A POSE PLUS AN ADDITIVE, not a cycle, because that is how the game
 * authored it. There is no c_1p_rifle_stand_idle_01 to loop. Asking the install
 * what it carries under 1p_rifle_stand_idle returns seven names, and only these
 * two are the resting hold:
 *
 *   p_1p_rifle_stand_idle_01      1 frame,   128 keys - the rest POSE
 *   ladtv_1p_rifle_stand_idle_01  799 frames, 128 keys - the looping additive
 *                                 breathing sway, deltas about the pose
 *
 * The other five are turn-in-place transitions and a phase-aim yaw.
 *
 * It used to be t_1p_rifle_stand_idle_turn_inplace_left_01, sampled for its
 * first frame, because the pose would not bind and the additive would not bind.
 * Both bind now that the RAW channel permutation is read, so the stand-in is
 * gone - and with it the bug where the "idle" was 41 frames of the arms turning
 * left forever.
 *
 * The additive is recognisably additive in its values rather than only in its
 * name: its channel 0 at frame 0 is exactly (0,0,0,1), the identity rotation,
 * and its largest excursion over all 799 frames is 0.034 - about two degrees.
 * A cycle holds absolute poses and would do neither. */
const char* const k1pStandClip =
    "animations/glacier/assets/1p/common/rifle/loco/p_1p_rifle_stand_idle_01";
const char* const k1pStandAdditive =
    "animations/glacier/assets/1p/common/rifle/loco/ladtv_1p_rifle_stand_idle_01";
/* THE OTHER TWO STANCES, WHICH ARE JUST CLIPS.
 *
 * Crouch and prone looked like graph states - the install declares
 * ptm.stancestate.current.enumgs and its stand/crouch/prone items, and a
 * chooser above 1p.cameraposition.sf picks between three camera pose nodes. All
 * true, and none of it is needed: the poses themselves ship beside the stand
 * pose in the shared rifle folder, and reading one is the same work as reading
 * the other.
 *
 * NEITHER HAS A SWAY LAYER. Only stand has a ladtv_ sibling, so these two are
 * still poses and the caller gets a single frame. That is the honest answer -
 * a motionless crouch is what the install carries here - and it is the reason
 * the one-frame case has to be allowed through rather than refused. */
const char* const k1pCrouchClip =
    "animations/glacier/assets/1p/common/rifle/loco/p_1p_rifle_crouch_idle_01";
const char* const k1pProneClip =
    "animations/glacier/assets/1p/common/rifle/loco/p_1p_rifle_prone_idle_01";

struct Pose {
    std::vector<bf6_anim_binding> bindings;
    std::vector<float> channels;
    bf6_anim_binding_stats stats{};
};

/* The first authored sample of the role's front-end loadout idle: a static
 * editor stand-in needs one pose, not the whole clip. */
/* WHICH FRONT-END IDLE THE MOUNT ACTUALLY CARRIES for this role, with its
 * bindings read. Shared by the single-frame pose and the whole-clip export so
 * the two can never disagree about which clip a role means. */
std::string resolve_idle_clip(bf6_ctx* c, const std::string& role,
                              std::vector<bf6_anim_binding>& bindings,
                              bf6_anim_binding_stats& stats, std::string& error)
{
    /* _02 IS THE LOADOUT TAKE. EVERY ROLE, NO EXCEPTIONS.
     *
     * This used to take the first suffix that resolved, trying _01 first. The
     * front-end mount carries assault's _01 and not the other three, so assault
     * played the _01 take and the other three played _02 - a different
     * performance, of a different length (1139 frames against 565, 701 and
     * 701). Three of four soldiers stood differently from the fourth, and
     * nothing reported an error because each had found *a* clip.
     *
     * The inconsistency was real; preferring _01 was the wrong repair. The
     * menu viewer traced the actual controller chain rather than guessing at
     * filenames: the installed Class.Loadout.Node selects classloadout.cbd,
     * whose Collection.SoldierClass.EnumGS outcomes are the four
     * ui.loadout.idlecl.<class>.rc assets, and each of those RCs has the
     * matching idle[_cl]_<class>_02 clip as its repeated unit-weight base
     * entry. The similarly named _01 clips belong to a different (detail)
     * surface and are referenced by none of these controllers. So _02 is what
     * the loadout screen plays, and assault was the odd one out.
     *
     * A happy consequence: every _02 is already in the front-end mount, so
     * nothing has to be widened to find them.
     *
     * The idlebreak01..04 families are optional HALF-weight interruptions on
     * the same controllers, not alternatives to this base entry - they are the
     * next thing to add, not a fallback.
     *
     * THE "cl_" SPELLING FIRST - it is the one those RCs import. All four
     * (ui.loadout.idlecl.<class>.rc) name idle_cl_<class>_02 as the unit-weight
     * base entry and no controller references the plain idle_<class>_02 at all.
     * Trying the plain spelling first, on the belief that it was the loadout
     * screen's, gave spawners a clip the menu never plays: support leaning back
     * 45 degrees and recon 24 along the whole spine, for the whole clip, where
     * the cl_ takes stand upright. Found by rendering every class
     * (native/render_diag.gd pose:<class>). _01 remains a genuine last resort. */
    static const char* const kVariants[3] = { "_02", "_01", "_03" };
    const std::string base = "animations/glacier/assets/frontend/mainmenu/loadout/"
                             "ui_frontend_standing_idle_";
    const std::string stems[2] = { "cl_" + role, role };
    std::string clip_path;
    int count = 0;
    for (const std::string& stem : stems) {
        for (const char* suffix : kVariants) {
            const std::string candidate = base + stem + suffix;
            const int n = bf6_anim_bindings(c, candidate.c_str(), kRig, kSkeleton,
                                            nullptr, 0, &stats);
            if (n >= 1 && n <= 4096) { clip_path = candidate; count = n; break; }
        }
        if (count) break;
    }
    if (count < 1 || count > 4096) {
        error = "The selected soldier pose has no readable animation binding.";
        return std::string();
    }
    bindings.assign((size_t)count, bf6_anim_binding{});
    if (bf6_anim_bindings(c, clip_path.c_str(), kRig, kSkeleton,
                          bindings.data(), count, &stats) != count) {
        error = "The selected soldier pose has no readable animation binding.";
        return std::string();
    }
    return clip_path;
}

/* THE FIRST-PERSON STANCE FOR THIS WEAPON. The game's standing 1P pose is
 * 1p.locostance.slc, whose base is 1p.weaponpose.cdb: a lookup on the
 * weapon's animation identity (through the dual-wield cached states) that
 * lands on each weapon's own stance - the M4A1's is p_1p_oma_m4a1_stand_idle_01.
 * The generic rifle hold is only the fallback for a weapon the lookup cannot
 * place; it differs from the M4A1's own stance by up to 39 degrees on the
 * left hand, which is a hand that misses the weapon. */
std::string first_person_stance(bf6_ctx* c, const std::string& item,
                                const std::string& stance = "stand")
{
    /* CROUCH AND PRONE TAKE THE SHARED POSE, not a weapon-specific one.
     *
     * 1p.weaponpose.cdb resolves the weapon's own STAND hold - that is what it
     * is for, and the M4A1's folder carries exactly one pose clip, its stand
     * idle. Asking it for a crouch would return that stand hold and the soldier
     * would stand up while claiming to crouch, which is worse than using the
     * shared rifle crouch pose the install does carry. */
    if (stance == "crouch") return k1pCrouchClip;
    if (stance == "prone")  return k1pProneClip;
    if (!item.empty()) {
        int32_t sw = -1, wt = -1;
        char err[256] = {0};
        if (bf6_inspect_ids(c, item.c_str(), &sw, &wt, err, (int)sizeof(err))) {
            char path[512] = {0};
            bf6_ant_resolve_for_weapon(c, "animations/kingston/controllers/1p.weaponpose.cdb", sw, wt, path, (int)sizeof(path));
            if (path[0]) {
                bf6_anim_binding_stats st{};
                const int n = bf6_anim_bindings(c, path, kRig1p, kSkeleton1p, nullptr, 0, &st);
                if (n >= 1 && n <= 4096) return path;
            }
        }
    }
    return k1pStandClip;
}

bool pose_samples(bf6_ctx* c, const std::string& role, Pose& out, std::string& error,
                  bool first_person = false, const std::string& item = std::string())
{
    /* WHICH VARIANT THE ARCHIVE ACTUALLY CARRIES, not a hardcoded suffix.
     *
     * This asked for "_01" and nothing else, which made Engineer, Support and
     * Recon fail with "no readable animation binding" while Assault worked.
     * Nothing is wrong with those clips or with the decode: under a FULL mount
     * all four resolve identically, 505 bindings and 1,139 keys each. The
     * front-end mount this preview uses simply carries a different variant of
     * each -
     *
     *     ui_frontend_standing_idle_assault_01    present
     *     ui_frontend_standing_idle_engineer_02   present, and no _01
     *     ui_frontend_standing_idle_support_02    present, and no _01
     *     ui_frontend_standing_idle_recon_02      present, and no _01
     *
     * - so one fixed suffix could only ever satisfy one role. A static editor
     * stand-in wants ANY authored idle for the role, so take the first that
     * resolves and stop caring which number it is. The "cl_" spellings are the
     * same clips under the archive's other naming, tried last so the plain ones
     * win where both exist. */
    std::string clip_path;
    if (first_person) {
        /* ONE CLIP FOR EVERY ROLE. A first-person view is a pair of arms and a
         * weapon, and the arms do not know which class is wearing them - the
         * per-role poses are a property of the third-person loadout screen. */
        clip_path = first_person_stance(c, item);
        bf6_anim_binding_stats st{};
        const int n = bf6_anim_bindings(c, clip_path.c_str(), kRig1p, kSkeleton1p, nullptr, 0, &st);
        if (n < 1 || n > 4096) {
            error = "The first-person hold has no readable animation binding.";
            return false;
        }
        out.bindings.assign((size_t)n, bf6_anim_binding{});
        if (bf6_anim_bindings(c, clip_path.c_str(), kRig1p, kSkeleton1p,
                              out.bindings.data(), n, &out.stats) != n) {
            error = "The first-person hold has no readable animation binding.";
            return false;
        }
    } else {
        clip_path = resolve_idle_clip(c, role, out.bindings, out.stats, error);
    }
    if (clip_path.empty()) return false;
    bf6_anim_clip* clip = bf6_anim_clip_open(c, clip_path.c_str());
    if (!clip) { error = "The selected soldier pose is unavailable."; return false; }
    /* BINDINGS AND CHANNELS ARE NOT THE SAME COUNT, and requiring that they be
     * was a rule inferred from one clip family. Every front-end 3P idle has 505
     * of each, which looked like an invariant; the first-person stand clip has
     * 129 bindings against 128 channels and was refused outright with "the
     * selected soldier pose is unavailable".
     *
     * A binding names a channel; nothing says every binding finds one. So size
     * the sample buffer by the CLIP's channel count, and drop the bindings that
     * point outside it - then the completeness check below is still exact,
     * because the stats are rebuilt from what actually survived. */
    bool ok = clip->channel_count >= 1 && clip->key_time_count >= 1;
    if (ok) {
        out.channels.assign((size_t)clip->channel_count * 4, 0.f);
        ok = bf6_anim_clip_sample(c, clip, 0, out.channels.data(), nullptr) != 0;
    }
    if (ok) {
        std::vector<bf6_anim_binding> kept;
        kept.reserve(out.bindings.size());
        int quats = 0, vecs = 0;
        for (const bf6_anim_binding& b : out.bindings) {
            if (b.channel < 0 || (size_t)b.channel * 4 + 3 >= out.channels.size()) continue;
            if (b.component == BF6_ANIM_DOF_QUATERNION) ++quats;
            else if (b.component == BF6_ANIM_DOF_VECTOR3) ++vecs;
            kept.push_back(b);
        }
        out.bindings.swap(kept);
        out.stats.quaternion_bones = quats;
        out.stats.vector_bones = vecs;
        ok = !out.bindings.empty();
    }
    bf6_free(c, clip);
    if (!ok) error = "The selected soldier pose is unavailable.";
    return ok;
}

/* ------------------------------------------------ the weapon's own grip --
 *
 * A CLASS IDLE IS NOT HOLDING THIS GUN. The front-end loadout idle is authored
 * once per class, and its arms and weapon bones carry a generic hold. The
 * game puts the equipped weapon's own grip on top: an authored H-pose clip per
 * weapon (`hposes_<weapon>` under assets/3p/weapons/) whose arm and weapon DOFs
 * replace the idle's, and then a runtime IK pass that lands the left hand on
 * the weapon's left-hand marker. Without either, the arms stand in a hold made
 * for no weapon in particular - crossed forearms, a left hand in the air - and
 * which classes look worst depends only on how far their idle's generic hold
 * is from a rifle grip. Support and recon are the furthest.
 *
 * Both steps are ported from the research UI viewer that poses the four class
 * stances correctly (impl/bf6_ui_tool/source/viewer/bf6viewer.cpp:
 * SoldierHposeChannel, ResolveSoldierWeaponHpose, ApplySoldierWeaponHpose,
 * RotateSoldierModelBasis, SolveSoldierArmIk), which is the oracle here. */

/* Which DOFs the grip pose owns: the weapon's bones and both arms from the
 * shoulder down (SoldierHposeChannel, mask All). */
bool hpose_channel(const char* dof)
{
    if (!dof || !*dof) return false;
    std::string name(dof);
    if (name.size() >= 2 && name[name.size() - 2] == '.') name.resize(name.size() - 2);
    auto starts = [&](const char* p) { return name.rfind(p, 0) == 0; };
    return starts("Wep") ||
           starts("LeftShoulder") || starts("LeftArm") || starts("LeftForeArm") || starts("LeftHand") ||
           starts("RightShoulder") || starts("RightArm") || starts("RightForeArm") || starts("RightHand");
}

/* The weapon's grip clip: `hposes_<leaf, lowercase alphanumerics>` under
 * assets/3p/weapons/. The M2010's grip chooser names it m2010 where the
 * armory says m2010esr; that one alias is kept exact, as the viewer keeps it,
 * so no suffix-stripping guess binds a different weapon's grip. */
std::string resolve_weapon_hpose(bf6_ctx* c, const std::string& item)
{
    const size_t slash = item.find_last_of("/\\");
    const std::string bare = slash == std::string::npos ? item : item.substr(slash + 1);
    std::string norm;
    for (unsigned char ch : bare) if (std::isalnum(ch)) norm.push_back((char)std::tolower(ch));
    if (norm.empty()) return std::string();
    std::vector<std::string> leaves{ "hposes_" + norm };
    if (norm == "m2010esr") leaves.push_back("hposes_m2010");
    for (const std::string& leaf : leaves) {
        const int n = bf6_list_ebx(c, leaf.c_str(), nullptr, 0);
        if (n <= 0) continue;
        std::vector<bf6_asset> rows((size_t)n);
        const int got = bf6_list_ebx(c, leaf.c_str(), rows.data(), n);
        for (int i = 0; i < got; ++i) {
            if (!rows[(size_t)i].name) continue;
            const std::string path = rows[(size_t)i].name;
            std::string lower = path;
            for (char& ch : lower) ch = (char)std::tolower((unsigned char)ch);
            const size_t at = lower.find_last_of("/\\");
            const std::string cand = at == std::string::npos ? lower : lower.substr(at + 1);
            if (cand == leaf && lower.find("/assets/3p/weapons/") != std::string::npos) return path;
        }
    }
    return std::string();
}

/* Overlay the grip pose's frame `pose_index` (1 by default, as the viewer
 * plays it) onto every frame of `channels`, DOF by DOF: a destination
 * binding the grip owns takes the grip clip's value for the SAME DOF name and
 * component. Scalars are never touched. Returns how many DOFs were replaced;
 * 0 means there is no grip for this weapon, which is reported, not fatal. */
struct GripOverride { int channel; float v[4]; };

std::vector<GripOverride> weapon_hpose_overrides(bf6_ctx* c, const std::string& item,
                                                 const std::vector<bf6_anim_binding>& bindings,
                                                 int channel_count, std::string* used = nullptr,
                                                 int pose_index = 1)
{
    std::vector<GripOverride> out;
    const std::string path = resolve_weapon_hpose(c, item);
    if (path.empty() || channel_count < 1) return out;
    bf6_anim_binding_stats st{};
    const int n = bf6_anim_bindings(c, path.c_str(), kRig, kSkeleton, nullptr, 0, &st);
    if (n < 1 || n > 4096) return out;
    std::vector<bf6_anim_binding> src((size_t)n);
    if (bf6_anim_bindings(c, path.c_str(), kRig, kSkeleton, src.data(), n, &st) != n) return out;
    bf6_anim_clip* clip = bf6_anim_clip_open(c, path.c_str());
    if (!clip) return out;
    if (clip->key_time_count > 1 && clip->channel_count > 0) {
        const int f = std::max(0, std::min(pose_index, clip->key_time_count - 1));
        std::vector<float> grip((size_t)clip->channel_count * 4, 0.f);
        if (bf6_anim_clip_sample(c, clip, f, grip.data(), nullptr)) {
            std::map<std::pair<std::string, int>, const bf6_anim_binding*> by_dof;
            for (const bf6_anim_binding& b : src) by_dof.emplace(std::make_pair(std::string(b.dof_name), b.component), &b);
            for (const bf6_anim_binding& d : bindings) {
                if (d.component == BF6_ANIM_DOF_SCALAR || !hpose_channel(d.dof_name)) continue;
                auto it = by_dof.find(std::make_pair(std::string(d.dof_name), d.component));
                if (it == by_dof.end()) continue;
                const bf6_anim_binding& s = *it->second;
                if (d.channel < 0 || d.channel >= channel_count || s.channel < 0 || s.channel >= clip->channel_count) continue;
                GripOverride g{ d.channel, {} };
                std::memcpy(g.v, grip.data() + (size_t)s.channel * 4, sizeof(g.v));
                out.push_back(g);
            }
        }
    }
    bf6_free(c, clip);
    if (!out.empty() && used) *used = path;
    static const bool trace = std::getenv("BF6_POSE_TRACE") != nullptr;
    if (trace) std::fprintf(stderr, "grip pose: %s -> %s, %zu DOF(s) replaced\n", item.c_str(), path.c_str(), out.size());
    return out;
}

void apply_grip(const std::vector<GripOverride>& g, float* channels, int channel_count)
{
    for (const GripOverride& o : g)
        if (o.channel >= 0 && o.channel < channel_count)
            std::memcpy(channels + (size_t)o.channel * 4, o.v, sizeof(o.v));
}

int apply_weapon_hpose(bf6_ctx* c, const std::string& item, const std::vector<bf6_anim_binding>& bindings,
                       std::vector<float>& channels, int channel_count, int frames)
{
    const std::vector<GripOverride> g = weapon_hpose_overrides(c, item, bindings, channel_count);
    for (int fr = 0; fr < frames; ++fr) apply_grip(g, channels.data() + (size_t)fr * channel_count * 4, channel_count);
    return (int)g.size();
}

/* Turn a model-space basis so `from` points along `to`, keeping its position
 * (RotateSoldierModelBasis). Row-vector convention, as mul() is. */
bool rotate_model_basis(M43& m, const float from_in[3], const float to_in[3])
{
    auto len = [](const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
    float from[3] = { from_in[0], from_in[1], from_in[2] }, to[3] = { to_in[0], to_in[1], to_in[2] };
    const float lf = len(from), lt = len(to);
    if (lf < 1e-12f || lt < 1e-12f) return false;
    for (int k = 0; k < 3; ++k) { from[k] /= lf; to[k] /= lt; }
    float dot = from[0] * to[0] + from[1] * to[1] + from[2] * to[2];
    if (!std::isfinite(dot)) return false;
    dot = std::max(-1.f, std::min(dot, 1.f));
    float axis[3] = { from[1] * to[2] - from[2] * to[1], from[2] * to[0] - from[0] * to[2], from[0] * to[1] - from[1] * to[0] };
    const float al = len(axis);
    if (al < 1e-6f) {
        if (dot > 0.999999f) return true;
        const float fb[3] = { std::fabs(from[0]) < 0.8f ? 1.f : 0.f, std::fabs(from[0]) < 0.8f ? 0.f : 1.f, 0.f };
        axis[0] = from[1] * fb[2] - from[2] * fb[1];
        axis[1] = from[2] * fb[0] - from[0] * fb[2];
        axis[2] = from[0] * fb[1] - from[1] * fb[0];
        const float l2 = len(axis);
        if (l2 < 1e-12f) return false;
        for (int k = 0; k < 3; ++k) axis[k] /= l2;
    } else {
        for (int k = 0; k < 3; ++k) axis[k] /= al;
    }
    /* XMMatrixRotationAxis, a row-vector matrix: the transpose of the
     * column-vector Rodrigues rotation. */
    const float ang = std::acos(dot), cs = std::cos(ang), sn = std::sin(ang), t = 1.f - cs;
    const float x = axis[0], y = axis[1], z = axis[2];
    M43 r = identity43();
    r.m[0] = t * x * x + cs;     r.m[1] = t * x * y + sn * z; r.m[2] = t * x * z - sn * y;
    r.m[3] = t * x * y - sn * z; r.m[4] = t * y * y + cs;     r.m[5] = t * y * z + sn * x;
    r.m[6] = t * x * z + sn * y; r.m[7] = t * y * z - sn * x; r.m[8] = t * z * z + cs;
    const float px = m.m[9], py = m.m[10], pz = m.m[11];
    m = mul(m, r);
    m.m[9] = px; m.m[10] = py; m.m[11] = pz;
    return true;
}

/* Recompose every bone after `from` from its local and its parent's model. */
bool recompose_after(const bf6_skeleton* s, int from, const std::vector<M43>& local, std::vector<M43>& model)
{
    for (int b = from + 1; b < s->bone_count; ++b) {
        const int p = s->bones[b].parent;
        if (p < 0 || p >= b) return false;
        model[(size_t)b] = mul(local[(size_t)b], model[(size_t)p]);
    }
    return true;
}

/* Two-bone arm IK to a hand target (SolveSoldierArmIk): the elbow stays on
 * the side it was bending to, the reach is clamped to what this outfit's
 * limb lengths can do, and the hand takes the target's orientation. */
bool solve_arm_ik(const bf6_skeleton* s, int arm, int fore, int hand, const M43& target,
                  const std::vector<M43>& local, std::vector<M43>& model)
{
    if (!s || arm < 0 || fore < 0 || hand < 0 ||
        s->bones[fore].parent != arm || s->bones[hand].parent != fore) return false;
    auto P = [&](int b, float* o) { o[0] = model[(size_t)b].m[9]; o[1] = model[(size_t)b].m[10]; o[2] = model[(size_t)b].m[11]; };
    auto sub = [](const float* a, const float* b, float* o) { for (int k = 0; k < 3; ++k) o[k] = a[k] - b[k]; };
    auto len = [](const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
    auto cross = [](const float* a, const float* b, float* o) {
        o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; };
    float sh[3], el[3], wr[3], rq[3] = { target.m[9], target.m[10], target.m[11] };
    P(arm, sh); P(fore, el); P(hand, wr);
    float u[3], l[3], reach[3];
    sub(el, sh, u); sub(wr, el, l); sub(rq, sh, reach);
    const float ul = len(u), ll = len(l), rl = len(reach);
    if (ul < 1e-5f || ll < 1e-5f || rl < 1e-5f) return false;
    float dir[3] = { reach[0] / rl, reach[1] / rl, reach[2] / rl };
    const float minr = std::fabs(ul - ll) + 1e-5f, maxr = ul + ll - 1e-5f;
    const float sr = std::max(minr, std::min(rl, maxr));
    const float st[3] = { sh[0] + dir[0] * sr, sh[1] + dir[1] * sr, sh[2] + dir[2] * sr };
    float nrm[3];
    cross(u, l, nrm);
    if (len(nrm) < 1e-5f) { const float up[3] = { 0, 1, 0 }; cross(dir, up, nrm); }
    if (len(nrm) < 1e-5f) { const float xa[3] = { 1, 0, 0 }; cross(dir, xa, nrm); }
    { const float nl = len(nrm); for (int k = 0; k < 3; ++k) nrm[k] /= nl; }
    float bend[3];
    cross(nrm, dir, bend);
    { const float bl = len(bend); if (bl < 1e-12f) return false; for (int k = 0; k < 3; ++k) bend[k] /= bl; }
    const float along = (ul * ul + sr * sr - ll * ll) / (2.f * sr);
    const float perp = std::sqrt(std::max(0.f, ul * ul - along * along));
    float ea[3], eb[3];
    for (int k = 0; k < 3; ++k) { ea[k] = sh[k] + dir[k] * along + bend[k] * perp; eb[k] = sh[k] + dir[k] * along - bend[k] * perp; }
    auto d2 = [](const float* a, const float* b) { float s2 = 0; for (int k = 0; k < 3; ++k) s2 += (a[k] - b[k]) * (a[k] - b[k]); return s2; };
    const float* se = d2(ea, el) <= d2(eb, el) ? ea : eb;
    float from[3], to[3];
    sub(el, sh, from); sub(se, sh, to);
    if (!rotate_model_basis(model[(size_t)arm], from, to)) return false;
    if (!recompose_after(s, arm, local, model)) return false;
    float me[3], mw[3];
    P(fore, me); P(hand, mw);
    sub(mw, me, from); sub(st, me, to);
    if (!rotate_model_basis(model[(size_t)fore], from, to)) return false;
    if (!recompose_after(s, fore, local, model)) return false;
    /* The marker carries the wrist's orientation as well as its position; a
     * clamped position is kept only when the marker is out of reach. */
    model[(size_t)hand] = target;
    if (rl > maxr || rl < minr) { model[(size_t)hand].m[9] = st[0]; model[(size_t)hand].m[10] = st[1]; model[(size_t)hand].m[11] = st[2]; }
    return recompose_after(s, hand, local, model);
}

/* THE LEFT HAND GOES TO THE WEAPON. The clip carries the weapon's hand targets
 * in weapon space under Wep_Align; the game's runtime IK moves that branch so
 * Wep_IK_RightHand sits on the drawn right hand, and pulls the left arm onto
 * Wep_IK_LeftHand. The viewer reconstructs exactly that (bf6viewer.cpp
 * 4250-4330): solvedAlign = inverse(rightIkRelative) * rightHand, then a
 * two-bone solve of the left arm to leftIkRelative * solvedAlign. The right
 * arm is left as authored.
 *
 * `model` must be composed from `local` on entry. On success both are
 * updated: `model` holds the solved arm and `local` is re-derived from it, so
 * a caller that exports locals hands out the same pose it skins. Returns
 * false (and changes nothing observable) when the rig lacks any of the bones. */
bool solve_left_arm_to_weapon(const bf6_skeleton* s, std::vector<M43>& local, std::vector<M43>& model)
{
    auto find = [&](const char* nm) {
        for (int i = 0; i < s->bone_count; ++i) if (s->bones[i].name && std::strcmp(s->bones[i].name, nm) == 0) return i;
        return -1;
    };
    const int wa = find("Wep_Align"), ikr = find("Wep_IK_RightHand"), ikl = find("Wep_IK_LeftHand");
    const int rh = find("RightHand"), la = find("LeftArm"), lf = find("LeftForeArm"), lh = find("LeftHand");
    M43 inv_wa, inv_right_rel;
    if (wa < 0 || ikr < 0 || ikl < 0 || rh < 0 || la < 0 || lf < 0 || lh < 0) return false;
    if (!inverse43(model[(size_t)wa], inv_wa)) return false;
    const M43 right_rel = mul(model[(size_t)ikr], inv_wa);
    if (!inverse43(right_rel, inv_right_rel)) return false;
    const M43 solved_align = mul(inv_right_rel, model[(size_t)rh]);
    const M43 left_target = mul(mul(model[(size_t)ikl], inv_wa), solved_align);
    auto dist = [&](const M43& a) {
        const float dx = a.m[9] - left_target.m[9], dy = a.m[10] - left_target.m[10], dz = a.m[11] - left_target.m[11];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    const float before = dist(model[(size_t)lh]);
    const std::vector<M43> keep = model;
    if (!solve_arm_ik(s, la, lf, lh, left_target, local, model)) { model = keep; return false; }
    /* The viewer's own diagnostic: how far the left hand was from its weapon
     * marker, and how far it is after the solve. */
    static const bool trace = std::getenv("BF6_POSE_TRACE") != nullptr;
    if (trace) std::fprintf(stderr, "left-hand IK: %.4f m from the weapon's marker before, %.4f m after\n",
                            before, dist(model[(size_t)lh]));
    for (int i = 0; i < s->bone_count; ++i) {
        const int p = s->bones[i].parent;
        M43 inv_p;
        if (p < 0) local[(size_t)i] = model[(size_t)i];
        else if (inverse43(model[(size_t)p], inv_p)) local[(size_t)i] = mul(model[(size_t)i], inv_p);
    }
    return true;
}

/* The posed skinning palette for one character mesh, the weapon frame in the
 * hand, and the point between the feet. `bind` is filled when asked for. */
bool skin_for(bf6_ctx* c, const std::set<std::string>& ebx, const std::string& mesh, const Pose& pose,
              std::vector<M43>& skin, int& rig_bones, M43& weapon, float foot[3], std::string& error,
              std::vector<BindBone>* bind = nullptr, M43* weapon_local = nullptr,
              int* weapon_bone = nullptr, float* eye_y = nullptr,
              bool first_person = false, float* view_origin = nullptr,
              M43* mag_local = nullptr, M43* mag2_local = nullptr)
{
    std::string renderbones;
    /* Prefer the mesh asset's own Renderbones import; some outfits reuse another rig. */
    std::string mesh_asset = mesh;
    if (ends(mesh_asset, "_mesh")) mesh_asset.resize(mesh_asset.size() - 5);
    bf6_ebx_instance_import rows[32]{};
    const int n = bf6_armory_ebx_instance_imports(c, mesh_asset.c_str(), 0, rows, 32);
    for (int i = 0; i < std::min(n, 32); ++i)
        if (rows[i].field_hash == 0xA38BC860 && rows[i].path[0]) renderbones = rows[i].path;
    if (ends(renderbones, ".ebx")) renderbones.resize(renderbones.size() - 4);
    if (renderbones.empty() && ebx.count(mesh_asset + "_renderbonesdata")) renderbones = mesh_asset + "_renderbonesdata";
    /* The skeleton must match the rig the pose was bound against, or every
     * channel lands on a plausible wrong bone. */
    bf6_skeleton* s = bf6_skeleton_compose(c, first_person ? kSkeleton1p : kSkeleton,
                                           renderbones.empty() ? nullptr : renderbones.c_str());
    if (!s) { error = "The character render skeleton is unavailable."; return false; }
    bool ok = s->bone_count >= 1 && s->bone_count <= 8192;
    std::vector<M43> local, model;
    if (ok) {
        rig_bones = s->rig_bone_count;
        if (bind) {
            /* Read from the skeleton BEFORE the pose is applied to `local`
             * below - after that loop `local` is the posed rest, not the bind
             * rest, and the difference is invisible until something animates. */
            bind->resize((size_t)s->bone_count);
            for (int i = 0; i < s->bone_count; ++i) {
                BindBone& b = (*bind)[(size_t)i];
                b.name = s->bones[i].name ? s->bones[i].name : "";
                b.parent = s->bones[i].parent;
                for (int k = 0; k < 12; ++k) {
                    b.local[k] = s->bones[i].local[k];
                    b.inverse[k] = s->bones[i].inverse[k];
                }
            }
        }
        local.resize((size_t)s->bone_count);
        model.resize((size_t)s->bone_count);
        skin.assign((size_t)s->bone_count, identity43());
        for (int i = 0; i < s->bone_count; ++i) local[(size_t)i] = from12(s->bones[i].local);
        int applied = 0;
        for (const bf6_anim_binding& b : pose.bindings) {
            if (b.bone < 0 || b.bone >= s->bone_count) continue;
            if (b.channel < 0 || (size_t)b.channel * 4 + 3 >= pose.channels.size()) { ok = false; break; }
            const float* v = pose.channels.data() + (size_t)b.channel * 4;
            if (b.component == BF6_ANIM_DOF_QUATERNION) {
                quat_rows(v[0], v[1], v[2], v[3], local[(size_t)b.bone].m);
                ++applied;
            } else if (b.component == BF6_ANIM_DOF_VECTOR3) {
                for (int k = 0; k < 3; ++k) local[(size_t)b.bone].m[9 + k] = v[k];
                ++applied;
            }
        }
        if (ok && applied != pose.stats.quaternion_bones + pose.stats.vector_bones) {
            error = "The soldier pose did not bind completely.";
            ok = false;
        }
        /* `local` is now the SAMPLED frame, still parent-relative. Copying it
         * here rather than earlier is the whole point: a bone the clip did not
         * bind keeps its bind value, so `posed` is a complete pose for every
         * bone and a consumer never has to know which ones the clip touched. */
        if (ok && bind)
            for (int i = 0; i < s->bone_count; ++i)
                for (int k = 0; k < 12; ++k)
                    (*bind)[(size_t)i].posed[k] = local[(size_t)i].m[k];
    }
    int right_hand = -1, right_grip = -1, wep_align = -1, feet = 0;
    int mag_align = -1;   /* the magazine bone, so its geometry can be re-framed */
    int mag2_align = -1;  /* and the second one, which the reload brings in */
    float eye_l = 0.f, eye_r = 0.f, head_y = 0.f;
    bool have_eye_l = false, have_eye_r = false, have_head = false;
    bool have_weapon = false;
    foot[0] = foot[1] = foot[2] = 0;
    for (int i = 0; ok && i < s->bone_count; ++i) {
        const int parent = s->bones[i].parent;
        if (parent >= i) { ok = false; break; }
        model[(size_t)i] = parent < 0 ? local[(size_t)i] : mul(local[(size_t)i], model[(size_t)parent]);
        skin[(size_t)i] = mul(from12(s->bones[i].inverse), model[(size_t)i]);
        const char* nm = s->bones[i].name;
        if (!nm) continue;
        if (std::strcmp(nm, "Wep_Align") == 0) { weapon = model[(size_t)i]; wep_align = i; have_weapon = true; }
        if (std::strcmp(nm, "Wep_MGZ_Mag1") == 0) mag_align = i;
        if (std::strcmp(nm, "Wep_MGZ_Mag2") == 0) mag2_align = i;
        if (std::strcmp(nm, "RightHand") == 0) right_hand = i;
        /* WHERE THE SOLDIER'S EYES ARE, at the pose actually shown.
         *
         * NOT CameraJoint, which was the first answer and a wrong one. At bind
         * pose it sits at 1.6938 and looks perfect; under the front-end idle it
         * reported a 2.20 m eye, because it is a CAMERA TRACK and the menu
         * animates it to wherever the menu camera belongs. Anatomy does not
         * move like that, so anatomy is what gets measured: the eye joints
         * themselves, averaged, and the head only if a rig lacks them. */
        if (eye_y) {
            if (std::strcmp(nm, "LeftEye") == 0) { eye_l = model[(size_t)i].m[10]; have_eye_l = true; }
            else if (std::strcmp(nm, "RightEye") == 0) { eye_r = model[(size_t)i].m[10]; have_eye_r = true; }
            else if (std::strcmp(nm, "Head") == 0) { head_y = model[(size_t)i].m[10]; have_head = true; }
        }
        /* THE VIEW POINT, for a first-person build. Here CameraJoint is exactly
         * right and the eye joints are not: this rig has no face, and the whole
         * point of the 1P skeleton is that its camera chain IS the view. The
         * arms get anchored to it so an engine can parent the record straight
         * onto its camera at identity. */
        if (view_origin && std::strcmp(nm, "CameraJoint") == 0)
            for (int k = 0; k < 3; ++k) view_origin[k] = model[(size_t)i].m[9 + k];
        if (std::strcmp(nm, "Wep_IK_RightHand") == 0) right_grip = i;
        if (std::strcmp(nm, "LeftFoot") == 0 || std::strcmp(nm, "RightFoot") == 0) {
            for (int k = 0; k < 3; ++k) foot[k] += model[(size_t)i].m[9 + k];
            ++feet;
        }
    }
    if (ok && feet) for (int k = 0; k < 3; ++k) foot[k] /= (float)feet;
    /* The left hand to the weapon (solve_left_arm_to_weapon). Third person
     * only: the reference viewer solves the front-end 3P idles, and whether the
     * first-person holds want the same pass is not established. The palette
     * and the exported local pose follow the solved arm, so the static and the
     * skinned soldier agree. */
    if (ok && !first_person && solve_left_arm_to_weapon(s, local, model)) {
        for (int i = 0; i < s->bone_count; ++i) {
            skin[(size_t)i] = mul(from12(s->bones[i].inverse), model[(size_t)i]);
            if (bind && i < (int)bind->size())
                for (int k = 0; k < 12; ++k) (*bind)[(size_t)i].posed[k] = local[(size_t)i].m[k];
        }
    }
    if (ok && eye_y) {
        if (have_eye_l && have_eye_r) *eye_y = (eye_l + eye_r) * 0.5f;
        else if (have_eye_l) *eye_y = eye_l;
        else if (have_eye_r) *eye_y = eye_r;
        else if (have_head) *eye_y = head_y;
    }
    if (ok) {
        if (first_person && have_weapon && wep_align >= 0) {
            /* FIRST PERSON HOLDS THE WEAPON ON ITS OWN WEAPON BONE.
             *
             * The gun's geometry is authored in Wep_Align's frame - that is
             * what `weapon = model[Wep_Align]` above means - and the 1P clip
             * DRIVES Wep_Align, along with Wep_Root and both hands. So the
             * authored hold already agrees about where all three are, and the
             * weapon needs no re-anchoring at all: ride Wep_Align at identity
             * and the hands arrive on the gun because the animator put them
             * there.
             *
             * Re-expressing it in the WRIST's frame, as third person must,
             * throws that away. The rifle then follows the right hand rigidly
             * and stops responding to the left one, because the left hand is
             * gripping a weapon whose position is no longer being taken from
             * the clip that posed it. */
            if (weapon_local) *weapon_local = identity43();
            if (weapon_bone) *weapon_bone = wep_align;
            /* THE MAGAZINE, WHICH RIDES A DIFFERENT BONE.
             *
             * The gun's geometry is authored in Wep_Align's frame, which is why
             * hanging it off Wep_Align at identity is right. The magazine hangs
             * off Wep_MGZ_Mag1 instead so a reload can take it out, and geometry
             * in one bone's frame attached to ANOTHER bone is simply in the wrong
             * place - measured as the magazine sitting off the weapon. So it is
             * re-expressed in the magazine bone's frame, exactly as the third
             * person path re-expresses the whole gun in the wrist's.
             *
             * THE BIND MATRICES, NOT THE POSED ONES. This first used model[], the
             * SAMPLED pose, and the magazine came out 162 mm off. An attachment
             * applies its bone's own transform - rest at build, animated after - so
             * the frame change has to be the one that holds at REST, or it is only
             * right for the single frame the palette happened to be sampled at.
             *
             * bones[].inverse IS the inverse bind matrix, so the magazine's is
             * already the inverse this needs and only Wep_Align's has to be undone.
             */
            if (wep_align >= 0) {
                M43 align_bind;
                if (inverse43(from12(s->bones[(size_t)wep_align].inverse), align_bind)) {
                    if (mag_local && mag_align >= 0)
                        *mag_local = mul(align_bind, from12(s->bones[(size_t)mag_align].inverse));
                    if (mag2_local && mag2_align >= 0)
                        *mag2_local = mul(align_bind, from12(s->bones[(size_t)mag2_align].inverse));
                }
            }
        } else if (have_weapon && right_hand >= 0 && right_grip >= 0) {
            /* Front-end clips carry a separate weapon IK target whose offset is
             * not applied to the rendered arm: join the authored grip frame to
             * the wrist actually drawn, or every rifle floats above the hands. */
            M43 grip_inv;
            if (inverse43(model[(size_t)right_grip], grip_inv)) {
                /* THE GRIP IN THE HAND'S OWN FRAME, before the hand's model
                 * pose is composed back on. An engine that animates attaches
                 * the weapon to RightHand with exactly this as its local
                 * transform, and the rifle then rides the hand through the
                 * clip instead of hanging in the air where one sampled frame
                 * happened to leave it. */
                if (weapon_local) *weapon_local = mul(weapon, grip_inv);
                if (weapon_bone) *weapon_bone = right_hand;
                weapon = mul(mul(weapon, grip_inv), model[(size_t)right_hand]);
            }
        } else {
            error = "The pose has no complete weapon-to-hand binding.";
            ok = false;
        }
    }
    bf6_free(c, s);
    return ok;
}

std::string field(const bf6json::Value& v, const char* key, const char* fallback)
{
    const bf6json::Value* f = v.find(key);
    return f && f->is_str() && !f->str.empty() ? f->str : std::string(fallback);
}

} // namespace

extern "C" int64_t bf6_loadout_catalogue(bf6_ctx* c, uint8_t** out)
{
    if (!c || !out) return -1;
    char why[512] = {0};
    if (!mount_full(c, why, sizeof(why))) return -1;
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
    /* The soldier choices: operators, their outfits, and the fixed faction and
     * role lists the Unreal plugin offers. */
    const std::vector<std::string> res = res_names(c, "common/characters/");
    const std::set<std::string> resources(res.begin(), res.end());
    std::vector<Character> characters;
    std::vector<Outfit> outfits;
    read_characters(names(c, "pf_cha"), resources, characters, outfits);
    j += "],\"characters\":[";
    for (size_t i = 0; i < characters.size(); ++i) {
        if (i) j += ',';
        j += "{\"id\":"; json_str(j, characters[i].id);
        j += ",\"label\":"; json_str(j, characters[i].label);
        j += '}';
    }
    j += "],\"outfits\":[";
    for (size_t i = 0; i < outfits.size(); ++i) {
        if (i) j += ',';
        j += "{\"character\":"; json_str(j, outfits[i].character);
        j += ",\"id\":"; json_str(j, outfits[i].id);
        j += ",\"label\":"; json_str(j, outfits[i].label);
        j += '}';
    }
    j += "],\"factions\":[{\"id\":\"alliance\",\"label\":\"NATO\"},{\"id\":\"pax\",\"label\":\"PAX\"}]";
    j += ",\"roles\":[{\"id\":\"assault\",\"label\":\"Assault\"},{\"id\":\"engineer\",\"label\":\"Engineer\"},"
         "{\"id\":\"support\",\"label\":\"Support\"},{\"id\":\"recon\",\"label\":\"Recon\"}]}";
    return give(j, out);
}

extern "C" int64_t bf6_loadout_attachments(bf6_ctx* c, const char* item_id, const char* portal_enums, uint8_t** out)
{
    if (!c || !item_id || !out) return -1;
    char why[512] = {0};
    if (!mount_full(c, why, sizeof(why))) return -1;
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
    std::vector<Sec> secs;
    std::string anchors_json, error, sections_json;
    std::vector<float> body;
    if (assemble_weapon(c, item_id, fits_text, portal_enums, secs, anchors_json, error))
        serialize(c, secs, sections_json, body);
    else
        anchors_json.clear();
    std::string j = "{\"item\":";
    json_str(j, item_id);
    j += ",\"error\":";
    json_str(j, error);
    j += ",\"sections\":[" + sections_json + "],\"anchors\":{" + anchors_json + "}}";
    return record(j, body, out);
}

extern "C" int64_t bf6_loadout_soldier(bf6_ctx* c, const char* request_json, const char* portal_enums, uint8_t** out)
{
    if (!c || !out) return -1;
    bf6json::Value req;
    if (request_json && *request_json) {
        std::string err;
        bf6json::Parser p(request_json, std::strlen(request_json));
        if (!p.parse(req, err)) req = bf6json::Value();
    }
    /* The Unreal plugin's FRequest defaults. */
    const std::string character = field(req, "character", "cha0001wisp");
    const std::string outfit_id = field(req, "outfit", "001");
    const std::string faction = field(req, "faction", "alliance");
    const std::string role = field(req, "role", "assault");
    const std::string item = field(req, "item", "carbine/m4a1");
    const std::string fits = field(req, "fits", "");
    /* SKINNED MODE, off by default so every existing caller is unaffected.
     *
     * With "skinned":"1" the record carries the BIND-pose vertices, the
     * per-vertex skin binding and the composed rig, and the engine poses the
     * mesh itself - which is what lets it play an animation rather than stand
     * in one sampled frame. Without it the behaviour is exactly as before: the
     * core skins at the sampled pose and hands back a static mesh. */
    const bool want_skin = field(req, "skinned", "") == "1";
    /* "view":"1p" builds what the player sees of themselves - their own arms
     * and their weapon, on the first-person skeleton - instead of the whole
     * soldier seen from outside. */
    const bool first_person = field(req, "view", "3p") == "1p";

    std::vector<Sec> secs;
    /* Filled only in skinned mode. `mesh_rig` is one mesh's whole composed
     * skeleton; `base_rig` is the shared part of it, kept once. */
    std::vector<BindBone> mesh_rig, base_rig;
    int base_rig_bones = 0;
    /* Where the engine puts the skeleton so the posed soldier's feet meet the
     * floor, since in skinned mode the geometry itself is not moved. */
    float root_offset[3] = {0, 0, 0};
    bool have_root = false;
    /* The eye height this soldier actually stands at, measured from the ground
     * they are placed on. Declared out here so the record writer can see it. */
    float eye_out = 0.f;
    std::string error, detail;
    auto finish = [&]() -> int64_t {
        std::string sections_json;
        std::vector<float> body;
        if (error.empty()) serialize(c, secs, sections_json, body);
        std::string j = "{\"item\":";
        json_str(j, item);
        j += ",\"error\":";
        json_str(j, error);
        j += ",\"detail\":";
        json_str(j, detail);
        j += ",\"sections\":[" + sections_json + "]";
        /* EYE HEIGHT, in BOTH modes, because standing a camera in this
         * soldier's boots does not require their skeleton - only this number.
         * 0 when the pose could not be measured, which a consumer must read as
         * "use your own default" rather than as "this soldier has no head". */
        if (eye_out > 0.f) {
            char eb[64];
            std::snprintf(eb, sizeof(eb), ",\"eye\":%.6g", eye_out);
            j += eb;
        }
        /* THE BIND RIG, only when asked for. An engine rebuilds its skeleton
         * from `local` (rest, parent-relative) and binds the skin with
         * `inverse`; both are 3x4 row-major, the same convention as every other
         * transform this API hands out. */
        if (!base_rig.empty()) {
            char cb[128];
            std::snprintf(cb, sizeof(cb), ",\"rig_bones\":%d", base_rig_bones);
            j += cb;
            if (have_root) {
                std::snprintf(cb, sizeof(cb), ",\"root\":[%.9g,%.9g,%.9g]",
                              root_offset[0], root_offset[1], root_offset[2]);
                j += cb;
            }
            j += ",\"rig\":[";
            for (size_t i = 0; i < base_rig.size(); ++i) {
                const BindBone& b = base_rig[i];
                if (i) j += ',';
                j += "{\"name\":";
                json_str(j, b.name);
                char buf[64];
                std::snprintf(buf, sizeof(buf), ",\"parent\":%d,\"local\":[", b.parent);
                j += buf;
                for (int k = 0; k < 12; ++k) {
                    std::snprintf(buf, sizeof(buf), "%s%.9g", k ? "," : "", b.local[k]);
                    j += buf;
                }
                j += "],\"inverse\":[";
                for (int k = 0; k < 12; ++k) {
                    std::snprintf(buf, sizeof(buf), "%s%.9g", k ? "," : "", b.inverse[k]);
                    j += buf;
                }
                j += "],\"posed\":[";
                for (int k = 0; k < 12; ++k) {
                    std::snprintf(buf, sizeof(buf), "%s%.9g", k ? "," : "", b.posed[k]);
                    j += buf;
                }
                j += "]}";
            }
            j += ']';
        }
        j += ",\"anchors\":{}}";
        return record(j, body, out);
    };

    char why[512] = {0};
    if (!mount_full(c, why, sizeof(why))) { error = why[0] ? why : "The reader cannot mount front-end equipment."; return finish(); }
    const std::vector<std::string> bundles = names(c, "pf_cha");
    std::set<std::string> ebx;
    for (const std::string& p : names(c, "common/characters/")) ebx.insert(p);
    for (const std::string& p : bundles) ebx.insert(p);
    const std::vector<std::string> res = res_names(c, "common/characters/");
    const std::set<std::string> resources(res.begin(), res.end());
    std::vector<Character> characters;
    std::vector<Outfit> outfits;
    read_characters(bundles, resources, characters, outfits);
    const Character* ch = nullptr;
    const Outfit* outfit = nullptr;
    for (const Character& x : characters) if (x.id == character) { ch = &x; break; }
    for (const Outfit& x : outfits) if (x.character == character && x.id == outfit_id) { outfit = &x; break; }
    if (!ch || !outfit) { error = "The selected character or outfit is unavailable."; return finish(); }

    Pose pose;
    if (!pose_samples(c, role, pose, error, first_person, item)) return finish();
    /* The equipped weapon's own grip over the class idle's generic hold (see
     * apply_weapon_hpose). Third person only: the grip clips live under
     * assets/3p/weapons/, and a first-person hold is its own authored clip. */
    if (!first_person && !pose.channels.empty())
        apply_weapon_hpose(c, item, pose.bindings, pose.channels, (int)(pose.channels.size() / 4), 1);

    const std::string base = ch->root + "/set/set_001/" + character + "_set_001";
    std::vector<std::string> meshes;
    std::string face_bundle;
    if (first_person) {
        /* WHAT A PLAYER SEES OF THEMSELVES: their own arms and their own legs,
         * and nothing else. No headgear, no backpack, no face - a first-person
         * soldier that included them would be standing inside their own head.
         * These are per character AND per outfit, named as the 3P meshes are,
         * which is why the ordinary sweep below has to exclude them.
         *
         * TWO MESHES, NOT ONE, and taking only the first is why looking down
         * showed nothing:
         *
         *   <outfit>_1parms_mesh   2 sections, 15,251 verts, bones 127-200
         *                          LeftShoulder..RightThumbBridge_VPJ - arms
         *                          and hands
         *   <outfit>_1p_mesh       2 sections, 36,205 verts, bones 11-36
         *                          Hips, both legs, Spine..Spine2 - the body
         *                          seen when looking down
         *
         * `_1p_mesh` was loaded by NEITHER path: this branch asked for the
         * `_1parms_mesh` suffix, and the 3P sweep below drops any name
         * containing `_1p`. The install pairs them 1:1 (323 of each under
         * common/characters/), so a character with arms has a body to match. */
        for (const std::string& p : res)
            if (starts(p, base) && (ends(p, "_1parms_mesh") || ends(p, "_1p_mesh")))
                meshes.push_back(p);
        if (meshes.empty()) {
            error = "This character has no first-person arms.";
            return finish();
        }
    } else {
        for (const std::string& p : res)
            if (starts(p, base) && ends(p, "_mesh") && p.find("_1p") == std::string::npos) meshes.push_back(p);
        for (const std::string& p : bundles)
            if (starts(p, "pf_" + character + "_face_001_") && p.find('/') == std::string::npos && ends(p, "_bundle_3p")) { face_bundle = p; break; }
        for (const char* suffix : {"base_standard", "parts_standard"}) {
            const std::string mesh = ch->root + "/face/_base/" + character + "_face_" + suffix + "_mesh";
            if (resources.count(mesh)) meshes.push_back(mesh);
        }
    }

    M43 weapon = identity43();
    M43 weapon_local = identity43();
    M43 mag_local = identity43();   /* Wep_Align's frame expressed in the magazine bone's */
    M43 mag2_local = identity43();  /* and in the second magazine bone's */
    int weapon_bone = -1;
    float camera_y = 0.f;      /* the eye joints' height in rig space, before placing */
    float view[3] = {0, 0, 0}; /* CameraJoint's position, the first-person origin */
    bool have_view = false;
    float foot[3] = {0, 0, 0};
    /* The ground offset. In the default path this is measured from the posed
     * vertices after the loop. In skinned mode there ARE no posed vertices, so
     * it is accumulated here from the palette instead - the soldier must stand
     * on the floor in the pose it is shown in, not in its bind stance. */
    float min_y = 1e30f;
    bool any = false;
    int parts = 0;
    for (const std::string& mesh : meshes) {
        const bool face = mesh.find("/face/") != std::string::npos;
        /* Hair cards need their own shader; a solid stand-in is worse than none. */
        if (mesh.find("eyelash") != std::string::npos || mesh.find("eyebrow") != std::string::npos
            || mesh.find("velcro") != std::string::npos)
            continue;
        std::string variation = mesh.substr(0, mesh.size() - 5);
        if (face) variation = replace_all(replace_all(variation, "/face/_base/", "/face/face_001/"), "_face_base_", "_face_001_base_");
        else variation = replace_all(variation, "set_001", "set_" + outfit_id);
        if (!ebx.count(variation)) variation.clear();
        std::vector<M43> skin;
        int rig_bones = 0;
        if (!skin_for(c, ebx, mesh, pose, skin, rig_bones, weapon, foot, error,
                      want_skin ? &mesh_rig : nullptr, &weapon_local, &weapon_bone,
                      &camera_y, first_person, view, &mag_local, &mag2_local)) {
            secs.clear();
            return finish();
        }
        const size_t from = secs.size();
        /* In skinned mode the palette is deliberately WITHHELD: handing it over
         * would CPU-skin the vertices into the sampled pose, and a posed vertex
         * cannot be re-posed. The engine gets bind-pose geometry plus the
         * binding and does the posing itself. */
        if (!read_sections(c, mesh, face ? face_bundle : outfit->bundle, variation,
                           want_skin ? nullptr : &skin, rig_bones, secs, error, want_skin)) {
            secs.clear();
            return finish();
        }
        if (want_skin && !mesh_rig.empty()) {
            /* The first rig_bones entries are the shared base rig - identical
             * for every mesh - and everything above is THIS mesh's appended
             * renderbones. Keep the base once and hand the appendix to the
             * sections this mesh just produced. */
            if (base_rig.empty() && (int)mesh_rig.size() >= rig_bones)
                base_rig.assign(mesh_rig.begin(), mesh_rig.begin() + rig_bones);
            base_rig_bones = rig_bones;
            std::vector<BindBone> appendix;
            if ((int)mesh_rig.size() > rig_bones)
                appendix.assign(mesh_rig.begin() + rig_bones, mesh_rig.end());
            for (size_t i = from; i < secs.size(); ++i) secs[i].rbones = appendix;
            /* HOW LOW THE POSED SOLDIER REACHES, without ever building the
             * posed mesh. Only the height is wanted, so only the palette's y
             * column is applied - the same sum the renderer would do, one
             * component wide. */
            for (size_t i = from; i < secs.size(); ++i) {
                const Sec& s = secs[i];
                if (s.influences <= 0 || s.sbones.empty()) continue;
                const size_t vcount = s.pos.size() / 3;
                for (size_t v = 0; v < vcount; ++v) {
                    float y = 0.f, total = 0.f;
                    for (int lane = 0; lane < s.influences; ++lane) {
                        const size_t at = v * (size_t)s.influences + (size_t)lane;
                        const float w = s.sweights[at];
                        const uint16_t b = s.sbones[at];
                        if (w <= 0.f || b >= skin.size()) continue;
                        const float* p = &s.pos[v * 3];
                        const float* m = skin[b].m;
                        y += w * (p[0] * m[1] + p[1] * m[4] + p[2] * m[7] + m[10]);
                        total += w;
                    }
                    if (total > 1e-6f) { min_y = std::min(min_y, y / total); any = true; }
                }
            }
        }
        if (ends(mesh, "_patch_mesh")) {
            const std::string badge = std::string("common/characters/_shared/patches/faction/t_patch_faction_")
                                    + (faction == "pax" ? "pax_01" : "alliance") + "_cs";
            const int texture = bf6_texture_id_by_name(c, badge.c_str());
            if (texture < 0) { error = "The selected faction badge is unavailable."; secs.clear(); return finish(); }
            for (size_t i = from; i < secs.size(); ++i) {
                Sec& s = secs[i];
                s.textures.erase(std::remove_if(s.textures.begin(), s.textures.end(),
                                                [](const std::pair<int, int>& b) { return b.first == 0; }),
                                 s.textures.end());
                s.textures.push_back({0, texture});
                s.has_material = true;
                s.alpha_test = 1;
                s.alpha_from_albedo = 1;
            }
        }
        ++parts;
    }

    if (!want_skin)
        for (const Sec& s : secs)
            for (size_t v = 1; v < s.pos.size(); v += 3) { min_y = std::min(min_y, s.pos[v]); any = true; }
    /* Both modes place the soldier with their lowest point on the floor, so the
     * eye height above that floor is the same subtraction either way. */
    if (any && camera_y != 0.f) eye_out = camera_y - min_y;
    std::vector<Sec> gun;
    std::string anchors_unused;
    if (!assemble_weapon(c, item.c_str(), fits.c_str(), portal_enums, gun, anchors_unused, error)) { secs.clear(); return finish(); }
    /* In skinned mode the weapon is left in the HAND's frame and the bone is
     * named, so it can be attached and ride the animation; otherwise it is
     * planted at the sampled frame, which is all a static soldier needs. */
    transform_sections(gun, want_skin ? weapon_local : weapon);
    /* And the magazine on top, into the bone it actually hangs from. Skinned
     * builds only: a static soldier has the whole gun planted at one frame and
     * nothing rides a bone at all. */
    /* THE MAGAZINE THE RELOAD BRINGS IN.
     *
     * The reload drives TWO magazine bones: Wep_MGZ_Mag1 travels 1243 mm taking the
     * spent magazine out, and Wep_MGZ_Mag2 travels 751 mm bringing a fresh one in.
     * The build ships ONE magazine, so the second bone moved nothing and a reload
     * ended with an empty magazine well.
     *
     * The game instances the same mesh twice, and that is safe to reproduce because
     * all three magazine bones - Mag1, Mag2 and the MGZ_ATT mount - rest at exactly
     * the same point, measured (0, 67.1, 147.6) mm. So the copy sits hidden inside
     * the first until the reload pulls them apart.
     *
     * Copied BEFORE the first is re-framed, or the copy would be re-framed twice.
     * The two surfaces are NOT split between the bones: they are 864 and 5080 verts
     * of one magazine, not two copies, and separating them would tear it in half. */
    std::vector<Sec> mag2;
    if (want_skin)
        for (const Sec& s : gun)
            if (s.mesh.find("_magazine_") != std::string::npos) mag2.push_back(s);
    if (want_skin) transform_sections_matching(gun, mag_local, "_magazine_");
    if (!mag2.empty()) transform_sections(mag2, mag2_local);
    const size_t gun_from = secs.size();
    for (Sec& s : gun) secs.push_back(std::move(s));
    if (first_person && !want_skin) {
        /* ANCHORED ON THE EYE, not on the floor. A first-person build has no
         * feet to stand on and its whole job is to be parented to a camera, so
         * the origin is CameraJoint and the engine attaches the record at
         * identity. Placing it by the lowest vertex, as the 3P soldier is,
         * would hang the arms wherever the shoulders happened to reach.
         *
         * STATIC BUILDS ONLY - see the skinned branch below for why. */
        have_view = view[0] != 0.f || view[1] != 0.f || view[2] != 0.f;
        if (have_view) {
            M43 anchor = identity43();
            anchor.m[9] = -view[0];
            anchor.m[10] = -view[1];
            anchor.m[11] = -view[2];
            transform_sections(secs, anchor);
        }
    } else if (want_skin) {
        /* THE GEOMETRY IS NOT MOVED. It is in rig space, which is the only
         * space the skinning palette will accept it in - translating it here
         * would be invisible at bind pose, where the palette is the identity,
         * and would tear the soldier apart the moment a pose was applied. The
         * engine is told where to stand the skeleton instead.
         *
         * THAT WARNING USED TO BE UNREACHABLE IN FIRST PERSON, because the
         * branch above claimed `first_person` before this one could test
         * `want_skin`, so the skinned 1P build had its geometry translated to
         * the eye after all. It failed exactly as written: invisible at bind
         * pose, and measured at 274x rest edge length the moment the idle
         * played, with every vertex sitting the eye height away from the bone
         * it followed. The arms were being skinned by a palette built for a
         * mesh that was no longer where the palette expected it.
         *
         * So first person takes this path too now. It differs only in where
         * the root goes: an eye anchor rather than a floor one, because a
         * first-person build still has no feet to stand on. */
        if (first_person) {
            have_view = view[0] != 0.f || view[1] != 0.f || view[2] != 0.f;
            if (have_view) {
                root_offset[0] = -view[0];
                root_offset[1] = -view[1];
                root_offset[2] = -view[2];
                have_root = true;
            }
        } else if (any) {
            root_offset[0] = -foot[0];
            root_offset[1] = -min_y;
            root_offset[2] = -foot[2];
            have_root = true;
        }
        /* THE MAGAZINE FOLLOWS THE MAGAZINE BONE, not the whole gun's.
         *
         * Every gun section used to hang off Wep_Align, which makes the weapon one
         * rigid lump. That is why a reload never took the magazine out: the m4a1's
         * own a_1p_m4a1_stand_reloadtactical_01 drives TWELVE weapon-part bones -
         * Wep_MGZ_Mag1, its two ammo bones, Wep_MGZ_Mag2, Wep_Bolt1, Wep_Bolt2,
         * Wep_BoltCatch, Wep_MagRelease, Wep_Trigger and two magnifier bones, all
         * measured present in BOTH the clip and the rig - and not one of them moved
         * anything, because no geometry was attached to any of them.
         *
         * ONLY WHAT THE DATA SEPARATES CAN BE ATTACHED SEPARATELY. The m4a1 arrives
         * as 12 gun sections over 9 mesh assets, and the magazine is its own asset
         * (ob_wep_carbine_m4a1_magazine_1p_mesh), so it can take the magazine bone
         * while the rest keep Wep_Align. The bolt, trigger and mag release are part
         * of the receiver mesh and cannot be split here at all - that needs the
         * source model, not this loop - so their tracks still drive nothing and this
         * does not pretend otherwise. */
        int mag_bone = -1;
        for (size_t i = 0; i < base_rig.size(); ++i)
            if (base_rig[i].name == "Wep_MGZ_Mag1") { mag_bone = (int)i; break; }
        for (size_t i = gun_from; i < secs.size(); ++i) {
            const bool is_mag = secs[i].mesh.find("_magazine_") != std::string::npos;
            secs[i].attach_bone = (is_mag && mag_bone >= 0) ? mag_bone : weapon_bone;
        }
        /* And the incoming magazine, on the second bone. */
        int mag2_bone = -1;
        for (size_t i = 0; i < base_rig.size(); ++i)
            if (base_rig[i].name == "Wep_MGZ_Mag2") { mag2_bone = (int)i; break; }
        if (mag2_bone >= 0)
            for (Sec& s : mag2) {
                s.attach_bone = mag2_bone;
                secs.push_back(std::move(s));
            }
        mag2.clear();
    } else if (any) {
        M43 anchor = identity43();
        anchor.m[9] = -foot[0];
        anchor.m[10] = -min_y;
        anchor.m[11] = -foot[2];
        transform_sections(secs, anchor);
    }
    char b[128];
    std::snprintf(b, sizeof(b), "Posed soldier, %d character parts and configured weapon", parts);
    detail = b;
    return finish();
}

/* THE WHOLE IDLE CLIP AS PARENT-RELATIVE BONE TRACKS.
 *
 * bf6_loadout_soldier's "posed" is one frame of this. Handing back every frame
 * in the same shape is what turns a rigged soldier into a moving one, and doing
 * the decode here rather than in each engine is what keeps them moving
 * identically - the binding rules, the 0x8000 resolution and the choice of clip
 * variant are all decisions, and a decision made twice is made differently. */
extern "C" int64_t bf6_loadout_soldier_clip(bf6_ctx* c, const char* request_json, uint8_t** out)
{
    if (!c || !out) return -1;
    *out = nullptr;
    char why[512] = {0};
    if (!mount_full(c, why, sizeof(why))) return -1;

    bf6json::Value req;
    if (request_json && *request_json) {
        std::string perr;
        bf6json::Parser p(request_json, std::strlen(request_json));
        if (!p.parse(req, perr)) req = bf6json::Value();
    }
    const std::string role = field(req, "role", "assault");
    /* "view":"1p" exports the FIRST-PERSON clip on the first-person rig, the
     * same switch bf6_loadout_soldier takes. Without it this function could
     * only ever animate the third-person soldier, and the arms the player
     * actually looks at stayed a still photograph.
     *
     * A 1P clip is not simply "the 3P clip on a different skeleton": it binds
     * ske_soldier_1p through soldier_1p.rig, and the authored resting motion
     * is a POSE plus additive fidget layers rather than a looping cycle - see
     * the note at k1pStandClip. This exports the base; layering the additives
     * on top is the caller's to do, and is the same decision in both engines,
     * so it will move here once it exists in one. */
    const bool first_person = field(req, "view", "3p") == "1p";
    const char* const rig_ebx = first_person ? kRig1p : kRig;
    const char* const ske_ebx = first_person ? kSkeleton1p : kSkeleton;

    std::string error, clip_path;
    std::vector<bf6_anim_binding> bindings, add_bindings;
    bf6_anim_binding_stats stats{};
    std::vector<float> body;
    std::string tracks_json;
    int frames = 0, bones_out = 0;

    bf6_skeleton* s = nullptr;
    bf6_anim_clip* clip = nullptr;
    bf6_anim_clip* add_clip = nullptr;
    /* The additive asset. Chosen inside the first_person branch and read again
     * where the clip is opened, so it is declared out here beside add_clip. */
    std::string swayclip;
    const char* add_asset = k1pStandAdditive;
    do {
        if (first_person) {
            /* The first-person resting hold. Now that the RAW and flat pose
             * containers read, this is the authored asset rather than frame 0
             * of a turn-in-place transition standing in for it. */
            const std::string stance = field(req, "stance", "stand");
            /* "clip" NAMES ONE OUTRIGHT, bypassing the stance lookup.
             *
             * The stance names cover the three poses the install ships as
             * poses. Everything else a first-person view does - the slide, the
             * turns, the stance transitions - ships as its own clip under its
             * own name, and there are 160 of them for the rifle alone. Naming
             * them one at a time in here would be a list that goes stale; the
             * caller knows which it wants. It is also the only way to find out
             * whether a clip binds against this rig at all without a rebuild
             * per guess. */
            const std::string named = field(req, "clip", "");
            clip_path = named.empty()
                ? first_person_stance(c, field(req, "item", "carbine/m4a1"), stance)
                : named;
            bf6_anim_binding_stats st{};
            const int n = bf6_anim_bindings(c, clip_path.c_str(), rig_ebx, ske_ebx,
                                            nullptr, 0, &st);
            if (n < 1 || n > 4096) {
                error = "The first-person hold has no readable animation binding.";
                break;
            }
            bindings.assign((size_t)n, bf6_anim_binding{});
            if (bf6_anim_bindings(c, clip_path.c_str(), rig_ebx, ske_ebx,
                                  bindings.data(), n, &stats) != n) {
                error = "The first-person hold has no readable animation binding.";
                break;
            }
            /* THE SWAY. Bound SEPARATELY and composed per bone rather than per
             * channel: two clips that both resolve 128 keys need not resolve
             * them in the same order, and nothing guarantees a shared channel
             * map - the mismatch diagnostics name a different
             * toolchanneltodofassetdata per clip. Bone plus component is the
             * common ground both agree on.
             *
             * Absent or unbindable, the hold is simply still. A missing fidget
             * layer is not a reason to refuse the arms. */
            /* "sway":"0" LEAVES IT OFF, for measurement rather than for taste.
             *
             * The hands hold the weapon rigidly, so the transform from the
             * weapon bone to each hand must not change over the clip. It does:
             * 0.0 mm at the loop ends and up to 22 mm in between, and the loop
             * ends are exactly where this layer's excursion is zero. That
             * implicates the layer, but implicating is not proving, and the
             * only way to tell a broken sway from a broken grip underneath it
             * is to measure the same thing with the sway absent. */
            /* AND ONLY STAND HAS ONE. There is no ladtv_ sibling for the crouch
             * or the prone pose, so asking for one there is a guaranteed
             * binding failure dressed up as a missing fidget layer. Skip it by
             * name rather than by letting it fail. */
            /* The sway belongs to the stand POSE. A named clip is a transition
             * or a cycle with its own motion, so layering the stand fidget over
             * it would be two animations at once. */
            const bool sway = field(req, "sway", "1") != "0" && stance == "stand"
                && named.empty();
            /* WHICH additive, overridable for measurement.
             *
             * k1pStandAdditive is `ladtv_1p_rifle_stand_idle_01`, and `ladtv_` is
             * a LAYER additive: that one is layer 0 of 1p.legs.idle.slc, the LEGS.
             * The arms have their own (`adtv_oma_rifle_stand_idle_01`) with a
             * breathing cycle beside it. Which of them is the 1P hold's fidget is
             * a question for the grip measurement, not for the folder a file
             * happens to sit in. */
            swayclip = field(req, "swayclip", "");
            add_asset = swayclip.empty() ? k1pStandAdditive : swayclip.c_str();
            bf6_anim_binding_stats ast{};
            const int an = sway ? bf6_anim_bindings(c, add_asset, rig_ebx, ske_ebx,
                                                   nullptr, 0, &ast) : 0;
            if (an > 0 && an <= 4096) {
                add_bindings.assign((size_t)an, bf6_anim_binding{});
                if (bf6_anim_bindings(c, add_asset, rig_ebx, ske_ebx,
                                      add_bindings.data(), an, &ast) != an)
                    add_bindings.clear();
            }
        } else {
            /* "clip" OVERRIDES THE THIRD-PERSON IDLE TOO.
             *
             * resolve_idle_clip answers with a FRONT-END idle - for the assault
             * rifle it is ui_frontend_standing_idle_assault_02 - because this
             * whole path was built for the loadout screen, where that is exactly
             * right. A soldier standing at a spawner in a level is not on the
             * loadout screen, and a menu pose there reads as wrong.
             *
             * The install ships gameplay idles beside it (p_3p_rifle_stand_idle_01,
             * l_3p_rifle_stand_idle_01 and its intensity variants), so a caller
             * that knows it is placing a soldier in the world can ask for one. */
            const std::string named3p = field(req, "clip", "");
            clip_path = named3p.empty()
                ? resolve_idle_clip(c, role, bindings, stats, error)
                : named3p;
            if (!named3p.empty()) {
                /* A named clip needs its own bindings; resolve_idle_clip fills
                 * them as a side effect and is not being called. */
                bf6_anim_binding_stats st{};
                const int n = bf6_anim_bindings(c, clip_path.c_str(), rig_ebx, ske_ebx,
                                                nullptr, 0, &st);
                if (n < 1 || n > 4096) {
                    error = "That clip has no readable animation binding.";
                    break;
                }
                bindings.assign((size_t)n, bf6_anim_binding{});
                if (bf6_anim_bindings(c, clip_path.c_str(), rig_ebx, ske_ebx,
                                      bindings.data(), n, &stats) != n) {
                    error = "That clip has no readable animation binding.";
                    break;
                }
            }
        }
        if (clip_path.empty()) break;
        /* The PLAIN rig, with no renderbones appended: a clip binds rig bones,
         * and the record's shared base rig is exactly this list, so track bone
         * indices line up with it without any further resolution. */
        s = bf6_skeleton_compose(c, ske_ebx, nullptr);
        if (!s || s->bone_count < 1 || s->bone_count > 8192) {
            error = "The character render skeleton is unavailable.";
            break;
        }
        clip = bf6_anim_clip_open(c, clip_path.c_str());
        /* NOT AN EQUALITY. `bindings` has one entry per channel-map KEY and
         * `channel_count` counts STORED channels, and those differ whenever
         * scalars are packed four to a float4 - the 1P hold declares 89
         * quaternions, 38 vectors and 2 scalars, so 129 keys against 128
         * stored. Both numbers are right. Requiring them equal rejected the
         * first-person clip outright with "the selected soldier pose is
         * unavailable", which is the same mistake the binding reader's layout
         * check made, in a second place.
         *
         * More stored channels than keys would be genuinely wrong - there
         * would be no binding to place them with - so that is still refused. */
        if (!clip || clip->channel_count < 1 ||
            clip->channel_count > (int)bindings.size() || clip->key_time_count < 1) {
            error = "The selected soldier pose is unavailable.";
            break;
        }
        frames = clip->key_time_count;

        /* The additive supplies the frames. The base is a single rest pose, so
         * without a layer on top the export is one frame - correct, and still.
         * Dropped rather than refused if its own layout check fails, on the
         * same principle as the binding above. */
        if (!add_bindings.empty()) {
            add_clip = bf6_anim_clip_open(c, add_asset);
            if (!add_clip || add_clip->channel_count < 1 ||
                add_clip->channel_count > (int)add_bindings.size() ||
                add_clip->key_time_count < 1) {
                if (add_clip) { bf6_free(c, add_clip); add_clip = nullptr; }
                add_bindings.clear();
            } else {
                frames = add_clip->key_time_count;
            }
        }

        /* Which bones the clip actually drives. Everything else stays at its
         * rest transform, so shipping a flat track for it would be bytes that
         * say "unchanged" several hundred times a frame. */
        std::vector<int> track_of((size_t)s->bone_count, -1);
        std::vector<int> bone_of;
        /* Both layers, because a bone the additive sways is a bone that moves
         * even if the base pose leaves it at its bind transform - and it would
         * get no track at all if only the base were consulted. */
        for (const std::vector<bf6_anim_binding>* set : { &bindings, &add_bindings })
        for (const bf6_anim_binding& b : *set) {
            if (b.bone < 0 || b.bone >= s->bone_count) continue;
            if (b.component != BF6_ANIM_DOF_QUATERNION && b.component != BF6_ANIM_DOF_VECTOR3) continue;
            if (track_of[(size_t)b.bone] < 0) {
                track_of[(size_t)b.bone] = (int)bone_of.size();
                bone_of.push_back(b.bone);
            }
        }
        if (bone_of.empty()) { error = "The selected soldier pose drives no bone."; break; }
        bones_out = (int)bone_of.size();

        /* Bone-major: one bone's whole track is contiguous, because that is how
         * an engine builds an animation track - per bone, not per frame. */
        body.assign((size_t)bones_out * (size_t)frames * 12, 0.f);
        std::vector<float> channels((size_t)clip->channel_count * 4, 0.f);
        std::vector<M43> local((size_t)s->bone_count);
        std::vector<M43> model((size_t)s->bone_count);
        std::vector<float> add_channels;
        if (add_clip) add_channels.assign((size_t)add_clip->channel_count * 4, 0.f);
        /* THE ADDITIVE, GATHERED PER BONE. Allocated once outside the frame loop
         * rather than per frame: 799 frames times a few hundred bones is not a
         * place to be calling the allocator. `add_d` holds identity rotations so
         * a bone with only a translation delta composes against identity rather
         * than against whatever was left from the previous frame. */
        std::vector<M43> add_d((size_t)s->bone_count, identity43());
        std::vector<uint8_t> add_rot, add_trans;
        /* The equipped weapon's grip over the class hold, and the left hand to
         * the weapon, on EVERY frame - the same two steps bf6_loadout_soldier
         * applies to its one frame, so frame 0 here is still its `posed`. */
        /* THE WEAPON'S GRIP, which was off for first person.
         *
         * weapon_hpose_overrides reads the weapon's own authored hand poses - the
         * install carries hposes.grippose.cdb with lefthand, righthand and attach
         * variants, plus wep.gripposemaster.cdb - and apply_grip writes them over
         * the class hold. Without it every weapon is held in the generic rifle
         * grip, which is the difference between a hand ON the gun and a hand near
         * it, and it is exactly the "different grips" the tool did not have.
         *
         * It was gated to third person with no reason recorded. "grip":"0" turns it
         * off again so the two can be compared on grip rigidity, which is the only
         * judge that has held up on this file.
         *
         * MEASURED, AND THE GATE WAS RIGHT: turning it on took the hand slide from
         * 5.02 to 18.61 mm. The reason is that hposes.* are THIRD-person hand poses
         * (hposes.lefthand.m4a1.pc and its siblings live under assets/3p/), and the
         * first-person grip is already baked into the weapon-specific 1P stance
         * pose - p_1p_oma_m4a1_stand_idle_01 is the M4A1 hold, grip included. So
         * first person HAS per-weapon grips; it just does not get them from here.
         * "grip":"1" turns it on for measurement. */
        const std::vector<GripOverride> grip =
            (first_person && field(req, "grip", "0") != "1")
            ? std::vector<GripOverride>()
            : weapon_hpose_overrides(c, field(req, "item", "carbine/m4a1"), bindings, clip->channel_count);

        bool ok = true;
        for (int f = 0; f < frames && ok; ++f) {
            /* The base is one pose; it is sampled at its own frame, not at f.
             * Asking a one-frame clip for frame 700 is a refusal, which would
             * have made the whole export fail the moment a layer lengthened
             * it. */
            const int base_f = f < clip->key_time_count ? f : clip->key_time_count - 1;
            if (!bf6_anim_clip_sample(c, clip, base_f, channels.data(), nullptr)) { ok = false; break; }
            apply_grip(grip, channels.data(), clip->channel_count);
            /* Start from the BIND local every frame. A bone the clip rotates
             * but does not translate then keeps its authored offset, instead of
             * collapsing onto its parent. */
            for (int i = 0; i < s->bone_count; ++i) local[(size_t)i] = from12(s->bones[i].local);
            for (const bf6_anim_binding& b : bindings) {
                if (b.bone < 0 || b.bone >= s->bone_count) continue;
                if (b.channel < 0 || (size_t)b.channel * 4 + 3 >= channels.size()) { ok = false; break; }
                const float* v = channels.data() + (size_t)b.channel * 4;
                if (b.component == BF6_ANIM_DOF_QUATERNION)
                    quat_rows(v[0], v[1], v[2], v[3], local[(size_t)b.bone].m);
                else if (b.component == BF6_ANIM_DOF_VECTOR3)
                    for (int k = 0; k < 3; ++k) local[(size_t)b.bone].m[9 + k] = v[k];
            }
            /* THE ADDITIVE LAYER, AS ONE TRANSFORM PER BONE.
             *
             * It used to be applied per BINDING, one component at a time, with
             * the rotation composed on the right and the translation simply
             * added. Two things were wrong with that.
             *
             * THE SIDE. The old note here said the clip's magnitudes - largest
             * excursion 0.034, about two degrees - cannot distinguish
             * posed*delta from delta*posed. That is true of the VALUES and
             * false of the RESULT. The hands hold the weapon, so the transform
             * from the weapon bone to each hand cannot change across the clip,
             * and the wrong side breaks exactly that. Measured, as the largest
             * slide of four hand bones against Wep_Align over the clip:
             *
             *     right (posed*delta)   21.49 .. 31.66 mm
             *     left  (delta*posed)    3.51 ..  7.18 mm
             *     third person, control   0.00 mm
             *
             * Third person being exactly rigid is what makes that readable: a
             * correct hold does not slide at all. So the side is LEFT.
             *
             * THE COMPONENTS ARE NOT INDEPENDENT. These are row-vector 3x4s, so
             * composing delta then bone sends a point to p*Rd*Rm + td*Rm + tm.
             * The translation therefore picks up the bone's OWN rotation, and
             * adding it raw - as the per-binding version did - drops that term.
             * Doing it per binding could not have got this right in any case,
             * because the two components arrive as separate bindings in no
             * guaranteed order, and the answer depends on both.
             *
             * So the deltas are gathered per bone first, then applied once. */
            if (add_clip && ok) {
                if (!bf6_anim_clip_sample(c, add_clip, f, add_channels.data(), nullptr)) { ok = false; break; }
                add_rot.assign((size_t)s->bone_count, 0);
                add_trans.assign((size_t)s->bone_count, 0);
                for (const bf6_anim_binding& b : add_bindings) {
                    if (b.bone < 0 || b.bone >= s->bone_count) continue;
                    if (b.channel < 0 || (size_t)b.channel * 4 + 3 >= add_channels.size()) { ok = false; break; }
                    const float* v = add_channels.data() + (size_t)b.channel * 4;
                    if (b.component == BF6_ANIM_DOF_QUATERNION) {
                        quat_rows(v[0], v[1], v[2], v[3], add_d[(size_t)b.bone].m);
                        add_rot[(size_t)b.bone] = 1;
                    } else if (b.component == BF6_ANIM_DOF_VECTOR3) {
                        for (int k = 0; k < 3; ++k) add_d[(size_t)b.bone].m[9 + k] = v[k];
                        add_trans[(size_t)b.bone] = 1;
                    }
                }
                if (!ok) break;
                const std::string side = field(req, "swayside", "left");
                /* THE LAYER IS MASKED, AND THIS WAS NOT MASKING IT.
                 *
                 * `ladtv_1p_rifle_stand_idle_01` is layer 0 of 1p.legs.idle.slc,
                 * and that layer carries the blend mask
                 * animations/kingston/global/blendmasks/1p.lowerbodyandchest.bml.
                 * Read out of the authored data, its 25 per-set weights are:
                 *
                 *   1 : connect ground trajectory deltatrajectory hips leftleg
                 *       rightleg lvelocityrig rvelocityrig ik.leftfoot
                 *       ik.rightfoot torso
                 *   0 : camera camera3p lefthand leftarm righthand rightarm
                 *       head headgear spinex weaponroot weaponparts weapon2*
                 *
                 * The sway is authored for the legs, hips and chest and is
                 * explicitly ZERO on the hands, the arms and the weapon. The
                 * `ladtv_` prefix says the same: it is a LAYER additive and it
                 * belongs to the lower body. Applying it to everything is what
                 * left the hands sliding 3.5..7.2 mm along a weapon they hold
                 * rigidly, where third person measures 0.00 mm.
                 *
                 * WHAT THIS IS NOT: matching bone NAMES is a hypothesis test,
                 * not the mask. The real mask is 25 DOF sets of packed 64-bit
                 * descriptors - lefthand.ds alone holds 141 - and decoding those
                 * is the correct fix. This decides whether that work is
                 * justified: "lowerbody" should take the slide to about 0.00 mm,
                 * and if it does not then the mask was never the cause.
                 *
                 * MEASURED, AND IT DOES NOT. "lowerbody" took the slide from
                 * 5.02 mm UP to 17.49 mm, so this is refused as the default and
                 * the descriptors are NOT worth decoding for this. The reason is
                 * instructive: LeftShoulder contains neither "Hand" nor "Arm",
                 * so it kept the sway while LeftArm, LeftForeArm and LeftHand
                 * lost it, and breaking a chain halfway is worse than leaving it
                 * whole. A name is not a DOF set.
                 *
                 * The likelier reading is that this layer is simply not the 1P
                 * hold s fidget at all: `ladtv_` is a LAYER additive and this one
                 * is layer 0 of 1p.legs.idle.slc, the LEGS. Whatever sways the
                 * arms is a different asset, and that is the thread to pull. */
                const std::string mask = field(req, "swaymask", "none");
                for (int i = 0; i < s->bone_count; ++i) {
                    if (!add_rot[(size_t)i] && !add_trans[(size_t)i]) continue;
                    if (mask == "lowerbody") {
                        const char* bn = s->bones[i].name;
                        if (bn && (std::strstr(bn, "Hand") || std::strstr(bn, "Arm")
                                   || std::strstr(bn, "Wep") || std::strstr(bn, "Camera")))
                            continue;
                    }
                    float* m = local[(size_t)i].m;
                    const float* d = add_d[(size_t)i].m;
                    float r[12];
                    for (int k = 0; k < 12; ++k) r[k] = m[k];
                    if (add_rot[(size_t)i]) {
                        for (int y = 0; y < 3; ++y)
                            for (int x = 0; x < 3; ++x)
                                r[y*3+x] = (side == "right")
                                    ? m[y*3+0]*d[0*3+x] + m[y*3+1]*d[1*3+x] + m[y*3+2]*d[2*3+x]
                                    : d[y*3+0]*m[0*3+x] + d[y*3+1]*m[1*3+x] + d[y*3+2]*m[2*3+x];
                    }
                    /* THE TRANSLATION IS ADDED RAW, on either side.
                     *
                     * Composing it as the row-vector algebra suggests - td*Rm +
                     * tm, the delta turned by the bone's own rotation - was
                     * tried and is wrong: it took the slide from 3.51..7.18 mm
                     * back up to 19.15..22.29 mm. So the delta translation is
                     * already in the frame the bone's translation is in, which
                     * is the parent's, and the only correct thing to do with it
                     * is add it. The rotation and the translation of this layer
                     * are therefore NOT two halves of one transform, however
                     * much they look like it. */
                    if (add_trans[(size_t)i])
                        for (int k = 0; k < 3; ++k) r[9 + k] = m[9 + k] + d[9 + k];
                    for (int k = 0; k < 12; ++k) m[k] = r[k];
                }
            }
            /* THE LEFT HAND SOLVED ONTO THE WEAPON, in first person too.
             *
             * This was third person only, with the note that "whether the
             * first-person holds want the same pass is not established". The grip
             * measurement establishes it. The hands hold the weapon, so the
             * transform from Wep_Align to each hand cannot change across the clip,
             * and:
             *
             *   third person, WITH this solve      0.00 mm of slide
             *   first person, WITHOUT it           5.02 .. 7.18 mm
             *
             * Third person is not rigid by luck - it is rigid BECAUSE this pass
             * forces it, and the soldier feature layer carries
             * common/gameplay/soldier/animation/ikchain_lefthand for exactly this.
             *
             * Two other explanations for that residual were tried and refuted by
             * measurement first: masking the sway to its authored
             * 1p.lowerbodyandchest weights took it UP to 17.49 mm, and every other
             * candidate additive scored worse (12.38, 12.57, 10.25) or was inert.
             * MEASURED, AND IT IS ALSO REFUTED: enabling it for first person took
             * the LEFT hand from 5.02 to 6.96 mm and left the right hand at 3.51,
             * which it would, since it only solves the left arm. So it stays off
             * for first person and "iksolve":"1" turns it on for measurement.
             *
             * AND THE TARGET ITSELF WAS WRONG. Third person is rigid on the RIGHT
             * hand too, which no left-arm solve can explain. In third person the
             * weapon rides RightHand through the grip transform; in first person
             * Wep_Align is animated on its own tracks. The two are built
             * differently, so 0.00 mm was never the figure first person should be
             * held to, and a few millimetres of authored hand-to-weapon variance
             * may simply be correct. Three hypotheses, three refutations, and the
             * fourth candidate is that there is nothing here to fix. */
            if (!first_person || field(req, "iksolve", "0") == "1") {
                bool topo = true;
                for (int i = 0; i < s->bone_count && topo; ++i) {
                    const int p = s->bones[i].parent;
                    if (p >= i) { topo = false; break; }
                    model[(size_t)i] = p < 0 ? local[(size_t)i] : mul(local[(size_t)i], model[(size_t)p]);
                }
                if (topo) solve_left_arm_to_weapon(s, local, model);
            }
            for (int t = 0; t < bones_out; ++t) {
                const float* m = local[(size_t)bone_of[(size_t)t]].m;
                float* dst = &body[((size_t)t * (size_t)frames + (size_t)f) * 12];
                for (int k = 0; k < 12; ++k) dst[k] = m[k];
            }
        }
        if (!ok) { error = "The selected soldier pose could not be sampled."; break; }

        /* THE NAME, NOT JUST THE INDEX.
         *
         * `bone_of` indexes THIS skeleton - the plain rig composed above, with
         * no renderbones appended. The caller's skeleton is not that list: it
         * is built per mesh and carries each section's renderbones, so the same
         * ordinal is not the same bone on both sides. Third person survives it
         * because the appended bones land past the shared base and leave the
         * low indices alone. First person does not, and there the index alone
         * sends every rotation to a different joint.
         *
         * Nothing about that can fail loudly. An index in range always names a
         * real bone, so every track binds, every quaternion stays an ordinary
         * rotation, and the only symptom is a skin pulled apart between joints
         * that disagree - measured at 274x rest edge length on the 1P arms.
         *
         * So the name goes on the wire and the caller resolves it. The index
         * stays, because a reader that already trusts it is no worse off and
         * dropping a field from a record breaks them for no gain. */
        for (int t = 0; t < bones_out; ++t) {
            const int bi = bone_of[(size_t)t];
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s{\"bone\":%d,\"track\":%zu,\"name\":",
                          t ? "," : "", bi, (size_t)t * (size_t)frames * 12);
            tracks_json += buf;
            const char* bn = (bi >= 0 && bi < s->bone_count) ? s->bones[(size_t)bi].name : nullptr;
            json_str(tracks_json, bn ? bn : "");
            tracks_json += "}";
        }
    } while (false);
    if (add_clip) bf6_free(c, add_clip);
    if (clip) bf6_free(c, clip);
    if (s) bf6_free(c, s);
    if (!error.empty()) { body.clear(); tracks_json.clear(); frames = 0; bones_out = 0; }

    std::string j = "{\"clip\":";
    json_str(j, clip_path);
    j += ",\"error\":";
    json_str(j, error);
    char hb[96];
    std::snprintf(hb, sizeof(hb), ",\"frames\":%d,\"bones\":%d,\"tracks\":[", frames, bones_out);
    j += hb;
    j += tracks_json;
    j += "]}";
    return record(j, body, out, 0x41574C42u /* BLWA */);
}
