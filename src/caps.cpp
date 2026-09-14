/* WHAT THIS BUILD CAN DO, AND WHAT CHANGED SINCE THE LAST ONE - the CHANGES
 * screen of both SDK editors.
 *
 * Three sources decide what a Portal mod can use and they disagree: the
 * installed SDK, the Portal website and the running game. This collects what
 * each says, keeps content-addressed snapshots with an append-only observation
 * log per source, and reports the differences between two observations.
 *
 * Two rules carry the whole design. ABSENCE IS NOT REMOVAL: a key missing from a
 * scope that was not fully collected both times is "not observed", never
 * "removed". EVIDENCE IS A LADDER, NOT A BOOLEAN: an identifier seen in an asset,
 * a parsed type, a control on a page, a name present in a running game and an
 * effect measured in one are different claims and are never merged.
 *
 * See bf6_caps_scan in bf6_core.h. */
#include "bf6_core.h"
#include "json.hpp"
#include "utf8_fs.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

using bf6json::Value;

// Bumped from the Unreal tool's caps/2: the content id is computed differently
// here, and the rule is that a changed instrument re-baselines instead of
// reporting every record as changed.
const char* COLLECTOR = "caps/3";
const char* PROBE_FORMAT = "probe/2";

enum Evidence { Observed, Structure, Exposed, Serialized, SiteAccepted, RuntimePresent, RuntimeVerified, NotObserved };
enum Coverage { Complete, Partial, Skipped, Unavailable, Failed, Cancelled };

const char* EVIDENCE[] = {"Observed", "Structure", "Exposed", "Serialized", "SiteAccepted", "RuntimePresent", "RuntimeVerified", "NotObserved"};
const char* COVERAGE[] = {"Complete", "Partial", "Skipped", "Unavailable", "Failed", "Cancelled"};
const char* SOURCES[] = {"Sdk", "Portal", "Game", "Watchlist"};

int evidence_from(const std::string& s) { for (int i = 0; i < 8; ++i) if (s == EVIDENCE[i]) return i; return 0; }
int coverage_from(const std::string& s) { for (int i = 0; i < 6; ++i) if (s == COVERAGE[i]) return i; return 4; }

std::string fmt(const char* f, ...)
{
    std::vector<char> b(4096);
    va_list ap;
    va_start(ap, f);
    int n = std::vsnprintf(b.data(), b.size(), f, ap);
    va_end(ap);
    if (n >= (int)b.size()) {
        b.resize((size_t)n + 1);
        va_start(ap, f);
        std::vsnprintf(b.data(), b.size(), f, ap);
        va_end(ap);
    }
    return b.data();
}

void jstr(std::string& out, const std::string& s)
{
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
            else out.push_back((char)c);
        }
    }
    out.push_back('"');
}

std::string q(const std::string& s) { std::string o; jstr(o, s); return o; }

int64_t give(const std::string& text, uint8_t** out)
{
    *out = (uint8_t*)std::malloc(text.size() + 1);
    if (!*out) return -1;
    std::memcpy(*out, text.data(), text.size());
    (*out)[text.size()] = 0;
    return (int64_t)text.size();
}

std::string str(const Value* v) { return v && v->is_str() ? v->str : std::string(); }
double num(const Value* v, double d) { return v && v->type == Value::Num ? v->num : d; }

