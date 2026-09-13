/* Cache source identity over synthetic installs; no game data needed.
 *
 *   cache_identity_test <scratch-parent>
 *
 * Creates one uniquely named child of the parent and builds tiny fake installs
 * inside it. Nothing here deletes a directory tree: every file, link and
 * directory this test created is removed individually, newest first, with the
 * non-recursive fs::remove, and anything that could not be removed is reported.
 *
 * CAS changes are only exercised through size or mtime. The helper does not
 * claim to see a CAS rewrite that keeps both, so neither does this test.
 */
#include "bf6_cache_identity.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

static int g_fail = 0;
static std::vector<fs::path> g_made; // creation order

static void check(bool ok, const char* what, const std::string& got = std::string())
{
    std::printf("%-56s %s%s%s\n", what, ok ? "ok" : "FAIL",
                got.empty() ? "" : "  got ", got.c_str());
    if (!ok) g_fail++;
}

static bool make_dir(const fs::path& p)
{
    std::error_code ec;
    if (fs::is_directory(fs::symlink_status(p, ec))) return true;
    if (!fs::create_directory(p, ec)) return false;
    g_made.push_back(p);
    return true;
}

// Creates parents as needed, so creation order follows file order.
static bool write_file(const fs::path& root, const std::string& rel, const std::string& bytes)
{
    if (!make_dir(root)) return false;
    fs::path p = root;
    const fs::path r(rel);
    for (auto it = r.begin(); it != r.end(); ++it)
    {
        if (std::next(it) == r.end()) break;
        p /= *it;
        if (!make_dir(p)) return false;
    }
    p = root / r;
    std::error_code ec;
    const bool fresh = !fs::exists(fs::symlink_status(p, ec));
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), (std::streamsize)bytes.size());
    f.close();
    if (!f) return false;
    if (fresh) g_made.push_back(p);
    return true;
}

static bool set_mtime(const fs::path& p, fs::file_time_type t)
{
    std::error_code ec;
    fs::last_write_time(p, t, ec);
    return !ec;
}

static void cleanup()
{
    for (auto it = g_made.rbegin(); it != g_made.rend(); ++it)
    {
        std::error_code ec;
        fs::remove(*it, ec); // never remove_all
        if (fs::exists(fs::symlink_status(*it, ec)))
            std::printf("left behind: %s\n", it->string().c_str());
    }
}

struct File { const char* rel; const char* bytes; };

// "Data/Win32/B.cas" sorts before "Data/Win32/a.toc" bytewise but after it in
// NTFS's case-insensitive enumeration, so the sort is observable.
static const File kInstall[] = {
    { "Data/layout.toc",        "LAYOUT-0001" },
    { "Data/Win32/a.toc",       "toc-a-0001" },
    { "Data/Win32/B.cas",       "casdata-1" },
    { "Data/Win32/readme.txt",  "ignored" },
    { "Patch/layout.toc",       "patch-layout" },
    { "Patch/Win32/cas_02.cas", "cas-2" },
    { "bf6.exe",                "MZ-mp" },
    { "SP/bf6.exe",             "MZ-sp" },
};
static const size_t kRecorded = 7; // readme.txt is not metadata

static bool build(const fs::path& root, bool reverse, fs::file_time_type t0)
{
    if (!make_dir(root)) return false;
    const size_t n = sizeof(kInstall) / sizeof(kInstall[0]);
    for (size_t k = 0; k < n; ++k)
    {
        const File& f = kInstall[reverse ? n - 1 - k : k];
        if (!write_file(root, f.rel, f.bytes)) return false;
    }
    for (const File& f : kInstall)
        if (!set_mtime(root / f.rel, t0)) return false;
    return true;
}

static std::string describe(const bf6_cache::Comparison& c)
{
    std::string s;
    for (const bf6_cache::Change& x : c.changes)
    {
        s += x.type == bf6_cache::Change::Type::added   ? "added:" :
             x.type == bf6_cache::Change::Type::removed ? "removed:" : "modified:";
        s += x.path + ";";
    }
    return s;
}

static bool empty_rejection(const bf6_cache::Snapshot& s)
{
    return !s.ok && s.identity.empty() && s.root.empty() && s.entry_count() == 0 &&
           s.content == bf6_cache::Fingerprint();
}

