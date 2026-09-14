#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "live_menu_cache.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <system_error>

namespace live_menu {
namespace {

namespace fs = std::filesystem;

constexpr char kCdnPrefix[] =
    "https://eaassets-a.akamaihd.net/battlelog/battlebinary/glacier/";
constexpr wchar_t kCdnHost[] = L"eaassets-a.akamaihd.net";

const std::set<std::string> kMenuFamilies = {
    "battlepass", "commerce", "commerce-product",
    "emblemsAndChallengeIcons", "first-party-offer", "genericEvent",
    "navBackgrounds", "navBulletins", "navTiles",
    "preApprovedThumbnails", "seasonal", "takeovers", "themedEvent",
    "upsell"
};

struct Cursor {
    const char* p = nullptr;
    const char* end = nullptr;

    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p; }

    bool take(char c) {
        ws();
        if (p >= end || *p != c) return false;
        ++p;
        return true;
    }

    bool string(std::string& out) {
        ws();
        if (p >= end || *p++ != '"') return false;
        out.clear();
        while (p < end) {
            const unsigned char c = static_cast<unsigned char>(*p++);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
            if (p >= end) return false;
            const char e = *p++;
            switch (e) {
            case '"': case '\\': case '/': out.push_back(e); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: return false; // URLs in this manifest require no \u escapes.
            }
        }
        return false;
    }

    bool integer(uint64_t& out) {
        ws();
        const char* first = p;
        while (p < end && *p >= '0' && *p <= '9') ++p;
        if (first == p) return false;
        const auto parsed = std::from_chars(first, p, out);
        return parsed.ec == std::errc{} && parsed.ptr == p;
    }

    bool skip_value() {
        ws();
        if (p >= end) return false;
        if (*p == '"') { std::string ignored; return string(ignored); }
        if (*p == '{') {
            ++p; ws(); if (p < end && *p == '}') { ++p; return true; }
            for (;;) {
                std::string key;
                if (!string(key) || !take(':') || !skip_value()) return false;
                ws(); if (p < end && *p == '}') { ++p; return true; }
                if (!take(',')) return false;
            }
        }
        if (*p == '[') {
            ++p; ws(); if (p < end && *p == ']') { ++p; return true; }
            for (;;) {
                if (!skip_value()) return false;
                ws(); if (p < end && *p == ']') { ++p; return true; }
                if (!take(',')) return false;
            }
        }
        const char* start = p;
        while (p < end && *p != ',' && *p != '}' && *p != ']' &&
               *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
        return p != start;
    }
};

bool parse_entry(Cursor& c, std::map<std::string, std::string>& strings,
                 std::map<std::string, uint64_t>& integers) {
    if (!c.take('{')) return false;
    c.ws();
    if (c.p < c.end && *c.p == '}') { ++c.p; return true; }
    for (;;) {
        std::string key;
        if (!c.string(key) || !c.take(':')) return false;
        c.ws();
        if (c.p < c.end && *c.p == '"') {
            std::string value;
            if (!c.string(value)) return false;
            strings.emplace(std::move(key), std::move(value));
        } else {
            const char* saved = c.p;
            uint64_t value = 0;
            if (c.integer(value)) integers.emplace(std::move(key), value);
            else { c.p = saved; if (!c.skip_value()) return false; }
        }
        c.ws();
        if (c.p < c.end && *c.p == '}') { ++c.p; return true; }
        if (!c.take(',')) return false;
    }
}

std::string family_of(const std::string& url) {
    if (url.rfind(kCdnPrefix, 0) != 0) return {};
    if (url.find('?', sizeof(kCdnPrefix) - 1) != std::string::npos ||
        url.find('#', sizeof(kCdnPrefix) - 1) != std::string::npos)
        return {};
    const size_t begin = sizeof(kCdnPrefix) - 1;
    const size_t slash = url.find('/', begin);
    if (slash == std::string::npos || slash == begin) return {};
    const std::string family = url.substr(begin, slash - begin);
    return kMenuFamilies.count(family) ? family : std::string{};
}

std::string leaf_of(const std::string& url) {
    const size_t slash = url.find_last_of('/');
    return slash == std::string::npos ? url : url.substr(slash + 1);
}

std::string narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), count);
    return result;
}

bool read_file(const fs::path& path, std::vector<uint8_t>& out,
               std::string& error) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec || size > static_cast<uintmax_t>((std::numeric_limits<size_t>::max)())) {
        error = "cannot stat " + narrow(path.wstring());
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { error = "cannot open " + narrow(path.wstring()); return false; }
    out.resize(static_cast<size_t>(size));
    if (!out.empty()) stream.read(reinterpret_cast<char*>(out.data()),
                                  static_cast<std::streamsize>(out.size()));
    if (!stream) { error = "short read " + narrow(path.wstring()); out.clear(); return false; }
    return true;
}

} // namespace