// FNV-1a, 64 bits, as hex: a content id, not a security boundary.
std::string hash_hex(const std::string& s, int chars)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    char b[20];
    std::snprintf(b, sizeof(b), "%016llx", (unsigned long long)h);
    return std::string(b).substr(0, (size_t)chars);
}

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool starts(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
bool ends(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }

std::string when(int64_t t) { return t > 0 ? bf6fs::utc_iso(t) : std::string("an unrecorded time"); }

int64_t parse_iso(const std::string& s)
{
    int y, mo, d, h, mi, se;
#if defined(_MSC_VER)
    if (sscanf_s(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
#else
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
#endif
    // days from civil
    y -= mo <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + se;
}

// ---------------------------------------------------------------- model

struct Record {
    std::string key, kind, scope, display, shape, detail;
    int evidence = Observed;
};

struct Scope {
    std::string name;
    int state = Complete;
    std::string why;
    int items = 0, cap = 0;
};

struct Snapshot {
    std::string id;
    int source = 0;
    std::string build, collector, context;
    int64_t captured = 0, observed = 0;
    int seq = 0;
    std::map<std::string, Record> records;   // "kind|key"
    std::vector<Scope> scopes;

    const Scope* scope(const std::string& n) const
    {
        for (const Scope& s : scopes) if (s.name == n) return &s;
        return nullptr;
    }
    bool complete(const std::string& n) const { const Scope* s = scope(n); return s && s->state == Complete; }
    void add(Record r) { const std::string k = r.kind + "|" + r.key; records[k] = std::move(r); }
};

enum ChangeKind { Added, Removed, Changed, NotSeen, Raised, Lowered };
const char* CHANGE[] = {"Added", "Removed", "Changed", "NotObserved", "EvidenceRaised", "EvidenceLowered"};

struct Change {
    int what = Added;
    Record before, after;
    std::string why;
};

std::string content_id(const Snapshot& s)
{
    std::string canon = std::string(COLLECTOR) + "\n" + SOURCES[s.source] + "\n" + s.build + "\n";
    for (const auto& kv : s.records)
        canon += kv.first + "\x1f" + kv.second.shape + "\x1f" + EVIDENCE[kv.second.evidence] + "\n";
    // Scopes are identity too: the same records under weaker coverage are a
    // different observation.
    std::vector<Scope> sc = s.scopes;
    std::sort(sc.begin(), sc.end(), [](const Scope& a, const Scope& b) { return a.name < b.name; });
    for (const Scope& p : sc) canon += "scope\x1f" + p.name + "\x1f" + COVERAGE[p.state] + "\n";
    return hash_hex(canon, 16);
}

std::string scopes_json(const std::vector<Scope>& scopes)
{
    std::string j = "[";
    for (size_t i = 0; i < scopes.size(); ++i) {
        const Scope& s = scopes[i];
        if (i) j += ',';
        j += "{\"name\":" + q(s.name) + ",\"state\":" + q(COVERAGE[s.state]) + ",\"why\":" + q(s.why)
           + ",\"items\":" + std::to_string(s.items) + ",\"cap\":" + std::to_string(s.cap) + "}";
    }
    return j + "]";
}

std::vector<Scope> scopes_from(const Value* arr)
{
    std::vector<Scope> out;
    if (arr && arr->is_arr())
        for (const Value& v : arr->arr) {
            if (!v.is_obj()) continue;
            Scope s;
            s.name = str(v.find("name"));
            s.state = coverage_from(str(v.find("state")));
            s.why = str(v.find("why"));
            s.items = (int)num(v.find("items"), 0);
            s.cap = (int)num(v.find("cap"), 0);
            out.push_back(s);
        }
    return out;
}

std::string snapshot_json(const Snapshot& s)
{
    std::string j = "{\"id\":" + q(s.id) + ",\"source\":" + q(SOURCES[s.source]) + ",\"build\":" + q(s.build)
                  + ",\"collector\":" + q(s.collector) + ",\"capturedUtc\":" + q(bf6fs::utc_iso(s.captured))
                  + ",\"context\":" + q(s.context) + ",\"scopes\":" + scopes_json(s.scopes) + ",\"records\":[";
    bool first = true;
    for (const auto& kv : s.records) {
        const Record& r = kv.second;
        if (!first) j += ',';
        first = false;
        j += "{\"key\":" + q(r.key) + ",\"kind\":" + q(r.kind) + ",\"scope\":" + q(r.scope) + ",\"display\":" + q(r.display)
           + ",\"shape\":" + q(r.shape) + ",\"detail\":" + q(r.detail) + ",\"evidence\":" + q(EVIDENCE[r.evidence]) + "}";
    }
    return j + "]}";
}

bool snapshot_from(const std::string& text, Snapshot& s)
{
    Value v;
    std::string err;
    bf6json::Parser p(text.c_str(), text.size());
    if (!p.parse(v, err) || !v.is_obj()) return false;
    s.id = str(v.find("id"));
    const std::string src = str(v.find("source"));
    for (int i = 0; i < 4; ++i) if (src == SOURCES[i]) s.source = i;
    s.build = str(v.find("build"));
    s.collector = str(v.find("collector"));
    s.context = str(v.find("context"));
    s.captured = parse_iso(str(v.find("capturedUtc")));
    s.scopes = scopes_from(v.find("scopes"));
    if (const Value* rs = v.find("records"); rs && rs->is_arr())
        for (const Value& r : rs->arr) {
            if (!r.is_obj()) continue;
            Record rec;
            rec.key = str(r.find("key"));
            rec.kind = str(r.find("kind"));
            rec.scope = str(r.find("scope"));
            rec.display = str(r.find("display"));
            rec.shape = str(r.find("shape"));
            rec.detail = str(r.find("detail"));
            rec.evidence = evidence_from(str(r.find("evidence")));
            if (!rec.key.empty()) s.add(rec);
        }
    return true;
}

// ---------------------------------------------------------------- store
//
// Content blobs are addressed by what is in them, so an unchanged source scanned
// twice costs one blob; they are written beside their destination and renamed,
// so a blob that exists is always finished. Observations are an append-only,
// numbered log per source: "what is current" and "what changed" read the log,
// never the blob directory.

struct Store {
    std::string root;

    std::string blob_dir(int source, const std::string& id) const
    { return bf6fs::join(bf6fs::join(bf6fs::join(root, "snapshots"), SOURCES[source]), id); }
    std::string obs_dir(int source) const
    { return bf6fs::join(bf6fs::join(root, "observations"), SOURCES[source]); }
    std::string obs_file(int source, int seq) const
    { return bf6fs::join(obs_dir(source), fmt("obs_%08d.json", seq)); }

    bool publish(Snapshot& s, std::string& err) const
    {
        s.collector = COLLECTOR;
        if (s.captured == 0) s.captured = bf6fs::now_unix();
        s.id = content_id(s);
        const std::string dir = blob_dir(s.source, s.id);
        if (bf6fs::is_dir(dir)) return true;   // deduplication, not "nothing happened"
        const std::string staging = dir + ".writing";
        bf6fs::remove_tree(staging);
        if (!bf6fs::make_dirs(staging)) { err = "could not create " + staging; return false; }
        if (!bf6fs::write_all(bf6fs::join(staging, "snapshot.json"), snapshot_json(s))) {
            bf6fs::remove_tree(staging);
            err = "could not write the snapshot";
            return false;
        }
        if (!bf6fs::move_replace(staging, dir)) {
            bf6fs::remove_tree(staging);
            err = "could not publish the snapshot";
            return false;
        }
        return true;
    }

    bool load(int source, const std::string& id, Snapshot& out) const
    {
        std::string text;
        if (!bf6fs::read_all(bf6fs::join(blob_dir(source, id), "snapshot.json"), text)) return false;
        return snapshot_from(text, out);
    }

    std::vector<std::string> blobs(int source) const
    {
        std::vector<std::string> ids;
        for (const std::string& d : bf6fs::list(bf6fs::join(bf6fs::join(root, "snapshots"), SOURCES[source]), true))
            if (!ends(d, ".writing")) ids.push_back(d);
        std::vector<std::pair<int64_t, std::string>> when;
        for (const std::string& d : ids) {
            Snapshot s;
            when.push_back({load(source, d, s) ? s.captured : 0, d});
        }
        std::sort(when.begin(), when.end(), [](auto& a, auto& b) { return a.first > b.first; });
        ids.clear();
        for (auto& w : when) ids.push_back(w.second);
        return ids;
    }

    std::vector<int> seqs(int source) const
    {
        std::vector<int> out;
        for (const std::string& f : bf6fs::list(obs_dir(source), false, "obs_", ".json")) {
            const std::string digits = f.substr(4, f.size() - 9);
            if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) continue;
            const int n = std::atoi(digits.c_str());
            if (n > 0) out.push_back(n);
        }
        std::sort(out.rbegin(), out.rend());
        return out;
    }

    bool load_observation(int source, int seq, Snapshot& out) const
    {
        std::string text;
        if (!bf6fs::read_all(obs_file(source, seq), text)) return false;
        Value v;
        std::string err;
        bf6json::Parser p(text.c_str(), text.size());
        if (!p.parse(v, err) || !v.is_obj()) return false;
        const std::string id = str(v.find("contentId"));
        if (id.empty() || !load(source, id, out)) return false;
        // The blob's time is when this CONTENT was first stored, not when this
        // observation happened.
        out.seq = seq;
        out.observed = parse_iso(str(v.find("observedUtc")));
        if (out.observed == 0) out.observed = out.captured;
        if (const Value* c = v.find("context"); c && c->is_str()) out.context = c->str;
        return true;
    }

    bool back(int source, int n, Snapshot& out) const
    {
        const std::vector<int> s = seqs(source);
        return n >= 0 && n < (int)s.size() && load_observation(source, s[(size_t)n], out);
    }

    bool write_observation(const Snapshot& s, int seq, std::string& err) const
    {
        const std::string j = "{\"seq\":" + std::to_string(seq) + ",\"source\":" + q(SOURCES[s.source]) + ",\"contentId\":" + q(s.id)
            + ",\"build\":" + q(s.build) + ",\"collector\":" + q(s.collector) + ",\"observedUtc\":" + q(bf6fs::utc_iso(s.observed))
            + ",\"context\":" + q(s.context) + ",\"records\":" + std::to_string(s.records.size())
            + ",\"scopes\":" + scopes_json(s.scopes) + "}";
        const std::string final_path = obs_file(s.source, seq);
        const std::string staging = final_path + ".writing";
        bf6fs::make_dirs(obs_dir(s.source));
        if (!bf6fs::write_all(staging, j)) { err = "could not write the observation"; return false; }
        if (!bf6fs::move_replace(staging, final_path)) { bf6fs::remove_file(staging); err = "could not append the observation"; return false; }
        return true;
    }

    // A store with blobs and no log is seeded from them once, in capture order.
    void seed(int source) const
    {
        if (!seqs(source).empty()) return;
        std::vector<std::string> ids = blobs(source);
        std::reverse(ids.begin(), ids.end());
        int seq = 0;
        for (const std::string& id : ids) {
            Snapshot s;
            if (!load(source, id, s)) continue;
            s.id = id;
            s.observed = s.captured;
            std::string err;
            if (write_observation(s, seq + 1, err)) ++seq;
        }
    }

    bool record(Snapshot s, Snapshot& stored, std::string& err) const
    {
        if (s.observed == 0) s.observed = bf6fs::now_unix();
        // Seed from what was already stored BEFORE adding this scan's blob, or the
        // very first scan of a store is logged twice.
        seed(s.source);
        if (!publish(s, err)) return false;
        const std::vector<int> ss = seqs(s.source);
        int next = (ss.empty() ? 0 : ss[0]) + 1;
        while (bf6fs::is_file(obs_file(s.source, next))) ++next;
        if (!write_observation(s, next, err)) return false;
        s.seq = next;
        stored = s;
        return true;
    }
};

// ---------------------------------------------------------------- compare

std::vector<Change> compare(const Snapshot& old_s, const Snapshot& new_s)
{
    std::vector<Change> out;
    if (old_s.source != new_s.source) return out;   // separate namespaces
    if (!old_s.collector.empty() && !new_s.collector.empty() && old_s.collector != new_s.collector) return out;
    for (const auto& kv : new_s.records) {
        auto was = old_s.records.find(kv.first);
        if (was == old_s.records.end()) {
            Change c; c.what = Added; c.after = kv.second;
            if (!old_s.complete(kv.second.scope))
                c.why = "the previous scan did not complete " + kv.second.scope + ", so this may not be new";
            out.push_back(c);
            continue;
        }
        if (was->second.shape != kv.second.shape) {
            Change c; c.what = Changed; c.before = was->second; c.after = kv.second;
            out.push_back(c);
        } else if (was->second.evidence != kv.second.evidence) {
            // NotObserved is the largest value and not the top of the ladder, so it
            // is handled by name before any ordinal comparison.
            Change c; c.before = was->second; c.after = kv.second;
            if (kv.second.evidence == NotObserved) {
                c.what = Lowered;
                c.why = std::string("it was ") + EVIDENCE[was->second.evidence]
                      + " before and was looked for and not seen this time. The record is kept so the change is visible.";
            } else if (was->second.evidence == NotObserved) {
                c.what = Raised;
                c.why = "it was recorded as not observed and has been seen again.";
            } else {
                c.what = kv.second.evidence > was->second.evidence ? Raised : Lowered;
            }
            out.push_back(c);
        }
    }
    for (const auto& kv : old_s.records) {
        if (new_s.records.count(kv.first)) continue;
        Change c; c.before = kv.second;
        if (old_s.complete(kv.second.scope) && new_s.complete(kv.second.scope)) {
            c.what = Removed;
            c.why = kv.second.scope + " was collected fully both times";
        } else {
            c.what = NotSeen;
            const Scope* now = new_s.scope(kv.second.scope);
            c.why = now ? kv.second.scope + " was " + COVERAGE[now->state] + " this time (" + now->why + "), so this is a gap in the scan and not a removal"
                        : kv.second.scope + " was not visited this time, so this is a gap in the scan and not a removal";
        }
        out.push_back(c);
    }
    std::stable_sort(out.begin(), out.end(), [](const Change& a, const Change& b) {
        if (a.what != b.what) return a.what < b.what;
        const std::string& ka = a.after.key.empty() ? a.before.key : a.after.key;
        const std::string& kb = b.after.key.empty() ? b.before.key : b.after.key;
        return ka < kb;
    });
    return out;
}

std::string report(const Snapshot& old_s, const Snapshot& new_s, const std::vector<Change>& changes)
{
    std::string md = fmt("# What changed in the %s\n\n", SOURCES[new_s.source]);
    md += new_s.build + fmt(" (observation %d, ", new_s.seq) + when(new_s.observed) + ") compared with "
        + old_s.build + fmt(" (observation %d, ", old_s.seq) + when(old_s.observed) + ").\n\n";
    // Coverage first: what was looked at, before anything found.
    md += "## What was looked at\n\n";
    for (const Scope& s : new_s.scopes) {
        md += "- " + s.name + ": " + COVERAGE[s.state];
        if (s.items) md += fmt(", %d item(s)", s.items);
        if (s.cap) md += fmt(", stopped at a limit of %d", s.cap);
        if (!s.why.empty()) md += " (" + s.why + ")";
        md += "\n";
    }
    md += "\nComplete means complete for what this scan set out to cover. It never means the whole of Portal is known.\n\n";
    if (changes.empty()) return md + "## Nothing changed\n\nNo differences in any scope that was collected.\n";
    auto section = [&](int kind, const char* title, const char* blurb) {
        std::vector<const Change*> mine;
        for (const Change& c : changes) if (c.what == kind) mine.push_back(&c);
        if (mine.empty()) return;
        md += fmt("## %s (%d)\n\n%s\n\n", title, (int)mine.size(), blurb);
        for (const Change* c : mine) {
            const Record& r = c->after.key.empty() ? c->before : c->after;
            md += "- **" + r.kind + "** `" + r.key + "`";
            if (!r.display.empty() && r.display != r.key) md += " - " + r.display;
            if (c->what == Changed) md += "\n    - was: `" + c->before.shape + "`\n    - now: `" + c->after.shape + "`";
            else if (!r.shape.empty() && c->what == Added) md += "\n    - `" + r.shape + "`";
            if (c->what == Raised || c->what == Lowered)
                md += std::string("\n    - ") + EVIDENCE[c->after.evidence] + ", was " + EVIDENCE[c->before.evidence];
            else
                md += std::string("\n    - evidence: ") + EVIDENCE[r.evidence];
            if (!c->why.empty()) md += "\n    - " + c->why;
            md += "\n";
        }
        md += "\n";
    };
    section(Added, "New", "Present now and not in the previous snapshot.");
    section(Changed, "Changed", "The same capability with a different shape. A widened range or a new option belongs here, and so does a changed signature.");
    section(Raised, "Better evidence", "Unchanged, but now known more firmly than it was.");
    section(Lowered, "Weaker evidence", "Still recorded, but the evidence behind it dropped. A capability that was found at runtime and is not found now appears here, not under Gone: the record still exists, what fell away is the proof.");
    section(Removed, "Gone", "Absent from a scope that was collected completely both times. This is the only section that claims something was taken away.");
    section(NotSeen, "Not observed", "Seen before, not seen now, in a scope this scan did not finish. Almost always a gap in the scan rather than a removal. Re-run the scan before reading anything into these.");
    return md;
}

// ---------------------------------------------------------------- the SDK collector

// Whitespace-normalized, so a reflow is not reported as a capability change.
std::string normalize(const std::string& in)
{
    std::string out;
    bool space = false;
    for (unsigned char c : in) {
        if (std::isspace(c)) { space = true; continue; }
        if (space && !out.empty()) out.push_back(' ');
        space = false;
        out.push_back((char)c);
    }
    return trim(out);
}

// Long signatures keep a readable head and a hash of the whole thing.
std::string bounded(const std::string& full)
{
    if (full.size() <= 240) return full;
    return full.substr(0, 200) + fmt(" ... %d chars, ", (int)full.size()) + hash_hex(full, 12);
}

std::string strip_comments(const std::string& line, bool& in_block)
{
    std::string out;
    char quote = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        const char n = i + 1 < line.size() ? line[i + 1] : '\0';
        if (in_block) { if (c == '*' && n == '/') { in_block = false; ++i; } continue; }
        if (quote) {
            out.push_back(c);
            if (c == '\\' && n) { out.push_back(n); ++i; continue; }
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') { quote = c; out.push_back(c); continue; }
        if (c == '/' && n == '/') break;
        if (c == '/' && n == '*') { in_block = true; ++i; continue; }
        out.push_back(c);
    }
    return out;
}

int bracket_delta(const std::string& text)
{
    int depth = 0;
    char quote = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quote) { if (c == '\\') { ++i; continue; } if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'' || c == '`') { quote = c; continue; }
        if (c == '{' || c == '(' || c == '[') ++depth;
        else if (c == '}' || c == ')' || c == ']') --depth;
    }
    return depth;
}

