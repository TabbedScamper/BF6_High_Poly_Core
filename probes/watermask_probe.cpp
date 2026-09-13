/* Falsification harness for terrain block 10 as the CoarseMask water input.
 *
 * This is research code, never a runtime intermediate. It reads the current
 * install and asks whether the candidate closes under the bounded raw-raster
 * grammars below. A deliberately absent block id is the extraction control.
 *
 *   watermask_probe <game_dir> <level>
 */
#include "source.h"
#include "splat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
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

struct RawNode {
    uint64_t key = 0;
    int depth = 0;
    std::vector<uint8_t> texels;
};

struct RawTree {
    const std::vector<uint8_t>* d = nullptr;
    size_t p = 0;
    int dim = 0;
    int declared_nodes = 0;
    int declared_data = 0;
    int levels = 0;
    int visited = 0;
    int data_a = 0, data_b = 1;
    std::vector<RawNode> nodes;

    bool node(uint64_t key, int depth, std::string& err)
    {
        if (!d || p + 3 > d->size()) { err = "node record past end"; return false; }
        const int f[3] = {(*d)[p], (*d)[p + 1], (*d)[p + 2]};
        p += 3;
        const int has_data = f[data_a];
        const int persistent = data_b >= 0 ? f[data_b] : 1;
        const int reserved = 0;
        if (reserved > 1 || has_data > 1 || persistent > 1)
        { err = "non-boolean node record"; return false; }
        visited++;

        std::vector<uint8_t> px;
        if (has_data && persistent)
        {
            const size_t n = (size_t)dim * (size_t)dim;
            if (p + n > d->size()) { err = "raw tile past end"; return false; }
            px.assign(d->begin() + (ptrdiff_t)p, d->begin() + (ptrdiff_t)(p + n));
            p += n;
        }
        if (p >= d->size()) { err = "child flag past end"; return false; }
        const int children = (*d)[p++];
        if (children > 1) { err = "non-boolean child flag"; return false; }
        if (!px.empty()) nodes.push_back({key, depth, std::move(px)});
        if (children)
            for (int i = 0; i < 4; ++i)
                if (!node((key << 4) | (uint64_t)i, depth + 1, err)) return false;
        return true;
    }

    bool parse(const std::vector<uint8_t>& in, size_t start, int a, int b, std::string& err)
    {
        if (in.size() < 40) { err = "block 10 shorter than header"; return false; }
        d = &in;
        dim = rd<int32_t>(in, 0);
        declared_nodes = rd<int32_t>(in, 24);
        declared_data = rd<int32_t>(in, 28);
        levels = rd<int32_t>(in, 32);
        // Four-byte extension after the ten-dword raster header, then one
        // four-byte record per preorder node.  A persistent data node carries
        // its raw dim*dim tile immediately after its record.
        p = start;
        data_a = a; data_b = b;
        if (!node(3, 0, err)) return false;
        if (p != in.size()) { err = "node walk leaves " + std::to_string(in.size() - p) + " bytes"; return false; }
        if (visited != declared_nodes) { err = "visited/declaration node mismatch"; return false; }
        if ((int)nodes.size() != declared_data) { err = "data/declaration node mismatch"; return false; }
        d = nullptr;
        return true;
    }
};

struct RawTree4 {
    const std::vector<uint8_t>* d = nullptr;
    size_t p = 0;
    int dim = 0, visited = 0, data_count = 0, max_nodes = 0, max_depth = 0;
    int data_a = 0, data_b = -1, child = 1;
    int data_va = 1, data_vb = 1, child_v = 1, data_mode = 0;
    std::vector<RawNode> nodes;
    bool node(uint64_t key, int depth)
    {
        if (visited >= max_nodes || depth > max_depth) return false;
        if (!d || p + 4 > d->size()) return false;
        const uint8_t* f = d->data() + p;
        for (int i = 0; i < 4; ++i) if (f[i] > 1) return false;
        const bool da = f[data_a] == data_va;
        const bool db = data_b < 0 ? true : f[data_b] == data_vb;
        const bool has_data = data_b < 0 ? da : (data_mode == 0 ? da && db : da || db);
        const bool has_children = f[child] == child_v;
        p += 4; ++visited;
        if (has_data) {
            const size_t n = (size_t)dim * dim;
            if (p + n > d->size()) return false;
            RawNode r; r.key = key; r.depth = depth;
            r.texels.assign(d->begin() + (ptrdiff_t)p,
                            d->begin() + (ptrdiff_t)(p + n));
            nodes.push_back(std::move(r)); p += n; ++data_count;
        }
        if (has_children)
            for (int i = 0; i < 4; ++i)
                if (!node((key << 4) | (uint64_t)i, depth + 1)) return false;
        return true;
    }
};

struct RawTreeSplit {
    const std::vector<uint8_t>* d = nullptr;
    size_t p = 0; int dim = 0, visited = 0, data_count = 0;
    int pre = 2, data_a = 0, data_b = -1, child_post = 0;
    std::vector<RawNode> nodes;
    bool node(uint64_t key, int depth)
    {
        if (!d || p + (size_t)pre > d->size()) return false;
        uint8_t f[4] = {};
        for (int i = 0; i < pre; ++i) { f[i] = (*d)[p+i]; if (f[i] > 1) return false; }
        const bool has_data = f[data_a] && (data_b < 0 || f[data_b]);
        p += pre; ++visited;
        if (has_data) {
            const size_t n = (size_t)dim * dim;
            if (p + n > d->size()) return false;
            RawNode r; r.key=key; r.depth=depth;
            r.texels.assign(d->begin()+(ptrdiff_t)p,d->begin()+(ptrdiff_t)(p+n));
            nodes.push_back(std::move(r)); p += n; ++data_count;
        }
        const int post = 4-pre;
        if (p + (size_t)post > d->size()) return false;
        for (int i=0;i<post;++i) { f[pre+i]=(*d)[p+i]; if(f[pre+i]>1)return false; }
        const bool has_children = f[pre+child_post] != 0;
        p += post;
        if (has_children)
            for(int i=0;i<4;++i) if(!node((key<<4)|(uint64_t)i,depth+1)) return false;
        return true;
    }
};

/* The current MP_Isolated block closes arithmetically as a split four-byte
   record: three boolean bytes, an optional 515x515 page, then one boolean
   byte.  Do not guess that the page predicate is one lane or a conjunction;
   exhaust the complete truth table of the three leading bits.  There are only
   256 such functions, so this remains a bounded format test rather than a
   heuristic scan. */
struct RawTreeTruth3 {
    const std::vector<uint8_t>* d = nullptr;
    size_t p = 0;
    int dim = 0, visited = 0, data_count = 0, max_nodes = 0, max_depth = 0;
    uint8_t data_truth = 0;
    int child_lane = 3, child_value = 1;
    std::vector<RawNode> nodes;

    bool node(uint64_t key, int depth)
    {
        if (!d || visited >= max_nodes || depth > max_depth || p + 3 > d->size())
            return false;
        const uint8_t a = (*d)[p], b = (*d)[p + 1], c = (*d)[p + 2];
        if (a > 1 || b > 1 || c > 1) return false;
        p += 3;
        ++visited;
        const unsigned state = (unsigned)a | ((unsigned)b << 1) | ((unsigned)c << 2);
        const bool has_data = ((data_truth >> state) & 1u) != 0;
        if (has_data) {
            const size_t n = (size_t)dim * (size_t)dim;
            if (p + n > d->size()) return false;
            RawNode page;
            page.key = key;
            page.depth = depth;
            page.texels.assign(d->begin() + (ptrdiff_t)p,
                               d->begin() + (ptrdiff_t)(p + n));
            nodes.push_back(std::move(page));
            p += n;
            ++data_count;
        }
        if (p >= d->size() || (*d)[p] > 1) return false;
        const uint8_t post = (*d)[p++];
        const uint8_t flags[4] = { a, b, c, post };
        const bool children = flags[child_lane] == child_value;
        if (children)
            for (int i = 0; i < 4; ++i)
                if (!node((key << 4) | (uint64_t)i, depth + 1)) return false;
        return true;
    }
};

struct RawTreeGeneric {
    const std::vector<uint8_t>* d = nullptr;
    size_t p = 0;
    int dim = 0, visited = 0, data_count = 0, max_nodes = 0, max_depth = 0;
    int record = 3, pre = 2, data_i = 0, data_j = -1;
    int data_v = 1, data_w = 1, data_mode = 0;
    int child_i = 2, child_v = 1;
    std::vector<RawNode> nodes;

