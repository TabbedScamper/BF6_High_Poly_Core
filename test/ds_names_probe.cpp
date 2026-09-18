/* WHAT THE DOFSET BLOB READS AS, through the reflection reader.
 *
 *   ds_names_probe <game> <dofset asset> [more...]
 *
 * DofSetAsset 0xf71b7fd3 is typed Int64[] but holds a byte blob: 8 x u32
 * header, one 32-byte record per DOF (tag, stride, pose byte offset, name
 * offset, ...), then packed NUL-terminated names "<Bone>.<q|t|s>" (research:
 * dof-names-and-pose-offsets-ship-in-the-ds-payload). That finding saw the
 * array reader TRUNCATE the field; it predates the primitive-array fix, so
 * this checks what comes back now, and whether the blob closes on itself.
 */
#include "bf6_core.h"
#include "ant_graph.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { std::printf("usage: ds_names_probe <game> <dofset> [more]\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    bf6_mount_all(c, 1, err, (int)sizeof(err));
    bf6ant::Graph g(c);
    for (int a = 2; a < argc; ++a) {
        const bf6ant::Obj* o = g.root(argv[a]);
        if (!o) { std::printf("%s: not loaded (%s)\n", argv[a], g.error().c_str()); continue; }
        const bf6::EbxValue* f = o->f(0xf71b7fd3u);
        std::vector<uint8_t> b;
        if (f) for (const auto& it : f->items) {
            const uint64_t u = it.kind == bf6::EbxValue::Kind::Int ? (uint64_t)it.i : it.u;
            for (int k = 0; k < 8; ++k) b.push_back((uint8_t)(u >> (8 * k)));
        }
        std::printf("%s: %zu items, %zu bytes\n", argv[a], f ? f->items.size() : (size_t)0, b.size());
        if (b.size() < 32) continue;
        uint32_t h[8];
        std::memcpy(h, b.data(), 32);
        const uint32_t n = h[1] + 1;
        const size_t rec0 = 32, blob = rec0 + (size_t)n * 32;
        std::printf("  hdr: %u %u %u %u %u %u %u %u  -> %u DOFs, blob at %zu\n",
                    h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], n, blob);
        for (uint32_t i = 0; i < n; ++i) {
            if (rec0 + (size_t)i * 32 + 32 > b.size()) { std::printf("  record %u past the data\n", i); break; }
            uint32_t r[8];
            std::memcpy(r, b.data() + rec0 + (size_t)i * 32, 32);
            const size_t no = blob + r[3];
            std::string name = no < b.size() ? std::string((const char*)b.data() + no, strnlen((const char*)b.data() + no, b.size() - no)) : "(past data)";
            std::printf("  %2u tag %-10u pose %4u  %s\n", i, r[0], r[2], name.c_str());
        }
    }
    bf6_close(c);
    return 0;
}