std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= text.size()) {
        size_t e = text.find('\n', at);
        if (e == std::string::npos) e = text.size();
        std::string l = text.substr(at, e - at);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(l);
        at = e + 1;
    }
    return out;
}

// Top-level `export` declarations of a .d.ts, each as far as its brackets close,
// every overload under one key. Not a TypeScript parser: re-export lists and
// defaults are counted as unreadable so the scope never claims completeness.
void collect_declarations(const std::string& path, Scope& scope, Snapshot& snap)
{
    std::string text;
    if (!bf6fs::read_all(path, text)) { scope.state = Failed; scope.why = "the typings file could not be read"; return; }
    const std::vector<std::string> lines = split_lines(text);
    struct Decl { std::string kind, name; std::vector<std::string> sigs; };
    std::vector<std::string> order;
    std::map<std::string, Decl> decls;
    int unreadable = 0;
    bool in_block = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string l = strip_comments(lines[i], in_block);
        size_t k = 0;
        while (k < l.size() && std::isspace((unsigned char)l[k])) ++k;
        l = l.substr(k);
        if (!starts(l, "export ")) continue;
        std::string head = l.substr(7);
        if (starts(head, "declare ")) head = head.substr(8);
        std::string kind;
        for (const char* kw : {"function ", "enum ", "const ", "class ", "interface ", "type ", "namespace "}) {
            if (starts(head, kw)) { kind = trim(kw); head = head.substr(std::strlen(kw)); break; }
        }
        if (kind.empty()) { ++unreadable; continue; }
        std::string name;
        for (char c : head) { if (std::isalnum((unsigned char)c) || c == '_') name.push_back(c); else break; }
        if (name.empty()) { ++unreadable; continue; }
        std::string full = l;
        int depth = bracket_delta(l);
        int consumed = 1;
        auto closed = [&]() { const std::string t = trim(full); return ends(t, ";") || ends(t, "}"); };
        while ((depth > 0 || !closed()) && i + 1 < lines.size() && consumed < 4000) {
            ++i;
            const std::string more = strip_comments(lines[i], in_block);
            full += " " + more;
            depth += bracket_delta(more);
            ++consumed;
        }
        if (depth > 0) ++unreadable;
        const std::string sig = normalize(full);
        if (sig.empty()) continue;
        const std::string ident = kind + "|" + name;
        if (!decls.count(ident)) { order.push_back(ident); decls[ident] = Decl{kind, name, {}}; }
        decls[ident].sigs.push_back(sig);
    }
    for (const std::string& ident : order) {
        const Decl& d = decls[ident];
        Record r;
        r.key = d.name;
        r.kind = d.kind;
        r.scope = scope.name;
        r.display = d.name;
        std::string joined;
        for (size_t s = 0; s < d.sigs.size(); ++s) { if (s) joined += " | "; joined += d.sigs[s]; }
        r.shape = bounded(joined);
        if (d.sigs.size() > 1) r.detail = fmt("%d overloads", (int)d.sigs.size());
        r.evidence = Structure;
        snap.add(r);
        ++scope.items;
    }
    const bool complete = unreadable == 0 && scope.items > 0;
    if (scope.items == 0) scope.why = "the typings file held no exported declarations";
    else if (unreadable > 0) scope.why = fmt("%d export(s) are re-export lists, defaults or unterminated declarations that this collector does not resolve, so absences here are gaps and not removals", unreadable);
    else scope.why = "every top-level exported declaration in the typings, with its body and all its overloads";
    scope.state = complete ? Complete : (scope.items > 0 ? Partial : Failed);
}