    bool node(uint64_t key, int depth)
    {
        if (!d || visited >= max_nodes || depth > max_depth ||
            p + (size_t)pre > d->size()) return false;
        uint8_t f[8] = {};
        for (int i = 0; i < pre; ++i) {
            f[i] = (*d)[p + (size_t)i];
            if (f[i] > 1) return false;
        }
        p += (size_t)pre;
        ++visited;
        const bool da = f[data_i] == data_v;
        const bool db = data_j < 0 ? true : f[data_j] == data_w;
        const bool has_data = data_j < 0 ? da : (data_mode == 0 ? da && db : da || db);
        if (has_data) {
            const size_t bytes = (size_t)dim * (size_t)dim;
            if (p + bytes > d->size()) return false;
            RawNode n; n.key = key; n.depth = depth;
            n.texels.assign(d->begin() + (ptrdiff_t)p,
                            d->begin() + (ptrdiff_t)(p + bytes));
            nodes.push_back(std::move(n));
            p += bytes;
            ++data_count;
        }
        const int post = record - pre;
        if (p + (size_t)post > d->size()) return false;
        for (int i = 0; i < post; ++i) {
            f[pre + i] = (*d)[p + (size_t)i];
            if (f[pre + i] > 1) return false;
        }
        p += (size_t)post;
        const bool children = f[child_i] == child_v;
        if (children)
            for (int i = 0; i < 4; ++i)
                if (!node((key << 4) | (uint64_t)i, depth + 1)) return false;
        return true;
    }
};

struct RawTreePadded {
    const std::vector<uint8_t>* d = nullptr;
    size_t p = 0; int dim=0, visited=0, tiles=0, max_nodes=0;
    int a=0,b=1,va=1,vb=1,mode=0,child_v=1,prefix=0,suffix=0;
    bool node(int depth)
    {
        if(!d || visited>=max_nodes || depth>8 || p+2>d->size()) return false;
        const uint8_t x=(*d)[p], y=(*d)[p+1];
        if(x>1||y>1)return false;
        const uint8_t f[2]={x,y}; p+=2; ++visited;
        const bool aa=f[a]==va, bb=f[b]==vb;
        const bool data=mode==0?(aa&&bb):(aa||bb);
        if(data){
            const size_t n=(size_t)dim*dim;
            if(p+(size_t)prefix+n+(size_t)suffix>d->size())return false;
            p+=(size_t)prefix+n+(size_t)suffix;++tiles;
        }
        if(p>=d->size()||(*d)[p]>1)return false;
        const bool children=(*d)[p++]==child_v;
        if(children)for(int i=0;i<4;++i)if(!node(depth+1))return false;
        return true;
    }
};

