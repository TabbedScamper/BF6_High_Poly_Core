/* ui_string_id_hash_live_test - the symbolic localisation key, live.
 *
 *   ui_string_id_hash_live_test <game-dir> [root-partition ...]
 *
 * Rime labels carry a SYMBOLIC StringId ("ID_MENUSOLDIER_NOPOINTAVAILABLE")
 * and the shipped US-English table is keyed by u32.  The current Steam
 * executable's localisation service joins the two by hashing the whole id
 * with seed 0xFFFFFFFF and h = h*33 + byte (bf6_string_id_hash).  This test
 * proves that join on the installed game with no exported table:
 *
 *   1. a fixed vector: the hash of the weapon screen's own
 *      NoPointAvailable id must be 0x93AB5399 and must resolve to text;
 *   2. every label with an authored StringId on each requested root (default:
 *      the weapon screen and the home screen) is hashed and looked up;
 *   3. controls through the identical path: the same ids with one character
 *      changed, the same ids under the ordinary 5381 seed, and the same ids
 *      under the XOR form of the loop.  All three must resolve nothing.
 *
 * Exit 0 when the vector holds, at least one live label resolves, and all
 * three controls are zero; 1 otherwise.  Counts are printed either way so a
 * regression is visible as numbers, not as a silent pass.
 */
#include "bf6_core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

static uint32_t seed5381(const std::string& s)
{
    uint32_t h = 5381u;
    for (unsigned char ch : s) h = h * 33u + ch;
    return h;
}

static uint32_t xorform(const std::string& s)
{
    uint32_t h = 0xFFFFFFFFu;
    for (unsigned char ch : s) h = (h * 33u) ^ ch;
    return h;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: ui_string_id_hash_live_test <game-dir> [root ...]\n");
        return 2;
    }
    char err[512]{};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::fprintf(stderr, "open: %s\n", err); return 1; }
    if (!bf6_mount_frontend(c, err, (int)sizeof(err)))
    { std::fprintf(stderr, "mount: %s\n", err); bf6_close(c); return 1; }

    int failures = 0;

    /* 1. Fixed vector.  The value is what the executable's loop produces for
     * this id; the text is what the installed table holds under that key. */
    const char* vectorId = "ID_MENUSOLDIER_NOPOINTAVAILABLE";
    const uint32_t vectorHash = bf6_string_id_hash(vectorId);
    const char* vectorText = bf6_localized_string_by_id(c, vectorId);
    std::printf("vector %s -> %08X text=%s\n", vectorId, vectorHash,
                vectorText ? vectorText : "<none>");
    if (vectorHash != 0x93AB5399u) { std::printf("FAIL: vector hash\n"); ++failures; }
    if (!vectorText || !*vectorText) { std::printf("FAIL: vector text\n"); ++failures; }
    if (bf6_string_id_hash(nullptr) != 0 || bf6_string_id_hash("") != 0)
    { std::printf("FAIL: null/empty must hash to 0\n"); ++failures; }

    /* 2 + 3. Live labels on each root. */
    std::vector<std::string> roots;
    for (int i = 2; i < argc; ++i) roots.emplace_back(argv[i]);
    if (roots.empty())
    {
        roots.emplace_back("common/ui/weapons/screens/menuweaponscreen");
        roots.emplace_back("common/ui/home/screens/home_screen");
    }

    int totalLabels = 0, withId = 0, resolved = 0;
    int mutated = 0, seeded = 0, xored = 0;
    std::set<std::string> distinct;
    for (const std::string& root : roots)
    {
        bf6_rime_tree_stats stats{};
        const int n = bf6_rime_tree(c, root.c_str(), 0, nullptr, 0, &stats);
        if (n < 0) { std::printf("FAIL: cannot read %s\n", root.c_str()); ++failures; continue; }
        std::vector<bf6_rime_node> rows((size_t)n);
        bf6_rime_tree(c, root.c_str(), 0, rows.data(), n, &stats);
        int rootLabels = 0, rootIds = 0, rootHits = 0;
        for (const bf6_rime_node& r : rows)
        {
            if (r.kind != BF6_RIME_LABEL) continue;
            ++rootLabels;
            if (!r.text_string_id[0]) continue;
            ++rootIds;
            const std::string id = r.text_string_id;
            const char* text = bf6_localized_string_by_id(c, id.c_str());
            if (text) ++rootHits;
            if (distinct.insert(id).second)
                std::printf("  %-48s %08X %s\n", id.c_str(), bf6_string_id_hash(id.c_str()),
                            text ? text : "<not in table>");
            /* controls: same id, one character changed; same id, 5381 seed;
             * same id, XOR loop. */
            std::string m = id; m[m.size() - 1] = (m[m.size() - 1] == 'A') ? 'B' : 'A';
            if (bf6_localized_string_by_id(c, m.c_str())) ++mutated;
            if (bf6_localized_string(c, seed5381(id))) ++seeded;
            if (bf6_localized_string(c, xorform(id))) ++xored;
        }
        std::printf("%s: rows=%d labels=%d authored-id=%d resolved=%d\n",
                    root.c_str(), n, rootLabels, rootIds, rootHits);
        totalLabels += rootLabels; withId += rootIds; resolved += rootHits;
    }
    std::printf("TOTAL labels=%d authored-id=%d resolved=%d | controls: mutated=%d seed5381=%d xor=%d\n",
                totalLabels, withId, resolved, mutated, seeded, xored);
    if (resolved < 1) { std::printf("FAIL: no live label resolved\n"); ++failures; }
    if (mutated || seeded || xored) { std::printf("FAIL: a control resolved\n"); ++failures; }

    bf6_close(c);
    std::printf(failures ? "FAIL (%d)\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}
