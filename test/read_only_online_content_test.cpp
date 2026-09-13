#include "read_only_online_content.h"
#include "bf6_core.h"

#include <cstdio>
#include <string>

using namespace read_only_online;

static bool expect(bool condition, const char* message)
{
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

int main(int argc, char** argv)
{
    bool ok = true;
    const RequestSpec exact{"GET", "https", "help.ea.com",
        "/_data/server-status/v1/server-statuses", false, false, false};
    ok &= expect(authorize_public_request(exact), "exact public GET rejected");
    for (RequestSpec changed : {
             RequestSpec{"POST", exact.scheme, exact.host, exact.path, false, false, false},
             RequestSpec{exact.method, "http", exact.host, exact.path, false, false, false},
             RequestSpec{exact.method, exact.scheme, "ea.com", exact.path, false, false, false},
             RequestSpec{exact.method, exact.scheme, exact.host, "/", false, false, false},
             RequestSpec{exact.method, exact.scheme, exact.host, exact.path, true, false, false},
             RequestSpec{exact.method, exact.scheme, exact.host, exact.path, false, true, false},
             RequestSpec{exact.method, exact.scheme, exact.host, exact.path, false, false, true}})
        ok &= expect(!authorize_public_request(changed), "mutated request was accepted");

    for (OnlineAction action : {
             OnlineAction::Matchmake, OnlineAction::Host,
             OnlineAction::JoinParty, OnlineAction::Invite,
             OnlineAction::SocialMessage, OnlineAction::Purchase,
             OnlineAction::AccountMutation, OnlineAction::ProfileMutation,
             OnlineAction::Telemetry})
        ok &= expect(!online_action_allowed(action), "online action was accepted");

    if (argc > 1)
    {
        char error[512]{};
        bf6_ctx* context = bf6_open(argv[1], error, sizeof(error));
        ok &= expect(context != nullptr, error);
        if (context)
        {
            ok &= expect(bf6_mount_frontend(context, error, sizeof(error)) != 0,
                         error);
            const InstallCatalog catalog = discover_install(context);
            std::printf("install cards: real=%d fake=%d images=%zu\n",
                        catalog.real_prefix_matches,
                        catalog.fake_prefix_matches, catalog.images.size());
            ok &= expect(catalog.real_prefix_matches > 0,
                         "no installed home presentation assets found");
            ok &= expect(catalog.fake_prefix_matches == 0,
                         "fake install prefix unexpectedly matched");
            bf6_close(context);
        }
    }
    return ok ? 0 : 1;
}