struct RawTreeAllPre {
    const std::vector<uint8_t>* d=nullptr; size_t p=0; int dim=0,visited=0,tiles=0,max_nodes=0;
    int child=2,child_v=1; std::vector<RawNode> pages;
    bool node(uint64_t key,int depth){
        if(!d||visited>=max_nodes||depth>8||p+3>d->size())return false;
        const uint8_t a=(*d)[p],b=(*d)[p+1],c=(*d)[p+2];
        if(a>1||b>1||c>1)return false;const uint8_t f[3]={a,b,c};p+=3;++visited;
        if(a&&b){const size_t n=(size_t)dim*dim;if(p+n>d->size())return false;
            RawNode page;page.key=key;page.depth=depth;
            page.texels.assign(d->begin()+(ptrdiff_t)p,d->begin()+(ptrdiff_t)(p+n));
            pages.push_back(std::move(page));p+=n;++tiles;}
        if((f[child]==child_v))for(int i=0;i<4;++i)
            if(!node((key<<4)|(uint64_t)i,depth+1))return false;
        return true;
    }
};

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: watermask_probe <game_dir> <level>\n");
        return 2;
    }
    Source src;
    std::string err;
    if (!src.open(argv[1], err)) { std::fprintf(stderr, "open: %s\n", err.c_str()); return 1; }
    if (!src.mount_level(argv[2], false, err)) { std::fprintf(stderr, "mount: %s\n", err.c_str()); return 1; }
    std::string level = argv[2], tree;
    for (char& c : level) c = (char)std::tolower((unsigned char)c);
    for (const auto& kv : src.res())
    {
        std::string n = kv.first;
        for (char& c : n) c = (char)std::tolower((unsigned char)c);
        if (n.find("streamingtree") != std::string::npos && n.find(level) != std::string::npos)
        { tree = kv.first; break; }
    }
    if (tree.empty()) { std::fprintf(stderr, "no terrain tree\n"); return 1; }
    std::vector<uint8_t> res = src.get_res(tree, err), b10;
    if (res.empty() || !Splat::find_block(res, 10, b10, err))
    { std::fprintf(stderr, "block 10: %s\n", err.c_str()); return 1; }

    std::printf("terrain block census:\n");
    for (int type = 0; type <= 12; ++type) {
        std::vector<uint8_t> block;
        std::string block_err;
        if (!Splat::find_block(res, type, block, block_err)) continue;
        std::printf("  type=%d bytes=%zu dwords=", type, block.size());
        for (size_t off = 0; off + 4 <= block.size() && off < 64; off += 4)
            std::printf("%s%08X", off ? "," : "", (unsigned)rd<uint32_t>(block, off));
        std::printf("\n");
    }
    SplatChunkDir directory;
    std::string directory_err;
    if (Splat::read_chunk_dir(res, directory, directory_err))
        std::printf("  shared chunk-directory nodes=%zu\n", directory.size());
    else
        std::printf("  shared chunk-directory rejected: %s\n", directory_err.c_str());

    std::printf("tree %s\nblock10 %zu bytes\n", tree.c_str(), b10.size());
    std::printf("header dim=%d blur=%d bounds=(%.1f %.1f)..(%.1f %.1f) nodes=%d data=%d levels=%d tail=%d\n",
        rd<int32_t>(b10, 0), rd<int32_t>(b10, 4), rd<float>(b10, 8), rd<float>(b10, 12),
        rd<float>(b10, 16), rd<float>(b10, 20), rd<int32_t>(b10, 24), rd<int32_t>(b10, 28),
        rd<int32_t>(b10, 32), rd<int32_t>(b10, 36));
    std::printf("head bytes:");
    for (size_t i = 32; i < std::min<size_t>(128, b10.size()); ++i)
        std::printf("%s%02X", ((i - 32) % 16) ? " " : "\n  ", (unsigned)b10[i]);
    std::printf("\n");
    {
        const int dim = rd<int32_t>(b10, 0);
        double best_peak = 1e30; size_t best_start = 0; int best_col = -1;
        for (size_t s = 72; s <= 128 && s + (size_t)dim * dim <= b10.size(); ++s) {
            double peak = 0.0; int peak_col = -1;
            for (int x = 0; x + 1 < dim; ++x) {
                double sum = 0.0;
                for (int y = 0; y < dim; ++y) {
                    const size_t o = s + (size_t)y * dim + x;
                    sum += std::abs((int)b10[o] - (int)b10[o + 1]);
                }
                const double mean = sum / dim;
                if (mean > peak) { peak = mean; peak_col = x; }
            }
            if (peak < best_peak) { best_peak = peak; best_start = s; best_col = peak_col; }
        }
        std::printf("first-tile seam control bestStart=%zu peak=%.3f column=%d\n",
            best_start, best_peak, best_col);

        auto seam_peak = [&](size_t s) {
            double peak = 0.0;
            for (int x = 0; x + 1 < dim; ++x) {
                uint64_t sum = 0;
                for (int y = 0; y < dim; ++y) {
                    const size_t o = s + (size_t)y * dim + x;
                    sum += (uint64_t)std::abs((int)b10[o] - (int)b10[o + 1]);
                }
                peak = std::max(peak, (double)sum / dim);
            }
            return peak;
        };
        std::vector<size_t> starts;
        starts.push_back(best_start);
        const int tiles = rd<int32_t>(b10, 28);
        for (int i = 1; i < tiles; ++i) {
            const size_t tile_span = (size_t)dim * dim;
            const size_t base = starts.back() + tile_span;
            double chosen_score = 1e30; size_t chosen = 0;
            for (size_t gap = 0; gap <= 32; ++gap) {
                const size_t s = base + gap;
                if (s + (size_t)dim * dim > b10.size()) continue;
                const double score = seam_peak(s);
                if (score < chosen_score) { chosen_score = score; chosen = s; }
            }
            starts.push_back(chosen);
        }
        std::printf("tile starts/gaps:");
        for (size_t i = 0; i < starts.size(); ++i) {
            const size_t gap = i ? starts[i] - starts[i-1] - (size_t)dim * dim : starts[i];
            std::printf(" %zu%s%zu", starts[i], i ? "+" : "@", gap);
        }
        std::printf("\nmetadata-consumed before tiles (minus 40, div/mod4):");
        for (size_t i = 0; i < starts.size(); ++i) {
            const size_t consumed = starts[i] - i * (size_t)dim * dim;
            std::printf(" %zu=%zu/%zu", consumed,
                consumed >= 40 ? (consumed - 40) / 4 : 0,
                consumed >= 40 ? (consumed - 40) % 4 : 0);
        }
        const size_t last_end = starts.back() + (size_t)dim * dim;
        std::printf(" trailer=%zu\n", b10.size() - last_end);

        /* Global alignment control.  A seam-correct page starts after a
           cumulative metadata count congruent to the first page (mod 4).
           Optimize all pages together, require monotone metadata consumption,
           bounded inter-page node bytes, and a <=32-byte trailer. */
        const size_t metadata_total = b10.size() -
            (size_t)tiles * (size_t)dim * dim;
        std::vector<size_t> meta_values;
        for (size_t m = best_start; m <= metadata_total; ++m) meta_values.push_back(m);
        const double inf = std::numeric_limits<double>::infinity();
        std::vector<std::vector<double>> score((size_t)tiles,
            std::vector<double>(meta_values.size(), inf));
        for (int i = 0; i < tiles; ++i)
            for (size_t j = 0; j < meta_values.size(); ++j) {
                const size_t s = (size_t)i * (size_t)dim * dim + meta_values[j];
                if (s + (size_t)dim * dim > b10.size()) continue;
                if (s < 3 || b10[s-3] != 1 || b10[s-2] != 1 || b10[s-1] != 0)
                    continue;
                double peak = 0.0;
                for (int x = 0; x + 1 < dim; ++x) {
                    uint64_t sum = 0, count = 0;
                    for (int y = 0; y < dim; y += 4) {
                        const size_t o = s + (size_t)y * dim + x;
                        sum += (uint64_t)std::abs((int)b10[o] - (int)b10[o + 1]);
                        ++count;
                    }
                    peak = std::max(peak, (double)sum / count);
                }
                score[(size_t)i][j] = peak;
            }
        std::vector<std::vector<double>> cost((size_t)tiles,
            std::vector<double>(meta_values.size(), inf));
        std::vector<std::vector<int>> back((size_t)tiles,
            std::vector<int>(meta_values.size(), -1));
        cost[0][0] = score[0][0];
        for (int i = 1; i < tiles; ++i)
            for (size_t j = 0; j < meta_values.size(); ++j)
                for (size_t k = 0; k <= j; ++k) {
                    const size_t delta = meta_values[j] - meta_values[k];
                    if (delta < 1 || delta > 128 ||
                        !std::isfinite(cost[(size_t)i-1][k])) continue;
                    const size_t gap_at = (size_t)i * (size_t)dim * dim + meta_values[k];
                    bool metadata_bytes = true;
                    for (size_t p = gap_at; p < gap_at + delta; ++p)
                        metadata_bytes &= b10[p] <= 1;
                    if (!metadata_bytes) continue;
                    const double c = cost[(size_t)i-1][k] + score[(size_t)i][j];
                    if (c < cost[(size_t)i][j]) {
                        cost[(size_t)i][j] = c; back[(size_t)i][j] = (int)k;
                    }
                }
        int best_j = -1; double best_cost = inf;
        for (size_t j = 0; j < meta_values.size(); ++j) {
            const size_t trailer = metadata_total - meta_values[j];
            const size_t trailer_at = (size_t)tiles * (size_t)dim * dim + meta_values[j];
            bool metadata_bytes = true;
            for (size_t p = trailer_at; p < b10.size(); ++p)
                metadata_bytes &= b10[p] <= 1;
            if (trailer <= 32 && metadata_bytes &&
                cost[(size_t)tiles-1][j] < best_cost) {
                best_cost = cost[(size_t)tiles-1][j]; best_j = (int)j;
            }
        }
        if (best_j >= 0) {
            std::vector<size_t> chosen((size_t)tiles);
            int j = best_j;
            for (int i = tiles - 1; i >= 0; --i) {
                chosen[(size_t)i] = meta_values[(size_t)j];
                j = i ? back[(size_t)i][(size_t)j] : 0;
            }
            std::printf("global page alignment cost=%.3f metadata:", best_cost);
            for (size_t m : chosen) std::printf(" %zu", m);
            std::printf(" trailer=%zu\n", metadata_total - chosen.back());

            std::vector<uint8_t> metadata;
            size_t source = 0;
            for (int i = 0; i < tiles; ++i) {
                const size_t page = (size_t)i * (size_t)dim * dim + chosen[(size_t)i];
                metadata.insert(metadata.end(), b10.begin() + (ptrdiff_t)source,
                    b10.begin() + (ptrdiff_t)page);
                source = page + (size_t)dim * dim;
            }
            metadata.insert(metadata.end(), b10.begin() + (ptrdiff_t)source, b10.end());
            std::vector<uint8_t> is_data((size_t)rd<int32_t>(b10,24));
            for (size_t m : chosen)
                if (m >= 43 && (m - 43) % 4 == 0 && (m - 43) / 4 < is_data.size())
                    is_data[(m - 43) / 4] = 1;
            std::printf("reconstructed metadata=%zu data nodes:", metadata.size());
            for(size_t i=0;i<is_data.size();++i)if(is_data[i])std::printf(" %zu",i);
            std::printf("\nrecord patterns [bytes]: data/nondata counts\n");
            struct Pattern { std::array<uint8_t,4> p{}; int data=0, other=0; };
            std::vector<Pattern> patterns;
            for(size_t i=0;i<is_data.size() && 40+i*4+4<=metadata.size();++i){
                std::array<uint8_t,4> p={metadata[40+i*4],metadata[41+i*4],
                    metadata[42+i*4],metadata[43+i*4]};
                auto it=std::find_if(patterns.begin(),patterns.end(),
                    [&](const Pattern& q){return q.p==p;});
                if(it==patterns.end()){patterns.push_back({p,0,0});it=patterns.end()-1;}
                if(is_data[i])it->data++;else it->other++;
            }
            for(const Pattern& p:patterns)
                std::printf("  %u%u%u%u: %d/%d\n",p.p[0],p.p[1],p.p[2],p.p[3],
                    p.data,p.other);

            /* Mip-hierarchy oracle.  If these are pages of one utility raster,
               a child page must match one quadrant of its parent after a 2:1
               coordinate reduction.  Compare only non-flat children and put
               a deterministic X permutation beside the real score as the
               null control. */
            std::vector<size_t> page_at((size_t)tiles);
            for (int i=0;i<tiles;++i)
                page_at[(size_t)i]=(size_t)i*(size_t)dim*dim+chosen[(size_t)i];
            std::printf("hierarchy candidates child: parent/quadrant realMAD shuffledMAD\n");
            for(int child=0;child<tiles;++child){
                const size_t co=page_at[(size_t)child];
                double mean=0.0,var=0.0;uint64_t vn=0;
                for(int y=4;y<dim-4;y+=8)for(int x=4;x<dim-4;x+=8){
                    mean+=b10[co+(size_t)y*dim+x];++vn;
                }
                mean/=std::max<uint64_t>(vn,1);
                for(int y=4;y<dim-4;y+=8)for(int x=4;x<dim-4;x+=8){
                    const double z=b10[co+(size_t)y*dim+x]-mean;var+=z*z;
                }
                var/=std::max<uint64_t>(vn,1);
                if(var<64.0)continue;
                double best=1e30,best_null=0.0;int bp=-1,bq=-1;
                for(int parent=0;parent<tiles;++parent){
                    if(parent==child)continue;
                    const size_t po=page_at[(size_t)parent];
                    for(int q=0;q<4;++q){
                        const int qx=q&1,qy=(q>>1)&1;
                        double mad=0.0,nul=0.0;uint64_t n=0;
                        for(int y=4;y<dim-4;y+=4)for(int x=4;x<dim-4;x+=4){
                            const int px=std::clamp((int)((qx+(x+0.5)/(double)dim)*dim/2.0),0,dim-1);
                            const int py=std::clamp((int)((qy+(y+0.5)/(double)dim)*dim/2.0),0,dim-1);
                            const int sx=4+(x-4+(dim/3))%(dim-8);
                            const int v=b10[po+(size_t)py*dim+px];
                            mad+=std::abs(v-(int)b10[co+(size_t)y*dim+x]);
                            nul+=std::abs(v-(int)b10[co+(size_t)y*dim+sx]);++n;
                        }
                        mad/=n;nul/=n;
                        if(mad<best){best=mad;best_null=nul;bp=parent;bq=q;}
                    }
                }
                std::printf("  %d: %d/q%d %.3f %.3f\n",child,bp,bq,best,best_null);
            }
        }
    }
    std::printf("control fake block type 11: ");
    std::vector<uint8_t> fake;
    std::string fake_err;
    const bool fake_ok = Splat::find_block(res, 11, fake, fake_err);
    std::printf("%s (%s)\n", fake_ok ? "UNEXPECTED PRESENT" : "rejected", fake_err.c_str());
    if (fake_ok) return 1;

    /* Learn the fixed-record data predicate instead of guessing a flag lane.
       A state carries the 4-bit record patterns already forced to mean
       data/non-data.  The physical offset is then determined exactly by
       (record index, number of prior pages), so contradictory uses of the
       same pattern reject the path.  This deliberately ignores topology;
       topology is a separate claim and must not bias page framing. */
    struct LearnedState {
        int pages = 0;
        uint16_t no = 0, yes = 0;
        std::vector<uint8_t> take;
        std::vector<std::array<uint8_t,4>> records;
    };
    const int learn_nodes = rd<int32_t>(b10, 24);
    const int learn_pages = rd<int32_t>(b10, 28);
    const size_t learn_page_bytes = (size_t)rd<int32_t>(b10, 0) *
                                    (size_t)rd<int32_t>(b10, 0);
    int learned_closures = 0;
    for (int placement = 0; placement <= 4 && learned_closures < 8; ++placement)
     for (size_t start = 36; start <= 128; ++start) {
        std::vector<LearnedState> states(1);
        for (int i = 0; i < learn_nodes && !states.empty(); ++i) {
            std::vector<LearnedState> next;
            for (const LearnedState& s : states) {
                const size_t p = start + (size_t)i * 4 +
                                 (size_t)s.pages * learn_page_bytes;
                for (int take = 0; take <= 1; ++take) {
                    if (s.pages + take > learn_pages) continue;
                    std::array<uint8_t,4> r{};
                    bool fits_record = true;
                    for (int lane = 0; lane < 4; ++lane) {
                        const size_t at = p + (size_t)lane +
                            ((take && lane >= placement) ? learn_page_bytes : 0);
                        if (at >= b10.size() || b10[at] > 1) {
                            fits_record = false; break;
                        }
                        r[(size_t)lane] = b10[at];
                    }
                    if (!fits_record) continue;
                    const int pattern = r[0] | (r[1] << 1) | (r[2] << 2) | (r[3] << 3);
                    const uint16_t bit = (uint16_t)(1u << pattern);
                    if (take && (s.no & bit)) continue;
                    if (!take && (s.yes & bit)) continue;
                    LearnedState q = s;
                    q.pages += take;
                    (take ? q.yes : q.no) |= bit;
                    q.take.push_back((uint8_t)take);
                    q.records.push_back(r);
                    next.push_back(std::move(q));
                }
            }
            /* Equal (page count, learned truth table) states have the same
               future byte position.  Retain one witness only. */
            std::vector<LearnedState> unique;
            for (LearnedState& s : next) {
                const auto it = std::find_if(unique.begin(), unique.end(),
                    [&](const LearnedState& q) { return q.pages == s.pages &&
                        q.no == s.no && q.yes == s.yes; });
                if (it == unique.end()) unique.push_back(std::move(s));
            }
            states.swap(unique);
        }
        for (const LearnedState& s : states) {
            if (s.pages != learn_pages) continue;
            const size_t end = start + (size_t)learn_nodes * 4 +
                               (size_t)learn_pages * learn_page_bytes;
            if (end > b10.size() || b10.size() - end > 64) continue;
            std::printf("learned flat closure: start=%zu placement=%d trailer=%zu yes=0x%04X no=0x%04X pages:",
                start, placement, b10.size() - end, (unsigned)s.yes, (unsigned)s.no);
            int prior = 0;
            for (int i = 0; i < learn_nodes; ++i) {
                if (s.take[(size_t)i]) {
                    const size_t page = start + (size_t)i * 4 +
                        (size_t)prior * learn_page_bytes + (size_t)placement;
                    std::printf(" %d@%zu", i, page);
                    ++prior;
                }
            }
            std::printf("\n  record patterns:");
            for (int p = 0; p < 16; ++p)
                if ((s.yes | s.no) & (1u << p))
                    std::printf(" %X=%c", p, (s.yes & (1u << p)) ? 'D' : '-');
            std::printf("\n");
            for (int lane = 0; lane < 4; ++lane)
                for (int value = 0; value <= 1; ++value) {
                    int pending = 1, max_pending = 1;
                    bool proper = true;
                    for (int i = 0; i < learn_nodes; ++i) {
                        if (pending <= 0) { proper = false; break; }
                        --pending;
                        if (s.records[(size_t)i][(size_t)lane] == value) pending += 4;
                        max_pending = std::max(max_pending, pending);
                    }
                    if (proper && pending == 0)
                        std::printf("  preorder control closes: child=(f%d==%d), maxPending=%d\n",
                            lane, value, max_pending);
                }
            if (++learned_closures >= 8) break;
        }
        if (learned_closures >= 8) break;
    }
    if (!learned_closures) std::printf("learned flat control: no fixed 4-byte predicate closes\n");

    /* The utility raster may serialize several roots even though each root is
       an ordinary four-way tree.  The material-tree control has one root; test
       the only root counts compatible with N = roots + 4*internal. */
    bool forest_closed = false;
    for (int roots = 1; roots <= 17 && !forest_closed; roots += 4)
      for (int record = 3; record <= 5 && !forest_closed; ++record)
       for (int pre = 1; pre < record && !forest_closed; ++pre)
        for (int di = 0; di < pre && !forest_closed; ++di)
         for (int dj = -1; dj < pre && !forest_closed; ++dj) {
          if (dj == di) continue;
          for (int dv = 0; dv <= 1 && !forest_closed; ++dv)
           for (int dw = 0; dw <= 1 && !forest_closed; ++dw)
            for (int mode = 0; mode <= (dj < 0 ? 0 : 1) && !forest_closed; ++mode)
             for (int ci = 0; ci < record && !forest_closed; ++ci)
              for (int cv = 0; cv <= 1 && !forest_closed; ++cv)
               for (size_t start = 36; start <= 128 && !forest_closed; ++start) {
                RawTreeGeneric t; t.d=&b10; t.p=start; t.dim=rd<int32_t>(b10,0);
                t.max_nodes=rd<int32_t>(b10,24); t.max_depth=rd<int32_t>(b10,32);
                t.record=record; t.pre=pre; t.data_i=di; t.data_j=dj;
                t.data_v=dv; t.data_w=dw; t.data_mode=mode;
                t.child_i=ci; t.child_v=cv;
                bool ok=true;
                for(int root=0;root<roots && ok;++root)
                    ok=t.node((uint64_t)(3+root),0);
                if(!ok)continue;
                const size_t remain=b10.size()-t.p;
                if(t.visited==t.max_nodes && t.data_count==rd<int32_t>(b10,28) && remain<=128){
                  std::printf("forest grammar closes: roots=%d start=%zu record=%d pre=%d "
                    "data=(f%d==%d)%s(f%d==%d) child=(f%d==%d) trailer=%zu\n",
                    roots,start,record,pre,di,dv,dj<0?"":(mode?" OR ":" AND "),
                    dj<0?di:dj,dj<0?dv:dw,ci,cv,remain);
                  forest_closed=true;
                }
               }
         }
    if(!forest_closed)std::printf("forest grammar control: no compatible root count closes\n");

    for(size_t start=36;start<=128;++start)
      for(int child=0;child<3;++child)for(int cv=0;cv<=1;++cv){
        RawTreeAllPre t;t.d=&b10;t.p=start;t.dim=rd<int32_t>(b10,0);
        t.max_nodes=rd<int32_t>(b10,24);t.child=child;t.child_v=cv;
        if(!t.node(3,0))continue;const size_t remain=b10.size()-t.p;
        if(t.visited==t.max_nodes&&t.tiles==rd<int32_t>(b10,28)&&remain<=128){
          std::printf("all-leading grammar closes: start=%zu data=f0&f1 child=(f%d==%d) "
            "nodes=%d tiles=%d trailer=%zu pages:",start,child,cv,t.visited,t.tiles,remain);
          for(const RawNode& p:t.pages)std::printf(" %llx@%d",
            (unsigned long long)p.key,p.depth);
          std::printf("\n");return 0;
        }
      }

    /* Solve the interleaving independently of semantics.  If the finding's
       four-byte-record model is right, record i is at
         start + 4*i + dim*dim*(number of earlier data nodes).
       Dynamic programming over only (record count, data count) recovers the
       data-node bitset without guessing which byte means what. */
    const int declared_nodes = rd<int32_t>(b10, 24);
    const int declared_tiles = rd<int32_t>(b10, 28);
    const size_t tile_bytes = (size_t)rd<int32_t>(b10, 0) * rd<int32_t>(b10, 0);
    for (size_t start = 36; start <= 64; ++start) {
        struct Prev { int pd = -1, took = -1; };
        std::vector<std::vector<uint8_t>> reach((size_t)declared_nodes + 1,
            std::vector<uint8_t>((size_t)declared_tiles + 1));
        std::vector<std::vector<Prev>> prev((size_t)declared_nodes + 1,
            std::vector<Prev>((size_t)declared_tiles + 1));
        reach[0][0] = 1;
        for (int i = 0; i < declared_nodes; ++i)
            for (int d = 0; d <= declared_tiles; ++d) {
                if (!reach[(size_t)i][(size_t)d]) continue;
                const size_t p = start + (size_t)i * 4 + (size_t)d * tile_bytes;
                if (p + 4 > b10.size()) continue;
                bool flags = true;
                for (int k = 0; k < 4; ++k) flags &= b10[p + (size_t)k] <= 1;
                if (!flags) continue;
                for (int take = 0; take <= 1; ++take) {
                    const int nd = d + take;
                    if (nd > declared_tiles || reach[(size_t)i + 1][(size_t)nd]) continue;
                    const size_t next = start + (size_t)(i + 1) * 4 +
                        (size_t)nd * tile_bytes;
                    if (next > b10.size()) continue;
                    reach[(size_t)i + 1][(size_t)nd] = 1;
                    prev[(size_t)i + 1][(size_t)nd] = {d, take};
                }
            }
        if (!reach[(size_t)declared_nodes][(size_t)declared_tiles]) continue;
        std::vector<int> data((size_t)declared_nodes);
        int d = declared_tiles;
        for (int i = declared_nodes; i > 0; --i) {
            const Prev q = prev[(size_t)i][(size_t)d];
            data[(size_t)i - 1] = q.took; d = q.pd;
        }
        const size_t end = start + (size_t)declared_nodes * 4 +
            (size_t)declared_tiles * tile_bytes;
        std::printf("flat four-byte interleave closes: start=%zu trailer=%zu data indices:",
            start, b10.size() - end);
        int prior_data = 0;
        std::vector<std::array<uint8_t,4>> records;
        for (int i = 0; i < declared_nodes; ++i) {
            if (data[(size_t)i]) std::printf(" %d", i);
            const size_t p = start + (size_t)i * 4 + (size_t)prior_data * tile_bytes;
            records.push_back({b10[p],b10[p+1],b10[p+2],b10[p+3]});
            prior_data += data[(size_t)i];
        }
        std::printf("\n");
        for (int lane = 0; lane < 4; ++lane)
            for (int value = 0; value <= 1; ++value) {
                int match = 0;
                for (int i = 0; i < declared_nodes; ++i)
                    match += ((records[(size_t)i][(size_t)lane] == value) ==
                              (data[(size_t)i] != 0));
                std::printf("  data control f%d==%d score=%d/%d\n",
                    lane, value, match, declared_nodes);
            }
        break;
    }

    bool generic_closed = false;
    RawTreeGeneric generic;
    for (int record = 2; record <= 6 && !generic_closed; ++record)
            for (int pre = 1; pre < record && !generic_closed; ++pre)
                for (int di = 0; di < pre && !generic_closed; ++di)
                  for (int dj = -1; dj < pre && !generic_closed; ++dj) {
                    if (dj == di) continue;
                    for (int dv = 0; dv <= 1 && !generic_closed; ++dv)
                      for (int dw = 0; dw <= 1 && !generic_closed; ++dw)
                       for (int mode = 0; mode <= (dj < 0 ? 0 : 1) && !generic_closed; ++mode)
                        for (int ci = 0; ci < record && !generic_closed; ++ci)
                         for (int cv = 0; cv <= 1 && !generic_closed; ++cv)
                          for (size_t start = 36; start <= 128 && !generic_closed; ++start) {
                                RawTreeGeneric t;
                                t.d = &b10; t.p = start; t.dim = rd<int32_t>(b10, 0);
                                t.max_nodes = rd<int32_t>(b10, 24);
                                t.max_depth = rd<int32_t>(b10, 32);
                                t.record = record; t.pre = pre;
                                t.data_i = di; t.data_j = dj;
                                t.data_v = dv; t.data_w = dw; t.data_mode = mode;
                                t.child_i = ci; t.child_v = cv;
                                if (!t.node(3, 0)) continue;
                                const size_t remain = b10.size() - t.p;
                                if (t.visited == t.max_nodes &&
                                    t.data_count == rd<int32_t>(b10, 28) && remain <= 128) {
                                    std::printf("generic grammar closes: start=%zu record=%d "
                                        "pre=%d data=(f%d==%d)%s(f%d==%d) child=(f%d==%d) "
                                        "nodes=%d tiles=%d trailer=%zu\n",
                                        start, record, pre, di, dv,
                                        dj < 0 ? "" : (mode ? " OR " : " AND "),
                                        dj < 0 ? di : dj, dj < 0 ? dv : dw, ci, cv,
                                        t.visited, t.data_count, remain);
                                    generic = std::move(t);
                                    generic_closed = true;
                                }
                          }
                  }
    if (generic_closed) {
        uint64_t hist[256] = {}; double adj = 0; uint64_t adj_n = 0;
        for (const RawNode& n : generic.nodes) {
            for (uint8_t v : n.texels) hist[v]++;
            for (int y = 0; y < generic.dim; ++y)
                for (int x = 1; x < generic.dim; ++x) {
                    const int o = y * generic.dim + x;
                    adj += std::abs((int)n.texels[o] - (int)n.texels[o - 1]); ++adj_n;
                }
        }
        uint64_t total=0, weighted=0; int values=0;
        for(int i=0;i<256;++i){total+=hist[i];weighted+=hist[i]*(uint64_t)i;if(hist[i])++values;}
        std::printf("generic walk stats values=%d zero=%.3f%% full=%.3f%% mean=%.3f adjacentMAD=%.3f\n",
            values,100.0*hist[0]/(double)total,100.0*hist[255]/(double)total,
            weighted/(double)total,adj/(double)adj_n);
        return 0;
    }

    for(size_t start=36;start<=96;++start)
      for(int a=0;a<2;++a)for(int b=0;b<2;++b){
       if(a==b)continue;
       for(int va=0;va<=1;++va)for(int vb=0;vb<=1;++vb)
        for(int mode=0;mode<=1;++mode)for(int cv=0;cv<=1;++cv)
         for(int prefix=0;prefix<=8;++prefix)for(int suffix=0;suffix<=8;++suffix){
          RawTreePadded t;t.d=&b10;t.p=start;t.dim=rd<int32_t>(b10,0);
          t.max_nodes=rd<int32_t>(b10,24);t.a=a;t.b=b;t.va=va;t.vb=vb;
          t.mode=mode;t.child_v=cv;t.prefix=prefix;t.suffix=suffix;
          if(!t.node(0))continue;
          const size_t remain=b10.size()-t.p;
          if(t.visited==t.max_nodes&&t.tiles==rd<int32_t>(b10,28)&&remain<=128){
            std::printf("padded raster grammar closes: start=%zu data=(f%d==%d)%s(f%d==%d) "
              "child=%d prefix=%d suffix=%d nodes=%d tiles=%d trailer=%zu\n",
              start,a,va,mode?" OR ":" AND ",b,vb,cv,prefix,suffix,
              t.visited,t.tiles,remain);
            return 0;
          }
         }
      }

    bool four_closed = false;
    RawTree4 four;
    for (size_t start = 36; start <= 64 && !four_closed; ++start)
        for (int a = 0; a < 4 && !four_closed; ++a)
            for (int b = -1; b < 4 && !four_closed; ++b) {
                if (b == a) continue;
                for (int va = 0; va <= 1 && !four_closed; ++va)
                    for (int vb = 0; vb <= 1 && !four_closed; ++vb)
                        for (int mode = 0; mode <= (b < 0 ? 0 : 1) && !four_closed; ++mode)
                            for (int ch = 0; ch < 4 && !four_closed; ++ch)
                                for (int cv = 0; cv <= 1 && !four_closed; ++cv) {
                                    RawTree4 t; t.d = &b10; t.p = start;
                                    t.dim = rd<int32_t>(b10, 0);
                                    t.max_nodes = rd<int32_t>(b10, 24);
                                    t.max_depth = rd<int32_t>(b10, 32);
                                    t.data_a = a; t.data_b = b; t.child = ch;
                                    t.data_va = va; t.data_vb = vb;
                                    t.data_mode = mode; t.child_v = cv;
                                    if (!t.node(3, 0)) continue;
                                    const size_t remain = b10.size() - t.p;
                                    if (t.visited == rd<int32_t>(b10, 24) &&
                                        t.data_count == rd<int32_t>(b10, 28) && remain <= 32) {
                                        std::printf("4-byte grammar closes: start=%zu "
                                            "data=(f%d==%d)%s(f%d==%d) child=(f%d==%d) "
                                            "nodes=%d tiles=%d trailer=%zu\n",
                                            start, a, va,
                                            b < 0 ? "" : (mode ? " OR " : " AND "),
                                            b < 0 ? a : b, b < 0 ? va : vb,
                                            ch, cv, t.visited, t.data_count, remain);
                                        four = std::move(t); four_closed = true;
                                    }
                                }
            }
    if (four_closed) {
        uint64_t hist[256] = {}; double adj = 0; uint64_t adj_n = 0;
        for (const RawNode& n : four.nodes) {
            for (uint8_t v : n.texels) hist[v]++;
            for (int y = 0; y < four.dim; ++y)
                for (int x = 1; x < four.dim; ++x) {
                    const int o = y * four.dim + x;
                    adj += std::abs((int)n.texels[o] - (int)n.texels[o - 1]); ++adj_n;
                }
        }
        uint64_t total = 0, weighted = 0; int values = 0;
        for (int i = 0; i < 256; ++i) { total += hist[i]; weighted += hist[i]*(uint64_t)i; if(hist[i])++values; }
        std::printf("4-byte walk stats values=%d zero=%.3f%% full=%.3f%% mean=%.3f adjacentMAD=%.3f\n",
            values, 100.0*hist[0]/(double)total, 100.0*hist[255]/(double)total,
            weighted/(double)total, adj/(double)adj_n);
        return 0;
    }

    /* First test the weaker, format-only hypothesis: the records may be a
       flat sparse-node table rather than recursive preorder.  The page split
       still gives a deterministic truth-table solve, but makes no claim about
       which flag encodes ancestry. */
    int flat_truth_solutions = 0;
    int flat_truth_first = -1;
    std::vector<RawNode> flat_truth_pages;
    std::vector<std::array<uint8_t,4>> flat_truth_records;
    size_t flat_truth_start = 0;
    for (size_t start = 36; start <= 40; ++start)
     for (int truth = 0; truth <= 255; ++truth) {
        size_t p = start;
        int tiles = 0;
        std::vector<RawNode> pages;
        std::vector<std::array<uint8_t,4>> records;
        bool ok = true;
        for (int i = 0; i < declared_nodes; ++i) {
            if (p + 3 > b10.size()) { ok = false; break; }
            const uint8_t a=b10[p], b=b10[p+1], c=b10[p+2];
            if (a>1 || b>1 || c>1) { ok = false; break; }
            p += 3;
            const unsigned state=(unsigned)a|((unsigned)b<<1)|((unsigned)c<<2);
            const bool data=((truth>>state)&1)!=0;
            if (data) {
                if (p + tile_bytes > b10.size()) { ok = false; break; }
                RawNode page; page.key=(uint64_t)i; page.depth=-1;
                page.texels.assign(b10.begin()+(ptrdiff_t)p,
                                   b10.begin()+(ptrdiff_t)(p+tile_bytes));
                pages.push_back(std::move(page)); p += tile_bytes; ++tiles;
            }
            if (p >= b10.size() || b10[p] > 1) { ok = false; break; }
            records.push_back({a,b,c,b10[p]}); ++p;
        }
        if (!ok || tiles != declared_tiles || b10.size()-p > 32) continue;
        ++flat_truth_solutions;
        if (flat_truth_first < 0) {
            flat_truth_first=truth;
            flat_truth_start=start;
            flat_truth_pages=std::move(pages);
            flat_truth_records=std::move(records);
            std::printf("flat truth3 grammar closes: start=%zu truth=0x%02X "
                        "nodes=%d tiles=%d trailer=%zu data indices:",
                        start, truth, declared_nodes, tiles, b10.size()-p);
            for (const RawNode& page : flat_truth_pages)
                std::printf(" %llu", (unsigned long long)page.key);
            std::printf("\n");
        }
    }
    std::printf("flat truth3 grammar control solutions=%d\n", flat_truth_solutions);

    /* Recover the split-record page bitset without assigning semantics to any
       flag.  The current byte budget says the record stream begins at 36 and
       a page, when present, is inserted after byte three.  Dynamic programming
       chooses exactly the declared number of pages using a cheap local image
       continuity score.  A deliberately shifted-page score is printed as the
       null control. */
    {
        const size_t record_start = 36;
        const double inf = std::numeric_limits<double>::infinity();
        std::vector<std::vector<double>> cost((size_t)declared_nodes + 1,
            std::vector<double>((size_t)declared_tiles + 1, inf));
        struct PagePrev { int data = -1; int took = -1; };
        std::vector<std::vector<PagePrev>> back((size_t)declared_nodes + 1,
            std::vector<PagePrev>((size_t)declared_tiles + 1));
        cost[0][0] = 0.0;
        auto page_score = [&](size_t at, int shift) {
            const ptrdiff_t shifted = (ptrdiff_t)at + (ptrdiff_t)shift;
            if (shifted < 0 || (size_t)shifted + tile_bytes > b10.size()) return inf;
            double sum = 0.0; uint64_t n = 0;
            const int dim = rd<int32_t>(b10, 0);
            at = (size_t)shifted;
            for (int y = 8; y < dim - 8; y += 16)
                for (int x = 8; x < dim - 9; x += 16) {
                    const size_t p = at + (size_t)y * (size_t)dim + (size_t)x;
                    sum += std::abs((int)b10[p] - (int)b10[p + 1]);
                    ++n;
                }
            return n ? sum / (double)n : inf;
        };
        for (int i = 0; i < declared_nodes; ++i)
            for (int d = 0; d <= declared_tiles; ++d) {
                if (!std::isfinite(cost[(size_t)i][(size_t)d])) continue;
                const size_t rec = record_start + (size_t)i * 4 + (size_t)d * tile_bytes;
                if (rec + 4 <= b10.size()) {
                    bool flags = true;
                    for (int k=0;k<4;++k) flags &= b10[rec+(size_t)k] <= 1;
                    if (flags && cost[(size_t)i][(size_t)d] < cost[(size_t)i+1][(size_t)d]) {
                        cost[(size_t)i+1][(size_t)d] = cost[(size_t)i][(size_t)d];
                        back[(size_t)i+1][(size_t)d] = {d,0};
                    }
                }
                if (d >= declared_tiles || rec + 3 + tile_bytes + 1 > b10.size()) continue;
                if (b10[rec]>1 || b10[rec+1]>1 || b10[rec+2]>1 ||
                    b10[rec+3+tile_bytes]>1) continue;
                const double s = page_score(rec + 3, 0);
                const double nc = cost[(size_t)i][(size_t)d] + s;
                if (nc < cost[(size_t)i+1][(size_t)d+1]) {
                    cost[(size_t)i+1][(size_t)d+1] = nc;
                    back[(size_t)i+1][(size_t)d+1] = {d,1};
                }
            }
        if (std::isfinite(cost[(size_t)declared_nodes][(size_t)declared_tiles])) {
            std::vector<int> data((size_t)declared_nodes);
            int d = declared_tiles;
            for (int i=declared_nodes;i>0;--i) {
                const PagePrev q=back[(size_t)i][(size_t)d];
                data[(size_t)i-1]=q.took; d=q.data;
            }
            std::vector<std::array<uint8_t,4>> recs;
            recs.reserve((size_t)declared_nodes);
            int prior=0; double null_cost=0.0;
            std::printf("continuity DP closes: real=%.3f data indices:",
                        cost[(size_t)declared_nodes][(size_t)declared_tiles]);
            for(int i=0;i<declared_nodes;++i){
                const size_t rec=record_start+(size_t)i*4+(size_t)prior*tile_bytes;
                const size_t post=rec+3+(data[(size_t)i]?tile_bytes:0);
                recs.push_back({b10[rec],b10[rec+1],b10[rec+2],b10[post]});
                if(data[(size_t)i]){
                    std::printf(" %d",i);
                    const int null_shift = rec + 3 + tile_bytes + 137 <= b10.size()
                        ? 137 : -137;
                    null_cost += page_score(rec+3, null_shift);
                    ++prior;
                }
            }
            std::printf("\ncontinuity control shifted=%.3f real/shifted=%.4f\n",
                        null_cost, cost[(size_t)declared_nodes][(size_t)declared_tiles] /
                        std::max(null_cost,1e-9));
            for(int lane=0;lane<4;++lane)for(int value=0;value<=1;++value){
                int pending=1; bool valid=true;
                for(int i=0;i<declared_nodes;++i){
                    if(pending<=0){valid=false;break;}
                    --pending;
                    if(recs[(size_t)i][(size_t)lane]==value) pending+=4;
                }
                if(valid && pending==0)
                    std::printf("hierarchy lane closes: child=(f%d==%d) nodes=%d\n",
                                lane,value,declared_nodes);
            }
            std::printf("record pattern data/nondata:\n");
            for(unsigned pat=0;pat<16;++pat){int yes=0,no=0;
                for(int i=0;i<declared_nodes;++i){
                    unsigned q=0;for(int k=0;k<4;++k)q|=(unsigned)recs[(size_t)i][(size_t)k]<<k;
                    if(q==pat)(data[(size_t)i]?yes:no)++;
                }
                if(yes||no)std::printf("  %X: %d/%d\n",pat,yes,no);
            }
        } else {
            std::printf("continuity DP control: no exact split-record solution\n");
        }
    }

    /* Stronger solve for the other possible framing: all four child-presence
       bytes precede an optional page.  A parent-before-child stream can be DFS
       or BFS; in either case one root plus child links must close exactly at
       nodeCount.  This invariant chooses page locations without naming a data
       flag or looking at image content. */
    {
        const int N=declared_nodes,D=declared_tiles,P=declared_nodes;
        const double inf=std::numeric_limits<double>::infinity();
        int closed=0;
        for(size_t record_start=36;record_start<=40;++record_start){
            const size_t states=(size_t)(N+1)*(size_t)(D+1)*(size_t)(P+1);
            std::vector<double> cost(states,inf);
            struct PrefixPrev{int prev_pending=-1;int took=-1;};
            std::vector<PrefixPrev> back(states);
            auto ix=[=](int i,int d,int pending){return ((size_t)i*(D+1)+(size_t)d)*(P+1)+(size_t)pending;};
            auto local_score=[&](size_t at){
                if(at+tile_bytes>b10.size())return inf;
                const int dim=rd<int32_t>(b10,0);double sum=0.0;uint64_t n=0;
                for(int y=7;y<dim-7;y+=13)for(int x=7;x<dim-8;x+=13){
                    const size_t p=at+(size_t)y*(size_t)dim+(size_t)x;
                    sum+=std::abs((int)b10[p]-(int)b10[p+1]);++n;
                }
                return n?sum/(double)n:inf;
            };
            cost[ix(0,0,1)]=0.0;
            for(int i=0;i<N;++i)for(int d=0;d<=D;++d)for(int pending=1;pending<=P;++pending){
                const double base=cost[ix(i,d,pending)];if(!std::isfinite(base))continue;
                const size_t rec=record_start+(size_t)i*4+(size_t)d*tile_bytes;
                if(rec+4>b10.size())continue;
                bool flags=true;int children=0;
                for(int k=0;k<4;++k){flags&=b10[rec+(size_t)k]<=1;children+=(int)b10[rec+(size_t)k];}
                if(!flags)continue;
                const int next_pending=pending-1+children;
                if(next_pending<0||next_pending>P||(i+1<N&&next_pending==0)||(i+1==N&&next_pending!=0))continue;
                for(int take=0;take<=1;++take){
                    if(d+take>D)continue;
                    const size_t next_pos=rec+4+(take?tile_bytes:0);
                    if(next_pos>b10.size())continue;
                    const double next=base+(take?local_score(rec+4):0.0);
                    const size_t ni=ix(i+1,d+take,next_pending);
                    if(next<cost[ni]){cost[ni]=next;back[ni]={pending,take};}
                }
            }
            const size_t finish=ix(N,D,0);
            const size_t end=record_start+(size_t)N*4+(size_t)D*tile_bytes;
            if(!std::isfinite(cost[finish])||end>b10.size()||b10.size()-end>32)continue;
            ++closed;std::vector<int> data((size_t)N);int d=D,pending=0;
            for(int i=N;i>0;--i){const PrefixPrev q=back[ix(i,d,pending)];data[(size_t)i-1]=q.took;d-=q.took;pending=q.prev_pending;}
            std::printf("prefix-child DP closes: start=%zu score=%.3f trailer=%zu data indices:",record_start,cost[finish],b10.size()-end);
            for(int i=0;i<N;++i)if(data[(size_t)i])std::printf(" %d",i);
            std::printf("\n");
        }
        std::printf("prefix-child DP control solutions=%d\n",closed);
    }

    /* Stronger solve: the four bytes are naturally a sparse quadtree's four
       child-presence flags.  That supplies a format invariant independent of
       image content: one root plus all child links must equal nodeCount, and
       the pending-node count may reach zero only after the final record. */
    {
        const size_t record_start = 36;
        const int N = declared_nodes, D = declared_tiles, P = declared_nodes;
        const double inf = std::numeric_limits<double>::infinity();
        const size_t states = (size_t)(N+1)*(size_t)(D+1)*(size_t)(P+1);
        std::vector<double> cost(states, inf);
        struct TreePrev { int prev_pending=-1; int took=-1; };
        std::vector<TreePrev> back(states);
        auto ix = [=](int i,int d,int pending){
            return ((size_t)i*(size_t)(D+1)+(size_t)d)*(size_t)(P+1)+(size_t)pending;
        };
        auto local_score = [&](size_t at) {
            if (at + tile_bytes > b10.size()) return inf;
            const int dim=rd<int32_t>(b10,0);
            double sum=0.0; uint64_t n=0;
            for(int y=7;y<dim-7;y+=13)for(int x=7;x<dim-8;x+=13){
                const size_t p=at+(size_t)y*(size_t)dim+(size_t)x;
                sum+=std::abs((int)b10[p]-(int)b10[p+1]);++n;
            }
            return n?sum/(double)n:inf;
        };
        cost[ix(0,0,1)]=0.0;
        for(int i=0;i<N;++i)for(int d=0;d<=D;++d)for(int pending=1;pending<=P;++pending){
            const double base=cost[ix(i,d,pending)];
            if(!std::isfinite(base))continue;
            for(int take=0;take<=1;++take){
                if(d+take>D)continue;
                const size_t rec=record_start+(size_t)i*4+(size_t)d*tile_bytes;
                const size_t post=rec+3+(take?tile_bytes:0);
                if(post>=b10.size()||b10[rec]>1||b10[rec+1]>1||
                   b10[rec+2]>1||b10[post]>1)continue;
                const int children=(int)b10[rec]+(int)b10[rec+1]+
                                   (int)b10[rec+2]+(int)b10[post];
                const int next_pending=pending-1+children;
                if(next_pending<0||next_pending>P)continue;
                if(i+1<N && next_pending==0)continue;
                if(i+1==N && next_pending!=0)continue;
                const double next=base+(take?local_score(rec+3):0.0);
                const size_t ni=ix(i+1,d+take,next_pending);
                if(next<cost[ni]){
                    cost[ni]=next;
                    back[ni]={pending,take};
                }
            }
        }
        const size_t finish=ix(N,D,0);
        if(std::isfinite(cost[finish])){
            std::vector<int> data((size_t)N);
            int d=D,pending=0;
            for(int i=N;i>0;--i){
                const TreePrev q=back[ix(i,d,pending)];
                data[(size_t)i-1]=q.took;
                d-=q.took;pending=q.prev_pending;
            }
            int prior=0,child_links=0;
            double real_seams=0.0,shifted_seams=0.0;
            auto seam_peak=[&](size_t at){
                const int dim=rd<int32_t>(b10,0); double peak=0.0;
                if(at+tile_bytes>b10.size())return inf;
                for(int x=0;x+1<dim;++x){uint64_t sum=0;
                    for(int y=0;y<dim;++y){const size_t p=at+(size_t)y*(size_t)dim+(size_t)x;
                        sum+=(uint64_t)std::abs((int)b10[p]-(int)b10[p+1]);}
                    peak=std::max(peak,(double)sum/(double)dim);
                }
                return peak;
            };
            std::printf("child-link DP closes: score=%.3f data indices:",cost[finish]);
            for(int i=0;i<N;++i){
                const size_t rec=record_start+(size_t)i*4+(size_t)prior*tile_bytes;
                const size_t post=rec+3+(data[(size_t)i]?tile_bytes:0);
                child_links+=(int)b10[rec]+(int)b10[rec+1]+(int)b10[rec+2]+(int)b10[post];
                if(data[(size_t)i]){
                    std::printf(" %d",i);
                    real_seams+=seam_peak(rec+3);
                    shifted_seams+=seam_peak(rec+3-137);
                    ++prior;
                }
            }
            std::printf("\nchild-link invariant links=%d expected=%d trailer=%zu\n",
                        child_links,N-1,b10.size()-(record_start+(size_t)N*4+(size_t)D*tile_bytes));
            std::printf("child-link continuity real=%.3f shifted=%.3f real/shifted=%.4f\n",
                        real_seams,shifted_seams,real_seams/std::max(shifted_seams,1e-9));
        } else {
            std::printf("child-link DP control: no exact sparse-quadtree solution\n");
        }
    }

    bool truth3_closed = false;
    RawTreeTruth3 truth3;
    int truth3_solutions = 0;
    for (size_t start = 36; start <= 64; ++start)
        for (int truth = 0; truth <= 255; ++truth)
            for (int child_lane = 0; child_lane < 4; ++child_lane)
             for (int child_value = 0; child_value <= 1; ++child_value) {
                RawTreeTruth3 t;
                t.d = &b10;
                t.p = start;
                t.dim = rd<int32_t>(b10, 0);
                t.max_nodes = rd<int32_t>(b10, 24);
                t.max_depth = rd<int32_t>(b10, 32);
                t.data_truth = (uint8_t)truth;
                t.child_lane = child_lane;
                t.child_value = child_value;
                if (!t.node(3, 0)) continue;
                const size_t remain = b10.size() - t.p;
                if (t.visited != t.max_nodes ||
                    t.data_count != rd<int32_t>(b10, 28) || remain > 32)
                    continue;
                ++truth3_solutions;
                if (!truth3_closed) {
                    truth3 = std::move(t);
                    truth3_closed = true;
                    std::printf("truth3 split grammar closes: start=%zu truth=0x%02X "
                                "child=f%d==%d nodes=%d tiles=%d trailer=%zu pages:",
                        start, truth, child_lane, child_value, truth3.visited,
                        truth3.data_count, remain);
                    for (const RawNode& p : truth3.nodes)
                        std::printf(" %llx@%d", (unsigned long long)p.key, p.depth);
                    std::printf("\n");
                }
            }
    std::printf("truth3 split grammar control solutions=%d\n", truth3_solutions);
    if (truth3_closed) {
        uint64_t hist[256] = {};
        double adj = 0.0;
        uint64_t adj_n = 0;
        for (const RawNode& n : truth3.nodes) {
            for (uint8_t v : n.texels) hist[v]++;
            for (int y = 0; y < truth3.dim; ++y)
                for (int x = 1; x < truth3.dim; ++x) {
                    const int o = y * truth3.dim + x;
                    adj += std::abs((int)n.texels[(size_t)o] -
                                    (int)n.texels[(size_t)o - 1]);
                    ++adj_n;
                }
        }
        uint64_t total = 0, weighted = 0;
        int values = 0;
        for (int i = 0; i < 256; ++i) {
            total += hist[i];
            weighted += hist[i] * (uint64_t)i;
            if (hist[i]) ++values;
        }
        std::printf("truth3 walk stats values=%d zero=%.3f%% full=%.3f%% "
                    "mean=%.3f adjacentMAD=%.3f\n",
            values, 100.0 * hist[0] / (double)total,
            100.0 * hist[255] / (double)total,
            weighted / (double)total, adj / (double)adj_n);
        return 0;
    }
    bool split_closed = false;
    RawTreeSplit split;
    for (size_t start=36;start<=64 && !split_closed;++start)
        for(int pre=1;pre<=3 && !split_closed;++pre)
            for(int a=0;a<pre && !split_closed;++a)
                for(int b=-1;b<pre && !split_closed;++b) {
                    if(b==a)continue;
                    for(int ch=0;ch<4-pre && !split_closed;++ch) {
                        RawTreeSplit t; t.d=&b10;t.p=start;t.dim=rd<int32_t>(b10,0);
                        t.pre=pre;t.data_a=a;t.data_b=b;t.child_post=ch;
                        if(!t.node(3,0))continue;
                        const size_t remain=b10.size()-t.p;
                        if(t.visited==rd<int32_t>(b10,24)&&t.data_count==rd<int32_t>(b10,28)&&remain<=32){
                            std::printf("split grammar closes: start=%zu pre=%d data=%d%s childPost=%d nodes=%d tiles=%d trailer=%zu\n",
                                start,pre,a,b<0?"":(std::string("&")+std::to_string(b)).c_str(),ch,t.visited,t.data_count,remain);
                            split=std::move(t);split_closed=true;
                        }
                    }
                }
    if(split_closed) return 0;

    {
        RawTree direct;
        std::string direct_err;
        const bool ok = direct.parse(b10, 40, 0, 1, direct_err);
        std::printf("known raster grammar control start=40 data=f0&f1: ok=%d "
                    "pos=%zu visited=%d tiles=%zu err=%s\n",
            ok ? 1 : 0, direct.p, direct.visited, direct.nodes.size(), direct_err.c_str());
    }

    /* A second bounded grammar: all node metadata precedes a dense tail of
       declared_data raw dim*dim tiles.  The earlier interleaved hypothesis is
       retained below as its control. */
    const size_t dense_bytes = (size_t)rd<int32_t>(b10, 28) *
        (size_t)rd<int32_t>(b10, 0) * (size_t)rd<int32_t>(b10, 0);
    if (dense_bytes <= b10.size()) {
        const size_t dense_at = b10.size() - dense_bytes;
        uint64_t dense_hist[256] = {};
        double dense_adj = 0.0; uint64_t dense_adj_n = 0;
        const int dim = rd<int32_t>(b10, 0);
        for (size_t p = dense_at; p < b10.size(); ++p) dense_hist[b10[p]]++;
        for (int tile = 0; tile < rd<int32_t>(b10, 28); ++tile) {
            const size_t base = dense_at + (size_t)tile * dim * dim;
            for (int y = 0; y < dim; ++y)
                for (int x = 1; x < dim; ++x) {
                    const size_t o = base + (size_t)y * dim + x;
                    dense_adj += std::abs((int)b10[o] - (int)b10[o - 1]);
                    dense_adj_n++;
                }
        }
        uint64_t total = 0, weighted = 0; int values = 0;
        for (int i = 0; i < 256; ++i) {
            total += dense_hist[i]; weighted += dense_hist[i] * (uint64_t)i;
            if (dense_hist[i]) values++;
        }
        std::printf("dense-tail candidate: metadata=%zu tiles=%zu values=%d "
                    "zero=%.3f%% full=%.3f%% mean=%.3f adjacentMAD=%.3f\n",
            dense_at, dense_bytes / ((size_t)dim * dim), values,
            100.0 * dense_hist[0] / (double)total,
            100.0 * dense_hist[255] / (double)total,
            weighted / (double)total, dense_adj / (double)dense_adj_n);
        std::printf("metadata tail bytes:");
        const size_t lo = dense_at > 96 ? dense_at - 96 : 0;
        for (size_t p = lo; p < dense_at; ++p)
            std::printf("%s%02X", ((p - lo) % 16) ? " " : "\n  ", (unsigned)b10[p]);
        std::printf("\nfirst raster bytes:");
        for (size_t p = dense_at; p < std::min(dense_at + 96, b10.size()); ++p)
            std::printf("%s%02X", ((p - dense_at) % 16) ? " " : "\n  ", (unsigned)b10[p]);
        std::printf("\n");
        std::printf("metadata nonzero offsets:");
        for (size_t p = 36; p < dense_at; ++p)
            if (b10[p]) std::printf(" %zu:%02X", p, (unsigned)b10[p]);
        std::printf("\n");
    }

    RawTree rt;
    bool parsed = false;
    for (size_t start = 36; start <= 96 && !parsed; ++start)
        for (int a = 0; a < 3 && !parsed; ++a)
        {
            RawTree trial;
            std::string trial_err;
            if (trial.parse(b10, start, a, -1, trial_err))
            {
                std::printf("grammar closes: start=%zu, data flag=%d, child=trailing\n", start, a);
                rt = std::move(trial);
                parsed = true;
            }
        }
    for (size_t start = 36; start <= 96 && !parsed; ++start)
        for (int a = 0; a < 3 && !parsed; ++a)
            for (int b = a + 1; b < 3 && !parsed; ++b)
            {
                RawTree trial;
                std::string trial_err;
                if (trial.parse(b10, start, a, b, trial_err))
                {
                    std::printf("grammar closes: start=%zu, data flags=%d&%d, child=trailing\n", start, a, b);
                    rt = std::move(trial);
                    parsed = true;
                }
            }
    if (!parsed)
    {
        rt.parse(b10, 44, 0, 1, err);
        std::fprintf(stderr, "raw walk: %s at %zu\nnear:", err.c_str(), rt.p);
        const size_t lo = rt.p > 32 ? rt.p - 32 : 0;
        for (size_t i = lo; i < std::min(rt.p + 32, b10.size()); ++i)
            std::fprintf(stderr, "%s%02X", ((i - lo) % 16) ? " " : "\n  ", (unsigned)b10[i]);
        std::fprintf(stderr, "\n");
        return 1;
    }

    uint64_t hist[256] = {};
    uint64_t adjacency_n = 0;
    double adjacency = 0.0;
    for (const RawNode& n : rt.nodes)
    {
        for (uint8_t v : n.texels) hist[v]++;
        for (int y = 0; y < rt.dim; ++y)
            for (int x = 1; x < rt.dim; ++x)
            {
                const int o = y * rt.dim + x;
                adjacency += std::abs((int)n.texels[o] - (int)n.texels[o - 1]);
                adjacency_n++;
            }
    }
    uint64_t total = 0, weighted = 0;
    int values = 0;
    for (int i = 0; i < 256; ++i) { total += hist[i]; weighted += hist[i] * (uint64_t)i; if (hist[i]) values++; }
    std::printf("walk closes: %d/%d nodes, %zu/%d data tiles; values=%d zero=%.3f%% full=%.3f%% mean=%.3f adjacentMAD=%.3f\n",
        rt.visited, rt.declared_nodes, rt.nodes.size(), rt.declared_data, values,
        100.0 * hist[0] / (double)total, 100.0 * hist[255] / (double)total,
        weighted / (double)total, adjacency / (double)adjacency_n);
    return 0;
}
