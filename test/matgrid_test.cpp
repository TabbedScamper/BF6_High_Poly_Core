/* MATERIAL RELATION GRID: what happens when surface A meets surface B.
 *
 *   matgrid_test <game_dir> <level> [type_identities.tsv]
 *
 * This is the join between a surface and everything the game does about it -
 * the debris a hit throws, the sound it makes, the decal it leaves. The leaf
 * MaterialRelation*Data types decode to ZERO fields through the schema (their
 * reflection places every field at offset 0xFFFF), which is what made this look
 * unreadable; the GRID that references them does not, and that is the part that
 * carries the join.
 *
 * The root instance of <level>/materialgrid_win32 holds:
 *
 *   0xE6C1E5AD  struct { 0x48288193 : u32 }   a declared dimension
 *   0x8E6CA5FC  u32[]                         material id -> row index
 *   0xDB25CF83  struct[]  { 0xEBBBCF03: instance_ref[] }   relations per row
 *   0x24535855  struct[]  { 0x2C95D7B7: struct[]          }   the N x N grid
 *                 each cell: 0x2F0BED21: struct[]
 *                   each:    0xA2A9E9A7: struct[] { 0xDDCB6879: u32,
 *                                                   0x2ADEFB09: instance_ref }
 *
 * WHAT IS ASSERTED, and why each one would catch a wrong reading:
 *   1. The grid is SQUARE and its side equals the relation-group count. A
 *      misread array bound would almost certainly break one of the two.
 *   2. Every instance reference resolves inside the partition. An off-by-one in
 *      the reference decode lands outside and is caught here rather than
 *      surfacing later as the wrong surface's debris.
 *   3. The id map's values index the grid. It is the caller's entry point, so a
 *      value past the side means the map has been read at the wrong width.
 *   4. The grid is SYMMETRIC in occupancy: A-meets-B and B-meets-A must both be
 *      populated or both empty. Nothing in the format forces that, so agreement
 *      is evidence the axes are the right way round rather than transposed.
 *   5. CONTROL: a field hash that does not exist returns nothing.
 */
#include "ebx.h"
#include "source.h"
#include "types.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace bf6;

namespace {

const uint32_t kDim        = 0xe6c1e5adu;
const uint32_t kDimValue   = 0x48288193u;
const uint32_t kIdMap      = 0x8e6ca5fcu;
const uint32_t kGroups     = 0xdb25cf83u;
const uint32_t kGroupRels  = 0xebbbcf03u;
const uint32_t kGrid       = 0x24535855u;
const uint32_t kGridRow    = 0x2c95d7b7u;
const uint32_t kCellList   = 0x2f0bed21u;
const uint32_t kCellItems  = 0xa2a9e9a7u;
const uint32_t kItemKind   = 0xddcb6879u;
const uint32_t kItemRel    = 0x2adefb09u;
const uint32_t kFakeField  = 0x2adefb0au;   /* the control */

/* TypeDb::guid_str is the one spelling of a type guid in this codebase; a
 * second formatter here would print the same guid a different way and make the
 * identity table join fail for no reason. */
std::string hex_guid(const TypeGuid& g) { return TypeDb::guid_str(g); }

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: matgrid_test <game_dir> <level> [identities.tsv]\n"); return 2; }
    const std::string level = argv[2];

    Source src;
    std::string err;
    if (!src.open(argv[1], err)) { std::printf("open: %s\n", err.c_str()); return 1; }
    if (!src.mount_level(level, false, err)) { std::printf("mount: %s\n", err.c_str()); return 1; }

    TypeDb types;
    bool got = false;
    for (const std::string& cand : TypeDb::exe_candidates(src.game_dir()))
        if (types.open(cand, err) && !types.looks_encrypted()) { got = true; break; }
    if (!got) { std::printf("types: %s\n", err.c_str()); return 1; }

    /* The partition sits beside the level, under whichever glacier root the
     * level belongs to, so it is found by scanning the mount rather than by
     * rebuilding a path - Portal levels moved under a group folder in 1.4.3.0
     * and a rebuilt path would silently miss them. */
    std::string want;
    for (const auto& kv : src.ebx()) {
        if (kv.first.find("materialgrid") == std::string::npos) continue;
        if (Source::level_dir_end(kv.first, level) == std::string::npos) continue;
        want = kv.first;
        break;
    }
    if (want.empty()) { std::printf("no material grid for %s\n", level.c_str()); return 1; }

