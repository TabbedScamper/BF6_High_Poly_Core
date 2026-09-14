#include "source.h"

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <set>
#include <sstream>
#include <thread>

#include "bundle.h"
#include "cas.h"
#include "oodle.h"
#include "stdio_compat.h"

namespace bf6 {

namespace {

// Two independent full-content 64-bit fingerprints. This is an invalidation
// identity, not an authenticity primitive: the cache remains untrusted and
// every decoded path is checked against the current live mount before use.
struct ContentId {
    uint64_t a = 1469598103934665603ull;
    uint64_t b = 0x9e3779b97f4a7c15ull;
    void add(const void* ptr, size_t size) {
        const uint8_t* p = static_cast<const uint8_t*>(ptr);
        for (size_t i = 0; i < size; ++i) {
            a = (a ^ p[i]) * 1099511628211ull;
            b ^= (uint64_t)p[i] + 0x9e3779b97f4a7c15ull + (b << 6) + (b >> 2);
        }
    }
    void add(const std::string& s) { add(s.data(), s.size()); }
};

static std::pair<uint64_t, uint64_t> content_id(const void* p, size_t n) {
    ContentId h; h.add(p, n); return {h.a, h.b};
}

static bool file_content_id(const std::filesystem::path& path, ContentId& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<char> buf(1 << 20); // heap: a Windows thread stack is only 1 MiB
    while (f) {
        f.read(buf.data(), (std::streamsize)buf.size());
        const std::streamsize n = f.gcount();
        if (n > 0) out.add(buf.data(), (size_t)n);
    }
    return f.eof();
}

static bool armory_candidate(const std::string& name) {
    return name.find("common/hardware") != std::string::npos ||
           name.find("common/characters") != std::string::npos ||
           // GlacierFlow's authored hangar is not self-contained: its single
           // StaticModelGroup imports the shell, floor and set dressing from
           // the shared common/environment family.  Without those partition
           // GUIDs the member records decode correctly but every MeshAsset is
           // left as <not indexed>, silently dropping all 3,215 placements.
           // This remains a bounded front-end index over mounted shared TOCs;
           // it does not pull in any playable-level partition family.
           name.find("common/environment") != std::string::npos ||
           // Front-end actor presentation walks this one authored scene for
           // camera anchors, studio lights and nearby hangar geometry.  Keep
           // the complete scene family in the bounded index so those walks do
           // not fall back to indexing every playable-level partition.
           name.find("game/glacierflow/flow_mainmenu") != std::string::npos ||
           name.find("game/glacierflow/common") != std::string::npos ||
           // Hardware shader sheets are direct material dependencies of the
           // vehicle meshes above (for example the Abrams emissive/CA sheet).
           // Keep this narrower than all common/shaders so the bounded index
           // does not regress into the global every-partition scan.
           name.find("common/shaders/textures/hardware") != std::string::npos ||
           // Character materials also bind shared shader-owned sheets outside
           // common/characters.  The eye record, for example, puts its iris
           // mask, inner normal and fake reflection under this exact root.
           // Keeping the texture/character subtree bounded avoids the global
           // partition scan while making the complete live eye record usable.
           name.find("common/shaders/textures/character") != std::string::npos ||
           name.find("common/gameplay") != std::string::npos ||
           name.find("common/ui") != std::string::npos ||
           name.find("common/gamesetup/options") != std::string::npos ||
           name.find("common/gamesetup/gameconfigurations") != std::string::npos ||
           name.find("weapon") != std::string::npos ||
           name.find("projectile") != std::string::npos;
}

static bool valid_guid(const std::string& s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return false; }
        else if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    }
    return true;
}

static void put_u32(std::vector<uint8_t>& o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back((uint8_t)(v >> (i * 8)));
}
static void put_u64(std::vector<uint8_t>& o, uint64_t v) {
    for (int i = 0; i < 8; ++i) o.push_back((uint8_t)(v >> (i * 8)));
}
static bool get_u32(const std::vector<uint8_t>& d, size_t& p, size_t end, uint32_t& v) {
    if (p + 4 > end) return false; v = 0;
    for (int i = 0; i < 4; ++i) v |= (uint32_t)d[p++] << (i * 8);
    return true;
}
static bool get_u64(const std::vector<uint8_t>& d, size_t& p, size_t end, uint64_t& v) {
    if (p + 8 > end) return false; v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)d[p++] << (i * 8);
    return true;
}
static std::string env_value(const char* name) {
#ifdef _MSC_VER
    char* raw = nullptr; size_t size = 0;
    if (_dupenv_s(&raw, &size, name) != 0 || !raw) return std::string();
    const std::string value(raw); std::free(raw); return value;
#else
    const char* raw = std::getenv(name); return raw ? raw : "";
#endif
}
static bool cache_disabled() {
    const std::string v = env_value("BF6_DISABLE_ARMORY_INDEX_CACHE");
    return !v.empty() && v != "0";
}
static std::filesystem::path cache_dir() {
    const std::string override_dir = env_value("BF6_ARMORY_INDEX_CACHE_DIR");
    if (!override_dir.empty()) return override_dir;
    const std::string local = env_value("LOCALAPPDATA");
    if (!local.empty()) return std::filesystem::path(local) / "BF6HighPoly" / "Cache";
    return std::filesystem::temp_directory_path() / "BF6HighPoly" / "Cache";
}

} // namespace

static std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> out;
    FILE* f = fopen_binary_read(path.c_str());
    if (!f) return out;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) { out.resize((size_t)n); out.resize(std::fread(out.data(), 1, (size_t)n, f)); }
    std::fclose(f);
    return out;
}

bool Source::open(const std::string& game_dir, std::string& err) {
    game_ = game_dir;
    // The decompressor must be loaded before any CAS read - without it every
    // block decode silently returns empty.
    if (!oodle_open(game_dir)) { err = oodle_error(); return false; }
    return loc_.open(game_dir, err);
}

std::vector<uint8_t> Source::read_seg(const CasLoc& seg, bool allow_raw, std::string& err) {
    std::string path = loc_.cas_path(seg.chunk_id, seg.cas_ix);
    if (path.empty()) {
        err = "no cas file for install chunk";
        return std::vector<uint8_t>();
    }
    return cas_read(path, seg.off, seg.size, allow_raw, err);
}

static std::string toc_identity(const std::string& toc_path) {
    std::string identity = std::filesystem::path(toc_path).lexically_normal().generic_string();
#ifdef _WIN32
    std::transform(identity.begin(), identity.end(), identity.begin(),
        [](unsigned char ch) { return (char)std::tolower(ch); });
#endif
    // Keep the literal-path route solely for regression/performance comparisons.
    if (env_value("BF6_MOUNT_LITERAL_PATH") == "1") identity = toc_path;
    return identity;
}