void collect_names(const std::string& dir, const std::string& prefix, const std::string& suffix, const char* kind,
                   Scope& scope, Snapshot& snap)
{
    for (const std::string& f : bf6fs::list(dir, false, prefix, suffix)) {
        Record r;
        r.key = f.substr(0, f.size() - suffix.size());
        r.kind = kind;
        r.scope = scope.name;
        r.display = r.key;
        r.shape = r.key;   // the name only; the report says so
        r.evidence = Observed;
        snap.add(r);
        ++scope.items;
    }
}

std::string sdk_version(const std::string& root)
{
    std::string text;
    if (!bf6fs::read_all(bf6fs::join(root, "sdk.version.json"), text)) return std::string();
    Value v;
    std::string err;
    bf6json::Parser p(text.c_str(), text.size());
    if (!p.parse(v, err) || !v.is_obj()) return std::string();
    return str(v.find("version"));
}

void collect_sdk(const std::string& root, Snapshot& out)
{
    out.source = 0;
    out.captured = bf6fs::now_unix();
    if (root.empty() || !bf6fs::is_dir(root)) {
        Scope s; s.name = "sdk"; s.state = Unavailable; s.why = "no Portal SDK folder is set up in this project";
        out.scopes.push_back(s);
        out.context = "no SDK";
        return;
    }
    out.build = sdk_version(root);
    out.context = root;
    {
        Scope s; s.name = "api";
        const std::string dts = bf6fs::join(root, "code/types/mod/index.d.ts");
        if (bf6fs::is_file(dts)) collect_declarations(dts, s, out);
        else { s.state = Unavailable; s.why = dts + " is not there"; }
        out.scopes.push_back(s);
    }
    {
        Scope s; s.name = "placeables";
        const std::string path = bf6fs::join(root, "FbExportData/asset_types.json");
        std::string text;
        if (bf6fs::read_all(path, text)) {
            Value v;
            std::string err;
            bf6json::Parser p(text.c_str(), text.size());
            const Value* rows = p.parse(v, err) && v.is_obj() ? v.find("AssetTypes") : nullptr;
            if (rows && rows->is_arr()) {
                for (const Value& o : rows->arr) {
                    const std::string t = str(o.find("type"));
                    if (t.empty()) continue;
                    Record r;
                    r.key = t; r.kind = "placeable"; r.scope = s.name; r.display = t;
                    r.shape = str(o.find("directory"));   // its category, which is what moves
                    r.evidence = Structure;
                    out.add(r);
                    ++s.items;
                }
                s.state = Complete;
            } else { s.state = Failed; s.why = "asset_types.json could not be read as JSON"; }
        } else { s.state = Unavailable; s.why = path + " is not there"; }
        out.scopes.push_back(s);
    }
    {
        Scope s; s.name = "maps";
        const std::string dir = bf6fs::join(root, "GodotProject/levels");
        if (bf6fs::is_dir(dir)) {
            collect_names(dir, "MP_", ".tscn", "map", s, out);
            s.state = Partial;
            s.why = "names only: a map whose contents changed under the same name is not detected";
        } else { s.state = Unavailable; s.why = "no levels folder"; }
        out.scopes.push_back(s);
    }
    {
        Scope s; s.name = "models";
        const std::string dir = bf6fs::join(root, "GodotProject/raw/models");
        if (bf6fs::is_dir(dir)) {
            collect_names(dir, "", ".glb", "model", s, out);
            s.state = Partial;
            s.why = "names only: a model whose contents changed under the same name is not detected";
        } else { s.state = Unavailable; s.why = "no models folder"; }
        out.scopes.push_back(s);
    }
}

