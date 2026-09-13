#pragma once
// Source identity for locally generated, disposable caches (C++17, header-only).
//
// inspect() describes an installed game from its raw files, so a cache built
// from one snapshot can be recognised as stale after the game updates, or when
// the user points the tool at a different installation. The installed game is
// always the source of truth; nothing here reads or trusts a persisted cache.
//
// What is recorded, under the canonical root:
//   Data/**, Patch/**, Update/**   every *.toc: path, size, mtime, content fingerprint
//                                  every *.cas: path, size, mtime (never hashed)
//   bf6.exe, SP/bf6.exe            path, size, mtime (never hashed)
//
// Limitations, deliberately accepted:
//   - A .cas or executable rewritten in place with identical size and mtime is
//     NOT detected. Only .toc bytes are read.
//   - Fingerprints are two independent non-cryptographic 64-bit hashes. They
//     detect accidental change; they say nothing about authenticity and offer
//     no protection against deliberate collisions.
//   - Any symlink, junction or other non-regular entry at or inside a named
//     root rejects the snapshot instead of being followed or skipped.
//   - mtime is normalised to nanoseconds since the Unix epoch, but the stored
//     precision depends on the standard library's filesystem implementation.
//     A precision mismatch can only cause a spurious rebuild, never a false match.
//   - Other file types (e.g. .txt, .dll) are ignored.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace bf6_cache {
namespace fs = std::filesystem;

// Bump when anything feeding the identity changes meaning.
inline constexpr std::uint32_t kSchema = 1;

enum class Kind : std::uint8_t { toc = 1, cas = 2, exe = 3 };

struct Fingerprint {
    std::uint64_t a = 0, b = 0;
    bool operator==(const Fingerprint& o) const { return a == o.a && b == o.b; }
    bool operator!=(const Fingerprint& o) const { return !(*this == o); }
};

struct Entry {
    std::string path;          // root-relative, '/'-separated, UTF-8, on-disk case
    Kind kind = Kind::toc;
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0; // since 1970-01-01 UTC
    Fingerprint content;       // .toc bytes; zero for .cas and executables
};

inline bool operator==(const Entry& x, const Entry& y)
{
    return x.path == y.path && x.kind == y.kind && x.size == y.size &&
           x.mtime_ns == y.mtime_ns && x.content == y.content;
}
inline bool operator!=(const Entry& x, const Entry& y) { return !(x == y); }

// Only ok snapshots carry root, identity, content and entries. A failed or
// cancelled inspection returns them all empty.
struct Snapshot {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    std::string root;          // canonical, generic separators
    std::string identity;      // "bf6-cache-source-v<schema>:<root fp>:<content fp>"
    Fingerprint content;       // root-independent fingerprint of all entries
    std::vector<Entry> entries; // sorted bytewise by path
    std::size_t entry_count() const { return entries.size(); }
};

// Polled between directory entries and between read chunks; true cancels.
using Cancel = std::function<bool()>;

namespace detail {

struct Hasher {
    std::uint64_t a = 0xcbf29ce484222325ull; // FNV-1a
    std::uint64_t b = 0x6a09e667f3bcc909ull; // xor-multiply-rotate
    std::uint64_t n = 0;

    void bytes(const unsigned char* p, std::size_t len)
    {
        for (std::size_t i = 0; i < len; ++i)
        {
            a = (a ^ p[i]) * 0x100000001b3ull;
            b = (b ^ p[i]) * 0x9fb21c651e98df25ull;
            b = (b << 23) | (b >> 41);
        }
        n += len;
    }
    void u64(std::uint64_t v)
    {
        unsigned char t[8];
        for (int i = 0; i < 8; ++i) t[i] = (unsigned char)(v >> (8 * i));
        bytes(t, 8);
    }
    void str(const std::string& s)
    {
        u64(s.size());
        bytes((const unsigned char*)s.data(), s.size());
    }
    static std::uint64_t mix(std::uint64_t z)
    {
        z ^= z >> 30; z *= 0xbf58476d1ce4e5b9ull;
        z ^= z >> 27; z *= 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    Fingerprint done() const { return { mix(a ^ n), mix(b + n * 0x9e3779b97f4a7c15ull) }; }
};

inline std::string u8(const fs::path& p)
{
#if defined(__cpp_char8_t)
    const auto s = p.generic_u8string();
    return std::string(s.begin(), s.end());
#else
    return p.generic_u8string();
#endif
}

inline std::string hex(std::uint64_t v)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)v);
    return buf;
}

// file_time_type's epoch is library-defined before C++20.
inline std::int64_t unix_ns(fs::file_time_type t)
{
#if defined(_MSVC_STL_VERSION)
    // 100 ns ticks since 1601-01-01; converting before rebasing would overflow.
    return (std::int64_t(t.time_since_epoch().count()) - 116444736000000000LL) * 100;
#elif defined(__GLIBCXX__)
    // libstdc++'s file clock is the system clock shifted back 6437664000 s.
    return std::int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count()) +
           6437664000LL * 1000000000LL;