bool Source::mount_toc(const std::string& toc_path, std::string& err) {
    // bf6_open and later targeted mounts share the same non-level TOCs. A
    // second parse cannot add anything under the first-wins law, so remember
    // successful paths and make repeated mount requests free.
    const std::string identity = toc_identity(toc_path);
    if (mounted_tocs_.find(identity) != mounted_tocs_.end()) return true;
    // Every attempt is part of the mount sequence, a failed one included: the
    // same sequence always leaves the same tables, which is what makes a
    // snapshot of them reusable.
    seq_add(toc_stamp(toc_path, identity));
    std::vector<uint8_t> raw = read_file(toc_path);
    if (raw.empty()) { err = "cannot read " + toc_path; return false; }

    // THE PARTITION INDEX IS BUILT FROM THE MOUNT, so a mount that grows after
    // it was built leaves it answering for a smaller world than exists. It only
    // ever MISSES - a guid it does not know reads as "no such partition" -
    // which is the worst shape a staleness bug can take: mounting a second
    // level and asking for its lighting returned "the level root imports no
    // outdoor preset" for a preset that is plainly imported. Marked stale here
    // and rebuilt on the next request, so the cost is only paid by a caller
    // that actually asks after mounting more.
    const size_t before = ebx_.size();


    Toc toc;
    if (!toc.parse(raw.data(), raw.size(), err)) return false;

    // Loose-chunk map, resolved while the toc body is resident.
    for (const TocChunk& c : toc.chunks) {
        chunks_.insert_if_absent(c.guid, toc.chunk_location(c));
    }

    const std::vector<uint8_t>& body = toc.body();
    for (const TocBundle& b : toc.bundles) {
        std::string e2;
        std::vector<CasLoc> segs = read_segments(body.data(), body.size(),
                                                 (size_t)b.offset, e2);
        if (segs.empty()) continue;
        std::vector<uint8_t> meta = read_seg(segs[0], true, e2);
        if (meta.empty()) continue;
        Payload pay;
        if (!pay.parse(meta.data(), meta.size(), e2)) continue;

        // Positional: entry i of ebx-then-res is segment i+1 (segment 0 is meta).
        size_t si = 1;
        size_t nseg = segs.size();
        const uint32_t bix = bundle_index(b.name);
        for (const auto& e : pay.ebx) {
            if (si < nseg) {
                EbxEntry ee; ee.loc = segs[si]; ee.dsize = e.second;
                ebx_.insert_if_absent(e.first, ee, bix);
            }
            si++;
        }
        for (const PayloadRes& r : pay.res) {
            /* COUNT THE LISTING, not the survivor. This runs before the dedup
             * test on purpose - it is the number data/res_types.tsv appears to
             * hold, and the whole point is to compare the two. */
            res_entry_type_[r.type]++;
            res_entries_total_++;
            ResEntry re; re.loc = si < nseg ? segs[si] : CasLoc(); re.dsize = r.size;
            re.type = r.type; re.rid = r.rid;
            if (si < nseg && res_.insert_if_absent(r.name, re, bix)) {

                // A depot names the bundle it covers, so the index is built
                // from the name rather than from a second pass.
                const size_t sh = r.name.find("_win32_shaderstate/shaderblockdepot_");
                if (sh != std::string::npos)
                    depot_by_bundle_.emplace(r.name.substr(0, sh), r.name);
            }
            si++;
        }
        // Bundle chunks come after res, and mesh vertex data lives in one.
        for (const std::string& cid : pay.chunk_id) {
            if (si < nseg) chunk_seg_.insert_if_absent(cid, segs[si]);
            si++;
        }
    }
    if (ebx_.size() != before) tables_grew();
    mounted_toc_hashes_[mount_key(toc_path)] = content_id(raw.data(), raw.size());
    mounted_tocs_.insert(identity);
    return true;
}

bool Source::mount_ebx_owner(const std::string& toc_path,
                             const std::string& bundle_name,
                             const std::string& ebx_name,
                             std::string& err)
{
    if (ebx_.lookup(ebx_name)) return true;
    const std::filesystem::path requested(toc_path);
    const std::string full = requested.is_absolute()
        ? requested.string()
        : (std::filesystem::path(game_) / requested).string();
    std::vector<uint8_t> raw = read_file(full);
    if (raw.empty()) { err = "cannot read " + full; return false; }

    Toc toc;
    if (!toc.parse(raw.data(), raw.size(), err)) return false;
    const std::vector<uint8_t>& body = toc.body();
    for (const TocBundle& b : toc.bundles)
    {
        if (b.name != bundle_name) continue;
        std::string e2;
        const std::vector<CasLoc> segs = read_segments(
            body.data(), body.size(), (size_t)b.offset, e2);
        if (segs.empty()) { err = e2; return false; }
        std::vector<uint8_t> meta = read_seg(segs[0], true, e2);
        if (meta.empty()) { err = e2; return false; }
        Payload pay;
        if (!pay.parse(meta.data(), meta.size(), e2)) { err = e2; return false; }
        size_t si = 1;
        for (const auto& e : pay.ebx)
        {
            if (e.first == ebx_name && si < segs.size())
            {
                EbxEntry ee; ee.loc = segs[si]; ee.dsize = e.second;
                ebx_.insert_if_absent(e.first, ee, bundle_index(b.name));
                seq_add("owner|" + toc_stamp(toc_path, toc_path) + "|" + b.name + "|" + e.first);
                tables_grew();
                pidx_built_ = false; pidx_.clear();
        light_names_built_ = false; light_names_.clear();
                armory_pidx_built_ = false; armory_pidx_.clear(); armory_pidx_candidates_.clear();
                mounted_toc_hashes_[mount_key(full)] = content_id(raw.data(), raw.size());
                return true;
            }
            ++si;
        }
        err = "bundle does not carry EBX " + ebx_name;
        return false;
    }
    err = "TOC does not carry bundle " + bundle_name;
    return false;
}

std::vector<uint8_t> Source::get_chunk(const std::string& guid_hex, std::string& err) {
    std::string g = guid_hex;
    for (char& ch : g) if (ch >= 'A' && ch <= 'Z') ch += 32;   // lower
    if (const CasLoc* at = chunks_.lookup(g)) return read_seg(*at, false, err);
    if (const CasLoc* at = chunk_seg_.lookup(g)) return read_seg(*at, false, err);
    err = "chunk " + g.substr(0, 16) + " is in no chunk map";
    return std::vector<uint8_t>();
}

bool Source::locate_res(const std::string& name, std::string& path, ResEntry& entry) const {
    const ResEntry* found = res_.lookup(name);
    if (!found) return false;
    entry = *found;
    path = loc_.cas_path(entry.loc.chunk_id, entry.loc.cas_ix);
    return !path.empty();
}

bool Source::locate_chunk(const std::string& guid_hex, std::string& path, CasLoc& loc) const {
    std::string g = guid_hex;
    for (char& ch : g) if (ch >= 'A' && ch <= 'Z') ch += 32;
    if (const CasLoc* at = chunks_.lookup(g)) loc = *at;
    else {
        const CasLoc* at2 = chunk_seg_.lookup(g);
        if (!at2) return false;
        loc = *at2;
    }
    path = loc_.cas_path(loc.chunk_id, loc.cas_ix);
    return !path.empty();
}

bool Source::has_chunk(const std::string& guid_hex) const {
    std::string g = guid_hex;
    for (char& ch : g) if (ch >= 'A' && ch <= 'Z') ch += 32;
    return chunks_.lookup(g) != nullptr || chunk_seg_.lookup(g) != nullptr;
}

std::vector<uint8_t> Source::get_res(const std::string& name, std::string& err) {
    const ResEntry* found = res_.lookup(name);
    if (!found) { err = "no res named " + name; return std::vector<uint8_t>(); }
    std::vector<uint8_t> d = read_seg(found->loc, false, err);
    if (d.size() != found->dsize) {
        char m[96];
        std::snprintf(m, sizeof(m), "res declared %u bytes, got %zu",
                      found->dsize, d.size());
        err = m;
        return std::vector<uint8_t>();
    }
    return d;
}

std::vector<uint8_t> Source::get_ebx(const std::string& name, std::string& err) {
    const EbxEntry* found = ebx_.lookup(name);
    if (!found) { err = "no ebx named " + name; return std::vector<uint8_t>(); }
    std::vector<uint8_t> d = read_seg(found->loc, false, err);
    if (d.size() != found->dsize) { err = "ebx size mismatch"; return std::vector<uint8_t>(); }
    return d;
}

const std::string& Source::bundle_of(const std::string& res_name) const
{
    static const std::string kEmpty;
    uint32_t b = ResTable::kNoBundle;
    return res_.lookup(res_name, &b) && b < bundles_.size() ? bundles_[b] : kEmpty;
}

const std::string& Source::bundle_of_ebx(const std::string& ebx_name) const
{
    static const std::string kEmpty;
    uint32_t b = EbxTable::kNoBundle;
    return ebx_.lookup(ebx_name, &b) && b < bundles_.size() ? bundles_[b] : kEmpty;
}

std::string Source::depot_for_bundle(const std::string& bundle) const
{
    if (bundle.empty()) return std::string();

    // A TOC SPELLS A BUNDLE "win32/game/..." AND A DEPOT SPELLS IT "game/...".
    // Depot resources are named after the bundle's ASSET path, which carries no
    // platform prefix. One token apart, and the lookup misses every time
    // without saying so.
    std::string b = bundle;
    if (b.rfind("win32/", 0) == 0) b = b.substr(6);

    auto hit = depot_by_bundle_.find(b);
    if (hit != depot_by_bundle_.end()) return hit->second;

    // ANCESTORS ONLY. Bundle paths nest by directory and a container's own
    // bundle repeats its directory name, so ".../mp_dumbo/sub_art_10_oob"
    // widens to ".../mp_dumbo/mp_dumbo". Never sideways: see the header.
    for (;;)
    {
        const size_t slash = b.find_last_of('/');
        if (slash == std::string::npos) break;
        b = b.substr(0, slash);
        const size_t leaf = b.find_last_of('/');
        const std::string cand = leaf == std::string::npos
            ? b + "/" + b : b + "/" + b.substr(leaf + 1);
        hit = depot_by_bundle_.find(cand);
        if (hit != depot_by_bundle_.end()) return hit->second;
        hit = depot_by_bundle_.find(b);
        if (hit != depot_by_bundle_.end()) return hit->second;
    }
    return std::string();
}

