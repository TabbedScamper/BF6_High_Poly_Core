/* bf6_game_log_* and bf6_caps_*: the log is found under its mojibake folder,
 * read whole and by delta, and a truncation is a new session; CHANGES records a
 * baseline, says nothing moved when nothing did, reports a change, calls a gap a
 * gap and a removal a removal, and only trusts a complete probe run. */
#include "bf6_core.h"
#include "json.hpp"
#include "utf8_fs.h"

#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;

static void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

// The record pointer is read after the call has filled it in.
static bf6json::Value take(int64_t n, uint8_t** outp)
{
    uint8_t* out = *outp;
    bf6json::Value v;
    if (n > 0 && out) {
        std::string err;
        bf6json::Parser p((const char*)out, (size_t)n);
        p.parse(v, err);
    }
    bf6_blob_free(out);
    return v;
}

static std::string jq(const std::string& s)
{
    std::string o = "\"";
    for (char c : s) { if (c == '\\' || c == '"') o.push_back('\\'); o.push_back(c); }
    return o + "\"";
}

static std::string scan(const std::string& root, const std::string& sources)
{
    const std::string req = "{\"root\":" + jq(root) + ",\"sources\":" + sources + "}";
    uint8_t* out = nullptr;
    const int64_t n = bf6_caps_scan(req.c_str(), req.size(), &out);
    bf6json::Value v = take(n, &out);
    return v.find("report") ? v.find("report")->str : std::string();
}

static bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string base = argc > 1 ? argv[1] : bf6fs::join(bf6fs::env("TEMP"), "bf6_log_caps_test");
    bf6fs::remove_tree(base);
    bf6fs::make_dirs(base);

    // ---- the log
    const std::string temp = bf6fs::join(base, "temp");
    const std::string folder = bf6fs::join(temp, "Battlefield\xC3\xA2\xC2\x84\xC2\xA2 6");
    bf6fs::make_dirs(folder);
    bf6fs::make_dirs(bf6fs::join(temp, "Battlefield Other"));   // no log inside: must not win
    const std::string log = bf6fs::join(folder, "PortalLog.txt");
    bf6fs::write_all(log,
        "[UTC 2026-09-04 07:01:26] Mod started\r\n"
        "[UTC 2026-09-04 07:01:31] QuickJS: console.log: [PITFALL] pads ready\r\n"
        "\r\n"
        "[UTC 2026-09-04 07:01:32] QuickJS: console.error: boom\r\n"
        "[UTC 2026-09-04 07:01:33] Script loading failed\r\n");
    uint8_t* out = nullptr;
    auto loc = take(bf6_game_log_locate(temp.c_str(), &out), &out);
    check(loc.find("path") && loc.find("path")->str == log, "the log is found under its mojibake folder, by pattern");
    out = nullptr;
    auto all = take(bf6_game_log_read(nullptr, temp.c_str(), -1, 0, &out), &out);
    const auto& e = all.find("entries")->arr;
    check(all.find("found")->b && e.size() == 4, "four non-empty lines");
    check(e.size() == 4 && e[0].find("kind")->str == "system" && e[0].find("time")->str == "2026-09-04 07:01:26"
          && e[1].find("kind")->str == "console.log" && e[1].find("text")->str == "[PITFALL] pads ready"
          && e[2].find("kind")->str == "error" && e[2].find("text")->str == "boom"
          && e[3].find("kind")->str == "error", "the mod's lines are told from the engine's");
    out = nullptr;
    auto tail = take(bf6_game_log_read(log.c_str(), nullptr, -1, 2, &out), &out);
    check(tail.find("entries")->arr.size() == 2 && tail.find("entries")->arr[1].find("text")->str == "Script loading failed", "max_entries keeps the tail");
    const int64_t end = (int64_t)all.find("offset")->num;
    {
        std::string t;
        bf6fs::read_all(log, t);
        bf6fs::write_all(log, t + "[UTC 2026-09-04 07:02:00] QuickJS: console.log: more\r\n[UTC 2026-09-04 07:02:01] QuickJS: console.log: half");
    }
    out = nullptr;
    auto delta = take(bf6_game_log_read(log.c_str(), nullptr, end, 0, &out), &out);
    check(delta.find("entries")->arr.size() == 1 && delta.find("entries")->arr[0].find("text")->str == "more" && !delta.find("reset")->b,
          "a delta brings whole new lines and holds back the one still being written");
    bf6fs::write_all(log, "[UTC 2026-09-04 08:00:00] Mod started\n");
    out = nullptr;
    auto fresh = take(bf6_game_log_read(log.c_str(), nullptr, (int64_t)delta.find("offset")->num, 0, &out), &out);
    check(fresh.find("reset")->b && fresh.find("entries")->arr.size() == 1, "a shorter file is a new session");
    out = nullptr;
    auto none = take(bf6_game_log_read(nullptr, bf6fs::join(base, "nowhere").c_str(), -1, 0, &out), &out);
    check(!none.find("found")->b && has(none.find("why")->str, "no Portal log found"), "no folder says what was looked for");

    // ---- CHANGES
    const std::string sdk = bf6fs::join(base, "sdk");
    const std::string store = bf6fs::join(base, "store");
    bf6fs::make_dirs(bf6fs::join(sdk, "code/types/mod"));
    bf6fs::make_dirs(bf6fs::join(sdk, "FbExportData"));
    bf6fs::make_dirs(bf6fs::join(sdk, "GodotProject/levels"));
    bf6fs::write_all(bf6fs::join(sdk, "sdk.version.json"), "{\"version\":\"1.4.2.0\"}");
    const std::string dts_path = bf6fs::join(sdk, "code/types/mod/index.d.ts");
    bf6fs::write_all(dts_path,
        "export function SpawnObject(id: number): void;\n"
        "export enum Teams {\n  Team1,\n  Team2\n}\n"
        "/* export function Hidden(): void; */\n");
    const std::string types = bf6fs::join(sdk, "FbExportData/asset_types.json");
    bf6fs::write_all(types, "{\"AssetTypes\":[{\"type\":\"Crate\",\"directory\":\"Props\"},{\"type\":\"Barrel\",\"directory\":\"Props\"}]}");
    bf6fs::write_all(bf6fs::join(sdk, "GodotProject/levels/MP_Test.tscn"), "x");
    bf6fs::write_all(bf6fs::join(sdk, "GodotProject/levels/MP_Other.tscn"), "x");
    const std::string sdk_src = "{\"sdk\":{\"root\":" + jq(sdk) + "}}";

    std::string r1 = scan(store, sdk_src);
    check(has(r1, "Baseline recorded: 6 capability record(s)"), "the first scan is a baseline, not a comparison");
    std::string r2 = scan(store, sdk_src);
    check(has(r2, "Nothing changed since the scan at") && has(r2, "observation 2"), "an unchanged source says so, and the scan still counts");

    bf6fs::write_all(dts_path,
        "export function SpawnObject(id: number, team: number): void;\n"
        "export enum Teams {\n  Team1,\n  Team2,\n  Team3\n}\n");
    bf6fs::write_all(types, "{\"AssetTypes\":[{\"type\":\"Crate\",\"directory\":\"Props\"}]}");
    bf6fs::remove_file(bf6fs::join(sdk, "GodotProject/levels/MP_Other.tscn"));
    std::string r3 = scan(store, sdk_src);
    check(has(r3, "## Changed (2)") && has(r3, "`SpawnObject`") && has(r3, "Team3"), "a changed signature and a new enum member are changes");
    check(has(r3, "## Gone (1)") && has(r3, "`Barrel`"), "a placeable missing from a complete scope is gone");
    check(has(r3, "## Not observed (1)") && has(r3, "`MP_Other`") && has(r3, "not a removal"), "a map missing from a partial scope is a gap, not a removal");

    // A/B/A: returning to earlier content is a change, reported against the observation before.
    bf6fs::write_all(types, "{\"AssetTypes\":[{\"type\":\"Crate\",\"directory\":\"Props\"},{\"type\":\"Barrel\",\"directory\":\"Props\"}]}");
    std::string r4 = scan(store, sdk_src);
    check(has(r4, "## New (1)") && has(r4, "`Barrel`"), "returning to earlier content is reported as a change");

    std::string nosdk = scan(bf6fs::join(base, "store2"), "{\"sdk\":{\"root\":\"\"},\"watchlist\":{}}");
    check(has(nosdk, "no Portal SDK folder is set up") || has(nosdk, "Baseline recorded: 0"), "no SDK is an answer, not an error");
    check(has(nosdk, "## Watchlist") && has(nosdk, "Baseline recorded: 10"), "the watchlist is recorded");

    out = nullptr;
    const std::string sreq = "{\"root\":" + jq(store) + "}";
    auto status = take(bf6_caps_status(sreq.c_str(), sreq.size(), &out), &out);
    check(status.find("sources") && status.find("sources")->arr[0].find("scans")->num == 4
          && status.find("sources")->arr[0].find("contents")->num == 3, "status counts scans and distinct contents apart");

    // ---- the probe
    out = nullptr;
    auto probe = take(bf6_caps_probe(&out), &out);
    const std::string hash = probe.find("hash")->str;
    check(has(probe.find("script")->str, "BF6PROBE begin") && has(probe.find("script")->str, hash.c_str()), "the probe carries its hash");
    const std::string good_run = "r1_a";
    std::string lines = "[";
    auto line = [&](const std::string& s) { if (lines.size() > 1) lines += ","; lines += jq(s); };
    // An older broken run, then a healthy one.
    line("QuickJS: console.log: BF6PROBE begin r0_z probe=" + hash);
    line("QuickJS: console.log: BF6PROBE r0_z controls 3/4");
    line("QuickJS: console.log: BF6PROBE begin " + good_run + " probe=" + hash);
    line("QuickJS: console.log: BF6PROBE " + good_run + " controls 4/4");
    const char* names[] = {"SetTickRate", "TickRates", "AutoPlayers_SetPlayerCount", "AISetAwareness", "EnableSpatialObject", "UIDumpTree",
                           "GetWaterHeight", "GetWaterIsEnabled", "GetWaterBeaufortScale", "GetWaterWaveAmplitude"};
    for (const char* n : names) line("QuickJS: console.log: BF6PROBE " + good_run + " " + n + (std::strcmp(n, "GetWaterHeight") == 0 ? " present" : " absent"));
    line("QuickJS: console.log: BF6PROBE end " + good_run + " 10");
    lines += "]";
    const std::string preq = "{\"root\":" + jq(store) + ",\"lines\":" + lines + "}";
    out = nullptr;
    auto ingest = take(bf6_caps_ingest_probe(preq.c_str(), preq.size(), &out), &out);
    check(ingest.find("updated")->num == 10 && has(ingest.find("summary")->str, "from run r1_a"), "the newest complete run is recorded");
    std::string broken = "[" + jq("BF6PROBE begin r9_q probe=" + hash) + "," + jq("BF6PROBE r9_q controls 2/4") + "," + jq("BF6PROBE end r9_q 0") + "]";
    const std::string breq = "{\"root\":" + jq(store) + ",\"lines\":" + broken + "}";
    out = nullptr;
    auto refused = take(bf6_caps_ingest_probe(breq.c_str(), breq.size(), &out), &out);
    check(refused.find("updated")->num == 0 && has(refused.find("summary")->str, "controls did not all pass"), "a run whose controls failed certifies nothing");

    bf6fs::remove_tree(base);
    std::printf("LOG CAPS %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