#elif defined(_LIBCPP_VERSION)
    return std::int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count());
#else
#error "bf6_cache_identity.h: unknown file_time_type epoch for this standard library"
#endif
}

inline std::string lower_ext(const fs::path& p)
{
    std::string e = u8(p.extension());
    for (char& c : e) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return e;
}

struct Scan {
    const Cancel* cancel = nullptr;
    std::vector<Entry> out;
    std::string error;
    bool cancelled = false;

    bool stop()
    {
        if (!cancelled && *cancel && (*cancel)()) cancelled = true;
        return cancelled;
    }
    bool fail(const std::string& m)
    {
        if (error.empty()) error = m;
        return false;
    }
};

// A missing path is reported as not_found without failing.
inline bool type_of(Scan& s, const fs::path& p, const std::string& rel, fs::file_type& t)
{
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(p, ec);
    t = st.type();
    if (t == fs::file_type::not_found) return true;
    if (ec) return s.fail("cannot stat " + rel + ": " + ec.message());
    return true;
}

inline bool stat_file(Scan& s, const fs::path& p, const std::string& rel,
                      std::uint64_t& size, std::int64_t& mtime)
{
    std::error_code ec;
    size = fs::file_size(p, ec);
    if (ec) return s.fail("cannot size " + rel + ": " + ec.message());
    const fs::file_time_type t = fs::last_write_time(p, ec);
    if (ec) return s.fail("cannot time " + rel + ": " + ec.message());
    mtime = unix_ns(t);
    return true;
}

inline bool hash_file(Scan& s, const fs::path& p, Entry& e)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return s.fail("cannot open " + e.path);
    Hasher h;
    std::vector<char> buf(1 << 16);
    std::uint64_t total = 0;
    for (;;)
    {
        if (s.stop()) return false;
        f.read(buf.data(), (std::streamsize)buf.size());
        const std::streamsize got = f.gcount();
        if (got > 0)
        {
            h.bytes((const unsigned char*)buf.data(), (std::size_t)got);
            total += (std::uint64_t)got;
        }
        if (!f)
        {
            if (f.eof() && !f.bad()) break;
            return s.fail("read error in " + e.path);
        }
    }
    e.content = h.done();

    // A file written while it was read has no trustworthy fingerprint.
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    if (!stat_file(s, p, e.path, size, mtime)) return false;
    if (total != e.size || size != e.size || mtime != e.mtime_ns)
        return s.fail("changed during inspection: " + e.path);
    return true;
}

inline bool add_file(Scan& s, const fs::path& p, const std::string& rel, Kind kind)
{
    Entry e;
    e.path = rel;
    e.kind = kind;
    if (!stat_file(s, p, rel, e.size, e.mtime_ns)) return false;
    if (kind == Kind::toc && !hash_file(s, p, e)) return false;
    s.out.push_back(std::move(e));
    return true;
}

inline bool walk(Scan& s, const fs::path& top, const std::string& top_rel)
{
    std::vector<std::pair<fs::path, std::string>> todo;
    todo.emplace_back(top, top_rel);
    while (!todo.empty())
    {
        const std::pair<fs::path, std::string> dir = std::move(todo.back());
        todo.pop_back();

        std::error_code iec;
        fs::directory_iterator it(dir.first, iec);
        if (iec) return s.fail("cannot enumerate " + dir.second + ": " + iec.message());
        for (const fs::directory_iterator end{}; it != end; it.increment(iec))
        {
            if (s.stop()) return false;
            const fs::path p = it->path();
            const std::string rel = dir.second + "/" + u8(p.filename());
            fs::file_type t;
            if (!type_of(s, p, rel, t)) return false;
            if (t == fs::file_type::directory)
                todo.emplace_back(p, rel);
            else if (t == fs::file_type::regular)
            {
                const std::string ext = lower_ext(p);
                if (ext == ".toc" && !add_file(s, p, rel, Kind::toc)) return false;
                if (ext == ".cas" && !add_file(s, p, rel, Kind::cas)) return false;
            }
            else if (t == fs::file_type::not_found)
                return s.fail("vanished during inspection: " + rel);
            else
                return s.fail("refusing link or special file: " + rel);
        }
        if (iec) return s.fail("enumeration of " + dir.second + " did not complete: " + iec.message());
    }
    return true;
}

inline Snapshot rejected(const Scan& s)
{
    Snapshot r;
    r.cancelled = s.cancelled;
    r.error = s.cancelled ? "cancelled" : s.error;
    return r;
}

} // namespace detail