std::string Source::depot_for_res(const std::string& res_name) const
{
    return depot_for_bundle(bundle_of(res_name));
}

// ---------------------------------------------------------------------------
// Partition index
// ---------------------------------------------------------------------------

// A partition's own GUID, out of its EFIX fixup.
//
// Deliberately NOT done by handing the bytes to the deserializer: this is a
// header read of every partition in the mount, and the guid sits in a known
// place. The formatting MUST match the one the EBX reader produces, because
// this index is looked up with the keys that reader hands out: .NET mixed
// endian, first three groups little-endian and the last eight bytes as they lie.
static std::string format_partition_guid(const uint8_t* g)
{
    char buf[40];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        (unsigned)(g[0] | (g[1] << 8) | (g[2] << 16) | ((unsigned)g[3] << 24)),
        (unsigned)(g[4] | (g[5] << 8)), (unsigned)(g[6] | (g[7] << 8)),
        g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return buf;
}

std::string partition_guid_from_bytes(const std::vector<uint8_t>& raw)
{
    if (raw.size() < 12 || std::memcmp(raw.data(), "RIFF", 4) != 0) return std::string();
    size_t o = 12;
    while (o + 8 <= raw.size())
    {
        uint32_t sz = 0;
        std::memcpy(&sz, raw.data() + o + 4, 4);
        if (std::memcmp(raw.data() + o, "EFIX", 4) == 0)
        {
            const size_t s = o + 8;
            if (s + 16 > raw.size()) return std::string();
            return format_partition_guid(raw.data() + s);
        }
        o += 8 + (size_t)sz;
        if (o % 2 == 1) o++;
    }
    return std::string();
}

// The same walk, asking the reader for exactly the bytes the loop above looks
// at. Every "o + n > raw.size()" test above becomes a refused read here, and
// a refused read (end of record OR a block that fails) returns empty exactly as
// a full read that failed would have. Chunk payloads between the headers are
// never requested, so blocks lying wholly inside them are never decoded.
std::string partition_guid_from_reader(CasRangeReader& reader)
{
    std::string e;
    uint8_t head[12];
    if (!reader.read(0, sizeof(head), head, e) || std::memcmp(head, "RIFF", 4) != 0)
        return std::string();
    uint64_t o = 12;
    for (;;)
    {
        uint8_t ch[8];
        if (!reader.read(o, sizeof(ch), ch, e)) return std::string();
        uint32_t sz = 0;
        std::memcpy(&sz, ch + 4, 4);
        if (std::memcmp(ch, "EFIX", 4) == 0)
        {
            uint8_t g[16];
            if (!reader.read(o + 8, sizeof(g), g, e)) return std::string();
            return format_partition_guid(g);
        }
        o += 8 + (uint64_t)sz;
        if (o % 2 == 1) o++;
    }
}

bool Source::partition_index_full_read() const
{
    if (pidx_full_override_ >= 0) return pidx_full_override_ != 0;
    const std::string v = env_value("BF6_PARTITION_INDEX_FULL_READ");
    return !v.empty() && v != "0";
}

void Source::reset_partition_indexes()
{
    pidx_built_ = false; pidx_.clear();
        light_names_built_ = false; light_names_.clear();
    armory_pidx_built_ = false; armory_pidx_.clear(); armory_pidx_candidates_.clear();
    pidx_stats_ = PartitionIndexStats();
    armory_pidx_stats_ = PartitionIndexStats();
}

std::string Source::partition_guid(const CasLoc& loc, bool full_read, CasReadStats* stats)
{
    std::string e;
    if (full_read)
    {
        // The original route, byte for byte: read_seg without the raw fallback.
        const std::string path = loc_.cas_path(loc.chunk_id, loc.cas_ix);
        if (path.empty()) return std::string();
        std::vector<uint8_t> bytes = cas_read(path, loc.off, loc.size, false, e, stats);
        return bytes.empty() ? std::string() : partition_guid_from_bytes(bytes);
    }
    const std::string path = loc_.cas_path(loc.chunk_id, loc.cas_ix);
    if (path.empty()) return std::string();
    CasRangeReader reader;
    if (!reader.open(path, loc.off, loc.size, false, e, stats)) return std::string();
    return partition_guid_from_reader(reader);
}

// READ IN PARALLEL, PUBLISH IN ORDER - the shared read phase of both indexes.
// Each worker owns its counters; they are summed after the join, so the
// statistics need neither a lock nor an atomic per byte.
void Source::read_partition_guids(const PartitionOrder& order, const char* label,
                                  std::vector<std::string>& found, PartitionIndexStats& st)
{
    const size_t n = order.size();
    found.assign(n, std::string());
    st = PartitionIndexStats();
    st.built = true;
    st.full_read = partition_index_full_read();
    st.partitions = n;
    const bool full = st.full_read;

    const unsigned hw = std::thread::hardware_concurrency();
    const size_t workers = std::max<size_t>(1, std::min<size_t>(hw ? hw : 4, 16));
    std::vector<CasReadStats> per_worker(workers);
    std::atomic<size_t> next{0};
    std::atomic<bool> cancelled{false};
    auto worker = [&](size_t w)
    {
        for (;;)
        {
            const size_t i = next.fetch_add(1);
            if (i >= n) return;
            // Every few hundred, from whichever worker got there. The callback
            // is documented as concurrent for exactly this.
            if (progress_ && (i & 511) == 0 && !progress_(label, (int)i, (int)n))
            {
                cancelled = true;
                return;
            }
            // Safe to run concurrently: cas_path only reads the locator, and
            // the CAS readers use per-thread handles. Nothing here touches
            // Source state.
            found[i] = partition_guid(order[i].loc, full, &per_worker[w]);
        }
    };

    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (size_t w = 0; w < workers; w++) pool.emplace_back(worker, w);
    for (std::thread& t : pool) t.join();
    st.read_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    for (const CasReadStats& s : per_worker) st.io.add(s);
    st.cancelled = cancelled;
    for (const std::string& g : found) if (!g.empty()) st.guids++;
}

const std::unordered_map<std::string, std::string>& Source::light_name_index()
{
    if (light_names_built_) return light_names_;
    for (const auto& kv : ebx_) {
        std::string low = kv.first;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        const std::string ref = kv.first + ".ebx";
        light_names_.emplace(low, ref);
        const size_t slash = low.find_last_of('/');
        light_names_.emplace(slash == std::string::npos ? low : low.substr(slash + 1), ref);
    }
    light_names_built_ = true;
    return light_names_;
}

const std::map<std::string, std::string>& Source::partition_index()
{
    if (pidx_built_) return pidx_;
    pidx_built_ = true;

    // STORED BESIDE THE MOUNT SNAPSHOT. The index is a pure function of the
    // mounted tables and the read mode, so when every table is a snapshot
    // layer its key is the snapshot's key and the index is read back rather
    // than rebuilt from a read of every partition.
    const std::string side = snapshot_side_path(partition_index_full_read() ? "pidx-full" : "pidx");
    if (!side.empty() && load_partition_index(side)) return pidx_;

    // CAS LOCALITY ORDER, not name order. Every partition in the mount is read
    // to get one 16-byte header, so the reads want to run down each archive in
    // the order the blocks lie rather than jumping the disk per name.
    PartitionOrder order;
    order.reserve(ebx_.size());
    for (const auto& kv : ebx_) order.push_back({kv.first, kv.second.loc});
    std::sort(order.begin(), order.end(),
        [](const PartitionRef& a, const PartitionRef& b)
        {
            if (a.loc.chunk_id != b.loc.chunk_id) return a.loc.chunk_id < b.loc.chunk_id;
            if (a.loc.cas_ix != b.loc.cas_ix) return a.loc.cas_ix < b.loc.cas_ix;
            if (a.loc.off != b.loc.off) return a.loc.off < b.loc.off;
            // SAME BYTES UNDER TWO NAMES. An asset shipped at two paths shares
            // one partition guid, so "first name wins" is decided by whatever
            // order the sort happened to leave them in - which is not an order
            // at all when the locations are equal. Broken by name so this index
            // is at least the same on every run. Note the Godot plugin has no
            // such tie-break, so its pick depends on dictionary order and the
            // two readers can legitimately name the same partition differently.
            return a.name < b.name;
        });

    // READ IN PARALLEL, PUBLISH IN ORDER.
    //
    // This reads every partition in the mount for one 16-byte header, which is
    // the single biggest cost of opening a level - about 19 seconds on mp_dumbo
    // against 4 for the mount and 2.5 for the walk. It is also embarrassingly
    // parallel: each read is independent, and the only shared thing is the
    // result.
    //
    // The GUIDS ARE COLLECTED INTO A SLOT PER PARTITION and folded in afterwards
    // in the original order, NOT inserted from the workers. "First name wins"
    // is the rule that makes this index stable across runs, and a map written
    // from several threads would resolve ties by whichever thread got there
    // first - which is no rule at all.
    const size_t n = order.size();
    std::vector<std::string> found;
    read_partition_guids(order, "indexing partitions", found, pidx_stats_);

    for (size_t i = 0; i < n; i++)
        if (!found[i].empty() && !pidx_.emplace(found[i], order[i].name + ".ebx").second)
            pidx_stats_.duplicates++;
    if (!side.empty() && !pidx_stats_.cancelled) save_partition_index(side);
    return pidx_;
}

