/* Live weapon-package identity probe.
 *
 * Several package PointerRef fields deserialize as Null through the current
 * executable schema.  This diagnostic asks Ebx::import_ref for every reflected
 * top-level field and prints only genuine EFIX imports.  It reads the mounted
 * install directly and is deliberately not a runtime lookup table.
 */
#include "ebx.h"
#include "source.h"
#include "types.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace bf6;

static bool open_partition(Source& src, TypeDb& types, const std::string& name,
                           Ebx& ebx, std::string& err)
{
    std::vector<uint8_t> raw = src.get_ebx(name + ".ebx", err);
    if (raw.empty()) raw = src.get_ebx(name, err);
    if (raw.empty()) return false;
    ebx.set_guid_index(&src.armory_partition_index());
    return ebx.parse(std::move(raw), err);
}

static void imports_for_instance(Ebx& ebx, size_t instance)
{
    const EbxValue row = ebx.read_instance(instance);
    for (const auto& field : row.fields)
    {
        std::string partition, path;
        if (ebx.import_ref(instance, field.first, partition, path))
            std::printf("  field=%08x import=%s partition=%s\n", field.first,
                        path.c_str(), partition.c_str());
    }
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::printf("usage: package_identity_probe <game_dir>\n");
        return 2;
    }

    Source src;
    std::string err;
    if (!src.open(argv[1], err) || !src.mount_frontend(err))
    {
        std::printf("mount failed: %s\n", err.c_str());
        return 1;
    }
    TypeDb types;
    if (!types.open(std::string(argv[1]) + "/bf6.exe", err))
    {
        std::printf("types failed: %s\n", err.c_str());
        return 1;
    }

    Ebx packages(types);
    const std::string pkg = "common/hardware/weapons/carbine/m4a1/pkg_m4a1";
    if (!open_partition(src, types, pkg, packages, err))
    {
        std::printf("package parse failed: %s\n", err.c_str());
        return 1;
    }
    const uint32_t kItems = 0x2c95d7b7u;
    const uint32_t kDebug = 0x55aded8du;
    const EbxValue root = packages.read_instance(0);
    const EbxValue* items = root.field(kItems);
    if (!items || items->kind != EbxValue::Kind::Array) return 1;
    for (size_t ordinal = 0; ordinal < items->items.size(); ++ordinal)
    {
        const EbxValue& ref = items->items[ordinal];
        if (ref.kind != EbxValue::Kind::InstanceRef || ref.instance < 0) continue;
        const EbxValue row = packages.read_instance((size_t)ref.instance);
        const EbxValue* debug = row.field(kDebug);
        std::printf("ui ordinal=%zu instance=%d key=%s\n", ordinal, ref.instance,
                    debug && debug->kind == EbxValue::Kind::Str ? debug->s.c_str() : "");
        for (const auto& field : row.fields)
        {
            const EbxValue& value = field.second;
            if (value.kind == EbxValue::Kind::Int)
                std::printf("  value field=%08x int=%lld\n", field.first,
                            (long long)value.i);
            else if (value.kind == EbxValue::Kind::Uint)
                std::printf("  value field=%08x uint=%llu\n", field.first,
                            (unsigned long long)value.u);
            else if (value.kind == EbxValue::Kind::Str && !value.s.empty())
                std::printf("  value field=%08x str=%s\n", field.first,
                            value.s.c_str());
        }
        imports_for_instance(packages, (size_t)ref.instance);
    }

    Ebx equipment(types);
    const std::string eq = "common/hardware/weapons/carbine/m4a1/equipment_m4a1";
    if (!open_partition(src, types, eq, equipment, err))
    {
        std::printf("equipment parse failed: %s\n", err.c_str());
        return 1;
    }
    const uint32_t kPackageList = 0x6780584du;
    const EbxValue eqRoot = equipment.read_instance(0);
    const EbxValue* configurations = eqRoot.field(kPackageList);
    std::printf("equipment package rows=%zu\n",
                configurations && configurations->kind == EbxValue::Kind::Array
                    ? configurations->items.size() : 0u);
    if (configurations && configurations->kind == EbxValue::Kind::Array)
        for (size_t i = 0; i < configurations->items.size(); ++i)
        {
            std::printf("equipment ordinal=%zu\n", i);
            /* Package rows are inline structs. Their reflected fields can be
             * printed, while raw PointerRefs inside an inline struct cannot be
             * addressed by instance index; the UI-side imports above are the
             * identity test this probe is intended to settle. */
            const EbxValue& row = configurations->items[i];
            for (const auto& field : row.fields)
                if (field.second.kind == EbxValue::Kind::ImportRef)
                    std::printf("  field=%08x import=%s\n", field.first,
                                field.second.import_path.c_str());
                else if (field.second.kind == EbxValue::Kind::Int)
                    std::printf("  field=%08x int=%lld\n", field.first,
                                (long long)field.second.i);
                else if (field.second.kind == EbxValue::Kind::Uint)
                    std::printf("  field=%08x uint=%llu\n", field.first,
                                (unsigned long long)field.second.u);
                else if (field.second.kind == EbxValue::Kind::Array)
                {
                    std::printf("  field=%08x array=%zu", field.first,
                                field.second.items.size());
                    for (const EbxValue& item : field.second.items)
                    {
                        const EbxValue* id = item.field(0xc1ad37dau);
                        if (id && id->kind == EbxValue::Kind::Uint)
                            std::printf(" %llu", (unsigned long long)id->u);
                        else if (id && id->kind == EbxValue::Kind::Int)
                            std::printf(" %lld", (long long)id->i);
                    }
                    std::printf("\n");
                }
        }

    const auto& index = src.armory_partition_index();
    const std::vector<uint8_t>& eqRaw = equipment.raw();
    std::printf("equipment raw package pointers\n");
    for (size_t importIndex = 0; importIndex < equipment.import_count(); ++importIndex)
    {
        const Ebx::Import& import = equipment.import_at(importIndex);
        const auto resolved = index.find(import.partition);
        const std::string path = resolved == index.end() ? std::string() : resolved->second;
        if (path.find("/m4a1/u_pkg_m4a1_") == std::string::npos &&
            path.find("/m4a1/u_m4a1_pkg_factory") == std::string::npos) continue;
        const uint32_t encoded = (uint32_t)((importIndex << 1u) | 1u);
        std::printf("  import_index=%zu encoded=%08x path=%s positions=", importIndex,
                    encoded, path.c_str());
        for (size_t pos = (size_t)equipment.payload(); pos + 4 <= eqRaw.size(); ++pos)
        {
            const uint32_t value = (uint32_t)eqRaw[pos] |
                ((uint32_t)eqRaw[pos + 1] << 8u) |
                ((uint32_t)eqRaw[pos + 2] << 16u) |
                ((uint32_t)eqRaw[pos + 3] << 24u);
            if (value == encoded)
                std::printf("%zu(rel=%zu) ", pos, pos - (size_t)equipment.payload());
        }
        std::printf("\n");
    }

    std::printf("unlock partitions\n");
    for (const auto& entry : src.ebx())
    {
        const std::string& path = entry.first;
        if (path.find("/m4a1/u_pkg_m4a1_") == std::string::npos &&
            path.find("/m4a1/u_m4a1_pkg_factory") == std::string::npos) continue;
        Ebx unlock(types);
        if (!open_partition(src, types, path, unlock, err)) continue;
        std::printf("unlock=%s partition=%s instances=%zu instance_guid=%s\n",
                    path.c_str(), unlock.partition_guid().c_str(), unlock.instance_count(),
                    unlock.instance_count() ? unlock.instance_guid(0).c_str() : "");
        for (size_t instance = 0; instance < unlock.instance_count(); ++instance)
        {
            const EbxValue row = unlock.read_instance(instance);
            for (const auto& field : row.fields)
            {
                if (field.second.kind == EbxValue::Kind::Int)
                    std::printf("  i=%zu field=%08x int=%lld\n", instance, field.first,
                                (long long)field.second.i);
                else if (field.second.kind == EbxValue::Kind::Uint)
                    std::printf("  i=%zu field=%08x uint=%llu\n", instance, field.first,
                                (unsigned long long)field.second.u);
                else if (field.second.kind == EbxValue::Kind::Str && !field.second.s.empty())
                    std::printf("  i=%zu field=%08x str=%s\n", instance, field.first,
                                field.second.s.c_str());
            }
        }
    }
    return 0;
}
