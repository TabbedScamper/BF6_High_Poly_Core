/* WHERE THE SOLDIER'S MOVEMENT NUMBERS LIVE.
 *
 *   soldier_movement_probe <game_dir> [stem...]
 *
 * The editors' walk mode runs on Unreal's character defaults - 2.6 m/s walking,
 * 6.0 running, 4.2 jump, 1.72 m eye - and says so in its own source. Plausible,
 * hand-picked, and not the game's. This looks for the authored values.
 *
 * Step one is only ever NAMES. Reading a record by fixed offset is the trap the
 * header warns about, and guessing a partition name is the same trap one level
 * up, so this lists what the install actually has against a spread of stems and
 * prints the counts. What comes back decides where to dump fields next.
 */
#include "bf6_core.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

/* Terms a Frostbite soldier's movement could plausibly be filed under. Wide on
 * purpose: this is a search, and a stem that returns nothing is a cheap answer. */
const char* const kStems[] = {
    "characterphysics", "charactermovement", "soldierbody", "soldierentity",
    "characterentity", "soldiermovement", "movementsettings", "sprint",
    "stancesettings", "posesettings", "characterpose", "soldierpose",
    "playerphysics", "walkspeed", "soldierspeed", "characterstate",
    "soldier_common", "soldiersettings", "playermovement", "locomotion",
};

/* THE DUMP PRINTS FLOATS AS THEIR BIT PATTERN when the schema has not named the
 * field's type, so a move-speed multiplier of 1.0 arrives as 1065353216 and
 * reads like a nonsense integer. This annotates any integer that reinterprets
 * to a sane float, which is what makes these records legible at all.
 *
 * Deliberately conservative: only magnitudes between 1e-4 and 1e5, so ordinary
 * small counts and ids are left alone rather than decorated with noise. It is a
 * READING AID, not a decode - the schema still decides what a field means. */
void print_with_floats(const char* text)
{
    const char* p = text;
    while (*p) {
        const char* eol = std::strchr(p, '\n');
        const size_t len = eol ? (size_t)(eol - p) : std::strlen(p);
        std::string line(p, len);
        /* The dump puts the value last, so look at the final token only. */
        const size_t at = line.find_last_of(" \t");
        bool annotated = false;
        if (at != std::string::npos && at + 1 < line.size()) {
            const std::string tok = line.substr(at + 1);
            bool digits = !tok.empty();
            for (size_t k = (tok[0] == '-' ? 1 : 0); k < tok.size(); ++k)
                if (tok[k] < '0' || tok[k] > '9') { digits = false; break; }
            if (digits && tok.size() >= 5) {
                const long long v = std::atoll(tok.c_str());
                if (v > INT32_MIN && v <= UINT32_MAX && v != 0) {
                    const uint32_t bits = (uint32_t)(v < 0 ? (uint32_t)(int32_t)v : (uint32_t)v);
                    float f = 0.f;
                    std::memcpy(&f, &bits, 4);
                    const float a = f < 0 ? -f : f;
                    if (a >= 1e-4f && a <= 1e5f) {
                        std::printf("%s   (= %g)\n", line.c_str(), (double)f);
                        annotated = true;
                    }
                }
            }
        }
        if (!annotated) std::printf("%s\n", line.c_str());
        if (!eol) break;
        p = eol + 1;
    }
}

/* The field carrying a tweakable's authored value, and the one carrying its
 * name. Verified against five entries of different kinds (a multiplier, a
 * velocity cap, an impulse length, a height) rather than inferred from one. */
const char* const kValueField = "0x42c8b257";
const char* const kNameField  = "0x0c59fa06";

/* Name = value for every tweakable in a GameRemixer dump, in file order. */
void tweakables(const char* text)
{
    std::string name, value;
    int printed = 0, orphan = 0;
    const char* p = text;
    auto flush = [&]() {
        if (!name.empty() && !value.empty()) { std::printf("%-56s %s\n", name.c_str(), value.c_str()); ++printed; }
        else if (!name.empty() || !value.empty()) ++orphan;
        name.clear();
        value.clear();
    };
    while (*p) {
        const char* eol = std::strchr(p, '\n');
        const std::string line(p, eol ? (size_t)(eol - p) : std::strlen(p));
        /* A new instance ends the previous one. */
        if (line.find("] type ") != std::string::npos) flush();
        const size_t at = line.find_last_of(" \t");
        const std::string tail = at == std::string::npos ? std::string() : line.substr(at + 1);
        if (line.find(kNameField) != std::string::npos && tail.size() > 2 && tail[0] == '"') {
            std::string n = tail.substr(1, tail.size() - 2);
            /* The partition names itself in the same field; that row is the
             * record's own path, not a tweakable. */
            if (n.find('/') == std::string::npos) name = n;
        } else if (line.find(kValueField) != std::string::npos && !tail.empty()) {
            value = tail;
        }
        if (!eol) break;
        p = eol + 1;
    }
    flush();
    std::printf("\n%d tweakable(s); %d instance(s) had a name or a value but not both\n",
                printed, orphan);
}

void show(bf6_ctx* c, const char* stem, int cap)
{
    const int n = bf6_list_ebx(c, stem, nullptr, 0);
    if (n <= 0) { std::printf("%-22s -\n", stem); return; }
    std::printf("%-22s %d\n", stem, n);
    const int want = n < cap ? n : cap;
    std::vector<bf6_asset> rows((size_t)want);
    const int got = bf6_list_ebx(c, stem, rows.data(), want);
    for (int i = 0; i < got; ++i)
        std::printf("      %s\n", rows[(size_t)i].name ? rows[(size_t)i].name : "");
    if (n > got) std::printf("      ... and %d more\n", n - got);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: soldier_movement_probe <game> [stem...]\n"); return 2; }
    /* Unbuffered: a crash with a block-buffered stdout loses everything printed
     * before it, which turns "it died on stem 14" into "it printed nothing". */
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_all(c, 1, err, (int)sizeof(err));

    if (argc > 2) {
        for (int i = 2; i < argc; ++i) {
            /* "dump:<partition>" reads the record's named field tree, which is
             * the only honest way in: a fixed offset that looks like a speed in
             * one partition is the middle of a string in another, and both
             * read back as a plausible number. */
            if (std::strncmp(argv[i], "dump:", 5) == 0) {
                const char* name = argv[i] + 5;
                std::vector<char> text(4 << 20);
                const int64_t n = bf6_ebx_dump(c, name, 8, text.data(), (int)text.size());
                std::printf("\n=== dump %s (%lld bytes) ===\n", name, (long long)n);
                print_with_floats(text.data());
            } else if (std::strncmp(argv[i], "tweak:", 6) == 0) {
                /* A GameRemixer partition is a flat list of named tweakables:
                 * each instance carries its name in 0x0c59fa06 and its authored
                 * value in 0x42c8b257. That pairing is the whole decode, and it
                 * is checked rather than assumed - an instance with one and not
                 * the other is reported instead of silently skipped. */
                const char* name = argv[i] + 6;
                std::vector<char> text(16 << 20);
                const int64_t n = bf6_ebx_dump(c, name, 8, text.data(), (int)text.size());
                if (n < 0) { std::printf("dump failed: %s\n", text.data()); continue; }
                std::printf("\n=== tweakables in %s ===\n", name);
                tweakables(text.data());
            } else {
                show(c, argv[i], 60);
            }
        }
        bf6_close(c);
        return 0;
    }

    std::printf("=== partitions per stem (full mount) ===\n");
    for (const char* stem : kStems) show(c, stem, 8);
    bf6_close(c);
    return 0;
}
