/* libbf6 internal - CAS block reader (module 2, lower half).
 *
 * Ported from bf6_cas.gd. Reads one CAS reference: a run of blocks in a .cas
 * file at a byte offset, each block an 8-byte big-endian header followed by its
 * (usually Oodle-compressed) payload. The mount resolves a resource name to
 * (path, offset, size); this turns that into the decompressed bytes.
 *
 * Not part of the public C ABI - the source/mount layer calls it.
 */
#ifndef LIBBF6_CAS_H
#define LIBBF6_CAS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bf6 {

// Byte and decode counters for one reader. NOT GLOBAL, deliberately: the
// partition index reads from several workers at once, so each worker owns one
// of these and the caller folds them together after the join.
struct CasReadStats {
    uint64_t records         = 0;  // references opened
    uint64_t file_reads      = 0;  // fread calls against the archive
    uint64_t file_bytes      = 0;  // bytes those freads returned
    uint64_t blocks_seen     = 0;  // block headers accepted
    uint64_t blocks_decoded  = 0;  // Oodle blocks actually decompressed
    uint64_t decoded_bytes   = 0;  // bytes those decodes produced
    uint64_t blocks_skipped  = 0;  // accepted blocks whose payload was never read
    uint64_t block_reuses    = 0;  // range reads served from an already-decoded block
    uint64_t failures        = 0;  // references rejected outright (no bytes at all)
    // Range reader copies served from a read-only mapped view instead of fread.
    // These are bytes REQUESTED and copied out of the view, not physical disk
    // I/O: the OS pages the view in, and a page may already be cached. Kept
    // apart from file_reads/file_bytes so the stdio A/B stays comparable.
    uint64_t mapped_reads    = 0;
    uint64_t mapped_bytes    = 0;
    uint64_t mapped_faults   = 0;  // copies that hit an in-page error (file shrank)

    void add(const CasReadStats& o) {
        records += o.records; file_reads += o.file_reads; file_bytes += o.file_bytes;
        blocks_seen += o.blocks_seen; blocks_decoded += o.blocks_decoded;
        decoded_bytes += o.decoded_bytes; blocks_skipped += o.blocks_skipped;
        block_reuses += o.block_reuses; failures += o.failures;
        mapped_reads += o.mapped_reads; mapped_bytes += o.mapped_bytes;
        mapped_faults += o.mapped_faults;
    }
};

// Read the reference at `path`+`offset`, `size` bytes on disk, and return the
// decompressed payload. Empty vector on failure with `err` set. `allow_raw`
// returns the bytes verbatim when the block guard is absent (non-block data).
// oodle_open() must have been called for the install first.
std::vector<uint8_t> cas_read(const std::string& path, int64_t offset,
                              int64_t size, bool allow_raw, std::string& err);
// Same, counting what it read and decoded into `stats` (may be null).
std::vector<uint8_t> cas_read(const std::string& path, int64_t offset,
                              int64_t size, bool allow_raw, std::string& err,
                              CasReadStats* stats);

// RANDOM ACCESS INTO ONE REFERENCE'S DECOMPRESSED BYTES without producing all
// of them.
//
// cas_read decodes every block to answer any question. A caller that needs a
// few header bytes out of a large record (the partition index wants a RIFF
// header, some chunk headers and a 16-byte GUID) pays for the whole payload.
// This walks block HEADERS lazily, reads and decodes only the blocks that
// intersect a requested range, and reads stored (codec 0) payload bytes
// straight from the file without copying the rest of the block.
//
// SAME ACCEPTANCE RULES AS cas_read, block for block: the guard nibble, the
// single-block test (csize == size - 8), the multi-block walk that stops at a
// broken guard or an overrunning block, the two-part minimum, the codec set
// and the stored-size check. The one thing it cannot match is a failure it
// never looks at: a corrupt Oodle stream (or bad header) in a block past every
// requested range fails cas_read but not this. Callers that need the full-read
// answer keep cas_read as the comparison path.
//
// All offsets are bounded by the declared reference size AND the archive's
// real length, so a truncated archive fails a read instead of running past the
// buffer. Not thread-safe; one reader per thread. Two decoded blocks are kept,
// so a chunk header straddling a boundary and repeated reads in one lookup do
// not decode a block twice.
//
// MAPPED INPUT (64-bit Windows, on by default; BF6_CAS_MAPPED_READ=0 turns it
// off). The reader copies bytes out of a read-only view of the whole archive
// instead of paying a seek and fread per small read. The view belongs to the
// same thread-local handle record as the FILE* and is unmapped when the thread
// ends; it is never writable and nothing is read ahead into RAM by this code.
// Any mapping failure falls back to the stdio route. cas_read never maps.
class CasRangeReader {
public:
    // False with `err` set when the reference cannot be a valid record (no
    // file, shorter than a header, no guard without allow_raw, or a multi-block
    // span with fewer than two parts). `stats` may be null and must outlive
    // the reader.
    bool open(const std::string& path, int64_t offset, int64_t size,
              bool allow_raw, std::string& err, CasReadStats* stats = nullptr);

    // Copy decompressed bytes [at, at+n) into dst. False when the range is not
    // entirely inside the decompressed record, or a block it touches fails.
    bool read(uint64_t at, size_t n, uint8_t* dst, std::string& err);

    // Decompressed length, walking any remaining block headers to learn it.
    // Returns false if that walk fails.
    bool total_size(uint64_t& out, std::string& err);

    // Blocks accepted so far (the walk is lazy).
    size_t blocks_known() const { return blocks_.size(); }

    CasRangeReader() = default;
    CasRangeReader(const CasRangeReader&) = delete;
    CasRangeReader& operator=(const CasRangeReader&) = delete;
    ~CasRangeReader();   // books never-touched blocks as skipped

private:
    struct Block {
        uint64_t file_pos = 0;   // relative to the reference start, at the header
        uint64_t dstart   = 0;   // decompressed offset
        uint32_t csize    = 0;
        uint32_t dsize    = 0;
        uint8_t  codec    = 0;
        bool     touched  = false;
    };
    struct Decoded { size_t index = (size_t)-1; std::vector<uint8_t> bytes; };

    bool file_read(uint64_t rel, size_t n, uint8_t* dst);
    bool walk_next(std::string& err);          // accept one more block header
    bool block_bytes(size_t bi, uint64_t in_block, size_t n, uint8_t* dst,
                     std::string& err);

    void*         file_  = nullptr;   // thread-local cached FILE*, not owned
    const uint8_t* map_  = nullptr;   // thread-local mapped archive, not owned; null = stdio
    int64_t       base_  = 0;
    uint64_t      avail_ = 0;         // min(declared size, bytes left in archive)
    bool          raw_   = false;
    bool          multi_ = false;
    bool          walk_done_ = false;
    std::string   walk_err_;          // a header the full read would reject
    uint8_t       hdr0_[8] = {};
    uint64_t      walk_pos_  = 0;
    uint64_t      dtotal_    = 0;     // decompressed bytes of accepted blocks
    std::vector<Block> blocks_;
    Decoded       cache_[2];
    size_t        cache_next_ = 0;
    std::vector<uint8_t> comp_;       // compressed payload scratch
    std::vector<uint8_t> win_;        // prefetched head of the reference
    uint64_t      win_pos_ = 0;
    CasReadStats* stats_ = nullptr;
    CasReadStats  local_;
};

}  // namespace bf6
#endif