// ---------------------------------------------------------------- the Portal collector
//
// What the site has already told us, from captures on disk. It never browses.

std::string num_text(double v)
{
    char b[40];
    std::snprintf(b, sizeof(b), "%.17g", v);
    std::string s = b;
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos) s += ".0";
    return s;
}

std::string json_canon(const Value& v)
{
    switch (v.type) {
    case Value::Null: return "null";
    case Value::Bool: return v.b ? "true" : "false";
    case Value::Num: return num_text(v.num);
    case Value::Str: return q(v.str);
    case Value::Arr: {
        std::string s = "[";
        for (size_t i = 0; i < v.arr.size(); ++i) { if (i) s += ','; s += json_canon(v.arr[i]); }
        return s + "]";
    }
    case Value::Obj: {
        std::string s = "{";
        for (size_t i = 0; i < v.obj.size(); ++i) { if (i) s += ','; s += q(v.obj[i].first) + ":" + json_canon(v.obj[i].second); }
        return s + "}";
    }
    }
    return "null";
}

void collect_portal(const std::string& blocks_file, const std::string& settings_catalog, Snapshot& out)
{
    out.source = 1;
    out.captured = bf6fs::now_unix();
    out.context = "from the captures already on disk";
    {
        Scope s; s.name = "blocks";
        std::string text;
        if (!blocks_file.empty() && bf6fs::read_all(blocks_file, text) && !text.empty()) {
            Value v;
            std::string err;
            bf6json::Parser p(text.c_str(), text.size());
            if (p.parse(v, err) && v.is_obj()) {
                for (const auto& kv : v.obj) {
                    Record r;
                    r.key = kv.first; r.kind = "block"; r.scope = s.name; r.display = kv.first;
                    // The definition is the shape: a new socket or dropdown is a change.
                    const std::string one = kv.second.is_obj() ? json_canon(kv.second) : std::string();
                    r.shape = hash_hex(one, 12);
                    r.detail = fmt("%d bytes of definition", (int)one.size());
                    r.evidence = Exposed;
                    out.add(r);
                    ++s.items;
                }
                s.state = Complete;
                s.why = "the block set the last Portal capture brought back";
            } else { s.state = Failed; s.why = "the captured definitions are not readable JSON"; }
        } else {
            s.state = Unavailable;
            s.why = blocks_file.empty() ? "this editor keeps no Portal block capture" : "no Portal capture yet: sign in to Portal once and open Blocks";
        }
        out.scopes.push_back(s);
    }
    {
        Scope s; s.name = "settings";
        std::string text;
        if (!settings_catalog.empty() && bf6fs::read_all(settings_catalog, text)) {
            Value v;
            std::string err;
            bf6json::Parser p(text.c_str(), text.size());
            if (p.parse(v, err) && v.is_obj()) {
                out.build = str(v.find("appVersion"));
                const std::string mined = str(v.find("minedAt"));
                const Value* pages = v.find("pages");
                if (pages && pages->is_arr()) {
                    for (const Value& page : pages->arr) {
                        if (!page.is_obj()) continue;
                        const std::string page_name = str(page.find("page"));
                        const Value* settings = page.find("settings");
                        if (!settings || !settings->is_arr()) continue;
                        for (const Value& o : settings->arr) {
                            const std::string id = str(o.find("testId"));
                            if (id.empty()) continue;
                            Record r;
                            r.key = id; r.kind = "setting"; r.scope = s.name;
                            r.display = str(o.find("title"));
                            if (r.display.empty()) r.display = id;
                            // The range and the options themselves: a label is what a
                            // person reads and a value is what a mod sends.
                            auto opt = [&](const char* k) { const Value* x = o.find(k); return x && x->type == Value::Num ? num_text(x->num) : std::string("-"); };
                            const Value* per = o.find("perTeam");
                            std::string opts;
                            if (const Value* list = o.find("options"); list && list->is_arr()) {
                                opts = " options=[";
                                for (size_t i = 0; i < list->arr.size(); ++i) {
                                    const Value& ov = list->arr[i];
                                    if (i) opts += ',';
                                    if (!ov.is_obj()) { opts += ov.is_str() ? ov.str : std::string(); continue; }
                                    const Value* val = ov.find("value");
                                    opts += (val && val->type == Value::Num ? num_text(val->num) : std::string("-")) + "=" + str(ov.find("label"));
                                }
                                opts += "]";
                            }
                            r.shape = bounded(str(o.find("kind")) + " min=" + opt("min") + " max=" + opt("max")
                                + " step=" + fmt("%g", num(o.find("step"), 0)) + " default=" + opt("default")
                                + " perTeam=" + (per && per->type == Value::Bool && per->b ? "yes" : "no") + opts);
                            r.detail = page_name;
                            r.evidence = Exposed;
                            out.add(r);
                            ++s.items;
                        }
                    }
                    s.state = Partial;
                    s.why = "the catalogue mined on " + (mined.empty() ? std::string("an unrecorded date") : mined) + ", not a fresh read of the live site";
                } else { s.state = Failed; s.why = "the catalogue has no pages"; }
            } else { s.state = Failed; s.why = "the catalogue is not readable JSON"; }
        } else { s.state = Unavailable; s.why = "no shipped settings catalogue"; }
        out.scopes.push_back(s);
    }
}

