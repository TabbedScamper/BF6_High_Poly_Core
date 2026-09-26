/* Which rig bone each vertex of a vehicle's body mesh belongs to.
 *
 *   vehicle_mesh_bones_probe <mesh RES name> <skeleton EBX name>
 *
 * A vehicle body is a composite mesh: every vertex carries a part index, and the
 * MeshSet's bone palette (header +0xAC) maps each part to a skeleton bone. This
 * prints the mesh type, the palette with bone names, and how many vertices land on
 * each bone - the check that the Godot side can skin the body to the rig the
 * vehicle graphs pose. */
#include "bf6_core.h"
#include "meshset.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: vehicle_mesh_bones_probe <mesh RES> <skeleton EBX>\n");
        return 1;
    }
    const char* game = std::getenv("BF6_GAME") ? std::getenv("BF6_GAME")
        : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Battlefield 6";
    char err[1024] = {};
    bf6_ctx* ctx = bf6_open(game, err, (int)sizeof(err));
    if (!ctx) { std::fprintf(stderr, "open: %s\n", err); return 2; }
    if (!bf6_mount_all(ctx, 1, err, (int)sizeof(err))) { std::fprintf(stderr, "mount: %s\n", err); return 2; }
    const uint8_t* res = nullptr;
    const int64_t res_len = bf6_read_raw(ctx, BF6_RAW_RES, argv[1], &res);
    if (res_len <= 0) { std::fprintf(stderr, "no RES %s\n", argv[1]); return 3; }
    std::vector<uint8_t> rbytes(res, res + res_len);
    std::string why;
    bf6::MeshSet ms = bf6::meshset_parse(rbytes.data(), rbytes.size(), why);
    std::printf("mesh_type %u, bone_count %d, palette %zu, lods %zu\n", ms.mesh_type, ms.bone_count,
                ms.bone_parts.size(), ms.lods.size());
    bf6_skeleton* sk = bf6_skeleton_read(ctx, argv[2]);
    auto bone_name = [&](int b) -> std::string {
        return sk && b >= 0 && b < sk->bone_count ? sk->bones[b].name : "?";
    };
    for (size_t i = 0; i < ms.bone_parts.size(); ++i)
        std::printf("  part %zu -> bone %u %s\n", i, ms.bone_parts[i], bone_name(ms.bone_parts[i]).c_str());
    if (ms.lods.empty()) return 0;
    const bf6::MeshLod& L = ms.lods[0];
    std::vector<uint8_t> chunk;
    bool zero = true;
    for (uint8_t b : L.chunk_id) zero = zero && b == 0;
    if (!zero) {
        char fwd[33] = {}, rev[33] = {};
        for (int i = 0; i < 16; ++i) {
            std::snprintf(fwd + i * 2, 3, "%02x", L.chunk_id[(size_t)i]);
            std::snprintf(rev + i * 2, 3, "%02x", L.chunk_id[(size_t)(15 - i)]);
        }
        const uint8_t* cp = nullptr;
        int64_t cl = bf6_read_raw(ctx, BF6_RAW_CHUNK, fwd, &cp);
        if (cl <= 0) cl = bf6_read_raw(ctx, BF6_RAW_CHUNK, rev, &cp);
        if (cl > 0) chunk.assign(cp, cp + cl);
    }
    auto secs = bf6::meshset_read_lod(ms, 0, chunk.empty() ? nullptr : chunk.data(), chunk.size(), why);
    std::map<int, size_t> per_bone;
    size_t total = 0, no_parts = 0;
    for (const auto& s : secs) {
        const size_t vc = s.positions.size() / 3;
        total += vc;
        if (s.parts.size() != vc) { no_parts += vc; continue; }
        for (uint16_t p : s.parts) {
            const int b = p < ms.bone_parts.size() ? (int)ms.bone_parts[p] : -1;
            per_bone[b] += 1;
        }
    }
    std::printf("sections %zu, vertices %zu, without parts %zu\n", secs.size(), total, no_parts);
    /* the skin lanes: raw index, through the palette and as a skeleton bone */
    std::map<int, size_t> via_palette, direct;
    size_t multi = 0, skinned = 0;
    for (const auto& s : secs) {
        const size_t vc = s.positions.size() / 3;
        if (s.influences <= 0 || s.skin_bones.size() < vc * (size_t)s.influences) continue;
        for (size_t v = 0; v < vc; ++v) {
            ++skinned;
            int lanes = 0;
            for (int k = 0; k < s.influences; ++k) lanes += s.skin_weights[v * s.influences + k] > 0.0f;
            multi += lanes > 1;
            const uint16_t raw = s.skin_bones[v * s.influences];
            via_palette[raw < ms.bone_parts.size() ? (int)ms.bone_parts[raw] : -1] += 1;
            direct[raw] += 1;
        }
    }
    std::printf("skinned vertices %zu, with more than one weighted lane %zu\n", skinned, multi);
    for (const auto& kv : via_palette)
        std::printf("  palette -> bone %d %-40s %zu\n", kv.first, bone_name(kv.first).c_str(), kv.second);
    for (const auto& kv : direct)
        std::printf("  direct   bone %d %-40s %zu\n", kv.first, bone_name(kv.first).c_str(), kv.second);
    for (const auto& kv : per_bone)
        std::printf("  bone %d %-40s %zu vertices\n", kv.first, bone_name(kv.first).c_str(), kv.second);
    return 0;
}