Catalog read(const std::wstring& manifest_path) {
    Catalog result;
    result.manifest_path = manifest_path;
    const fs::path manifest(manifest_path);
    std::vector<uint8_t> raw;
    if (!read_file(manifest, raw, result.error)) return result;
    while (!raw.empty() && raw.back() == 0) raw.pop_back();

    const std::string text(raw.begin(), raw.end());
    const size_t marker = text.find("\"entries\"");
    if (marker == std::string::npos) { result.error = "manifest has no entries array"; return result; }
    Cursor c{text.data() + marker + 9, text.data() + text.size()};
    if (!c.take(':') || !c.take('[')) { result.error = "malformed entries array"; return result; }
    const fs::path cache = manifest.parent_path();
    c.ws();
    while (c.p < c.end && *c.p != ']') {
        std::map<std::string, std::string> strings;
        std::map<std::string, uint64_t> integers;
        if (!parse_entry(c, strings, integers)) {
            result.assets.clear();
            result.known_assets.clear();
            result.error = "malformed cache entry";
            return result;
        }
        ++result.manifest_entries;
        const auto urlIt = strings.find("url");
        const auto idIt = integers.find("id");
        const auto sizeIt = integers.find("size");
        const auto usedIt = integers.find("lastUsed");
        const std::string family = urlIt == strings.end() ? std::string{} : family_of(urlIt->second);
        if (family.empty() || idIt == integers.end() || sizeIt == integers.end() ||
            idIt->second > static_cast<uint64_t>((std::numeric_limits<int>::max)())) {
            ++result.rejected_entries;
        } else {
            const fs::path payload = cache / std::to_wstring(idIt->second);
            Asset asset;
            asset.id = static_cast<int>(idIt->second);
            asset.expected_size = sizeIt->second;
            asset.last_used = usedIt == integers.end() ? 0 : usedIt->second;
            asset.url = urlIt->second;
            asset.family = family;
            asset.name = leaf_of(asset.url);
            asset.cache_path = payload.wstring();
            result.known_assets.push_back(asset);
            std::error_code ec;
            const uintmax_t actual = fs::file_size(payload, ec);
            if (ec || actual != sizeIt->second) ++result.evicted_entries;
            else
                result.assets.push_back(std::move(asset));
        }
        c.ws();
        if (c.p < c.end && *c.p == ',') { ++c.p; c.ws(); }
        else break;
    }
    if (c.p >= c.end || *c.p != ']') {
        result.assets.clear();
        result.known_assets.clear();
        result.error = "unterminated entries array";
        return result;
    }
    std::stable_sort(result.assets.begin(), result.assets.end(),
        [](const Asset& a, const Asset& b) {
            if (a.last_used != b.last_used) return a.last_used > b.last_used;
            return a.id > b.id;
        });
    std::stable_sort(result.known_assets.begin(), result.known_assets.end(),
        [](const Asset& a, const Asset& b) {
            if (a.last_used != b.last_used) return a.last_used > b.last_used;
            return a.id > b.id;
        });
    return result;
}

Catalog discover() {
    Catalog failure;
    wchar_t local[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!length || length >= MAX_PATH) {
        failure.error = "LOCALAPPDATA is unavailable";
        return failure;
    }
    const fs::path temp = fs::path(local) / L"Temp";
    std::error_code ec;
    fs::path newest;
    fs::file_time_type newestTime{};
    for (const fs::directory_entry& entry : fs::directory_iterator(temp, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec)) continue;
        const std::wstring name = entry.path().filename().wstring();
        if (name.rfind(L"Battlefield", 0) != 0 || name.find(L'6') == std::wstring::npos)
            continue;
        const fs::path candidate = entry.path() / L"httpcache" / L"manifest";
        if (!fs::is_regular_file(candidate, ec)) continue;
        const fs::file_time_type time = fs::last_write_time(candidate, ec);
        if (!ec && (newest.empty() || time > newestTime)) {
            newest = candidate;
            newestTime = time;
        }
    }
    if (newest.empty()) {
        failure.error = "no Battlefield*6/httpcache/manifest below LOCALAPPDATA\\Temp";
        return failure;
    }
    return read(newest.wstring());
}

std::vector<Asset> newest_cohort(const Catalog& catalog,
                                 const char* family,
                                 uint64_t last_used_window) {
    std::vector<Asset> result;
    if (!family || !*family) return result;
    uint64_t newest = 0;
    for (const Asset& asset : catalog.assets)
        if (asset.family == family)
            newest = (std::max)(newest, asset.last_used);
    for (const Asset& asset : catalog.assets) {
        if (asset.family != family || newest < asset.last_used ||
            newest - asset.last_used > last_used_window)
            continue;
        result.push_back(asset);
    }
    return result;
}