// ---------------------------------------------------------------- game and watchlist

void collect_game(const std::string& install, bool reader, Snapshot& out)
{
    out.source = 2;
    out.captured = bf6fs::now_unix();
    Scope s; s.name = "install";
    if (install.empty()) { s.state = Unavailable; s.why = "no Battlefield install is being read"; }
    else {
        Record r;
        r.key = "install"; r.kind = "game"; r.scope = s.name; r.display = "Battlefield install";
        r.shape = install;
        r.detail = reader ? "the High Poly reader is present" : "no High Poly reader";
        r.evidence = Observed;
        out.add(r);
        out.context = install;
        s.items = 1;
        s.state = Partial;
        s.why = "identity only: map, placement and water contents are not decoded by this scan";
    }
    out.scopes.push_back(s);
}

struct Candidate { const char* name; const char* kind; const char* note; int known; };

// Recorded observations, with where they came from.
const Candidate WATCH[] = {
    {"SetTickRate", "native", "Real in the engine and rejected by name at upload. Takes a TickRates enum member, not a number: SetTickRate(1) is refused as a Number where TickRates was wanted. Rate_60Hz applies only on a local host, where the sim runs at client framerate; a measured online host reported exactly 30 Hz.", RuntimeVerified},
    {"TickRates", "enum", "Observed present at runtime alongside SetTickRate. Carries Rate_60Hz. Rejected by name at upload.", RuntimePresent},
    {"AutoPlayers_SetPlayerCount", "native", "Part of the bot API, all of which was found present at runtime while undeclared in the typings. Nothing has been called.", RuntimePresent},
    {"AISetAwareness", "native", "Probed at runtime and absent.", NotObserved},
    {"EnableSpatialObject", "native", "Probed at runtime and absent.", NotObserved},
    {"UIDumpTree", "native", "Probed at runtime and absent.", NotObserved},
    {"GetWaterHeight", "native", "In the archived ModBuilder registries, derived signature returns Number. An archived expression graph exists and needs host state. Not probed at runtime under this name, and absent from the shipped typings.", Observed},
    {"GetWaterIsEnabled", "native", "In the archived ModBuilder registries, derived signature returns Boolean. No expression graph recorded. Not probed at runtime under this name, and absent from the shipped typings.", Observed},
    {"GetWaterBeaufortScale", "native", "In the archived ModBuilder registries, derived signature returns Number. An archived expression graph exists and needs host state. Not probed at runtime under this name, and absent from the shipped typings.", Observed},
    {"GetWaterWaveAmplitude", "native", "In the archived ModBuilder registries, derived signature returns Number. An archived expression graph exists and needs host state. Not probed at runtime under this name, and absent from the shipped typings.", Observed},
};
const char* CONTROLS[] = {"SpawnObject", "GetObjectPosition", "DealDamage", "CreateVector"};

void collect_watchlist(Snapshot& out)
{
    out.source = 3;
    out.captured = bf6fs::now_unix();
    out.build = "recorded";
    out.context = "named candidates, with the evidence behind each";
    Scope s; s.name = "candidates";
    for (const Candidate& c : WATCH) {
        Record r;
        r.key = c.name; r.kind = c.kind; r.scope = s.name; r.display = c.name; r.shape = c.kind;
        r.detail = c.note; r.evidence = c.known;
        out.add(r);
        ++s.items;
    }
    s.state = Complete;
    s.why = "the list as recorded; running the probe in a game updates it";
    out.scopes.push_back(s);
}

std::string probe_hash()
{
    std::string canon = std::string(PROBE_FORMAT) + "\n";
    for (const char* c : CONTROLS) canon += std::string("control ") + c + "\n";
    for (const Candidate& c : WATCH) canon += std::string("candidate ") + c.name + "\n";
    return hash_hex(canon, 12);
}