const std::map<std::string, std::string>& Source::armory_partition_index()
{
    if (armory_pidx_built_) return armory_pidx_;
    armory_pidx_built_ = true;

    // This is intentionally a broad runtime selector. The full-index control
    // resolves 931/931 model-definition mesh imports for all 185 roster rows
    // inside these families (shuffled/non-family control: 0 outside). Keeping
    // whole authored roots also covers gadgets and future names that do not
    // happen to contain the word "weapon".
    PartitionOrder order;
    order.reserve(ebx_.size() / 4);
    for (const auto& kv : ebx_) if (armory_candidate(kv.first)) order.push_back({kv.first, kv.second.loc});
    std::sort(order.begin(), order.end(),
        [](const PartitionRef& a, const PartitionRef& b)
        {
            if (a.loc.chunk_id != b.loc.chunk_id) return a.loc.chunk_id < b.loc.chunk_id;
            if (a.loc.cas_ix != b.loc.cas_ix) return a.loc.cas_ix < b.loc.cas_ix;
            if (a.loc.off != b.loc.off) return a.loc.off < b.loc.off;
            return a.name < b.name;
        });

    // A cache is useful only when its identity was computed from the current
    // executable and every raw TOC in this exact mount. It stores no asset
    // payloads and is never authoritative: every path is revalidated against
    // ebx_ and the live family selector before it can be published.
    ContentId install;
    static const char schema[] =
        "bf6-armory-partition-index-v8-character-shader-textures";
    install.add(schema, sizeof(schema) - 1);
    const std::filesystem::path exe = std::filesystem::path(game_) / "bf6.exe";
    const bool have_exe = file_content_id(exe, install);
    for (const auto& kv : mounted_toc_hashes_) {
        install.add(kv.first);
        install.add(&kv.second.first, sizeof(kv.second.first));
        install.add(&kv.second.second, sizeof(kv.second.second));
    }
    const size_t candidate_count = order.size();
    install.add(&candidate_count, sizeof(candidate_count));
    std::ostringstream key;
    key << std::hex << std::setfill('0') << std::setw(16) << install.a
        << std::setw(16) << install.b;
    const std::filesystem::path cache = cache_dir() / ("armory-pidx-" + key.str() + ".bin");

    if (!cache_disabled() && have_exe && !mounted_toc_hashes_.empty()) {
        const std::vector<uint8_t> raw = read_file(cache.string());
        const size_t trailer = 16;
        size_t p = 0;
        bool ok = raw.size() >= 8 + 4 + 16 + 4 + trailer && raw.size() <= 32u * 1024u * 1024u;
        static const char magic[8] = {'B','F','6','A','P','I','D','X'};
        if (ok && std::memcmp(raw.data(), magic, 8) != 0) ok = false;
        p = 8;
        uint32_t version = 0, count = 0; uint64_t ia = 0, ib = 0, ca = 0, cb = 0;
        if (ok) ok = get_u32(raw, p, raw.size() - trailer, version) && version == 2 &&
                     get_u64(raw, p, raw.size() - trailer, ia) &&
                     get_u64(raw, p, raw.size() - trailer, ib) &&
                     ia == install.a && ib == install.b &&
                     get_u32(raw, p, raw.size() - trailer, count) && count <= order.size();
        if (ok) {
            size_t q = raw.size() - trailer;
            ok = get_u64(raw, q, raw.size(), ca) && get_u64(raw, q, raw.size(), cb);
            const auto actual = content_id(raw.data(), raw.size() - trailer);
            ok = ok && ca == actual.first && cb == actual.second;
        }
        std::map<std::string, std::string> first;
        std::map<std::string, std::vector<std::string>> all;
        for (uint32_t i = 0; ok && i < count; ++i) {
            uint32_t gl = 0, pl = 0;
            ok = get_u32(raw, p, raw.size() - trailer, gl) && gl == 36 && p + gl <= raw.size() - trailer;
            std::string guid;
            if (ok) { guid.assign((const char*)raw.data() + p, gl); p += gl; }
            ok = ok && get_u32(raw, p, raw.size() - trailer, pl) && pl > 4 && pl <= 4096 && p + pl <= raw.size() - trailer;
            std::string path;
            if (ok) { path.assign((const char*)raw.data() + p, pl); p += pl; }
            if (ok) {
                const bool suffix = path.size() > 4 && path.compare(path.size() - 4, 4, ".ebx") == 0;
                const std::string base = suffix ? path.substr(0, path.size() - 4) : std::string();
                ok = valid_guid(guid) && suffix && armory_candidate(base) && ebx_.lookup(base) != nullptr;
                if (ok) { first.emplace(guid, path); all[guid].push_back(path); }
            }
        }
        ok = ok && p == raw.size() - trailer && !first.empty();
        if (ok) {
            armory_pidx_.swap(first); armory_pidx_candidates_.swap(all);
            armory_pidx_stats_ = PartitionIndexStats();
            armory_pidx_stats_.built = armory_pidx_stats_.from_cache = true;
            armory_pidx_stats_.full_read = partition_index_full_read();
            armory_pidx_stats_.partitions = order.size();
            armory_pidx_stats_.guids = count;
            std::fprintf(stderr, "bf6 armory index cache hit: %zu candidates (%s)\n",
                         (size_t)count, key.str().c_str());
            return armory_pidx_;
        }
        if (!raw.empty()) std::fprintf(stderr, "bf6 armory index cache rejected; rebuilding from current install\n");
    }

    const size_t n = order.size();
    std::vector<std::string> found;
    read_partition_guids(order, "indexing armory partitions", found, armory_pidx_stats_);
    for (size_t i = 0; i < n; i++)
        if (!found[i].empty())
        {
            const std::string path = order[i].name + ".ebx";
            if (!armory_pidx_.emplace(found[i], path).second) armory_pidx_stats_.duplicates++;
            armory_pidx_candidates_[found[i]].push_back(path);
        }

    if (!cache_disabled() && have_exe && !mounted_toc_hashes_.empty() && !armory_pidx_.empty()) {
        std::vector<uint8_t> raw;
        static const char magic[8] = {'B','F','6','A','P','I','D','X'};
        raw.insert(raw.end(), magic, magic + 8); put_u32(raw, 2);
        put_u64(raw, install.a); put_u64(raw, install.b);
        uint32_t count = 0;
        for (const auto& kv : armory_pidx_candidates_) count += (uint32_t)kv.second.size();
        put_u32(raw, count);
        for (const auto& kv : armory_pidx_candidates_) for (const std::string& path : kv.second) {
            put_u32(raw, (uint32_t)kv.first.size()); raw.insert(raw.end(), kv.first.begin(), kv.first.end());
            put_u32(raw, (uint32_t)path.size()); raw.insert(raw.end(), path.begin(), path.end());
        }
        const auto sum = content_id(raw.data(), raw.size()); put_u64(raw, sum.first); put_u64(raw, sum.second);
        std::error_code ec; std::filesystem::create_directories(cache.parent_path(), ec);
        const auto nonce = (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
        const std::filesystem::path tmp = cache.string() + "." + std::to_string(nonce) + ".tmp";
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (f) { f.write((const char*)raw.data(), (std::streamsize)raw.size()); f.close();
            if (f) { std::filesystem::rename(tmp, cache, ec); if (ec && std::filesystem::exists(cache, ec)) std::filesystem::remove(tmp, ec); }
        }
    }
    return armory_pidx_;
}

const std::map<std::string, std::vector<std::string>>& Source::armory_partition_candidates()
{
    armory_partition_index();
    return armory_pidx_candidates_;
}

// ---------------------------------------------------------------------------
// Finding and mounting a level's archives
// ---------------------------------------------------------------------------

namespace {

std::string lower_slash(const std::string& s)
{
    std::string o = s;
    for (char& c : o)
    {
        if (c == '\\') c = '/';
        else c = (char)std::tolower((unsigned char)c);
    }
    return o;
}

// The SDK names a scene by display name while the game files the level under an
// mp_ id (Portal_Sand -> levels/mp_portal_sand), so both spellings match.
std::vector<std::string> level_dirs(const std::string& level)
{
    std::string l = lower_slash(level);
    std::vector<std::string> out{ "/levels/" + l + "/" };
    if (l.rfind("mp_", 0) != 0) out.push_back("/levels/mp_" + l + "/");
    return out;
}

bool in_level_dir(const std::string& path, const std::vector<std::string>& dirs)
{
    const std::string p = lower_slash(path);
    for (const std::string& d : dirs)
        if (p.find(d) != std::string::npos) return true;
    return false;
}

}  // namespace

bool Source::is_level_toc(const std::string& path)
{
    return lower_slash(path).find("/levels/") != std::string::npos;
}

std::string Source::mount_key(const std::string& path)
{
    const std::string low = lower_slash(path);
    return (low.find("/update/") != std::string::npos ? "1" : "0") + low;
}

std::vector<std::string> Source::available_levels() const
{
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(game_, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec))
    {
        if (ec) { ec.clear(); continue; }
        if (!it->is_directory(ec)) continue;
        const std::string here = lower_slash(it->path().string());
        if (here.size() >= 7 && here.compare(here.size() - 7, 7, "/levels") == 0)
        {
            std::error_code e2;
            for (const auto& sub : fs::directory_iterator(it->path(), e2))
                if (sub.is_directory(e2)) out.push_back(lower_slash(sub.path().filename().string()));
            it.disable_recursion_pending();   // the level dirs need no descent
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// THE ORDER IS THE CORRECTNESS, not a tidy-up. Mounting is FIRST WINS: the
// sweep keeps the first entry it sees for a name and skips the rest.
//
// Shared archives therefore go first, so a level cannot displace a global it
// depends on. Among LEVELS the same rule reads backwards from how it sounds:
// the level being read has to come FIRST, or every other level outranks it for
// any name they share, and levels share names freely - the shader-state depots,
// the terrain resources, the section keys. Sorted purely by path, mp_dumbo
// lands wherever the alphabet puts it and the level you are reading resolves
// against another level's data.
//
// So: shared archives, then this level, then everything else purely to make its
// objects reachable.
std::vector<std::string> Source::find_tocs(const std::string& level, bool all_levels) const
{
    namespace fs = std::filesystem;
    std::vector<std::string> shared, lvl;
    const std::vector<std::string> want = level_dirs(level);

    std::error_code ec;
    for (fs::recursive_directory_iterator it(game_, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec))
    {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const std::string p = it->path().string();
        if (p.size() < 4 || lower_slash(p).compare(p.size() - 4, 4, ".toc") != 0) continue;
        if (is_level_toc(p))
        {
            if (all_levels || (!level.empty() && in_level_dir(p, want))) lvl.push_back(p);
        }
        else shared.push_back(p);
    }

    auto by_key = [](const std::string& a, const std::string& b)
    { return mount_key(a) < mount_key(b); };
    std::sort(shared.begin(), shared.end(), by_key);
    std::sort(lvl.begin(), lvl.end(), by_key);

    if (all_levels && !level.empty())
    {
        std::vector<std::string> mine, others;
        for (const std::string& p : lvl)
            (in_level_dir(p, want) ? mine : others).push_back(p);
        lvl = mine;
        lvl.insert(lvl.end(), others.begin(), others.end());
    }

    std::vector<std::string> out = shared;
    out.insert(out.end(), lvl.begin(), lvl.end());
    return out;
}

bool Source::mount_level(const std::string& level, bool all_levels, std::string& err)
{
    const std::vector<std::string> tocs = find_tocs(level, all_levels);
    if (tocs.empty()) { err = "no .toc found under " + game_; return false; }
    // A toc that will not mount is not fatal on its own: the install carries
    // archives this reader has no business in. An empty mount is.
    //
    // In two calls, the shared archives and then the level's own, in the same
    // order as one list: every level shares the first snapshot layer, and each
    // level stores only what it adds.
    size_t split = 0;
    while (split < tocs.size() && !is_level_toc(tocs[split])) ++split;
    const std::vector<std::string> shared(tocs.begin(), tocs.begin() + (std::ptrdiff_t)split);
    const std::vector<std::string> own(tocs.begin() + (std::ptrdiff_t)split, tocs.end());
    size_t mounted = 0, mounted_own = 0;
    if (!mount_tocs(shared, "mounting the level's archives", mounted, err)) return false;
    if (!own.empty() && !mount_tocs(own, "mounting the level's archives", mounted_own, err)) return false;
    mounted += mounted_own;
    if (mounted == 0) { err = "no .toc mounted"; return false; }
    if (!level.empty())
    {
        bool any_level_toc = false;
        for (const std::string& t : tocs)
            if (is_level_toc(t) && in_level_dir(t, level_dirs(level))) { any_level_toc = true; break; }
        if (!any_level_toc)
        {
            err = "no archives for level '" + level + "'";
            return false;
        }
    }
    return true;
}

bool Source::mount_frontend(std::string& err)
{
    const std::vector<std::string> all = find_tocs(std::string(), false);
    if (all.empty()) { err = "no shared .toc found under " + game_; return false; }

    auto ends = [](const std::string& value, const char* suffix) {
        const size_t n = std::strlen(suffix);
        return value.size() >= n && value.compare(value.size() - n, n, suffix) == 0;
    };
    std::vector<std::string> selected;
    for (const std::string& toc : all)
    {
        const std::string low = lower_slash(toc);
        const bool contentFamily =
            ends(low, "/characters.toc") || ends(low, "/globals.toc") ||
            ends(low, "/ui.toc") || ends(low, "/vehicles.toc") ||
            ends(low, "/weapons.toc");
        const bool mainMenu =
            low.find("/game/glacierflow/flow_mainmenu/") != std::string::npos;
        const bool englishText = ends(low, "/loc/en.toc");
        if (contentFamily || mainMenu || englishText) selected.push_back(toc);
    }
    if (selected.empty()) { err = "no front-end archive families found"; return false; }

    size_t mounted = 0;
    if (!mount_tocs(selected, "mounting front-end archives", mounted, err)) return false;
    if (!mounted) { err = "no front-end archive mounted"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// The mount snapshot
// ---------------------------------------------------------------------------

uint32_t Source::bundle_index(const std::string& name)
{
    auto it = bundle_ix_.find(name);
    if (it != bundle_ix_.end()) return it->second;
    const uint32_t ix = (uint32_t)bundles_.size();
    bundles_.push_back(name);
    bundle_ix_.emplace(name, ix);
    return ix;
}

void Source::seq_add(const std::string& item)
{
    ContentId h; h.a = seq_a_; h.b = seq_b_;
    const uint32_t n = (uint32_t)item.size();
    h.add(&n, sizeof(n));
    h.add(item);
    seq_a_ = h.a; seq_b_ = h.b;
}

std::string Source::toc_stamp(const std::string& toc_path, const std::string& identity) const
{
    std::error_code ec;
    const uint64_t size = (uint64_t)std::filesystem::file_size(toc_path, ec);
    const auto when = std::filesystem::last_write_time(toc_path, ec);
    const int64_t ticks = ec ? 0 : (int64_t)when.time_since_epoch().count();
    return identity + "|" + std::to_string(ec ? 0 : size) + "|" + std::to_string(ticks);
}

void Source::tables_grew()
{
    pidx_built_ = false; pidx_.clear();
    light_names_built_ = false; light_names_.clear();
    armory_pidx_built_ = false; armory_pidx_.clear(); armory_pidx_candidates_.clear();
}

bool Source::snapshots_enabled() const
{
    if (snap_override_ >= 0) return snap_override_ == 1;
    const std::string v = env_value("BF6_DISABLE_MOUNT_SNAPSHOT");
    return v.empty() || v == "0";
}

namespace {

// Format 1. The record sizes are part of the header, so a build whose entry
// layout differs rejects the file instead of misreading it.
constexpr char     kSnapMagic[8] = {'B','F','6','M','N','T','S','1'};
constexpr uint32_t kSnapVersion = 2;
constexpr uint32_t kSnapSections = 11;
enum SnapSection : uint32_t {
    S_RES_RECS, S_RES_SLOTS, S_EBX_RECS, S_EBX_SLOTS, S_CHUNK_RECS, S_CHUNK_SLOTS,
    S_SEG_RECS, S_SEG_SLOTS, S_NAMES, S_STATE,
    S_STAMPS     // the TOC stamps this layer mounted, for telling a stale chain
};
struct SnapHeader {
    char     magic[8];
    uint32_t version;
    uint32_t sections;
    uint32_t rec_sizes[4];
    uint64_t key_a, key_b, parent_a, parent_b;
    uint64_t off[kSnapSections];
    uint64_t size[kSnapSections];
};

uint32_t snap_rec_size(uint32_t i)
{
    switch (i) {
    case 0: return (uint32_t)sizeof(ResTable::Rec);
    case 1: return (uint32_t)sizeof(EbxTable::Rec);
    default: return (uint32_t)sizeof(ChunkTable::Rec);
    }
}

std::string hex64(uint64_t v)
{
    char b[17];
    std::snprintf(b, sizeof(b), "%016llx", (unsigned long long)v);
    return b;
}

void put_str(std::vector<uint8_t>& o, const std::string& s)
{
    put_u32(o, (uint32_t)s.size());
    o.insert(o.end(), s.begin(), s.end());
}
bool get_str(const uint8_t* d, size_t& p, size_t end, std::string& s)
{
    if (p + 4 > end) return false;
    uint32_t n = 0; std::memcpy(&n, d + p, 4); p += 4;
    if (p + n > end) return false;
    s.assign((const char*)d + p, n); p += n;
    return true;
}
bool get_raw64(const uint8_t* d, size_t& p, size_t end, uint64_t& v)
{
    if (p + 8 > end) return false;
    std::memcpy(&v, d + p, 8); p += 8;
    return true;
}
bool get_raw32(const uint8_t* d, size_t& p, size_t end, uint32_t& v)
{
    if (p + 4 > end) return false;
    std::memcpy(&v, d + p, 4); p += 4;
    return true;
}

template <class T>
bool map_layer(const MappedFile& f, const SnapHeader& h, SnapSection recs, SnapSection slots,
               typename T::Layer& out)
{
    const uint64_t rs = sizeof(typename T::Rec);
    if (h.size[recs] % rs || h.size[slots] % 4 || h.size[slots] < 4) return false;
    if (h.off[recs] % alignof(typename T::Rec) || h.off[slots] % 4) return false;
    out.recs = (const typename T::Rec*)(void*)(f.data() + h.off[recs]);
    out.count = (uint32_t)(h.size[recs] / rs);
    out.slots = (const uint32_t*)(void*)(f.data() + h.off[slots]);
    out.mask = (uint32_t)(h.size[slots] / 4 - 1);
    out.names = (const char*)f.data() + h.off[S_NAMES];
    out.names_size = (size_t)h.size[S_NAMES];
    return T::valid(out);
}

// A hit marks a file used, so the size cap keeps what is actually opened.
void touch(const std::string& path)
{
    std::error_code ec;
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), ec);
}

// SNAPSHOTS ARE BOUNDED PER INSTALLATION. A game patch changes every key and
// the first-layer sweep removes the old chain; this catches the rest (levels
// opened in unusual sequences, tools mounting other archive sets). Least
// recently used first, never a file named in `keep`.
void enforce_snapshot_cap(const std::filesystem::path& dir, const std::string& root,
                          const std::vector<std::string>& keep)
{
    constexpr uint64_t kCap = 2ull << 30;
    struct F { std::filesystem::path p; uint64_t size; std::filesystem::file_time_type t; };
    std::vector<F> files;
    uint64_t total = 0;
    std::error_code ec;
    const std::string prefix = root + "-";
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string fn = e.path().filename().string();
        if (fn.compare(0, prefix.size(), prefix) != 0) continue;
        std::error_code fe;
        F f{e.path(), (uint64_t)e.file_size(fe), e.last_write_time(fe)};
        if (fe) continue;
        total += f.size;
        files.push_back(std::move(f));
    }
    if (total <= kCap) return;
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.t < b.t; });
    for (const F& f : files) {
        if (total <= kCap) break;
        const std::string s = f.p.string();
        if (std::find(keep.begin(), keep.end(), s) != keep.end()) continue;
        std::error_code rm;
        if (std::filesystem::remove(f.p, rm)) total -= f.size;
    }
}

std::string snapshot_root(const std::string& game)
{
    std::string low = std::filesystem::path(game).lexically_normal().generic_string();
    std::transform(low.begin(), low.end(), low.begin(),
        [](unsigned char ch) { return (char)std::tolower(ch); });
    while (!low.empty() && low.back() == '/') low.pop_back();
    const auto id = content_id(low.data(), low.size());
    return hex64(id.first);
}

} // namespace

