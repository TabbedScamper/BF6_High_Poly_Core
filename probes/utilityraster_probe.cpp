/* Exact Frostbite utility-raster decoder control.
 *
 * This probe reads the current installed game. It does not consume an
 * exported or pre-solved intermediate. The grammar mirrors the current
 * bf6.exe utility-raster loader at 0x14591e3e0/0x14591e740 and its R8 payload
 * reader at 0x14591ebb0.
 *
 *   utilityraster_probe <game_dir> <level>
 */
#include "source.h"
#include "splat.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace bf6;

namespace {

template <class T> T rd(const std::vector<uint8_t>& d, size_t o)
{
    T v{};
    if (o + sizeof(T) <= d.size()) std::memcpy(&v, d.data() + o, sizeof(T));
    return v;
}

struct Page {
    uint16_t x = 0, y = 0;
    uint8_t level = 0;
    size_t source_offset = 0;
    std::vector<uint8_t> texels;
};

struct Node {
    uint16_t child_base = 0xffff;
    uint16_t x = 0, y = 0;
    uint8_t level = 0;
    bool has_page = false;
    bool has_inline = false;
    uint8_t inline_value = 0;
};

struct UtilityRaster {
    uint32_t tile_side = 0;
    uint32_t bytes_per_texel = 0;
    float min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    uint32_t declared_nodes = 0, declared_pages = 0;
    uint32_t max_level = 0, border = 0, default_value = 0;
    uint32_t interior_side = 0;
    size_t pos = 0;
    std::vector<Node> nodes;
    std::vector<Page> pages;

    bool byte(const std::vector<uint8_t>& d, uint8_t& out, std::string& err)
    {
        if (pos >= d.size()) { err = "read past end at " + std::to_string(pos); return false; }
        out = d[pos++];
        return true;
    }

    bool flag(const std::vector<uint8_t>& d, bool& out, std::string& err)
    {
        uint8_t v = 0;
        if (!byte(d, v, err)) return false;
        if (v > 1) { err = "non-boolean flag at " + std::to_string(pos - 1); return false; }
        out = v != 0;
        return true;
    }

    bool parse_node(const std::vector<uint8_t>& d, uint32_t index,
                    uint16_t x, uint16_t y, uint8_t level,
                    uint16_t& next_child, std::string& err)
    {
        if (index >= nodes.size()) { err = "node index exceeds declaration"; return false; }
        Node& n = nodes[index];
        n.x = x; n.y = y; n.level = level;

        bool has_data = false, persistent = false, has_inline = false;
        if (!flag(d, has_data, err) || !flag(d, persistent, err) ||
            !flag(d, has_inline, err)) return false;
        n.has_inline = has_inline;
        if (has_inline && !byte(d, n.inline_value, err)) return false;

        /* The game only invokes the payload hooks for this conjunction. For
           the utility R8 specialization +0x58 reads tile_side^2 bytes. */
        n.has_page = has_data && persistent;
        if (n.has_page) {
            const uint64_t count = (uint64_t)tile_side * tile_side * bytes_per_texel;
            if (count > d.size() || pos + (size_t)count > d.size()) {
                err = "page payload past end at " + std::to_string(pos);
                return false;
            }
            Page p;
            p.x = x; p.y = y; p.level = level; p.source_offset = pos;
            p.texels.assign(d.begin() + (ptrdiff_t)pos,
                            d.begin() + (ptrdiff_t)(pos + (size_t)count));
            pages.push_back(std::move(p));
            pos += (size_t)count;
        }

        bool has_children = false;
        if (!flag(d, has_children, err)) return false;
        if (!has_children) return true;
        if ((uint32_t)next_child + 3 >= nodes.size()) {
            err = "child group exceeds declared node count";
            return false;
        }
        const uint16_t base = next_child;
        n.child_base = base;
        next_child = (uint16_t)(next_child + 4);
        const uint16_t x2 = (uint16_t)(x * 2), y2 = (uint16_t)(y * 2);
        if (!parse_node(d, base + 0, x2 + 0, y2 + 0, level + 1, next_child, err) ||
            !parse_node(d, base + 1, x2 + 1, y2 + 0, level + 1, next_child, err) ||
            !parse_node(d, base + 2, x2 + 1, y2 + 1, level + 1, next_child, err) ||
            !parse_node(d, base + 3, x2 + 0, y2 + 1, level + 1, next_child, err))
            return false;
        return true;
    }