std::string probe_script()
{
    const std::string hash = probe_hash();
    std::string controls;
    for (size_t i = 0; i < sizeof(CONTROLS) / sizeof(CONTROLS[0]); ++i) controls += std::string(i ? ", " : "") + "\"" + CONTROLS[i] + "\"";
    std::string s;
    s += "// Paste into your mod, host it locally, and read the LOG section.\n";
    s += "// This only ASKS whether a name exists. It calls none of them, so a\n";
    s += "// \"present\" answer means the name is there and nothing more.\n";
    s += "{\n";
    s += "  const M = mod as unknown as Record<string, unknown>;\n";
    s += "  const has = (n: string) => typeof M[n] === \"function\" || (M[n] !== undefined && typeof M[n] === \"object\");\n";
    s += "  // One id for this run. Every line below carries it so results from\n";
    s += "  // two runs in one log can never be read as one run.\n";
    s += "  const run = \"r\" + Date.now().toString(36) + \"_\" + Math.floor(Math.random() * 1679616).toString(36);\n";
    s += "  const say = (s: string) => console.log(\"BF6PROBE \" + run + \" \" + s);\n";
    s += "  console.log(\"BF6PROBE begin \" + run + \" probe=" + hash + "\");\n";
    s += "  const controls = [" + controls + "];\n";
    s += "  const ok = controls.filter(has).length;\n";
    s += "  say(\"controls \" + ok + \"/\" + controls.length);\n";
    s += "  const names = [\n";
    for (const Candidate& c : WATCH) s += std::string("    \"") + c.name + "\",\n";
    s += "  ];\n";
    s += "  for (const n of names) say(n + \" \" + (has(n) ? \"present\" : \"absent\"));\n";
    s += "  console.log(\"BF6PROBE end \" + run + \" \" + names.length);\n";
    s += "}\n";
    return s;
}

// ---------------------------------------------------------------- scan

std::string scan_one(const Store& store, Snapshot fresh)
{
    const char* name = SOURCES[fresh.source];
    Snapshot before;
    const bool had = store.back(fresh.source, 0, before);
    Snapshot stored;
    std::string err;
    if (!store.record(fresh, stored, err)) return fmt("## %s\n\ncollected but not recorded: %s\n\n", name, err.c_str());
    if (!had) {
        std::string md = fmt("## %s\n\nBaseline recorded: %d capability record(s). There is nothing earlier to compare against, so nothing here is new or missing yet.\n\n", name, (int)stored.records.size());
        for (const Scope& s : stored.scopes)
            md += "- " + s.name + ": " + COVERAGE[s.state] + (s.why.empty() ? std::string() : " (" + s.why + ")") + "\n";
        return md + "\n";
    }
    if (!before.collector.empty() && before.collector != stored.collector)
        return fmt("## %s\n\nRe-baselined. The previous scan was taken by collector %s and this one by %s, so their shapes are not comparable and no changes are being claimed. This scan is observation %d, and the next scan will compare against it.\n\n",
                   name, before.collector.c_str(), stored.collector.c_str(), stored.seq);
    if (before.id == stored.id)
        return fmt("## %s\n\nNothing changed since the scan at %s. This scan was recorded as observation %d.\n\n", name, when(before.observed).c_str(), stored.seq);
    return report(before, stored, compare(before, stored)) + "\n";
}

bool parse_request(const char* json, size_t len, Value& v)
{
    if (!json) return false;
    std::string err;
    bf6json::Parser p(json, len ? len : std::strlen(json));
    return p.parse(v, err) && v.is_obj();
}

} // namespace

extern "C" int64_t bf6_caps_scan(const char* request_json, size_t len, uint8_t** out)
{
    if (!out) return -1;
    *out = nullptr;
    Value req;
    if (!parse_request(request_json, len, req)) return -1;
    Store store{str(req.find("root"))};
    if (store.root.empty()) return -1;
    const Value* sources = req.find("sources");
    std::string md;
    if (sources && sources->is_obj()) {
        if (const Value* s = sources->find("sdk")) { Snapshot snap; collect_sdk(str(s->find("root")), snap); md += scan_one(store, snap); }
        if (const Value* s = sources->find("portal")) { Snapshot snap; collect_portal(str(s->find("blocks_file")), str(s->find("settings_catalog")), snap); md += scan_one(store, snap); }
        if (const Value* s = sources->find("game")) {
            const Value* rd = s->find("reader");
            Snapshot snap; collect_game(str(s->find("install")), rd && rd->type == Value::Bool && rd->b, snap); md += scan_one(store, snap);
        }
        if (sources->find("watchlist")) { Snapshot snap; collect_watchlist(snap); md += scan_one(store, snap); }
    }
    if (md.empty()) md = "No sources were selected.\n";
    // Beside the snapshots, so it can be read again without re-running the scan.
    const std::string dir = bf6fs::join(store.root, "reports");
    bf6fs::make_dirs(dir);
    std::string stamp = bf6fs::utc_iso(bf6fs::now_unix());
    stamp.erase(std::remove(stamp.begin(), stamp.end(), '-'), stamp.end());
    stamp.erase(std::remove(stamp.begin(), stamp.end(), ':'), stamp.end());
    std::replace(stamp.begin(), stamp.end(), 'T', '_');
    if (!stamp.empty() && stamp.back() == 'Z') stamp.pop_back();
    const std::string file = bf6fs::join(dir, "scan_" + stamp + ".md");
    bf6fs::write_all(file, md);
    return give("{\"report\":" + q(md) + ",\"file\":" + q(file) + "}", out);
}

extern "C" int64_t bf6_caps_status(const char* request_json, size_t len, uint8_t** out)
{
    if (!out) return -1;
    *out = nullptr;
    Value req;
    if (!parse_request(request_json, len, req)) return -1;
    Store store{str(req.find("root"))};
    if (store.root.empty()) return -1;
    std::string j = "{\"root\":" + q(store.root) + ",\"sources\":[";
    for (int s = 0; s < 4; ++s) {
        if (s) j += ',';
        const std::vector<int> seqs = store.seqs(s);
        const size_t contents = store.blobs(s).size();
        j += "{\"source\":" + q(SOURCES[s]) + ",\"scans\":" + std::to_string(seqs.size()) + ",\"contents\":" + std::to_string(contents);
        Snapshot snap;
        if (!seqs.empty() && store.back(s, 0, snap))
            j += ",\"newest\":{\"seq\":" + std::to_string(snap.seq) + ",\"observed\":" + q(when(snap.observed)) + ",\"id\":" + q(snap.id)
               + ",\"build\":" + q(snap.build) + ",\"records\":" + std::to_string(snap.records.size()) + ",\"scopes\":" + scopes_json(snap.scopes) + "}";
        j += "}";
    }
    return give(j + "]}", out);
}