std::string Source::snapshot_side_path(const char* tag) const
{
    if (!snapshots_enabled() || snap_base_.empty() || res_.layers().empty()) return std::string();
    if (!res_.overlay().empty() || !ebx_.overlay().empty() ||
        !chunks_.overlay().empty() || !chunk_seg_.overlay().empty()) return std::string();
    return (cache_dir() / "mount" / (snapshot_root(game_) + "-" + snap_base_.substr(0, 16) + "-" +
            hex64(seq_a_) + hex64(seq_b_) + "-" + tag + ".bin")).string();
}

namespace {
constexpr char kPidxMagic[8] = {'B','F','6','P','I','D','X','1'};
}

bool Source::load_partition_index(const std::string& path)
{
    std::shared_ptr<MappedFile> f = MappedFile::open(path);
    if (!f) return false;
    const uint8_t* d = f->data();
    const size_t end = f->size();
    size_t p = 8;
    uint64_t a = 0, b = 0, partitions = 0, duplicates = 0;
    uint32_t count = 0;
    bool ok = end >= 8 && std::memcmp(d, kPidxMagic, 8) == 0 &&
              get_raw64(d, p, end, a) && get_raw64(d, p, end, b) && a == seq_a_ && b == seq_b_ &&
              get_raw64(d, p, end, partitions) && get_raw64(d, p, end, duplicates) &&
              get_raw32(d, p, end, count) && partitions == ebx_.size() && count <= partitions;
    std::map<std::string, std::string> index;
    std::string guid, name;
    auto hint = index.end();
    for (uint32_t i = 0; ok && i < count; ++i) {
        ok = get_str(d, p, end, guid) && get_str(d, p, end, name) && valid_guid(guid);
        if (ok) hint = index.emplace_hint(hint, guid, name);    // written in key order
    }
    ok = ok && p == end && index.size() == count;
    if (!ok) return false;
    pidx_.swap(index);
    touch(path);
    pidx_stats_ = PartitionIndexStats();
    pidx_stats_.built = pidx_stats_.from_cache = true;
    pidx_stats_.full_read = partition_index_full_read();
    pidx_stats_.partitions = partitions;
    pidx_stats_.guids = count + duplicates;
    pidx_stats_.duplicates = duplicates;
    return true;
}

