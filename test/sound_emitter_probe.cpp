/* WHAT A PLACED SPATIAL SOUND EMITTER ACTUALLY HOLDS.
 *
 *   sound_emitter_probe <game_dir> <level> [type_guid_prefix]
 *
 * Neither editor reads a single sound emitter, and audio is the largest absent
 * dimension in the census: 230,299 instances fleet-wide, 7 percent of every
 * instance in every level. DiceSoundSpatialEntityData is the entry point -
 * named, so the schema reads it, and present on 25 of 28 levels.
 *
 * Before any ABI is designed, this asks the data what the entity carries: every
 * field of every instance, by name where the repo's field dictionary knows it,
 * with the value. A reader designed from the SDK type dump alone would miss
 * what inheritance contributes, which is where the transform lives.
 */
#include "bf6_core.h"

#include "ebx.h"
#include "source.h"
#include "types.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace bf6;

namespace {

const char* kind_name(EbxValue::Kind k)
{
    switch (k) {
    case EbxValue::Kind::Null: return "null";
    case EbxValue::Kind::Bool: return "bool";
    case EbxValue::Kind::Int: return "int";
    case EbxValue::Kind::Uint: return "uint";
    case EbxValue::Kind::Real: return "real";
    case EbxValue::Kind::Str: return "str";
    case EbxValue::Kind::Guid: return "guid";
    case EbxValue::Kind::ResRef: return "resref";
    case EbxValue::Kind::Struct: return "struct";
    case EbxValue::Kind::Array: return "array";
    case EbxValue::Kind::InstanceRef: return "iref";
    case EbxValue::Kind::ImportRef: return "import";
    default: return "unknown";
    }
}

std::string brief(const EbxValue& v)
{
    char b[256];
    switch (v.kind) {
    case EbxValue::Kind::Bool: std::snprintf(b, sizeof(b), "%s", v.b ? "true" : "false"); break;
    case EbxValue::Kind::Int:  std::snprintf(b, sizeof(b), "%lld", (long long)v.i); break;
    case EbxValue::Kind::Uint: std::snprintf(b, sizeof(b), "%llu", (unsigned long long)v.u); break;
    case EbxValue::Kind::Real: std::snprintf(b, sizeof(b), "%.4f", v.f); break;
    case EbxValue::Kind::Str:  std::snprintf(b, sizeof(b), "\"%s\"", v.s.c_str()); break;
    case EbxValue::Kind::Array: std::snprintf(b, sizeof(b), "[%zu]", v.items.size()); break;
    case EbxValue::Kind::Struct: std::snprintf(b, sizeof(b), "{%zu field(s)}", v.fields.size()); break;
    case EbxValue::Kind::ImportRef:
        std::snprintf(b, sizeof(b), "-> %s", v.import_path.empty() ? "(unresolved)" : v.import_path.c_str());
        break;
    case EbxValue::Kind::InstanceRef: std::snprintf(b, sizeof(b), "#%d", v.instance); break;
    default: std::snprintf(b, sizeof(b), "-"); break;
    }
    return b;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: sound_emitter_probe <game> <level> [guid_prefix]\n"); return 2; }
    const std::string level = argv[2];
    const std::string want = (argc > 3) ? argv[3] : "4450a080";   /* DiceSoundSpatialEntityData */

    Source src;
    std::string err;
    if (!src.open(argv[1], err)) { std::printf("open: %s\n", err.c_str()); return 1; }
    if (!src.mount_level(level, false, err)) { std::printf("mount: %s\n", err.c_str()); return 1; }
    TypeDb types;
    bool got = false;
    for (const std::string& cand : TypeDb::exe_candidates(src.game_dir()))
        if (types.open(cand, err) && !types.looks_encrypted()) { got = true; break; }
    if (!got) { std::printf("types: %s\n", err.c_str()); return 1; }

    size_t found = 0, partitions = 0, shown = 0;
    std::map<uint32_t, size_t> field_freq;
    std::map<uint32_t, std::string> field_kind;
    std::set<std::string> sound_paths;

    for (const auto& kv : src.ebx()) {
        if (Source::level_dir_end(kv.first, level) == std::string::npos) continue;
        std::string e;
        std::vector<uint8_t> raw = src.get_ebx(kv.first, e);
        if (raw.empty()) continue;
        Ebx ebx(types);
        if (!ebx.parse(std::move(raw), e)) continue;
        ebx.set_guid_index(&src.partition_index());
        partitions++;
        for (size_t i = 0; i < ebx.instance_count(); ++i) {
            const std::string tg = TypeDb::guid_str(ebx.instance_type(i));
            if (tg.compare(0, want.size(), want) != 0) continue;
            found++;
            const EbxValue v = ebx.read_instance(i);
            for (const auto& f : v.fields) {
                field_freq[f.first]++;
                field_kind[f.first] = kind_name(f.second.kind);
                if (f.second.kind == EbxValue::Kind::ImportRef && !f.second.import_path.empty())
                    sound_paths.insert(f.second.import_path);
            }
            if (shown < 3) {
                std::printf("\n--- instance %zu in %s\n", i, kv.first.c_str());
                for (const auto& f : v.fields)
                    std::printf("    %08x  %-8s %s\n", f.first, kind_name(f.second.kind),
                                brief(f.second).c_str());
                shown++;
            }
        }
    }

    std::printf("\n%s: %zu instance(s) of type %s* across %zu partition(s)\n",
                level.c_str(), found, want.c_str(), partitions);
    if (!found) { std::printf("none found\n"); return 1; }

    std::printf("\nFIELDS, by how many instances carry them (a field every instance has is\n"
                "structural; one only some carry is optional and needs a default):\n");
    std::printf("    %-10s %-8s %8s  %s\n", "hash", "kind", "instances", "share");
    for (const auto& kv : field_freq)
        std::printf("    %08x   %-8s %8zu  %5.1f%%\n", kv.first, field_kind[kv.first].c_str(),
                    kv.second, 100.0 * (double)kv.second / (double)found);

    std::printf("\nDISTINCT SOUND ASSETS referenced: %zu\n", sound_paths.size());
    size_t n = 0;
    for (const std::string& s : sound_paths) {
        std::printf("    %s\n", s.c_str());
        if (++n >= 12) break;
    }

    /* CAN WE ACTUALLY PLAY THEM? The emitter names a sound CONFIG, and
     * bf6_ui_sound_decode already takes either a config or a wave resource, so
     * the chain from a placed emitter to PCM may already be complete. Asking
     * here decides whether a preview can be audible or only show markers - a
     * difference worth knowing before designing either.
     *
     * The import path carries ".ebx"; every name the mount accepts is without
     * one, which is the same trap the reflection textures had. */
    /* WHAT THE SOUND CONFIG ITSELF CARRIES. The emitter has no radius, so
     * either the config does or a consumer has to invent one - and inventing a
     * falloff is how a preview stops matching the game. Asking before deciding. */
    if (!sound_paths.empty()) {
        std::string cfg = *sound_paths.begin();
        if (cfg.size() > 4 && cfg.compare(cfg.size() - 4, 4, ".ebx") == 0) cfg.resize(cfg.size() - 4);
        std::printf("\nTHE SOUND CONFIG'S OWN FIELDS: %s\n", cfg.c_str());
        std::string ce;
        std::vector<uint8_t> craw = src.get_ebx(cfg, ce);
        if (craw.empty()) std::printf("    could not read it: %s\n", ce.c_str());
        else {
            Ebx cebx(types);
            if (!cebx.parse(std::move(craw), ce)) std::printf("    parse: %s\n", ce.c_str());
            else {
                cebx.set_guid_index(&src.partition_index());
                std::printf("    %zu instance(s)\n", cebx.instance_count());
                for (size_t i = 0; i < cebx.instance_count() && i < 4; ++i) {
                    const EbxValue cv = cebx.read_instance(i);
                    std::printf("    -- instance %zu (%s)\n", i,
                                TypeDb::guid_str(cebx.instance_type(i)).c_str());
                    for (const auto& f : cv.fields)
                        std::printf("        %08x  %-8s %s\n", f.first,
                                    kind_name(f.second.kind), brief(f.second).c_str());
                }
            }
        }
    }

    std::printf("\nDECODE CHECK (emitter -> PCM), the whole point of reading these:\n");
    char derr[512] = {0};
    bf6_ctx* ctx = bf6_open(argv[1], derr, (int)sizeof(derr));
    if (!ctx) { std::printf("    could not open a context: %s\n", derr); return 0; }
    /* The sounds live under common/, not in the level, so the level's own mount
     * is not enough: without this every decode returns -1 and reads as "the
     * sounds cannot be decoded" when it only means "nothing was mounted". */
    if (!bf6_mount_all(ctx, 1, derr, (int)sizeof(derr)))
        std::printf("    mount_all: %s\n", derr);
    size_t tried = 0, decoded = 0;
    for (const std::string& s : sound_paths) {
        std::string p = s;
        if (p.size() > 4 && p.compare(p.size() - 4, 4, ".ebx") == 0) p.resize(p.size() - 4);
        int ch = 0, rate = 0;
        const int samples = bf6_ui_sound_decode(ctx, p.c_str(), 0, nullptr, 0, &ch, &rate);
        tried++;
        if (samples > 0 && ch > 0 && rate > 0) {
            decoded++;
            std::printf("    ok    %7d samples, %d ch, %d Hz  %s\n", samples, ch, rate,
                        p.substr(p.rfind('/') + 1).c_str());
        } else {
            std::printf("    FAIL  (%d)  %s\n", samples, p.substr(p.rfind('/') + 1).c_str());
        }
        if (tried >= 8) break;
    }
    std::printf("    %zu of %zu decoded\n", decoded, tried);
    bf6_close(ctx);
    return 0;
}