inline Snapshot inspect(const fs::path& game_root, const Cancel& cancel = Cancel())
{
    detail::Scan s;
    s.cancel = &cancel;
    if (s.stop()) return detail::rejected(s);

    std::error_code ec;
    const fs::path root = fs::canonical(game_root, ec);
    if (ec)
    {
        s.fail("game root not found: " + detail::u8(game_root) + ": " + ec.message());
        return detail::rejected(s);
    }
    fs::file_type t;
    if (!detail::type_of(s, root, ".", t)) return detail::rejected(s);
    if (t != fs::file_type::directory)
    {
        s.fail("game root is not a directory");
        return detail::rejected(s);
    }

    if (!detail::type_of(s, root / "Data", "Data", t)) return detail::rejected(s);
    if (t != fs::file_type::directory)
    {
        s.fail(t == fs::file_type::not_found ? "not a BF6 installation: Data missing"
                                             : "Data is a link or not a directory");
        return detail::rejected(s);
    }
    if (!detail::type_of(s, root / "Data" / "layout.toc", "Data/layout.toc", t)) return detail::rejected(s);
    if (t != fs::file_type::regular)
    {
        s.fail(t == fs::file_type::not_found ? "not a BF6 installation: Data/layout.toc missing"
                                             : "Data/layout.toc is a link or not a file");
        return detail::rejected(s);
    }

    for (const char* name : { "Data", "Patch", "Update" })
    {
        if (!detail::type_of(s, root / name, name, t)) return detail::rejected(s);
        if (t == fs::file_type::not_found) continue;
        if (t != fs::file_type::directory)
        {
            s.fail(std::string(name) + " is a link or not a directory");
            return detail::rejected(s);
        }
        if (!detail::walk(s, root / name, name)) return detail::rejected(s);
    }

    if (!detail::type_of(s, root / "SP", "SP", t)) return detail::rejected(s);
    const bool sp = t == fs::file_type::directory;
    if (t != fs::file_type::not_found && !sp)
    {
        s.fail("SP is a link or not a directory");
        return detail::rejected(s);
    }
    std::vector<std::pair<fs::path, std::string>> exes{ { root / "bf6.exe", "bf6.exe" } };
    if (sp) exes.emplace_back(root / "SP" / "bf6.exe", "SP/bf6.exe");
    for (const auto& x : exes)
    {
        if (s.stop()) return detail::rejected(s);
        if (!detail::type_of(s, x.first, x.second, t)) return detail::rejected(s);
        if (t == fs::file_type::not_found) continue;
        if (t != fs::file_type::regular)
        {
            s.fail(x.second + " is a link or not a file");
            return detail::rejected(s);
        }
        if (!detail::add_file(s, x.first, x.second, Kind::exe)) return detail::rejected(s);
    }
    if (s.stop()) return detail::rejected(s);

    Snapshot r;
    r.entries = std::move(s.out);
    std::sort(r.entries.begin(), r.entries.end(),
              [](const Entry& x, const Entry& y) { return x.path < y.path; });

    detail::Hasher h;
    h.u64(kSchema);
    h.u64(r.entries.size());
    for (const Entry& e : r.entries)
    {
        h.str(e.path);
        h.u64((std::uint64_t)e.kind);
        h.u64(e.size);
        h.u64((std::uint64_t)e.mtime_ns);
        h.u64(e.content.a);
        h.u64(e.content.b);
    }
    r.content = h.done();

    r.root = detail::u8(root);
    detail::Hasher hr;
    hr.str(r.root);
    const Fingerprint rf = hr.done();
    r.identity = "bf6-cache-source-v" + std::to_string(kSchema) + ":" +
                 detail::hex(rf.a) + detail::hex(rf.b) + ":" +
                 detail::hex(r.content.a) + detail::hex(r.content.b);
    r.ok = true;
    return r;
}

struct Change {
    enum class Type { added, removed, modified };
    Type type;
    std::string path;
};

struct Comparison {
    bool same = false;         // both ok and identities equal: cache may be reused
    bool comparable = false;   // both ok; changes and root_changed are meaningful
    bool root_changed = false;
    std::vector<Change> changes; // sorted by path
};

inline Comparison compare(const Snapshot& before, const Snapshot& after)
{
    Comparison c;
    if (!before.ok || !after.ok) return c;
    c.comparable = true;
    c.same = before.identity == after.identity;
    c.root_changed = before.root != after.root;

    std::size_t i = 0, j = 0;
    const std::vector<Entry>& x = before.entries;
    const std::vector<Entry>& y = after.entries;
    while (i < x.size() || j < y.size())
    {
        if (j == y.size() || (i < x.size() && x[i].path < y[j].path))
            c.changes.push_back({ Change::Type::removed, x[i++].path });
        else if (i == x.size() || y[j].path < x[i].path)
            c.changes.push_back({ Change::Type::added, y[j++].path });
        else
        {
            if (x[i] != y[j]) c.changes.push_back({ Change::Type::modified, x[i].path });
            ++i, ++j;
        }
    }
    return c;
}

} // namespace bf6_cache