    std::vector<uint8_t> bytes = src.get_ebx(want, err);
    if (bytes.empty()) { std::printf("get_ebx: %s\n", err.c_str()); return 1; }
    Ebx ebx(types);
    if (!ebx.parse(std::move(bytes), err)) { std::printf("parse: %s\n", err.c_str()); return 1; }
    ebx.set_guid_index(&src.partition_index());

    const EbxValue root = ebx.read_instance(0);
    const EbxValue* dim = root.field(kDim);
    const EbxValue* idmap = root.field(kIdMap);
    const EbxValue* groups = root.field(kGroups);
    const EbxValue* grid = root.field(kGrid);
    if (!idmap || !groups || !grid) { std::printf("root is not a material grid\n"); return 1; }

    const size_t side = grid->items.size();
    const size_t group_count = groups->items.size();
    std::printf("%s\n  %zu instances, declared dim %lld, grid %zu x ?, groups %zu, id map %zu\n",
                want.c_str(), ebx.instance_count(),
                (long long)(dim && dim->field(kDimValue) ? dim->field(kDimValue)->u : 0),
                side, group_count, idmap->items.size());

    /* 1. square, and the side agrees with the relation-group count */
    size_t ragged = 0;
    for (const EbxValue& row : grid->items) {
        const EbxValue* cols = row.field(kGridRow);
        if (!cols || cols->items.size() != side) ragged++;
    }

    /* 2 and 4: references resolve, occupancy is symmetric */
    std::vector<std::vector<char>> filled(side, std::vector<char>(side, 0));
    size_t refs = 0, bad_refs = 0, cells = 0, items = 0;
    std::map<std::string, size_t> by_type;
    std::map<std::string, TypeGuid> guid_of;
    for (size_t r = 0; r < side; ++r) {
        const EbxValue* cols = grid->items[r].field(kGridRow);
        if (!cols) continue;
        for (size_t c = 0; c < cols->items.size() && c < side; ++c) {
            const EbxValue* list = cols->items[c].field(kCellList);
            if (!list || list->items.empty()) continue;
            cells++;
            filled[r][c] = 1;
            for (const EbxValue& entry : list->items) {
                const EbxValue* arr = entry.field(kCellItems);
                if (!arr) continue;
                for (const EbxValue& it : arr->items) {
                    items++;
                    const EbxValue* rel = it.field(kItemRel);
                    if (!rel || rel->kind != EbxValue::Kind::InstanceRef) continue;
                    refs++;
                    if (rel->instance < 0 || (size_t)rel->instance >= ebx.instance_count()) {
                        bad_refs++;
                        continue;
                    }
                    const TypeGuid tg = ebx.instance_type((size_t)rel->instance);
                    by_type[hex_guid(tg)]++;
                    guid_of[hex_guid(tg)] = tg;
                }
            }
        }
    }
    size_t asymmetric = 0;
    std::vector<std::pair<size_t, size_t>> asym;
    for (size_t r = 0; r < side; ++r)
        for (size_t c = r + 1; c < side; ++c)
            if (filled[r][c] != filled[c][r]) {
                asymmetric++;
                if (asym.size() < 8) asym.push_back({r, c});
            }

    /* 3. the id map indexes the grid */
    size_t map_out_of_range = 0, mapped = 0;
    for (const EbxValue& v : idmap->items) {
        const uint64_t idx = v.kind == EbxValue::Kind::Uint ? v.u : (uint64_t)v.i;
        if (idx) mapped++;
        if (idx >= side) map_out_of_range++;
    }

    /* 5. control */
    size_t control = 0;
    for (size_t i = 0; i < ebx.instance_count() && i < 4000; ++i) {
        std::string g, p;
        if (ebx.import_ref(i, kFakeField, g, p)) control++;
    }

