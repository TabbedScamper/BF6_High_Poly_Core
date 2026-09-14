#pragma once

#include <string>
#include <vector>

struct bf6_ctx;

namespace read_only_online {

// Presentation assets are discovered from the current mounted install.  They
// are never copied beside the executable and no generated manifest is a
// runtime input.
struct InstallImage {
    std::string asset_path;
    std::string label;
    std::string category;
};

struct InstallCatalog {
    std::vector<InstallImage> images;
    int real_prefix_matches = 0;
    int fake_prefix_matches = 0;
};

InstallCatalog discover_install(bf6_ctx* context);

enum class ServiceState {
    Unknown,
    Online,
    Offline,
    Degraded,
    Unavailable
};

struct StatusSnapshot {
    ServiceState state = ServiceState::Unknown;
    std::string label = "UNKNOWN";
    std::string timestamp;
    int http_status = 0;
    bool from_network = false;
    std::string error;
};

// This is deliberately not a general HTTP client.  The policy accepts one
// public, credential-free, GET-only EA endpoint and rejects everything else.
struct RequestSpec {
    std::string method;
    std::string scheme;
    std::string host;
    std::string path;
    bool has_body = false;
    bool has_credentials = false;
    bool has_cookie = false;
};

bool authorize_public_request(const RequestSpec& request);
StatusSnapshot fetch_ea_bf6_status();

enum class OnlineAction {
    Matchmake,
    Host,
    JoinParty,
    Invite,
    SocialMessage,
    Purchase,
    AccountMutation,
    ProfileMutation,
    Telemetry
};

// No online action is legal in the offline viewer.  Keeping this as a single
// executable policy makes it testable and prevents an inert-looking control
// from acquiring network behavior later.
bool online_action_allowed(OnlineAction action);

} // namespace read_only_online
