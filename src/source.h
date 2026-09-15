/* libbf6 internal - the mount (module 2, final piece).
 *
 * Ties the mount together: a locator (cas_path), one or more parsed TOCs, and
 * for each bundle the segments-vs-payload zip that fills the res/ebx tables.
 * get_res / get_ebx then resolve a name to its CAS location and read+decompress
 * it. First mount wins on a name collision, matching bf6_source.gd.
 */
#ifndef LIBBF6_SOURCE_H
#define LIBBF6_SOURCE_H

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cas.h"
#include "caslocator.h"
#include "mount_table.h"
#include "toc.h"

namespace bf6 {

struct ResEntry { CasLoc loc; uint32_t dsize = 0; uint32_t type = 0; uint64_t rid = 0; };
struct EbxEntry { CasLoc loc; uint32_t dsize = 0; };

using ResTable   = MountTable<ResEntry, false>;
using EbxTable   = MountTable<EbxEntry, false>;
using ChunkTable = MountTable<CasLoc, true>;

// What the last mount_tocs call did, for benches and tests.
struct MountSnapshotStats {
    bool        loaded = false;     // the tables came from a mapped snapshot
    bool        saved = false;      // a snapshot was written after mounting
    double      seconds = 0.0;      // the whole call
    double      save_seconds = 0.0;
    std::string path;
    std::string note;               // why a snapshot was not used, when it was not
};

// A partition's own GUID out of its EFIX chunk, in the EBX reader's spelling.
// Two routes to the same answer: from a fully decoded record, and through a
// range reader that decodes only the blocks holding the RIFF header, the chunk
// headers it walks, and the 16 GUID bytes. Neither assumes where EFIX sits;
// both walk the chunks with the same even-padding rule. Empty when absent.
std::string partition_guid_from_bytes(const std::vector<uint8_t>& raw);
std::string partition_guid_from_reader(CasRangeReader& reader);

// What building one partition index cost. `io` is summed from per-worker
// counters after the join, so it needs no shared counter.
struct PartitionIndexStats {
    bool     built         = false;
    bool     full_read     = false;  // the original decode-everything path
    bool     from_cache    = false;  // armory disk cache hit: nothing was read
    bool     cancelled     = false;
    uint64_t partitions    = 0;      // records asked
    uint64_t guids         = 0;      // records that yielded a GUID
    uint64_t duplicates    = 0;      // GUIDs lost to an earlier name (first wins)
    double   read_seconds  = 0;      // the parallel read phase only
    CasReadStats io;
};

class Source {
public:
    bool open(const std::string& game_dir, std::string& err);   // build the locator
    bool mount_toc(const std::string& toc_path, std::string& err);

    // MOUNT SEVERAL TOCS, IN ORDER, THROUGH THE MOUNT SNAPSHOT.
    //
    // The tables a sequence of TOCs produces depend only on those TOCs (path,
    // size, modification time), their order, and what was mounted before. That
    // is the snapshot key. On a hit the tables are mapped from disk and nothing
    // is read from the archives; on a miss the TOCs are mounted as usual and
    // the result is written, then mapped, so the process also stops holding the
    // tables as heap strings. `mounted` receives how many of `toc_paths` are
    // mounted; false only when the progress callback cancels.
    // BF6_DISABLE_MOUNT_SNAPSHOT=1 mounts every TOC directly.
    bool mount_tocs(const std::vector<std::string>& toc_paths, const char* label,
                    size_t& mounted, std::string& err);
    const MountSnapshotStats& mount_snapshot_stats() const { return snap_stats_; }
    void set_mount_snapshots(bool enabled) { snap_override_ = enabled ? 1 : 0; }

    // Add one EBX from its proven TOC + bundle owner without importing every
    // unrelated name in that archive into the active mount/index.
    bool mount_ebx_owner(const std::string& toc_path,
                         const std::string& bundle_name,
                         const std::string& ebx_name,
                         std::string& err);

