#include "bf6_core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool raw_contains(bf6_ctx* context, const char* partition, const char* token)
{
    const uint8_t* bytes = nullptr;
    const int64_t size = bf6_read_raw(
        context, BF6_RAW_EBX, partition, &bytes);
    if (size <= 0 || !bytes) return false;
    const size_t token_size = std::strlen(token);
    if (token_size > static_cast<size_t>(size)) return false;
    for (int64_t at = 0; at + static_cast<int64_t>(token_size) <= size; ++at)
        if (std::memcmp(bytes + at, token, token_size) == 0) return true;
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    char error[1024]{};
    bf6_ctx* context = bf6_open(argv[1], error, sizeof(error));
    if (!context || !bf6_mount_all(context, 1, error, sizeof(error))) {
        std::fprintf(stderr, "%s\n", error);
        if (context) bf6_close(context);
        return 1;
    }
    const char* candidates[] = {
        "game/glacierflow/flow_mainmenu/ui/assets/bootflowintrostatedbd",
        "game/glacierflow/flow_mainmenu/ui/assets/bootflowcompletedstatedbd",
        "game/glacierflow/flow_mainmenu/ui/assets/bootflowdbd",
        "game/glacierflow/flow_mainmenu/ui/assets/bootflowintrostatedbd__control__",
    };
    bool intro_movie_done = false;
    bool intro_menu_music = false;
    bool completed_movie_done = false;
    bool control_movie_done = false;
    for (int candidate = 0; candidate != 4; ++candidate) {
        char data_name[256]{};
        const int count = bf6_rime_dbd_fields(
            context, candidates[candidate], data_name, sizeof(data_name),
            nullptr, 0);
        std::vector<bf6_rime_dbd_field> fields(
            count > 0 ? static_cast<size_t>(count) : 0u);
        const int filled = count >= 0 ? bf6_rime_dbd_fields(
            context, candidates[candidate], data_name, sizeof(data_name),
            fields.data(), count) : count;
        std::printf("partition=%s data=%s count=%d\n",
                    candidates[candidate], data_name, filled);
        for (const auto& field : fields) {
            std::printf("  field=%s type=0x%016llX\n", field.name,
                        static_cast<unsigned long long>(field.type_signature));
            const bool match = std::string(field.name) == "MovieDone";
            if (candidate == 0) intro_movie_done |= match;
            if (candidate == 3) control_movie_done |= match;
        }
        const bool raw_movie_done = raw_contains(
            context, candidates[candidate], "MovieDone");
        const bool raw_menu_music = raw_contains(
            context, candidates[candidate], "StartMenuMusic");
        std::printf("  raw MovieDone=%d StartMenuMusic=%d\n",
                    raw_movie_done ? 1 : 0, raw_menu_music ? 1 : 0);
        if (candidate == 0) {
            intro_movie_done |= raw_movie_done;
            intro_menu_music |= raw_menu_music;
        }
        if (candidate == 1) completed_movie_done |= raw_movie_done;
        if (candidate == 3) control_movie_done |= raw_movie_done;
    }
    bf6_close(context);
    return intro_movie_done && intro_menu_music &&
        !completed_movie_done && !control_movie_done ? 0 : 1;
}
