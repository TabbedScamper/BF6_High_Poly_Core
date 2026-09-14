/* Small UTF-8 file helpers for the editor-side modules (game log, capability
 * store): paths are UTF-8 everywhere and become wide on Windows, because the
 * folders these read have names no ANSI code page can spell - Battlefield's
 * temp folder carries a mojibake trademark sign permanently. Reads share the
 * file with a writer that still has it open. */
#ifndef LIBBF6_UTF8_FS_H
#define LIBBF6_UTF8_FS_H

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <filesystem>
#  include <fstream>
#  include <sys/stat.h>
#endif

namespace bf6fs {

#if defined(_WIN32)
inline std::wstring wide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

inline std::string narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
#endif

inline std::string join(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    const char last = a.back();
    return (last == '/' || last == '\\') ? a + b : a + "/" + b;
}

inline std::string env(const char* name)
{
#if defined(_WIN32)
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(wide(name).c_str(), buf, 32768);
    return (n > 0 && n < 32768) ? narrow(std::wstring(buf, n)) : std::string();
#else
    const char* v = std::getenv(name);
    return v ? v : "";
#endif
}

inline bool is_file(const std::string& p)
{
#if defined(_WIN32)
    DWORD a = GetFileAttributesW(wide(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st; return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

inline bool is_dir(const std::string& p)
{
#if defined(_WIN32)
    DWORD a = GetFileAttributesW(wide(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

/* Names (not paths) in a directory; dirs or files, pattern "*" style prefix match. */
inline std::vector<std::string> list(const std::string& dir, bool want_dirs, const std::string& prefix = "",
                                     const std::string& suffix = "")
{
    std::vector<std::string> out;
#if defined(_WIN32)
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wide(join(dir, "*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string name = narrow(fd.cFileName);
        if (name == "." || name == "..") continue;
        const bool d = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (d != want_dirs) continue;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        if (suffix.size() > name.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        out.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (e.is_directory() != want_dirs) continue;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        if (suffix.size() > name.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        out.push_back(name);
    }
#endif
    return out;
}

/* Size and last-write time (unix seconds); false when absent. */
inline bool stat_file(const std::string& p, int64_t& size, int64_t& mtime)
{
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(wide(p).c_str(), GetFileExInfoStandard, &d)) return false;
    size = ((int64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    const int64_t ft = ((int64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
    mtime = ft / 10000000 - 11644473600LL;
    return true;
#else
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return false;
    size = (int64_t)st.st_size;
    mtime = (int64_t)st.st_mtime;
    return true;
#endif
}

/* Read [offset, offset+max) sharing the file with a writer. */
inline bool read_range(const std::string& p, int64_t offset, int64_t max, std::string& out, int64_t& file_size)
{
    out.clear();
#if defined(_WIN32)
    HANDLE h = CreateFileW(wide(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return false; }
    file_size = sz.QuadPart;
    if (offset < 0) offset = 0;
    int64_t want = file_size - offset;
    if (want > max) want = max;
    if (want > 0) {
        LARGE_INTEGER pos; pos.QuadPart = offset;
        SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
        out.resize((size_t)want);
        DWORD got = 0;
        if (!ReadFile(h, &out[0], (DWORD)want, &got, nullptr)) { CloseHandle(h); out.clear(); return false; }
        out.resize(got);
    }
    CloseHandle(h);
    return true;
#else
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    file_size = (int64_t)f.tellg();
    if (offset < 0) offset = 0;
    int64_t want = file_size - offset;
    if (want > max) want = max;
    if (want > 0) {
        f.seekg(offset);
        out.resize((size_t)want);
        f.read(&out[0], want);
        out.resize((size_t)f.gcount());
    }
    return true;
#endif
}

inline bool read_all(const std::string& p, std::string& out)
{
    int64_t size = 0;
    return read_range(p, 0, INT64_MAX / 2, out, size);
}

inline bool make_dirs(const std::string& p)
{
    if (p.empty() || is_dir(p)) return true;
    const size_t cut = p.find_last_of("/\\");
    if (cut != std::string::npos && cut > 0) {
        std::string parent = p.substr(0, cut);
        if (!(parent.size() == 2 && parent[1] == ':') && !make_dirs(parent)) return false;
    }
#if defined(_WIN32)
    return CreateDirectoryW(wide(p).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    std::error_code ec;
    std::filesystem::create_directory(p, ec);
    return is_dir(p);
#endif
}

inline bool write_all(const std::string& p, const std::string& data)
{
#if defined(_WIN32)
    HANDLE h = CreateFileW(wide(p).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    const bool ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size());
    CloseHandle(h);
    return ok;
#else
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    return (bool)f;
#endif
}

/* Rename, replacing the destination. */
inline bool move_replace(const std::string& from, const std::string& to)
{
#if defined(_WIN32)
    return MoveFileExW(wide(from).c_str(), wide(to).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    return !ec;
#endif
}

inline void remove_file(const std::string& p)
{
#if defined(_WIN32)
    DeleteFileW(wide(p).c_str());
#else
    std::remove(p.c_str());
#endif
}

inline void remove_tree(const std::string& p)
{
#if defined(_WIN32)
    for (const std::string& f : list(p, false)) DeleteFileW(wide(join(p, f)).c_str());
    for (const std::string& d : list(p, true)) remove_tree(join(p, d));
    RemoveDirectoryW(wide(p).c_str());
#else
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
#endif
}

inline std::string utc_iso(int64_t unix_seconds)
{
    std::time_t t = (std::time_t)unix_seconds;
    std::tm g{};
#if defined(_WIN32)
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char b[40];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

inline int64_t now_unix() { return (int64_t)std::time(nullptr); }

} // namespace bf6fs

#endif
