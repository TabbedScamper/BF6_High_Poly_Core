/* THE EMITTER BUDGET, checked: bf6_fx_budget_keep.
 *
 *   fx_budget_test
 *
 * This decides which emitters Godot and Unreal draw when a level wants more
 * than a preview can carry. It is in the core precisely so the two agree, so
 * what matters is not that it trims but that it trims EXACTLY and EVENLY -
 * a rule that keeps 2,999 on one editor and 3,001 on the other, or that keeps
 * a contiguous prefix, would be worse than no rule at all.
 *
 * Asserted:
 *  1. Under the cap nothing is dropped.
 *  2. Over the cap exactly `budget` survive, for many awkward ratios.
 *  3. The survivors are SPREAD: every tenth of the range keeps roughly a tenth
 *     of the budget. This is the property a prefix truncation fails, and it is
 *     the one that decides whether a trimmed map still reads as the map.
 *  4. It is a pure function of (index, total, budget) - no state, no order
 *     dependence - which is what lets two processes agree without talking.
 *  5. Out-of-range indices keep nothing rather than wrapping.
 */
#include "bf6_core.h"

#include <cstdio>
#include <vector>

static int fails = 0;

static void check(bool ok, const char* what)
{
    if (!ok) { std::printf("  FAIL %s\n", what); fails++; }
}

int main(void)
{
    const int dflt = bf6_fx_budget_default();
    std::printf("default budget %d\n", dflt);
    check(dflt > 0, "default budget is positive");

    /* 1. under the cap */
    int kept = 0;
    for (int i = 0; i < 500; ++i) kept += bf6_fx_budget_keep(i, 500, 3000);
    std::printf("  500 candidates, cap 3000 -> kept %d\n", kept);
    check(kept == 500, "nothing trimmed below the cap");

    /* 2. exactly `budget` survive */
    const int totals[] = {3001, 4000, 7671, 23013, 100000, 3000};
    const int budgets[] = {3000, 1, 17, 2999, 3000, 3000};
    for (int t = 0; t < 6; ++t)
    {
        for (int b = 0; b < 6; ++b)
        {
            const int total = totals[t], budget = budgets[b];
            int n = 0;
            for (int i = 0; i < total; ++i) n += bf6_fx_budget_keep(i, total, budget);
            const int want = total <= budget ? total : budget;
            if (n != want)
            {
                std::printf("  FAIL total=%d budget=%d kept %d, wanted %d\n",
                            total, budget, n, want);
                fails++;
            }
        }
    }
    std::printf("  exact-count holds over 36 total/budget pairs\n");

    /* 3. SPREAD, not a prefix. */
    {
        const int total = 23013, budget = 3000;
        int bucket[10] = {0};
        for (int i = 0; i < total; ++i)
            if (bf6_fx_budget_keep(i, total, budget))
                bucket[(int)((long long)i * 10 / total)]++;
        const int want = budget / 10;
        int worst = 0;
        for (int k = 0; k < 10; ++k)
        {
            const int d = bucket[k] > want ? bucket[k] - want : want - bucket[k];
            if (d > worst) worst = d;
        }
        std::printf("  23013 -> 3000: per-tenth counts");
        for (int k = 0; k < 10; ++k) std::printf(" %d", bucket[k]);
        std::printf(" (worst deviation %d of %d)\n", worst, want);
        /* A prefix truncation would put 3000 in the first two tenths and 0 in
         * the other eight, so this is the assertion that tells them apart. */
        check(worst <= 2, "survivors are spread evenly across the range");
    }

    /* 4. purity: the same three numbers always give the same answer, and the
     * answer does not depend on having been asked about earlier indices. */
    {
        bool stable = true;
        for (int i = 0; i < 4000; i += 7)
        {
            const int a = bf6_fx_budget_keep(i, 7671, 3000);
            const int b = bf6_fx_budget_keep(i, 7671, 3000);
            if (a != b) { stable = false; break; }
        }
        check(stable, "pure function of its arguments");
        /* backwards, to catch any hidden cursor */
        int fwd = 0, rev = 0;
        for (int i = 0; i < 7671; ++i) fwd += bf6_fx_budget_keep(i, 7671, 3000);
        for (int i = 7670; i >= 0; --i) rev += bf6_fx_budget_keep(i, 7671, 3000);
        check(fwd == rev, "order of questioning does not change the answer");
    }

    /* 5. out of range */
    check(bf6_fx_budget_keep(-1, 100, 10) == 0, "negative index keeps nothing");
    check(bf6_fx_budget_keep(100, 100, 10) == 0, "index past the end keeps nothing");
    check(bf6_fx_budget_keep(0, 0, 10) == 0, "empty list keeps nothing");
    /* budget <= 0 means "use the default", not "keep nothing" */
    {
        int n = 0;
        for (int i = 0; i < 5000; ++i) n += bf6_fx_budget_keep(i, 5000, 0);
        std::printf("  budget 0 falls back to the default -> kept %d\n", n);
        check(n == dflt, "budget 0 means the default cap");
    }

    std::printf("\n%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
