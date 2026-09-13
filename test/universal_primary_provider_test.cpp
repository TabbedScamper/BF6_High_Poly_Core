#include "bf6_core.h"
#include "universal_primary_provider.h"

#include <cstdio>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr,
            "usage: universal_primary_provider_test <game-dir>\n");
        return 2;
    }
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, sizeof(error));
    if (!context)
    {
        std::fprintf(stderr, "open: %s\n", error);
        return 1;
    }
    if (!bf6_mount_frontend(context, error, sizeof(error)))
    {
        std::fprintf(stderr, "mount: %s\n", error);
        bf6_close(context);
        return 1;
    }
    universal_primary::Navigation navigation;
    const bool ok = universal_primary::read(context, navigation);
    const auto event = universal_primary::visible_offline(navigation, true);
    const auto plain = universal_primary::visible_offline(navigation, false);
    std::printf(
        "connections=%d localized=%d paths=%d collections=%d "
        "event=%zu/%zu plain=%zu/%zu ambiguous=%d controls=%d/%d\n",
        navigation.report.connections, navigation.report.localized_sources,
        navigation.report.complete_paths, navigation.report.collections,
        event.size(), navigation.event_entries.size(), plain.size(),
        navigation.plain_entries.size(), navigation.report.ambiguous_paths,
        navigation.report.input_hash_control_matches,
        navigation.report.fake_partition_connections);
    for (const auto& entry : event)
        std::printf("%d\t%s\n", entry.input, entry.label.c_str());
    bf6_close(context);
    const bool expected = ok && event.size() == 7 && plain.size() == 6 &&
        !event.empty() && event.front().label == "PLAY" &&
        navigation.report.ambiguous_paths == 0;
    std::puts(expected ? "PASS" : "FAIL");
    return expected ? 0 : 1;
}