std::vector<Asset> newest_cohort_matching(const Catalog& catalog,
                                          const char* family,
                                          uint64_t last_used_window,
                                          const char* identity_token) {
    std::vector<Asset> result;
    if (!identity_token || !*identity_token) return result;
    std::string token(identity_token);
    std::transform(token.begin(), token.end(), token.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::vector<Asset> cohort = newest_cohort(
        catalog, family, last_used_window);
    for (const Asset& asset : cohort) {
        std::string identity = asset.name + " " + asset.url;
        std::transform(identity.begin(), identity.end(), identity.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (identity.find(token) != std::string::npos)
            result.push_back(asset);
    }
    return result;
}

std::vector<Asset> matching(const Catalog& catalog,
                            const char* family,
                            const char* identity_token) {
    std::vector<Asset> result;
    if (!family || !*family || !identity_token || !*identity_token)
        return result;
    std::string token(identity_token);
    std::transform(token.begin(), token.end(), token.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const Asset& asset : catalog.assets) {
        if (asset.family != family) continue;
        std::string identity = asset.name + " " + asset.url;
        std::transform(identity.begin(), identity.end(), identity.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (identity.find(token) != std::string::npos)
            result.push_back(asset);
    }
    return result;
}

std::vector<Asset> matching_known(const Catalog& catalog,
                                  const char* family,
                                  const char* identity_token) {
    std::vector<Asset> result;
    if (!family || !*family || !identity_token || !*identity_token)
        return result;
    std::string token(identity_token);
    std::transform(token.begin(), token.end(), token.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const Asset& asset : catalog.known_assets) {
        if (asset.family != family) continue;
        std::string identity = asset.name + " " + asset.url;
        std::transform(identity.begin(), identity.end(), identity.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (identity.find(token) != std::string::npos)
            result.push_back(asset);
    }
    return result;
}

bool read_payload(const Asset& asset, std::vector<uint8_t>& bytes,
                  std::string& error) {
    if (asset.id < 0 || asset.cache_path.empty()) {
        error = "invalid cache asset";
        return false;
    }
    if (!read_file(fs::path(asset.cache_path), bytes, error)) return false;
    if (bytes.size() != asset.expected_size) {
        error = "cache payload changed size after manifest read";
        bytes.clear();
        return false;
    }
    return true;
}

bool read_online_payload(const Asset& asset, std::vector<uint8_t>& bytes,
                         std::string& error) {
    bytes.clear();
    constexpr uint64_t kMaximumPayload = 32ull * 1024ull * 1024ull;
    if (asset.id < 0 || asset.expected_size == 0 ||
        asset.expected_size > kMaximumPayload ||
        asset.url.rfind(kCdnPrefix, 0) != 0 ||
        family_of(asset.url).empty()) {
        error = "online menu asset rejected by CDN policy";
        return false;
    }
    const size_t hostEnd = asset.url.find('/', sizeof("https://") - 1);
    if (hostEnd == std::string::npos) {
        error = "online menu asset has no request path";
        return false;
    }
    const std::wstring path = widen(asset.url.substr(hostEnd));
    if (path.empty()) {
        error = "online menu asset path is not UTF-8";
        return false;
    }

    HINTERNET session = WinHttpOpen(
        L"BF6OfflineViewer/1.0 (read-only presentation)",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { error = "WinHttpOpen failed"; return false; }
    WinHttpSetTimeouts(session, 2000, 2000, 5000, 5000);
    HINTERNET connection = WinHttpConnect(
        session, kCdnHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) {
        error = "WinHttpConnect failed";
        WinHttpCloseHandle(session);
        return false;
    }
    HINTERNET request = WinHttpOpenRequest(
        connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        error = "WinHttpOpenRequest failed";
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE,
                     &disabled, sizeof(disabled));
    bool ok = WinHttpSendRequest(
                  request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE &&
              WinHttpReceiveResponse(request, nullptr) != FALSE;
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (ok)
        ok = WinHttpQueryHeaders(
                 request,
                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                 WINHTTP_NO_HEADER_INDEX) != FALSE && status == 200;
    while (ok && bytes.size() < asset.expected_size) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            ok = false;
            break;
        }
        if (!available) break;
        const uint64_t remaining = asset.expected_size - bytes.size();
        if (static_cast<uint64_t>(available) > remaining) {
            ok = false;
            error = "online menu payload exceeds manifest size";
            break;
        }
        const size_t oldSize = bytes.size();
        bytes.resize(oldSize + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, bytes.data() + oldSize,
                             available, &read) || read == 0) {
            ok = false;
            break;
        }
        bytes.resize(oldSize + read);
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (!ok || bytes.size() != asset.expected_size) {
        if (error.empty())
            error = status == 200 ? "online menu payload size mismatch"
                                  : "online menu GET failed";
        bytes.clear();
        return false;
    }
    return true;
}

} // namespace live_menu
