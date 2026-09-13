#include "source.h"
#include "types.h"
#include "ebx.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr,
            "usage: ebx_array_descriptor_probe <game> <exe> <ebx>\n");
        return 2;
    }
    std::string error;
    bf6::Source source;
    if (!source.open(argv[1], error) ||
        !source.mount_level(std::string(), true, error))
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
    if (bytes.empty() || !ebx.parse(std::move(bytes), error))
    {
        std::fprintf(stderr, "ebx: %s\n", error.c_str());
        return 1;
    }
    std::printf("payload=%lld bytes=%zu instances=%zu arrays=%zu\n",
        (long long)ebx.payload(), ebx.raw().size(), ebx.instance_count(),
        ebx.opaque_array_descriptors().size());
    for (size_t i = 0; i < ebx.instance_count(); ++i)
    {
        const bf6::TypeLayout& layout = types.layout_full(ebx.instance_type(i));
        std::printf("I %zu rel=%u type=%08x size=%u fields=%zu\n", i,
            ebx.instance_offset(i), layout.name_hash, layout.size,
            layout.fields.size());
        for (const bf6::FieldInfo& field : layout.fields)
            if (field.name_hash == 0x6EAF60BDu ||
                field.name_hash == 0x9B5FAFC2u ||
                field.name_hash == 0xADE23047u ||
                field.name_hash == 0x918B083Du ||
                field.name_hash == 0xF71B7FD3u)
                std::printf("  F %08x off=%u flags=%04x\n",
                    field.name_hash, field.offset, field.flags);
    }
    for (const bf6::Ebx::OpaqueArrayDescriptor& array :
         ebx.opaque_array_descriptors())
        std::printf("A off=%u count=%u hash=%08x flags=%04x class=%u\n",
            array.offset, array.count, array.hash, array.flags,
            array.class_ref);
    for (size_t i = 0; i < ebx.instance_count(); ++i)
        for (int offset = 0x20; offset <= 0x68; offset += 8)
        {
            std::vector<uint8_t> raw_array;
            bf6::Ebx::OpaqueArrayDescriptor descriptor;
            if (ebx.opaque_array_at(ebx.payload() + ebx.instance_offset(i) + offset,
                                    1, raw_array, &descriptor))
                std::printf("R I=%zu +%02x -> off=%u count=%u bytes=%zu\n",
                    i, offset, descriptor.offset, descriptor.count,
                    raw_array.size());
        }
    return 0;
}
