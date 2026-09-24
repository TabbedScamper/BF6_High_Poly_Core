/* The shape of one large asset: instances per type, and every distinct string with the
 * field and type it sits in.
 *
 * Written for common/gameplay/soldier/soldiermotionmachine (28 MB): if the soldier's
 * named state fields - the ones the motion-machine graphs read by u16 id - are defined
 * anywhere in the install, their names show up here as strings, beside whatever carries
 * the id.
 *
 *   ebx_strings_probe <game_dir> <asset> [max_strings]
 */
#include "ebx.h"
#include "source.h"
#include "types.h"

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace bf6;

struct Seen { int count = 0; uint32_t field = 0; std::string type; };
static std::map<std::string, Seen> g_strings;

static void walk(const EbxValue& v, uint32_t field, const std::string& type, int depth)
{
    if (depth > 16) return;
    if (v.kind == EbxValue::Kind::Str && !v.s.empty()) {
        Seen& s = g_strings[v.s];
        if (s.count++ == 0) { s.field = field; s.type = type; }
        return;
    }
    if (v.kind == EbxValue::Kind::Struct) {
        for (const auto& f : v.fields) walk(f.second, f.first, type, depth + 1);
        return;
    }
    if (v.kind == EbxValue::Kind::Array)
        for (const auto& it : v.items) walk(it, field, type, depth + 1);
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: ebx_strings_probe <game_dir> <asset> [max]\n"); return 2; }
    const int max_out = argc > 3 ? std::atoi(argv[3]) : 400;
    Source src;
    std::string err;
    if (!src.open(argv[1], err)) { std::fprintf(stderr, "open: %s\n", err.c_str()); return 1; }
    if (!src.mount_level(std::string(), false, err)) { std::fprintf(stderr, "mount: %s\n", err.c_str()); return 1; }
    TypeDb types;
    bool typed = false;
    for (const std::string& cand : TypeDb::exe_candidates(src.game_dir())) {
        std::string te;
        if (!types.open(cand, te) || types.looks_encrypted()) continue;
        typed = true;
        break;
    }
    if (!typed) { std::fprintf(stderr, "no readable type schema\n"); return 1; }
    std::string asset = argv[2];
    std::vector<uint8_t> raw = src.get_ebx(asset + ".ebx", err);
    if (raw.empty()) raw = src.get_ebx(asset, err);
    if (raw.empty()) { std::fprintf(stderr, "missing %s: %s\n", asset.c_str(), err.c_str()); return 1; }
    Ebx ebx(types);
    ebx.set_guid_index(&src.armory_partition_index());
    if (!ebx.parse(std::move(raw), err)) { std::fprintf(stderr, "parse: %s\n", err.c_str()); return 1; }
    /* BF6_LIST_TYPE=<guid>: every instance of that type IN ORDER, with its ordinal among
     * that type, its name (0x0c59fa06) and every scalar field - to read which field a
     * motion-machine descriptor id names, and what (if anything) records the id. */
    /* BF6_INSTANCES=i,j,...: those instances whole - type and every field, nested - to see
     * what an instance that only showed up as a number (a name hash) actually is. */
    if (const char* want = std::getenv("BF6_INSTANCES")) {
        std::function<void(const EbxValue&, const std::string&, int)> dump =
            [&](const EbxValue& v, const std::string& path, int depth) {
            if (depth > 8) return;
            switch (v.kind) {
            case EbxValue::Kind::Struct:
                for (const auto& f : v.fields) {
                    char h[16]; std::snprintf(h, sizeof h, "%08x", f.first);
                    dump(f.second, path + "." + h, depth + 1);
                }
                return;
            case EbxValue::Kind::Array:
                for (size_t k = 0; k < v.items.size() && k < 64; ++k)
                    dump(v.items[k], path + "[" + std::to_string(k) + "]", depth + 1);
                return;
            case EbxValue::Kind::Str:  std::printf("  %-60s \"%s\"\n", path.c_str(), v.s.c_str()); return;
            case EbxValue::Kind::Bool: std::printf("  %-60s %d\n", path.c_str(), v.b ? 1 : 0); return;
            case EbxValue::Kind::Int:  std::printf("  %-60s %lld\n", path.c_str(), (long long)v.i); return;
            case EbxValue::Kind::Uint: std::printf("  %-60s %llu (0x%llx)\n", path.c_str(),
                                                   (unsigned long long)v.u, (unsigned long long)v.u); return;
            case EbxValue::Kind::Real: std::printf("  %-60s %g\n", path.c_str(), v.f); return;
            case EbxValue::Kind::InstanceRef:
                std::printf("  %-60s -> inst %d%s\n", path.c_str(), v.instance,
                            v.instance >= 0 ? (" type " + TypeDb::guid_str(ebx.instance_type((size_t)v.instance))).c_str() : "");
                return;
            case EbxValue::Kind::ImportRef:
                std::printf("  %-60s -> import %s %s\n", path.c_str(), v.import_path.c_str(), v.s.c_str());
                return;
            default: std::printf("  %-60s <kind %d>\n", path.c_str(), (int)v.kind); return;
            }
        };
        for (const char* p = want; *p;) {
            const size_t i = (size_t)std::strtoul(p, nullptr, 10);
            if (i < ebx.instance_count()) {
                std::printf("== inst %zu type %s\n", i, TypeDb::guid_str(ebx.instance_type(i)).c_str());
                dump(ebx.read_instance(i), "", 0);
            }
            while (*p && *p != ',') ++p;
            if (*p == ',') ++p;
        }
        return 0;
    }
    if (const char* want = std::getenv("BF6_LIST_TYPE")) {
        int ord = 0;
        for (size_t i = 0; i < ebx.instance_count(); ++i) {
            if (TypeDb::guid_str(ebx.instance_type(i)) != want) continue;
            const EbxValue v = ebx.read_instance(i);
            std::string name, rest;
            for (const auto& f : v.fields) {
                char t[64];
                if (f.first == 0x0c59fa06u && f.second.kind == EbxValue::Kind::Str) { name = f.second.s; continue; }
                switch (f.second.kind) {
                case EbxValue::Kind::Bool: std::snprintf(t, sizeof t, " %08x=%d", f.first, f.second.b ? 1 : 0); break;
                case EbxValue::Kind::Int:  std::snprintf(t, sizeof t, " %08x=%lld", f.first, (long long)f.second.i); break;
                case EbxValue::Kind::Uint: std::snprintf(t, sizeof t, " %08x=%llu", f.first, (unsigned long long)f.second.u); break;
                case EbxValue::Kind::Real: std::snprintf(t, sizeof t, " %08x=%g", f.first, f.second.f); break;
                default: continue;
                }
                rest += t;
            }
            std::printf("%4d inst %5zu %-50s%s\n", ord++, i, name.c_str(), rest.c_str());
        }
        return 0;
    }
    std::map<std::string, int> by_type;
    for (size_t i = 0; i < ebx.instance_count(); ++i) {
        const std::string t = TypeDb::guid_str(ebx.instance_type(i));
        ++by_type[t];
        walk(ebx.read_instance(i), 0, t, 0);
    }
    std::printf("== %s: %zu instances, %zu types, %zu distinct strings\n", asset.c_str(),
                ebx.instance_count(), by_type.size(), g_strings.size());
    std::vector<std::pair<int, std::string>> tv;
    for (const auto& kv : by_type) tv.push_back({kv.second, kv.first});
    std::sort(tv.rbegin(), tv.rend());
    for (size_t i = 0; i < tv.size() && i < 30; ++i) std::printf("  type %s x%d\n", tv[i].second.c_str(), tv[i].first);
    int n = 0;
    for (const auto& kv : g_strings) {
        if (n++ >= max_out) break;
        std::printf("  str %-60s x%-5d field %08x type %s\n", kv.first.c_str(), kv.second.count,
                    kv.second.field, kv.second.type.c_str());
    }
    return 0;
}
