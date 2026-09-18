/* WHO REFERENCES WHAT, across the animation tree.
 *
 *   anim_refgraph_probe <game> <out.tsv> <prefix> [prefix ...]
 *
 * The 1P tree is 3491 assets: clips, and 22 kinds of structural node (.seq,
 * .seqflow, .bs, .pc, .bc, .cdb ...) that decide which clip plays when. A
 * clip on its own says nothing about when it is used; the node that imports it
 * does. So the question "how is the whole tree used" is a graph question, and
 * the graph is only visible from the REFERRING side - a node knows what it
 * imports, a clip does not know who imports it.
 *
 * This dumps every EBX under each prefix through reflection and writes one
 * edge per import:  <source>\t<imported path>\t<field hash>
 *
 * Deliberately plain: the analysis (in-degree, roots, which game-states drive
 * which nodes) is a sort/uniq away once the edges exist, and doing it here
 * would bake in the questions before seeing the data.
 */
#include "bf6_core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::printf("usage: anim_refgraph_probe <game> <out.tsv> <prefix> [prefix ...]\n");
        return 2;
    }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }
    if (!bf6_mount_all(c, 1, err, (int)sizeof(err)))
        std::printf("note: mount_all said %s\n", err);

    FILE* out = std::fopen(argv[2], "wb");
    if (!out) { std::printf("cannot write %s\n", argv[2]); return 1; }

    std::vector<char> text((size_t)8 << 20);
    long assets = 0, edges = 0, failed = 0;
    for (int a = 3; a < argc; ++a) {
        const int n = bf6_list_ebx(c, argv[a], nullptr, 0);
        if (n < 1) { std::printf("%s: 0\n", argv[a]); continue; }
        std::vector<bf6_asset> rows((size_t)n);
        const int got = bf6_list_ebx(c, argv[a], rows.data(), n);
        std::printf("%s: %d asset(s)\n", argv[a], got);
        for (int i = 0; i < got; ++i) {
            const char* name = rows[(size_t)i].name;
            if (!name || std::strncmp(name, argv[a], std::strlen(argv[a])) != 0) continue;
            ++assets;
            /* Depth 12: imports sit inside arrays of structs inside instances,
             * and a shallow dump would silently report a node as a leaf. */
            const int64_t len = bf6_ebx_dump(c, name, 12, text.data(), (int)text.size());
            if (len <= 0 || len > (int64_t)text.size()) {
                ++failed;
                std::fprintf(out, "%s\t!unreadable\t-\n", name);
                continue;
            }
            /* Lines look like:   0x8e60f433     import <path>.ebx */
            const char* p = text.data();
            const char* end = text.data() + std::strlen(text.data());
            while (p < end) {
                const char* eol = std::strchr(p, '\n');
                if (!eol) eol = end;
                const std::string line(p, eol);
                /* Instance headers - "  [3] type <guid>" - become ":type" edges,
                 * so each asset's reflected types join against type_names.tsv.
                 * The asset's name suffix is a convention; the type is the
                 * fact. */
                const size_t ty = line.find("] type ");
                if (ty != std::string::npos && line.find('[') != std::string::npos &&
                    line.find('[') < ty) {
                    std::string g = line.substr(ty + 7, 36);
                    const std::string idx = line.substr(line.find('[') + 1,
                                                        ty - line.find('[') - 1);
                    std::fprintf(out, "%s\t:type:%s\t%s\n", name, g.c_str(), idx.c_str());
                }
                const size_t im = line.find(" import ");
                if (im != std::string::npos) {
                    std::string hash = "-";
                    const size_t hx = line.find("0x");
                    if (hx != std::string::npos && hx < im) hash = line.substr(hx, 10);
                    std::string dst = line.substr(im + 8);
                    while (!dst.empty() && (dst.back() == '\r' || dst.back() == ' ')) dst.pop_back();
                    if (dst.size() > 4 && dst.compare(dst.size() - 4, 4, ".ebx") == 0)
                        dst.resize(dst.size() - 4);
                    std::fprintf(out, "%s\t%s\t%s\n", name, dst.c_str(), hash.c_str());
                    ++edges;
                }
                p = eol + 1;
            }
            if (assets % 2000 == 0) std::printf("  ... %ld assets, %ld edges\n", assets, edges);
        }
    }
    std::fclose(out);
    std::printf("DONE %ld asset(s), %ld edge(s), %ld unreadable\n", assets, edges, failed);
    bf6_close(c);
    return 0;
}
