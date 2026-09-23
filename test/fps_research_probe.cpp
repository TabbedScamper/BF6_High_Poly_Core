// THE FIRST-PERSON SYSTEM, ENUMERATED FROM THE GAME'S OWN TABLES.
//
//   fps_research_probe <game> cdbs            every context database, in full
//   fps_research_probe <game> dump <asset>    one EBX partition through reflection
//   fps_research_probe <game> find <text>     assets whose path contains text
//   fps_research_probe <game> clips <prefix>  which clips under a prefix READ
//
// WHY THIS EXISTS. Every answer about the first-person system so far has come
// from guessing a name and seeing whether it loaded, and that has been wrong in
// both directions: bf6_list_ebx omits the 1P sprint cycle, which reads fine at
// 137 frames, while listing crawl, jump and falling clips that do not load at
// all. Absence from that listing is not evidence, and a list of guesses is not
// research.
//
// A CONTEXT DATABASE IS THE AUTHORITATIVE ANSWER. It is the table the game
// itself consults to choose an animation: its entries ARE the complete set of
// assets that choice can produce, and its asset keys ARE the exact game states
// that decide between them. Nothing is inferred - if the fire and reload clips
// are selectable at all, they are entries in one of these.
//
// The key CLASS matters as much as the name, because it decides how a caller
// has to drive the state: a bool is a switch, a class 1 or 2 is an integer with
// an UNSET value that matches everything, and classes 4 to 7 are float windows.
// That is the difference between setting a state and setting it correctly.

#include "bf6_core.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const char* kRig1p = "animations/glacier/global/rigging/soldier_1p.rig";
static const char* kSke1p = "common/characters/_soldier/ske_soldier_1p";