static int run(const fs::path& base)
{
    using bf6_cache::inspect;
    using bf6_cache::compare;

    const auto now = fs::file_time_type::clock::now() - std::chrono::hours(48);
    const fs::file_time_type t0(std::chrono::duration_cast<fs::file_time_type::duration>(
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())));

    const fs::path a = base / "install_a", b = base / "install_b";
    if (!build(a, false, t0) || !build(b, true, t0))
    { std::fprintf(stderr, "could not build synthetic installs\n"); return 1; }

    // Stable snapshot.
    const bf6_cache::Snapshot a1 = inspect(a), a2 = inspect(a);
    check(a1.ok && !a1.cancelled && a1.error.empty(), "install a inspects", a1.error);
    check(a1.entry_count() == kRecorded, "toc/cas/exe entries recorded", std::to_string(a1.entry_count()));
    check(!a1.identity.empty() && a1.identity == a2.identity, "identity stable across runs", a2.identity);
    check(a1.entries == a2.entries, "entries stable across runs");
    check(compare(a1, a2).same && compare(a1, a2).changes.empty(), "compare reports same");
    check(a1.identity.rfind("bf6-cache-source-v1:", 0) == 0, "identity is versioned", a1.identity);
    check(!a1.entries.empty() && a1.entries[0].path == "Data/Win32/B.cas", "entries sorted bytewise",
          a1.entries.empty() ? "" : a1.entries[0].path);
    bool meta = true;
    for (const bf6_cache::Entry& e : a1.entries)
    {
        const bool toc = e.kind == bf6_cache::Kind::toc;
        meta = meta && e.mtime_ns == a1.entries[0].mtime_ns && e.size > 0 &&
               toc == (e.content != bf6_cache::Fingerprint());
    }
    check(meta, "sizes, mtimes, toc-only fingerprints");

    // Creation order and root are independent of content.
    const bf6_cache::Snapshot b1 = inspect(b);
    check(b1.ok && b1.content == a1.content && b1.entries == a1.entries,
          "reversed creation order gives same content", b1.error);
    check(b1.root != a1.root && b1.identity != a1.identity, "switched root changes identity");
    const bf6_cache::Comparison ab = compare(a1, b1);
    check(!ab.same && ab.root_changed && ab.changes.empty(), "switch reported as root change only", describe(ab));

    // TOC rewritten with same size and restored mtime.
    const fs::path toc = a / "Data/Win32/a.toc";
    std::error_code ec;
    const auto toc_size = fs::file_size(toc, ec);
    write_file(a, "Data/Win32/a.toc", "toc-a-0002");
    set_mtime(toc, t0);
    check(fs::file_size(toc, ec) == toc_size && fs::last_write_time(toc, ec) == t0,
          "precondition: toc size and mtime unchanged");
    const bf6_cache::Snapshot a3 = inspect(a);
    check(a3.ok && a3.content != a1.content && a3.identity != a1.identity, "same-size same-mtime toc edit detected");
    check(describe(compare(a1, a3)) == "modified:Data/Win32/a.toc;", "toc edit summary", describe(compare(a1, a3)));

    // CAS resize with restored mtime, then a timestamp-only touch.
    write_file(a, "Data/Win32/B.cas", "casdata-1+grown");
    set_mtime(a / "Data/Win32/B.cas", t0);
    const bf6_cache::Snapshot a4 = inspect(a);
    check(a4.ok && describe(compare(a3, a4)) == "modified:Data/Win32/B.cas;", "cas resize detected", describe(compare(a3, a4)));
    set_mtime(a / "Data/Win32/B.cas", t0 + std::chrono::hours(1));
    const bf6_cache::Snapshot a5 = inspect(a);
    check(a5.ok && describe(compare(a4, a5)) == "modified:Data/Win32/B.cas;", "cas mtime detected", describe(compare(a4, a5)));

    // Executable touch.
    set_mtime(a / "SP/bf6.exe", t0 + std::chrono::hours(2));
    const bf6_cache::Snapshot a6 = inspect(a);
    check(a6.ok && describe(compare(a5, a6)) == "modified:SP/bf6.exe;", "exe mtime detected", describe(compare(a5, a6)));

    // Add then remove an archive; removal restores the prior identity.
    write_file(a, "Patch/Win32/cas_03.cas", "cas-3");
    const bf6_cache::Snapshot a7 = inspect(a);
    check(a7.ok && describe(compare(a6, a7)) == "added:Patch/Win32/cas_03.cas;", "added archive detected", describe(compare(a6, a7)));
    fs::remove(a / "Patch/Win32/cas_03.cas", ec);
    const bf6_cache::Snapshot a8 = inspect(a);
    check(a8.ok && describe(compare(a7, a8)) == "removed:Patch/Win32/cas_03.cas;", "removed archive detected", describe(compare(a7, a8)));
    check(a8.identity == a6.identity, "removal restores prior identity");

    // Invalid installs.
    const fs::path c = base / "install_no_layout";
    write_file(c, "Data/Win32/a.toc", "toc");
    const bf6_cache::Snapshot c1 = inspect(c);
    check(empty_rejection(c1) && !c1.cancelled && !c1.error.empty(), "missing Data/layout.toc rejected", c1.error);
    const fs::path d = base / "install_no_data";
    write_file(d, "bf6.exe", "MZ");
    const bf6_cache::Snapshot d1 = inspect(d);
    check(empty_rejection(d1) && !d1.error.empty(), "missing Data rejected", d1.error);
    const bf6_cache::Snapshot e1 = inspect(base / "does_not_exist");
    check(empty_rejection(e1) && !e1.error.empty(), "nonexistent root rejected", e1.error);
    const bf6_cache::Comparison ce = compare(a8, c1);
    check(!ce.same && !ce.comparable, "rejected snapshot never compares same");

    // Cancellation at every poll point publishes nothing.
    int polls = 0;
    const bf6_cache::Snapshot counted = inspect(a, [&] { ++polls; return false; });
    check(counted.ok && counted.identity == a8.identity, "counting callback does not perturb", std::to_string(polls));
    bool all_cancelled = polls > 1;
    for (int k = 1; k <= polls; ++k)
    {
        int n = 0;
        const bf6_cache::Snapshot s = inspect(a, [&] { return ++n >= k; });
        all_cancelled = all_cancelled && s.cancelled && empty_rejection(s);
    }
    check(all_cancelled, "cancel at each of the poll points is empty", std::to_string(polls));
    int n = 0;
    const bf6_cache::Snapshot late = inspect(a, [&] { return ++n > polls; });
    check(late.ok && !late.cancelled && late.identity == a8.identity, "cancel after last poll completes");

    // Links are refused, not followed. Symlink creation needs privilege on
    // Windows, so this is skipped when the platform refuses.
    write_file(base, "outside/outside.toc", "outside");
    const fs::path link = a / "Data" / "linked";
    fs::create_directory_symlink(base / "outside", link, ec);
    if (ec)
        std::printf("%-56s skip  (%s)\n", "directory symlink inside Data refused", ec.message().c_str());
    else
    {
        g_made.push_back(link);
        const bf6_cache::Snapshot l1 = inspect(a);
        check(empty_rejection(l1) && l1.error.find("link") != std::string::npos,
              "directory symlink inside Data refused", l1.error);
        fs::remove(link, ec);
        const bf6_cache::Snapshot l2 = inspect(a);
        check(l2.ok && l2.identity == a8.identity, "identity returns after link removed");
    }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: cache_identity_test <scratch-parent>\n"); return 2; }
    const fs::path parent(argv[1]);
    std::error_code ec;
    if (!fs::is_directory(parent, ec)) { std::fprintf(stderr, "not a directory: %s\n", argv[1]); return 2; }

    std::random_device rd;
    std::mt19937_64 rng(((unsigned long long)rd() << 32) ^ rd() ^
                        (unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count());
    fs::path base;
    for (int attempt = 0; attempt < 16 && base.empty(); ++attempt)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "bf6_cache_identity_test_%016llx", (unsigned long long)rng());
        const fs::path p = parent / name;
        if (fs::create_directory(p, ec)) { base = p; g_made.push_back(p); }
    }
    if (base.empty()) { std::fprintf(stderr, "could not create a unique child of %s\n", argv[1]); return 1; }
    std::printf("scratch: %s\n", base.string().c_str());

    const int rc = run(base);
    cleanup();
    if (rc) return rc;
    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
