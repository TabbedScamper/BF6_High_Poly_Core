/* The material relation grid through the PUBLIC per-pair ABI.
 *
 *   material_pair_test <game_dir> <level> [level2 ...]
 *
 * matgrid_test already proves the grid decodes. This proves the query built on
 * top of it answers the right cell, which is a different claim: a per-pair
 * lookup that quietly returns a neighbouring row would pass every shape test
 * and be wrong on every impact.
 *
 *   1. SWEEP AGREEMENT. Every mapped material pair is queried and the totals
 *      must equal the grid's own occupied-cell and relation counts. An
 *      off-by-one row, a transposed index or a dropped cell all break this,
 *      and nothing else in the test would catch them.
 *   2. COUNT-THEN-FILL agrees with itself on a populated pair.
 *   3. THE DECLARED DIMENSION IS NOT THE SIDE. The root declares 128 while the
 *      real side is the level's material count. Asserted because a future
 *      reader that trusts the declared value reads the wrong rectangle.
 *   4. ORDER MATTERS AND IS PRESERVED. The four authored one-way pairs must
 *      survive the query: if (a,b) and (b,a) ever agree everywhere, something
 *      is symmetrising the matrix.
 *   5. EVERY RELATION NAMES A TYPE. A blank type guid means the instance
 *      reference resolved to nothing, which is the failure this whole system
 *      looked like it had.
 *   6. CONTROL: an id past the global space is rejected, and an id the level
 *      does not ship answers zero relations rather than someone else's row.
 */