    bool parse(const std::vector<uint8_t>& d, size_t start, std::string& err)
    {
        *this = UtilityRaster{};
        if (d.size() < 44 || start > d.size()) { err = "short utility-raster header"; return false; }
        tile_side = rd<uint32_t>(d, 0);
        const uint32_t texel_shift = rd<uint32_t>(d, 4);
        if (texel_shift > 3) { err = "invalid texel-size shift"; return false; }
        bytes_per_texel = 1u << texel_shift;
        min_x = rd<float>(d, 8); min_y = rd<float>(d, 12);
        max_x = rd<float>(d, 16); max_y = rd<float>(d, 20);
        declared_nodes = rd<uint32_t>(d, 24);
        declared_pages = rd<uint32_t>(d, 28);
        max_level = rd<uint32_t>(d, 32);
        border = rd<uint32_t>(d, 36);
        default_value = rd<uint32_t>(d, 40);
        if (!tile_side || declared_nodes > 65535 || max_level > 15 ||
            border * 2 + 1 >= tile_side) {
            err = "invalid utility-raster header values";
            return false;
        }
        interior_side = tile_side - border * 2 - 1;
        nodes.resize(declared_nodes);
        pos = start;
        uint16_t next_child = 1;
        if (!parse_node(d, 0, 0, 0, 0, next_child, err)) return false;
        if (next_child != declared_nodes) {
            err = "node count mismatch: parsed " + std::to_string(next_child) +
                  ", declared " + std::to_string(declared_nodes);
            return false;
        }
        if (pages.size() != declared_pages) {
            err = "page count mismatch: parsed " + std::to_string(pages.size()) +
                  ", declared " + std::to_string(declared_pages);
            return false;
        }
        if (pos != d.size()) {
            err = "decoder leaves " + std::to_string(d.size() - pos) + " bytes";
            return false;
        }
        return true;
    }
};

double page_adjacent_mad(const Page& p, uint32_t side)
{
    if (p.texels.empty() || side < 2) return 0.0;
    uint64_t total = 0, count = 0;
    for (uint32_t y = 0; y < side; ++y)
        for (uint32_t x = 0; x + 1 < side; ++x) {
            total += (uint64_t)std::abs((int)p.texels[(size_t)y * side + x] -
                                       (int)p.texels[(size_t)y * side + x + 1]);
            ++count;
        }
    return count ? (double)total / count : 0.0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: utilityraster_probe <game_dir> <level>\n");
        return 2;
    }
    Source src;
    std::string err;
    if (!src.open(argv[1], err)) { std::fprintf(stderr, "open: %s\n", err.c_str()); return 1; }
    if (!src.mount_level(argv[2], false, err)) { std::fprintf(stderr, "mount: %s\n", err.c_str()); return 1; }

    std::string level = argv[2], tree;
    for (char& c : level) c = (char)std::tolower((unsigned char)c);
    for (const auto& kv : src.res()) {
        std::string n = kv.first;
        for (char& c : n) c = (char)std::tolower((unsigned char)c);
        if (n.find("streamingtree") != std::string::npos && n.find(level) != std::string::npos) {
            tree = kv.first; break;
        }
    }
    if (tree.empty()) { std::fprintf(stderr, "no terrain tree\n"); return 1; }
    std::vector<uint8_t> res = src.get_res(tree, err), block;
    if (res.empty() || !Splat::find_block(res, 10, block, err)) {
        std::fprintf(stderr, "block 10: %s\n", err.c_str()); return 1;
    }

    UtilityRaster raster;
    if (!raster.parse(block, 44, err)) {
        std::fprintf(stderr, "exact decoder rejected block 10: %s\n", err.c_str());
        return 1;
    }
    std::printf("PASS live block10 utility raster\n");
    std::printf("  resource=%s\n", tree.c_str());
    std::printf("  bytes=%zu tile=%u interior=%u bpp=%u border=%u default=%u\n",
        block.size(), raster.tile_side, raster.interior_side,
        raster.bytes_per_texel, raster.border, raster.default_value);
    std::printf("  bounds=(%.1f,%.1f)..(%.1f,%.1f) nodes=%zu pages=%zu maxLevel=%u consumed=%zu\n",
        raster.min_x, raster.min_y, raster.max_x, raster.max_y,
        raster.nodes.size(), raster.pages.size(), raster.max_level, raster.pos);
    std::printf("  page offsets:");
    for (const Page& p : raster.pages)
        std::printf(" %zu@L%u(%u,%u)", p.source_offset, p.level, p.x, p.y);
    std::printf("\n");
    double real_mad = 0.0;
    for (const Page& p : raster.pages) real_mad += page_adjacent_mad(p, raster.tile_side);
    real_mad /= std::max<size_t>(raster.pages.size(), 1);
    std::printf("  page adjacent MAD=%.6f\n", real_mad);

    /* Required controls: a shifted body must fail the grammar, and an absent
       detail slot must not be silently widened to another block. */
    UtilityRaster shifted;
    std::string shifted_err;
    const bool shifted_ok = shifted.parse(block, 45, shifted_err);
    std::printf("CONTROL shifted+1: %s (%s)\n",
        shifted_ok ? "UNEXPECTED PASS" : "rejected", shifted_err.c_str());

    std::vector<uint8_t> sibling;
    std::string sibling_err;
    const bool sibling_ok = Splat::find_block(res, 11, sibling, sibling_err);
    std::printf("CONTROL sibling block11: %s (%s)\n",
        sibling_ok ? "present" : "rejected", sibling_err.c_str());
    return shifted_ok ? 1 : 0;
}