void Source::save_partition_index(const std::string& path) const
{
    std::vector<uint8_t> raw(kPidxMagic, kPidxMagic + 8);
    put_u64(raw, seq_a_); put_u64(raw, seq_b_);
    put_u64(raw, pidx_stats_.partitions); put_u64(raw, pidx_stats_.duplicates);
    put_u32(raw, (uint32_t)pidx_.size());
    for (const auto& kv : pidx_) { put_str(raw, kv.first); put_str(raw, kv.second); }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    const auto nonce = (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string tmp = path + "." + std::to_string(nonce) + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return;
        o.write((const char*)raw.data(), (std::streamsize)raw.size());
        o.close();
        if (!o) { std::filesystem::remove(tmp, ec); return; }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}

bool Source::load_snapshot(const std::string& path, uint64_t parent_a, uint64_t parent_b,
                           uint64_t key_a, uint64_t key_b, std::string& why)
{
    std::shared_ptr<MappedFile> f = MappedFile::open(path);
    if (!f) { why = "no snapshot"; return false; }
    if (f->size() < sizeof(SnapHeader)) { why = "snapshot too small"; return false; }
    SnapHeader h;
    std::memcpy(&h, f->data(), sizeof(h));
    if (std::memcmp(h.magic, kSnapMagic, 8) != 0 || h.version != kSnapVersion ||
        h.sections != kSnapSections) { why = "snapshot format differs"; return false; }
    for (uint32_t i = 0; i < 3; ++i)
        if (h.rec_sizes[i] != snap_rec_size(i)) { why = "snapshot record layout differs"; return false; }
    if (h.key_a != key_a || h.key_b != key_b || h.parent_a != parent_a || h.parent_b != parent_b)
    { why = "snapshot key differs"; return false; }
    for (uint32_t i = 0; i < kSnapSections; ++i)
        if (h.off[i] > f->size() || h.size[i] > f->size() - h.off[i])
        { why = "snapshot section out of bounds"; return false; }

    ResTable::Layer rl; EbxTable::Layer el; ChunkTable::Layer cl, sl;
    if (!map_layer<ResTable>(*f, h, S_RES_RECS, S_RES_SLOTS, rl) ||
        !map_layer<EbxTable>(*f, h, S_EBX_RECS, S_EBX_SLOTS, el) ||
        !map_layer<ChunkTable>(*f, h, S_CHUNK_RECS, S_CHUNK_SLOTS, cl) ||
        !map_layer<ChunkTable>(*f, h, S_SEG_RECS, S_SEG_SLOTS, sl))
    { why = "snapshot table is corrupt"; return false; }

    // The small state is stored whole: it replaces what is here.
    const uint8_t* d = f->data();
    size_t p = (size_t)h.off[S_STATE];
    const size_t end = p + (size_t)h.size[S_STATE];
    std::unordered_set<std::string> tocs;
    std::map<std::string, std::pair<uint64_t, uint64_t>> hashes;
    std::map<uint32_t, uint64_t> types;
    std::map<std::string, std::string> depots;
    std::vector<std::string> bundles;
    uint64_t total = 0;
    uint32_t n = 0;
    bool ok = get_raw32(d, p, end, n);
    for (uint32_t i = 0; ok && i < n; ++i) { std::string s; ok = get_str(d, p, end, s); tocs.insert(s); }
    ok = ok && get_raw32(d, p, end, n);
    for (uint32_t i = 0; ok && i < n; ++i) {
        std::string s; uint64_t a = 0, b = 0;
        ok = get_str(d, p, end, s) && get_raw64(d, p, end, a) && get_raw64(d, p, end, b);
        hashes[s] = {a, b};
    }
    ok = ok && get_raw32(d, p, end, n);
    for (uint32_t i = 0; ok && i < n; ++i) {
        uint32_t k = 0; uint64_t v = 0;
        ok = get_raw32(d, p, end, k) && get_raw64(d, p, end, v);
        types[k] = v;
    }
    ok = ok && get_raw64(d, p, end, total) && get_raw32(d, p, end, n);
    for (uint32_t i = 0; ok && i < n; ++i) {
        std::string a, b;
        ok = get_str(d, p, end, a) && get_str(d, p, end, b);
        depots.emplace(a, b);
    }
    ok = ok && get_raw32(d, p, end, n);
    if (ok) bundles.reserve(n);
    for (uint32_t i = 0; ok && i < n; ++i) { std::string s; ok = get_str(d, p, end, s); bundles.push_back(std::move(s)); }
    ok = ok && p == end;
    if (ok) {
        for (uint32_t i = 0; ok && i < rl.count; ++i)
            ok = rl.recs[i].bundle == ResTable::kNoBundle || rl.recs[i].bundle < bundles.size();
        for (uint32_t i = 0; ok && i < el.count; ++i)
            ok = el.recs[i].bundle == EbxTable::kNoBundle || el.recs[i].bundle < bundles.size();
    }
    if (!ok) { why = "snapshot state is corrupt"; return false; }

    res_.clear_overlay(); ebx_.clear_overlay(); chunks_.clear_overlay(); chunk_seg_.clear_overlay();
    res_.add_layer(rl); ebx_.add_layer(el); chunks_.add_layer(cl); chunk_seg_.add_layer(sl);
    mounted_tocs_.swap(tocs);
    mounted_toc_hashes_.swap(hashes);
    res_entry_type_.swap(types);
    res_entries_total_ = total;
    depot_by_bundle_.swap(depots);
    bundles_.swap(bundles);
    bundle_ix_.clear();
    bundle_ix_.reserve(bundles_.size());
    for (uint32_t i = 0; i < bundles_.size(); ++i) bundle_ix_.emplace(bundles_[i], i);
    seq_a_ = key_a; seq_b_ = key_b;
    snap_files_.push_back(std::move(f));
    tables_grew();
    return true;
}

bool Source::save_snapshot(const std::string& path, uint64_t key_a, uint64_t key_b,
                           uint64_t parent_a, uint64_t parent_b,
                           const std::vector<std::string>& stamps, std::string& why)
{
    std::vector<uint8_t> sec[kSnapSections];
    put_u32(sec[S_STAMPS], (uint32_t)stamps.size());
    for (const std::string& s : stamps) put_str(sec[S_STAMPS], s);
    std::vector<uint8_t> names;
    res_.write_overlay(sec[S_RES_RECS], sec[S_RES_SLOTS], names);
    ebx_.write_overlay(sec[S_EBX_RECS], sec[S_EBX_SLOTS], names);
    chunks_.write_overlay(sec[S_CHUNK_RECS], sec[S_CHUNK_SLOTS], names);
    chunk_seg_.write_overlay(sec[S_SEG_RECS], sec[S_SEG_SLOTS], names);
    sec[S_NAMES].swap(names);

    std::vector<uint8_t>& st = sec[S_STATE];
    put_u32(st, (uint32_t)mounted_tocs_.size());
    for (const std::string& s : mounted_tocs_) put_str(st, s);
    put_u32(st, (uint32_t)mounted_toc_hashes_.size());
    for (const auto& kv : mounted_toc_hashes_) { put_str(st, kv.first); put_u64(st, kv.second.first); put_u64(st, kv.second.second); }
    put_u32(st, (uint32_t)res_entry_type_.size());
    for (const auto& kv : res_entry_type_) { put_u32(st, kv.first); put_u64(st, kv.second); }
    put_u64(st, res_entries_total_);
    put_u32(st, (uint32_t)depot_by_bundle_.size());
    for (const auto& kv : depot_by_bundle_) { put_str(st, kv.first); put_str(st, kv.second); }
    put_u32(st, (uint32_t)bundles_.size());
    for (const std::string& s : bundles_) put_str(st, s);

    SnapHeader h{};
    std::memcpy(h.magic, kSnapMagic, 8);
    h.version = kSnapVersion;
    h.sections = kSnapSections;
    for (uint32_t i = 0; i < 3; ++i) h.rec_sizes[i] = snap_rec_size(i);
    h.key_a = key_a; h.key_b = key_b; h.parent_a = parent_a; h.parent_b = parent_b;
    uint64_t at = sizeof(SnapHeader);
    for (uint32_t i = 0; i < kSnapSections; ++i) {
        at = (at + 7) & ~7ull;                  // every section 8-aligned for in-place reads
        h.off[i] = at; h.size[i] = sec[i].size();
        at += sec[i].size();
    }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    const auto nonce = (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string tmp = path + "." + std::to_string(nonce) + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) { why = "cannot write " + tmp; return false; }
        o.write((const char*)&h, sizeof(h));
        uint64_t pos = sizeof(h);
        static const char zeros[8] = {0};
        for (uint32_t i = 0; i < kSnapSections; ++i) {
            if (h.off[i] > pos) o.write(zeros, (std::streamsize)(h.off[i] - pos));
            if (!sec[i].empty()) o.write((const char*)sec[i].data(), (std::streamsize)sec[i].size());
            pos = h.off[i] + sec[i].size();
        }
        o.close();
        if (!o) { std::filesystem::remove(tmp, ec); why = "short write " + tmp; return false; }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        // Another process got there first (and may have it mapped): use theirs.
        std::filesystem::remove(tmp, ec);
    }
    return true;
}

