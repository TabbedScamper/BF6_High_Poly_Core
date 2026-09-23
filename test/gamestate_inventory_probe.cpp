// WHAT CAN THE 1P ANIMATION GRAPH BE TOLD?
//
//   gamestate_inventory_probe <game> [filter]
//
// The runtime is driven entirely by NAMED game states:
//
//     bf6_ant_runtime_set_bool (rt, "animations/.../gamestates/<name>.bool",  v)
//     bf6_ant_runtime_set_float(rt, "animations/.../gamestates/<name>.float", v)
//
// So every stance, every sight, every trigger the graph can respond to is one of
// these names, and the ones the install actually declares are the complete list
// of what a first-person view can be asked to do. Guessing them is how the last
// three wrong turns started; this prints them.
//
// Grouped by kind, because the kind is the API: a .bool is a switch or a pulse,
// a .float is a blend axis, and the difference decides how the controller has to
// drive it.

#include "bf6_core.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::string tail(const std::string& p)
{
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

// The words that say a state is about what this tool still cannot do.
static bool interesting(const std::string& n)
{
    static const char* const want[] = {
        "crouch", "prone", "stance", "sprint", "run", "walk", "ads", "aim",
        "zoom", "sight", "fire", "shoot", "trigger", "reload", "jump", "vault",
        "lean", "melee", "speed", "moving", "velocity", "pitch", "yaw",
    };
    for (const char* w : want)
        if (n.find(w) != std::string::npos) return true;
    return false;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: %s <game dir> [filter]\n", argv[0]);
        return 2;
    }
    const std::string filter = argc > 2 ? argv[2] : std::string();

    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) {
        std::printf("FAIL: could not open the game: %s\n", err);
        return 1;
    }

    // Two passes: ask how many, then take them. A fixed buffer would silently
    // truncate the answer, and a truncated inventory is worse than none - it
    // reads as "the game has no prone state".
    const char* search = argc > 3 ? argv[3] : "gamestates";
    const int n = bf6_list_ebx(c, search, nullptr, 0);
    std::printf("assets matching \"%s\": %d\n", search, n);
    if (n <= 0) { bf6_close(c); return 0; }
    std::vector<bf6_asset> all((size_t)n);
    const int got = bf6_list_ebx(c, search, all.data(), n);
    std::printf("read back: %d\n\n", got);

    std::vector<std::string> bools, floats, ints, other;
    for (int i = 0; i < got; ++i) {
        const std::string p = all[(size_t)i].name ? all[(size_t)i].name : "";
        if (p.empty()) continue;
        const std::string nm = tail(p);
        if (!filter.empty() && nm.find(filter) == std::string::npos) continue;
        if (filter.empty() && !interesting(nm)) continue;
        if      (nm.size() > 5 && nm.compare(nm.size() - 5, 5, ".bool") == 0)  bools.push_back(p);
        else if (nm.size() > 6 && nm.compare(nm.size() - 6, 6, ".float") == 0) floats.push_back(p);
        else if (nm.size() > 4 && nm.compare(nm.size() - 4, 4, ".int") == 0)   ints.push_back(p);
        else other.push_back(p);
    }
    struct Group { const char* label; std::vector<std::string>* v; };
    const Group groups[] = {
        { "BOOL  - a switch or a one-update pulse", &bools },
        { "FLOAT - a blend axis", &floats },
        { "INT   - a discrete selector", &ints },
        { "OTHER", &other },
    };
    for (const Group& g : groups) {
        std::sort(g.v->begin(), g.v->end());
        std::printf("%s: %zu\n", g.label, g.v->size());
        // FULL PATHS. The setters and bf6_ant_runtime_create take the whole
        // asset path, and the folder is not guessable from the leaf: the same
        // leaf name lives under glacier, kingston and the mesh-local folders in
        // different versions of the install.
        for (const std::string& s : *g.v) std::printf("    %s\n", s.c_str());
        std::printf("\n");
    }
    // One full path per kind, since the setters take the whole asset path and
    // the folder is not guessable from the leaf name.
    if (!bools.empty())  std::printf("a full path looks like: %s\n", bools.front().c_str());
    if (!floats.empty()) std::printf("                        %s\n", floats.front().c_str());
    bf6_close(c);
    return 0;
}
