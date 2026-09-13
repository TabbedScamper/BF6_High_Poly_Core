#include "bf6_core.h"

#include <cstring>
#include <cstdio>
#include <vector>

static bool ascii_equal_fold(const char* a, const char* b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        unsigned char ac = static_cast<unsigned char>(*a++);
        unsigned char bc = static_cast<unsigned char>(*b++);
        if (ac >= 'a' && ac <= 'z') ac = static_cast<unsigned char>(ac - 32);
        if (bc >= 'a' && bc <= 'z') bc = static_cast<unsigned char>(bc - 32);
        if (ac != bc) return false;
    }
    return *a == '\0' && *b == '\0';
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: localized_reverse_test <game-dir>\n");
        return 2;
    }
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, sizeof(error));
    if (!context) {
        std::fprintf(stderr, "open: %s\n", error);
        return 1;
    }
    if (!bf6_mount_all(context, 1, error, sizeof(error))) {
        std::fprintf(stderr, "mount: %s\n", error);
        bf6_close(context);
        return 1;
    }
    struct Probe { const char* text; bool install_required; };
    const Probe labels[] = {
        {"BULLETIN", true}, {"QUICKPLAY", false}, {"QUICK PLAY", false},
        {"TOP GUN", false}, {"MULTIPLAYER", true}, {"RANKED", true},
        {"REDSEC", true}, {"PORTAL", true}, {"CAMPAIGN", true},
        {"TRAINING", true}, {"TRAINING GROUND", false},
        {"TRAINING GROUNDS", false}, {"FIRING RANGE", true},
        {"CONQUEST", false}, {"BREAKTHROUGH", false}, {"RUSH", false},
        {"CUSTOM SEARCH", false}, {"CARRIER STRIKE", false},
        {"FIGHTER SWEEP", false}, {"BATTLEFIELD PORTAL", false},
        {"PLAY FOR FREE", false}, {"XP BOOST", false},
        {"SERVER BROWSER", false},
        {"Find a server that's just right for you.", false},
        {"PORTAL DISCOVERY", false},
        {"SEARCH", false}, {"SERVER", false},
        {"SERVER HOSTING", false}, {"PERSISTENT SERVERS", false},
        {"WINTER OFFENSIVE CONQUEST", false},
        {"GULF OF LYNDON: NIGHT OPS CONQUEST LARGE", false},
        {"HARDCORE BREAKTHROUGH", false}
    };
    bool passed = true;
    for (const Probe& probe : labels) {
        const int count = bf6_localized_string_ids_by_text(
            context, probe.text, nullptr, 0);
        std::vector<uint32_t> ids(count > 0 ? static_cast<size_t>(count) : 0u);
        const int filled = count > 0
            ? bf6_localized_string_ids_by_text(
                  context, probe.text, ids.data(), count)
            : count;
        bool roundTrip = filled == count;
        for (uint32_t id : ids) {
            const char* localized = bf6_localized_string(context, id);
            roundTrip = roundTrip && localized &&
                        ascii_equal_fold(localized, probe.text);
        }
        if ((probe.install_required && count <= 0) || !roundTrip)
            passed = false;
        std::printf("%-18s matches=%d required=%d roundtrip=%d",
                    probe.text, count, probe.install_required ? 1 : 0,
                    roundTrip ? 1 : 0);
        for (uint32_t id : ids) std::printf(" %08X", id);
        std::printf("\n");
    }
    const int control = bf6_localized_string_ids_by_text(
        context, "__BF6_HOME_SECTION_CONTROL_MISSING__", nullptr, 0);
    std::printf("control=%d %s\n", control,
                passed && control == 0 ? "PASS" : "FAIL");
    bf6_close(context);
    return passed && control == 0 ? 0 : 1;
}
