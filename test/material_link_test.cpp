/* Does a placed collision shape carry a MATERIAL GRID id?
 *
 *   material_link_test <game_dir> <level> [max_resources]
 *
 * The grid answers "what happens when material A meets material B". An editor
 * can only ask that if it can get from an object it can see to a material id,
 * and `bf6_phys_inst::material_packed` is the only surface-material field the
 * physics reader exposes. It has never been decoded. This measures what is in
 * it before anything is built on top of it.
 *
 * Four lanes are tested against the grid's own id space, which is the only
 * honest test available: a lane that IS a material id must land inside 0..784
 * and must map to a row on the level.
 *
 *   whole u32 | low u16 | high u16 | low byte
 *
 * THE CONTROL IS THE POINT. A narrow lane lands inside a 785-wide window by
 * luck: every low byte is under 785 by construction, so "100% in range" proves
 * nothing for that lane. So each lane also reports how many DISTINCT values it
 * takes and how many of those are ids the level actually maps. A lane that is
 * really a material id should take many distinct values, all mapped; a lane
 * that is a flag field takes a handful; a lane that is luck maps no better
 * than the id map's own density.
 */
#include "bf6_core.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

struct Lane {
    const char* name;
    unsigned int (*get)(unsigned int);
    std::set<unsigned int> values;
    long in_range = 0, mapped = 0, total = 0;
};

