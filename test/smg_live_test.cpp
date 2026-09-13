#include "source.h"
#include "types.h"
#include "ebx.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {
constexpr uint32_t Members = 0xA43BD992u;
constexpr uint32_t Transforms = 0xEBF4D386u;
constexpr uint32_t MemberType = 0xB608BEEEu;
constexpr uint32_t MeshAsset = 0x1B4A547Cu;

const char* kind(const bf6::EbxValue* value)
{
    if (!value) return "missing";
    switch (value->kind)
    {
    case bf6::EbxValue::Kind::Struct: return "struct";
    case bf6::EbxValue::Kind::Array: return "array";
    case bf6::EbxValue::Kind::ImportRef: return "import";
    case bf6::EbxValue::Kind::InstanceRef: return "instance";
    default: return "other";
    }
}
}

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::fprintf(stderr, "usage: smg_live_test <game> <exe> <ebx>\n");
        return 2;
    }
    std::string error;
    bf6::Source source;
    if (!source.open(argv[1], error) || !source.mount_frontend(error))
    {
        std::fprintf(stderr, "mount: %s\n", error.c_str());
        return 1;
    }
    bf6::TypeDb types;
    if (!types.open(argv[2], error))
    {
        std::fprintf(stderr, "types: %s\n", error.c_str());
        return 1;
    }
    std::vector<uint8_t> bytes = source.get_ebx(argv[3], error);
    bf6::Ebx ebx(types);
    ebx.set_guid_index(&source.armory_partition_index());
    if (bytes.empty() || !ebx.parse(std::move(bytes), error))
    {
        std::fprintf(stderr, "ebx: %s\n", error.c_str());
        return 1;
    }
    const std::vector<uint32_t> wanted{ Members };
    for (size_t i = 0; i < ebx.instance_count(); ++i)
    {
        const bf6::EbxValue root = ebx.read_instance(i, &wanted);
        const bf6::EbxValue* members = root.field(Members);
        if (!members || members->kind != bf6::EbxValue::Kind::Array) continue;
        std::printf("root=%zu members=%zu root_type=%s\n", i, members->items.size(),
                    bf6::TypeDb::guid_str(root.guid).c_str());
        size_t total = 0, linear = 0;
        for (size_t m = 0; m < members->items.size(); ++m)
        {
            const bf6::EbxValue& member = members->items[m];
            const bf6::EbxValue* xf = member.field(Transforms);
            const bf6::EbxValue* bp = member.field(MemberType);
            const bf6::EbxValue* mesh = member.field(MeshAsset);
            const size_t count = xf && xf->kind == bf6::EbxValue::Kind::Array
                ? xf->items.size() : 0;
            total += count;
            if (xf && xf->kind == bf6::EbxValue::Kind::Array)
                for (const bf6::EbxValue& value : xf->items)
                    if (bf6::TypeDb::guid_str(value.guid) ==
                        "06ce1d10-9a4e-fc64-2a9f-a9d482576ffa") ++linear;
            if (m < 8)
                std::printf("member=%zu type=%s transforms=%zu first_type=%s "
                            "member_type=%s:%s mesh=%s:%s\n", m,
                    bf6::TypeDb::guid_str(member.guid).c_str(), count,
                    count ? bf6::TypeDb::guid_str(xf->items[0].guid).c_str() : "-",
                    kind(bp), bp ? bp->import_path.c_str() : "",
                    kind(mesh), mesh ? mesh->import_path.c_str() : "");
        }
        std::printf("total_transforms=%zu linear_transforms=%zu\n", total, linear);
        return total > 0 && total == linear ? 0 : 1;
    }
    std::fprintf(stderr, "no StaticModelGroup member array\n");
    return 1;
}