extern "C" int64_t bf6_caps_probe(uint8_t** out)
{
    if (!out) return -1;
    return give("{\"script\":" + q(probe_script()) + ",\"hash\":" + q(probe_hash()) + "}", out);
}

extern "C" int64_t bf6_caps_ingest_probe(const char* request_json, size_t len, uint8_t** out)
{
    if (!out) return -1;
    *out = nullptr;
    Value req;
    if (!parse_request(request_json, len, req)) return -1;
    Store store{str(req.find("root"))};
    std::vector<std::string> lines;
    if (const Value* l = req.find("lines"); l && l->is_arr())
        for (const Value& v : l->arr) if (v.is_str()) lines.push_back(v.str);

    // Grouped by run id FIRST: results and the controls that certify them must
    // come from the same run or they certify nothing.
    struct Run { std::string id, hash; bool began = false, ended = false; int promised = -1, controls = -1, controls_of = -1; std::map<std::string, bool> results; };
    std::vector<Run> runs;
    std::map<std::string, size_t> by_id;
    int untagged = 0;
    auto run_for = [&](const std::string& id) -> Run& {
        auto it = by_id.find(id);
        if (it != by_id.end()) return runs[it->second];
        runs.push_back(Run{});
        runs.back().id = id;
        by_id[id] = runs.size() - 1;
        return runs.back();
    };
    auto split1 = [](const std::string& s, std::string& a, std::string& b) {
        const size_t sp = s.find(' ');
        if (sp == std::string::npos) { a = s; b.clear(); return false; }
        a = s.substr(0, sp); b = s.substr(sp + 1); return true;
    };
    for (const std::string& line : lines) {
        const size_t at = line.find("BF6PROBE ");
        if (at == std::string::npos) continue;
        std::string rest = trim(line.substr(at + 9));
        if (starts(rest, "begin ")) {
            std::string body = trim(rest.substr(6)), id, tail;
            split1(body, id, tail);
            if (id.empty()) { ++untagged; continue; }
            Run& r = run_for(id);
            r.began = true;
            const size_t p = tail.find("probe=");
            if (p != std::string::npos) {
                r.hash = tail.substr(p + 6);
                const size_t sp = r.hash.find(' ');
                if (sp != std::string::npos) r.hash = r.hash.substr(0, sp);
                r.hash = trim(r.hash);
            }
            continue;
        }
        if (starts(rest, "end ")) {
            std::string body = trim(rest.substr(4)), id, tail;
            split1(body, id, tail);
            if (id.empty()) { ++untagged; continue; }
            Run& r = run_for(id);
            r.ended = true;
            r.promised = std::atoi(trim(tail).c_str());
            continue;
        }
        std::string id, what;
        if (!split1(rest, id, what)) { ++untagged; continue; }
        what = trim(what);
        if (id.empty() || what.empty()) { ++untagged; continue; }
        // Ids the probe generates are "r<base36>_<base36>"; anything else is an
        // older probe's line and cannot be tied to one run.
        const bool looks = starts(id, "r") && id.find('_') != std::string::npos;
        if (!looks && !by_id.count(id)) { ++untagged; continue; }
        Run& r = run_for(id);
        if (starts(what, "controls ")) {
            const std::string n = what.substr(9);
            const size_t slash = n.find('/');
            if (slash != std::string::npos) { r.controls = std::atoi(n.substr(0, slash).c_str()); r.controls_of = std::atoi(n.substr(slash + 1).c_str()); }
            continue;
        }
        std::string nm, state;
        if (split1(what, nm, state)) r.results[nm] = trim(state) == "present";
    }

    const std::string want = probe_hash();
    const int control_count = (int)(sizeof(CONTROLS) / sizeof(CONTROLS[0]));
    const Run* use = nullptr;
    std::string refused;
    for (size_t i = runs.size(); i-- > 0;) {
        const Run& r = runs[i];
        const char* why = nullptr;
        if (!r.began) why = "its start marker is not in the log, so it may be cut off at the top";
        else if (!r.ended) why = "it has no end marker, so it did not finish or the log is cut off";
        else if (r.hash != want) why = "it was built from a different candidate list than this build has";
        else if (r.controls_of != control_count) why = "it reported a different number of controls than this build expects";
        else if (r.controls != r.controls_of) why = "its controls did not all pass, so the probe itself was not working";
        else if ((int)r.results.size() != r.promised) why = "it produced fewer answers than it promised, so its output is partial";
        if (!why) { use = &r; break; }
        if (refused.empty()) refused = "the newest run (" + r.id + ") was not used because " + why + ".";
    }
    std::string summary;
    int updated = 0;
    if (!use) {
        if (runs.empty())
            summary = untagged > 0
                ? fmt("found %d probe line(s) with no run id. They are from an older probe and cannot be tied to a single run, so nothing was recorded. Copy the probe again and re-run it.", untagged)
                : std::string("no probe output found in the log yet. Host a match locally with the probe in your mod.");
        else
            summary = fmt("found %d probe run(s) and used none of them. ", (int)runs.size()) + refused + " Nothing was recorded.";
    } else {
        Snapshot snap;
        collect_watchlist(snap);
        snap.context = "probe run " + use->id + ", probe " + use->hash + fmt(", %d of %d controls passed", use->controls, use->controls_of);
        for (auto& kv : snap.records) {
            auto found = use->results.find(kv.second.key);
            if (found == use->results.end()) continue;
            // Present means the name exists; nothing was called, so never RuntimeVerified.
            kv.second.evidence = found->second ? RuntimePresent : NotObserved;
            kv.second.detail = found->second
                ? "the name exists in a running game (run " + use->id + fmt(", all %d controls passed). Nothing was called, so this is not proof it works.", use->controls_of)
                : "the name does not exist in a running game (run " + use->id + fmt(", all %d controls passed).", use->controls_of);
            ++updated;
        }
        Snapshot stored;
        std::string err;
        if (updated && (store.root.empty() || !store.record(snap, stored, err)))
            summary = fmt("read %d result(s) but could not record them: %s", updated, store.root.empty() ? "no store folder was given" : err.c_str());
        else
            summary = fmt("recorded %d result(s) from run %s, whose %d controls all passed.%s%s", updated, use->id.c_str(), use->controls_of,
                          runs.size() > 1 ? " Other runs in the log were left alone." : "",
                          untagged > 0 ? " Lines with no run id were ignored." : "");
    }
    return give("{\"updated\":" + std::to_string(updated) + ",\"summary\":" + q(summary) + "}", out);
}