static std::string tail(const std::string& p)
{
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

static const char* key_class_name(int c)
{
    switch (c) {
        case 0: return "bool";
        case 1: case 2: return "int";
        case 3: return "bitmap";
        case 4: case 6: return "float-window";
        case 5: case 7: return "float+weight";
        default: return "?";
    }
}

// Every asset the listing offers whose path contains `needle`.
static std::vector<std::string> find_assets(bf6_ctx* c, const char* needle)
{
    std::vector<std::string> out;
    const int n = bf6_list_ebx(c, needle, nullptr, 0);
    if (n <= 0) return out;
    std::vector<bf6_asset> all((size_t)n);
    const int got = bf6_list_ebx(c, needle, all.data(), n);
    for (int i = 0; i < got; ++i)
        if (all[(size_t)i].name) out.emplace_back(all[(size_t)i].name);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// One context database, in full: what it can choose and what chooses it.
static void dump_cdb(bf6_ctx* c, const std::string& path, int& ok_count, int& entry_total)
{
    char err[512] = {0};
    bf6_ant_cdb* db = bf6_ant_cdb_open(c, path.c_str(), err, (int)sizeof(err));
    if (!db) {
        std::printf("  %-64s REFUSED: %s\n", tail(path).c_str(), err);
        return;
    }
    bf6_ant_cdb_info info{};
    if (!bf6_ant_cdb_get_info(db, &info)) {
        std::printf("  %-64s opened but reports no info\n", tail(path).c_str());
        bf6_free(c, db);
        return;
    }
    ++ok_count;
    entry_total += info.entries;
    std::printf("\n%s\n", path.c_str());
    std::printf("  %d entr%s, %d slot(s), %d key(s), %d asset key(s), %d value byte(s)%s\n",
                info.entries, info.entries == 1 ? "y" : "ies", info.slots, info.keys,
                info.asset_keys, info.value_bytes,
                info.has_mirror ? ", NAMES A MIRROR STATE" : "");
    /* THE STATES THAT DECIDE. Printed before the entries because this is the
     * part a caller has to reproduce: an entry it cannot reach is not usable. */
    for (int r = 0; r < info.asset_keys; ++r) {
        int desc = -1, flags = 0;
        const char* gs = bf6_ant_cdb_asset_key(db, r, &desc, &flags);
        bf6_ant_cdb_key k{};
        const bool have = desc >= 0 && desc < info.keys && bf6_ant_cdb_key_at(db, desc, &k);
        std::printf("    key %-3d %-13s %-58s%s%s%s\n", r,
                    have ? key_class_name(k.key_class) : "?",
                    gs ? tail(gs).c_str() : "<none>",
                    (flags & BF6_ANT_CDB_ROW_BITFLAG) ? " BITFLAG" : "",
                    (flags & BF6_ANT_CDB_ROW_SKIPPED) ? " SKIPPED" : "",
                    (flags & BF6_ANT_CDB_ROW_MIRRORABLE) ? " MIRRORABLE" : "");
        if (have && (k.key_class >= 4))
            std::printf("            window %.4f .. %.4f\n", k.lo, k.hi);
    }
    for (int e = 0; e < info.entries; ++e) {
        const char* a = bf6_ant_cdb_entry(db, e);
        std::printf("    -> %s\n", a ? a : "<null>");
    }
    bf6_free(c, db);
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: %s <game dir> cdbs|dump|find|clips [arg]\n", argv[0]);
        return 2;
    }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("FAIL: %s\n", err); return 1; }
    const std::string mode = argv[2];
    const std::string arg = argc > 3 ? argv[3] : std::string();

    /* HOW MUCH OF THE INSTALL IS MOUNTED, and it is the whole reason the fire
     * and reload clips looked absent.
     *
     * bf6_open mounts Data/Win32/*.toc ONLY. The core's own note puts the cost
     * exactly: "a weapon folder that holds 250 assets holds 4 of them in the
     * Data/Win32 mount - every per-attachment record is in the Update
     * packages." The M4A1 folder answered with 7 assets and I read that as the
     * game not shipping 1P fire and reload animations. It was reading a tenth
     * of a folder.
     *
     * So the mount is an explicit argument here, and every claim about what
     * exists has to say which mount it was made under. "levels" is much slower
     * and is only needed for content owned by a level bundle. */
    const std::string mount = argc > 4 ? argv[4] : std::string("open");
    if (mount == "all" || mount == "levels") {
        const int with_levels = mount == "levels" ? 1 : 0;
        if (!bf6_mount_all(c, with_levels, err, (int)sizeof(err)))
            std::printf("NOTE: bf6_mount_all(%d) failed: %s\n", with_levels, err);
        else
            std::printf("mount: whole install%s\n",
                        with_levels ? " including every level's archives" : "");
    } else {
        std::printf("mount: Data/Win32 only (bf6_open default)\n");
    }

    if (mode == "find") {
        for (const std::string& s : find_assets(c, arg.c_str()))
            std::printf("%s\n", s.c_str());
    } else if (mode == "dump") {
        /* Depth 6: deep enough to reach an array of structs inside a struct,
         * which is where authored numbers live. */
        std::vector<char> buf(1 << 22, 0);
        const int64_t n = bf6_ebx_dump(c, arg.c_str(), 6, buf.data(), (int)buf.size());
        if (n < 0) std::printf("FAIL: %s\n", buf.data());
        else std::printf("%s\n", buf.data());
    } else if (mode == "clips") {
        /* WHICH ONES ACTUALLY READ, which is the only test that has held up.
         * A clip that binds against the 1P rig is usable; one that does not is
         * not present in this mount whatever a listing says. */
        int read = 0, refused = 0;
        for (const std::string& s : find_assets(c, arg.c_str())) {
            bf6_anim_binding_stats st{};
            const int n = bf6_anim_bindings(c, s.c_str(), kRig1p, kSke1p, nullptr, 0, &st);
            if (n >= 1 && n <= 4096) { ++read; std::printf("READ  %-5d %s\n", n, s.c_str()); }
            else ++refused;
        }
        std::printf("\n%d read, %d refused\n", read, refused);
    } else if (mode == "sounds") {
        /* WHICH SOUNDS DECODE, under a prefix.
         *
         * bf6_ui_sound_decode is misnamed. Its contract takes "either a sound
         * config partition or a NewWaveResource", so it is the general decoder
         * and the ui_ prefix is a historical accident - which is the difference
         * between "the tool can only play menu clicks" and "the tool can play
         * the weapon". With out_pcm16 NULL it still reports channels and rate,
         * so this costs a header read rather than a full decode. */
        int ok = 0, bad = 0;
        for (const std::string& s : find_assets(c, arg.c_str())) {
            int ch = 0, rate = 0;
            const int n = bf6_ui_sound_decode(c, s.c_str(), 0, nullptr, 0, &ch, &rate);
            if (n > 0) {
                ++ok;
                std::printf("OK %-9d %d ch %6d Hz %7.2f s  %s\n", n, ch, rate,
                            ch > 0 && rate > 0 ? (double)n / ch / rate : 0.0, s.c_str());
            } else {
                ++bad;
            }
        }
        std::printf("\n%d decoded, %d refused\n", ok, bad);
    } else if (mode == "cdbs") {
        /* Discovered rather than listed by hand, then each one OPENED - the
         * listing supplies candidates and the open supplies the truth. */
        std::vector<std::string> cand;
        for (const char* needle : { ".cdb", "cdbchooser", "stancepose", "weaponpose" })
            for (const std::string& s : find_assets(c, needle))
                if (s.size() > 4 && s.find(".cdb") != std::string::npos
                    && s.find("(wrapper)") == std::string::npos
                    && s.find("(controller)") == std::string::npos)
                    cand.push_back(s);
        std::sort(cand.begin(), cand.end());
        cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
        std::printf("%zu context database candidate(s)\n", cand.size());
        int ok = 0, entries = 0;
        for (const std::string& s : cand) dump_cdb(c, s, ok, entries);
        std::printf("\n%d of %zu opened, %d entries in total\n", ok, cand.size(), entries);
    } else {
        std::printf("unknown mode %s\n", mode.c_str());
    }
    bf6_close(c);
    return 0;
}