bool Source::mount_tocs(const std::vector<std::string>& toc_paths, const char* label,
                        size_t& mounted, std::string& err)
{
    const auto t0 = std::chrono::steady_clock::now();
    snap_stats_ = MountSnapshotStats();
    mounted = 0;

    // The same identity twice in one list mounts once.
    std::vector<std::string> todo;
    std::vector<std::string> ids;
    {
        std::unordered_set<std::string> seen;
        for (const std::string& p : toc_paths) {
            std::string id = toc_identity(p);
            if (mounted_tocs_.count(id) || !seen.insert(id).second) continue;
            todo.push_back(p);
            ids.push_back(std::move(id));
        }
    }
    auto finish = [&]() {
        for (const std::string& p : toc_paths) if (mounted_tocs_.count(toc_identity(p))) mounted++;
        snap_stats_.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return true;
    };
    if (todo.empty()) { snap_stats_.note = "already mounted"; return finish(); }

    // A snapshot only stacks on tables that are themselves all snapshot (or
    // empty): an overlay means this process mounted something the key cannot
    // describe as a stored layer. Four layers at most - bf6_open's archives,
    // the rest of the shared archives, one level, and every other level on top
    // of that (the object catalogue, whose own level must still win) - so the
    // stored files are the ones ordinary opens ask for. Anything past that
    // mounts in memory.
    const bool overlays_empty = res_.overlay().empty() && ebx_.overlay().empty() &&
                                chunks_.overlay().empty() && chunk_seg_.overlay().empty();
    const bool use = snapshots_enabled() && overlays_empty && res_.layers().size() < 4;
    const uint64_t parent_a = seq_a_, parent_b = seq_b_;
    uint64_t key_a = seq_a_, key_b = seq_b_;
    std::string path, base;
    const std::string root = snapshot_root(game_);
    std::vector<std::string> stamps;
    if (use) {
        // The key the mount below would leave: exactly what mount_toc adds.
        stamps.reserve(todo.size());
        for (size_t i = 0; i < todo.size(); ++i) {
            stamps.push_back(toc_stamp(todo[i], ids[i]));
            seq_add(stamps.back());
        }
        key_a = seq_a_; key_b = seq_b_;
        seq_a_ = parent_a; seq_b_ = parent_b;
        const std::string key = hex64(key_a) + hex64(key_b);
        base = snap_base_.empty() ? key : snap_base_;
        path = (cache_dir() / "mount" / (root + "-" + base.substr(0, 16) + "-" + key + ".bf6m")).string();
        snap_stats_.path = path;
        if (progress_ && !progress_(label, 0, (int)todo.size())) { err = "cancelled"; return false; }
        std::string why;
        if (load_snapshot(path, parent_a, parent_b, key_a, key_b, why)) {
            if (snap_base_.empty()) snap_base_ = base;
            touch(path);
            snap_stats_.loaded = true;
            if (progress_) progress_(label, (int)todo.size(), (int)todo.size());
            return finish();
        }
        snap_stats_.note = why;
    } else {
        snap_stats_.note = !snapshots_enabled() ? "disabled" :
                           !overlays_empty ? "tables already hold unsnapshotted mounts" : "layer limit";
    }

    int done = 0;
    for (const std::string& t : todo) {
        if (progress_ && !progress_(label, done++, (int)todo.size())) { err = "cancelled"; return false; }
        std::string e;
        mount_toc(t, e);
    }

    // Write what this call added, then read it back from the file so the heap
    // copy goes. Small mounts are not worth a file.
    const size_t added = res_.overlay().size() + ebx_.overlay().size() +
                         chunks_.overlay().size() + chunk_seg_.overlay().size();
    if (use && added >= 20000 && seq_a_ == key_a && seq_b_ == key_b) {
        const auto s0 = std::chrono::steady_clock::now();
        std::string why;
        if (save_snapshot(path, key_a, key_b, parent_a, parent_b, stamps, why)) {
            snap_stats_.saved = true;
            if (snap_base_.empty()) sweep_stale_chains(root, base.substr(0, 16));
            enforce_snapshot_cap(cache_dir() / "mount", root, {path});
            // Re-open from the file: identical tables, no heap copy.
            // load_snapshot swaps the overlays for the mapped layer only when
            // the file validates; on failure the heap tables stay as they are.
            std::string why2;
            if (load_snapshot(path, parent_a, parent_b, key_a, key_b, why2)) {
                if (snap_base_.empty()) snap_base_ = base;
            } else {
                snap_stats_.note = "saved but not reopened: " + why2;
            }
        } else {
            snap_stats_.note = "not saved: " + why;
        }
        snap_stats_.save_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s0).count();
    }
    return finish();
}