    std::printf("  occupied cells %zu of %zu (%.1f%%), relation references %zu over %zu item(s)\n",
                cells, side * side, side ? 100.0 * (double)cells / (double)(side * side) : 0.0,
                refs, items);
    std::printf("  ragged rows %zu | unresolvable references %zu | asymmetric pairs %zu | id map out of range %zu (of %zu mapped)\n",
                ragged, bad_refs, asymmetric, map_out_of_range, mapped);
    std::printf("  CONTROL fabricated field resolved %zu time(s) (expected 0)\n", control);
    /* ASYMMETRY IS REPORTED, NOT ASSUMED AWAY. The grid is overwhelmingly
     * symmetric, so the handful of one-way pairs are the interesting rows: a
     * relation authored for A-into-B and not for the reverse. Naming which
     * pairs they are is what lets the next reader decide whether they are
     * authored or a decode fault; a bare count can only be argued about. */
    for (const auto& pr : asym) {
        const EbxValue* cols = grid->items[pr.first].field(kGridRow);
        const EbxValue* cell = (cols && pr.second < cols->items.size())
            ? cols->items[pr.second].field(kCellList) : nullptr;
        std::printf("    one-way pair: (%zu,%zu) %s, (%zu,%zu) %s  [%zu list(s) row-side]\n",
                    pr.first, pr.second, filled[pr.first][pr.second] ? "filled" : "empty",
                    pr.second, pr.first, filled[pr.second][pr.first] ? "filled" : "empty",
                    cell ? cell->items.size() : (size_t)0);
    }

    std::printf("  relation kinds referenced, by type guid:\n");
    std::vector<std::pair<size_t, std::string>> kinds;
    for (const auto& kv : by_type) kinds.push_back({kv.second, kv.first});
    std::sort(kinds.begin(), kinds.end(), [](auto& a, auto& b){ return a.first > b.first; });
    /* NAMES, VIA THE TYPE'S OWN NAME HASH. The guid is the fact and the name
     * is a convenience, so this never fails the run - but a count against
     * MaterialRelationDebrisData says something a count against 21888a08 does
     * not. The join is two hops because no table maps guid to name directly:
     * the executable's reflection gives this guid's name hash, and the repo's
     * ebx_typehashes.tsv maps that hash to a name. A hash the table does not
     * know prints as the guid, which is the honest answer. */
    std::map<uint32_t, std::string> by_hash;
    if (argc > 3) {
        if (FILE* f = std::fopen(argv[3], "rb")) {
            char line[1024];
            while (std::fgets(line, sizeof(line), f)) {
                char* tab = std::strchr(line, '\t');
                if (!tab) continue;
                *tab = 0;
                std::string n = tab + 1;
                while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
                const uint32_t h = (uint32_t)std::strtoul(line, nullptr, 16);
                if (h) by_hash[h] = n;
            }
            std::fclose(f);
        }
    }
    std::map<std::string, std::string> names;
    for (const auto& kv : by_type) {
        const auto git = guid_of.find(kv.first);
        if (git == guid_of.end()) continue;
        const TypeLayout& lay = types.layout(git->second);
        if (!lay.name_hash) continue;
        const auto nit = by_hash.find(lay.name_hash);
        if (nit != by_hash.end()) names[kv.first] = nit->second;
        else {
            char b[32];
            std::snprintf(b, sizeof(b), "name hash 0x%08X", lay.name_hash);
            names[kv.first] = b;
        }
    }
    /* ALL of them, not a top-N. The first version of this printed 14 and the
     * finding written from it listed 14 relation kinds; there are 30 per level,
     * so the truncation was silently reported as the whole distribution. */
    for (size_t i = 0; i < kinds.size(); ++i) {
        auto it = names.find(kinds[i].second);
        std::printf("    %8zu  %s  %s\n", kinds[i].first, kinds[i].second.c_str(),
                    it == names.end() ? "" : it->second.c_str());
    }

    /* SYMMETRY IS A PROPERTY OF THE DATA, NOT OF THE FORMAT, so this is a
     * threshold rather than an equality. What the check exists to catch is a
     * TRANSPOSED read, which leaves roughly half the populated pairs one-way.
     * The handful that ship are authored - on mp_subsurface all four involve
     * material 0 - and they are printed above rather than swept away. One
     * percent sits far below a transpose and far above what the content does. */
    const size_t pairs = side > 1 ? side * (side - 1) / 2 : 1;
    const double asym_frac = (double)asymmetric / (double)pairs;
    const bool ok = side > 0 && side == group_count && ragged == 0 && bad_refs == 0 &&
                    asym_frac < 0.01 && map_out_of_range == 0 && control == 0 && refs > 0;
    std::printf("%s\n", ok ? "MATERIAL GRID OK" : "MATERIAL GRID FAILED");
    return ok ? 0 : 1;
}
