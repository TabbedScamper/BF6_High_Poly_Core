#include "cache_store.h"

#include "../json.hpp"   // bf6json: the core's minimal parser
#include "../../include/bf6_cache_identity.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>

namespace bf6::cache {

namespace {

std::string hex64(std::uint64_t v)
{
    static const char* d = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i) { s[(size_t)i] = d[v & 15]; v >>= 4; }
    return s;
}

std::string random_suffix()
{
    static std::mt19937_64 rng{ std::random_device{}() ^
        (std::uint64_t)std::chrono::steady_clock::now().time_since_epoch().count() };
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    return hex64(rng());
}

const char* kMarker = "bf6hp-cache-root";
const char kNl = '\n';

std::string quote(const std::string& s)
{
    std::string o(1, '"');
    for (unsigned char ch : s) {
        if (ch == '"' || ch == '\\') { o.push_back('\\'); o.push_back((char)ch); }
        else if (ch < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", ch); o += b; }
        else o.push_back((char)ch);
    }
    o.push_back('"');
    return o;
}

bool parse_json(const std::string& text, bf6json::Value& out)
{
    std::string err;
    out = bf6json::parse(text.data(), text.size(), err);
    return err.empty() && out.is_obj();
}

std::string str_of(const bf6json::Value& v, const char* key)
{
    const bf6json::Value* f = v.find(key);
    return f && f->is_str() ? f->str : std::string();
}

double num_of(const bf6json::Value& v, const char* key)
{
    const bf6json::Value* f = v.find(key);
    return f && f->type == bf6json::Value::Num ? f->num : -1.0;
}

// "key": value lines for a small flat JSON object.
std::string object(const std::vector<std::pair<std::string, std::string>>& members, const std::string& indent = "")
{
    std::string o = "{";
    o.push_back(kNl);
    for (std::size_t i = 0; i < members.size(); ++i) {
        o += indent + "  " + quote(members[i].first) + ": " + members[i].second;
        if (i + 1 < members.size()) o.push_back(',');
        o.push_back(kNl);
    }
    return o + indent + "}";
}

}  // namespace

std::string cache_key(const std::string& install_identity, std::uint32_t recipe, std::uint32_t format)
{
    bf6_cache::detail::Hasher h;
    h.str("bf6hp-cache-key");
    h.str(install_identity);
    h.u64(recipe);
    h.u64(format);
    return hex64(h.done().a);
}

bool read_file(const fs::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool has_link_below(const fs::path& base, const fs::path& path)
{
    std::error_code ec;
    fs::path cur = base;
    if (fs::is_symlink(fs::symlink_status(cur, ec))) return true;
    const fs::path rel = path.lexically_relative(base);
    for (const auto& part : rel) {
        if (part == "." || part.empty()) continue;
        if (part == "..") return true;
        cur /= part;
        const auto st = fs::symlink_status(cur, ec);
        if (ec || !fs::exists(st)) break;
        if (fs::is_symlink(st)) return true;
#ifdef _WIN32
        // Junctions and other reparse points can report as neither regular files
        // nor directories; treat them as links.
        if (!fs::is_directory(st) && !fs::is_regular_file(st)) return true;
#endif
    }
    return false;
}

bool atomic_write(const fs::path& target, const std::string& bytes, std::string& err)
{
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) { err = "cannot create " + target.parent_path().u8string() + ": " + ec.message(); return false; }
    fs::path tmp = target;
    tmp += ".tmp-" + random_suffix();
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { err = "cannot write " + tmp.u8string(); return false; }
        out.write(bytes.data(), (std::streamsize)bytes.size());
        out.flush();
        if (!out) { err = "write failed " + tmp.u8string(); out.close(); fs::remove(tmp, ec); return false; }
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        err = "cannot publish " + target.u8string() + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool Store::valid_level_name(const std::string& level)
{
    if (level.empty() || level.size() > 63) return false;
    for (char ch : level) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        if (!ok) return false;
    }
    return true;
}

fs::path Store::map_dir(const std::string& level) const
{
    return root_ / "maps" / level;
}

bool Store::open(const fs::path& cache_root, const std::string& install_identity, std::string& err)
{
    if (install_identity.empty()) { err = "empty install identity"; return false; }
    std::error_code ec;
    if (cache_root.empty() || !cache_root.is_absolute()) { err = "cache root must be an absolute path"; return false; }
    fs::create_directories(cache_root, ec);
    if (ec) { err = "cannot create cache root: " + ec.message(); return false; }
    base_ = fs::weakly_canonical(cache_root, ec) / "bf6hp-cache";
    if (ec) { err = "cannot resolve cache root: " + ec.message(); return false; }
    identity_ = install_identity;
    key_ = cache_key(install_identity);
    root_ = base_ / ("v" + std::to_string(kFormat)) / key_;
    if (has_link_below(base_.parent_path(), root_)) { err = "cache path goes through a link"; return false; }
    fs::create_directories(root_ / "shared", ec);
    if (!ec) fs::create_directories(root_ / "maps", ec);
    if (ec) { err = "cannot create cache folders: " + ec.message(); return false; }

    const fs::path marker = root_ / "root.json";
    std::string text;
    if (read_file(marker, text)) {
        bf6json::Value j;
        if (!parse_json(text, j) || str_of(j, "marker") != kMarker || str_of(j, "key") != key_ ||
            str_of(j, "identity") != identity_ || num_of(j, "format") != kFormat ||
            num_of(j, "recipe") != kRecipe) {
            err = "root.json does not match this cache key";
            return false;
        }
        return true;
    }
    const long long created = (long long)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string j = object({ {"marker", quote(kMarker)}, {"key", quote(key_)}, {"identity", quote(identity_)},
                                   {"format", std::to_string(kFormat)}, {"recipe", std::to_string(kRecipe)},
                                   {"created_unix", std::to_string(created)} }) + kNl;
    return atomic_write(marker, j, err);
}

bool Store::map_complete(const std::string& level, const std::map<std::string, std::uint32_t>& required) const
{
    if (!valid_level_name(level) || root_.empty()) return false;
    std::string text;
    if (!read_file(map_dir(level) / "complete.json", text)) return false;
    bf6json::Value j;
    if (!parse_json(text, j) || str_of(j, "key") != key_ || str_of(j, "level") != level) return false;
    const bf6json::Value* layers = j.find("layers");
    if (!layers || !layers->is_obj()) return false;
    for (const auto& [name, version] : required) {
        const bf6json::Value* layer = layers->find(name.c_str());
        if (!layer || !layer->is_obj() || num_of(*layer, "version") < (double)version) return false;
    }
    return true;
}

bool Store::write_map_complete(const std::string& level, const std::map<std::string, LayerRecord>& layers,
                               std::string& err)
{
    if (!valid_level_name(level)) { err = "invalid level name"; return false; }
    std::vector<std::pair<std::string, std::string>> rows;
    for (const auto& [name, rec] : layers)
        rows.push_back({ name, object({ {"version", std::to_string(rec.version)},
                                        {"bytes", std::to_string(rec.bytes)},
                                        {"digest", quote(rec.digest)} }, "    ") });
    const std::string j = object({ {"key", quote(key_)}, {"level", quote(level)}, {"layers", object(rows, "  ")} }) + kNl;
    return atomic_write(map_dir(level) / "complete.json", j, err);
}

bool Store::write_install_complete(const std::vector<std::string>& levels, std::string& err)
{
    std::string list = "[";
    for (std::size_t i = 0; i < levels.size(); ++i) list += (i ? ", " : "") + quote(levels[i]);
    list += "]";
    return atomic_write(root_ / "install-complete.json",
                        object({ {"key", quote(key_)}, {"levels", list} }) + kNl, err);
}

bool Store::install_complete(const std::vector<std::string>& levels) const
{
    std::string text;
    if (root_.empty() || !read_file(root_ / "install-complete.json", text)) return false;
    bf6json::Value j;
    if (!parse_json(text, j) || str_of(j, "key") != key_) return false;
    const bf6json::Value* done = j.find("levels");
    if (!done || !done->is_arr()) return false;
    for (const auto& level : levels) {
        bool found = false;
        for (const auto& v : done->arr) if (v.is_str() && v.str == level) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

// "bf6-cache-source-v<n>:<root fingerprint>:<content>" -> the root fingerprint.
static std::string install_root_of(const std::string& identity)
{
    const std::size_t a = identity.find(':');
    if (a == std::string::npos) return identity;
    const std::size_t b = identity.find(':', a + 1);
    return identity.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
}

int Store::sweep_stale(std::string& err) const
{
    if (root_.empty()) { err = "store is not open"; return -1; }
    std::error_code ec;
    int removed = 0;
    if (!fs::is_directory(base_, ec)) return 0;
    for (const auto& version : fs::directory_iterator(base_, ec)) {
        if (fs::is_symlink(version.symlink_status(ec)) || !version.is_directory(ec)) continue;
        for (const auto& candidate : fs::directory_iterator(version.path(), ec)) {
            const fs::path p = candidate.path();
            if (p == root_) continue;
            if (fs::is_symlink(candidate.symlink_status(ec)) || !candidate.is_directory(ec)) continue;
            if (has_link_below(base_, p)) continue;
            std::string text;
            if (!read_file(p / "root.json", text)) continue;          // not ours: keep
            bf6json::Value j;
            if (!parse_json(text, j) || str_of(j, "marker") != kMarker) continue;
            // Only an older cache of THIS installation folder is stale. Another
            // install (a second storefront copy) keeps its own cache.
            if (install_root_of(str_of(j, "identity")) != install_root_of(identity_)) continue;
            std::error_code rm;
            fs::remove_all(p, rm);
            if (rm) { err = "could not remove " + p.u8string() + ": " + rm.message(); continue; }
            ++removed;
        }
    }
    return removed;
}

}  // namespace bf6::cache
