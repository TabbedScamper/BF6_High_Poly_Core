#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "read_only_online_content.h"

#include "bf6_core.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <vector>

namespace read_only_online {
namespace {

constexpr char kHomeAssetRoot[] = "common/ui/home/assets/";
constexpr char kModePrefix[] = "common/ui/home/assets/temp/t_ui_gamemodes_tilebg_";
constexpr char kPortalPrefix[] = "common/ui/home/assets/temp/t_ui_portal_";
constexpr char kBulletinPrefix[] = "common/ui/home/assets/temp/t_ui_bulletin_";
constexpr char kFakePrefix[] = "common/ui/home/assets/__control_never_exists__/";

std::string lower_ascii(std::string value)
{
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

bool starts_with(const std::string& value, const char* prefix)
{
    const size_t n = std::char_traits<char>::length(prefix);
    return value.size() >= n && value.compare(0, n, prefix) == 0;
}

std::string label_from_asset(const std::string& path, const char* prefix)
{
    std::string label = path.substr(std::char_traits<char>::length(prefix));
    for (char& c : label)
    {
        if (c == '_') c = ' ';
        else c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return label;
}

std::string json_string_after(const std::string& json, const char* key)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return {};
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return {};
    p = json.find('"', p + 1);
    if (p == std::string::npos) return {};
    const size_t e = json.find('"', p + 1);
    if (e == std::string::npos) return {};
    return json.substr(p + 1, e - p - 1);
}

} // namespace

InstallCatalog discover_install(bf6_ctx* context)
{
    InstallCatalog result;
    if (!context) return result;

    const int count = bf6_list_ebx(context, kHomeAssetRoot, nullptr, 0);
    if (count <= 0) return result;
    std::vector<bf6_asset> rows(static_cast<size_t>(count));
    const int got = bf6_list_ebx(context, kHomeAssetRoot,
                                 rows.data(), count);
    for (int i = 0; i < got; ++i)
    {
        if (!rows[static_cast<size_t>(i)].name) continue;
        const std::string path = lower_ascii(rows[static_cast<size_t>(i)].name);
        if (starts_with(path, kFakePrefix))
        {
            ++result.fake_prefix_matches;
            continue;
        }
        InstallImage image;
        image.asset_path = path;
        if (starts_with(path, kModePrefix))
        {
            image.category = "game-mode";
            image.label = label_from_asset(path, kModePrefix);
        }
        else if (starts_with(path, kPortalPrefix))
        {
            image.category = "portal";
            image.label = label_from_asset(path, kPortalPrefix);
        }
        else if (starts_with(path, kBulletinPrefix))
        {
            image.category = "bulletin";
            image.label = label_from_asset(path, kBulletinPrefix);
        }
        else
            continue;
        ++result.real_prefix_matches;
        result.images.push_back(std::move(image));
    }
    std::sort(result.images.begin(), result.images.end(),
              [](const InstallImage& a, const InstallImage& b) {
                  return a.asset_path < b.asset_path;
              });
    return result;
}

bool authorize_public_request(const RequestSpec& request)
{
    return request.method == "GET" &&
           request.scheme == "https" &&
           request.host == "help.ea.com" &&
           request.path == "/_data/server-status/v1/server-statuses" &&
           !request.has_body && !request.has_credentials && !request.has_cookie;
}

StatusSnapshot fetch_ea_bf6_status()
{
    StatusSnapshot result;
    const RequestSpec request{
        "GET", "https", "help.ea.com",
        "/_data/server-status/v1/server-statuses", false, false, false};
    if (!authorize_public_request(request))
    {
        result.error = "public status request rejected by policy";
        return result;
    }

    HINTERNET session = WinHttpOpen(
        L"BF6OfflineViewer/1.0 (read-only status)",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
    {
        result.error = "WinHttpOpen failed";
        return result;
    }
    WinHttpSetTimeouts(session, 1500, 1500, 2000, 2000);
    HINTERNET connection = WinHttpConnect(
        session, L"help.ea.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection)
    {
        result.error = "WinHttpConnect failed";
        WinHttpCloseHandle(session);
        return result;
    }
    HINTERNET handle = WinHttpOpenRequest(
        connection, L"GET", L"/_data/server-status/v1/server-statuses",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!handle)
    {
        result.error = "WinHttpOpenRequest failed";
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return result;
    }
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(handle, WINHTTP_OPTION_DISABLE_FEATURE,
                     &disabled, sizeof(disabled));

    bool ok = WinHttpSendRequest(handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE &&
              WinHttpReceiveResponse(handle, nullptr) != FALSE;
    DWORD status = 0;
    DWORD statusBytes = sizeof(status);
    if (ok)
        WinHttpQueryHeaders(handle,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes,
            WINHTTP_NO_HEADER_INDEX);
    result.http_status = static_cast<int>(status);

    std::string body;
    constexpr size_t kResponseLimit = 256 * 1024;
    while (ok && body.size() < kResponseLimit)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(handle, &available)) { ok = false; break; }
        if (!available) break;
        const size_t remaining = kResponseLimit - body.size();
        const DWORD take = static_cast<DWORD>((std::min)(
            remaining, static_cast<size_t>(available)));
        std::vector<char> buffer(take);
        DWORD read = 0;
        if (!WinHttpReadData(handle, buffer.data(), take, &read))
        { ok = false; break; }
        body.append(buffer.data(), read);
        if (take < available) { ok = false; result.error = "response too large"; break; }
    }

    WinHttpCloseHandle(handle);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (!ok || status != 200)
    {
        result.state = ServiceState::Unavailable;
        result.label = "UNAVAILABLE";
        if (result.error.empty()) result.error = "public status GET failed";
        return result;
    }
    std::string state = json_string_after(body, "battlefield-6");
    result.timestamp = json_string_after(body, "timestamp");
    result.from_network = true;
    const std::string folded = lower_ascii(state);
    if (folded == "online")
    { result.state = ServiceState::Online; result.label = "ONLINE"; }
    else if (folded == "offline")
    { result.state = ServiceState::Offline; result.label = "OFFLINE"; }
    else if (folded == "degraded" || folded == "limited")
    { result.state = ServiceState::Degraded; result.label = "DEGRADED"; }
    else
    {
        result.state = ServiceState::Unknown;
        result.label = state.empty() ? "UNKNOWN" : state;
        result.error = state.empty() ? "battlefield-6 status missing" : "unrecognized status";
    }
    return result;
}

bool online_action_allowed(OnlineAction)
{
    return false;
}

} // namespace read_only_online
