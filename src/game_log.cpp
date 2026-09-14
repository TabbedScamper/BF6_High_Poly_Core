/* The Portal log: what a mod printed while it ran, for every editor's LOG
 * screen. Finding the file, reading it while the game still has it open, and
 * telling the mod's own lines from the engine's. See bf6_game_log_read in
 * bf6_core.h. */
#include "bf6_core.h"
#include "utf8_fs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char* LOG_NAME = "PortalLog.txt";

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

int64_t give(const std::string& text, uint8_t** out)
{
    *out = (uint8_t*)std::malloc(text.size() + 1);
    if (!*out) return -1;
    std::memcpy(*out, text.data(), text.size());
    (*out)[text.size()] = 0;
    return (int64_t)text.size();
}

std::string temp_root(const char* given)
{
    if (given && *given) return given;
    std::string t = bf6fs::env("TEMP");
    if (t.empty()) t = bf6fs::env("TMP");
    return t;
}

bool contains_ci(const std::string& hay, const char* needle)
{
    const size_t n = std::strlen(needle);
    if (n == 0 || hay.size() < n) return false;
    for (size_t i = 0; i + n <= hay.size(); ++i) {
        size_t k = 0;
        for (; k < n; ++k) {
            char a = hay[i + k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (k == n) return true;
    }
    return false;
}

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

bool starts(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

// One line of the file:
//   [UTC 2026-09-04 07:01:26] Mod started
//   [UTC 2026-09-04 07:01:31] QuickJS: console.log: [PITFALL] pads ready
void entry(std::string& j, const std::string& line)
{
    std::string rest = line, time, kind;
    if (starts(rest, "[UTC ")) {
        const size_t close = rest.find(']');
        if (close != std::string::npos && close > 5) {
            time = trim(rest.substr(5, close - 5));
            rest = rest.substr(close + 1);
            size_t k = 0;
            while (k < rest.size() && (rest[k] == ' ' || rest[k] == '\t')) ++k;
            rest = rest.substr(k);
        }
    }
    // QuickJS is the script engine: anything it prefixes came from the mod, not
    // the game around it.
    if (starts(rest, "QuickJS: ")) {
        rest = rest.substr(9);
        if (starts(rest, "console.log: ")) { kind = "console.log"; rest = rest.substr(13); }
        else if (starts(rest, "console.error: ")) { kind = "error"; rest = rest.substr(15); }
        else kind = "script";
    } else {
        kind = "system";
        // "Script loading failed" and friends carry no marker; they matter most.
        if (contains_ci(rest, "fail") || contains_ci(rest, "error")) kind = "error";
    }
    j += "{\"time\":"; jstr(j, time);
    j += ",\"kind\":"; jstr(j, kind);
    j += ",\"text\":"; jstr(j, rest);
    j += "}";
}

std::string locate(const std::string& root)
{
    if (root.empty()) return std::string();
    // A pattern, never a literal: the folder name on disk carries mojibake for
    // the trademark sign, and EA may spell it differently in another build.
    std::string best;
    int64_t best_time = -1;
    for (const std::string& name : bf6fs::list(root, true, "Battlefield")) {
        const std::string full = bf6fs::join(root, name);
        const std::string file = bf6fs::join(full, LOG_NAME);
        int64_t size = 0, mtime = 0;
        // Only a folder that actually holds the log counts.
        if (!bf6fs::stat_file(file, size, mtime)) continue;
        if (mtime > best_time) { best_time = mtime; best = full; }
    }
    return best;
}

std::string not_found(const std::string& root)
{
    return std::string("no Portal log found. Looked for a folder matching \"Battlefield*\" holding ") + LOG_NAME
         + " under " + root + ". The game writes it on PC only, and only after a mod has run, so play a round on localhost first.";
}

} // namespace

extern "C" int64_t bf6_game_log_locate(const char* temp_dir, uint8_t** out)
{
    if (!out) return -1;
    const std::string root = temp_root(temp_dir);
    const std::string folder = locate(root);
    std::string j = "{\"folder\":";
    jstr(j, folder);
    j += ",\"path\":";
    jstr(j, folder.empty() ? std::string() : bf6fs::join(folder, LOG_NAME));
    j += ",\"why\":";
    jstr(j, folder.empty() ? not_found(root) : std::string());
    j += "}";
    return give(j, out);
}

extern "C" int64_t bf6_game_log_read(const char* path, const char* temp_dir, int64_t offset, int32_t max_entries,
                                     uint8_t** out)
{
    if (!out) return -1;
    std::string file = path ? path : "";
    std::string why;
    if (file.empty()) {
        const std::string root = temp_root(temp_dir);
        const std::string folder = locate(root);
        if (folder.empty()) why = not_found(root);
        else file = bf6fs::join(folder, LOG_NAME);
    }
    int64_t size = 0, mtime = 0;
    bool found = false, reset = false;
    int64_t next = offset < 0 ? 0 : offset;
    std::vector<std::string> lines;
    if (why.empty()) {
        if (!bf6fs::stat_file(file, size, mtime)) {
            why = "no log file at " + file;
        } else {
            std::string text;
            int64_t start = offset < 0 ? 0 : offset;
            if (offset >= 0 && size < offset) {
                // Shorter than last time: the game truncates it when a mod starts.
                start = 0;
                reset = true;
            }
            const int64_t cap = offset < 0 ? (int64_t)1 << 40 : 4 * 1024 * 1024;
            if (!bf6fs::read_range(file, start, cap, text, size)) {
                why = "found " + file + " but could not read it. If the game is running, the file may be locked; try again after leaving the match.";
            } else {
                found = true;
                // A watch takes only whole lines; a line still being written is
                // read next time rather than split in two.
                size_t usable = text.size();
                if (offset >= 0) {
                    const size_t nl = text.find_last_of('\n');
                    usable = nl == std::string::npos ? 0 : nl + 1;
                }
                next = start + (int64_t)usable;
                size_t at = 0;
                while (at < usable) {
                    size_t e = text.find('\n', at);
                    if (e == std::string::npos || e > usable) e = usable;
                    std::string line = text.substr(at, e - at);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (!trim(line).empty()) lines.push_back(line);
                    at = e + 1;
                }
            }
        }
    }
    // The tail is what matters: the last run is at the end.
    size_t first = 0;
    if (max_entries > 0 && lines.size() > (size_t)max_entries) first = lines.size() - (size_t)max_entries;
    std::string j = "{\"found\":";
    j += found ? "true" : "false";
    j += ",\"path\":"; jstr(j, file);
    j += ",\"why\":"; jstr(j, why);
    j += ",\"bytes\":" + std::to_string(size);
    j += ",\"written\":" + std::to_string(mtime);
    j += ",\"offset\":" + std::to_string(next);
    j += reset ? ",\"reset\":true" : ",\"reset\":false";
    j += ",\"entries\":[";
    for (size_t i = first; i < lines.size(); ++i) {
        if (i > first) j += ',';
        entry(j, lines[i]);
    }
    j += "]}";
    return give(j, out);
}