static unsigned int lane_u32(unsigned int v)  { return v; }
static unsigned int lane_lo16(unsigned int v) { return v & 0xFFFFu; }
static unsigned int lane_hi16(unsigned int v) { return v >> 16; }
static unsigned int lane_lo8(unsigned int v)  { return v & 0xFFu; }

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: material_link_test <game> <level> [max]\n"); return 2; }
    const char* level = argv[2];
    const int maxn = (argc > 3) ? std::atoi(argv[3]) : 300;

    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }

    bf6_material_grid_stats st{};
    if (bf6_material_grid_info(c, level, &st, err, (int)sizeof(err)) != 0) {
        std::printf("grid: %s\n", err); return 1;
    }
    /* The level's own id map, so "is this a material id" is asked of the level
     * the object is placed in and not of a constant. */
    std::vector<char> is_mapped((size_t)st.id_map_len, 0);
    int mapped_ids = 0;
    for (int id = 0; id < st.id_map_len; ++id)
        if (bf6_material_row(c, level, id, err, (int)sizeof(err)) >= 0) { is_mapped[(size_t)id] = 1; mapped_ids++; }

    if (!bf6_mount_all(c, 1, err, (int)sizeof(err))) { std::printf("mount: %s\n", err); return 1; }
    const int total = bf6_list_res(c, nullptr, nullptr, 0);
    std::vector<bf6_asset> assets((size_t)(total > 0 ? total : 0));
    const int got = bf6_list_res(c, nullptr, assets.data(), total);

    Lane lanes[4] = {
        {"whole u32", lane_u32, {}, 0, 0, 0},
        {"low u16",   lane_lo16, {}, 0, 0, 0},
        {"high u16",  lane_hi16, {}, 0, 0, 0},
        {"low byte",  lane_lo8, {}, 0, 0, 0},
    };

    int read = 0;
    long instances = 0;
    long res_a_const = 0, res_a_vary = 0, res_b_const = 0, res_b_vary = 0;
    std::set<unsigned int> raw_values;
    for (int i = 0; i < got && read < maxn; i++) {
        const bf6_asset& a = assets[(size_t)i];
        if (!a.name || a.type != 0x41759364u) continue;   /* PhysicsResource */
        bf6_physics* p = bf6_physics_read(c, a.name);
        if (!p) continue;
        read++;
        std::set<unsigned int> res_a, res_b;
        for (int n = 0; n < p->inst_count; n++) {
            const unsigned int v = p->instances[n].material_packed;
            res_a.insert((v >> 8) & 0x3FFu);
            res_b.insert((v >> 20) & 0x3FFu);
            instances++;
            if (raw_values.size() < 4096) raw_values.insert(v);
            for (Lane& L : lanes) {
                const unsigned int x = L.get(v);
                L.total++;
                if (L.values.size() < 4096) L.values.insert(x);
                if (x < (unsigned int)st.id_map_len) {
                    L.in_range++;
                    if (is_mapped[x]) L.mapped++;
                }
            }
        }
        if (p->inst_count > 1) {
            (res_a.size() == 1 ? res_a_const : res_a_vary)++;
            (res_b.size() == 1 ? res_b_const : res_b_vary)++;
        }
        bf6_free(c, p);
    }

    std::printf("per-resource structure: %ld resource(s) where lane B is CONSTANT across every\n"
                "  instance, %ld where it varies; lane A constant %ld, varies %ld.\n"
                "  A lane that is constant per resource is an object-level default; the lane\n"
                "  that varies per instance is the per-shape surface.\n\n",
                res_b_const, res_b_vary, res_a_const, res_a_vary);

    std::printf("%s: grid side %d, id space %d, mapped ids %d (%.1f%% of the space)\n",
                level, st.side, st.id_map_len, mapped_ids,
                st.id_map_len ? 100.0 * mapped_ids / st.id_map_len : 0.0);
    std::printf("read %d physics resource(s), %ld instance(s), %zu distinct packed value(s)\n\n",
                read, instances, raw_values.size());
    if (instances == 0) { std::printf("no instances; nothing measured\n"); bf6_close(c); return 1; }

    std::printf("%-10s  %8s  %9s  %9s  %s\n", "lane", "distinct", "in range", "mapped", "verdict");
    for (const Lane& L : lanes) {
        const double inr = 100.0 * (double)L.in_range / (double)L.total;
        const double map = L.in_range ? 100.0 * (double)L.mapped / (double)L.in_range : 0.0;
        const char* verdict;
        if (L.name[0] == 'l' && L.name[4] == 'b')            /* low byte: in range by construction */
            verdict = "in range BY CONSTRUCTION, proves nothing";
        else if (inr < 99.0)                                  verdict = "escapes the id space: NOT a material id";
        else if (L.values.size() < 8)                         verdict = "too few distinct values: looks like flags";
        else if (map > 99.0)                                  verdict = "in range and mapped: CANDIDATE";
        else                                                  verdict = "in range but unmapped values: doubtful";
        std::printf("%-10s  %8zu  %8.1f%%  %8.1f%%  %s\n",
                    L.name, L.values.size(), inr, map, verdict);
    }

    /* WHICH BITS ACTUALLY VARY. Lane boundaries are read off the data rather
     * than guessed: a bit that never changes across 10,000 instances is not
     * part of an index, and a run of varying bits is a candidate lane. */
    unsigned int ones = 0u, zeros = 0u;
    for (unsigned int v : raw_values) { ones |= v; zeros |= ~v; }
    const unsigned int varying = ones & zeros;
    std::printf("\nbit map (31..0), '.' = constant across every instance:\n  ");
    for (int b = 31; b >= 0; --b) {
        std::printf("%c", (varying >> b) & 1u ? 'x' : ((ones >> b) & 1u ? '1' : '0'));
        if (b % 8 == 0) std::printf(" ");
    }
    std::printf("\n  constant-1 mask 0x%08X, varying mask 0x%08X\n",
                ones & ~varying, varying);

    /* THE WHOLE POINT, end to end: a real placed object's packed material,
     * unpacked, asked of the grid. If this answers nothing the chain is
     * useless no matter how clean the bit layout looks. */
    std::printf("\nend to end (real collision instances -> grid answers):\n");
    int asked = 0, answered = 0, shown_cats = 0;
    long total_rel = 0;
    for (unsigned int v : raw_values) {
        bf6_surface_material sm{};
        bf6_material_unpack(v, &sm);
        if (!sm.reserved_clear) continue;
        const int n = bf6_material_relations(c, level, sm.material_a, sm.material_b,
                                             nullptr, 0, err, (int)sizeof(err));
        if (n < 0) continue;
        asked++;
        if (n > 0) { answered++; total_rel += n; }
        if (n > 0 && shown_cats < 6) {
            std::vector<bf6_material_relation> rel((size_t)n);
            bf6_material_relations(c, level, sm.material_a, sm.material_b, rel.data(), n,
                                   err, (int)sizeof(err));
            std::printf("  0x%08X -> materials %3d + %3d : %2d relation(s)  ",
                        v, sm.material_a, sm.material_b, n);
            std::set<std::string> cats;
            for (int i = 0; i < n; ++i)
                cats.insert(rel[(size_t)i].category[0] ? rel[(size_t)i].category : "(uncategorised)");
            for (const std::string& s : cats) std::printf("%s ", s.c_str());
            std::printf("\n");
            shown_cats++;
        }
    }
    std::printf("  %d of %d packed values name a pair with relations (%ld relation(s) total).\n"
                "  Low by design: the grid is a surface meeting ANOTHER surface, so an\n"
                "  object's own two lanes are not a meaningful pair. The row is the answer.\n",
                answered, asked, total_rel);

    /* THE CALL AN EDITOR ACTUALLY MAKES: one clicked object, one material, what
     * that surface does against everything else on the level. */
    /* DO THE TWO LANES BEHAVE DIFFERENTLY? If one lane's materials have rich,
     * mixed rows and the other's are thin and single-category, the lanes are
     * different KINDS of material and the editor must not treat them alike.
     * Averaged over every distinct packed value, not eyeballed from a few. */
    std::printf("\nrow profiles by lane (averaged over the distinct packed values):\n");
    for (int lane = 0; lane < 2; ++lane) {
        long rows = 0, partners = 0, relations = 0;
        long cat_tot[BF6_MATERIAL_CATEGORIES] = {0};
        std::set<int> ids;
        for (unsigned int v : raw_values) {
            bf6_surface_material sm{};
            bf6_material_unpack(v, &sm);
            if (!sm.reserved_clear) continue;
            const int id = lane ? sm.material_b : sm.material_a;
            if (id == 0 || !ids.insert(id).second) continue;
            bf6_material_profile pr{};
            if (bf6_material_profile_get(c, level, id, &pr, err, (int)sizeof(err)) < 0) continue;
            rows++; partners += pr.partners; relations += pr.relations;
            for (int i = 0; i < BF6_MATERIAL_CATEGORIES; ++i) cat_tot[i] += pr.by_category[i];
        }
        if (!rows) { std::printf("  lane %c: no material\n", lane ? 'b' : 'a'); continue; }
        std::printf("  lane %c: %ld distinct material(s), mean %.1f partner(s), mean %.1f relation(s)  ",
                    lane ? 'b' : 'a', rows, (double)partners / rows, (double)relations / rows);
        for (int i = 0; i < BF6_MATERIAL_CATEGORIES; ++i) {
            const char* nm = bf6_material_category_name(i);
            if (nm && cat_tot[i]) std::printf("%s:%ld ", nm, cat_tot[i]);
        }
        std::printf("\n");
    }

    std::printf("\nall %zu distinct packed value(s):\n", raw_values.size());
    int shown = 0;
    for (unsigned int v : raw_values) {
        std::printf("0x%08X%s", v, (++shown % 8) ? " " : "\n");
    }
    std::printf("\n");
    bf6_close(c);
    return 0;
}