    // Called from inside the long loops. See the ABI's note: it can be called
    // from several threads at once and must not touch a UI. False asks to stop.
    using Progress = std::function<bool(const char*, int, int)>;
    void set_progress(Progress p) { progress_ = std::move(p); }

    // ---- mounting a LEVEL, not just the shared archives ----
    //
    // Non-level archives are always mounted. Level archives are normally just
    // the one asked for, which is right for READING a level: its terrain, its
    // placements, its lighting.
    //
    // It is the wrong answer for the objects a PLAYER can place. Measured on
    // mp_dumbo, one level's mount carries pf_portal_ prefabs for 1,609 of the
    // SDK's 10,883 placeables and another 235 reachable by folder name; the
    // remaining 9,039 are not in that mount in any form, because a prefab lives
    // in the bundles of the levels that use the object. all_levels mounts every
    // level so the whole catalogue resolves. It is not the default, because a
    // level read does not need it and it is not free.
    bool mount_level(const std::string& level, bool all_levels, std::string& err);

    // Focused front-end/armory mount. Discovers the current install's shared
    // TOCs at runtime, then mounts only the authored UI, weapons, characters,
    // vehicles, global, main-menu and English-localization families. This is
    // a path-class filter over the installed files, never a staged asset list.
    bool mount_frontend(std::string& err);

    // The .toc paths under the install, IN MOUNT ORDER. See the ordering law in
    // the .cpp: first mount wins, so shared archives go first, then the level
    // being read, then every other level.
    std::vector<std::string> find_tocs(const std::string& level, bool all_levels) const;

    // The level ids the install actually carries, from directory names only.
    std::vector<std::string> available_levels() const;

    // Partition guid -> "<name>.ebx", for resolving EBX imports to real names.
    // Every partition's EFIX header is read to build it, which is the whole
    // cost; the result is cached on this Source. First name wins, so the result
    // is stable across runs rather than dependent on iteration order.
    const std::map<std::string, std::string>& partition_index();
    // Generation-scoped name/leaf aliases for repeated fixture traversals.
    const std::unordered_map<std::string, std::string>& light_name_index();

    // The armory follows imports only into authored hardware, gameplay and UI
    // families.  Building this runtime index reads that bounded name space
    // instead of every level partition; it is an optimization of the same
    // EFIX read path, not a staged/exported cache.
    const std::map<std::string, std::string>& armory_partition_index();

    // Some authored UI aliases deliberately share a partition GUID.  A
    // one-value index cannot represent that fact.  Callers that have an
    // authored discriminator (for example a Rime widget-reference Name) use
    // this live candidate set and must decline unresolved ambiguity.
    const std::map<std::string, std::vector<std::string>>& armory_partition_candidates();

    // HOW THE INDEXES READ EACH PARTITION. Bounded (the default) decodes only
    // the blocks the EFIX walk touches; full read is the original path that
    // decodes the whole record, kept as the A/B control and the fallback. The
    // environment variable BF6_PARTITION_INDEX_FULL_READ=1 selects the full
    // read; this setter overrides the environment for tests. It takes effect
    // on the next build, so call reset_partition_indexes() between runs.
    void set_partition_index_full_read(bool full) { pidx_full_override_ = full ? 1 : 0; }
    bool partition_index_full_read() const;
    void reset_partition_indexes();
    const PartitionIndexStats& partition_index_stats() const { return pidx_stats_; }
    const PartitionIndexStats& armory_partition_index_stats() const { return armory_pidx_stats_; }

    // One partition's GUID by either route. Thread-safe for concurrent calls:
    // it reads only the locator and counts into the caller's `stats`.
    std::string partition_guid(const CasLoc& loc, bool full_read, CasReadStats* stats);

    // WHICH BUNDLE A RESOURCE CAME IN, which is how a mesh finds its depot.
    //
    // A shader state key is only unique within a scope, and the scope is the
    // BUNDLE, not the directory: depots are named
    // <bundle asset path>_win32_shaderstate/shaderblockdepot_<n>. Nothing else
    // in the mount records this, so it has to be captured while the bundles are
    // being read.
    const std::string& bundle_of(const std::string& res_name) const;

