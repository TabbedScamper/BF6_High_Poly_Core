#include "bf6_core.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char* kHomeBackground =
    "common/ui/home/screens/home_screen_bg";
constexpr const char* kImageFader =
    "common/ui/home/widgets/bulletin/bulletinimagefader";
constexpr const char* kImageDbd =
    "common/ui/missionbriefinghome/data/missionbriefingimagedbd";
constexpr const char* kMainMenuLogic =
    "common/ui/universalmenu/logic/mainmenu_universalmenu_logic";
constexpr const char* kFallbackTexture =
    "common/ui/home/assets/temp/t_ui_bulletin_01";
constexpr const char* kFake = "common/ui/__control__/not_a_real_background";

bool has_edge(const std::vector<bf6_rime_connection>& edges, int source,
              uint32_t source_field, int target, uint32_t target_field,
              int mode)
{
    return std::any_of(edges.begin(), edges.end(), [&](const auto& edge) {
        return edge.source == source && edge.source_field == source_field &&
               edge.target == target && edge.target_field == target_field &&
               edge.mode == mode;
    });
}

std::vector<bf6_rime_connection> connections(bf6_ctx* context,
                                              const char* partition)
{
    const int count = bf6_rime_connections(context, partition, nullptr, 0);
    std::vector<bf6_rime_connection> result(
        static_cast<size_t>((std::max)(count, 0)));
    if (count > 0)
        bf6_rime_connections(context, partition, result.data(), count);
    return result;
}

void dump_partition(bf6_ctx* context, const char* partition)
{
    const int node_count = bf6_rime_tree(context, partition, 6, nullptr, 0,
                                          nullptr);
    std::vector<bf6_rime_node> nodes(
        static_cast<size_t>((std::max)(node_count, 0)));
    if (node_count > 0)
        bf6_rime_tree(context, partition, 6, nodes.data(), node_count, nullptr);

    const int field_count =
        bf6_rime_interface_fields(context, partition, nullptr, 0);
    std::vector<bf6_rime_interface_field> fields(
        static_cast<size_t>((std::max)(field_count, 0)));
    if (field_count > 0)
        bf6_rime_interface_fields(context, partition, fields.data(), field_count);

    const int struct_count =
        bf6_rime_interface_struct_types(context, partition, nullptr, 0);
    std::vector<bf6_rime_interface_struct_type> structs(
        static_cast<size_t>((std::max)(struct_count, 0)));
    if (struct_count > 0)
        bf6_rime_interface_struct_types(context, partition, structs.data(),
                                        struct_count);

    const int edge_count = bf6_rime_connections(context, partition, nullptr, 0);
    std::vector<bf6_rime_connection> edges(
        static_cast<size_t>((std::max)(edge_count, 0)));
    if (edge_count > 0)
        bf6_rime_connections(context, partition, edges.data(), edge_count);

    std::printf("partition=%s nodes=%d interface-fields=%d struct-defaults=%d "
                "edges=%d\n", partition, node_count, field_count, struct_count,
                edge_count);
    for (const bf6_rime_node& node : nodes) {
        if (node.kind == BF6_RIME_TEXTURE ||
            node.kind == BF6_RIME_WIDGET_REFERENCE)
            std::printf("  node instance=%d kind=%d name=%s type=%s "
                        "reference=%s image=%s\n",
                        node.instance, node.kind, node.name, node.type_name,
                        node.reference, node.image_asset);
    }
    for (const bf6_rime_interface_field& field : fields)
        std::printf("  interface instance=%d field=0x%08X access=%d kind=%d\n",
                    field.interface_instance, field.field_id,
                    field.access_type, field.value_kind);
    for (const bf6_rime_interface_struct_type& item : structs)
        std::printf("  interface-struct instance=%d field=0x%08X type=%s\n",
                    item.interface_instance, item.field_id, item.type_guid);
    for (const bf6_rime_connection& edge : edges)
        std::printf("  edge %d.0x%08X -> %d.0x%08X mode=%d\n",
                    edge.source, edge.source_field, edge.target,
                    edge.target_field, edge.mode);
}

} // namespace

