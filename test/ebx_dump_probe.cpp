/* Dump EBX partitions through the reflection, for exploring authored bindings.
 *
 *   ebx_dump_probe <depth> <name-substring> [more substrings...]
 *
 * Every EBX whose name contains a substring is dumped. Mounts everything first
 * (a partial mount makes a folder look empty). */
#include "bf6_core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: ebx_dump_probe <depth> <substr>...\n");
        return 1;
    }
    /* ebx_dump_probe bones <rig substr> [decimal or 0x hashes...]: list every bone
     * of each matching skeleton with its FNV-1 hash, marking ones in the list. */
    const bool bones_mode = std::strcmp(argv[1], "bones") == 0;
    std::vector<uint32_t> want;
    if (bones_mode)
        for (int a = 3; a < argc; ++a) want.push_back((uint32_t)std::strtoul(argv[a], nullptr, 0));
    const int depth = bones_mode ? 0 : std::atoi(argv[1]);
    const char* game = std::getenv("BF6_GAME") ? std::getenv("BF6_GAME")
        : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Battlefield 6";
    char err[1024] = {};
    bf6_ctx* ctx = bf6_open(game, err, (int)sizeof(err));
    if (!ctx) { std::fprintf(stderr, "open: %s\n", err); return 2; }
    if (!bf6_mount_all(ctx, 1, err, (int)sizeof(err))) {
        std::fprintf(stderr, "mount_all: %s\n", err); return 2;
    }
    /* ebx_dump_probe guids <hex32> [hex32...]: every EBX partition whose raw bytes
     * contain one of these 16-byte type guids (written as in type_names.tsv). */
    /* ebx_dump_probe res <substr>: list RES resources whose name contains substr. */
    if (std::strcmp(argv[1], "res") == 0) {
        const int tot = bf6_list_res(ctx, argv[2], nullptr, 0);
        std::vector<bf6_asset> all((size_t)(tot > 0 ? tot : 0));
        const int n = bf6_list_res(ctx, argv[2], all.data(), tot);
        for (int i = 0; i < n; ++i) {
            const uint8_t* raw = nullptr;
            const int64_t len = bf6_read_raw(ctx, BF6_RAW_RES, all[(size_t)i].name, &raw);
            std::printf("RES %s type 0x%08X bytes %lld\n", all[(size_t)i].name, (unsigned)all[(size_t)i].type, (long long)len);
        }
        bf6_close(ctx);
        return 0;
    }
    /* ebx_dump_probe types <name substr or -> <guid prefix>...: every partition
     * holding an instance whose type guid starts with a prefix, with the count. */
    if (std::strcmp(argv[1], "types") == 0) {
        const int tot = bf6_list_ebx(ctx, nullptr, nullptr, 0);
        std::vector<bf6_asset> all((size_t)(tot > 0 ? tot : 0));
        const int n = bf6_list_ebx(ctx, nullptr, all.data(), tot);
        std::vector<char> buf(8u << 20);
        int scanned = 0, failed = 0;
        for (int i = 0; i < n; ++i) {
            if (std::strcmp(argv[2], "-") != 0 && !std::strstr(all[(size_t)i].name, argv[2])) continue;
            ++scanned;
            /* BF6_STRUCTS: match embedded struct types ("<inst> <guid>" lines). */
            const bool structs = std::getenv("BF6_STRUCTS") != nullptr;
            if ((structs ? bf6_ebx_struct_types(ctx, all[(size_t)i].name, buf.data(), (int)buf.size())
                         : bf6_ebx_instance_types(ctx, all[(size_t)i].name, buf.data(), (int)buf.size())) < 0) {
                ++failed; continue;
            }
            for (int a = 3; a < argc; ++a) {
                int cnt = 0;
                const size_t pl = std::strlen(argv[a]);
                for (const char* p = buf.data(); *p; ) {
                    const char* g = p;
                    if (structs) { const char* sp = std::strchr(p, ' '); if (sp) g = sp + 1; }
                    if (std::strncmp(g, argv[a], pl) == 0) {
                        ++cnt;
                        if (structs && std::getenv("BF6_STRUCTS_VERBOSE"))
                            std::printf("  %s inst %d\n", argv[a], std::atoi(p));
                    }
                    const char* nl = std::strchr(p, '\n');
                    if (!nl) break;
                    p = nl + 1;
                }
                if (cnt) std::printf("TYPE %s x%d in %s\n", argv[a], cnt, all[(size_t)i].name);
            }
            if (scanned % 20000 == 0) { std::fprintf(stderr, "  %d scanned\n", scanned); }
        }
        std::printf("scanned %d partitions (%d failed to parse)\n", scanned, failed);
        bf6_close(ctx);
        return 0;
    }
    if (std::strcmp(argv[1], "guids") == 0) {
        std::vector<std::vector<uint8_t>> want;
        for (int a = 2; a < argc; ++a) {
            std::vector<uint8_t> g;
            for (int i = 0; i + 1 < (int)std::strlen(argv[a]) && g.size() < 16; i += 2) {
                char hb[3] = {argv[a][i], argv[a][i + 1], 0};
                g.push_back((uint8_t)std::strtoul(hb, nullptr, 16));
            }
            want.push_back(g);
        }
        /* BF6_SCANRES: scan RES resources (expression graphs live there) not EBX. */
        const bool res = std::getenv("BF6_SCANRES") != nullptr;
        const char* nf = std::getenv("BF6_NAMEFILTER");
        const int tot = res ? bf6_list_res(ctx, nf, nullptr, 0) : bf6_list_ebx(ctx, nullptr, nullptr, 0);
        std::vector<bf6_asset> all((size_t)(tot > 0 ? tot : 0));
        const int n = res ? bf6_list_res(ctx, nf, all.data(), tot) : bf6_list_ebx(ctx, nullptr, all.data(), tot);
        int hits = 0;
        /* BF6_NAMEFILTER limits the scan to names containing it; a pattern may be
         * shorter than 16 bytes (a 4-byte name hash, written in file byte order). */
        for (int i = 0; i < n; ++i) {
            if (nf && !std::strstr(all[(size_t)i].name, nf)) continue;
            const uint8_t* raw = nullptr;
            const int64_t len = bf6_read_raw(ctx, res ? BF6_RAW_RES : BF6_RAW_EBX, all[(size_t)i].name, &raw);
            if (len <= 0 || !raw) continue;
            for (size_t w = 0; w < want.size(); ++w) {
                const auto& g = want[w];
                const int64_t gl = (int64_t)g.size();
                int cnt = 0;
                for (int64_t o = 0; o + gl <= len; ++o)
                    if (raw[o] == g[0] && std::memcmp(raw + o, g.data(), (size_t)gl) == 0) ++cnt;
                if (cnt) {
                    std::printf("GUID %s x%d in %s\n", argv[2 + w], cnt, all[(size_t)i].name);
                    ++hits;
                }
            }
        }
        std::printf("scanned %d partitions, %d hits\n", n, hits);
        bf6_close(ctx);
        return 0;
    }
    const int total = bf6_list_ebx(ctx, nullptr, nullptr, 0);
    std::vector<bf6_asset> assets((size_t)(total > 0 ? total : 0));
    const int got = bf6_list_ebx(ctx, nullptr, assets.data(), total);
    std::vector<char> buf(64u << 20);
    int dumped = 0;
    for (int i = 0; i < got; ++i) {
        bool hit = false;
        for (int a = 2; a < (bones_mode ? 3 : argc); ++a)
            if (std::strstr(assets[(size_t)i].name, argv[a])) hit = true;
        if (!hit) continue;
        if (std::getenv("BF6_BINDINGS")) {
            std::vector<bf6_channel_binding> bs(256);
            const int nb = bf6_expression_channel_bindings(ctx, assets[(size_t)i].name, bs.data(), 256);
            std::printf("===== %s  bindings %d\n", assets[(size_t)i].name, nb);
            for (int b = 0; b < nb && b < 256; ++b)
                std::printf("  pool 0x%X  %-36s 0x%08X\n", bs[(size_t)b].pool_offset,
                            bs[(size_t)b].channel_name, bs[(size_t)b].channel_hash);
            ++dumped;
            continue;
        }
        if (bones_mode) {
            bf6_skeleton* sk = bf6_skeleton_read(ctx, assets[(size_t)i].name);
            if (!sk) continue;
            int marked = 0;
            std::printf("===== %s  bones %d\n", assets[(size_t)i].name, sk->bone_count);
            for (int b = 0; b < sk->bone_count; ++b) {
                const bf6_bone& bn = sk->bones[b];
                int idx = -1;
                for (size_t w = 0; w < want.size(); ++w) if (want[w] == bn.name_hash) idx = (int)w;
                if (idx >= 0) ++marked;
                std::printf("  %3d  0x%08X  %-40s parent %3d  model t (%.3f %.3f %.3f)%s",
                            b, bn.name_hash, bn.name ? bn.name : "?", bn.parent,
                            bn.model[3], bn.model[7], bn.model[11], idx >= 0 ? "  <== list" : "");
                if (idx >= 0) std::printf("[%d]", idx);
                std::printf("\n");
            }
            std::printf("marked %d of %zu\n", marked, want.size());
            bf6_free(ctx, sk);
            ++dumped;
            continue;
        }
        const int64_t n = bf6_ebx_dump(ctx, assets[(size_t)i].name, depth, buf.data(), (int)buf.size());
        std::printf("===== %s (%lld)\n%s\n", assets[(size_t)i].name, (long long)n,
                    n > 0 ? buf.data() : buf.data());
        ++dumped;
    }
    std::printf("dumped %d\n", dumped);
    bf6_close(ctx);
    return 0;
}