// A GAME PATCH LEAVES WHOLE CHAINS UNREACHABLE. When a new first layer is
// written, every other first layer of this installation folder is checked
// against the files it was built from; a chain whose TOCs are gone or changed is
// deleted. A chain that merely started from a different archive set (a
// front-end context, a tool) is still valid and stays.
void Source::sweep_stale_chains(const std::string& root, const std::string& keep_base) const
{
    const std::filesystem::path dir = cache_dir() / "mount";
    const std::string prefix = root + "-";
    std::set<std::string> bases;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string fn = e.path().filename().string();
        if (fn.size() < prefix.size() + 17 || fn.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string b = fn.substr(prefix.size(), 16);
        if (b != keep_base) bases.insert(b);
    }
    for (const std::string& b : bases) {
        bool stale = true;
        // The first layer's own key starts with its base.
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            const std::string fn = e.path().filename().string();
            if (fn.rfind(prefix + b + "-" + b, 0) != 0 || e.path().extension() != ".bf6m") continue;
            std::shared_ptr<MappedFile> f = MappedFile::open(e.path().string());
            if (!f || f->size() < sizeof(SnapHeader)) break;
            SnapHeader hd;
            std::memcpy(&hd, f->data(), sizeof(hd));
            if (std::memcmp(hd.magic, kSnapMagic, 8) != 0 || hd.version != kSnapVersion ||
                hd.off[S_STAMPS] > f->size() || hd.size[S_STAMPS] > f->size() - hd.off[S_STAMPS]) break;
            const uint8_t* d = f->data();
            size_t p = (size_t)hd.off[S_STAMPS];
            const size_t end = p + (size_t)hd.size[S_STAMPS];
            uint32_t n = 0;
            if (!get_raw32(d, p, end, n) || n == 0) break;
            stale = false;
            for (uint32_t i = 0; i < n && !stale; ++i) {
                std::string s;
                if (!get_str(d, p, end, s)) { stale = true; break; }
                const size_t bar = s.find('|');
                const std::string id = s.substr(0, bar);
                stale = toc_stamp(id, id) != s;
            }
            break;
        }
        if (!stale) continue;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            const std::string fn = e.path().filename().string();
            if (fn.rfind(prefix + b + "-", 0) == 0) {
                std::error_code rm;
                std::filesystem::remove(e.path(), rm);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// MappedFile
// ---------------------------------------------------------------------------

#ifdef _WIN32
}  // namespace bf6
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace bf6 {

std::shared_ptr<MappedFile> MappedFile::open(const std::string& path)
{
    const std::wstring wide = std::filesystem::path(path).wstring();
    HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) { CloseHandle(file); return nullptr; }
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) { CloseHandle(file); return nullptr; }
    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view) { CloseHandle(mapping); CloseHandle(file); return nullptr; }
    std::shared_ptr<MappedFile> f(new MappedFile());
    f->data_ = (const uint8_t*)view;
    f->size_ = (size_t)size.QuadPart;
    f->file_ = file;
    f->mapping_ = mapping;
    return f;
}

MappedFile::~MappedFile()
{
    if (data_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle((HANDLE)mapping_);
    if (file_) CloseHandle((HANDLE)file_);
}
#else
}  // namespace bf6
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
namespace bf6 {

std::shared_ptr<MappedFile> MappedFile::open(const std::string& path)
{
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return nullptr;
    struct stat s;
    if (fstat(fd, &s) != 0 || s.st_size <= 0) { ::close(fd); return nullptr; }
    void* view = mmap(nullptr, (size_t)s.st_size, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (view == MAP_FAILED) return nullptr;
    std::shared_ptr<MappedFile> f(new MappedFile());
    f->data_ = (const uint8_t*)view;
    f->size_ = (size_t)s.st_size;
    return f;
}

MappedFile::~MappedFile()
{
    if (data_) munmap((void*)data_, size_);
}
#endif

}  // namespace bf6
