/* THE CONTEXTDATABASE PORT AGAINST THE ENGINE'S OWN ANSWERS.
 *
 *   ant_cdb_replay_test <game> <replay.tsv>
 *
 * The fixture is written by the research repo's
 *   impl/retools/cdb_differential.py <cdbs.json> 40 --emit replay.tsv
 * which runs the game's own encoder and matcher under Unicorn and records,
 * for every context, what THE ENGINE produced:
 *
 *   database <TAB> values <TAB> context (byte per slot) <TAB> scores <TAB> pick
 *
 * 45,400 contexts over all 1,135 databases the install ships, seed fixed.
 * This replays every one through bf6_ant_cdb_match and requires the encoded
 * context, every score byte and the pick to equal the engine's - not the
 * research model's. The model already agrees with the engine; this checks
 * the C++ port did not drift from either.
 *
 * The fixture is derived from the installed game, so it is not committed:
 * regenerate it against the install being tested.
 */
#include "bf6_core.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> unhex(const std::string& s)
{
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back((uint8_t)std::strtoul(s.substr(i, 2).c_str(), nullptr, 16));
    return out;
}

std::string hex(const uint8_t* p, size_t n)
{
    static const char* H = "0123456789abcdef";
    std::string o;
    for (size_t i = 0; i < n; ++i) { o += H[p[i] >> 4]; o += H[p[i] & 15]; }
    return o;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: ant_cdb_replay_test <game> <replay.tsv>\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    if (!bf6_mount_all(c, 1, err, (int)sizeof(err))) std::printf("note: mount_all said %s\n", err);

    FILE* f = std::fopen(argv[2], "rb");
    if (!f) { std::printf("cannot read %s\n", argv[2]); return 1; }

    std::map<std::string, bf6_ant_cdb*> open;
    long lines = 0, ctx_bad = 0, score_bad = 0, pick_bad = 0, refused = 0;
    int shown = 0;
    std::vector<char> buf(1 << 16);
    while (std::fgets(buf.data(), (int)buf.size(), f)) {
        std::string line = buf.data();
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty()) continue;
        std::vector<std::string> col;
        size_t a = 0;
        for (;;) {
            const size_t t = line.find('\t', a);
            col.push_back(line.substr(a, t == std::string::npos ? std::string::npos : t - a));
            if (t == std::string::npos) break;
            a = t + 1;
        }
        if (col.size() != 5) { std::printf("malformed line %ld\n", lines + 1); return 1; }
        ++lines;

        bf6_ant_cdb*& db = open[col[0]];
        if (!db) {
            char why[512] = {0};
            db = bf6_ant_cdb_open(c, col[0].c_str(), why, (int)sizeof(why));
            if (!db) {
                ++refused;
                if (shown < 10) { ++shown; std::printf("REFUSED %s: %s\n", col[0].c_str(), why); }
                continue;
            }
        }
        bf6_ant_cdb_info info{};
        bf6_ant_cdb_get_info(db, &info);
        const std::vector<uint8_t> values = unhex(col[1]);
        const std::vector<uint8_t> want_ctx = unhex(col[2]);
        const std::vector<uint8_t> want_sc = unhex(col[3]);
        const int want_pick = std::atoi(col[4].c_str());

        std::vector<uint8_t> ctx((size_t)info.slots), sc((size_t)info.entries);
        const int pick = bf6_ant_cdb_match(db, values.data(), (int)values.size(),
                                           ctx.data(), (int)ctx.size(), sc.data(), (int)sc.size());
        const bool cb = ctx != want_ctx, sb = sc != want_sc, pb = pick != want_pick;
        ctx_bad += cb; score_bad += sb; pick_bad += pb;
        if ((cb || sb || pb) && shown < 12) {
            ++shown;
            std::printf("MISMATCH %s values %s\n", col[0].c_str(), col[1].c_str());
            if (cb) std::printf("   ctx    engine %s  port %s\n", col[2].c_str(), hex(ctx.data(), ctx.size()).c_str());
            if (sb) std::printf("   scores engine %s  port %s\n", col[3].c_str(), hex(sc.data(), sc.size()).c_str());
            if (pb) std::printf("   pick   engine %d  port %d\n", want_pick, pick);
        }
    }
    std::fclose(f);
    for (auto& kv : open) if (kv.second) bf6_free(c, kv.second);

    std::printf("\n%ld context(s) over %zu database(s), %ld refused at open\n",
                lines, open.size(), refused);
    std::printf("  context mismatches %ld   score mismatches %ld   pick mismatches %ld\n",
                ctx_bad, score_bad, pick_bad);
    const bool ok = lines > 0 && refused == 0 && ctx_bad == 0 && score_bad == 0 && pick_bad == 0;
    std::printf("%s\n", ok ? "PASS" : "FAILED");
    bf6_close(c);
    return ok ? 0 : 1;
}