    // Which bundle an EBX partition came in. This is the one that matters for
    // materials: the depot rule is THE PLACING BUNDLE - the bundle whose
    // placement pulled a mesh in - not the bundle the mesh resource happens to
    // live in. Measured over 79,000 placed instances on a retail level, the
    // placing bundle's own depot carries every one of its section keys.
    const std::string& bundle_of_ebx(const std::string& ebx_name) const;

    // The depot covering a bundle, or empty. Handles the one-token difference
    // that makes this fail silently: a TOC spells a bundle "win32/game/..."
    // while a depot spells it "game/...".
    //
    // On a miss it widens to the bundle's ANCESTORS and never to a sibling. A
    // state key is unique only within a scope, so a sibling that happens to
    // hold the key binds a material that merely collides - a confidently wrong
    // texture, which is worse than an untextured surface because nothing about
    // it looks broken.
    // Every bundle that has a depot. Exposed so a caller can find the bundle
    // that OWNS a part when the part's own resource bundle carries no
    // material - see bf6_part_bundle. Deliberately a caller's decision:
    // depot_for_bundle will not widen to a sibling by itself, because a
    // sibling holding the same key binds a confidently wrong texture.
    const std::map<std::string, std::string>& depots_by_bundle() const
    { return depot_by_bundle_; }

    std::string depot_for_bundle(const std::string& bundle) const;
    std::string depot_for_res(const std::string& res_name) const;

    static bool        is_level_toc(const std::string& path);
    static std::string mount_key(const std::string& path);

    // LEVEL PATHS, one rule for every reader. A level lives at
    // ".../levels/<level>/" and, since the 1.4.3.0 game update, also under one
    // group folder: ".../levels/gr/mp_portal_sand/", ".../levels/mp/
    // mp_aftermath_portal/". Both take a LOWERCASE, forward-slash name.
    //   level_root_tail: name ends with "/levels/[group/]<leaf>/<leaf>".
    //   level_dir_end:   index just past "/levels/[group/]<level>" in name, or npos.
    static bool   level_root_tail(const std::string& name, const std::string& leaf);
    static size_t level_dir_end(const std::string& name, const std::string& level);

    std::vector<uint8_t> get_res(const std::string& name, std::string& err);
    std::vector<uint8_t> get_ebx(const std::string& name, std::string& err);
    // Loose chunk or bundle chunk, by guid hex (either spelling - see get_chunk).
    std::vector<uint8_t> get_chunk(const std::string& guid_hex, std::string& err);
    // Where a chunk lives in the installation, without reading it: the archive
    // path and the reference inside it. Same lookup order as get_chunk.
    bool locate_chunk(const std::string& guid_hex, std::string& path, CasLoc& loc) const;
    // Same for a resource by name.
    bool locate_res(const std::string& name, std::string& path, ResEntry& entry) const;

    const std::string& game_dir() const { return game_; }
    size_t res_count() const { return res_.size(); }
    size_t ebx_count() const { return ebx_.size(); }
    // Catalogue membership without reading or decompressing the resource.
    // Registration paths use this when payload bytes are consumed later by a
    // dedicated decoder.
    bool has_res(const std::string& name) const
    { return res_.lookup(name) != nullptr; }
    const ResTable& res() const { return res_; }