int main(int argc, char** argv)
{
    const char* game = argc > 1 ? argv[1]
        : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Battlefield 6";
    char error[512]{};
    bf6_ctx* context = bf6_open(game, error, static_cast<int>(sizeof(error)));
    if (!context) {
        std::fprintf(stderr, "open: %s\n", error);
        return 1;
    }
    if (!bf6_mount_frontend(context, error, static_cast<int>(sizeof(error)))) {
        std::fprintf(stderr, "mount: %s\n", error);
        bf6_close(context);
        return 1;
    }

    dump_partition(context, kHomeBackground);
    dump_partition(context, kImageFader);
    dump_partition(context, kMainMenuLogic);

    std::vector<char> dbd_dump(1 << 20);
    const int64_t dbd_bytes = bf6_ebx_dump(
        context, kImageDbd, 8, dbd_dump.data(),
        static_cast<int>(dbd_dump.size()));
    const std::string dbd_text = dbd_bytes >= 0 ? dbd_dump.data() : "";
    const bool dbd_contract =
        dbd_text.find("\"MissionBriefingImageDBD\"") != std::string::npos &&
        dbd_text.find("\"UpdatePreviousImage\"") != std::string::npos &&
        dbd_text.find("\"PreviousImage\"") != std::string::npos;
    std::printf("image-dbd bytes=%lld contract=%d\n",
                static_cast<long long>(dbd_bytes), dbd_contract ? 1 : 0);

    const auto fader_edges = connections(context, kImageFader);
    const auto menu_edges = connections(context, kMainMenuLogic);
    const bool fader_contract =
        has_edge(fader_edges, 25, 0x97E01A76u, 14, 0x97E01A76u, 2) &&
        has_edge(fader_edges, 25, 0x2A6DE7C7u, 11, 0xBDD7E0DAu, 2) &&
        has_edge(fader_edges, 13, 0xAC996E7Au, 9, 0xBDD7E0DAu, 2);
    const bool menu_contract =
        has_edge(menu_edges, 1, 0xDEE9DD63u, 54, 0xD1F16E9Cu, 18) &&
        has_edge(menu_edges, 1, 0xC2601279u, 54, 0xC2601279u, 18) &&
        has_edge(menu_edges, 1, 0x77C9D6EFu, 54, 0xECC4DA8Fu, 18);
    const bool shuffled_control =
        has_edge(menu_edges, 1, 0xDEE9DD63u, 54, 0xC2601279u, 18) ||
        has_edge(fader_edges, 25, 0x97E01A76u, 11, 0xBDD7E0DAu, 2);

    const uint8_t* fallback_raw = nullptr;
    const uint8_t* fake_raw = nullptr;
    const int64_t fallback_bytes = bf6_read_raw(
        context, BF6_RAW_EBX, kFallbackTexture, &fallback_raw);
    const int64_t fake_raw_bytes =
        bf6_read_raw(context, BF6_RAW_EBX, kFake, &fake_raw);
    const int fallback_id = bf6_texture_id_by_name(context, kFallbackTexture);
    const bf6_texture* fallback = fallback_id >= 0
        ? bf6_texture_at(context, fallback_id) : nullptr;
    std::printf("routes fader=%d menu=%d shuffled=%d fallback-ebx=%lld "
                "fallback-texture=%d %dx%d fake-ebx=%lld\n",
                fader_contract ? 1 : 0, menu_contract ? 1 : 0,
                shuffled_control ? 1 : 0,
                static_cast<long long>(fallback_bytes), fallback_id,
                fallback ? fallback->width : 0, fallback ? fallback->height : 0,
                static_cast<long long>(fake_raw_bytes));

    const int fake_tree =
        bf6_rime_tree(context, kFake, 6, nullptr, 0, nullptr);
    const int fake_interface =
        bf6_rime_interface_fields(context, kFake, nullptr, 0);
    std::vector<char> fake_dump(256);
    const int64_t fake_dbd = bf6_ebx_dump(
        context, kFake, 8, fake_dump.data(), static_cast<int>(fake_dump.size()));
    std::printf("controls fake-tree=%d fake-interface=%d fake-dbd=%lld\n",
                fake_tree, fake_interface, static_cast<long long>(fake_dbd));

    const bool pass = dbd_contract && fader_contract && menu_contract &&
                      !shuffled_control && fallback_bytes > 0 && fallback &&
                      fake_raw_bytes < 0 && fake_tree < 0 &&
                      fake_interface < 0 && fake_dbd < 0;
    bf6_close(context);
    return pass ? 0 : 1;
}
