#include "cas.h"

#include <algorithm>
#include <cstring>
#include <map>

#include <cstdio>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "oodle.h"
#include "stdio_compat.h"

namespace bf6 {

// The CAS block header, ported field-for-field from bf6_cas.gd:
//   d0 (BE) : flags = d0>>24,        dsize = d0 & 0x00FFFFFF
//   d1 (BE) : codec = d1>>24, guard = (d1>>20)&0xF, csize = d1 & 0x000FFFFF
static const uint8_t  GUARD      = 7;
static const uint8_t  CODEC_NONE = 0x00;
static const int      MAX_BLOCKS = 4096;
static const size_t   PREFETCH   = 16 * 1024;  // range reader's first read

static bool is_oodle_codec(uint8_t c) {
    return c == 0x11 || c == 0x15 || c == 0x17 || c == 0x19;
}

static uint32_t be32(const uint8_t* b, size_t at) {
    return (uint32_t(b[at]) << 24) | (uint32_t(b[at + 1]) << 16) |
           (uint32_t(b[at + 2]) << 8) | uint32_t(b[at + 3]);
}

struct BlockHdr {
    uint8_t  flags;
    uint32_t dsize;
    uint8_t  codec;
    uint8_t  guard;
    uint32_t csize;
};

static BlockHdr block_header(const uint8_t* b, size_t pos) {
    uint32_t d0 = be32(b, pos);
    uint32_t d1 = be32(b, pos + 4);
    BlockHdr h;
    h.flags = uint8_t((d0 >> 24) & 0xFF);
    h.dsize = d0 & 0x00FFFFFF;
    h.codec = uint8_t((d1 >> 24) & 0xFF);
    h.guard = uint8_t((d1 >> 20) & 0x0F);
    h.csize = d1 & 0x000FFFFF;
    return h;
}

// Decode one block's payload of `csize` bytes at buf+at into `out` (appended).
static bool decode_one(const uint8_t* buf, size_t at, uint32_t csize,
                       uint32_t dsize, uint8_t codec,
                       std::vector<uint8_t>& out, std::string& err) {
    if (codec == CODEC_NONE) {
        if (csize != dsize) { err = "uncompressed block size mismatch"; return false; }
        out.insert(out.end(), buf + at, buf + at + csize);
        return true;
    }
    if (!is_oodle_codec(codec)) {
        char m[64];
        std::snprintf(m, sizeof(m), "codec 0x%02X is not present in BF6 data", codec);
        err = m;
        return false;
    }
    size_t base = out.size();
    out.resize(base + dsize);
    if (!oodle_decompress(buf + at, csize, out.data() + base, dsize)) {
        err = "oodle decompress failed / short";
        out.resize(base);
        return false;
    }
    return true;
}

namespace {

// OPEN EACH ARCHIVE ONCE PER THREAD, not once per read.
//
// This used to fopen and fclose around every read, which is fine for a handful
// and ruinous for the partition index: that walks every partition in the mount
// for one 16-byte header, so mp_dumbo alone paid 228,818 file opens. Threading
// the index only took it from 19 seconds to 14 precisely because the cost was
// syscalls rather than work.
//
// THREAD_LOCAL RATHER THAN SHARED, deliberately. A FILE* carries one position,
// so a shared handle would need a lock around the seek and the read together,
// which serialises exactly the thing being parallelised. A handle per thread per
// archive costs a few dozen opens in total and needs no lock at all.
//
// Handles close when the thread ends, which for the index's short-lived workers
// is immediately after.
struct CasHandle
{
    FILE*   f    = nullptr;
    int64_t size = -1;   // archive length, learned once per handle
    // Read-only view of the whole archive, made on the range reader's first
    // use. map_state: 0 not tried, 1 mapped, -1 failed or disabled (stdio).
    int            map_state = 0;
    const uint8_t* map       = nullptr;
    uint64_t       map_size  = 0;
};

#if defined(_WIN32)
void cas_unmap(CasHandle& h)
{
    if (h.map) UnmapViewOfFile(h.map);
    h.map = nullptr; h.map_size = 0;
}
#else
void cas_unmap(CasHandle& h) { h.map = nullptr; h.map_size = 0; }
#endif

struct CasHandles
{
    std::map<std::string, CasHandle> open;
    ~CasHandles()
    {
        for (auto& kv : open) {
            cas_unmap(kv.second);
            if (kv.second.f) std::fclose(kv.second.f);
        }
    }
};

#if defined(_WIN32)
// Experimental A/B only: BF6_CAS_MAPPED_READ=1 enables mapping. The measured
// small-record workload did not improve, so stdio remains the default.
bool cas_mapping_enabled()
{
    static const bool enabled = [] {
        char v[8];
        DWORD n = GetEnvironmentVariableA("BF6_CAS_MAPPED_READ", v, sizeof(v));
        return n == 1 && v[0] == '1';
    }();
    return enabled;
}

// Map the whole archive read-only. 64-bit only: a full BF6 archive view in a
// 32-bit address space is not a reasonable ask, so that build stays on stdio.
// The file and mapping handles are closed straight away; the view keeps the
// section alive until UnmapViewOfFile. Sharing READ|WRITE|DELETE so the view
// never blocks the launcher or another reader.
void cas_try_map(CasHandle& h, const std::string& path)
{
    h.map_state = -1;
    if (sizeof(void*) < 8 || !cas_mapping_enabled() || h.size <= 0) return;

    HANDLE file = INVALID_HANDLE_VALUE;
    int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, nullptr, 0);
    if (wn > 0) {
        std::wstring wide((size_t)wn, L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, &wide[0], wn) == wn)
            file = CreateFileW(wide.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    // Not UTF-8: the stdio route opened it as an ANSI path, so do the same.
    if (file == INVALID_HANDLE_VALUE)
        file = CreateFileA(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER len;
    if (!GetFileSizeEx(file, &len) || len.QuadPart <= 0) { CloseHandle(file); return; }
    HANDLE section = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(file);
    if (!section) return;
    const LPVOID view = MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(section);
    if (!view) return;

    h.map      = static_cast<const uint8_t*>(view);
    // Never trust more than both lengths agree on.
    h.map_size = (uint64_t)std::min<int64_t>(len.QuadPart, h.size);
    h.map_state = 1;
}

// Copy out of the view. An archive truncated after it was mapped raises
// EXCEPTION_IN_PAGE_ERROR on the missing pages rather than returning short, so
// the copy is guarded where the compiler supports SEH. Kept free of C++ objects
// so __try is legal here.
#if defined(_MSC_VER)
static int cas_map_filter(unsigned long code)
{
    return (code == EXCEPTION_IN_PAGE_ERROR || code == EXCEPTION_ACCESS_VIOLATION)
               ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

bool cas_map_copy(uint8_t* dst, const uint8_t* src, size_t n)
{
    __try {
        std::memcpy(dst, src, n);
    } __except (cas_map_filter(GetExceptionCode())) {
        return false;
    }
    return true;
}
#else
// No SEH here (MinGW): a mapped install must not shrink while it is read.
// Game installs are treated as immutable for a session already.
bool cas_map_copy(uint8_t* dst, const uint8_t* src, size_t n)
{
    std::memcpy(dst, src, n);
    return true;
}
#endif
#else
void cas_try_map(CasHandle& h, const std::string&) { h.map_state = -1; }
bool cas_map_copy(uint8_t*, const uint8_t*, size_t) { return false; }
#endif

CasHandle& cas_handle_entry(const std::string& path)
{
    static thread_local CasHandles cache;
    auto it = cache.open.find(path);
    if (it != cache.open.end()) return it->second;
    CasHandle h;
    h.f = fopen_binary_read(path.c_str());
    if (h.f && _fseeki64(h.f, 0, SEEK_END) == 0) h.size = _ftelli64(h.f);
    // A null is cached too: a missing archive should not be reopened once per
    // read either.
    return cache.open.emplace(path, h).first->second;
}

FILE* cas_handle(const std::string& path)
{
    return cas_handle_entry(path).f;
}

// Bytes of the reference that really exist: the declared size clipped to the
// archive's length. Negative inputs give zero.
uint64_t cas_available(const CasHandle& h, int64_t offset, int64_t size)
{
    if (!h.f || offset < 0 || size <= 0) return 0;
    if (h.size < 0) return (uint64_t)size;
    if (offset >= h.size) return 0;
    return (uint64_t)std::min<int64_t>(size, h.size - offset);
}

}  // namespace

std::vector<uint8_t> cas_read(const std::string& path, int64_t offset,
                              int64_t size, bool allow_raw, std::string& err) {
    return cas_read(path, offset, size, allow_raw, err, nullptr);
}

std::vector<uint8_t> cas_read(const std::string& path, int64_t offset,
                              int64_t size, bool allow_raw, std::string& err,
                              CasReadStats* stats) {
    CasReadStats scratch;
    CasReadStats& st = stats ? *stats : scratch;
    st.records++;
    std::vector<uint8_t> empty;
    CasHandle& handle = cas_handle_entry(path);
    FILE* f = handle.f;
    if (!f) { err = "cannot open " + path; st.failures++; return empty; }
    if (offset < 0 || size < 0) { err = "negative CAS reference"; st.failures++; return empty; }
    // Clipped to the archive so a corrupt size cannot demand a huge buffer.
    const uint64_t want = cas_available(handle, offset, size);
    _fseeki64(f, offset, SEEK_SET);
    std::vector<uint8_t> buf((size_t)want);
    size_t got = want ? std::fread(buf.data(), 1, (size_t)want, f) : 0;
    st.file_reads++; st.file_bytes += got;
    buf.resize(got);
    if (got < 8) {
        err = "CAS reference shorter than a block header"; st.failures++; return empty;
    }

    BlockHdr h = block_header(buf.data(), 0);
    if (h.guard != GUARD) {
        if (allow_raw) return buf;
        err = "guard nibble != 7";
        st.failures++;
        return empty;
    }

    std::vector<uint8_t> out;
    auto decode = [&](size_t at, const BlockHdr& b) {
        const bool ok = decode_one(buf.data(), at, b.csize, b.dsize, b.codec, out, err);
        st.blocks_seen++;
        if (ok && b.codec != CODEC_NONE) { st.blocks_decoded++; st.decoded_bytes += b.dsize; }
        return ok;
    };
    // Single block: the compressed size accounts for the whole reference.
    if ((int64_t)h.csize == size - 8) {
        // A truncated archive used to decode past the end of buf here.
        if (8 + (size_t)h.csize > buf.size()) {
            err = "CAS reference truncated"; st.failures++; return empty;
        }
        if (!decode(8, h)) { st.failures++; return empty; }
        return out;
    }

    // Multi-block span: walk blocks until the guard breaks or the buffer ends.
    size_t pos = 0;
    int nparts = 0;
    for (int i = 0; i < MAX_BLOCKS; i++) {
        if (pos + 8 > buf.size()) break;
        BlockHdr h2 = block_header(buf.data(), pos);
        if (h2.guard != GUARD || pos + 8 + h2.csize > buf.size()) break;
        if (!decode(pos + 8, h2)) { st.failures++; return empty; }
        nparts++;
        pos += 8 + h2.csize;
    }
    if (nparts < 2) { err = "not a multi-block span"; st.failures++; return empty; }
    return out;
}

// ---------------------------------------------------------------------------
// CasRangeReader
// ---------------------------------------------------------------------------

// The header checks decode_one makes before it touches a payload. Done when a
// block is ACCEPTED, so a lazy reader refuses the same headers a full read does.
static bool header_ok(const BlockHdr& h, std::string& err) {
    if (h.codec == CODEC_NONE) {
        if (h.csize != h.dsize) { err = "uncompressed block size mismatch"; return false; }
        return true;
    }
    if (!is_oodle_codec(h.codec)) {
        char m[64];
        std::snprintf(m, sizeof(m), "codec 0x%02X is not present in BF6 data", h.codec);
        err = m;
        return false;
    }
    return true;
}

CasRangeReader::~CasRangeReader() {
    if (!stats_) return;
    for (const Block& b : blocks_) if (!b.touched) stats_->blocks_skipped++;
}

bool CasRangeReader::file_read(uint64_t rel, size_t n, uint8_t* dst) {
    if (rel > avail_ || n > avail_ - rel) return false;
    if (n == 0) return true;
    if (rel >= win_pos_ && rel - win_pos_ <= win_.size() && n <= win_.size() - (rel - win_pos_)) {
        std::memcpy(dst, win_.data() + (rel - win_pos_), n);
        return true;
    }
    if (map_) {
        // open() checked base_ + avail_ against the view, and rel + n <= avail_.
        stats_->mapped_reads++;
        if (!cas_map_copy(dst, map_ + base_ + rel, n)) { stats_->mapped_faults++; return false; }
        stats_->mapped_bytes += n;
        return true;
    }
    FILE* f = static_cast<FILE*>(file_);
    if (_fseeki64(f, base_ + (int64_t)rel, SEEK_SET) != 0) return false;
    const size_t got = std::fread(dst, 1, n, f);
    stats_->file_reads++;
    stats_->file_bytes += got;
    return got == n;
}

bool CasRangeReader::open(const std::string& path, int64_t offset, int64_t size,
                          bool allow_raw, std::string& err, CasReadStats* stats) {
    // Reset, booking any blocks a previous open never touched.
    if (stats_) for (const Block& b : blocks_) if (!b.touched) stats_->blocks_skipped++;
    blocks_.clear();
    for (Decoded& d : cache_) { d.index = (size_t)-1; d.bytes.clear(); }
    cache_next_ = 0;
    raw_ = multi_ = walk_done_ = false;
    walk_err_.clear();
    walk_pos_ = dtotal_ = 0;
    win_.clear(); win_pos_ = 0;
    file_ = nullptr;
    map_ = nullptr;
    local_ = CasReadStats();
    stats_ = stats ? stats : &local_;
    stats_->records++;

    auto fail = [&](const std::string& why) {
        err = why; stats_->failures++; walk_done_ = true; file_ = nullptr; map_ = nullptr; avail_ = 0;
        return false;
    };
    CasHandle& handle = cas_handle_entry(path);
    if (!handle.f) return fail("cannot open " + path);
    if (offset < 0 || size < 0) return fail("negative CAS reference");
    file_  = handle.f;
    base_  = offset;
    avail_ = cas_available(handle, offset, size);
    if (handle.map_state == 0) cas_try_map(handle, path);
    // A reference the view does not wholly cover stays on stdio, which reports
    // the short read itself.
    if (handle.map_state == 1 && (uint64_t)offset <= handle.map_size &&
        avail_ <= handle.map_size - (uint64_t)offset)
        map_ = handle.map;
    // ONE READ FOR A SMALL RECORD. Most partitions are a single block of a few
    // kilobytes; reading the head of the reference once covers its header and
    // payload, so the bounded route never costs more reads than cas_read. A
    // mapped reader has no read to save, so it skips the copy.
    if (!map_ && avail_ >= 8) {
        std::vector<uint8_t> head((size_t)std::min<uint64_t>(avail_, PREFETCH));
        if (file_read(0, head.size(), head.data())) { win_.swap(head); win_pos_ = 0; }
    }
    if (avail_ < 8 || !file_read(0, 8, hdr0_))
        return fail("CAS reference shorter than a block header");

    const BlockHdr h = block_header(hdr0_, 0);
    if (h.guard != GUARD) {
        if (!allow_raw) return fail("guard nibble != 7");
        raw_ = true; walk_done_ = true; dtotal_ = avail_;
        return true;
    }

    if ((int64_t)h.csize == size - 8) {
        if (8 + (uint64_t)h.csize > avail_) return fail("CAS reference truncated");
        std::string why;
        if (!header_ok(h, why)) return fail(why);
        Block b; b.file_pos = 0; b.dstart = 0; b.csize = h.csize; b.dsize = h.dsize; b.codec = h.codec;
        blocks_.push_back(b);
        stats_->blocks_seen++;
        dtotal_ = h.dsize;
        walk_done_ = true;
        return true;
    }

    multi_ = true;
    walk_next(err);
    walk_next(err);
    if (!walk_err_.empty()) return fail(walk_err_);
    if (blocks_.size() < 2) return fail("not a multi-block span");
    return true;
}

bool CasRangeReader::walk_next(std::string& err) {
    if (walk_done_) { if (!walk_err_.empty()) err = walk_err_; return false; }
    if (blocks_.size() >= (size_t)MAX_BLOCKS || walk_pos_ + 8 > avail_) {
        walk_done_ = true; return false;
    }
    uint8_t hb[8];
    if (walk_pos_ == 0) std::memcpy(hb, hdr0_, 8);
    else if (!file_read(walk_pos_, 8, hb)) {
        walk_done_ = true; walk_err_ = "CAS header read failed"; err = walk_err_; return false;
    }
    const BlockHdr h = block_header(hb, 0);
    if (h.guard != GUARD || walk_pos_ + 8 + h.csize > avail_) { walk_done_ = true; return false; }
    std::string why;
    if (!header_ok(h, why)) { walk_done_ = true; walk_err_ = why; err = why; return false; }
    Block b;
    b.file_pos = walk_pos_; b.dstart = dtotal_;
    b.csize = h.csize; b.dsize = h.dsize; b.codec = h.codec;
    blocks_.push_back(b);
    stats_->blocks_seen++;
    dtotal_ += h.dsize;
    walk_pos_ += 8 + (uint64_t)h.csize;
    return true;
}

bool CasRangeReader::total_size(uint64_t& out, std::string& err) {
    if (!file_) { err = "reader not open"; return false; }
    while (walk_next(err)) {}
    if (!walk_err_.empty()) { err = walk_err_; return false; }
    out = dtotal_;
    return true;
}

bool CasRangeReader::block_bytes(size_t bi, uint64_t in_block, size_t n, uint8_t* dst,
                                 std::string& err) {
    Block& b = blocks_[bi];
    b.touched = true;
    if (b.codec == CODEC_NONE) {
        if (!file_read(b.file_pos + 8 + in_block, n, dst)) { err = "CAS stored block read failed"; return false; }
        return true;
    }
    for (Decoded& d : cache_) {
        if (d.index == bi) {
            stats_->block_reuses++;
            std::memcpy(dst, d.bytes.data() + in_block, n);
            return true;
        }
    }
    Decoded& slot = cache_[cache_next_];
    cache_next_ = (cache_next_ + 1) % 2;
    slot.index = (size_t)-1;
    comp_.resize(b.csize);
    if (!file_read(b.file_pos + 8, b.csize, comp_.data())) { err = "CAS block read failed"; return false; }
    slot.bytes.resize(b.dsize);
    if (!oodle_decompress(comp_.data(), b.csize, slot.bytes.data(), b.dsize)) {
        err = "oodle decompress failed / short";
        return false;
    }
    stats_->blocks_decoded++;
    stats_->decoded_bytes += b.dsize;
    slot.index = bi;
    std::memcpy(dst, slot.bytes.data() + in_block, n);
    return true;
}

bool CasRangeReader::read(uint64_t at, size_t n, uint8_t* dst, std::string& err) {
    if (!file_) { err = "reader not open"; return false; }
    if (at > UINT64_MAX - n) { err = "range overflow"; return false; }
    const uint64_t end = at + n;
    while (dtotal_ < end && walk_next(err)) {}
    if (!walk_err_.empty()) { err = walk_err_; return false; }
    if (end > dtotal_) { err = "range beyond record"; return false; }
    if (n == 0) return true;
    if (raw_) {
        if (!file_read(at, n, dst)) { err = "CAS raw read failed"; return false; }
        return true;
    }
    // First block whose span reaches past `at`. Zero-size blocks are skipped
    // naturally because their span never does.
    size_t bi = (size_t)(std::upper_bound(blocks_.begin(), blocks_.end(), at,
        [](uint64_t v, const Block& b) { return v < b.dstart; }) - blocks_.begin());
    if (bi) bi--;
    size_t done = 0;
    while (done < n) {
        while (bi < blocks_.size() && blocks_[bi].dstart + blocks_[bi].dsize <= at + done) bi++;
        if (bi >= blocks_.size()) { err = "range beyond record"; return false; }
        const Block& b = blocks_[bi];
        const uint64_t in_block = at + done - b.dstart;
        const size_t take = (size_t)std::min<uint64_t>(n - done, b.dsize - in_block);
        if (!block_bytes(bi, in_block, take, dst + done, err)) return false;
        done += take;
    }
    return true;
}

}  // namespace bf6