    /* RES ENTRIES SEEN BEFORE DEDUP, per type. res_ keys by NAME and the first
     * bundle wins, so res_.size() is a DISTINCT-NAME count. A resource shared
     * by many bundles is listed once per bundle in the payloads and counted
     * once here per listing. Exposed to settle what data/res_types.tsv counts:
     * its totals are ~18x res_.size() overall and vary 2x-76x per type, which
     * distinct names cannot explain but listings can. */
    const std::map<uint32_t, uint64_t>& res_entries_by_type() const { return res_entry_type_; }
    uint64_t res_entries_total() const { return res_entries_total_; }
    const EbxTable& ebx() const { return ebx_; }
    // The chunk tables, for a caller that enumerates rather than asking for one
    // guid it already knows. Two of them because they ARE two: a loose chunk is
    // in the TOC's own chunk list, a bundle chunk is a segment of a bundle, and
    // get_chunk looks in both.
    const ChunkTable& loose_chunks() const { return chunks_; }
    const ChunkTable& bundle_chunks() const { return chunk_seg_; }
    // Is this guid in either chunk map, WITHOUT reading it. A resource names
    // its chunk in one of two byte orders and the only way to know which is to
    // look; doing that with get_chunk would decompress a megabyte to answer a
    // yes/no question.
    bool has_chunk(const std::string& guid_hex) const;

private:
    std::string game_;
    CasLocator  loc_;
    ResTable res_;
    std::map<uint32_t, uint64_t> res_entry_type_;
    uint64_t res_entries_total_ = 0;
    EbxTable ebx_;
    std::unordered_set<std::string>           mounted_tocs_;
    // Full-content fingerprints of the exact live TOCs accepted into this
    // mount. They key the disposable armory index cache, so a patched install
    // cannot silently consume an index produced from older archive metadata.
    std::map<std::string, std::pair<uint64_t, uint64_t>> mounted_toc_hashes_;
    ChunkTable                                chunks_;    // loose-chunk guid -> loc
    ChunkTable                                chunk_seg_; // bundle-chunk guid -> loc
    Progress                                  progress_;
    // Bundle names, indexed by the bundle field of res_ and ebx_ entries. One
    // string per bundle rather than one per resource.
    std::vector<std::string>                  bundles_;
    std::unordered_map<std::string, uint32_t> bundle_ix_;
    uint32_t bundle_index(const std::string& name);

    // The mount sequence so far, folded into a key (see mount_tocs), and the
    // snapshot files whose layers the tables are reading.
    uint64_t seq_a_ = 0x6d6f756e74736571ull;
    uint64_t seq_b_ = 0x9e3779b97f4a7c15ull;
    void     seq_add(const std::string& item);
    std::string toc_stamp(const std::string& toc_path, const std::string& identity) const;
    std::vector<std::shared_ptr<MappedFile>>  snap_files_;
    std::string                               snap_base_;   // key of the first layer
    MountSnapshotStats                        snap_stats_;
    int                                       snap_override_ = -1; // -1: environment
    bool snapshots_enabled() const;
    bool load_snapshot(const std::string& path, uint64_t parent_a, uint64_t parent_b,
                       uint64_t key_a, uint64_t key_b, std::string& why);
    bool save_snapshot(const std::string& path, uint64_t key_a, uint64_t key_b,
                       uint64_t parent_a, uint64_t parent_b,
                       const std::vector<std::string>& stamps, std::string& why);
    void sweep_stale_chains(const std::string& root, const std::string& keep_base) const;
    void tables_grew();
    // "" unless every table is a snapshot layer; else a file keyed like them.
    std::string snapshot_side_path(const char* tag) const;
    bool load_partition_index(const std::string& path);
    void save_partition_index(const std::string& path) const;
    std::map<std::string, std::string>        depot_by_bundle_; // bundle -> depot res
    std::map<std::string, std::string>        pidx_;      // partition guid -> name.ebx
    bool                                      pidx_built_ = false;
    std::unordered_map<std::string, std::string> light_names_;
    bool                                      light_names_built_ = false;
    std::map<std::string, std::string>        armory_pidx_;
    std::map<std::string, std::vector<std::string>> armory_pidx_candidates_;
    bool                                      armory_pidx_built_ = false;
    int                                       pidx_full_override_ = -1; // -1: environment
    PartitionIndexStats                       pidx_stats_;
    PartitionIndexStats                       armory_pidx_stats_;

    std::vector<uint8_t> read_seg(const CasLoc& seg, bool allow_raw, std::string& err);
    struct PartitionRef { std::string name; CasLoc loc; };
    using PartitionOrder = std::vector<PartitionRef>;
    void read_partition_guids(const PartitionOrder& order, const char* label,
                              std::vector<std::string>& found, PartitionIndexStats& st);
};

}  // namespace bf6
#endif