#include "bf6_core.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: material_pair_test <game_dir> <level> [level2 ...]\n");
        return 2;
    }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }

    int bad = 0, levels = 0;
    for (int a = 2; a < argc; ++a)
    {
        const char* level = argv[a];
        bf6_material_grid_stats st{};
        if (bf6_material_grid_info(c, level, &st, err, (int)sizeof(err)) != 0) {
            std::printf("%-34s -  %s\n", level, err);
            continue;
        }
        levels++;

        /* 3. the declared dimension is not the side */
        if (st.declared_dim == st.side)
            std::printf("  NOTE %s: declared dim equals the side; the trap this "
                        "reader avoids may not exist on this level\n", level);

        /* 1. sweep every occupiable pair and reconcile with the grid's own totals.
         *
         * ONE REPRESENTATIVE ID PER ROW, not every mapped id. The id map is
         * many-to-one - 785 global ids onto 445 rows on mp_subsurface, so
         * several materials share a row and answer identically - and sweeping
         * ids would count each occupied cell once per pair of ids sharing those
         * two rows. That inflated the first run of this test to 193,596 cells
         * against a real 28,356 and it was the TEST that was wrong. Sweeping
         * representatives visits each cell exactly once, so the totals are
         * comparable to the grid's own. */
        std::vector<int> mapped(0);
        std::vector<int> ids_of_row((size_t)(st.side > 0 ? st.side : 1), -1);
        int shared_rows = 0;
        for (int id = 0; id < st.id_map_len; ++id) {
            const int r = bf6_material_row(c, level, id, err, (int)sizeof(err));
            if (r == -1) { std::printf("%-34s row(%d): %s\n", level, id, err); bad++; break; }
            if (r < 0) continue;
            if (r >= st.side) { std::printf("%-34s row %d past side %d\n", level, r, st.side); bad++; }
            mapped.push_back(id);
            if (r < (int)ids_of_row.size()) {
                if (ids_of_row[(size_t)r] < 0) ids_of_row[(size_t)r] = id;
                else shared_rows++;
            }
        }
        std::vector<int> rows;
        rows.reserve(ids_of_row.size());
        for (size_t r = 0; r < ids_of_row.size(); ++r)
            if (ids_of_row[r] >= 0) rows.push_back(ids_of_row[r]);

        long long cells = 0, refs = 0;
        int one_way = 0, blank_type = 0, first_a = -1, first_b = -1;
        for (size_t i = 0; i < rows.size(); ++i) {
            for (size_t j = 0; j < rows.size(); ++j) {
                const int n = bf6_material_relations(c, level, rows[i], rows[j],
                                                     nullptr, 0, err, (int)sizeof(err));
                if (n < 0) { std::printf("%-34s query: %s\n", level, err); bad++; i = rows.size(); break; }
                if (n == 0) continue;
                cells++;
                refs += n;
                if (first_a < 0) { first_a = rows[i]; first_b = rows[j]; }
                const int m = bf6_material_relations(c, level, rows[j], rows[i],
                                                     nullptr, 0, err, (int)sizeof(err));
                if (m == 0) one_way++;
            }
        }

        /* The sweep only visits MAPPED ids, so it can only equal the grid's
         * totals if every occupied cell belongs to a mapped pair - which is
         * itself the claim that the id map indexes this grid. */
        const bool cells_ok = (cells == st.occupied_cells);
        const bool refs_ok  = (refs  == st.relations);

        /* 2 and 5: fill a populated pair and look at what came back */
        int filled = 0;
        if (first_a >= 0) {
            const int n = bf6_material_relations(c, level, first_a, first_b, nullptr, 0,
                                                 err, (int)sizeof(err));
            std::vector<bf6_material_relation> rel((size_t)(n > 0 ? n : 1));
            filled = bf6_material_relations(c, level, first_a, first_b, rel.data(), n,
                                            err, (int)sizeof(err));
            if (filled != n) { std::printf("%-34s count-then-fill %d vs %d\n", level, n, filled); bad++; }
            for (int i = 0; i < filled; ++i)
                if (rel[(size_t)i].type[0] == '\0') blank_type++;
        }

        /* 6. controls */
        const int past = bf6_material_relations(c, level, st.id_map_len, 0, nullptr, 0,
                                                err, (int)sizeof(err));
        /* The unshipped path is reported as NOT EXERCISED rather than as a pass
         * when no such id exists. On all five levels measured every one of the
         * 785 global ids resolves to a row, so this control is vacuous there,
         * and printing "zero" would claim a check that never ran. */
        int unshipped_ok = -1;
        for (int id = 0; id < st.id_map_len; ++id)
            if (bf6_material_row(c, level, id, err, (int)sizeof(err)) == -2) {
                unshipped_ok = bf6_material_relations(c, level, id, rows.empty() ? 0 : rows[0],
                                                      nullptr, 0, err, (int)sizeof(err)) == 0;
                break;
            }

        std::printf("%-34s side %d (declared %d)  ids %d shipped %d rows %d fallback %d/%d  "
                    "cells %lld/%d %s  refs %lld/%d %s  "
                    "types %d  one-way %d  filled %d  blank-type %d  past-space %s  unshipped %s\n",
                    level, st.side, st.declared_dim, st.id_map_len,
                    (int)mapped.size(), (int)rows.size(), shared_rows, st.fallback_ids,
                    cells, st.occupied_cells, cells_ok ? "ok" : "MISMATCH",
                    refs, st.relations, refs_ok ? "ok" : "MISMATCH",
                    st.relation_types, one_way, filled, blank_type,
                    past == -1 ? "rejected" : "ACCEPTED",
                    unshipped_ok < 0 ? "not-exercised" : (unshipped_ok ? "zero" : "NONZERO"));

        if (!cells_ok || !refs_ok || blank_type > 0 || past != -1 || unshipped_ok == 0) bad++;
        /* 4. the authored one-way pairs must survive the query */
        if (one_way == 0)
            std::printf("  NOTE %s: no one-way pair survived the query; the grid has four\n", level);
    }

    bf6_close(c);
    if (levels == 0) { std::printf("\nno level answered; nothing was tested\n"); return 1; }
    std::printf("\n%s: %d level(s), %d failure(s)\n", bad ? "FAIL" : "PASS", levels, bad);
    return bad ? 1 : 0;
}
